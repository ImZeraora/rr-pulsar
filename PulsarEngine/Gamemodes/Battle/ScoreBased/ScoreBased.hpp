#ifndef _PUL_BATTLE_SCOREBASED_
#define _PUL_BATTLE_SCOREBASED_
#include <kamek.hpp>

namespace Pulsar {
namespace Network { struct PulRH1; }
namespace ScoreBased {

// Cumulative host state: losing an RH1 packet cannot lose an elimination.
#pragma pack(push, 1)
struct SyncState {
    u32 elapsedMs;
    u16 round;
    u16 eliminated;
    u16 eliminationRound[12];
    u16 scores[12];
    u8 playerCount;
    u8 finished;
    u8 singleCoin;
    u8 coinOwner;  // 0xff while unowned
    u32 ownedRemainingMs[12];
    u32 unownedRemainingMs;
};
#pragma pack(pop)

bool IsActive();
bool IsCoinBattle();
bool IsSingleCoinBattle();
u32 GetCoinTimerMilliseconds();
bool IsEliminated(u8 playerId);
u16 GetScoreLimit();
u16 GetCutoff();
u32 GetSecondsUntilCutoff();
u8 GetSurvivorCount();
bool HasFinished();
void UpdateSpectatorCameras();
void WriteRH1Packet(Network::PulRH1 &packet);
const wchar_t *GetSettingText(s32 bmgId);

} // namespace ScoreBased
} // namespace Pulsar
#endif
