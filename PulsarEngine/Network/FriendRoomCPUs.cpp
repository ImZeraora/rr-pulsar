#include <Network/FriendRoomCPUs.hpp>

#include <MarioKartWii/Item/ItemManager.hpp>
#include <MarioKartWii/Kart/KartManager.hpp>
#include <MarioKartWii/Kart/KartLink.hpp>
#include <MarioKartWii/Race/RaceInfo/RaceInfo.hpp>
#include <MarioKartWii/UI/Page/RaceHUD/RaceHUD.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/RKNet/PacketMgr.hpp>
#include <MarioKartWii/RKNet/RKNetController.hpp>
#include <PulsarSystem.hpp>
#include <core/egg/mem/Heap.hpp>
#include <core/rvl/OS/OS.hpp>
#include <runtimeWrite.hpp>

namespace Pulsar {
namespace Network {

static const u8 FRIEND_ROOM_CPU_MAGIC = 0xC7;
static const u8 FRIEND_ROOM_CPU_FLAG_RESULTS_READY = 0x01;

// This runtime function is used by the host CPU finish path below.  Keep the
// auto-map declaration before that first use so MWCC emits the region-mapped
// symbol declaration before compiling the call site.
kmRuntimeUse(0x805342e8);
kmRuntimeUse(0x80602488);

// One state block keeps all receive buffers and race gates resettable.
struct FriendRoomCPUState {
    FriendRoomCPUItem items[12];
    bool itemValid[12];
    RKNet::RACEDATAPacket raceData[12];
    u32 raceSeq[12];
    bool raceValid[12];
    u16 cpuFinishMask;
    u8 finishOrder[12];
    bool finishValid;

    bool cpu[12];
    u8 cpuOrder[FriendRoomCPUCountMax];
    u8 cpuCount;
    bool rosterReady;
    void* senders[12];
    void* flags[12];

    bool active;
    bool host;
    bool battleRoyale;
    u8 humans;

    bool completing;
    bool loggedCPU;
    bool loggedRemote;
    bool loggedResults;
    bool loggedEnd;
    bool requestedResults;
    bool resultsReady;
    bool loggedReady;
    bool loggedGate;
    u8 lastStage;
    u8 lastHumans;
    u8 lastFinished;
    u32 lastFrame;

