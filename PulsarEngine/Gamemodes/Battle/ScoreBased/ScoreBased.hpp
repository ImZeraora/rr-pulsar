#ifndef _PUL_BATTLE_SCOREBASED_
#define _PUL_BATTLE_SCOREBASED_
#include <kamek.hpp>

namespace Kart { class Killer; }

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
    u8 bossMode;
    u8 bossId;
    u8 balloons[12];
    u16 bossItemSequence;
    u8 bossItem;
    u8 winnerTeam;  // 0 red/boss, 1 blue/survivors, 2 draw
    u32 nextBossItemMs;
};
#pragma pack(pop)

bool IsActive();
bool IsCoinBattle();
bool IsSingleCoinBattle();
bool IsBossBattle();
u8 GetBossPlayerId();
bool IsBoss(u8 playerId);
void ConfigureBossTeams();
u8 GetBossStartingBalloons(u8 playerId);
u8 GetBossBalloons(u8 playerId);
u8 GetBossWinnerTeam();
u8 ReadBossBalloonCount(u8 playerId);
void ResetBossItems();
void ApplyBossItem(u16 sequence, u8 item);
bool UpdateBossBullet(Kart::Killer &killer);
u32 GetCoinTimerMilliseconds();
u32 GetBossElapsedMilliseconds();
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
