#ifndef _PUL_CAUSE_AND_EFFECT_
#define _PUL_CAUSE_AND_EFFECT_

#include <kamek.hpp>
#include <Gamemodes/CauseAndEffect/Rules.hpp>

namespace Pulsar {
namespace CauseAndEffect {

bool IsEnabled();
bool ShouldShowDisplay();
void Reset();
void Update();
void OnItemBox(u8 playerId);
Pair GetPair(u8 playerId);
u32 GetLap(u8 playerId);
u32 GetRound(u8 playerId);
bool UsesTimedRounds();
float GetGravityFactor(u8 playerId);

}  // namespace CauseAndEffect
}  // namespace Pulsar

#endif
