#include <Gamemodes/Battle/ScoreBased/ScoreBased.hpp>
#include <MarioKartWii/Kart/KartPointers.hpp>
#include <runtimeWrite.hpp>

namespace Pulsar {
namespace ScoreBased {

static Quat bulletRotation;
static Vec3 bulletDirection;

// The native activation uses an ITPT before doing the safe model, sound,
// hitbox, speed-limit and network setup. Only replace that route-settings call.
kmRuntimeUse(0x8059d2d4);
static void InitBulletSettings(Kart::Killer *killer, u8 point) {
    if (!IsBoss(killer->GetPlayerIdx())) {
        reinterpret_cast<void (*)(Kart::Killer *, u8)>(kmRuntimeAddr(0x8059d2d4))(killer, point);
        return;
    }
    killer->nextITPT = 0xff;
    killer->curITPT = 0xff;
    killer->settingBitfield = 1; // ordinary gravity, no route-dependent flying
    bulletRotation = killer->GetPhysicsHolder().physics->mainRot;
    bulletDirection = killer->GetMovement().dir;
}
kmCall(0x8059b850, InitBulletSettings);

kmRuntimeUse(0x8059c118); // native cancellation, including model, sound and speed restoration
bool UpdateBossBullet(Kart::Killer &killer) {
    if (!IsBoss(killer.GetPlayerIdx())) return false;
    Kart::Status &status = *killer.pointers->kartStatus;
    // Remote movement/rotation and ending are supplied by native RACEDATA.
    // Never run the route-following update on the receiving console either.
    if (status.bitfield4 & 8) return true;
    if ((status.bitfield0 & 0x70) || IsEliminated(killer.GetPlayerIdx()) || HasFinished()) {
        // Wall or OOB contact ends immediately. 0xff makes the native cancel's
        // PlayerItemPoint reset skip its own ITPT lookup as well.
        killer.nextITPT = 0xff;
        reinterpret_cast<void (*)(Kart::Killer *)>(kmRuntimeAddr(0x8059c118))(&killer);
        return true;
    }
    Kart::Movement &movement = killer.GetMovement();
    Kart::Physics &physics = *killer.GetPhysicsHolder().physics;
    physics.mainRot = bulletRotation;
    physics.rotVec0.x = physics.rotVec0.y = physics.rotVec0.z = 0.0f;
    physics.rotVec1.x = physics.rotVec1.y = physics.rotVec1.z = 0.0f;
    physics.rotVec2.x = physics.rotVec2.y = physics.rotVec2.z = 0.0f;
    movement.dir = bulletDirection;
    movement.vel1Dir = bulletDirection;
    // Movement::calcAcceleration still provides native Bullet speed and floor
    // physics. No ITPT steering, stick steering, placement timeout or route
    // height correction runs; the bill lasts until it meets a wall/OOB boundary.
    return true;
}

} // namespace ScoreBased
} // namespace Pulsar
