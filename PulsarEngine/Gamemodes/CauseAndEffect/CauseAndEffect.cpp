#include <Gamemodes/CauseAndEffect/CauseAndEffect.hpp>
#include <MarioKartWii/Item/ItemManager.hpp>
#include <MarioKartWii/Item/Obj/Bomb.hpp>
#include <MarioKartWii/Item/Obj/ItemObjHolder.hpp>
#include <MarioKartWii/Audio/RSARPlayer.hpp>
#include <MarioKartWii/Kart/KartBody.hpp>
#include <MarioKartWii/Kart/KartManager.hpp>
#include <MarioKartWii/Kart/KartPhysics.hpp>
#include <MarioKartWii/Kart/KartValues.hpp>
#include <MarioKartWii/KMP/KMPManager.hpp>
#include <MarioKartWii/Race/RaceInfo/RaceInfo.hpp>
#include <MarioKartWii/RKNet/RKNetController.hpp>
#include <MarioKartWii/System/Identifiers.hpp>
#include <PulsarSystem.hpp>
#include <Settings/Settings.hpp>
#include <runtimeWrite.hpp>

namespace Pulsar {
namespace CauseAndEffect {

static const u32 kMaxPlayers = 12;
static const u32 kBoostTimerCount = 3;
static const u32 kTrickBoostTimerIndex = 2;
static const u32 kTimedRoundMaxLaps = 2;
static const u32 kTimedRoundFrames = 30 * 60;
static const float kMinimumStatFactor = 0.75f;
static const float kMaximumStatFactor = 1.25f;
static const float kStatBuffFactor = 1.05f;
static const float kStatNerfFactor = 0.95f;
static const float kLowGravityFactor = 0.75f;
static const float kHighGravityFactor = 1.25f;

static const u32 kWheelieStatusFlag = 0x20000000;
static const u32 kMiniTurboStatusFlag = 0x100000;
static const u32 kCannonStartStatusFlag = 0x8;
static const u32 kUseItemFlags = 0x2002;
struct PlayerState {
    TriggerState triggers;
    bool statsActive;
    float statsFactor;
    Kart::Stats *modifiedStats;
    Kart::Stats originalStats;
    bool ownsShock;
    s16 lastShockTimer;
    ItemId pendingItem;
    u32 randomCounter;
    s16 boostTimers[kBoostTimerCount];
    DamageType lastDamage;
    u8 lastPosition;
    float gravityFactor;
};
static PlayerState states[kMaxPlayers];
static bool applyingEffect;
static bool timedRoundStarted;
static u32 timedRoundStartFrame;
static u32 lastTimedRoundSound;

// PAL routines absent from symbols.txt; runtimeWrite resolves other supported regions.
kmRuntimeUse(0x805918bc);  // Link::IncrementHitOtherCount
kmRuntimeUse(0x8056eef4);  // Collision::CheckPlayerHitboxCollision
kmRuntimeUse(0x80581a58);  // Movement::ApplyInk
kmRuntimeUse(0x8057f3d8);  // Movement::ActivateMushroom
kmRuntimeUse(0x80580268);  // Movement::ActivateStar
kmRuntimeUse(0x80584044);  // Movement::SetInitialPhysicsValues
kmRuntimeUse(0x807ba2d8);  // PlayerRoulette::Reset (header calls this OnRouletteEnd)
kmRuntimeUse(0x80795350);  // PlayerObj::LoseItem
kmRuntimeUse(0x8022f8e4);  // EGG::Math::Atan2

extern "C" {
void SpawnItemInternal__Q24Item9ObjHolderFPQ24Item3Obj(Item::ObjHolder *, Item::Obj *);
void InitProperties__Q24Item3ObjFUiP4Vec3P4Vec3P4Vec3(Item::Obj *, u32, const Vec3 *, const Vec3 *, const Vec3 *);
void LoadEntity__Q24Item3ObjFb(Item::Obj *, bool);
}

static bool IsSupportedRaceMode(GameMode mode) {
    return mode == MODE_GRAND_PRIX || mode == MODE_VS_RACE || mode == MODE_PRIVATE_VS;
}

static bool IsOfflineCauseAndEffectSetting() {
    const RKNet::Controller *network = RKNet::Controller::sInstance;
    return network != nullptr && network->roomType == RKNet::ROOMTYPE_NONE && Settings::Mgr::IsCreated() &&
           Settings::Mgr::Get().GetSettingValue(Settings::SETTING_ITEMMODE) == GAMEMODE_CAUSEANDEFFECT;
}

bool IsEnabled() {
    const System *system = System::sInstance;
    const Racedata *data = Racedata::sInstance;
    if (system == nullptr || data == nullptr) return false;
    if (!IsSupportedRaceMode(data->racesScenario.settings.gamemode)) return false;
    if (system->IsContext(PULSAR_MODE_CAUSEANDEFFECT)) return true;

    // The offline HUD is created before some paths finish applying System's
    // context bits. The local Item Mode remains authoritative in that case.
    return IsOfflineCauseAndEffectSetting();
}

bool ShouldShowDisplay() {
    return IsEnabled() || (Settings::Mgr::IsCreated() &&
                           Settings::Mgr::Get().GetSettingValue(Settings::SETTING_ITEMMODE) == GAMEMODE_CAUSEANDEFFECT);
}

static Kart::Player *GetKart(u8 id) {
    Kart::Manager *manager = Kart::Manager::sInstance;
    return manager != nullptr && id < manager->playerCount && id < kMaxPlayers ? manager->GetKartPlayer(id)
                                                                                : static_cast<Kart::Player *>(nullptr);
}

static RaceinfoPlayer *GetRacePlayer(u8 id) {
    Raceinfo *race = Raceinfo::sInstance;
    return race != nullptr && Racedata::sInstance != nullptr &&
                   id < Racedata::sInstance->racesScenario.playerCount && id < kMaxPlayers
               ? race->players[id] : static_cast<RaceinfoPlayer *>(nullptr);
}

static Item::Player *GetItemPlayer(u8 id) {
    Item::Manager *manager = Item::Manager::sInstance;
    return manager != nullptr && id < manager->playerCount && id < kMaxPlayers ? &manager->players[id]
                                                                        : static_cast<Item::Player *>(nullptr);
}

static bool IsRacing(u8 id) {
    const RaceinfoPlayer *player = GetRacePlayer(id);
    return player != nullptr && (player->stateFlags & 0x32) == 0;
}

static bool IsOwned(const Kart::Player &kart) {
    // Online remotes and ghosts are never simulated a second time on this console.
    return (kart.pointers.kartStatus->bitfield4 & (0x8 | 0x40)) == 0;
}

u32 GetLap(u8 id) {
    const RaceinfoPlayer *player = GetRacePlayer(id);
    if (player == nullptr) return 1;
    // This is the same field CtrlRaceLap uses for the on-screen lap counter.
    // currentLap increments at the finish-line crossing, before a new lap has
    // been accepted, whereas maxLap remains the displayed active lap.
    return player->maxLap == 0 ? 1U : static_cast<u32>(player->maxLap);
}

bool UsesTimedRounds() {
    const Racedata *data = Racedata::sInstance;
    if (data == nullptr) return false;
    const u8 lapCount = data->racesScenario.settings.lapCount;
    return lapCount > 0 && lapCount <= kTimedRoundMaxLaps;
}

u32 GetRound(u8 id) {
    if (!UsesTimedRounds()) return GetLap(id);

    const Raceinfo *race = Raceinfo::sInstance;
    if (race == nullptr || !timedRoundStarted) return 1U;
    // raceFrames includes the countdown, so measure from its value on the
    // first gameplay frame instead of from the race load.
    return (race->raceFrames - timedRoundStartFrame) / kTimedRoundFrames + 1U;
}

static u32 GetSeed() {
    const RacedataSettings &settings = Racedata::sInstance->racesScenario.settings;
    // SELECT's agreed identifier is shared by every console for this race.
    // Offline InitRNG initializes the same field from OSGetTick instead.
    return settings.selectId ^ 0x43414546U;
}

static bool TrackHasCannon() {
    const KMP::Manager *manager = KMP::Manager::sInstance;
    if (manager == nullptr || manager->gobjSection == nullptr) return false;
    const KMP::GOBJSection *section = manager->gobjSection;
    if (section->holdersArray == nullptr) return false;

    for (u16 i = 0; i < section->pointCount; ++i) {
        const KMP::Holder<GOBJ> *holder = section->holdersArray[i];
        if (holder == nullptr || holder->raw == nullptr) continue;
        switch (holder->raw->objID) {
            case 0x017a:  // StarRing
            case 0x0259:  // DonkyCannonGC
            case 0x025f:  // DonkyCannon_wii
            case 0x0261:  // tree_cannon
                return true;
        }
    }
    return false;
}

Pair GetPair(u8 id) {
    return PairForRound(GetSeed(), GetRound(id), TrackHasCannon());
}

static void Queue(u8 id, Cause cause) {
    if (applyingEffect || !IsEnabled() || Raceinfo::sInstance == nullptr ||
        Raceinfo::sInstance->stage != RACESTAGE_RACE || !IsRacing(id)) return;
    Kart::Player *kart = GetKart(id);
    if (kart == nullptr || !IsOwned(*kart)) return;
    states[id].triggers.Queue(cause, GetRound(id));
}

void OnItemBox(u8 id) { Queue(id, CAUSE_ITEM_BOX); }

static void OnHitOther(Kart::Link &source) {
    reinterpret_cast<void (*)(Kart::Link *)>(kmRuntimeAddr(0x805918bc))(&source);
    Queue(source.GetPlayerIdx(), CAUSE_HIT_PLAYER);
}
// Accepted Star/Mega/Bullet collisions, Blooper/TC/POW/Lightning, and item-hit
// celebrations. The last call also handles native received RACEDATA hit reports.
kmCall(0x8056ff28, OnHitOther);
kmCall(0x8056ffac, OnHitOther);
kmCall(0x805701f8, OnHitOther);
kmCall(0x8057038c, OnHitOther);
kmCall(0x80570590, OnHitOther);
kmCall(0x80570718, OnHitOther);
kmCall(0x807a9488, OnHitOther);
kmCall(0x807af720, OnHitOther);
kmCall(0x807afd70, OnHitOther);
kmCall(0x807aff14, OnHitOther);
kmCall(0x807b2454, OnHitOther);
kmCall(0x807b7d5c, OnHitOther);
kmCall(0x807d19dc, OnHitOther);

static bool OnPlayerBump(Kart::Collision *collision, Vec3 *overlap, Vec3 *surface, Kart::Player *other) {
    const bool hit = reinterpret_cast<bool (*)(Kart::Collision *, Vec3 *, Vec3 *, Kart::Player *)>(kmRuntimeAddr(0x8056eef4))(
        collision, overlap, surface, other);
    if (hit && collision != nullptr && other != nullptr) {
        Queue(collision->GetPlayerIdx(), CAUSE_BUMP_PLAYER);
        Queue(other->GetPlayerIdx(), CAUSE_BUMP_PLAYER);
    }
    return hit;
}
// Called only after the game's player hitboxes overlap, so nearby racers do not trigger this cause.
kmCall(0x8056fb10, OnPlayerBump);

static u32 ActiveCauses(const Kart::Movement &move, const Kart::Status &status, const Item::Player *itemPlayer) {
    u32 active = 0;
    if ((status.bitfield0 & kWheelieStatusFlag) != 0) active |= 1U << CAUSE_WHEELIE;
    // The native MT-boost flag is set only when a charged drift is released.
    if ((status.bitfield1 & kMiniTurboStatusFlag) != 0) active |= 1U << CAUSE_DRIFT;
    if ((status.bitfield1 & kCannonStartStatusFlag) != 0) active |= 1U << CAUSE_ENTER_CANNON;
    // 0x2000 is set by the use-button path even when the player has no item.
    // Require the native inventory-present flag (0x2) as well.
    if (itemPlayer != nullptr && (itemPlayer->bitfield & kUseItemFlags) == kUseItemFlags) active |= 1U << CAUSE_USE_ITEM;
    if (move.boost.types != 0 || (status.bitfield0 & (0x100000 | 0x2000000 | 0x80000000)) != 0 ||
        (status.bitfield1 & (0x2000 | kMiniTurboStatusFlag)) != 0 || (status.bitfield2 & 0x2) != 0)
        active |= 1U << CAUSE_BOOST;
    return active;
}

static void ResetLapState(PlayerState &state) {
    state.pendingItem = ITEM_NONE;
    state.gravityFactor = 1.0f;
}

float GetGravityFactor(u8 id) {
    if (id >= kMaxPlayers) return 1.0f;
    const float factor = states[id].gravityFactor;
    return factor == 0.0f ? 1.0f : factor;
}

static Kart::Stats *GetStats(Kart::Player &kart) {
    return kart.pointers.values != nullptr ? kart.pointers.values->statsAndBsp.stats : static_cast<Kart::Stats *>(nullptr);
}

static void ApplyCharacterStats(Kart::Stats &stats, float factor) {
    stats.weight *= factor;
    stats.baseSpeed *= factor;
    // This is the speed retained in a normal turn, rather than steering
    // strength. Reducing it makes a handling nerf collapse the kart's speed.

    for (u32 i = 0; i < 4; ++i) stats.standard_acceleration_as[i] *= factor;
    for (u32 i = 0; i < 2; ++i) stats.drift_acceleration_as[i] *= factor;
    // The *_ts arrays are speed-ratio interpolation thresholds. Scaling them
    // shifts the acceleration curve and produces incorrect acceleration.

    stats.manualHandling *= factor;
    stats.autoHandling *= factor;
    stats.handlingReactivity *= factor;
    stats.manualDrift *= factor;
    stats.automaticDrift *= factor;
    stats.driftReactivity *= factor;
    stats.mt = static_cast<u32>(stats.mt * factor + 0.5f);
}

static void ApplyLiveSpeedStat(Kart::Movement &move, float factor) {
    // Movement caches the combined Speed stat at initialization. The other
    // character stats are read from Kart::Stats by their normal update paths.
    move.baseSpeed *= factor;
    move.unknown_0x1c *= factor;  // native CPU soft speed limit
}

static float ClampStatFactor(float factor) {
    if (factor < kMinimumStatFactor) return kMinimumStatFactor;
    if (factor > kMaximumStatFactor) return kMaximumStatFactor;
    return factor;
}

static void ApplyStatModifier(u8 id, Kart::Player &kart, float factor) {
    PlayerState &state = states[id];
    Kart::Stats *stats = GetStats(kart);
    if (stats == nullptr) return;

    if (!state.statsActive || state.modifiedStats != stats) {
        state.modifiedStats = stats;
        state.originalStats = *stats;
        state.statsFactor = 1.0f;
        state.statsActive = true;
    }

    const float previousFactor = state.statsFactor;
    // Stat buffs and nerfs stack within a ±25% range of the original values.
    state.statsFactor = ClampStatFactor(state.statsFactor * factor);
    // Reapply from the unmodified snapshot so stacked effects are exact and
    // mini-turbo rounding never accumulates across laps.
    *stats = state.originalStats;
    ApplyCharacterStats(*stats, state.statsFactor);
    ApplyLiveSpeedStat(*kart.pointers.kartMovement, state.statsFactor / previousFactor);
}

static void RestoreStatModifier(PlayerState &state, Kart::Player &kart) {
    if (!state.statsActive) return;
    Kart::Stats *stats = GetStats(kart);
    if (stats == state.modifiedStats && stats != nullptr) {
        *stats = state.originalStats;
        ApplyLiveSpeedStat(*kart.pointers.kartMovement, 1.0f / state.statsFactor);
    }
    state.statsActive = false;
    state.statsFactor = 1.0f;
    state.modifiedStats = nullptr;
}

static u32 NextRandom(u8 id) {
    PlayerState &state = states[id];
    return Mix(GetSeed() ^ (GetRound(id) * 0x9e3779b9U) ^ (id * 0x85ebca6bU) ^ ++state.randomCounter);
}

static void TryGrantItem(u8 id) {
    PlayerState &state = states[id];
    Item::Manager *manager = Item::Manager::sInstance;
    if (state.pendingItem == ITEM_NONE || manager == nullptr || id >= manager->playerCount) return;
    Item::Player &player = manager->players[id];
    // Release existing dragged/orbiting objects through their native lifetime/network path.
    // Grant after release, so the old triple-item update cannot erase the new inventory.
    if (player.playerObj.activeItemCount != 0 && player.playerObj.useType != Item::PlayerObj::ONLY_USE) {
        if (static_cast<u32>(player.playerObj.unknown_0x54) == 0)
            reinterpret_cast<void (*)(Item::PlayerObj *, u32)>(kmRuntimeAddr(0x80795350))(&player.playerObj, kMaxPlayers);
        return;
    }
    if ((player.pointers->kartStatus->bitfield2 & 0x8000000) != 0) return;
    ItemId item = state.pendingItem;
    if (!Item::Manager::IsThereCapacityForItem(item)) item = MUSHROOM;

    if (player.roulette.isTheRouletteSpinning != 0) {
        // Finish an active item-box roulette through its native path. Calling
        // OnRouletteEnd while inactive starts its HUD holder animation without
        // a matching roulette state, leaving the item icon squashed.
        player.roulette.nextItemId = item;
        player.roulette.isItemForcedDueToCapacity = false;
        reinterpret_cast<void (*)(Item::PlayerRoulette *)>(kmRuntimeAddr(0x807ba2d8))(&player.roulette);
    } else {
        player.inventory.ClearAll();
        player.inventory.SetItem(item, false);
    }
    state.pendingItem = ITEM_NONE;
}

static void SpawnBombAbove(u8 id, Kart::Player &kart) {
    Item::Manager *manager = Item::Manager::sInstance;
    if (manager == nullptr) return;
    Item::ObjHolder &holder = manager->itemObjHolders[OBJ_BOBOMB];
    if (holder.bodyCount >= holder.capacity) return;

    const Kart::PhysicsHolder *physics = kart.pointers.kartBody->kartPhysicsHolder;
    if (physics == nullptr) return;
    Vec3 position = physics->position;
    position.y += 300.0f;
    Item::Obj *obj = nullptr;
    holder.Spawn(1, &obj, id, position, false);
    if (obj == nullptr) return;
    if (obj->entity == nullptr) LoadEntity__Q24Item3ObjFb(obj, false);
    obj->bitfield78 &= ~0x20000;
    obj->playerUsedItemId = id;
    obj->bitfield7c &= ~0x20;
    SpawnItemInternal__Q24Item9ObjHolderFPQ24Item3Obj(&holder, obj);
    const Vec3 direction(0.0f, 0.0f, -1.0f);
    const Vec3 zero(0.0f, 0.0f, 0.0f);
    InitProperties__Q24Item3ObjFUiP4Vec3P4Vec3P4Vec3(obj, 0, &direction, &zero, &zero);
    Item::ObjBomb *bomb = static_cast<Item::ObjBomb *>(obj);
    bomb->timer = 90;
    *reinterpret_cast<u32 *>(reinterpret_cast<u8 *>(obj) + 0x1ac) = Item::ObjBomb::STATE_TICKING;
}

static void ClearItem(u8 id) {
    PlayerState &state = states[id];
    Item::Player *player = GetItemPlayer(id);
    state.pendingItem = ITEM_NONE;
    if (player == nullptr) return;

    // Discard tethered/triple objects through the native lifetime path before
    // clearing the inventory they originated from.
    if (player->playerObj.activeItemCount != 0 && player->playerObj.useType != Item::PlayerObj::ONLY_USE &&
        static_cast<u32>(player->playerObj.unknown_0x54) == 0) {
        reinterpret_cast<void (*)(Item::PlayerObj *, u32)>(kmRuntimeAddr(0x80795350))(&player->playerObj, kMaxPlayers);
    }
    player->inventory.ClearAll();
}

static void TurnAround(Kart::Player &kart, Kart::Movement &move) {
    const Kart::PhysicsHolder *physics = kart.pointers.kartBody->kartPhysicsHolder;
    if (physics == nullptr) return;

    // Reinitialize at the current position with the heading reversed. This is
    // the native physics path and does not enter the game's respawn state.
    const float heading =
        reinterpret_cast<float (*)(float, float)>(kmRuntimeAddr(0x8022f8e4))(move.dir.x, move.dir.z) * 57.2957795f + 180.0f;
    const Vec3 angles(0.0f, heading >= 360.0f ? heading - 360.0f : heading, 0.0f);
    reinterpret_cast<void (*)(Kart::Movement *, const Vec3 &, const Vec3 &)>(kmRuntimeAddr(0x80584044))(
        &move, physics->position, angles);
}

static void ApplyEffect(u8 id, Effect effect, Kart::Player &kart) {
    PlayerState &state = states[id];
    Kart::Movement &move = *kart.pointers.kartMovement;
    switch (effect) {
        case EFFECT_ITEM:
            state.pendingItem = static_cast<ItemId>(NextRandom(id) % 19);
            TryGrantItem(id);
            break;
        case EFFECT_SPEED_UP:
            ApplyStatModifier(id, kart, kStatBuffFactor);
            break;
        case EFFECT_SPEED_DOWN:
            ApplyStatModifier(id, kart, kStatNerfFactor);
            break;
        case EFFECT_LIGHTNING: {
            const s16 previous = move.shockTimer;
            move.ApplyLightning();
            if (move.shockTimer > previous) state.ownsShock = true;
            break;
        }
        case EFFECT_MEGA:
            move.ActivateMega();
            break;
        case EFFECT_BLOOPER:
            reinterpret_cast<void (*)(Kart::Movement *, bool)>(kmRuntimeAddr(0x80581a58))(&move, false);
            break;
        case EFFECT_BOMB:
            SpawnBombAbove(id, kart);
            break;
        case EFFECT_MUSHROOM_BOOST:
            reinterpret_cast<void (*)(Kart::Movement *)>(kmRuntimeAddr(0x8057f3d8))(&move);
            break;
        case EFFECT_CLEAR_ITEM:
            ClearItem(id);
            break;
        case EFFECT_TURN_AROUND:
            TurnAround(kart, move);
            break;
        case EFFECT_STAR:
            reinterpret_cast<void (*)(Kart::Movement *)>(kmRuntimeAddr(0x80580268))(&move);
            break;
        case EFFECT_LOW_GRAVITY:
            state.gravityFactor = kLowGravityFactor;
            break;
        case EFFECT_HIGH_GRAVITY:
            state.gravityFactor = kHighGravityFactor;
            break;
        default:
            break;
    }
    state.lastShockTimer = move.shockTimer;
}

void Reset() {
    // A race load may reuse the same player objects. Restore their snapshot
    // before discarding state so stacked modifiers never leak into a new race.
    for (u8 id = 0; id < kMaxPlayers; ++id) {
        Kart::Player *kart = GetKart(id);
        if (kart != nullptr) RestoreStatModifier(states[id], *kart);
    }
    memset(states, 0, sizeof(states));
    for (u32 id = 0; id < kMaxPlayers; ++id) {
        states[id].pendingItem = ITEM_NONE;
        states[id].statsFactor = 1.0f;
        states[id].gravityFactor = 1.0f;
        states[id].lastDamage = NO_DAMAGE;
    }
    applyingEffect = false;
    timedRoundStarted = false;
    timedRoundStartFrame = 0;
    lastTimedRoundSound = 0;
}
static RaceLoadHook resetCauseAndEffect(Reset);

static void UpdateDamageCause(u8 id, PlayerState &state, const Kart::Player &kart, const Kart::Movement &move,
                              bool playerRacing, bool newRound) {
    if (move.shockTimer > state.lastShockTimer || move.shockTimer <= 0) state.ownsShock = false;
    const DamageType damage = kart.pointers.kartDamage->currentDamage;
    // currentDamage is NO_DAMAGE between hits. Edge-detect its transition
    // instead of replacing KartAction::start, which also owns the native
    // damage callback and must always be allowed to run.
    if (!newRound && playerRacing && damage != NO_DAMAGE && state.lastDamage == NO_DAMAGE &&
        damage != SQUISH_RESPAWN && !(state.ownsShock && damage == SPINOUT_SHOCK)) {
        Queue(id, CAUSE_GET_HIT);
    }
    state.lastDamage = damage;
}

static void UpdateBoostCause(u8 id, PlayerState &state, const Kart::Movement &move, bool playerRacing, bool newRound) {
    const s16 timers[kBoostTimerCount] = {move.boost.mtFrames, move.boost.mushroomBoostPanelFrames, move.boost.trickZipperFrames};
    for (u32 i = 0; i < kBoostTimerCount; ++i) {
        // Also catch a second mushroom/MT while another boost is still active.
        if (!newRound && playerRacing && timers[i] > state.boostTimers[i]) {
            Queue(id, CAUSE_BOOST);
            // trickZipperFrames rises when the completed trick awards its boost.
            // Unlike the one-frame trick-start status flag, this cannot be missed
            // by the race-frame update order.
            if (i == kTrickBoostTimerIndex) Queue(id, CAUSE_TRICK);
        }
        state.boostTimers[i] = timers[i];
    }
}

static void UpdatePositionCause(u8 id, PlayerState &state, bool playerRacing, bool newRound) {
    const RaceinfoPlayer *player = GetRacePlayer(id);
    if (player == nullptr) return;
    const u8 position = player->position;
    if (!newRound && playerRacing && state.lastPosition != 0 && position != state.lastPosition) {
        Queue(id, CAUSE_POSITION_CHANGE);
    }
    state.lastPosition = position;
}

static void UpdatePlayer(u8 id, bool racing) {
    Kart::Player *kart = GetKart(id);
    if (kart == nullptr || !IsOwned(*kart)) return;
    PlayerState &state = states[id];
    Kart::Movement &move = *kart->pointers.kartMovement;
    const u32 active = ActiveCauses(move, *kart->pointers.kartStatus, GetItemPlayer(id));
    const bool newRound = state.triggers.EnterRound(GetRound(id), active);
    const bool playerRacing = racing && IsRacing(id);
    if (newRound || !playerRacing) {
        // A new round installs a new Cause and Effect pair. Clear all stacked
        // stat changes from the preceding pair before processing it.
        RestoreStatModifier(state, *kart);
        ResetLapState(state);
    }

    UpdateDamageCause(id, state, *kart, move, playerRacing, newRound);
    UpdateBoostCause(id, state, move, playerRacing, newRound);
    UpdatePositionCause(id, state, playerRacing, newRound);
    if (playerRacing) {
        const Pair pair = GetPair(id);
        applyingEffect = true;  // Lightning caused by this mode must not trigger itself.
        if (state.triggers.Consume(pair.cause, active)) ApplyEffect(id, pair.effect, *kart);
        TryGrantItem(id);
        applyingEffect = false;
    } else {
        state.triggers.observed = active;
        state.triggers.pending = 0;
    }
    state.lastShockTimer = move.shockTimer;
}

static void UpdateTimedRoundClock(bool racing) {
    if (!racing || !UsesTimedRounds()) return;

    Raceinfo *race = Raceinfo::sInstance;
    if (race == nullptr) return;
    if (!timedRoundStarted) {
        // This is the first frame after the countdown. The first 30-second
        // round begins here, regardless of how many countdown frames elapsed.
        timedRoundStartFrame = race->raceFrames;
        timedRoundStarted = true;
    }

    const u32 round = GetRound(0);
    if (round == lastTimedRoundSound) return;
    if (lastTimedRoundSound != 0 && round > lastTimedRoundSound) {
        Audio::RaceRSARPlayer *soundPlayer = static_cast<Audio::RaceRSARPlayer *>(Audio::RSARPlayer::sInstance);
        if (soundPlayer != nullptr) soundPlayer->PlaySound(SOUND_ID_NEW_RECORD, 0);
    }
    lastTimedRoundSound = round;
}

void Update() {
    if (!IsEnabled() || Raceinfo::sInstance == nullptr || Kart::Manager::sInstance == nullptr) return;
    const bool racing = Raceinfo::sInstance->stage == RACESTAGE_RACE;
    UpdateTimedRoundClock(racing);
    for (u8 id = 0; id < Kart::Manager::sInstance->playerCount && id < kMaxPlayers; ++id) UpdatePlayer(id, racing);
}

}  // namespace CauseAndEffect
}  // namespace Pulsar
