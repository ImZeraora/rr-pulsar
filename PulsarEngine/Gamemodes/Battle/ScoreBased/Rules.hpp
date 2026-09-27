#ifndef _PUL_SCOREBASED_RULES_
#define _PUL_SCOREBASED_RULES_

namespace Pulsar {
namespace ScoreBased {
// Round up fractional points; lock the target to the starting roster.
inline unsigned ScoreLimitForPlayers(unsigned players) {
    if (players > 12) players = 12;
    if (players == 0) players = 1;
    return (75 * players + 11) / 12;
}

// The battle timer starts at 3:00 for two players and gains 42 seconds per
// additional starting player, reaching 10:00 at twelve players.
inline unsigned BalloonCountdownMilliseconds(unsigned players) {
    if (players < 2) players = 2;
    if (players > 12) players = 12;
    return 180000 + (players - 2) * 42000;
}

// SELECT supplies the same random race seed to every peer before course objects
// are constructed. Fold its bits without consuming the native race RNG.
inline bool SingleCoinFromSeed(unsigned seed) {
    seed ^= seed >> 16;
    seed ^= seed >> 8;
    seed ^= seed >> 4;
    seed ^= seed >> 2;
    seed ^= seed >> 1;
    return (seed & 1) != 0;
}

inline unsigned BossRandom(unsigned seed) {
    seed ^= seed >> 16;
    seed *= 0x7feb352dU;
    seed ^= seed >> 15;
    seed *= 0x846ca68bU;
    return seed ^ (seed >> 16);
}
inline unsigned BossPlayerFromSeed(unsigned seed, unsigned players) {
    if (players == 0 || players > 12) return 0xff;
    return BossRandom(seed ^ 0xb055b055U) % players;
}
inline unsigned BossItemDelay(unsigned seed, unsigned sequence) {
    return 10000 + BossRandom(seed ^ (sequence * 0x9e3779b9U)) % 5001;
}
inline unsigned BossItemIndex(unsigned seed, unsigned sequence) {
    return BossRandom(seed ^ (sequence * 0x85ebca6bU) ^ 0x17eaf00dU) % 4;
}
inline bool BossRoundFinished(unsigned eliminated, unsigned boss, unsigned players) {
    if (players < 2 || players > 12 || boss >= players) return false;
    const unsigned bossBit = 1u << boss;
    const unsigned fighters = ((1u << players) - 1) & ~bossBit;
    return (eliminated & bossBit) || (eliminated & fighters) == fighters;
}
inline unsigned BossWinner(unsigned eliminated, unsigned boss, unsigned players) {
    if (!BossRoundFinished(eliminated, boss, players)) return 2;
    if (eliminated == (1u << players) - 1) return 2;
    return (eliminated & (1u << boss)) ? 1 : 0;
}

inline unsigned CountdownAfter(unsigned remaining, unsigned elapsed) {
    return elapsed >= remaining ? 0 : remaining - elapsed;
}

// Only the current owner spends their saved time. The shared unowned budget
// is paused during ownership and resumes on every drop, including disconnects.
inline bool AdvanceCoinTimers(unsigned *owned, unsigned &unowned, unsigned owner,
                              unsigned players, unsigned elapsed) {
    if (owner < players && owner < 12) {
        owned[owner] = CountdownAfter(owned[owner], elapsed);
        return owned[owner] == 0;
    }
    unowned = CountdownAfter(unowned, elapsed);
    return unowned == 0;
}

inline unsigned CoinCutoffMilliseconds(unsigned completedRounds) {
    return (completedRounds + 1) * 30000;
}
inline unsigned CompletedRounds(unsigned elapsedMs) { return elapsedMs / 30000; }
inline bool BelowCutoff(unsigned coins, unsigned completedRounds) {
    return completedRounds != 0 && coins < completedRounds;
}
inline unsigned EliminationMask(unsigned previous, const unsigned short *scores,
                                unsigned players, unsigned completedRounds) {
    if (players > 12) players = 12;
    for (unsigned id = 0; id < players; ++id)
        if (BelowCutoff(scores[id], completedRounds)) previous |= 1u << id;
    return previous;
}
inline unsigned Survivors(unsigned eliminated, unsigned players) {
    unsigned count = 0;
    for (unsigned id = 0; id < players && id < 12; ++id)
        if (!(eliminated & (1u << id))) ++count;
    return count;
}
} // namespace ScoreBased
} // namespace Pulsar
#endif
