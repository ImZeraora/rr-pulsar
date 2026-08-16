#include <Network/FriendRoomCPUs.hpp>

#include <MarioKartWii/Item/ItemManager.hpp>
#include <MarioKartWii/Kart/KartManager.hpp>
#include <MarioKartWii/Kart/KartLink.hpp>
#include <MarioKartWii/Race/RaceInfo/RaceInfo.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/RKNet/PacketMgr.hpp>
#include <MarioKartWii/RKNet/RKNetController.hpp>
#include <core/egg/mem/Heap.hpp>
#include <core/rvl/OS/OS.hpp>
#include <runtimeWrite.hpp>

namespace Pulsar {
namespace Network {

static const u8 FRIEND_ROOM_CPU_MAGIC = 0xC7;
static const u32 FRIEND_ROOM_NATIVE_TIMEOUT_FRAMES = 1801;

// These are stock online-VS routines.  The CPU bridge calls them only after
// the native race-mode path has decided that its online timeout elapsed.
kmRuntimeUse(0x8053e7ac);
kmRuntimeUse(0x8053e680);
kmRuntimeUse(0x8053ec40);
kmRuntimeUse(0x80533dd4);

// One state block keeps all receive buffers and CPU transport state resettable.
struct FriendRoomCPUState {
    FriendRoomCPUItem items[12];
    bool itemValid[12];
    RKNet::RACEDATAPacket raceData[12];
    u32 raceSeq[12];
    bool raceValid[12];
    RKNet::RACEHEADER2Packet rh2Data[12];
    u32 rh2Seq[12];
    bool rh2Valid[12];

    bool cpu[12];
    u8 cpuOrder[FriendRoomCPUCountMax];
    u8 cpuCount;
    bool rosterReady;
    void* senders[12];
    void* flags[12];