    // Diagnostic state for the human-only finish gate. These are deliberately
    // separate from the end-progress fields above: the gate is called from
    // more than one hook during a frame, and we want to identify which hook
    // observed a transition without suppressing the existing progress log.
    bool gateLogValid;
    bool stageLogValid;
    u8 lastGateStage;
    u8 lastGateHumans;
    u8 lastGateFinished;
    u8 lastGateHost;
    u8 lastGateResults;
    u8 lastGateRequested;
    u8 lastGateCompleting;
    u8 lastObservedStage;
    u32 lastGateFrame;
};

static FriendRoomCPUState s;

static bool GetFriendRoomSession(bool* isHost) {
    if (isHost) *isHost = false;
    const RKNet::Controller* controller = RKNet::Controller::sInstance;
    if (!controller) {
        return false;
    }

    const RKNet::RoomType roomType = controller->roomType;
    const bool isFriendRoom = roomType == RKNet::ROOMTYPE_FROOM_HOST ||
                              roomType == RKNet::ROOMTYPE_FROOM_NONHOST;
    if (isFriendRoom) {
        s.active = true;
        s.host = roomType == RKNet::ROOMTYPE_FROOM_HOST;
        if (System::sInstance && System::sInstance->IsContext(PULSAR_MODE_BATTLEROYALE))
            s.battleRoyale = true;
        if (s.humans == 0) {
            const u8 playerCount = controller->subs[controller->currentSub].playerCount;
            if (playerCount != 0 && playerCount <= 12) s.humans = playerCount;
        }
        if (isHost) *isHost = s.host;
        return true;
    }

    // FriendRoomRegional converts the room enum to VS_REGIONAL before the
    // race scene is created. Keep the CPU transport active for that converted
    // session, but do not classify unrelated public regional rooms as friend
    // rooms.
    if (s.active && roomType == RKNet::ROOMTYPE_VS_REGIONAL) {
        if (isHost) *isHost = s.host;
        return true;
    }

    if (roomType != RKNet::ROOMTYPE_VS_REGIONAL) {
        s.active = false;
        s.host = false;
        s.battleRoyale = false;
        s.humans = 0;
    }
    return false;
}

static bool IsFriendRoom() {
    return GetFriendRoomSession(nullptr);
}

static bool IsFriendRoomHost() {
    bool isHost = false;
    return GetFriendRoomSession(&isHost) && isHost;
}

static bool IsFriendRoomBattleRoyale() {
    const System* system = System::sInstance;
    return IsFriendRoom() &&
           (s.battleRoyale || (system != nullptr &&
                               system->IsContext(PULSAR_MODE_BATTLEROYALE)));
}

static bool IsSyntheticFriendRoomSlotInSession(u8 playerId);

bool IsFriendRoomCPU(u8 playerId) {
    if (!GetFriendRoomSession(nullptr) || playerId >= 12) return false;
    return s.cpu[playerId] ||
           (!s.rosterReady && IsSyntheticFriendRoomSlotInSession(playerId));
}

static bool IsSyntheticFriendRoomSlotInSession(u8 playerId) {
    if (playerId >= 12) return false;
    if (s.cpu[playerId]) return true;
    if (s.rosterReady) return false;

    // The bitmap is rebuilt during InitRace.  During a scene/race transition
    // it can briefly be empty even though the scenario already contains the
    // synthetic entries.  Friend-room's sub record still identifies the
    // real-player prefix, so use it as a finish-count fallback.
    const RKNet::Controller* controller = RKNet::Controller::sInstance;
    const Racedata* racedata = Racedata::sInstance;
    if (!controller || !racedata) return false;

    const u32 humanCount = s.humans != 0
        ? s.humans
        : controller->subs[controller->currentSub].playerCount;
    if (humanCount == 0 || humanCount >= 12 || playerId < humanCount) return false;
    return racedata->racesScenario.players[playerId].GetPlayerType() != PLAYER_NONE;
}

static void LogFriendRoomFinishSnapshot(const char* phase, const Raceinfo* raceinfo) {
    if (!phase || !raceinfo || !raceinfo->players) return;

    const u8* order = raceinfo->playerIdInEachPosition;
    OS::Report("[PULSAR] friend-finish %s stage=%u frame=%u order=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
               phase, static_cast<u32>(raceinfo->stage), raceinfo->raceFrames, order ? order[0] : 0xff,
               order ? order[1] : 0xff, order ? order[2] : 0xff, order ? order[3] : 0xff,
               order ? order[4] : 0xff, order ? order[5] : 0xff, order ? order[6] : 0xff,
               order ? order[7] : 0xff, order ? order[8] : 0xff, order ? order[9] : 0xff,
               order ? order[10] : 0xff, order ? order[11] : 0xff);

    for (u8 playerId = 0; playerId < 12; ++playerId) {
        const RaceinfoPlayer* player = raceinfo->players[playerId];
        if (!player) {
            OS::Report("[PULSAR] friend-finish %s p=%u missing\n", phase, playerId);
            continue;
        }

        const Timer* timer = player->raceFinishTime;
        const u32 completionBits = *reinterpret_cast<const u32*>(&player->raceCompletion);
        const u32 completionMaxBits = *reinterpret_cast<const u32*>(&player->raceCompletionMax);
        if (timer) {
            OS::Report("[PULSAR] friend-finish %s p=%u cpu=%u flags=0x%08x pos=%u lap=%u cp=%u completion=0x%08x completionMax=0x%08x timer=%u:%02u.%03u valid=%u\n",
                       phase, playerId, IsFriendRoomCPU(playerId) ? 1 : 0, player->stateFlags,
                       player->position, player->currentLap, player->checkpoint, completionBits,
                       completionMaxBits, timer->minutes, timer->seconds, timer->milliseconds,
                       timer->isActive ? 1 : 0);
        } else {
            OS::Report("[PULSAR] friend-finish %s p=%u cpu=%u flags=0x%08x pos=%u lap=%u cp=%u completion=0x%08x completionMax=0x%08x timer=null\n",
                       phase, playerId, IsFriendRoomCPU(playerId) ? 1 : 0, player->stateFlags,
                       player->position, player->currentLap, player->checkpoint, completionBits,
                       completionMaxBits);
        }
    }
}

void LogFriendRoomRaceFinishEvent(Raceinfo* raceinfo, u8 playerId, const char* phase) {
    if (!phase || !raceinfo || !IsFriendRoom() || playerId >= 12 || !raceinfo->players)
        return;

    const RaceinfoPlayer* player = raceinfo->players[playerId];
    const Racedata* racedata = Racedata::sInstance;
    const RKNet::Controller* controller = RKNet::Controller::sInstance;
    const u32 finishedCounter = *reinterpret_cast<const u8*>(reinterpret_cast<const u8*>(raceinfo) + 0x1c);
    const u32 scenarioType = racedata
        ? static_cast<u32>(racedata->racesScenario.players[playerId].GetPlayerType())
        : 0xff;
    const u32 finishPos = racedata ? racedata->racesScenario.players[playerId].finishPos : 0xff;
    const u32 completionBits = player ? *reinterpret_cast<const u32*>(&player->raceCompletion) : 0;
    const u32 completionMaxBits = player ? *reinterpret_cast<const u32*>(&player->raceCompletionMax) : 0;

    OS::Report("[PULSAR] friend-end event phase=%s p=%u cpu=%u type=%u room=%u host=%u stage=%u frame=%u finishedCounter=%u flags=0x%08x pos=%u finishPos=%u completion=0x%08x completionMax=0x%08x\n",
               phase, playerId, IsFriendRoomCPU(playerId) ? 1 : 0, scenarioType,
               controller ? static_cast<u32>(controller->roomType) : 0xff,
               IsFriendRoomHost() ? 1 : 0, static_cast<u32>(raceinfo->stage), raceinfo->raceFrames,
               finishedCounter, player ? player->stateFlags : 0, player ? player->position : 0xff,
               finishPos, completionBits, completionMaxBits);
}

// RaceModeOnlineVs::calc asks PacketMgr::GetRH2 for every non-local player.
// Friend-room CPU identities are carried through our RH1 extension rather
// than vanilla RH2, so keep this lookup on a valid empty record.  This also
// protects the track-load path before the host-AID mapping has been copied to
// the regular network tables.
static RKNet::RACEHEADER2Packet s_friendRoomCPUEmptyRH2;

static RKNet::RACEHEADER2Packet& GetFriendRoomRH2(RKNet::PacketMgr* packetMgr, u8 playerId) {
    if (IsFriendRoomCPU(playerId)) {
        memset(&s_friendRoomCPUEmptyRH2, 0, sizeof(s_friendRoomCPUEmptyRH2));
        return s_friendRoomCPUEmptyRH2;
    }
    return packetMgr->GetRH2(playerId);
}

kmCall(0x8053e4b8, GetFriendRoomRH2);

static u8 GetFriendRoomCPUCount() {
    if (!IsFriendRoom()) return 0;
    if (!s.rosterReady) {
        s.cpuCount = 0;
        for (u8 playerId = 0; playerId < 12 && s.cpuCount < FriendRoomCPUCountMax; ++playerId) {
            if (IsSyntheticFriendRoomSlotInSession(playerId)) {
                s.cpu[playerId] = true;
                s.cpuOrder[s.cpuCount++] = playerId;
            }
        }
        s.rosterReady = true;
    }
    return s.cpuCount;
}

static u8 GetFriendRoomCPUId(u8 index) {
    return s.cpuOrder[index];
}

static u8 GetHumanPlayerCount(const RacedataScenario& scenario) {
    // Friend Room's sub record is the authoritative number of real players.
    // Do not infer it by scanning all twelve scenario records: non-host
    // synthetic slots are represented as PLAYER_REAL_ONLINE, and stale slot
    // records can otherwise make the scan report all twelve players before
    // their kart/character fields are populated.
    if (IsFriendRoom()) {
        const RKNet::Controller* controller = RKNet::Controller::sInstance;
        if (controller != nullptr) {
            const u32 playerCount = controller->subs[controller->currentSub].playerCount;
            if (playerCount != 0 && playerCount < 12) {
                s.humans = static_cast<u8>(playerCount);
                return s.humans;
            }
        }
        if (s.humans != 0 && s.humans <= 12)
            return s.humans;
    }

    u8 count = 0;
    for (u8 playerId = 0; playerId < 12; ++playerId) {
        const PlayerType type = scenario.players[playerId].GetPlayerType();
        if (type != PLAYER_NONE && type != PLAYER_CPU && playerId + 1 > count) count = playerId + 1;
    }
    return count;
}

enum CPUWeightClass {
    CPU_WEIGHT_LIGHT,
    CPU_WEIGHT_MEDIUM,
    CPU_WEIGHT_HEAVY,
};

static CPUWeightClass GetCPUWeightClass(CharacterId character) {
    switch (character) {
        case BABY_PEACH:
        case BABY_DAISY:
        case BABY_MARIO:
        case DRY_BONES:
        case BABY_LUIGI:
        case TOAD:
        case TOADETTE:
        case KOOPA_TROOPA:
            return CPU_WEIGHT_LIGHT;
        case MARIO:
        case LUIGI:
        case PEACH:
        case DAISY:
        case YOSHI:
        case BIRDO:
        case DIDDY_KONG:
        case BOWSER_JR:
            return CPU_WEIGHT_MEDIUM;
        default:
            return CPU_WEIGHT_HEAVY;
    }
}

static KartId GetCPUKart(u8 cpuIndex, CharacterId character) {
    // Stock selection only exposes vehicles from the character's weight
    // class. Keep the synthetic roster within those same 12-vehicle tables
    // so ResourceManager requests an archive that exists on the disc.
    static const KartId karts[3][12] = {
        { STANDARD_KART_S, BABY_BOOSTER, MINI_BEAST, CHEEP_CHARGER, RALLY_ROMPER, BLUE_FALCON,
          STANDARD_BIKE_S, BULLET_BIKE, BIT_BIKE, QUACKER, MAGIKRUISER, JET_BUBBLE },
        { STANDARD_KART_M, CLASSIC_DRAGSTER, WILD_WING, SUPER_BLOOPER, ROYAL_RACER, SPRINTER,
          STANDARD_BIKE_M, MACH_BIKE, BON_BON, RAPIDE, NITROCYCLE, DOLPHIN_DASHER },
        { STANDARD_KART_L, OFFROADER, FLAME_FLYER, PIRANHA_PROWLER, JETSETTER, HONEYCOUPE,
          STANDARD_BIKE_L, BOWSER_BIKE, WARIO_BIKE, SHOOTING_STAR, SPEAR, PHANTOM },
    };
    return karts[GetCPUWeightClass(character)][cpuIndex % 12];
}

static const wchar_t* GetFriendRoomCPUName(CharacterId character) {
    static const wchar_t* names[24] = {
        L"Mario", nullptr, L"Waluigi", L"Bowser", nullptr, L"Dry Bones",
        nullptr, L"Luigi", L"Toad", L"Donkey", L"Yoshi", L"Wario",
        nullptr, L"Toadette", L"Koopa", L"Daisy", L"Peach", nullptr,
        nullptr, nullptr, nullptr, nullptr, L"Funky", L"Rosalina",
    };
    const u32 id = static_cast<u32>(character);
    return id < 24 && names[id] ? names[id] : L"CPU";
}

static bool SetFriendRoomMiiName(wchar_t* target, u32 targetLength,
                                 const wchar_t* source, u32 sourceLimit) {
    u32 sourceLength = 0;
    while (sourceLength < sourceLimit && source[sourceLength] != L'\0') ++sourceLength;

    bool changed = false;
    for (u32 i = 0; i < targetLength; ++i) {
        const wchar_t value = i < sourceLength ? source[i] : L'\0';
        if (target[i] != value) {
            target[i] = value;
            changed = true;
        }
    }
    return changed;
}

static void SetFriendRoomCPUName(Mii* mii, CharacterId character) {
    if (!mii) return;

    const wchar_t* name = GetFriendRoomCPUName(character);
    const bool infoChanged = SetFriendRoomMiiName(mii->info.name, 11, name, 10);
    const bool rawChanged = SetFriendRoomMiiName(mii->rawStoreMii.miiName, 10, name, 9);
    if (infoChanged || rawChanged)
        mii->rawStoreMii.crc16 = RFL::CalcCRC16(&mii->rawStoreMii, 0x4a);
}

static void SyncFriendRoomCPUScenarioNames(Racedata* racedata) {
    if (!racedata) return;

    for (u8 playerId = 0; playerId < 12; ++playerId) {
        if (!s.cpu[playerId]) continue;

        const CharacterId character = racedata->menusScenario.players[playerId].GetCharacterId();
        SetFriendRoomCPUName(racedata->menusScenario.players[playerId].GetMii(), character);
        SetFriendRoomCPUName(racedata->racesScenario.players[playerId].GetMii(), character);
    }
}

static void SyncFriendRoomCPUSectionNames(bool copyMissing) {
    if (!SectionMgr::sInstance || !SectionMgr::sInstance->sectionParams) return;

    MiiGroup& playerMiis = SectionMgr::sInstance->sectionParams->playerMiis;
    if (!playerMiis.mii || playerMiis.miiCount < 12) return;

    Mii* sourceMii = nullptr;
    u8 sourceId = 0xff;
    for (u8 playerId = 0; playerId < 12; ++playerId) {
        if (s.cpu[playerId]) continue;
        sourceMii = playerMiis.GetMii(playerId);
        if (sourceMii) {
            sourceId = playerId;
            break;
        }
    }

    for (u8 playerId = 0; playerId < 12; ++playerId) {
        if (!s.cpu[playerId]) continue;

        Mii* mii = playerMiis.GetMii(playerId);
        if (!mii && copyMissing && sourceMii) {
            playerMiis.CopyMii(sourceId, playerId);
            mii = playerMiis.GetMii(playerId);
        }
        if (mii) {
            const CharacterId character = Racedata::sInstance
                ? Racedata::sInstance->menusScenario.players[playerId].GetCharacterId()
                : MARIO;
            SetFriendRoomCPUName(mii, character);
        }
    }
}

Mii* GetFriendRoomCPUDisplayMii(u8 playerId) {
    if (playerId >= 12 || !Racedata::sInstance || !IsFriendRoomCPU(playerId)) return nullptr;
    return Racedata::sInstance->racesScenario.players[playerId].GetMii();
}

void ActivateFriendRoomCPUTransport(bool isHost) {
    s.active = true;
    s.host = isHost;
    s.battleRoyale = System::sInstance != nullptr &&
                     System::sInstance->IsContext(PULSAR_MODE_BATTLEROYALE);

    // FriendRoomRegional converts the room enum to VS_REGIONAL after reading
    // this sub record.  Preserve the original real-player count before any
    // later networking/scenario code can replace it with the twelve expanded
    // race slots.
    RKNet::Controller* controller = RKNet::Controller::sInstance;
    if (controller) {
        const u8 playerCount = controller->subs[controller->currentSub].playerCount;
        if (playerCount != 0 && playerCount <= 12) s.humans = playerCount;
    }
}

bool IsFriendRoomCPUTransportActive() {
    return IsFriendRoom();
}

static u8 PackItemFlags(u16 bitfield, u8 activeItemCount) {
    u8 flags = 0;
    if (bitfield & 0x0002) flags |= 0x01;  // has inventory item
    if (bitfield & 0x0004) flags |= 0x02;  // releasing dragged item
    if (bitfield & 0x0010) flags |= 0x04;  // holding throw
    if (bitfield & 0x0020) flags |= 0x08;  // holding place
    if (bitfield & 0x0040) flags |= 0x10;  // can use item
    if (bitfield & 0x0100) flags |= 0x20;  // dragging item
    if (activeItemCount > 3) activeItemCount = 3;
    if (activeItemCount != 0) flags |= static_cast<u8>(activeItemCount << 6);
    return flags;
}

static u8 GetPackedActiveItemCount(const FriendRoomCPUItem& item) {
    if (item.activeItem >= ITEM_NONE) return 0;
    const u8 count = static_cast<u8>((item.flags >> 6) & 0x03);
    return count == 0 ? 1 : count;
}

static void ApplyFriendRoomCPUItemPacket(u8 playerId, const FriendRoomCPUItem& item) {
    if (!RKNet::ITEMHandler::sInstance || playerId >= 12) return;

    // These are the four fields consumed by Item::PlayerObj::UpdateRemote:
    // storedItem/activeItem are ItemIds, while mode/tailMode are the vanilla
    // held/trailing state handshakes.  The custom RH1 carries the state from
    // the host CPU because no real console owns an ITEM packet for that slot.
    RKNet::ITEMPacket& packet = RKNet::ITEMHandler::sInstance->receivedPackets[playerId];
    packet.storedItem = item.storedItem;
    packet.draggedItem = item.activeItem;
    packet.mode = item.storedItem == ITEM_NONE ? 0 : 7;
    packet.tailMode = GetPackedActiveItemCount(item);
}

// Item::KartItem::init normally obtains this pointer through
// KartObjectProxy::apply.  Repair the entry immediately before its calc call
// so no later proxy-list operation can replace it before
// KartObjectProxy::getInput dereferences it.
static bool IsFriendRoomWiiPointer(const void* pointer) {
    const u32 address = reinterpret_cast<u32>(pointer);
    return address >= 0x80000000u && address < 0x94000000u;
}

static Kart::Player* GetFriendRoomKartPlayer(u8 playerId) {
    if (playerId >= 12 || !Kart::Manager::sInstance) return nullptr;

    Kart::Player* kart = Kart::Manager::sInstance->GetKartPlayer(playerId);
    // GetKartPlayer is an unchecked array lookup.  A synthetic slot can be
    // visible to Item::Manager before Kart::Manager has finished creating its
    // twelve entries; never install an out-of-range result as a Link pointer.
    if (IsFriendRoomWiiPointer(kart)) return kart;
    return nullptr;
}

static void RepairFriendRoomItemLink(Item::Player* item) {
    if (!item || !IsFriendRoomCPU(item->id)) return;

    Kart::Player* kart = GetFriendRoomKartPlayer(item->id);
    if (!kart) return;

    item->pointers = &kart->pointers;
    item->kartPlayer = kart;
    item->playerObj.itemPlayer = item;
    item->playerObj.playerId = item->id;
    item->playerObj.pointers = &kart->pointers;
}

static bool RepairFriendRoomPlayerObjLink(Item::PlayerObj* playerObj) {
    if (!playerObj) return false;

    // Player::playerObj is at +0xb4 in the stock 0x248-byte Item::Player.
    // Recovering the owner from the embedded object remains valid even when
    // the non-host Item::Manager exposes fewer active players than the full
    // twelve-slot friend-room roster.
    Item::Player* item = reinterpret_cast<Item::Player*>(reinterpret_cast<u8*>(playerObj) - 0xb4);
    if (item->id >= 12 || !IsFriendRoomCPU(item->id)) return true;

    Kart::Player* kart = GetFriendRoomKartPlayer(item->id);
    if (!kart) return false;

    // PlayerObj::Init stores the owning Item::Player at +0xc.  The stock
    // proxy list can rebuild this link, so restore the canonical owner
    // immediately before any PlayerObj operation uses it.
    playerObj->itemPlayer = item;
    playerObj->playerId = item->id;
    playerObj->pointers = &kart->pointers;
    return true;
}

kmRuntimeUse(0x80791910);
kmRuntimeUse(0x80795668);

static void UseFriendRoomItem(Item::PlayerObj* playerObj, bool isRemote) {
    if (!RepairFriendRoomPlayerObjLink(playerObj)) return;
    reinterpret_cast<void (*)(Item::PlayerObj*, bool)>(kmRuntimeAddr(0x80791910))(playerObj, isRemote);
}

// These are the two stock callers of Item::PlayerObj::UseItem: the normal
// local/CPU path and the remote trailing-item path.
kmCall(0x80797f94, UseFriendRoomItem);
kmCall(0x80795764, UseFriendRoomItem);

static void UpdateFriendRoomRemoteItem(Item::PlayerObj* playerObj) {
    if (!RepairFriendRoomPlayerObjLink(playerObj)) return;

    bool isHost = false;
    const bool friendRoom = GetFriendRoomSession(&isHost);
    if (playerObj && friendRoom && !isHost && playerObj->playerId < 12 &&
        IsSyntheticFriendRoomSlotInSession(playerObj->playerId)) {
        FriendRoomCPUItem item = {};
        item.storedItem = ITEM_NONE;
        item.activeItem = ITEM_NONE;
        const FriendRoomCPUItem* receivedItem = 0;
        if (s.itemValid[playerObj->playerId])
            receivedItem = &s.items[playerObj->playerId];
        if (receivedItem) item = *receivedItem;

        // ItemHandler::ImportNewPackets can run after the RH1 reader and has
        // no real network ITEM record for a synthetic slot.  Re-apply the
        // CPU's four vanilla remote-item fields at the exact call boundary
        // where UpdateRemote consumes them.  The empty default also clears a
        // stale packet during the first frames of a new race.
        ApplyFriendRoomCPUItemPacket(playerObj->playerId, item);
    }

    reinterpret_cast<void (*)(Item::PlayerObj*)>(kmRuntimeAddr(0x80795668))(playerObj);
}

// Item::Player::Update invokes Item::PlayerObj::UpdateRemote here.  Keeping
// the injection at this boundary prevents the stock ITEM import from erasing
// the synthetic CPU state before the trailing objects are spawned/despawned.
kmCall(0x80797f00, UpdateFriendRoomRemoteItem);

static const Mtx34& GetFriendRoomPlayerObjMtx(Item::PlayerObj* playerObj) {
    RepairFriendRoomPlayerObjLink(playerObj);
    return playerObj->GetMtx();
}

// PlayerObj::ComputeWheelPosRelativeToKart calls Kart::Link::GetMtx here.
// Repair at the actual dereference site so the nested proxy cannot remain
// poisoned even if another proxy-list operation ran earlier in the frame.
kmCall(0x807955d8, GetFriendRoomPlayerObjMtx);

static void UpdateFriendRoomItem(Item::Player* item) {
    RepairFriendRoomItemLink(item);
    if (item) item->Update();
}

// Item::Manager::Update calls Item::Player::Update at 0x8079994c.
kmCall(0x8079994c, UpdateFriendRoomItem);

static bool UpdateFriendRoomPlayerObjParams(Item::PlayerObj* playerObj) {
    // UpdateTetheredItems calls UpdateParams directly.  Repairing only at the
    // outer Item::Player update is not sufficient because a stale proxy can
    // survive until this exact callsite.  Returning false skips the stock
    // tether update when the kart entry is not ready, avoiding a dereference
    // through an unchecked GetKartPlayer result.
    if (!RepairFriendRoomPlayerObjLink(playerObj)) return false;
    return playerObj->UpdateParams();
}

// Item::PlayerObj::UpdateTetheredItems -> Item::PlayerObj::UpdateParams.
kmCall(0x80792330, UpdateFriendRoomPlayerObjParams);

static void DecideFriendRoomItem(Item::Player* item, u16 playerItemBoxType, u16 cpuItemBoxType, u32 lotteryType) {
    RepairFriendRoomItemLink(item);

    // Item-box decisions are host authoritative for friend-room CPUs. The
    // non-host still runs the normal item update so host-sent input and
    // inventory state can animate/use the item, but it must not independently
    // roll a different item from its local ItemSlotTable.
    bool isHost = false;
    const bool friendRoom = GetFriendRoomSession(&isHost);
    if (item && friendRoom && !isHost && IsSyntheticFriendRoomSlotInSession(item->id)) return;
    if (item) item->DecideItem(playerItemBoxType, cpuItemBoxType, lotteryType);
}

// GeoObj::ObjItembox::OnCollision has two direct Item::Player::DecideItem
// calls. Guard both paths; the second is the CPU/default-settings path.
kmCall(0x80828d70, DecideFriendRoomItem);
kmCall(0x80828da4, DecideFriendRoomItem);

static void ResetFriendRoomCPURaceState() {
    // FriendRoomRegional can convert the enum before the next race.
    const bool active = s.active;
    const bool host = s.host;
    const bool battleRoyale = s.battleRoyale;
    const u8 humans = s.humans;
    memset(&s, 0, sizeof(s));
    s.active = active;
    s.host = host;
    s.battleRoyale = battleRoyale;
    s.humans = humans;
    memset(s.finishOrder, 0xFF, sizeof(s.finishOrder));
    s.lastStage = 0xFF;
    s.lastHumans = 0xFF;
    s.lastFinished = 0xFF;
}

void PrepareFriendRoomCPUs(Racedata* racedata) {
    if (!racedata || !IsFriendRoom()) return;

    // These factories are owned by the current race's kart heap.  They must
    // be created again during the next stock kart-construction pass; keeping
    // pointers from the previous race would leave dangling proxy objects.
    ResetFriendRoomCPURaceState();

    // Add the synthetic entries to the menu scenario before vanilla InitRace.
    // Scenario::initRace computes the complete count, initializes the player
    // controllers, and copies this finished roster into racesScenario.  The
    // resource loader then sees exactly the same twelve-player scenario that
    // Kart::Manager uses, so every CPU archive is requested through the stock
    // loading path.
    RacedataScenario& scenario = racedata->menusScenario;
    const u8 humanCount = GetHumanPlayerCount(scenario);
    if (humanCount == 0 || humanCount >= 12) return;

    Mii* sourceMii = scenario.players[0].GetMii();
    static const CharacterId characters[] = {
        MARIO, LUIGI, PEACH, DAISY, YOSHI, TOAD, TOADETTE, KOOPA_TROOPA,
        DRY_BONES, WARIO, WALUIGI, BOWSER, DONKEY_KONG, ROSALINA, FUNKY_KONG,
    };
    const bool isHost = IsFriendRoomHost();

    for (u8 playerId = humanCount; playerId < 12; ++playerId) {
        RacedataPlayer& player = scenario.players[playerId];
        const u8 cpuIndex = playerId - humanCount;
        const CharacterId character = characters[cpuIndex % (sizeof(characters) / sizeof(characters[0]))];
        s.cpu[playerId] = true;
        // The host is the only machine that owns the CPU AI.  Every other
        // machine must construct this slot as a vanilla remote player so it
        // gets KartNetReceiver, MovementRemote, and ActionRemote components.
        player.SetPlayerType(isHost ? PLAYER_CPU : PLAYER_REAL_ONLINE);
        player.SetCharacterId(character);
        player.SetKartId(GetCPUKart(cpuIndex, character));
        player.hudSlotId = -1;
        player.realControllerChannel = -1;
        player.team = TEAM_NONE;
        if (sourceMii) player.SetMii(sourceMii);
        SetFriendRoomCPUName(player.GetMii(), character);
    }

    SyncFriendRoomCPUScenarioNames(racedata);
    SyncFriendRoomCPUSectionNames(false);

}

static void RefreshFriendRoomCPUAidMappings() {
    bool isHost = false;
    if (!GetFriendRoomSession(&isHost)) return;

    RKNet::Controller* controller = RKNet::Controller::sInstance;
    if (!controller) return;
    const RKNet::ControllerSub& sub = controller->subs[controller->currentSub];
    const u8 hostAid = isHost ? sub.localAid : sub.hostAid;

    const u8 cpuCount = GetFriendRoomCPUCount();
    for (u8 i = 0; i < cpuCount; ++i)
        controller->aidsBelongingToPlayerIds[GetFriendRoomCPUId(i)] = hostAid;
}

void FinalizeFriendRoomCPUs() {
    RefreshFriendRoomCPUAidMappings();
    if (!IsFriendRoom()) return;

    SyncFriendRoomCPUScenarioNames(Racedata::sInstance);
    SyncFriendRoomCPUSectionNames(false);
}

static bool IsFriendRoomRacePlayerDone(const RaceinfoPlayer* player) {
    // Raceinfo::EndPlayerRace counts only the stock finished and disconnected
    // flags.  Do not treat the intermediate finishing flag as complete here.
    return player != nullptr && (player->stateFlags & (0x02 | 0x10)) != 0;
}

static bool IsFriendRoomHumanNoLongerRacing(const RaceinfoPlayer* player) {
    // Royale's LapKO path calls RaceinfoPlayer::Vanish for an eliminated
    // player.  Vanish sets 0x20 and does not call EndPlayerRace, so counting
    // only 0x02/0x10 makes the host believe that eliminated remote humans are
    // still racing.  Keep the stock predicate above for CPU finalization,
    // but include the vanished state in the human-only one-racer gate.
    return player != nullptr && (player->stateFlags & (0x02 | 0x10 | 0x20)) != 0;
}

static bool IsFriendRoomRaceEndReady(u32 humanCount, u32 finishedHumans) {
    // Friend-room CPUs are synthetic slots. Apply the one-racer rule to the
    // real-player subset only: the race is ready whenever zero or one humans
    // remain unfinished. This also covers a room with one human from the
    // beginning, as requested by the Friend Room rule.
    return humanCount != 0 && finishedHumans <= humanCount &&
           humanCount - finishedHumans <= 1;
}

static bool IsFriendRoomResultsReady() {
    return IsFriendRoomHost() || s.resultsReady;
}

static bool GetFriendRoomHumanFinishCounts(const Raceinfo* raceinfo, u32* humanCount,
                                           u32* finishedHumans, u8* unfinishedHumanId);

void CompleteFriendRoomCPURace(Raceinfo* raceinfo) {
    // CPU finish times are host-authoritative.  Non-hosts use the CPU progress
    // already reconstructed from the host's RACEDATA packets.  Once the stock
    // online-VS rule leaves at most one human unfinished, close that last human
    // and the synthetic slots through the stock finish path so RaceManager can
    // leave RACE with valid timers and placement metadata.
    if (s.completing || !raceinfo || !IsFriendRoom() ||
        raceinfo->stage >= RACESTAGE_FINISHED)
        return;

    // Royale uses the same immediate one-human threshold, but only the host
    // may finalize the synthetic records.  Remote clients wait for the host's
    // results-ready bit below.
    if (IsFriendRoomBattleRoyale() && !IsFriendRoomHost()) return;

    if (!IsFriendRoomResultsReady()) return;

    if (!raceinfo->players) return;

    u32 humanCount = 0;
    u32 finishedHumans = 0;
    u8 unfinishedHumanId = 0xff;
    if (!GetFriendRoomHumanFinishCounts(raceinfo, &humanCount, &finishedHumans,
                                        &unfinishedHumanId) ||
        !IsFriendRoomRaceEndReady(humanCount, finishedHumans))
        return;

    s.completing = true;

    if (!s.loggedCPU)
        LogFriendRoomFinishSnapshot("before-cpu-finalize", raceinfo);

    // RaceMode::endRace uses RaceManagerPlayer::endLocalRace(player, 2, 1)
    // for unfinished local racers.  That routine derives an individual finish
    // time from raceCompletion, so a CPU that is ahead of the last human stays
    // ahead in the results and a CPU behind them stays behind.  Giving every
    // CPU one cloned timer loses that information and makes the stock
    // position calculator fall back to player-ID tie breaking.
    typedef void (*EndLocalRaceFn)(RaceinfoPlayer*, u32, u32);

    // This is the normal online-VS "last racer" behavior.  It is important to
    // do this before the CPU slots: endRace snapshots the players-ahead mask
    // and the finished-player counter while it writes the finish timer.
    if (unfinishedHumanId != 0xff) {
        RaceinfoPlayer* player = raceinfo->players[unfinishedHumanId];
        if (player) {
            OS::Report("[PULSAR] friend-finish finalizing-last-human p=%u frame=%u completion=0x%08x\n",
                       unfinishedHumanId, raceinfo->raceFrames,
                       *reinterpret_cast<const u32*>(&player->raceCompletion));
            reinterpret_cast<EndLocalRaceFn>(kmRuntimeAddr(0x805342e8))(player, 2, 1);
        }
    }

    const u8 cpuCount = GetFriendRoomCPUCount();
    for (u8 cpuIndex = 0; cpuIndex < cpuCount; ++cpuIndex) {
        const u8 playerId = GetFriendRoomCPUId(cpuIndex);
        RaceinfoPlayer* player = raceinfo->players[playerId];
        if (!player || IsFriendRoomRacePlayerDone(player)) continue;
        reinterpret_cast<EndLocalRaceFn>(kmRuntimeAddr(0x805342e8))(player, 2, 1);
    }

    // The position array was calculated before the CPUs received their final
    // timers.  Re-run the stock online-VS position handler now so the
    // authoritative order agrees with the timestamps just written above.
    if (IsFriendRoomHost() && raceinfo->gamemodeData)
        raceinfo->gamemodeData->HandlePositionTracking();

    s.completing = false;

    // The custom rule is about humans, not synthetic CPU records. The host
    // has attempted to close the optional last human above and has attempted
    // every CPU slot, so commit the intermediate stock state now. Do not wait
    // for a CPU callback or for the stock all-record predicate: either one can
    // be absent on a client that is already following the host's transition.
    if (IsFriendRoomHost() && raceinfo->stage < RACESTAGE_IS_FINISHING) {
        u32 finalHumanCount = 0;
        u32 finalFinishedHumans = 0;
        GetFriendRoomHumanFinishCounts(raceinfo, &finalHumanCount,
                                       &finalFinishedHumans, nullptr);
        OS::Report("[PULSAR] friend-finish host-state-commit stage=%u frame=%u humans=%u/%u\n",
                   static_cast<u32>(raceinfo->stage), raceinfo->raceFrames,
                   finalFinishedHumans, finalHumanCount);
        raceinfo->stage = RACESTAGE_IS_FINISHING;
    }

    if (!s.loggedCPU) {
        LogFriendRoomFinishSnapshot("after-cpu-finalize", raceinfo);
        s.loggedCPU = true;
    }
}

static bool GetFriendRoomHumanFinishCounts(const Raceinfo* raceinfo, u32* humanCount,
                                           u32* finishedHumans, u8* unfinishedHumanId) {
    if (!humanCount || !finishedHumans || !raceinfo || !IsFriendRoom() || !raceinfo->players)
        return false;

    *humanCount = 0;
    *finishedHumans = 0;
    if (unfinishedHumanId) *unfinishedHumanId = 0xff;
    for (u8 playerId = 0; playerId < 12; ++playerId) {
        if (IsSyntheticFriendRoomSlotInSession(playerId)) continue;

        ++(*humanCount);
        if (IsFriendRoomHumanNoLongerRacing(raceinfo->players[playerId])) {
            ++(*finishedHumans);
        } else if (unfinishedHumanId) {
            *unfinishedHumanId = playerId;
        }
    }
    return *humanCount != 0;
}

// This is intentionally diagnostic only. It records the inputs to the custom
// one-human gate at the point each caller sees them, including the source
// hook. A stage change is logged independently so a later stock update that
// overwrites our intermediate state is visible in the same log.
static bool LogFriendRoomGateState(const char* source, const Raceinfo* raceinfo,
                                   bool isHost, u32 humanCount, u32 finishedHumans) {
    if (!source || !raceinfo || !raceinfo->players) return false;

    const u8 stage = static_cast<u8>(raceinfo->stage);
    const u8 host = isHost ? 1 : 0;
    const u8 results = s.resultsReady ? 1 : 0;
    const u8 requested = s.requestedResults ? 1 : 0;
    const u8 completing = s.completing ? 1 : 0;

    if (!s.stageLogValid || s.lastObservedStage != stage) {
        OS::Report("[PULSAR] friend-stage source=%s old=%u new=%u frame=%u room=%u host=%u resultsReady=%u requested=%u\n",
                   source, s.stageLogValid ? static_cast<u32>(s.lastObservedStage) : 0xff,
                   static_cast<u32>(stage), raceinfo->raceFrames,
                   RKNet::Controller::sInstance
                       ? static_cast<u32>(RKNet::Controller::sInstance->roomType)
                       : 0xff,
                   host, results, requested);
        s.lastObservedStage = stage;
        s.stageLogValid = true;
    }

    const bool changed = !s.gateLogValid ||
                         s.lastGateStage != stage ||
                         s.lastGateHumans != static_cast<u8>(humanCount) ||
                         s.lastGateFinished != static_cast<u8>(finishedHumans) ||
                         s.lastGateHost != host ||
                         s.lastGateResults != results ||
                         s.lastGateRequested != requested ||
                         s.lastGateCompleting != completing;
    const bool ready = IsFriendRoomRaceEndReady(humanCount, finishedHumans);
    const bool heartbeat = !changed && ready &&
                           raceinfo->raceFrames - s.lastGateFrame >= 60;
    if (!changed && !heartbeat) return false;

    const RKNet::Controller* controller = RKNet::Controller::sInstance;
    const RKNet::ControllerSub* sub = controller
        ? &controller->subs[controller->currentSub]
        : (const RKNet::ControllerSub*)0;
    u16 cpuMask = 0;
    for (u8 playerId = 0; playerId < 12; ++playerId) {
        const bool synthetic = s.cpu[playerId] ||
                               (!s.rosterReady &&
                                IsSyntheticFriendRoomSlotInSession(playerId));
        if (synthetic) cpuMask |= static_cast<u16>(1u << playerId);
    }

    const u32 unfinishedHumans = humanCount >= finishedHumans
        ? humanCount - finishedHumans
        : 0;
    OS::Report("[PULSAR] friend-gate source=%s room=%u host=%u royale=%u stage=%u frame=%u rawPlayers=%u cachedHumans=%u humans=%u/%u unfinished=%u ready=%u rosterReady=%u cpuCount=%u cpuMask=0x%04x cpuFinishMask=0x%04x resultsReady=%u requested=%u completing=%u localAid=%u hostAid=%u\n",
               source, controller ? static_cast<u32>(controller->roomType) : 0xff,
               host, IsFriendRoomBattleRoyale() ? 1 : 0, static_cast<u32>(stage),
               raceinfo->raceFrames, sub ? static_cast<u32>(sub->playerCount) : 0xff,
               static_cast<u32>(s.humans), humanCount, finishedHumans, unfinishedHumans,
               ready ? 1 : 0, s.rosterReady ? 1 : 0, static_cast<u32>(s.cpuCount),
               static_cast<u32>(cpuMask), static_cast<u32>(s.cpuFinishMask), results,
               requested, completing, sub ? static_cast<u32>(sub->localAid) : 0xff,
               sub ? static_cast<u32>(sub->hostAid) : 0xff);

    // Slot details are emitted only on a state change, not on the heartbeat.
    // This shows whether the host/remote player is actually being counted as
    // human and which stock completion flag caused it to be counted finished.
    if (changed) {
        const Racedata* racedata = Racedata::sInstance;
        for (u8 playerId = 0; playerId < 12; ++playerId) {
            const RaceinfoPlayer* player = raceinfo->players[playerId];
            const bool synthetic = (cpuMask & static_cast<u16>(1u << playerId)) != 0;
            const u32 scenarioType = racedata
                ? static_cast<u32>(racedata->racesScenario.players[playerId].GetPlayerType())
                : 0xff;
            const u32 completionBits = player
                ? *reinterpret_cast<const u32*>(&player->raceCompletion)
                : 0;
            const u32 completionMaxBits = player
                ? *reinterpret_cast<const u32*>(&player->raceCompletionMax)
                : 0;
            const Timer* timer = player ? player->raceFinishTime : (const Timer*)0;
            OS::Report("[PULSAR] friend-gate-slot source=%s p=%u synthetic=%u type=%u flags=0x%08x humanDone=%u pos=%u lap=%u cp=%u completion=0x%08x completionMax=0x%08x timer=%u\n",
                       source, playerId, synthetic ? 1 : 0, scenarioType,
                       player ? player->stateFlags : 0,
                       synthetic ? 0 : (IsFriendRoomHumanNoLongerRacing(player) ? 1 : 0),
                       player ? player->position : 0xff,
                       player ? player->currentLap : 0xff,
                       player ? player->checkpoint : 0xff,
                       completionBits, completionMaxBits,
                       timer && timer->isActive ? 1 : 0);
        }
    }

    s.gateLogValid = true;
    s.lastGateStage = stage;
    s.lastGateHumans = static_cast<u8>(humanCount);
    s.lastGateFinished = static_cast<u8>(finishedHumans);
    s.lastGateHost = host;
    s.lastGateResults = results;
    s.lastGateRequested = requested;
    s.lastGateCompleting = completing;
    s.lastGateFrame = raceinfo->raceFrames;
    return changed || heartbeat;
}

static void LogFriendRoomEndProgress(Raceinfo* raceinfo, u32 humanCount, u32 finishedHumans) {
    if (!raceinfo || raceinfo->stage < RACESTAGE_RACE || !raceinfo->players) return;

    const bool changed = static_cast<u8>(raceinfo->stage) != s.lastStage ||
                         humanCount != s.lastHumans ||
                         finishedHumans != s.lastFinished;
    const bool waitingForResults = IsFriendRoomRaceEndReady(humanCount, finishedHumans) &&
                                   raceinfo->raceFrames - s.lastFrame >= 60;
    if (!changed && !waitingForResults) return;

    const u8 syntheticCount = GetFriendRoomCPUCount();
    u8 finishedSynthetic = 0;
    for (u8 cpuIndex = 0; cpuIndex < syntheticCount; ++cpuIndex) {
        if (IsFriendRoomRacePlayerDone(raceinfo->players[GetFriendRoomCPUId(cpuIndex)]))
            ++finishedSynthetic;
    }

    const RKNet::Controller* controller = RKNet::Controller::sInstance;
    const u32 finishedCounter = *reinterpret_cast<const u8*>(reinterpret_cast<const u8*>(raceinfo) + 0x1c);
    OS::Report("[PULSAR] friend-end progress stage=%u frame=%u room=%u host=%u humans=%u/%u synthetic=%u/%u finishedCounter=%u completing=%u\n",
               static_cast<u32>(raceinfo->stage), raceinfo->raceFrames,
               controller ? static_cast<u32>(controller->roomType) : 0xff,
               IsFriendRoomHost() ? 1 : 0, finishedHumans, humanCount,
               finishedSynthetic, syntheticCount, finishedCounter,
               s.completing ? 1 : 0);
    LogFriendRoomFinishSnapshot("end-progress", raceinfo);

    s.lastStage = static_cast<u8>(raceinfo->stage);
    s.lastHumans = static_cast<u8>(humanCount);
    s.lastFinished = static_cast<u8>(finishedHumans);
    s.lastFrame = raceinfo->raceFrames;
}

static void ForceFriendRoomEndTransition(Raceinfo* raceinfo) {
    if (!raceinfo || raceinfo->stage >= RACESTAGE_FINISHED) return;

    if (raceinfo->stage >= RACESTAGE_IS_FINISHING) return;

    // EndPlayerRace writes the intermediate state (3), then RaceManager::calc
    // advances it to the final state (4) on the following frame. Do not ask
    // the stock predicate to count synthetic CPUs here; the custom rule is
    // explicitly about real humans, and CPU result records were already
    // attempted by CompleteFriendRoomCPURace.
    if (!s.loggedEnd) {
        OS::Report("[PULSAR] friend-finish forcing-results stage=%u frame=%u\n",
                   static_cast<u32>(raceinfo->stage), raceinfo->raceFrames);
        s.loggedEnd = true;
    }
    raceinfo->stage = RACESTAGE_IS_FINISHING;
}

static void ForceFriendRoomRoyaleRemoteEndTransition(Raceinfo* raceinfo) {
    bool isHost = false;
    if (!raceinfo || !IsFriendRoomBattleRoyale() ||
        !GetFriendRoomSession(&isHost) || isHost || !s.resultsReady ||
        raceinfo->stage < RACESTAGE_RACE || raceinfo->stage >= RACESTAGE_FINISHED)
        return;

    if (!s.loggedEnd) {
        OS::Report("[PULSAR] friend-finish royale-remote-results stage=%u frame=%u cpuMask=0x%04x\n",
                   static_cast<u32>(raceinfo->stage), raceinfo->raceFrames,
                   s.cpuFinishMask);
        s.loggedEnd = true;
    }
    raceinfo->stage = RACESTAGE_IS_FINISHING;
}

static void RequestFriendRoomResults(Raceinfo* raceinfo) {
    if (s.requestedResults || !raceinfo || !IsFriendRoom() ||
        IsFriendRoomBattleRoyale())
        return;

    // The host is authoritative for the shared transition.  A non-host must
    // not leave its HUD merely because its local copy happened to mark the
    // last human finished first; wait for the host's RH1 results-ready bit.
    if (IsFriendRoomHost()) {
        if (raceinfo->stage < RACESTAGE_FINISHED) return;
    } else if (!s.resultsReady) {
        return;
    }

    Pages::RaceHUD* raceHud = Pages::RaceHUD::sInstance;
    if (!raceHud || raceHud->currentState != STATE_ACTIVE) return;

    // Keep the stock per-race results destination.  PAGE_WIFI_VS_RESULTS is
    // the friend-room final/congratulations page reached from the total
    // leaderboard, not the basic results page shown after this race.
    s.requestedResults = true;
    raceHud->nextPageId = PAGE_GPVS_LEADERBOARD_UPDATE;
    raceHud->EndState();
    OS::Report("[PULSAR] friend-finish request-results stage=%u frame=%u page=%u\n",
               static_cast<u32>(raceinfo->stage), raceinfo->raceFrames,
               static_cast<u32>(raceHud->nextPageId));
}

// RaceHUD's stock friend-room path ends the HUD after the local finish
// message has been visible for 120 frames.  That is only a presentation
// timeout; it does not mean the shared race is ready to leave RACE.  With
// synthetic CPUs that timeout would send the room to PAGE_WWRACEEND_WAIT and
// voting while remote humans were still driving.  Hold that call until
// RequestFriendRoomResults has armed the synchronized results transition.
typedef void (*EndStateAnimatedFn)(Pages::RaceHUD*, u32, float);

static void EndFriendRoomHUDAfterSharedFinish(Pages::RaceHUD* raceHud, u32 animDirection,
                                              float animLength) {
    if (raceHud && IsFriendRoom()) {
        const bool isRoyale = IsFriendRoomBattleRoyale();
        const Raceinfo* raceinfo = Raceinfo::sInstance;
        const bool royaleStillRacing = isRoyale &&
            (raceinfo == nullptr || raceinfo->stage < RACESTAGE_IS_FINISHING);
        if ((!isRoyale && !s.requestedResults) || royaleStillRacing)
            return;
    }

    reinterpret_cast<EndStateAnimatedFn>(kmRuntimeAddr(0x80602488))(raceHud, animDirection, animLength);
}

// This is the automatic finish-message timeout in RaceHUD::AfterControlUpdate.
kmCall(0x80857a0c, EndFriendRoomHUDAfterSharedFinish);

static bool CanEndFriendRoomRace(GMData* raceMode) {
    if (!raceMode) return false;
    // Raceinfo::EndPlayerRace counts the expanded twelve-slot scenario. That
    // stock predicate is intentionally disabled for Friend Rooms; the host's
    // human-only gate below owns the one-racer transition and non-hosts follow
    // the host's results-ready packet.
    return IsFriendRoom() ? false : raceMode->CanRaceEnd();
}

static bool TryEndFriendRoomRace(GMData* raceMode, u32 finishedCount, u32 playerCount) {
    if (!raceMode) return false;
    return IsFriendRoom() ? false : raceMode->vf_0x24(finishedCount, playerCount);
}

static void AutoEndFriendRoomRace(Raceinfo* raceinfo, u32 humanCount, u32 finishedHumans) {
    if (!IsFriendRoom()) return;
    if (!raceinfo || raceinfo->stage < RACESTAGE_RACE ||
        raceinfo->stage >= RACESTAGE_FINISHED)
        return;

    // The host owns the human-only decision. Once it has published the
    // results-ready bit, non-hosts must not wait for their local RH2 finish
    // flags to catch up before leaving the race.
    if (!IsFriendRoomHost()) {
        if (!s.resultsReady) return;
        if (IsFriendRoomBattleRoyale())
            ForceFriendRoomRoyaleRemoteEndTransition(raceinfo);
        else
            ForceFriendRoomEndTransition(raceinfo);
        return;
    }

    if (!IsFriendRoomRaceEndReady(humanCount, finishedHumans)) return;

    if (IsFriendRoomBattleRoyale()) {
        // A remote finish can leave one HUMAN unfinished without producing
        // another local EndPlayerRace callback.  Finalize that record and the
        // synthetic slots through the stock path, then broadcast the shared
        // results-ready state.
        if (!IsFriendRoomHost()) return;

        u32 currentHumanCount = 0;
        u32 currentFinishedHumans = 0;
        if (!GetFriendRoomHumanFinishCounts(raceinfo, &currentHumanCount,
                                            &currentFinishedHumans, nullptr))
            return;

        CompleteFriendRoomCPURace(raceinfo);

        if (!s.loggedEnd) {
            OS::Report("[PULSAR] friend-finish royale-host-results stage=%u frame=%u humans=%u/%u\n",
                       static_cast<u32>(raceinfo->stage), raceinfo->raceFrames,
                       currentFinishedHumans, currentHumanCount);
            s.loggedEnd = true;
        }
        s.requestedResults = true;
        raceinfo->stage = RACESTAGE_IS_FINISHING;
        return;
    }

    // The normal finish callback handles this case.  Keep the update fallback
    // so a final human finish received through the network still finalizes
    // the synthetic slots before the stock results transition.
    CompleteFriendRoomCPURace(raceinfo);
    ForceFriendRoomEndTransition(raceinfo);
}

static void UpdateFriendRoomFinishGateFrom(const char* source);

// RaceScene skips the AI-manager call while Raceinfo is in its spectator
// update path.  Keep the host's one-human gate reachable from the race update
// hook as well; normal frames return immediately because the human count is
// not ready or the stage has already advanced.
static void CheckFriendRoomHostRaceEnd() {
    UpdateFriendRoomFinishGateFrom("race-frame");
}

static RaceFrameHook friendRoomHostRaceEndHook(CheckFriendRoomHostRaceEnd);

static void UpdateFriendRoomFinishGateFrom(const char* source) {
    bool isHost = false;
    if (!GetFriendRoomSession(&isHost)) return;

    Raceinfo* raceinfo = Raceinfo::sInstance;
    u32 humanCount = 0;
    u32 finishedHumans = 0;
    if (!GetFriendRoomHumanFinishCounts(raceinfo, &humanCount, &finishedHumans, nullptr))
        return;

    const bool loggedState = LogFriendRoomGateState(source, raceinfo, isHost,
                                                    humanCount, finishedHumans);
    if (loggedState) {
        const char* action = "skip-stage";
        if (raceinfo->stage >= RACESTAGE_RACE && raceinfo->stage < RACESTAGE_FINISHED) {
            if (!isHost)
                action = s.resultsReady ? "follow-host-results" : "wait-results-packet";
            else if (!IsFriendRoomRaceEndReady(humanCount, finishedHumans))
                action = "wait-human-threshold";
            else if (IsFriendRoomBattleRoyale())
                action = "commit-royale-results";
            else
                action = "commit-human-threshold";
        }
        OS::Report("[PULSAR] friend-gate-decision source=%s action=%s stage=%u frame=%u humans=%u/%u unfinished=%u\n",
                   source ? source : "unknown", action, static_cast<u32>(raceinfo->stage),
                   raceinfo->raceFrames, finishedHumans, humanCount,
                   humanCount >= finishedHumans ? humanCount - finishedHumans : 0);
    }

    LogFriendRoomEndProgress(raceinfo, humanCount, finishedHumans);

    if (!s.loggedGate && IsFriendRoomRaceEndReady(humanCount, finishedHumans)) {
        OS::Report("[PULSAR] friend-finish boundary host=%u royale=%u stage=%u frame=%u humans=%u/%u\n",
                   isHost ? 1 : 0, IsFriendRoomBattleRoyale() ? 1 : 0,
                   static_cast<u32>(raceinfo->stage), raceinfo->raceFrames,
                   finishedHumans, humanCount);
        s.loggedGate = true;
    }

    // This is deliberately callable after RaceScene::UpdateRaceInstances.
    // That update is where stock RaceModeOnlineVs consumes remote finish
    // timers and sets their FINISHED flags; the AI-manager hook can run before
    // those flags are visible on the host for the current frame.
    AutoEndFriendRoomRace(raceinfo, humanCount, finishedHumans);
    RequestFriendRoomResults(raceinfo);

    // AutoEndFriendRoomRace can change stage and can finalize the last human,
    // so take a second bounded observation after both transition attempts.
    u32 finalHumanCount = 0;
    u32 finalFinishedHumans = 0;
    if (GetFriendRoomHumanFinishCounts(raceinfo, &finalHumanCount, &finalFinishedHumans, nullptr))
        LogFriendRoomGateState(source, raceinfo, isHost, finalHumanCount, finalFinishedHumans);
}

void UpdateFriendRoomFinishGate() {
    UpdateFriendRoomFinishGateFrom("race-instances");
}

// Raceinfo::EndPlayerRace calls these two race-mode gates before transitioning
// to the finished state. Friend Rooms use the update-side human-only authority
// instead of the expanded stock player count.
kmCall(0x80533d2c, CanEndFriendRoomRace);
kmCall(0x80533d58, TryEndFriendRoomRace);

typedef void* (*RacedataFactoryFlagsConstructFn)(void* flags);
typedef void* (*RacedataFactoryConstructFn)(void* sender, void* flags);
typedef void (*RacedataFactoryPackFn)(void* sender);
typedef RKNet::RACEDATAPacket& (*OriginalGetRACEDATAFn)(RKNet::PacketMgr* packetMgr, u8 playerId);
typedef u32 (*OriginalGetPlayerRH1TimerFn)(const RKNet::PacketMgr* packetMgr, u32 playerId);
typedef void (*OriginalCreateKartModelFn)(Kart::Player* player);

kmRuntimeUse(0x8058d3cc);
kmRuntimeUse(0x8058ca28);
kmRuntimeUse(0x8058cb30);
kmRuntimeUse(0x80653abc);
kmRuntimeUse(0x806544a8);
kmRuntimeUse(0x8058f820);
kmRuntimeUse(0x806465e0);

static void BindHostRacedataFactory(u8 playerId, Kart::Player* kart) {
    if (!kart || playerId >= 12) return;

    void* flags = s.flags[playerId];
    void* sender = s.senders[playerId];
    if (!flags || !sender) return;

    // RacedataFactory_construct writes this link for newly-created senders.
    // Repeat it when the stock kart object is rebound to this CPU slot.
    *reinterpret_cast<void**>(reinterpret_cast<u8*>(sender) + 0x10) = flags;

    // KartNetSender and RacedataFactoryFlags are KartObjectProxy objects,
    // but they are created after KartObject::Create has finished its proxy
    // list setup.  Set their proxy pointer directly to this CPU's Pointers;
    // calling SetupInList here would relink the last kart's other proxies.
    *reinterpret_cast<Kart::Pointers**>(flags) = &kart->pointers;
    *reinterpret_cast<Kart::Pointers**>(sender) = &kart->pointers;

    // Stock KartObject::createRACEDATAStructs stores the sender at
    // KartPointers + 0x3c.  The CPU does not run that stock local-online
    // path, so restore the same link explicitly for every new kart instance.
    *reinterpret_cast<void**>(reinterpret_cast<u8*>(&kart->pointers) + 0x3c) = sender;
}

static void CreateHostRacedataFactory(u8 playerId, Kart::Player* kart) {
    if (!kart || !IsFriendRoomHost() || !IsFriendRoomCPU(playerId)) return;

    void*& flags = s.flags[playerId];
    void*& sender = s.senders[playerId];
    if (flags || sender) {
        BindHostRacedataFactory(playerId, kart);
        return;
    }

    // This runs from the stock Kart::Manager construction call, before the
    // race heap is closed to allocation.  Never call this from the update
    // hook: EGG::ExpHeap::alloc asserts once dameFlag is set.
    flags = EGG::Heap::alloc(0x28, 0x20);
    if (!flags) return;
    memset(flags, 0, 0x28);
    reinterpret_cast<RacedataFactoryFlagsConstructFn>(kmRuntimeAddr(0x8058d3cc))(flags);

    sender = EGG::Heap::alloc(0x5c, 0x20);
    if (!sender) {
        flags = nullptr;
        return;
    }
    memset(sender, 0, 0x5c);
    reinterpret_cast<RacedataFactoryConstructFn>(kmRuntimeAddr(0x8058ca28))(sender, flags);
    BindHostRacedataFactory(playerId, kart);
}

static void CreateFriendRoomKartModel(Kart::Player* player) {
    if (player != nullptr && player->values != nullptr)
        CreateHostRacedataFactory(player->values->playerIdx, player);

    reinterpret_cast<OriginalCreateKartModelFn>(kmRuntimeAddr(0x8058f820))(player);
}

// Kart::Manager::__ct has already returned from KartObject::Create at this
// call. Allocate the host CPU's sender/factory before vanilla createModel
// runs, while the construction heap is still active.
kmCall(0x8058fd80, CreateFriendRoomKartModel);

static RKNet::RACEDATAPacket s_emptyFriendRoomRaceData;

static bool IsFriendRoomRemoteCPU(u8 playerId) {
    bool isHost = false;
    return playerId < 12 && GetFriendRoomSession(&isHost) && !isHost &&
           IsSyntheticFriendRoomSlotInSession(playerId);
}

static RKNet::RACEDATAPacket& GetFriendRoomRACEDATA(RKNet::PacketMgr* packetMgr, u8 playerId) {
    if (IsFriendRoomRemoteCPU(playerId)) {
        if (s.raceValid[playerId]) return s.raceData[playerId];
        memset(&s_emptyFriendRoomRaceData, 0, sizeof(s_emptyFriendRoomRaceData));
        return s_emptyFriendRoomRaceData;
    }
    return reinterpret_cast<OriginalGetRACEDATAFn>(kmRuntimeAddr(0x80653abc))(packetMgr, playerId);
}

static u32 GetFriendRoomPlayerRH1Timer(const RKNet::PacketMgr* packetMgr, u32 playerId) {
    if (playerId < 12 && IsFriendRoomRemoteCPU(static_cast<u8>(playerId))) {
        return s.raceValid[playerId] ? s.raceSeq[playerId] : 0;
    }
    return reinterpret_cast<OriginalGetPlayerRH1TimerFn>(kmRuntimeAddr(0x806544a8))(packetMgr, playerId);
}

// These are the exact call sites used by RacedataHandler::unpackPackets and
// RacedataHandler::calc.  Feeding the custom record here lets the stock
// remote-player decoder and correction loop own movement on non-hosts.
kmCall(0x8058a1d0, GetFriendRoomRACEDATA);
kmCall(0x80589ab4, GetFriendRoomPlayerRH1Timer);
kmCall(0x80589ad8, GetFriendRoomPlayerRH1Timer);

bool WriteFriendRoomCPUState(PulRH1* packet) {
    bool isHost = false;
    if (!packet || !GetFriendRoomSession(&isHost) || !isHost) return false;

    const u8 cpuCount = GetFriendRoomCPUCount();
    if (cpuCount == 0) return false;

    FriendRoomCPUSyncPacket* sync = reinterpret_cast<FriendRoomCPUSyncPacket*>(reinterpret_cast<u8*>(packet) + PulRH1SizeFull);
    memset(sync, 0, sizeof(*sync));
    sync->magic = FRIEND_ROOM_CPU_MAGIC;
    sync->cpuCount = cpuCount;
    sync->raceDataPlayerId = 0xff;
    sync->cpuFinishMask = 0;

    const Raceinfo* raceinfo = Raceinfo::sInstance;
    const u32 sequence = raceinfo ? raceinfo->raceFrames : 0;
    const bool royaleResultsReady = IsFriendRoomBattleRoyale() && raceinfo &&
                                    raceinfo->stage >= RACESTAGE_IS_FINISHING;
    sync->reserved = (s.requestedResults || royaleResultsReady)
                         ? FRIEND_ROOM_CPU_FLAG_RESULTS_READY
                         : 0;
    if ((s.requestedResults || royaleResultsReady) && !s.loggedReady) {
        OS::Report("[PULSAR] friend-finish host-results-ready frame=%u royale=%u\n",
                   sequence, royaleResultsReady ? 1 : 0);
        s.loggedReady = true;
    }

    for (u8 position = 0; position < 12; ++position)
        sync->finishOrder[position] = position;

    sync->raceDataSequence = sequence;
    if (raceinfo && raceinfo->playerIdInEachPosition)
        memcpy(sync->finishOrder, raceinfo->playerIdInEachPosition, sizeof(sync->finishOrder));

    if (raceinfo && raceinfo->players) {
        for (u8 cpuIndex = 0; cpuIndex < cpuCount; ++cpuIndex) {
            const u8 playerId = GetFriendRoomCPUId(cpuIndex);
            const RaceinfoPlayer* player = raceinfo->players[playerId];
            if (player && ((player->stateFlags & (0x02 | 0x10)) != 0 ||
                           (player->raceFinishTime && player->raceFinishTime->isActive)))
                sync->cpuFinishMask |= static_cast<u16>(1u << playerId);
        }
    }

    RKNet::Controller* controller = RKNet::Controller::sInstance;
    if (controller) {
        const RKNet::ControllerSub& sub = controller->subs[controller->currentSub];
        for (u8 cpuIndex = 0; cpuIndex < cpuCount; ++cpuIndex)
            packet->aidsBelongingToPlayerIds[GetFriendRoomCPUId(cpuIndex)] = sub.localAid;
    }

    Item::Manager* itemManager = Item::Manager::sInstance;
    Kart::Manager* kartManager = Kart::Manager::sInstance;
    for (u8 cpuIndex = 0; cpuIndex < cpuCount; ++cpuIndex) {
        const u8 playerId = GetFriendRoomCPUId(cpuIndex);
        sync->playerIds[cpuIndex] = playerId;

        if (itemManager && itemManager->players && playerId < itemManager->playerCount) {
            const Item::Player& item = itemManager->players[playerId];
            sync->items[cpuIndex].storedItem = static_cast<u8>(item.inventory.currentItemId);
            sync->items[cpuIndex].storedItemCount = static_cast<u8>(item.inventory.currentItemCount);
            const bool isDragged = item.playerObj.itemObjId != OBJ_NONE &&
                                   !item.playerObj.isNotDragged &&
                                   item.playerObj.itemId < ITEM_NONE;
            sync->items[cpuIndex].activeItem = isDragged
                                                   ? static_cast<u8>(item.playerObj.itemId)
                                                   : static_cast<u8>(ITEM_NONE);
            sync->items[cpuIndex].flags = PackItemFlags(
                item.bitfield, isDragged ? static_cast<u8>(item.playerObj.activeItemCount) : 0);
        }
    }

    // One complete vanilla remote-player record per RH1.  RacedataFactory's
    // packet is at offset 0x14, confirmed from the PAL sender disassembly.
    const u8 raceDataIndex = static_cast<u8>(sequence % cpuCount);
    const u8 raceDataPlayerId = GetFriendRoomCPUId(raceDataIndex);
    if (kartManager) {
        Kart::Player* kart = kartManager->GetKartPlayer(raceDataPlayerId);
        BindHostRacedataFactory(raceDataPlayerId, kart);
        void* sender = s.senders[raceDataPlayerId];
        if (sender) {
            reinterpret_cast<RacedataFactoryPackFn>(kmRuntimeAddr(0x8058cb30))(sender);
            sync->raceDataPlayerId = raceDataPlayerId;
            memcpy(&sync->raceData, reinterpret_cast<u8*>(sender) + 0x14, sizeof(sync->raceData));
        }
    }
    return true;
}

void ReadFriendRoomCPUState(const PulRH1* packet, u32 packetSize, u8 senderAid) {
    bool isHost = false;
    if (!packet || !GetFriendRoomSession(&isHost) || isHost ||
        packetSize < PulRH1SizeFriendRoomCPU) return;

    RKNet::Controller* controller = RKNet::Controller::sInstance;
    if (!controller) return;
    const RKNet::ControllerSub& sub = controller->subs[controller->currentSub];
    if (senderAid != sub.hostAid) return;

    const FriendRoomCPUSyncPacket* sync = reinterpret_cast<const FriendRoomCPUSyncPacket*>(reinterpret_cast<const u8*>(packet) + PulRH1SizeFull);
    if (sync->magic != FRIEND_ROOM_CPU_MAGIC || sync->cpuCount > FriendRoomCPUCountMax) return;

    memset(s.itemValid, 0, sizeof(s.itemValid));
    for (u8 i = 0; i < sync->cpuCount; ++i) {
        const u8 playerId = sync->playerIds[i];
        if (playerId >= 12) continue;
        s.items[playerId] = sync->items[i];
        s.itemValid[playerId] = true;
        ApplyFriendRoomCPUItemPacket(playerId, sync->items[i]);
        controller->aidsBelongingToPlayerIds[playerId] = senderAid;
    }
    s.cpuFinishMask = sync->cpuFinishMask;
    if (sync->reserved & FRIEND_ROOM_CPU_FLAG_RESULTS_READY) {
        s.resultsReady = true;
        if (!s.loggedReady) {
            OS::Report("[PULSAR] friend-finish remote-results-ready frame=%u\n",
                       sync->raceDataSequence);
            s.loggedReady = true;
        }
    }

    bool validFinishOrder = true;
    u16 finishOrderMask = 0;
    for (u8 position = 0; position < 12; ++position) {
        const u8 playerId = sync->finishOrder[position];
        if (playerId >= 12 || (finishOrderMask & (1 << playerId)) != 0) {
            validFinishOrder = false;
            break;
        }
        finishOrderMask |= 1 << playerId;
    }
    if (validFinishOrder) {
        memcpy(s.finishOrder, sync->finishOrder, sizeof(s.finishOrder));
        s.finishValid = true;
    }

    const u8 raceDataPlayerId = sync->raceDataPlayerId;
    if (raceDataPlayerId < 12 && IsFriendRoomCPU(raceDataPlayerId) &&
        (!s.raceValid[raceDataPlayerId] ||
         sync->raceDataSequence != s.raceSeq[raceDataPlayerId])) {
        memcpy(&s.raceData[raceDataPlayerId], &sync->raceData, sizeof(sync->raceData));
        s.raceSeq[raceDataPlayerId] = sync->raceDataSequence;
        s.raceValid[raceDataPlayerId] = true;
    }
}

static void ApplyReceivedFriendRoomCPUFinishState() {
    bool isHost = false;
    if (!s.cpuFinishMask || !GetFriendRoomSession(&isHost) || isHost) return;

    Raceinfo* raceinfo = Raceinfo::sInstance;
    if (!raceinfo || !raceinfo->players) return;

    const u8 cpuCount = GetFriendRoomCPUCount();
    for (u8 cpuIndex = 0; cpuIndex < cpuCount; ++cpuIndex) {
        const u8 playerId = GetFriendRoomCPUId(cpuIndex);
        if ((s.cpuFinishMask & static_cast<u16>(1u << playerId)) == 0) continue;

        RaceinfoPlayer* player = raceinfo->players[playerId];
        if (player) player->stateFlags |= 0x02;
    }
}

static void ApplyReceivedCPUItems() {
    bool isHost = false;
    if (!GetFriendRoomSession(&isHost) || isHost) return;

    Item::Manager* itemManager = Item::Manager::sInstance;
    const u8 cpuCount = GetFriendRoomCPUCount();
    for (u8 i = 0; i < cpuCount; ++i) {
        const u8 playerId = GetFriendRoomCPUId(i);
        if (!s.itemValid[playerId]) continue;

        if (itemManager && itemManager->players && playerId < itemManager->playerCount) {
            Item::Player& item = itemManager->players[playerId];
            const FriendRoomCPUItem& received = s.items[playerId];
            const ItemId storedItem = static_cast<ItemId>(received.storedItem);
            if (item.inventory.currentItemId != storedItem)
                item.inventory.currentItemId = storedItem;
            if (item.inventory.currentItemCount != received.storedItemCount)
                item.inventory.currentItemCount = received.storedItemCount;
            // Do not write PlayerObj::itemObjId or its active flags directly.
            // Those fields reference locally spawned Obj instances; copying
            // only the ID creates a phantom tether with null usedObjs. The
            // host's copied button input drives the normal local UseItem path,
            // while the inventory snapshot supplies the authoritative item.
        }
    }
}

static void ApplyReceivedFriendRoomFinishOrder() {
    bool isHost = false;
    if (!s.finishValid || !GetFriendRoomSession(&isHost) || isHost) return;

    Raceinfo* raceinfo = Raceinfo::sInstance;
    Racedata* racedata = Racedata::sInstance;
    if (!raceinfo || !raceinfo->playerIdInEachPosition || !racedata) return;

    const bool orderChanged = memcmp(raceinfo->playerIdInEachPosition, s.finishOrder,
                                     sizeof(s.finishOrder)) != 0;
    // During the race the stock order is allowed to remain untouched until
    // the host sends a new order.  At the result boundary force the rank
    // fields once so a stock write cannot leave stale result rows behind.
    if (!orderChanged && raceinfo->stage < RACESTAGE_FINISHED) return;
    if (orderChanged)
        memcpy(raceinfo->playerIdInEachPosition, s.finishOrder,
               sizeof(s.finishOrder));

    // The result scene reads RacedataPlayer::finishPos, while race HUD and
    // leaderboard code read RaceinfoPlayer::position and the order array.
    // Update all three views from the host's authoritative order.
    for (u8 position = 0; position < 12; ++position) {
        const u8 playerId = s.finishOrder[position];
        if (playerId >= 12) continue;

        if (raceinfo->players && raceinfo->players[playerId])
            raceinfo->players[playerId]->position = position + 1;
        racedata->racesScenario.players[playerId].finishPos = position + 1;
        racedata->menusScenario.players[playerId].finishPos = position + 1;
        racedata->menusScenario.players[playerId].gpRank = position + 1;
    }

    if (!s.loggedRemote && raceinfo->stage >= RACESTAGE_FINISHED) {
        LogFriendRoomFinishSnapshot("nonhost-final-order", raceinfo);
        s.loggedRemote = true;
    }
}

static void RefreshReceivedFriendRoomCPUState();

// WiFiVSResults::OnActivate stores this flag at 0x15f7.  For the normal
// friend-room sections the stock value is false until the final GP race, and
// AfterEntranceAnimations immediately replaces the page when it is false.
// That is why forcing RaceHUD to PAGE_WIFI_VS_RESULTS previously appeared to
// skip the results screen entirely.
//
// This replaces only the stock store instruction.  Return the page pointer in
// r3 because the following stock instructions use r3 again after the call.
static void* SetFriendRoomResultsDisplayFlag(void* page) {
    if (page == nullptr) return page;

    SectionMgr* const sectionMgr = SectionMgr::sInstance;
    u32 displayResults = 0;
    if (sectionMgr != nullptr && sectionMgr->curSection != nullptr) {
        const u32 sectionId = static_cast<u32>(sectionMgr->curSection->sectionId);
        if (sectionMgr->sectionParams != nullptr) {
            // Friend VS result sections are the contiguous 0x70..0x77 block;
            // the low two-bit group selects VS (0) or battle (2).
            const u32 friendSection = sectionId - SECTION_P1_WIFI_FRIEND_VS;
            if (friendSection < 8 && (friendSection & 2) == 0) {
                displayResults = sectionMgr->sectionParams->onlineParams.currentRaceNumber > 2 ? 1 : 0;
            } else if (friendSection < 8) {
                displayResults = sectionMgr->sectionParams->redWins >= 2 ||
                                         sectionMgr->sectionParams->blueWins >= 2
                                     ? 1
                                     : 0;
            }
        }
    }

    // FriendRoomRegional may expose the converted race as public VS sections;
    // the transport flag still identifies it as the CPU-enabled friend room.
    reinterpret_cast<u8*>(page)[0x15f7] = static_cast<u8>(displayResults);
    return page;
}

// 0x8064610c is the stb r5,0x15f7(r3) in WiFiVSResults::OnActivate.
kmCall(0x8064610c, SetFriendRoomResultsDisplayFlag);

// WiFiVSResults::FillPlayerResults does not consume Raceinfo's position array.
// It iterates the player IDs and reads menusScenario.gpRank for each row, so
// apply the host order again at that final read boundary.
static void FillFriendRoomResults(void* page) {
    RefreshReceivedFriendRoomCPUState();
    ApplyReceivedFriendRoomCPUFinishState();
    ApplyReceivedFriendRoomFinishOrder();
    SyncFriendRoomCPUScenarioNames(Racedata::sInstance);
    SyncFriendRoomCPUSectionNames(true);
    if (!s.loggedResults) {
        LogFriendRoomFinishSnapshot(IsFriendRoomHost() ? "host-results" : "nonhost-results", Raceinfo::sInstance);
        s.loggedResults = true;
    }
    reinterpret_cast<void (*)(void*)>(kmRuntimeAddr(0x806465e0))(page);
}

kmCall(0x80646130, FillFriendRoomResults);

static void RefreshReceivedFriendRoomCPUState() {
    if (!IsFriendRoom() || IsFriendRoomHost()) return;

    RKNet::Controller* controller = RKNet::Controller::sInstance;
    if (!controller) return;
    const RKNet::ControllerSub& sub = controller->subs[controller->currentSub];
    const u8 hostAid = sub.hostAid;
    if (hostAid >= 12) return;
    const u8 bufferIdx = controller->lastReceivedBufferUsed[hostAid][RKNet::PACKET_RACEHEADER1];
    RKNet::SplitRACEPointers* split = controller->splitReceivedRACEPackets[bufferIdx][hostAid];
    if (!split) return;

    const RKNet::PacketHolder<PulRH1>* holder = split->GetPacketHolder<PulRH1>();
    if (!holder) return;
    ReadFriendRoomCPUState(holder->packet, holder->packetSize, hostAid);
}

void UpdateFriendRoomCPUs(AI::Manager* manager) {
    bool isHost = false;
    const bool friendRoom = GetFriendRoomSession(&isHost);
    if (!friendRoom) {
        if (manager) manager->Update();
        return;
    }

    const bool useHostCPUData = !isHost;
    if (useHostCPUData) {
        RefreshReceivedFriendRoomCPUState();
        ApplyReceivedFriendRoomCPUFinishState();
    }
    if (isHost) {
        Kart::Manager* kartManager = Kart::Manager::sInstance;
        if (kartManager) {
            const u8 cpuCount = GetFriendRoomCPUCount();
            for (u8 i = 0; i < cpuCount; ++i) {
                const u8 playerId = GetFriendRoomCPUId(i);
                BindHostRacedataFactory(playerId, kartManager->GetKartPlayer(playerId));
            }
        }
    }
    if (manager) manager->Update();
    if (useHostCPUData) {
        ApplyReceivedFriendRoomCPUFinishState();
        ForceFriendRoomRoyaleRemoteEndTransition(Raceinfo::sInstance);
    }
    // Remote kart movement/input is now consumed by the stock
    // RacedataHandler from the hooks above.  Only the separate item
    // inventory snapshot still needs a small friend-room update here.
    if (useHostCPUData) {
        ApplyReceivedCPUItems();
        ApplyReceivedFriendRoomFinishOrder();
    }
    UpdateFriendRoomFinishGateFrom("cpu-update");
    // Networking can rebuild the scenario/Mii records while the race is
    // active. Keep the old refresh point, but SetFriendRoomCPUName is now
    // idempotent and avoids the CRC/write work when the name is unchanged.
    SyncFriendRoomCPUScenarioNames(Racedata::sInstance);
    SyncFriendRoomCPUSectionNames(false);
    // Only the AID table must be refreshed every frame; networking may rebuild
    // it while the race is active.
    RefreshFriendRoomCPUAidMappings();
}
kmCall(0x80554bd8, UpdateFriendRoomCPUs);

}  // namespace Network
}  // namespace Pulsar
