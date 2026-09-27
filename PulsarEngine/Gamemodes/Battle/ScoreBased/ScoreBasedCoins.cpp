#include <Gamemodes/Battle/ScoreBased/ScoreBased.hpp>
#include <runtimeWrite.hpp>

namespace Pulsar {
namespace ScoreBased {

// CoinManager owns a fixed pool: picking up or dropping a coin changes the
// owner/state of an existing entry rather than allocating another coin.
struct CoinPoolHeader {
    u8 count;       // 0x00
    u8 maximum;     // 0x01
    u8 initial;     // 0x02
};
static bool retainEliminatedCoins;

kmRuntimeUse(0x8087b4d0);
static void *ConstructCoinManager(void *manager) {
    manager = reinterpret_cast<void *(*)(void *)>(kmRuntimeAddr(0x8087b4d0))(manager);
    const bool singleCoin = IsSingleCoinBattle();
    retainEliminatedCoins = IsCoinBattle() && !singleCoin;
    if (IsCoinBattle()) {
        // Apply after the native minigame.kmg settings, before AddCoin allocates
        // objects or Init builds the ownership data used by RH2.
        CoinPoolHeader *pool = static_cast<CoinPoolHeader *>(manager);
        pool->maximum = singleCoin ? 1 : 12;
        if (singleCoin) pool->initial = 1;
        if (pool->initial > pool->maximum) pool->initial = pool->maximum;
    }
    return manager;
}
kmCall(0x8087b450, ConstructCoinManager);

// CoinManager::calc normally recycles coins owned by disconnected players.
// Score Based marks eliminated players disconnected but retains their scores:
// keep their pool entries occupied so those coins cannot also respawn on course.
// Only override this recycling check, preserving native disconnect handling and
// network acknowledgements elsewhere. The following instruction compares r0.
static asmFunc RetainEliminatedCoins() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        stw r12, 0x8(r1);
        lis r12, retainEliminatedCoins@ha;
        lbz r0, retainEliminatedCoins@l(r12);
        cmpwi r0, 0;
        li r0, 0;
        bne done;
        lbz r0, 0x1f5c(r3);
    done:
        lwz r12, 0x8(r1);
        addi r1, r1, 0x10;
        blr;
    )
}
kmCall(0x808821c4, RetainEliminatedCoins);

} // namespace ScoreBased
} // namespace Pulsar
