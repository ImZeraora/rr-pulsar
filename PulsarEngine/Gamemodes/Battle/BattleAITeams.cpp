#include <kamek.hpp>

namespace Pulsar {
namespace Battle {

// PAL AIBattleManager::createTeamInfo (80727f60) allocates six AI pointers
// followed by a count at +0x18. Boss Mode's 1-vs-11 roster overwrites that
// count and the next heap block. Keep the native contiguous list, but give
// each team twelve slots and move its count to +0x30 (allocation: 0x34).
// All consumers must share this layout, including ordinary Balloon/Coin
// battles. Their roster and AI behavior are unchanged; storage grows 48 bytes
// in total. Native initialization fills every slot below count, and the native
// destructor still owns both allocations.

kmWrite32(0x80727f80, 0x38600034); // red allocation
kmWrite32(0x80727fbc, 0x38600034); // blue allocation
kmWrite32(0x80727f98, 0x93430030); // red count initialization
kmWrite32(0x80727fcc, 0x93230030); // blue count initialization
kmWrite32(0x80727d20, 0x80a30030); // enemy count for red player
kmWrite32(0x80727d34, 0x80a30030); // enemy count for blue player
kmWrite32(0x80727dc8, 0x83a50030); // red local-player scan
kmWrite32(0x80727e08, 0x83830030); // blue local-player scan
kmWrite32(0x80727eac, 0x83a30030); // local-player count getter
kmWrite32(0x807281dc, 0x80180030); // per-player course setup loop
kmWrite32(0x807284e4, 0x83c30030); // Coin strategy team count
kmWrite32(0x807288b4, 0x83c30030); // Balloon red target capacity
kmWrite32(0x807288c4, 0x83a30030); // Balloon blue target capacity
kmWrite32(0x80728ad0, 0x83c30030); // red target-searcher rebuild
kmWrite32(0x80728ae0, 0x83a30030); // blue target-searcher rebuild
kmWrite32(0x80728fa4, 0x83830030); // Coin red target capacity
kmWrite32(0x80728fb4, 0x83a30030); // Coin blue target capacity
kmWrite32(0x80728cf0, 0x80650030); // target update rotation count

// Coin AI also uses the team count to fill a native six-entry permutation
// buffer. Give it twelve entries too; its factorial/permutation routines
// support 12! within 32 bits. Redirect both native users through their shared
// r31 pointer. Replace the following address additions too: their native
// immediates differ by region. Only r31 is live-written by the replaced lis.
static u32 teamPermutation[12];
static asmFunc SetTeamPermutationBase() {
    ASM(
        nofralloc;
        lis r31, teamPermutation @ha;
        addi r31, r31, teamPermutation @l;
        blr;);
}
kmCall(0x80728534, SetTeamPermutationBase);
kmWrite32(0x80728540, 0x7fe5fb78); // mr r5,r31: permutation output
kmWriteNop(0x80728548);          // r31 already points at the permutation

} // namespace Battle
} // namespace Pulsar
