#include <Gamemodes/Battle/ScoreBased/ScoreBased.hpp>
#include <MarioKartWii/AI/KartAIController.hpp>
#include <MarioKartWii/Kart/KartManager.hpp>
#include <MarioKartWii/Race/RaceInfo/RaceInfo.hpp>
#include <runtimeWrite.hpp>

namespace Pulsar {
namespace ScoreBased {

// Verified PAL prefixes. Keep the native state machine, route allocation,
// item use, steering, drift and stuck recovery; override only its destination.
struct BossAIInputs {
    KartAIController *controller;
};
struct BossAIPathPoint {
    u8 native[0x38];
    Vec3 target;
};
struct BossAIPath {
    u8 native[0x14];
    BossAIPathPoint *point;
    u8 native18[9];
    bool skipPrevious;
};
struct BossAIDriveInfo {
    Vec3 destination;
};
struct BossAIControl {
    u8 native[0x38];
    BossAIInputs *inputs;
    BossAIPath *path;
    u8 native40[0x1c];
    u32 routeMode;
};
struct BossAIPerception {
    void *vtable;
    BossAIInputs *inputs;
    KartAIController *lockedTarget;
    KartAIController *opponents[6];
    u32 classifications[6];
    u32 native3c;
};
typedef char CheckBossAIControl[(sizeof(BossAIControl) == 0x60) ? 1 : -1];
typedef char CheckBossAIPath[(offsetof(BossAIPath, skipPrevious) == 0x21) ? 1 : -1];
typedef char CheckBossAIPoint[(offsetof(BossAIPathPoint, target) == 0x38) ? 1 : -1];
typedef char CheckBossAIPerception[(sizeof(BossAIPerception) == 0x40) ? 1 : -1];

static KartAIController *GetBattleAI(u8 id) {
    Kart::Manager *manager = Kart::Manager::sInstance;
    if (!manager || !manager->players || id >= manager->playerCount || id >= 12 ||
        !manager->players[id]) return nullptr;
    KartAIController *ai = manager->players[id]->pointers.kartAIController;
    if (!ai || !ai->pointers || !ai->pointers->kartStatus) return nullptr;
    return ai;
}

static bool IsAvailable(const KartAIController &ai) {
    const Kart::Status &status = *ai.pointers->kartStatus;
    return !IsEliminated(ai.GetPlayerIdx()) && !(status.bitfield0 & 0x10) &&
           !(status.bitfield1 & 0x1a) && !(status.bitfield2 & 0xc6000);
}

static bool IsCPUBoss(const KartAIController *ai) {
    if (!ai || !ai->pointers || !ai->pointers->kartStatus || !IsBossBattle() ||
        HasFinished() || !Raceinfo::sInstance || Raceinfo::sInstance->stage != RACESTAGE_RACE)
        return false;
    const u8 id = ai->GetPlayerIdx();
    return id < Racedata::sInstance->racesScenario.playerCount && id == GetBossPlayerId() &&
           Racedata::sInstance->racesScenario.players[id].playerType == PLAYER_CPU &&
           !(ai->pointers->kartStatus->bitfield4 & 8) && IsAvailable(*ai);
}

static float DistanceSquared(const Vec3 &first, const Vec3 &second) {
    const float x = first.x - second.x;
    const float y = first.y - second.y;
    const float z = first.z - second.z;
    return x * x + y * y + z * z;
}

static KartAIController *FindPriorityHuman(KartAIController &boss) {
    const Kart::Status &status = *boss.pointers->kartStatus;
    if (status.bitfield2 & 0x08000000) return nullptr; // preserve straight Boss Bullet
    const bool powered = (status.bitfield1 & 0x80000000) || (status.bitfield2 & 0x8000);
    const RacedataScenario &scenario = Racedata::sInstance->racesScenario;
    KartAIController *nearest = nullptr;
    float nearestDistance = 0.0f;
    for (u8 id = 0; id < scenario.playerCount && id < 12; ++id) {
        if (id == boss.GetPlayerIdx() ||
            (scenario.players[id].playerType != PLAYER_REAL_LOCAL &&
             scenario.players[id].playerType != PLAYER_REAL_ONLINE)) continue;
        KartAIController *candidate = GetBattleAI(id);
        if (!candidate || !IsAvailable(*candidate) ||
            (!powered && !(candidate->pointers->kartStatus->bitfield2 & 0x80))) continue;
        const float distance = DistanceSquared(boss.GetPosition(), candidate->GetPosition());
        if (!nearest || distance < nearestDistance) {
            nearest = candidate;
            nearestDistance = distance;
        }
    }
    return nearest;
}

kmRuntimeUse(0x8072b618); // setBasicDriveInfo_: restore the native route destination
kmRuntimeUse(0x8072b680); // recompute steering angle after choosing a destination
kmRuntimeUse(0x8072a570); // AIControlBattle::doUpdate_: route, steering, stuck, drift
static void UpdateBossDriving(BossAIControl *control, BossAIDriveInfo *drive) {
    KartAIController *boss = control->inputs->controller;
    KartAIController *target = nullptr;
    if (IsCPUBoss(boss)) target = FindPriorityHuman(*boss);
    if (!target || !control->path || !control->path->point) {
        reinterpret_cast<void (*)(BossAIControl *, BossAIDriveInfo *)>(kmRuntimeAddr(0x8072a570))(control, drive);
        return;
    }

    BossAIPath &path = *control->path;
    const u32 oldMode = control->routeMode;
    const Vec3 oldTarget = path.point->target;
    const bool oldSkipPrevious = path.skipPrevious;
    const Vec3 targetPosition = target->GetPosition();
    // Native battle branch selector 8073e01c, case 9: take the ENPT nearest
    // this target. Allow turning back toward it at a route junction.
    control->routeMode = 9;
    path.point->target = targetPosition;
    path.skipPrevious = false;
    reinterpret_cast<void (*)(BossAIControl *, BossAIDriveInfo *)>(kmRuntimeAddr(0x8072b618))(control, drive);
    const Vec3 &position = boss->GetPosition();
    const float height = position.y - targetPosition.y;
    // Follow arena routes at range; aim at the kart for the final contact.
    // Avoid a direct approach to players on another level of the arena.
    if (height > -200.0f && height < 200.0f &&
        DistanceSquared(position, targetPosition) < 1000.0f * 1000.0f)
        drive->destination = targetPosition;
    reinterpret_cast<void (*)(BossAIControl *, BossAIDriveInfo *)>(kmRuntimeAddr(0x8072b680))(control, drive);
    reinterpret_cast<void (*)(BossAIControl *, BossAIDriveInfo *)>(kmRuntimeAddr(0x8072a570))(control, drive);
    // The native state machine keeps running. No chase state leaks into the
    // next frame after an effect expires, elimination, or a mode change.
    control->routeMode = oldMode;
    path.point->target = oldTarget;
    path.skipPrevious = oldSkipPrevious;
}
// Replace the doUpdate_ call in AIControlBattle::Update, after its state and
// angle updates. This callsite has verified mappings for all three regions.
kmCall(0x8072b5cc, UpdateBossDriving);

// Unlike the expanded manager team lists, each CPU's sensory cache has only
// six pointers (8072c5e8 and its consumers explicitly process six). The native
// initializer at 8072b874 otherwise writes eleven into those six slots.
kmRuntimeUse(0x80727cf8);
static u32 GetPerceptionOpponentCount(void *manager, u32 playerId) {
    const u32 count = reinterpret_cast<u32 (*)(void *, u32)>(kmRuntimeAddr(0x80727cf8))(manager, playerId);
    return count < 6 ? count : 6;
}
kmCall(0x8072b8c8, GetPerceptionOpponentCount);

kmRuntimeUse(0x8072c5e8);
static void UpdateBossPerception(BossAIPerception *perception) {
    KartAIController *boss = perception->inputs->controller;
    if (IsCPUBoss(boss)) {
        KartAIController *priority = FindPriorityHuman(*boss);
        KartAIController *nearest[6] = {};
        float distances[6] = {};
        u32 first = 0;
        if (priority) nearest[first++] = priority;
        const u8 count = Racedata::sInstance->racesScenario.playerCount;
        // Refresh from the whole roster, so players beyond the first six
        // opponents can be perceived too. Keep the priority human in slot 0.
        for (u8 id = 0; id < count && id < 12; ++id) {
            if (id == boss->GetPlayerIdx()) continue;
            KartAIController *candidate = GetBattleAI(id);
            if (!candidate || candidate == priority || !IsAvailable(*candidate)) continue;
            const float distance = DistanceSquared(boss->GetPosition(), candidate->GetPosition());
            for (u32 slot = first; slot < 6; ++slot) {
                if (nearest[slot] && distance >= distances[slot]) continue;
                for (u32 next = 5; next > slot; --next) {
                    nearest[next] = nearest[next - 1];
                    distances[next] = distances[next - 1];
                }
                nearest[slot] = candidate;
                distances[slot] = distance;
                break;
            }
        }
        for (u32 slot = 0; slot < 6; ++slot) {
            perception->opponents[slot] = nearest[slot];
            perception->classifications[slot] = 0;
        }
    }
    reinterpret_cast<void (*)(BossAIPerception *)>(kmRuntimeAddr(0x8072c5e8))(perception);
}
kmCall(0x8072b518, UpdateBossPerception);

} // namespace ScoreBased
} // namespace Pulsar
