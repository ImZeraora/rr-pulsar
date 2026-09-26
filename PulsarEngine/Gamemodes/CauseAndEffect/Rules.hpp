#ifndef _PUL_CAUSE_AND_EFFECT_RULES_
#define _PUL_CAUSE_AND_EFFECT_RULES_

// Kept independent of the game ABI so the shared round schedule can be tested on a host.
namespace Pulsar {
namespace CauseAndEffect {

enum Cause {
    CAUSE_ITEM_BOX,
    CAUSE_WHEELIE,
    CAUSE_DRIFT,
    CAUSE_HIT_PLAYER,
    CAUSE_GET_HIT,
    CAUSE_BOOST,
    CAUSE_BUMP_PLAYER,
    CAUSE_TRICK,
    CAUSE_POSITION_CHANGE,
    CAUSE_USE_ITEM,
    CAUSE_ENTER_CANNON,
    CAUSE_COUNT
};

enum Effect {
    EFFECT_ITEM,
    EFFECT_SPEED_UP,
    EFFECT_SPEED_DOWN,
    EFFECT_LIGHTNING,
    EFFECT_MEGA,
    EFFECT_BLOOPER,
    EFFECT_BOMB,
    EFFECT_MUSHROOM_BOOST,
    EFFECT_CLEAR_ITEM,
    EFFECT_TURN_AROUND,
    EFFECT_STAR,
    EFFECT_LOW_GRAVITY,
    EFFECT_HIGH_GRAVITY,
    EFFECT_COUNT
};

struct Pair {
    Cause cause;
    Effect effect;
};

inline unsigned int Mix(unsigned int value) {
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    return value ^ (value >> 16);
}

inline bool IsUnsafePair(const Pair &pair) {
    if (pair.cause == CAUSE_POSITION_CHANGE && pair.effect == EFFECT_TURN_AROUND) return true;
    if (pair.cause == CAUSE_GET_HIT && pair.effect == EFFECT_STAR) return true;
    if (pair.cause != CAUSE_ENTER_CANNON) return false;

    return pair.effect == EFFECT_BOMB || pair.effect == EFFECT_MUSHROOM_BOOST || pair.effect == EFFECT_LIGHTNING ||
           pair.effect == EFFECT_TURN_AROUND;
}

inline void ExcludeUnsafePair(Pair &pair, unsigned int entropy) {
    if (!IsUnsafePair(pair)) return;

    // Search every effect from a deterministic starting point so a Cannon
    // pair cannot roll into another cannon-incompatible effect.
    const unsigned int start = Mix(entropy) % EFFECT_COUNT;
    for (unsigned int offset = 0; offset < EFFECT_COUNT; ++offset) {
        pair.effect = static_cast<Effect>((start + offset) % EFFECT_COUNT);
        if (!IsUnsafePair(pair)) return;
    }
}

// No shared RNG is advanced: every player in round N gets the same pair.
// A round is normally a lap, but can be a fixed time interval on short tracks.
inline Pair PairForRound(unsigned int seed, unsigned int round, bool hasCannon) {
    // Cannon is appended to the enum, so cannon-free tracks retain the same
    // schedule as before while never selecting the unavailable cause.
    const unsigned int causeCount = hasCannon ? CAUSE_COUNT : CAUSE_ENTER_CANNON;
    Pair pair = {static_cast<Cause>(Mix(seed) % causeCount),
                 static_cast<Effect>(Mix(seed ^ 0xa511e9b3U) % EFFECT_COUNT)};
    ExcludeUnsafePair(pair, seed ^ 0x7f4a7c15U);
    for (unsigned int i = 2; i <= round; ++i) {
        const unsigned int value = Mix(seed + i * 0x9e3779b9U);
        pair.cause = static_cast<Cause>((pair.cause + 1 + value % (causeCount - 1)) % causeCount);
        pair.effect = static_cast<Effect>((pair.effect + 1 + Mix(value) % (EFFECT_COUNT - 1)) % EFFECT_COUNT);
        ExcludeUnsafePair(pair, value ^ i);
    }
    return pair;
}

struct TriggerState {
    unsigned int round;
    unsigned int observed;
    unsigned int pending;
    unsigned int pendingRound;

    void Reset() { round = observed = pending = pendingRound = 0; }

    bool EnterRound(unsigned int nextRound, unsigned int activeCauses) {
        if (round == nextRound) return false;
        round = nextRound;
        observed = activeCauses;
        if (pendingRound != round) pending = 0;
        return true;
    }

    void Queue(Cause cause, unsigned int eventRound) {
        if (pendingRound != eventRound) pending = 0;
        pendingRound = eventRound;
        pending |= 1U << cause;
    }

    bool Consume(Cause cause, unsigned int activeCauses) {
        const unsigned int events = (activeCauses & ~observed) | (pendingRound == round ? pending : 0);
        observed = activeCauses;
        pending = 0;
        return (events & (1U << cause)) != 0;
    }
};

}  // namespace CauseAndEffect
}  // namespace Pulsar

#endif
