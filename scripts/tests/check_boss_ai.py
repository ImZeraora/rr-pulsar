"""Actual Boss AI hook bodies with native-call stubs, plus regional relocations.

Run from the repository root after linking. This does not simulate steering,
ENPT navigation, collisions or network delivery in Dolphin.
"""
from pathlib import Path
import struct
import subprocess
import tempfile
from check_battle_ai_teams import records, regional

source = Path('PulsarEngine/Gamemodes/Battle/ScoreBased/ScoreBasedBossAI.cpp').read_text()
# Target layout assertions are enforced by MWCC; the host has 64-bit pointers.
source = '\n'.join(line for line in source.splitlines()
                   if not line.startswith(('#include', 'typedef char Check')))
harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
using u8=uint8_t; using u32=uint32_t;
struct Vec3 { float x,y,z; };
class KartAIController;
namespace Kart {
struct Status { u32 bitfield0,bitfield1,bitfield2,bitfield4; };
struct Pointers { Status *kartStatus; KartAIController *kartAIController; };
struct Player { Pointers pointers; };
struct Manager { static Manager *sInstance; Player **players; u8 playerCount; };
Manager *Manager::sInstance;
}
class KartAIController {
public:
    Kart::Pointers *pointers; u8 id; Vec3 position;
    u8 GetPlayerIdx() const { return id; }
    const Vec3 &GetPosition() const { return position; }
};
enum { PLAYER_REAL_LOCAL,PLAYER_CPU,PLAYER_NONE,PLAYER_GHOST,PLAYER_REAL_ONLINE };
enum { RACESTAGE_INTRO,RACESTAGE_COUNTDOWN,RACESTAGE_RACE,RACESTAGE_FINISHED };
struct RacedataScenario { u8 playerCount; struct { int playerType; } players[12]; };
struct Racedata { static Racedata *sInstance; RacedataScenario racesScenario; };
Racedata *Racedata::sInstance;
struct Raceinfo { static Raceinfo *sInstance; int stage; };
Raceinfo *Raceinfo::sInstance;
bool active=true,finished=false; u8 bossId; u32 eliminated;
namespace Pulsar { namespace ScoreBased {
bool IsBossBattle() { return active; }
bool HasFinished() { return finished; }
u8 GetBossPlayerId() { return bossId; }
bool IsEliminated(u8 id) { assert(id<12); return (eliminated & (1u<<id))!=0; }
} }
uintptr_t Native(u32 address);
#define kmRuntimeUse(address)
#define kmRuntimeAddr(address) Native(address)
#define kmCall(address, function)
#define kmWritePointer(address, function)
''' + source + r'''
using namespace Pulsar::ScoreBased;
KartAIController ai[12]; Kart::Status status[12]; Kart::Player players[12];
Kart::Player *playerPtrs[12]; Kart::Manager manager; Racedata data; Raceinfo race;
BossAIInputs inputs; BossAIPathPoint point; BossAIPath path; BossAIControl control;
BossAIDriveInfo drive; BossAIPerception perception;
int basicCalls,angleCalls,updateCalls,perceptionCalls,countCalls;
u32 nativeCount; int expectedTarget=-1; bool direct;
const Vec3 routePosition={1234,0,5678}, savedTarget={13,17,19};
bool Equal(const Vec3 &a,const Vec3 &b) { return a.x==b.x&&a.y==b.y&&a.z==b.z; }
void Basic(BossAIControl *c,BossAIDriveInfo *d) {
    assert(c==&control&&d==&drive); ++basicCalls; d->destination=routePosition;
}
void Angle(BossAIControl *c,BossAIDriveInfo *d) {
    assert(c==&control&&d==&drive&&basicCalls==1); ++angleCalls;
}
void Update(BossAIControl *c,BossAIDriveInfo *d) {
    assert(c==&control&&d==&drive); ++updateCalls;
    if(expectedTarget>=0) {
        assert(c->routeMode==9&&!path.skipPrevious);
        assert(Equal(point.target,ai[expectedTarget].position));
        assert(Equal(d->destination,direct?ai[expectedTarget].position:routePosition));
        assert(basicCalls==1&&angleCalls==1);
    } else {
        assert(c->routeMode==3&&Equal(point.target,savedTarget)&&path.skipPrevious);
        assert(basicCalls==0&&angleCalls==0);
    }
}
void Perceive(BossAIPerception *p) { assert(p==&perception); ++perceptionCalls; }
u32 Count(void *p,u32 id) { assert(p==&manager&&id==bossId); ++countCalls; return nativeCount; }
uintptr_t Native(u32 address) {
    switch(address) {
    case 0x8072b618: return reinterpret_cast<uintptr_t>(&Basic);
    case 0x8072b680: return reinterpret_cast<uintptr_t>(&Angle);
    case 0x8072a570: return reinterpret_cast<uintptr_t>(&Update);
    case 0x8072c5e8: return reinterpret_cast<uintptr_t>(&Perceive);
    case 0x80727cf8: return reinterpret_cast<uintptr_t>(&Count);
    default: assert(false); return 0;
    }
}
void Reset(u8 boss=0,u8 count=12) {
    active=true; finished=false; eliminated=0; bossId=boss;
    manager.players=playerPtrs; manager.playerCount=count; Kart::Manager::sInstance=&manager;
    data.racesScenario.playerCount=count; Racedata::sInstance=&data;
    race.stage=RACESTAGE_RACE; Raceinfo::sInstance=&race;
    for(u8 id=0;id<12;++id) {
        status[id]={}; players[id].pointers={&status[id],&ai[id]}; playerPtrs[id]=&players[id];
        ai[id].pointers=&players[id].pointers; ai[id].id=id; ai[id].position={float(id*200),0,0};
        data.racesScenario.players[id].playerType=PLAYER_CPU;
    }
    inputs.controller=&ai[boss]; control.inputs=&inputs; control.path=&path; control.routeMode=3;
    path.point=&point; path.skipPrevious=true; point.target=savedTarget;
    perception={}; perception.inputs=&inputs; perception.lockedTarget=&ai[0]; perception.native3c=0xabcdef12;
    basicCalls=angleCalls=updateCalls=perceptionCalls=countCalls=0; expectedTarget=-1; direct=false;
}
void Run(int target) {
    basicCalls=angleCalls=updateCalls=0; expectedTarget=target;
    if(target>=0) {
        const Vec3 &a=ai[bossId].position,&b=ai[target].position;
        direct=(a.y-b.y>-200&&a.y-b.y<200&&DistanceSquared(a,b)<1000000);
    }
    UpdateBossDriving(&control,&drive);
    assert(updateCalls==1&&control.routeMode==3&&path.skipPrevious&&Equal(point.target,savedTarget));
}
int main() {
    unsigned scenarios=0;
    // Every boss slot, roster size, active power and local/online human kind.
    for(u8 count=2;count<=12;++count) for(u8 boss=0;boss<count;++boss)
    for(int effect=0;effect<4;++effect) for(int online=0;online<2;++online) {
        Reset(boss,count); const u8 human=(boss+count-1)%count;
        data.racesScenario.players[human].playerType=online?PLAYER_REAL_ONLINE:PLAYER_REAL_LOCAL;
        if(effect==1) status[boss].bitfield1=0x80000000;
        if(effect==2) status[boss].bitfield2=0x8000;
        if(effect==3) status[human].bitfield2=0x80;
        Run(effect?human:-1);
        // Native state is restored, and removal of the effect ends pursuit immediately.
        status[boss].bitfield1=status[boss].bitfield2=status[human].bitfield2=0;
        Run(-1); ++scenarios;
    }
    Reset(); data.racesScenario.players[11].playerType=PLAYER_REAL_LOCAL;
    status[0].bitfield1=0x80000000; Run(11); // human beyond native six-slot list
    UpdateBossPerception(&perception);
    assert(perception.opponents[0]==&ai[11]&&perception.lockedTarget==&ai[0]);
    for(int slot=1;slot<6;++slot) assert(perception.opponents[slot]==&ai[slot]);
    assert(perception.native3c==0xabcdef12&&perceptionCalls==1);
    data.racesScenario.players[2].playerType=PLAYER_REAL_LOCAL; Run(2); // nearest human
    eliminated=1u<<2; Run(11); eliminated|=1u<<11; Run(-1);
    UpdateBossPerception(&perception);
    for(int slot=0;slot<6;++slot) assert(perception.opponents[slot]!=&ai[2]&&perception.opponents[slot]!=&ai[11]);
    // A nearby full-size human does not displace a more distant shrunken human.
    Reset(); data.racesScenario.players[1].playerType=PLAYER_REAL_LOCAL;
    data.racesScenario.players[11].playerType=PLAYER_REAL_ONLINE; status[11].bitfield2=0x80; Run(11);
    // Close pursuit, separate floors, then return to route following at range.
    ai[11].position={300,0,0}; Run(11); ai[11].position.y=201; Run(11);
    ai[11].position={3000,0,0}; Run(11);
    // Lifecycle, ownership, Bullet and temporarily unavailable targets.
    for(int condition=0;condition<15;++condition) {
        Reset(); data.racesScenario.players[1].playerType=PLAYER_REAL_LOCAL; status[0].bitfield2=0x8000;
        switch(condition) {
        case 0: active=false; break;
        case 1: finished=true; break;
        case 2: race.stage=RACESTAGE_COUNTDOWN; break;
        case 3: race.stage=RACESTAGE_FINISHED; break;
        case 4: data.racesScenario.players[0].playerType=PLAYER_REAL_LOCAL; break;
        case 5: bossId=2; break;
        case 6: eliminated=1; break;
        case 7: status[0].bitfield4=8; break;
        case 8: status[0].bitfield2|=0x08000000; break;
        case 9: status[1].bitfield0=0x10; break;
        case 10: status[1].bitfield1=0x10; break;
        case 11: status[1].bitfield2=0x80000; break;
        case 12: playerPtrs[1]=nullptr; break;
        case 13: manager.playerCount=1; break;
        case 14: Kart::Manager::sInstance=nullptr; break;
        }
        Run(-1);
    }
    // Bounded perception writes for every boss/roster, including no opponents.
    for(u8 count=2;count<=12;++count) for(u8 boss=0;boss<count;++boss) {
        Reset(boss,count); UpdateBossPerception(&perception);
        unsigned seen=0; float previous=-1;
        for(int slot=0;slot<6;++slot) {
            const auto *opponent=perception.opponents[slot];
            assert(perception.classifications[slot]==0);
            if(!opponent) continue;
            assert(opponent->id<count&&opponent->id!=boss&&!(seen&(1u<<opponent->id)));
            seen|=1u<<opponent->id;
            const float distance=DistanceSquared(ai[boss].position,opponent->position);
            assert(distance>=previous); previous=distance;
        }
        assert(__builtin_popcount(seen)==(count-1<6?count-1:6));
        assert(perception.native3c==0xabcdef12&&perception.lockedTarget==&ai[0]);
        eliminated=0xfff^(1u<<boss); UpdateBossPerception(&perception);
        for(auto *opponent:perception.opponents) assert(!opponent);
    }
    Reset(); active=false; perception.opponents[0]=&ai[4]; perception.classifications[0]=77;
    UpdateBossPerception(&perception); assert(perceptionCalls==1);
    assert(perception.opponents[0]==&ai[4]&&perception.classifications[0]==77);
    for(nativeCount=0;nativeCount<=12;++nativeCount) {
        countCalls=0;
        assert(GetPerceptionOpponentCount(&manager,bossId)==(nativeCount<6?nativeCount:6));
        assert(countCalls==1);
    }
    std::printf("Boss AI: %u power/roster scenarios plus targeting, lifecycle, route restoration and cache bounds passed.\n",scenarios);
}
'''
with tempfile.TemporaryDirectory(prefix='boss-ai-') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)

# All three callsite hooks must point inside the new module;
# the five native call addresses must map to each region in emitted code.
for region in 'PEJ':
    data=Path(f'build/Code.{region}.bin').read_bytes()
    linked=records(data)
    code_size=struct.unpack_from('>I',data,0xc)[0]
    hooks = [(65,0x8072b5cc,(0x8072b618,0x8072b680,0x8072a570)),
             (65,0x8072b8c8,(0x80727cf8,)), (65,0x8072b518,(0x8072c5e8,))]
    for command,address,native_calls in hooks:
        start=linked[command,regional(address,region)]
        assert 0 <= start < code_size
        words=[]
        for offset in range(start,code_size,4):
            word=struct.unpack_from('>I',data,0x20+offset)[0]
            words.append(word)
            if word==0x4e800020: break
        else: raise AssertionError('No return in emitted hook')
        # MWCC materializes these addresses with lis/addi r12 then bctrl.
        for native in native_calls:
            mapped=regional(native,region)
            hi=((mapped+0x8000)>>16)&0xffff
            lo=mapped&0xffff
            assert 0x3d800000|hi in words,hex(native)
            assert 0x398c0000|lo in words,hex(native)
        assert 0x4e800421 in words
    print(f'{region}: Boss driving and bounded perception hooks and native calls are linked')