    bool active;
    bool host;
    u8 humans;
    bool cpuTimeoutApplied;
    u16 nativeTimeoutFrames;
    bool nativeTimeoutActive;
    u16 hostFinishedMask;
    u16 hostDisconnectedMask;
    u16 loggedFinishedMask;
    u16 loggedDisconnectedMask;
    u16 loggedTimeoutMilestone;
    u8 loggedStage;
    u8 loggedActiveCount;
    u8 loggedFinishedCount;
    u8 loggedDisconnectedCount;
    u8 loggedManagerFinishedCount;
    bool terminalLogValid;
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

// RaceModeOnlineVs::calc asks PacketMgr::GetRH2 for every non-local player.
// Friend-room CPU identities are carried through the compact RH1 extension,
// but the stock online decoder still consumes a normal RH2 record. Returning
// that cached record keeps CPU finish timers and ahead masks on the native
// RaceModeOnlineVs path.
static RKNet::RACEHEADER2Packet s_friendRoomCPUEmptyRH2;

static RKNet::RACEHEADER2Packet& GetFriendRoomRH2(RKNet::PacketMgr* packetMgr, u8 playerId) {
    if (playerId < 12 && GetFriendRoomSession(nullptr) && IsFriendRoomCPU(playerId)) {
        // RaceModeOnlineVs::calc decodes RH2 for every non-REAL_LOCAL slot,
        // including the host's synthetic CPU slots.  The host therefore must
        // consume the same cached RH2 record that it publishes to remotes;
        // returning an empty record here leaves the native finish timer and
        // players-ahead mask at zero, so the host can wait forever while a
        // remote already has the CPU's terminal state.
        if (s.rh2Valid[playerId]) return s.rh2Data[playerId];
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
            // A full room must refresh s.humans too, not fall through to the
            // cached value. ResetFriendRoomCPURaceState deliberately preserves
            // s.humans across races, so a room that grows to twelve between
            // races would otherwise keep the previous smaller count and let the
            // loop below overwrite real players with synthetic entries.
            if (playerCount != 0 && playerCount <= 12) {
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
    const u8 humans = s.humans;
    memset(&s, 0, sizeof(s));
    s.active = active;
    s.host = host;
    s.humans = humans;
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

static bool IsFriendRoomOnlineVS() {
    const Racedata* racedata = Racedata::sInstance;
    if (!racedata) return false;
    const GameMode mode = racedata->racesScenario.settings.gamemode;
    return mode == MODE_PRIVATE_VS || mode == MODE_PUBLIC_VS;
}

static u16 GetFriendRoomTimeoutLogMilestone(u16 frames) {
    if (frames >= FRIEND_ROOM_NATIVE_TIMEOUT_FRAMES) return FRIEND_ROOM_NATIVE_TIMEOUT_FRAMES;
    if (frames >= 1800) return 1800;
    if (frames >= 900) return 900;
    if (frames >= 300) return 300;
    if (frames >= 60) return 60;
    return frames == 0 ? 0 : 1;
}

static bool IsFriendRoomFinishTimerValid(const Timer* timer) {
    return timer && timer->isActive &&
           (timer->minutes != 0 || timer->seconds != 0 || timer->milliseconds != 0);
}

static void LogFriendRoomTerminalState(const char* source, const Raceinfo* raceinfo) {
    if (!raceinfo || !raceinfo->players) return;

    u8 activeCount = 0;
    u8 finishedCount = 0;
    u8 disconnectedCount = 0;
    for (u8 playerId = 0; playerId < 12; ++playerId) {
        const RaceinfoPlayer* player = raceinfo->players[playerId];
        if (!player) continue;

        const u32 flags = player->stateFlags;
        if (flags & 0x02) ++finishedCount;
        if (flags & 0x10) ++disconnectedCount;
        if ((flags & (0x02 | 0x10 | 0x20)) == 0) ++activeCount;
    }

    const u8* finishedCounter = reinterpret_cast<const u8*>(raceinfo) + 0x1c;
    const u16 timeoutMilestone = GetFriendRoomTimeoutLogMilestone(s.nativeTimeoutFrames);
    const u8 stage = static_cast<u8>(raceinfo->stage);
    const bool changed = !s.terminalLogValid ||
                         s.loggedFinishedMask != s.hostFinishedMask ||
                         s.loggedDisconnectedMask != s.hostDisconnectedMask ||
                         s.loggedTimeoutMilestone != timeoutMilestone ||
                         s.loggedStage != stage ||
                         s.loggedActiveCount != activeCount ||
                         s.loggedFinishedCount != finishedCount ||
                         s.loggedDisconnectedCount != disconnectedCount ||
                         s.loggedManagerFinishedCount != *finishedCounter;
    if (!changed) return;

    OS::Report("[PULSAR] friend-terminal source=%s frame=%u stage=%u hostFinish=0x%04x hostDisconnect=0x%04x timeout=%u active=%u finished=%u disconnected=%u managerFinished=%u\n",
               source, raceinfo->raceFrames, stage, s.hostFinishedMask,
               s.hostDisconnectedMask, s.nativeTimeoutFrames, activeCount,
               finishedCount, disconnectedCount, *finishedCounter);
    s.loggedFinishedMask = s.hostFinishedMask;
    s.loggedDisconnectedMask = s.hostDisconnectedMask;
    s.loggedTimeoutMilestone = timeoutMilestone;
    s.loggedStage = stage;
    s.loggedActiveCount = activeCount;
    s.loggedFinishedCount = finishedCount;
    s.loggedDisconnectedCount = disconnectedCount;
    s.loggedManagerFinishedCount = *finishedCounter;
    s.terminalLogValid = true;
}

static void ApplyReceivedFriendRoomTerminalState() {
    bool isHost = false;
    if (!GetFriendRoomSession(&isHost) || isHost || !IsFriendRoomOnlineVS() ||
        (s.hostFinishedMask == 0 && s.hostDisconnectedMask == 0))
        return;

    Raceinfo* raceinfo = Raceinfo::sInstance;
    if (!raceinfo || !raceinfo->players || raceinfo->stage < RACESTAGE_RACE ||
        raceinfo->stage >= RACESTAGE_FINISHED)
        return;

    const u16 disconnectedMask = s.hostDisconnectedMask;
    const u16 finishedMask = s.hostFinishedMask & ~disconnectedMask;
    for (u8 playerId = 0; playerId < 12; ++playerId) {
        RaceinfoPlayer* player = raceinfo->players[playerId];
        if (!player) continue;

        const u16 bit = static_cast<u16>(1u << playerId);
        if (disconnectedMask & bit) {
            if ((player->stateFlags & 0x10) == 0) {
                raceinfo->SetPlayerDisconnected(playerId);
                raceinfo->CheckEndRaceOnline(playerId);
            }
            continue;
        }

        if ((finishedMask & bit) == 0 ||
            (player->stateFlags & (0x02 | 0x10)) != 0)
            continue;

        // The mask is only a delivery/terminal hint.  Never manufacture a
        // finish time from the local race clock: EndRace snapshots the
        // players-ahead flags, and an artificial timestamp changes native
        // placement ordering for every later finisher.  The stock RH2 pass
        // will call EndRace once the authoritative timer arrives.
        if (!IsFriendRoomFinishTimerValid(player->raceFinishTime))
            continue;
        player->EndRace(*player->raceFinishTime, false, 5);
    }
}

typedef void (*FriendRoomNativeRH2FinishFn)(GMDataOnlineVS*);

static void ApplyFriendRoomNativeRH2Finish(GMDataOnlineVS* mode) {
    reinterpret_cast<FriendRoomNativeRH2FinishFn>(kmRuntimeAddr(0x8053e680))(mode);
    ApplyReceivedFriendRoomTerminalState();
    bool isHost = false;
    if (GetFriendRoomSession(&isHost) && !isHost)
        LogFriendRoomTerminalState("native-rh2", Raceinfo::sInstance);
}

// RaceModeOnlineVs::calc calls the stock RH2 finish pass here.  Applying the
// host's terminal mask immediately after that pass keeps CPU/human records in
// the same native state before position tracking, timeout, and scene logic.
kmCall(0x8053f2f4, ApplyFriendRoomNativeRH2Finish);

static void ApplyReceivedFriendRoomNativeTimeout() {
    bool isHost = false;
    if (!s.nativeTimeoutActive || !GetFriendRoomSession(&isHost) || isHost ||
        !IsFriendRoomOnlineVS())
        return;

    Raceinfo* raceinfo = Raceinfo::sInstance;
    if (!raceinfo || raceinfo->stage < RACESTAGE_RACE ||
        raceinfo->stage >= RACESTAGE_FINISHED)
        return;

    // Ghidra: RaceModeOnlineVs::_10c is the frame counter used by the stock
    // 1,801-frame timeout at 0x8053ec40.  The host's value is authoritative;
    // never move a remote counter backwards when packets arrive out of order.
    if (raceinfo->gamemodeData) {
        u8* modeBytes = reinterpret_cast<u8*>(raceinfo->gamemodeData);
        u32* timeoutFrames = reinterpret_cast<u32*>(modeBytes + 0x10c);
        if (*timeoutFrames < s.nativeTimeoutFrames)
            *timeoutFrames = s.nativeTimeoutFrames;
    }

    // Ghidra: Raceinfo's finished-player counter is the byte at +0x1c used
    // by 0x8053ec40 to decide whether _10c should advance.  A host CPU finish
    // is not otherwise present in the remote RaceManager, so publish the
    // native "at least one player finished" prerequisite without changing
    // the native end/results state machine.
    u8* finishedCount = reinterpret_cast<u8*>(raceinfo) + 0x1c;
    if (*finishedCount == 0) *finishedCount = 1;
}

typedef bool (*FriendRoomNativeTimeoutFn)(GMDataOnlineVS*);

static void StopFriendRoomCPUsAtNativeTimeout() {
    if (s.cpuTimeoutApplied || !GetFriendRoomSession(nullptr) ||
        !IsFriendRoomOnlineVS())
        return;

    Raceinfo* raceinfo = Raceinfo::sInstance;
    if (!raceinfo || !raceinfo->players || raceinfo->stage < RACESTAGE_RACE ||
        raceinfo->stage >= RACESTAGE_FINISHED)
        return;

    s.cpuTimeoutApplied = true;
    const u8 cpuCount = GetFriendRoomCPUCount();
    for (u8 cpuIndex = 0; cpuIndex < cpuCount; ++cpuIndex) {
        const u8 playerId = GetFriendRoomCPUId(cpuIndex);
        RaceinfoPlayer* player = raceinfo->players[playerId];
        if (!player || (player->stateFlags & (0x02 | 0x10 | 0x20)) != 0) continue;

        // A CPU which has not crossed the line is not a finisher.  Stop it
        // first, then run the stock disconnect transition immediately.  The
        // normal RaceModeOnlineVs pass does this on its next iteration, but a
        // remote client can otherwise leave the CPU in STOPPED while its
        // local race-end check is already waiting for terminal players.
        raceinfo->SetPlayerDisconnected(playerId);
        raceinfo->CheckEndRaceOnline(playerId);
    }
}

static bool UpdateFriendRoomNativeTimeout(GMDataOnlineVS* mode) {
    // The stock timeout checks RaceManager::m_finishedPlayerCount before it
    // increments _10c.  Apply both host state and the replicated counter
    // before entering the native function, not after it.
    ApplyReceivedFriendRoomTerminalState();
    ApplyReceivedFriendRoomNativeTimeout();
    const bool timedOut = reinterpret_cast<FriendRoomNativeTimeoutFn>(
        kmRuntimeAddr(0x8053ec40))(mode);
    if (timedOut ||
        (!IsFriendRoomHost() && s.nativeTimeoutActive &&
         s.nativeTimeoutFrames >= FRIEND_ROOM_NATIVE_TIMEOUT_FRAMES)) {
        // The host's RH1 is assembled before RaceModeOnlineVs::calc advances
        // _10c.  A remote can therefore receive the terminal value one frame
        // before its local stock timeout returns true. Stop the synthetic
        // slots at that boundary; the native race manager still decides when
        // the scene can enter results.
        StopFriendRoomCPUsAtNativeTimeout();
    }
    ApplyReceivedFriendRoomTerminalState();
    bool isHost = false;
    if (GetFriendRoomSession(&isHost) && !isHost)
        LogFriendRoomTerminalState("native-timeout", Raceinfo::sInstance);
    return timedOut;
}

// RaceModeOnlineVs::calc calls FUN_8053ec40 here.  Keep the native 1,801-frame
// countdown and only bridge its terminal event to the synthetic CPU records.
kmCall(0x8053f39c, UpdateFriendRoomNativeTimeout);

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

typedef int (*FriendRoomRH2PackFn)(GMDataOnlineVS*);

static bool PackFriendRoomCPUHeader(GMDataOnlineVS* mode, u8 playerId,
                                    RKNet::RACEHEADER2Packet* output) {
    if (!mode || !output || playerId >= 12) return false;

    // RaceModeOnlineVs::PackRH2 uses localPlayerIds at 0x108/0x109. Save the
    // complete RH2/state tail because the packer also updates its bit masks
    // and packer parameters while producing the record.
    u8 savedState[0x7c];
    u8 savedPlayer[sizeof(GMDataOnlineVSPlayer)];
    u8* modeBytes = reinterpret_cast<u8*>(mode);
    memcpy(savedState, modeBytes + 0xf8, sizeof(savedState));
    memcpy(savedPlayer, &mode->players[playerId], sizeof(savedPlayer));

    // The stock packer normally gets these values from the receive-side RH2
    // timer array. A CPU is local to the host, so mirror its actual
    // RaceinfoPlayer finish state into the same fields for this temporary
    // pack. This makes the remote decoder see the CPU's native finish bit and
    // timer instead of only receiving a movement-shaped empty RH2.
    Raceinfo* raceinfo = Raceinfo::sInstance;
    RaceinfoPlayer* racePlayer = 0;
    if (raceinfo && raceinfo->players)
        racePlayer = raceinfo->players[playerId];
    const u32 stateFlags = racePlayer ? racePlayer->stateFlags : 0;
    const bool finished = (stateFlags & 0x02) != 0 && racePlayer &&
                          IsFriendRoomFinishTimerValid(racePlayer->raceFinishTime);
    const bool disconnected = (stateFlags & 0x10) != 0;
    // GMDataOnlineVS initializes remote-player timers inactive.  A
    // RaceinfoPlayer owns an active zeroed timer from construction, so copy
    // it only after the player has actually finished; otherwise the RH2
    // packer advertises a valid 0:00 finish record for every CPU.
    if (finished)
        mode->players[playerId].raceFinishTime = *racePlayer->raceFinishTime;
    else
        mode->players[playerId].raceFinishTime.isActive = false;
    *reinterpret_cast<u16*>(reinterpret_cast<u8*>(&mode->players[playerId]) + 0xc) =
        finished ? static_cast<u16>(1u << playerId) : 0;
    *reinterpret_cast<u16*>(reinterpret_cast<u8*>(&mode->players[playerId]) + 0x10) =
        disconnected ? static_cast<u16>(1u << playerId) : 0;
    modeBytes[0x108] = playerId;
    modeBytes[0x109] = 0xff;

    reinterpret_cast<FriendRoomRH2PackFn>(kmRuntimeAddr(0x8053e7ac))(mode);
    memcpy(output, &mode->rh2Packet, sizeof(*output));
    memcpy(&mode->players[playerId], savedPlayer, sizeof(savedPlayer));
    memcpy(modeBytes + 0xf8, savedState, sizeof(savedState));
    return true;
}

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
    sync->rh2PlayerId = 0xff;
    sync->nativeTimeoutFrames = 0;
    sync->nativeTimeoutActive = 0;
    sync->hostFinishedMask = 0;
    sync->hostDisconnectedMask = 0;

    const Raceinfo* raceinfo = Raceinfo::sInstance;
    const u32 sequence = raceinfo ? raceinfo->raceFrames : 0;
    sync->raceDataSequence = sequence;

    if (raceinfo && raceinfo->players) {
        for (u8 playerId = 0; playerId < 12; ++playerId) {
            const RaceinfoPlayer* player = raceinfo->players[playerId];
            if (!player) continue;

            const u16 bit = static_cast<u16>(1u << playerId);
            if (player->stateFlags & 0x02) sync->hostFinishedMask |= bit;
            if (player->stateFlags & (0x10 | 0x20))
                sync->hostDisconnectedMask |= bit;
        }
    }

    if (raceinfo && IsFriendRoomOnlineVS() && raceinfo->gamemodeData) {
        const u32 nativeTimeoutFrames = *reinterpret_cast<const u32*>(
            reinterpret_cast<const u8*>(raceinfo->gamemodeData) + 0x10c);
        // RH1 is built before the native RaceModeOnlineVs::calc call. Carry
        // the value that will exist after that call so the remote cannot miss
        // the terminal 1,801-frame packet when the host changes scenes.
        const u32 nextNativeTimeoutFrames =
            nativeTimeoutFrames == 0
                ? 0
                : (nativeTimeoutFrames < FRIEND_ROOM_NATIVE_TIMEOUT_FRAMES
                       ? nativeTimeoutFrames + 1
                       : FRIEND_ROOM_NATIVE_TIMEOUT_FRAMES);
        sync->nativeTimeoutFrames = static_cast<u16>(nextNativeTimeoutFrames);
        sync->nativeTimeoutActive = sync->nativeTimeoutFrames != 0;
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

    // RH2 is the stock online finish transport. Pack one CPU's normal RH2
    // record per RH1 and rotate it with the movement record above.
    if (raceinfo && IsFriendRoomOnlineVS() && raceinfo->gamemodeData) {
        RKNet::RACEHEADER2Packet rh2 = {};
        GMDataOnlineVS* mode = reinterpret_cast<GMDataOnlineVS*>(raceinfo->gamemodeData);
        if (PackFriendRoomCPUHeader(mode, raceDataPlayerId, &rh2)) {
            sync->rh2PlayerId = raceDataPlayerId;
            memcpy(&sync->rh2Data, &rh2, sizeof(rh2));
            memcpy(&s.rh2Data[raceDataPlayerId], &rh2, sizeof(rh2));
            s.rh2Seq[raceDataPlayerId] = sequence;
            s.rh2Valid[raceDataPlayerId] = true;
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

    const u8 raceDataPlayerId = sync->raceDataPlayerId;
    if (raceDataPlayerId < 12 && IsFriendRoomCPU(raceDataPlayerId) &&
        (!s.raceValid[raceDataPlayerId] ||
         sync->raceDataSequence != s.raceSeq[raceDataPlayerId])) {
        memcpy(&s.raceData[raceDataPlayerId], &sync->raceData, sizeof(sync->raceData));
        s.raceSeq[raceDataPlayerId] = sync->raceDataSequence;
        s.raceValid[raceDataPlayerId] = true;
    }

    const u8 rh2PlayerId = sync->rh2PlayerId;
    if (rh2PlayerId < 12 && IsFriendRoomCPU(rh2PlayerId) &&
        (!s.rh2Valid[rh2PlayerId] ||
         sync->raceDataSequence != s.rh2Seq[rh2PlayerId])) {
        memcpy(&s.rh2Data[rh2PlayerId], &sync->rh2Data, sizeof(sync->rh2Data));
        s.rh2Seq[rh2PlayerId] = sync->raceDataSequence;
        s.rh2Valid[rh2PlayerId] = true;
    }

    if (sync->nativeTimeoutActive) {
        if (!s.nativeTimeoutActive ||
            sync->nativeTimeoutFrames > s.nativeTimeoutFrames)
            s.nativeTimeoutFrames = sync->nativeTimeoutFrames;
        s.nativeTimeoutActive = true;
    }

    s.hostFinishedMask |= sync->hostFinishedMask;
    s.hostDisconnectedMask |= sync->hostDisconnectedMask;
    LogFriendRoomTerminalState("rh1-recv", Raceinfo::sInstance);

    // Apply immediately as well as from the race update hook.  Once a local
    // racer finishes, RaceScene can enter its spectator update path, which
    // does not call the AI-manager hook on every frame.
    ApplyReceivedFriendRoomTerminalState();
    ApplyReceivedFriendRoomNativeTimeout();
    if (s.nativeTimeoutActive &&
        s.nativeTimeoutFrames >= FRIEND_ROOM_NATIVE_TIMEOUT_FRAMES)
        StopFriendRoomCPUsAtNativeTimeout();
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

static void RefreshReceivedFriendRoomCPUState();

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
    // Remote kart movement/input is now consumed by the stock
    // RacedataHandler from the hooks above.  Only the separate item
    // inventory snapshot still needs a small friend-room update here.
    if (useHostCPUData) {
        ApplyReceivedCPUItems();
    }
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
