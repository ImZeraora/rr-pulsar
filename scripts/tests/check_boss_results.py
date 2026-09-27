"""Exercise the actual Boss results source with native UI/game calls stubbed.

ASan/UBSan check both pages, all boss slots for 2-12 players, complete control
registration, scores, ordering, repeat activation, and control destruction.
This does not render BRCTR assets or run Dolphin. Run from the repository root.
"""
from pathlib import Path
import subprocess
import tempfile
import struct

# PAL InitSelf: the seventh matching player's stores at 807f6a84/6b34/6b4c
# use this + 6*0x14. In update results the animated score is float zero.
control = bytearray(0xb90)
struct.pack_into('>I', control, 0x1f0, 0x808befb4)
for index in range(7):
    struct.pack_into('>I', control, 0x180 + index * 0x14, index)
    struct.pack_into('>f', control, 0x178 + index * 0x14, 0.0)
    struct.pack_into('>I', control, 0x174 + index * 0x14, 1)
assert struct.unpack_from('>I', control, 0x1f0)[0] == 0
assert 0x81430094 - 0x68 + 0x1f0 == 0x8143021c  # reported child address
print('Reproduced the native seventh-entry overwrite at the reported row offset.', flush=True)

STUBS = r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>
using u8=uint8_t; using u32=uint32_t;
enum Team { TEAM_RED, TEAM_BLUE };
enum PageId { PAGE_BATTLE_LEADERBOARDS_UPDATE=0x33, PAGE_BATTLE_TOTAL_LEADERBOARDS=0x34 };
enum CharacterId { MII_S_A_MALE=24 };
enum PlayerType { PLAYER_REAL_LOCAL, PLAYER_CPU, PLAYER_REMOTE };
const int MODE_PRIVATE_VS=7, MODE_PRIVATE_BATTLE=10;
struct Mii { unsigned id; };
struct MiiGroup { Mii data[12]; Mii *GetMii(u8 id) { assert(id<12); data[id].id=id; return &data[id]; } };
namespace Text { struct Info { int intToPass[1]{}; Mii *miis[1]{}; }; }
struct RacePlayer { Team team; unsigned score,previousScore,gpRank; CharacterId characterId; PlayerType playerType; };
struct RacedataScenario { unsigned playerCount, localPlayerCount; struct { int gamemode; } settings; RacePlayer players[12]; };
struct Racedata { static Racedata *sInstance; RacedataScenario racesScenario,menusScenario; } data;
Racedata *Racedata::sInstance=&data;
struct Raceinfo { static Raceinfo *sInstance; struct Player { unsigned battleScore; } *players[12]; } raceinfo;
Raceinfo *Raceinfo::sInstance=&raceinfo;
struct SectionParams { MiiGroup playerMiis; unsigned vsRaceCount; } params;
struct SectionMgr { static SectionMgr *sInstance; SectionParams *sectionParams; } sections{&params};
SectionMgr *SectionMgr::sInstance=&sections;
struct Page;
struct ControlGroup { Page *parentPage=nullptr; std::vector<class UIControl*> controls; };
struct PositionAndScale { struct { float x=0,y=0,z=0; } position; struct { float x=1,z=1; } scale; };
struct UIControl {
    static int live;
    UIControl() { ++live; }
    virtual ~UIControl() { --live; }
    virtual void InitSelf() {}
    virtual const char *GetClassName() const { return "stub"; }
    ControlGroup *parentGroup=nullptr, childrenGroup;
    PositionAndScale positionAndscale[4]; bool isHidden=false;
    void InitControlGroup(u32 n) { childrenGroup.controls.resize(n); childrenGroup.parentPage=parentGroup->parentPage; }
    void AddControl(u32 i,UIControl *ctrl) { assert(i<childrenGroup.controls.size()); assert(!childrenGroup.controls[i]); childrenGroup.controls[i]=ctrl; ctrl->parentGroup=&childrenGroup; }
};
int UIControl::live=0;
struct Animation { bool isActive=true; void PlayAnimationAtFrame(unsigned,float) {} };
struct Animator { Animation groups[3]; Animation &GetAnimationGroupById(unsigned i) { assert(i<3); return groups[i]; } };
struct LayoutUIControl : UIControl {
    Animator animator; int score=-1,player=-1; bool red=false;
    void SetPaneVisibility(const char *pane,bool visible) { assert(!visible); red=!strcmp(pane,"blue_null"); }
    void SetSoundIds(unsigned,unsigned) {}
    void ResetMsg() {}
    void SetTextBoxMessage(const char *pane,unsigned bmg,const Text::Info *info=nullptr) {
        if (bmg==0x251d) { assert(info&&info->miis[0]); player=info->miis[0]->id; }
        if (!strcmp(pane,"point")) { assert(info); score=info->intToPass[0]; }
    }
    void SetMiiPane(const char*,MiiGroup&,unsigned id,unsigned) { assert(id<12); }
    void SetPicturePane(const char*,const char*) {}
};
struct ControlLoader {
    LayoutUIControl *ctrl; ControlLoader(LayoutUIControl *ctrl):ctrl(ctrl) {}
    void Load(const char*,const char *name,const char *variant,const char**) {
        if (!strcmp(name,"ResultVSTeam")) {
            unsigned row=99; assert(sscanf(variant,"BlueRed%u",&row)==1&&row<6);
            for (unsigned s=0;s<4;++s) ctrl->positionAndscale[s].position.y=50.0f+s*20-row*30;
        }
    }
};
struct CtrlRaceBattleSetPoint : LayoutUIControl { unsigned team,idx; void Load(unsigned t,unsigned i) { team=t; idx=i; } };
struct CtrlRaceCount : LayoutUIControl { void Load(const char*,const char*) {} };
struct Page {
    PageId pageId; ControlGroup group;
    virtual ~Page() {}
    virtual void OnInit()=0;
    virtual bool CanEnd()=0;
    virtual void FillRows()=0;
    virtual PageId GetNextPage()=0;
    void InitControlGroup(unsigned n) { group.parentPage=this; group.controls.resize(n); }
    void AddControl(unsigned i,UIControl &ctrl,unsigned) { assert(i<group.controls.size()); assert(!group.controls[i]); group.controls[i]=&ctrl; ctrl.parentGroup=&group; }
};
namespace Pages {
struct TeamLeaderboardBase : Page { static bool hasTeamScoreDiffBeenCalcd; static int teamScoreDiff; CtrlRaceCount *ctrlRaceCount=nullptr; };
bool TeamLeaderboardBase::hasTeamScoreDiffBeenCalcd=true; int TeamLeaderboardBase::teamScoreDiff=9;
struct BattleLeaderboardUpdate : TeamLeaderboardBase {
    void OnInit() override { assert(false); }
    bool CanEnd() override { assert(false); return true; }
    void FillRows() override { assert(ctrlRaceCount); }
    PageId GetNextPage() override { return PAGE_BATTLE_TOTAL_LEADERBOARDS; }
};
struct BattleLeaderboardTotal : BattleLeaderboardUpdate { void FillRows() override {} };
}
unsigned GetCharacterBMGId(CharacterId,bool) { return 100; }
const char *GetCharacterIconPaneName(CharacterId) { return "icon"; }
namespace Pulsar {
namespace CustomCharacters { bool SetRaceNameTextIfCustom(LayoutUIControl &row,const char*,u8 id) { assert(id<12); row.player=id; return false; } }
namespace ScoreBased { bool active=true; bool IsBossBattle() { return active; } }
}
'''
TESTS = r'''
void initGroup(ControlGroup &group) {
    for (auto *ctrl: group.controls) {
        assert(ctrl&&ctrl->GetClassName());
        ctrl->InitSelf(); initGroup(ctrl->childrenGroup);
    }
}
int main() {
    using namespace Pulsar::ScoreBased;
    Raceinfo::Player scores[12];
    for (unsigned id=0;id<12;++id) raceinfo.players[id]=&scores[id];
    unsigned cases=0;
    for (unsigned count=2;count<=12;++count) for (unsigned boss=0;boss<count;++boss)
    for (unsigned online=0;online<2;++online) for (PageId pageId: {PAGE_BATTLE_LEADERBOARDS_UPDATE,PAGE_BATTLE_TOTAL_LEADERBOARDS}) {
        params.vsRaceCount=3;
        data.racesScenario.playerCount=count; data.racesScenario.localPlayerCount=2;
        data.racesScenario.settings.gamemode=online?MODE_PRIVATE_BATTLE:3;
        for (unsigned id=0;id<count;++id) {
            auto &p=data.racesScenario.players[id];
            p.team=id==boss?TEAM_RED:TEAM_BLUE;
            p.playerType=static_cast<PlayerType>(id%3);
            p.characterId=static_cast<CharacterId>(id%2?id:24);
            p.previousScore=id/2;
            data.menusScenario.players[id].score=100+id/2;
            scores[id].battleScore=id%3;
        }
        auto *page=CreateBossResultsPage(pageId); assert(page); page->pageId=pageId; page->OnInit();
        assert(!Pages::TeamLeaderboardBase::hasTeamScoreDiffBeenCalcd&&Pages::TeamLeaderboardBase::teamScoreDiff==0);
        assert(page->group.controls.size()==(pageId==0x33?3:2));
        for (unsigned activation=0;activation<2;++activation) {
            page->FillRows(); initGroup(page->group); assert(!page->CanEnd());
            std::set<int> displayed;
            for (unsigned team=0;team<2;++team) {
                unsigned n=team?count-1:1;
                auto &children=page->group.controls[team]->childrenGroup.controls;
                assert(children.size()==n+(pageId==0x33?1:params.vsRaceCount));
                unsigned sum=0,previous=~0u;
                for (unsigned i=0;i<n;++i) {
                    auto *row=dynamic_cast<LayoutUIControl*>(children[i]); assert(row&&!row->isHidden);
                    assert(row->player>=0&&static_cast<unsigned>(row->player)<count);
                    unsigned id=row->player; assert(displayed.insert(id).second);
                    assert(data.racesScenario.players[id].team==team&&row->red==(team==0));
                    unsigned score=pageId==0x33?scores[id].battleScore:data.menusScenario.players[id].score;
                    assert(static_cast<unsigned>(row->score)==score&&score<=previous); previous=score; sum+=score;
                    assert(data.menusScenario.players[id].gpRank==i+1);
                    for (unsigned s=0;s<4;++s) {
                        auto &p=row->positionAndscale[s];
                        assert(p.scale.z>0&&p.scale.z<=1);
                        if (i) assert(p.position.y<children[i-1]->positionAndscale[s].position.y);
                        assert(p.position.y>=50.0f+s*20-180);
                    }
                }
                if (pageId==0x33) assert(dynamic_cast<LayoutUIControl*>(children[n])->score==static_cast<int>(sum));
                else for (unsigned i=0;i<params.vsRaceCount;++i) {
                    auto *point=dynamic_cast<CtrlRaceBattleSetPoint*>(children[n+i]);
                    assert(point&&point->team==team&&point->idx==i);
                }
            }
            assert(displayed.size()==count);
        }
        delete page; assert(UIControl::live==0); ++cases;
    }
    active=false;
    assert(!CreateBossResultsPage(PAGE_BATTLE_LEADERBOARDS_UPDATE));
    assert(!CreateBossResultsPage(PAGE_BATTLE_TOTAL_LEADERBOARDS));
    printf("Boss results: %u scenarios passed, both pages, 2-12 players, every boss slot, offline/online, repeated activation, no omitted players.\n",cases);
}
'''
source=Path('PulsarEngine/Gamemodes/Battle/ScoreBased/ScoreBasedResults.cpp').read_text()
source='\n'.join(line for line in source.splitlines() if not line.startswith(('#include','typedef char Battle')))
with tempfile.TemporaryDirectory(prefix='boss-results-') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(STUBS+source+TESTS)
    subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-g',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)

# Verify that each linked region uses the replacement initializer, while its
# page vtables retain the native battle accounting and navigation methods.
import re
from check_battle_ai_teams import records, regional
symbols = {}
for line in Path('build/Code.map').read_text().splitlines():
    match = re.match(r'\s*([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]+)\s+(\S+)', line)
    if match:
        address, size, name = match.groups()
        symbols[name] = (int(address, 16), int(size, 16))

def symbol(prefix, contains):
    matches = [value for name, value in symbols.items() if name.startswith(prefix) and contains in name]
    assert len(matches) == 1, (prefix, contains, matches)
    return matches[0]

for region in 'PEJ':
    data = Path(f'build/Code.{region}.bin').read_bytes()
    linked = records(data)
    def pointer(address):
        return linked.get((1, address), struct.unpack_from('>I', data, 0x20 + address)[0])
    for page, fill, next_page in [('BattleLeaderboardUpdate', 0x8085ea64, 0x8085ede4),
                                 ('BattleLeaderboardTotal', 0x8085ea54, 0x8085ee14)]:
        table, _ = symbol('__vt__', 'BossLeaderboard<' + ('Q25Pages23' if page.endswith('Update') else 'Q25Pages22') + page)
        assert pointer(table + 0x28) == symbol('OnInit__', 'BossLeaderboard<' + ('Q25Pages23' if page.endswith('Update') else 'Q25Pages22') + page)[0]
        assert pointer(table + 0x64) == symbol('CanEnd__', 'BossLeaderboard<' + ('Q25Pages23' if page.endswith('Update') else 'Q25Pages22') + page)[0]
        assert pointer(table + 0x68) == regional(fill, region)
        assert pointer(table + 0x10) == regional(next_page, region)
        assert pointer(table + 0x4c) == regional(0x8085e7a0, region)
    team, _ = symbol('__vt__', '14BossResultTeam')
    assert pointer(team + 0x18) == symbol('InitSelf__', '14BossResultTeam')[0]
    assert pointer(team + 0x1c) == regional(0x805bd2e0, region)  # no native six-row updater
    factory, _ = symbol('CreateBossResultsPage__', 'ScoreBased')
    ui_start, ui_size = symbol('CreateAndInitPage__', 'ExpSection')
    calls = []
    for address in range(ui_start, ui_start + ui_size, 4):
        word, = struct.unpack_from('>I', data, 0x20 + address)
        if word >> 26 == 18 and word & 3 == 1:
            displacement = word & 0x3fffffc
            if displacement & 0x2000000: displacement -= 0x4000000
            calls.append(address + displacement)
    assert factory in calls, 'UI factory does not call Boss results factory'
    print(f'{region}: Boss results vtables and UI factory linked; native win accounting/navigation retained')

combined = Path('build/Code.pul').read_bytes()
lengths = struct.unpack_from('>4I', combined)
assert lengths[3] == 0
pos = 16
for region, length in zip('PEJ', lengths):
    assert combined[pos:pos+length] == Path(f'build/Code.{region}.bin').read_bytes()
    pos += length
assert pos == len(combined)
print('Combined Code.pul contains all three verified regional binaries.')
