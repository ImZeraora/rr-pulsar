#include <Gamemodes/Battle/ScoreBased/ScoreBased.hpp>
#include <Gamemodes/Battle/ScoreBased/Rules.hpp>
#include <PulsarSystem.hpp>
#include <Network/PacketExpansion.hpp>
#include <MarioKartWii/Race/RaceInfo/RaceInfo.hpp>
#include <MarioKartWii/Race/RaceInfo/GameModeData.hpp>
#include <runtimeWrite.hpp>
#include <MarioKartWii/Item/Obj/ItemObj.hpp>

namespace Pulsar {
namespace ScoreBased {

static SyncState state;
static u16 appliedEliminations;
static bool finishApplied;

bool IsActive() {
    if (!System::sInstance || !Racedata::sInstance ||
        !System::sInstance->IsContext(PULSAR_BATTLE_SCOREBASED)) return false;
    const GameMode mode = Racedata::sInstance->menusScenario.settings.gamemode;
    return mode == MODE_BATTLE || mode == MODE_PRIVATE_BATTLE;
}

bool IsCoinBattle() {
    return IsActive() && Racedata::sInstance->menusScenario.settings.battleType == BATTLE_COIN;
}

bool IsSingleCoinBattle() {
    return IsCoinBattle() && SingleCoinFromSeed(Racedata::sInstance->racesScenario.settings.selectId);
}

u32 GetCoinTimerMilliseconds() {
    // Every HUD follows the current holder, including CPUs and remote players.
    // Individual saved countdowns remain independent when ownership changes.
    const u8 owner = state.coinOwner;
    return owner < state.playerCount && owner < 12 ? state.ownedRemainingMs[owner] : state.unownedRemainingMs;
}

static bool IsBalloonBattle() { return IsActive() && !IsCoinBattle(); }

static bool AreBalloonOpponents(u32 first, u32 second) {
    const RacedataScenario &scenario = Racedata::sInstance->racesScenario;
    if (first >= scenario.playerCount || second >= scenario.playerCount || first >= 12 || second >= 12 || first == second)
        return false;
    const Team firstTeam = scenario.players[first].team;
    const Team secondTeam = scenario.players[second].team;
    return (firstTeam == TEAM_RED && secondTeam == TEAM_BLUE) ||
           (firstTeam == TEAM_BLUE && secondTeam == TEAM_RED);
}

static bool BlockFriendlyBalloonHit(u32 first, u32 second) {
    return IsBalloonBattle() && !AreBalloonOpponents(first, second);
}

// PAL red-shell candidate selection calls the shared item-team predicate here.
// MiscRace.cpp's No Team Invincibility patches disable that predicate globally;
// bypass it only for Score Based Balloon red shells, using the real roster teams.
kmRuntimeUse(0x807a33e0);
static bool RejectFriendlyRedShellTarget(const Item::Obj *item, u32 target) {
    if (item->itemObjId == OBJ_RED_SHELL && !(item->bitfield78 & 0x8000) && IsBalloonBattle())
        return !AreBalloonOpponents(item->playerUsedItemId, target);
    return reinterpret_cast<bool (*)(const Item::Obj *, u32)>(kmRuntimeAddr(0x807a33e0))(item, target);
}
kmCall(0x807b3950, RejectFriendlyRedShellTarget);

// Keep native balloon loss, blink, and respawn behavior. Score Based Balloon
// Battle only removes the score penalty from a hit or a final balloon loss.
static u8 SnapshotBalloonScores(u16 *scores) {
    Raceinfo *race = Raceinfo::sInstance;
    if (!race || !IsBalloonBattle()) return 0;
    const u8 count = state.playerCount;
    for (u8 id = 0; id < count; ++id) scores[id] = race->players[id]->battleScore;
    return count;
}

static void RestoreBalloonScoreLosses(const u16 *scores, u8 count) {
    Raceinfo *race = Raceinfo::sInstance;
    if (!race || !count || !IsBalloonBattle()) return;
    for (u8 id = 0; id < count; ++id)
        if (race->players[id]->battleScore < scores[id]) race->players[id]->battleScore = scores[id];
}

static bool IsOnline() {
    return Racedata::sInstance->racesScenario.settings.gamemode == MODE_PRIVATE_BATTLE;
}

static bool IsAuthority() {
    if (!IsOnline()) return true;
    const RKNet::Controller *controller = RKNet::Controller::sInstance;
    if (!controller) return false;
    const RKNet::ControllerSub &sub = controller->subs[controller->currentSub];
    return sub.localAid == sub.hostAid;
}

static void Reset() {
    memset(&state, 0, sizeof(state));
    appliedEliminations = 0;
    finishApplied = false;
    if (!IsActive()) return;
    const u8 count = Racedata::sInstance->racesScenario.playerCount;
    state.playerCount = count <= 12 ? count : 12;
    state.singleCoin = IsSingleCoinBattle();
    state.coinOwner = 0xff;
    state.unownedRemainingMs = 180000;
    for (u8 id = 0; id < 12; ++id) state.ownedRemainingMs[id] = 20000;
}
static RaceLoadHook resetHook(Reset);

bool IsEliminated(u8 playerId) { return playerId < 12 && (state.eliminated & (1 << playerId)); }
bool HasFinished() { return state.finished != 0; }
u16 GetScoreLimit() { return ScoreLimitForPlayers(state.playerCount); }
u16 GetCutoff() { return state.round; }
u32 GetSecondsUntilCutoff() {
    const u32 cutoffMs = CoinCutoffMilliseconds(state.round);
    if (state.elapsedMs >= cutoffMs) return 0;
    return (cutoffMs - state.elapsedMs + 999) / 1000;
}
u8 GetSurvivorCount() {
    return Survivors(state.eliminated, state.playerCount);
}

static void ReadHostState() {
    RKNet::Controller *controller = RKNet::Controller::sInstance;
    if (!controller) return;
    const RKNet::ControllerSub &sub = controller->subs[controller->currentSub];
    const u8 host = sub.hostAid;
    if (host >= 12 || !(sub.availableAids & (1 << host))) return;
    const u32 buffer = controller->lastReceivedBufferUsed[host][RKNet::PACKET_RACEHEADER1];
    if (buffer >= 2 || !controller->splitReceivedRACEPackets[buffer][host]) return;
    const RKNet::PacketHolder<Network::PulRH1> *holder =
        controller->splitReceivedRACEPackets[buffer][host]->GetPacketHolder<Network::PulRH1>();
    if (!holder || !holder->packet || holder->packetSize < Network::PulRH1SizeFull) return;
    const Network::PulRH1 &packet = *holder->packet;
    const SyncState &received = packet.scoreBased;
    if (packet.selectId != Racedata::sInstance->racesScenario.settings.selectId || received.playerCount != state.playerCount ||
        received.finished > 1 || received.elapsedMs < state.elapsedMs || received.round < state.round ||
        (received.eliminated & state.eliminated) != state.eliminated ||
        (received.eliminated >> state.playerCount) != 0) return;
    if (received.singleCoin != state.singleCoin) return;
    if (state.singleCoin) {
        if ((received.coinOwner != 0xff && received.coinOwner >= state.playerCount) ||
            received.unownedRemainingMs > state.unownedRemainingMs || received.round != 0) return;
        for (u8 id = 0; id < state.playerCount; ++id)
            if (received.ownedRemainingMs[id] > state.ownedRemainingMs[id]) return;
    }
    // Only accept cumulative, structurally valid host snapshots for this race.
    if (received.round > CompletedRounds(received.elapsedMs)) return;
    for (u8 id = 0; id < state.playerCount; ++id) {
        if (received.eliminationRound[id] > received.round + 1) return;
    }
    state = received;
}

void WriteRH1Packet(Network::PulRH1 &packet) {
    memset(&packet.scoreBased, 0, sizeof(packet.scoreBased));
    if (IsActive() && IsAuthority()) packet.scoreBased = state;
}

static void Eliminate(u8 id, u16 round) {
    state.eliminated |= 1 << id;
    state.eliminationRound[id] = round;
}

static void UpdateAuthority(Raceinfo &race) {
    const u32 previousElapsedMs = state.elapsedMs;
    const Timer &elapsed = race.timerMgr->timers[0];
    state.elapsedMs = static_cast<u32>(elapsed.minutes) * 60000 + elapsed.seconds * 1000 + elapsed.milliseconds;
    for (u8 id = 0; id < state.playerCount; ++id) {
        if (!IsEliminated(id)) state.scores[id] = race.players[id]->battleScore;
    }
    if (!IsCoinBattle()) {
        u32 teamScores[2] = {0, 0};
        for (u8 id = 0; id < state.playerCount; ++id) {
            const u32 team = Racedata::sInstance->racesScenario.players[id].team;
            if (team < 2) teamScores[team] += state.scores[id];
        }
        // Balloon Battle ends at the first of the scaled countdown or score target.
        state.finished = state.elapsedMs >= BalloonCountdownMilliseconds(state.playerCount) ||
                         teamScores[0] >= GetScoreLimit() || teamScores[1] >= GetScoreLimit();
        return;
    }
    // Disconnects cannot leave an absent player alive indefinitely.
    for (u8 id = 0; id < state.playerCount; ++id) {
        if (!IsEliminated(id) && (race.players[id]->stateFlags & 0x10)) Eliminate(id, state.round + 1);
    }
    if (state.singleCoin) {
        // Native CoinManager/RH2 owns inventory; battleScore is its held-coin
        // count. Never write countdown progress into inventory during the race.
        state.coinOwner = 0xff;
        for (u8 id = 0; id < state.playerCount; ++id) {
            if (!IsEliminated(id) && state.scores[id] != 0) {
                state.coinOwner = id;
                break;
            }
        }
        const u32 delta = state.elapsedMs >= previousElapsedMs ? state.elapsedMs - previousElapsedMs : 0;
        state.finished = AdvanceCoinTimers(state.ownedRemainingMs, state.unownedRemainingMs,
                                           state.coinOwner, state.playerCount, delta);
        return;
    }
    const u16 round = CompletedRounds(state.elapsedMs);
    if (round > state.round) {
        state.round = round;
        // Evaluate the entire roster before checking for a winner, including a simultaneous wipeout.
        const u16 eliminated = EliminationMask(state.eliminated, state.scores, state.playerCount, round);
        for (u8 id = 0; id < state.playerCount; ++id) {
            if (!IsEliminated(id) && (eliminated & (1 << id))) Eliminate(id, round);
        }
    }
    state.finished = GetSurvivorCount() <= 1;
}

static bool RanksAhead(u8 a, u8 b, const Raceinfo &race) {
    const bool aOut = IsEliminated(a), bOut = IsEliminated(b);
    if (aOut != bOut) return !aOut;
    if (state.singleCoin && state.ownedRemainingMs[a] != state.ownedRemainingMs[b])
        return state.ownedRemainingMs[a] < state.ownedRemainingMs[b];
    if (aOut && state.eliminationRound[a] != state.eliminationRound[b])
        return state.eliminationRound[a] > state.eliminationRound[b];
    const u16 aScore = (aOut || state.finished) ? state.scores[a] : race.players[a]->battleScore;
    const u16 bScore = (bOut || state.finished) ? state.scores[b] : race.players[b]->battleScore;
    if (aScore != bScore) return aScore > bScore;
    return a < b;
}

static void ApplyState(Raceinfo &race) {
    if (IsCoinBattle()) {
        for (u8 id = 0; id < state.playerCount; ++id) {
            if (IsEliminated(id)) {
                RaceinfoPlayer &player = *race.players[id];
                if (!(appliedEliminations & (1 << id))) {
                    player.Vanish();
                    appliedEliminations |= 1 << id;
                }
                // Keep native Coin Runners from converting STOPPED into a new network disconnect.
                player.stateFlags |= 0x10;
                player.battleScore = state.scores[id];
            }
        }
        for (u8 id = 0; id < state.playerCount; ++id) {
            u8 position = 1;
            for (u8 other = 0; other < state.playerCount; ++other)
                if (other != id && RanksAhead(other, id, race)) ++position;
            race.players[id]->position = position;
            race.playerIdInEachPosition[position - 1] = id;
        }
    }
    if (!state.finished || finishApplied) return;
    finishApplied = true;
    Timer finish;
    finish.minutes = state.elapsedMs / 60000;
    finish.seconds = state.elapsedMs / 1000 % 60;
    finish.milliseconds = state.elapsedMs % 1000;
    finish.isActive = true;
    // Freeze the authoritative scores and finish once, in placement order.
    for (u8 id = 0; id < state.playerCount; ++id) {
        // Results show seconds of cumulative ownership; placements above use
        // full millisecond precision, even after the coin has been dropped.
        race.players[id]->battleScore = state.singleCoin ? (20000 - state.ownedRemainingMs[id]) / 1000 : state.scores[id];
    }
    for (u8 pos = 0; pos < state.playerCount; ++pos) {
        const u8 id = race.playerIdInEachPosition[pos];
        if (id < state.playerCount && !(race.players[id]->stateFlags & 0x2))
            race.players[id]->EndRace(finish, false, 2);
    }
    race.stage = RACESTAGE_IS_FINISHING;
}

static void Update() {
    if (!IsActive()) return;
    Raceinfo *race = Raceinfo::sInstance;
    if (!race || !race->timerMgr || race->stage != RACESTAGE_RACE || !state.playerCount) return;
    if (!state.finished) {
        if (IsAuthority()) UpdateAuthority(*race);
        else ReadHostState();
    }
    ApplyState(*race);
    UpdateSpectatorCameras();
}
static RaceFrameHook updateHook(Update);

// PAL Ghidra: TimerManager_updateTimers, 80535904; virtual slot +0x10.
// The native reverse-timer path displays timers[2] minus elapsed timers[0].
// Balloon's duration is also the host-authoritative timeout; Coin Runners
// leaves the race duration unlimited because each visible countdown is a cutoff.
kmRuntimeUse(0x80535904);
static void SetCountdownTarget(Timer &target, u32 milliseconds) {
    target.minutes = milliseconds / 60000;
    target.seconds = milliseconds / 1000 % 60;
    target.milliseconds = milliseconds % 1000;
    target.isActive = true;
}

static void UpdateTimer(RaceTimerMgr *timer) {
    if (IsActive()) {
        const u32 countdownTarget = IsSingleCoinBattle()
                                        ? state.elapsedMs + GetCoinTimerMilliseconds()
                                        : IsCoinBattle() ? CoinCutoffMilliseconds(state.round)
                                                         : BalloonCountdownMilliseconds(state.playerCount);
        timer->raceDurationMs = IsCoinBattle() ? 0xffffffff : countdownTarget;
        timer->isTimerReversed = true;
        if (IsCoinBattle()) timer->hasRaceTimeRanOut = false;
        SetCountdownTarget(timer->timers[2], countdownTarget);
    }
    reinterpret_cast<void (*)(RaceTimerMgr *)>(kmRuntimeAddr(0x80535904))(timer);
}
kmWritePointer(0x808b34c0, UpdateTimer);

// Retain native scoring, RH2 transport, coin accounting and arena updates.
kmRuntimeUse(0x80539574);
kmRuntimeUse(0x80539824);
kmRuntimeUse(0x8053bbf4);
kmRuntimeUse(0x8053d428);
typedef void (*BattleUpdate)(GMData *);
static void UpdateBattle(GMData *mode, u32 original, bool onlineBalloon) {
    u16 scores[12];
    const u8 scoreCount = SnapshotBalloonScores(scores);
    if (IsActive()) {
        if (finishApplied) return; // native updates must not re-sort or overwrite the final snapshot
        if (onlineBalloon) static_cast<GMDataOnlineBalloonBattle *>(mode)->timer.isActive = false;
    }
    reinterpret_cast<BattleUpdate>(original)(mode);
    RestoreBalloonScoreLosses(scores, scoreCount);
}
static void UpdateBalloon(GMData *mode) { UpdateBattle(mode, kmRuntimeAddr(0x80539574), false); }
static void UpdateCoins(GMData *mode) { UpdateBattle(mode, kmRuntimeAddr(0x80539824), false); }
static void UpdateOnlineBalloon(GMData *mode) { UpdateBattle(mode, kmRuntimeAddr(0x8053bbf4), true); }
static void UpdateOnlineCoins(GMData *mode) { UpdateBattle(mode, kmRuntimeAddr(0x8053d428), false); }
kmWritePointer(0x808b36f4, UpdateBalloon);
kmWritePointer(0x808b36a8, UpdateCoins);
kmWritePointer(0x808b3580, UpdateOnlineBalloon);
kmWritePointer(0x808b3534, UpdateOnlineCoins);

// PAL Ghidra: the four offline balloon callbacks remove balloons and apply score
// deltas. Preserve their visual/gameplay effects while preventing any negative
// battleScore change in Score Based Balloon Battle.
kmRuntimeUse(0x80538770);
kmRuntimeUse(0x80538994);
kmRuntimeUse(0x80538bc0);
kmRuntimeUse(0x80538ce0);
typedef void (*BalloonHit)(GMDataBalloonBattle *, u8, u8);
typedef void (*BalloonReceiver)(GMDataBalloonBattle *, u8);
static void RunBalloonHit(BalloonHit original, GMDataBalloonBattle *mode, u8 subject, u8 user) {
    if (BlockFriendlyBalloonHit(subject, user)) return;
    u16 scores[12];
    const u8 scoreCount = SnapshotBalloonScores(scores);
    original(mode, subject, user);
    RestoreBalloonScoreLosses(scores, scoreCount);
}
static void RunBalloonReceiver(BalloonReceiver original, GMDataBalloonBattle *mode, u8 player) {
    u16 scores[12];
    const u8 scoreCount = SnapshotBalloonScores(scores);
    original(mode, player);
    RestoreBalloonScoreLosses(scores, scoreCount);
}
static void BalloonItemCollision(GMDataBalloonBattle *mode, u8 subject, u8 user) {
    RunBalloonHit(reinterpret_cast<BalloonHit>(kmRuntimeAddr(0x80538770)), mode, subject, user);
}
static void BalloonKartCollision(GMDataBalloonBattle *mode, u8 collided, u8 collider) {
    RunBalloonHit(reinterpret_cast<BalloonHit>(kmRuntimeAddr(0x80538994)), mode, collided, collider);
}
static void BalloonOOBCollision(GMDataBalloonBattle *mode, u8 player) {
    RunBalloonReceiver(reinterpret_cast<BalloonReceiver>(kmRuntimeAddr(0x80538bc0)), mode, player);
}
static void BalloonObjectCollision(GMDataBalloonBattle *mode, u8 player) {
    RunBalloonReceiver(reinterpret_cast<BalloonReceiver>(kmRuntimeAddr(0x80538ce0)), mode, player);
}
kmWritePointer(0x808b3710, BalloonItemCollision);
kmWritePointer(0x808b3714, BalloonKartCollision);
kmWritePointer(0x808b3718, BalloonOOBCollision);
kmWritePointer(0x808b371c, BalloonObjectCollision);

// Online callbacks enqueue native hit/steal events instead of updating scores
// immediately. Filter before enqueueing so peers cannot award friendly-hit points.
kmRuntimeUse(0x8053b4cc);
kmRuntimeUse(0x8053b584);
typedef void (*OnlineBalloonHit)(GMDataOnlineBalloonBattle *, u8, u8);
static void OnlineBalloonItemCollision(GMDataOnlineBalloonBattle *mode, u8 first, u8 second) {
    if (!BlockFriendlyBalloonHit(first, second))
        reinterpret_cast<OnlineBalloonHit>(kmRuntimeAddr(0x8053b4cc))(mode, first, second);
}
static void OnlineBalloonKartCollision(GMDataOnlineBalloonBattle *mode, u8 first, u8 second) {
    if (!BlockFriendlyBalloonHit(first, second))
        reinterpret_cast<OnlineBalloonHit>(kmRuntimeAddr(0x8053b584))(mode, first, second);
}
kmWritePointer(0x808b359c, OnlineBalloonItemCollision);
kmWritePointer(0x808b35a0, OnlineBalloonKartCollision);

// +0x0c is the native end-all-players method (misnamed UpdateLocalPlayers in the header).
kmRuntimeUse(0x80535de8);
static void EndNativeBattle(GMData *mode) {
    if (!IsActive()) reinterpret_cast<BattleUpdate>(kmRuntimeAddr(0x80535de8))(mode);
}
kmWritePointer(0x808b36f0, EndNativeBattle);
kmWritePointer(0x808b36a4, EndNativeBattle);
kmWritePointer(0x808b357c, EndNativeBattle);
kmWritePointer(0x808b3530, EndNativeBattle);

// CoinManager's completed-animation check otherwise ends the race independently of its timer.
kmRuntimeUse(0x80883250);
static bool CoinsFinished(void *coinManager) {
    if (IsCoinBattle()) return false;
    return reinterpret_cast<bool (*)(void *)>(kmRuntimeAddr(0x80883250))(coinManager);
}
kmCall(0x8053991c, CoinsFinished);
kmCall(0x8053d6c8, CoinsFinished);

// Suppress the online balloon end event; this mode finishes from the host's RH1 snapshot.
kmRuntimeUse(0x8053b3cc);
static void BalloonEndEvent(GMData *mode, u32 event, u32 first, u32 second) {
    if (!IsActive()) reinterpret_cast<void (*)(GMData *, u32, u32, u32)>(kmRuntimeAddr(0x8053b3cc))(mode, event, first, second);
}
kmCall(0x8053c788, BalloonEndEvent);

const wchar_t *GetSettingText(s32 id) {
    switch (id) {
        case 0x68002: return L"Battle Rules";
        case 0x68030: return L"Time Based";
        case 0x68031: return L"Score Based";
        case 0x68300: return L"Use the normal battle timer and existing battle settings.";
        case 0x68301: return L"Balloon: team score target (75 at 12 players).\nCoins: 50% cutoff, 50% single coin (hold for 20 seconds).";
        default: return nullptr;
    }
}

} // namespace ScoreBased
} // namespace Pulsar
