#include <Gamemodes/Battle/ScoreBased/ScoreBased.hpp>
#include <UI/CtrlRaceBase/CustomCtrlRaceBase.hpp>
#include <MarioKartWii/UI/Ctrl/CtrlRace/CtrlRaceWifi.hpp>
#include <MarioKartWii/Race/RaceInfo/RaceInfo.hpp>

namespace Pulsar {
namespace ScoreBased {

class RuleDisplay : public CtrlRaceWifiStartMessage {
public:
    static u32 Count() { return IsActive() ? 1 : 0; }
    static void Create(Page &page, u32 index, u32) {
        RuleDisplay *control = new RuleDisplay;
        page.AddControl(index, *control, 0);
        control->hudSlotId = 0;
        ControlLoader loader(control);
        loader.Load("game_image", "CTInfo", "CTInfo", nullptr);
        control->textBox_00 = control->layout.GetPaneByName("TextBox_00");
    }

    bool IsInactive() override { return !IsActive(); }
    bool HasStarted() override { return true; }

    void OnUpdate() override {
        if (!IsActive()) return;
        if (IsBossBattle()) {
            if (HasFinished()) {
                const u8 winner = GetBossWinnerTeam();
                swprintf(text, 160, winner == 0 ? L"Boss wins!" : winner == 1 ? L"Survivors win!" : L"Draw!");
            } else {
                const u8 boss = GetBossPlayerId();
                const u8 survivors = GetSurvivorCount() - (IsEliminated(boss) ? 0 : 1);
                swprintf(text, 160, L"Boss Mode | Boss: %u balloons | %u survivors", GetBossBalloons(boss), survivors);
            }
        } else if (IsSingleCoinBattle()) {
            swprintf(text, 160, HasFinished() ? L"Round over!" : L"Single coin | Hold the coin for 20 seconds");
        } else if (IsCoinBattle()) {
            if (HasFinished()) {
                swprintf(text, 160, GetSurvivorCount() ? L"Last player standing!" : L"Everyone eliminated - draw!");
            } else {
                bool allLocalOut = Racedata::sInstance->racesScenario.localPlayerCount != 0;
                for (u8 local = 0; local < Racedata::sInstance->racesScenario.localPlayerCount; ++local) {
                    const u8 id = Racedata::sInstance->GetPlayerIdOfLocalPlayer(local);
                    if (!IsEliminated(id)) allLocalOut = false;
                }
                swprintf(text, 160, L"%ls%u remaining | %u coins or fewer at the next cutoff",
                         allLocalOut ? L"Eliminated | " : L"", GetSurvivorCount(), GetCutoff());
            }
        } else {
            swprintf(text, 160, L"First team to %u points", GetScoreLimit());
        }
        Text::Info info;
        info.strings[0] = text;
        this->SetMessage(UI::BMG_TEXT, &info);
        CtrlRaceWifiStartMessage::OnUpdate();
    }

private:
    wchar_t text[160];
};
static UI::CustomCtrlBuilder ruleDisplay(RuleDisplay::Count, RuleDisplay::Create);

} // namespace ScoreBased
} // namespace Pulsar
