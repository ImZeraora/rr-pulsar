#include <Gamemodes/Battle/ScoreBased/ScoreBased.hpp>
#include <Gamemodes/Battle/ScoreBased/Rules.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/Race/RaceBalloon.hpp>
#include <MarioKartWii/Item/ItemManager.hpp>
#include <MarioKartWii/RKNet/ITEM.hpp>
#include <MarioKartWii/Kart/KartMovement.hpp>
#include <runtimeWrite.hpp>

namespace Pulsar {
namespace ScoreBased {

bool IsBossBattle() {
    if (!IsActive()) return false;
    const Racedata &data = *Racedata::sInstance;
    // SetFFAmode runs after the roster copy but just before the race seed copy.
    // The initialized menu seed is already shared and remains fixed for the race.
    return data.menusScenario.settings.battleType == BATTLE_BALLOON &&
           data.racesScenario.playerCount >= 2 && data.racesScenario.playerCount <= 12 &&
           SingleCoinFromSeed(data.menusScenario.settings.selectId);
}

u8 GetBossPlayerId() {
    if (!IsBossBattle()) return 0xff;
    const Racedata &data = *Racedata::sInstance;
    return BossPlayerFromSeed(data.menusScenario.settings.selectId, data.racesScenario.playerCount);
}

bool IsBoss(u8 playerId) { return playerId < 12 && playerId == GetBossPlayerId(); }

void ConfigureBossTeams() {
    if (!IsBossBattle()) return;
    RacedataScenario &scenario = Racedata::sInstance->racesScenario;
    const u8 boss = GetBossPlayerId();
    scenario.settings.modeFlags |= 2;
    for (u8 id = 0; id < scenario.playerCount; ++id)
        scenario.players[id].team = id == boss ? TEAM_RED : TEAM_BLUE;
}

u8 GetBossStartingBalloons(u8 playerId) { return IsBoss(playerId) ? 5 : 1; }

// Both native blink starters have already enabled the battle immunity bit
// and initialized Movement+0x1a8 when they call getKartBlink. Extend only the
// boss's timer, then return the original blink object for native animation.
// calcBlink (80581824) decrements once per simulation frame and clears immunity
// at zero. Do not change BLINK_DURATION globally: it also times defeat/respawn.
kmRuntimeUse(0x8059108c);
static void *GetBossBattleBlink(Kart::Movement *movement) {
    if (IsBoss(movement->GetPlayerIdx())) {
        s16 &blinkTimer = *reinterpret_cast<s16 *>(reinterpret_cast<u8 *>(movement) + 0x1a8);
        blinkTimer = 5 * 60;
    }
    return reinterpret_cast<void *(*)(Kart::Movement *)>(kmRuntimeAddr(0x8059108c))(movement);
}
kmCall(0x8058180c, GetBossBattleBlink); // startBlink: battle/remote balloon events
kmCall(0x80581a0c, GetBossBattleBlink); // startBlinkLocal: damage recovery

u8 ReadBossBalloonCount(u8 playerId) {
    const RaceBalloonManager *mgr = RaceBalloonManager::sInstance;
    if (!mgr || playerId >= Racedata::sInstance->racesScenario.playerCount || playerId >= 12) return 0;
    // PAL RaceBalloonManager::players[12], each entry is 0x18 bytes.
    return reinterpret_cast<const u8 *>(mgr)[0x3c4 + playerId * 0x18];
}

// These three Add calls refill all balloons after a defeat, including the
// online respawn event. Initial allocation remains in BattleRoyale's existing
// 80869ba8 wrapper; ordinary native damage and Mushroom theft are unchanged.
static void RefillBalloons(RaceBalloonManager *mgr, int playerId, u32 team, u32 initial,
                          int delay, u32 count, int interval) {
    if (!IsBossBattle()) mgr->Add(playerId, team, initial, delay, count, interval);
}
kmCall(0x80538e98, RefillBalloons);
kmCall(0x8053d244, RefillBalloons);
kmCall(0x8053be14, RefillBalloons);

static void RestoreRemoteBalloons(RaceBalloonManager *mgr, int playerId, u32 team, u32 initial,
                                 int delay, u32 count, int interval) {
    if (IsBossBattle() && IsEliminated(playerId)) return;
    mgr->Add(playerId, team, initial, delay, count, interval);
}
kmCall(0x8053c2a0, RestoreRemoteBalloons);
kmCall(0x8053c2c4, RestoreRemoteBalloons);

static u16 appliedItemSequence;
void ResetBossItems() { appliedItemSequence = 0; }

kmRuntimeUse(0x807ba2d8); // native roulette reset, including ITEM notification
kmRuntimeUse(0x8065cfc4); // ITEMHandler::SetItemAndMode
void ApplyBossItem(u16 sequence, u8 item) {
    if (!sequence || sequence == appliedItemSequence || !IsBossBattle()) return;
    const u8 boss = GetBossPlayerId();
    Item::Manager *mgr = Item::Manager::sInstance;
    if (!mgr || boss >= mgr->playerCount || IsEliminated(boss)) return;
    Item::Player &player = mgr->players[boss];
    if (player.isRemote) return; // the owning console supplies inventory and item-use events
    if (item != STAR && item != MEGA_MUSHROOM && item != LIGHTNING && item != BULLET_BILL) return;
    const RacedataScenario &scenario = Racedata::sInstance->racesScenario;
    const bool online = scenario.settings.gamemode == MODE_PRIVATE_BATTLE;
    const bool localHuman = scenario.players[boss].playerType == PLAYER_REAL_LOCAL;
    // The online reset indexes a local-human ITEM slot. CPU roulette cannot
    // spin here (boxes are disabled), so it needs no such notification.
    if (!online || localHuman)
        reinterpret_cast<void (*)(Item::PlayerRoulette *)>(kmRuntimeAddr(0x807ba2d8))(&player.roulette);
    // All four grants are single use, untethered items. Replace an unused grant
    // when the next interval arrives without ending active Star/Mega/Bullet effects.
    player.inventory.ClearAll();
    player.inventory.SetItem(static_cast<ItemId>(item), false);
    if (online && localHuman && RKNet::ITEMHandler::sInstance) {
        // Native roulette completion advertises held-state 3 before normal item
        // updates advance it to the one-item-left state (8065c7e0 / 8065e938).
        reinterpret_cast<void (*)(RKNet::ITEMHandler *, u8, u8, ItemId)>(kmRuntimeAddr(0x8065cfc4))(
            RKNet::ITEMHandler::sInstance, boss, 3, static_cast<ItemId>(item));
    }
    appliedItemSequence = sequence;
}

kmRuntimeUse(0x8058160c);
static void UpdateBossScale(Kart::Movement *movement) {
    reinterpret_cast<void (*)(Kart::Movement *)>(kmRuntimeAddr(0x8058160c))(movement);
    if (!IsBoss(movement->GetPlayerIdx())) return;
    // Multiply the freshly computed scale, preserving shrink/growth and vanish
    // animations without compounding the multiplier from one frame to the next.
    movement->scale.x *= 1.5f;
    movement->scale.y *= 1.5f;
    movement->scale.z *= 1.5f;
    movement->unknown_0x170 *= 1.5f; // hitboxScale
    movement->someScale *= 1.5f; // totalScale at 0x174
}
kmCall(0x80578de4, UpdateBossScale);
kmCall(0x80579890, UpdateBossScale);

} // namespace ScoreBased
} // namespace Pulsar
