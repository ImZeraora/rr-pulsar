#include <Gamemodes/Battle/ScoreBased/ScoreBasedResults.hpp>
#include <Gamemodes/Battle/ScoreBased/ScoreBased.hpp>
#include <MarioKartWii/UI/Page/Leaderboard/TeamLeaderboard.hpp>
#include <MarioKartWii/UI/Ctrl/CtrlRace/CtrlRaceBattleSetPoint.hpp>
#include <MarioKartWii/Race/RaceInfo/RaceInfo.hpp>
#include <MarioKartWii/GlobalFunctions.hpp>
#include <CustomCharacters/CustomCharacters.hpp>

typedef char BattleUpdateLayoutCheck[sizeof(Pages::BattleLeaderboardUpdate) == 0x18c4 ? 1 : -1];
typedef char BattleTotalLayoutCheck[sizeof(Pages::BattleLeaderboardTotal) == 0x18c4 ? 1 : -1];

namespace Pulsar {
namespace ScoreBased {

// PAL 807f69e4 writes all members into CtrlRaceResultTeam::players[6].
// Entry 6 overwrites team at +1ec and the first row's vtable at +1f0.
// Keep the native page prefix for win accounting/navigation, but never load or
// activate those embedded six-row controls in Boss Mode.
class BossResultTeam : public LayoutUIControl {
public:
    struct Player { u32 score; u32 previousScore; u8 id; };
    BossResultTeam() : rowCount(0), pointCount(0), setPoints(nullptr), teamPoint(nullptr) {}
    ~BossResultTeam() override {
        delete teamPoint;
        for (u32 i = 0; i < pointCount; ++i) delete setPoints[i];
        delete[] setPoints;
    }
    void Load(Team team);
    void InitSelf() override;
    const char *GetClassName() const override { return "BossResultTeam"; }
private:
    Team team;
    u32 rowCount;
    Player players[12];
    LayoutUIControl rows[12];
    u32 pointCount;
    CtrlRaceBattleSetPoint **setPoints;
    LayoutUIControl *teamPoint;
};

void BossResultTeam::Load(Team team) {
    this->team = team;
    const RacedataScenario &race = Racedata::sInstance->racesScenario;
    for (u32 id = 0; id < race.playerCount && id < 12; ++id)
        if (race.players[id].team == team) ++rowCount;
    const bool total = parentGroup->parentPage->pageId == PAGE_BATTLE_TOTAL_LEADERBOARDS;
    // Native ResultTeam::Load reads the configured match count at SectionParams+64.
    pointCount = total ? SectionMgr::sInstance->sectionParams->vsRaceCount : 0;
    setPoints = nullptr;
    char variant[24];
    snprintf(variant, sizeof(variant), "ResultVSTeamNULL%d", team);
    ControlLoader loader(this);
    loader.Load("result", total ? "ResultVSTeamNULL" : "ResultBTTeamNULL", variant, nullptr);
    InitControlGroup(rowCount + (total ? pointCount : 1));
    const char *anims[] = {"Loop", "Loop", nullptr, "Select", "SelectOn", "SelectOff", nullptr,
                          "Select2", "Select2On", "Select2Off", nullptr, nullptr};
    for (u32 i = 0; i < rowCount; ++i) {
        AddControl(i, &rows[i]);
        snprintf(variant, sizeof(variant), "BlueRed%d", i % 6);
        ControlLoader rowLoader(&rows[i]);
        rowLoader.Load("result", "ResultVSTeam", variant, anims);
        rows[i].SetPaneVisibility(team == TEAM_RED ? "blue_null" : "red_null", false);
        rows[i].animator.GetAnimationGroupById(0).PlayAnimationAtFrame(0, 0.0f);
        rows[i].animator.GetAnimationGroupById(1).PlayAnimationAtFrame(1, 0.0f);
        rows[i].animator.GetAnimationGroupById(2).PlayAnimationAtFrame(1, 0.0f);
        rows[i].SetSoundIds(0xe1, 0xe0);
    }
    if (rowCount > 6) {
        // Fit every survivor in the original column, including entrance/exit
        // positions. Reuse only the six BRCTR variants that actually exist.
        const float factor = 6.0f / rowCount;
        for (u32 state = 0; state < 4; ++state) {
            const float top = rows[0].positionAndscale[state].position.y;
            const float step = rows[1].positionAndscale[state].position.y - top;
            for (u32 i = 0; i < rowCount; ++i) {
                rows[i].positionAndscale[state].position.y = top + step * i * factor;
                rows[i].positionAndscale[state].scale.z *= factor;
            }
        }
    }
    if (total) {
        setPoints = new CtrlRaceBattleSetPoint *[pointCount];
        for (u32 i = 0; i < pointCount; ++i) {
            setPoints[i] = new CtrlRaceBattleSetPoint;
            AddControl(rowCount + i, setPoints[i]);
            setPoints[i]->Load(team, i);
        }
    } else {
        teamPoint = new LayoutUIControl;
        AddControl(rowCount, teamPoint);
        const char *pointAnims[] = {"team", "blue", "red", nullptr, nullptr};
        ControlLoader pointLoader(teamPoint);
        pointLoader.Load("result", "ResultTeamPoint", team == TEAM_RED ? "red" : "blue", pointAnims);
        teamPoint->animator.GetAnimationGroupById(0).PlayAnimationAtFrame(team == TEAM_RED ? 1 : 0, 0.0f);
        teamPoint->animator.GetAnimationGroupById(0).isActive = false;
    }
}

void BossResultTeam::InitSelf() {
    const RacedataScenario &race = Racedata::sInstance->racesScenario;
    RacedataScenario &menu = Racedata::sInstance->menusScenario;
    const bool total = parentGroup->parentPage->pageId == PAGE_BATTLE_TOTAL_LEADERBOARDS;
    u32 count = 0;
    u32 teamScore = 0;
    for (u32 id = 0; id < race.playerCount && id < 12 && count < rowCount; ++id) {
        if (race.players[id].team != team) continue;
        Player &entry = players[count++];
        entry.id = id;
        entry.score = total ? menu.players[id].score : Raceinfo::sInstance->players[id]->battleScore;
        entry.previousScore = race.players[id].previousScore;
        teamScore += entry.score;
    }
    // Native ordering: score, local humans first, previous score, player ID.
    for (u32 i = 1; i < count; ++i) {
        const Player entry = players[i];
        u32 j = i;
        while (j > 0) {
            const Player &prev = players[j - 1];
            const bool local = race.players[entry.id].playerType == PLAYER_REAL_LOCAL;
            const bool prevLocal = race.players[prev.id].playerType == PLAYER_REAL_LOCAL;
            const bool before = entry.score != prev.score ? entry.score > prev.score :
                local != prevLocal ? local : entry.previousScore != prev.previousScore ?
                entry.previousScore > prev.previousScore : entry.id < prev.id;
            if (!before) break;
            players[j] = prev;
            --j;
        }
        players[j] = entry;
    }
    MiiGroup &miis = SectionMgr::sInstance->sectionParams->playerMiis;
    const bool online = race.settings.gamemode >= MODE_PRIVATE_VS && race.settings.gamemode <= MODE_PRIVATE_BATTLE;
    for (u32 i = 0; i < rowCount; ++i) {
        LayoutUIControl &row = rows[i];
        row.isHidden = i >= count;
        if (row.isHidden) continue;
        const u8 id = players[i].id;
        const CharacterId character = race.players[id].characterId;
        const PlayerType type = race.players[id].playerType;
        menu.players[id].gpRank = i + 1;
        for (u32 group = 1; group < 3; ++group)
            row.animator.GetAnimationGroupById(group).PlayAnimationAtFrame(type == PLAYER_REAL_LOCAL ? 0 : 1, 0.0f);
        Text::Info info;
        if (character >= MII_S_A_MALE || ((online || race.localPlayerCount > 1) && type != PLAYER_CPU)) {
            info.miis[0] = miis.GetMii(id);
            row.SetTextBoxMessage("mii_name", 0x251d, &info);
        } else if (!CustomCharacters::SetRaceNameTextIfCustom(row, "mii_name", id)) {
            row.SetTextBoxMessage("mii_name", GetCharacterBMGId(character, true));
        }
        if (online || character >= MII_S_A_MALE) {
            row.SetMiiPane("chara_icon", miis, id, 2);
            row.SetMiiPane("chara_icon_sha", miis, id, 2);
        } else {
            row.SetPicturePane("chara_icon", GetCharacterIconPaneName(character));
            row.SetPicturePane("chara_icon_sha", GetCharacterIconPaneName(character));
        }
        info.intToPass[0] = players[i].score;
        row.SetTextBoxMessage("point", 0x522, &info);
        row.SetTextBoxMessage("pts", 0x521, &info);
    }
    if (teamPoint) {
        Text::Info info;
        info.intToPass[0] = teamScore;
        teamPoint->SetTextBoxMessage("point", team == TEAM_RED ? 0x76c : 0x76d, &info);
    }
}

template <class NativePage>
class BossLeaderboard : public NativePage {
public:
    BossLeaderboard() { this->ctrlRaceCount = nullptr; }
    ~BossLeaderboard() override { delete this->ctrlRaceCount; }
    void OnInit() override {
        this->ctrlRaceCount = nullptr;
        const bool update = this->pageId == PAGE_BATTLE_LEADERBOARDS_UPDATE;
        this->InitControlGroup(update ? 3 : 2);
        for (u32 team = 0; team < 2; ++team) {
            this->AddControl(team, bossResults[team], 0);
            bossResults[team].Load(static_cast<Team>(team));
        }
        if (update) {
            this->ctrlRaceCount = new CtrlRaceCount;
            this->AddControl(2, *this->ctrlRaceCount, 0);
            this->ctrlRaceCount->Load("BattleWin", "BattleWin");
            this->ctrlRaceCount->ResetMsg();
        }
        Pages::TeamLeaderboardBase::hasTeamScoreDiffBeenCalcd = false;
        Pages::TeamLeaderboardBase::teamScoreDiff = 0;
    }
    // Despite its name, the native CanEnd slot returns true while busy. Boss
    // scores are shown immediately, so no six-row native animation scan runs.
    bool CanEnd() override { return false; }
private:
    BossResultTeam bossResults[2];
};

Page *CreateBossResultsPage(PageId id) {
    if (!IsBossBattle()) return nullptr;
    if (id == PAGE_BATTLE_LEADERBOARDS_UPDATE) return new BossLeaderboard<Pages::BattleLeaderboardUpdate>;
    if (id == PAGE_BATTLE_TOTAL_LEADERBOARDS) return new BossLeaderboard<Pages::BattleLeaderboardTotal>;
    return nullptr;
}

} // namespace ScoreBased
} // namespace Pulsar
