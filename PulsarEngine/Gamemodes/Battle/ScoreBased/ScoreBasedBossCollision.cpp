#include <Gamemodes/Battle/ScoreBased/ScoreBased.hpp>
#include <MarioKartWii/Kart/KartPointers.hpp>

namespace Pulsar {
namespace ScoreBased {

// These are collision-local Mega eligibility bits, not kart status flags.
// Lightning sets bitfield2:0x80 until the shrink timer expires (80580778 /
// 80580998). Only the boss gains the extra attack against that shrunken rival.
static u32 GetBossCrushMask(const Kart::Link &first, const Kart::Link &second) {
    if (!IsBossBattle()) return 0;
    const u8 firstId = first.GetPlayerIdx();
    const u8 secondId = second.GetPlayerIdx();
    if (firstId >= 12 || secondId >= 12 || firstId == secondId ||
        IsEliminated(firstId) || IsEliminated(secondId)) return 0;
    const u8 boss = GetBossPlayerId();
    if (firstId == boss && (second.pointers->kartStatus->bitfield2 & 0x80)) return 1;
    if (secondId == boss && (first.pointers->kartStatus->bitfield2 & 0x80)) return 2;
    return 0;
}

// PAL CheckKartCollision, after hitbox intersection and both status snapshots.
// r24/r23 hold the two Mega predicates; r30+4/r31 are the two Kart::Links.
// The native Star/Bullet priority, crush immunity, Damage::SetDamage, and
// offline/online battle callbacks still run. No persistent Mega state is set.
// At 8056fdcc r3 is the live Racedata singleton address base; all other volatile
// GPRs, CR, and FPRs are dead. Preserve r3 and replay the displaced li r29,0.
static asmFunc AddBossCrushEligibility() {
    ASM(
        nofralloc;
        stwu r1, -0x20(r1);
        mflr r0;
        stw r0, 0x24(r1);
        stw r3, 0x18(r1);
        addi r3, r30, 4;
        mr r4, r31;
        bl GetBossCrushMask;
        rlwinm r0, r3, 0, 31, 31;
        or r24, r24, r0;
        rlwinm r0, r3, 31, 31, 31;
        or r23, r23, r0;
        lwz r3, 0x18(r1);
        lwz r0, 0x24(r1);
        mtlr r0;
        li r29, 0;
        addi r1, r1, 0x20;
        blr;
    );
}
kmCall(0x8056fdcc, AddBossCrushEligibility);

} // namespace ScoreBased
} // namespace Pulsar
