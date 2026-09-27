"""Boss shrink-crush predicate, emitted wrapper, and PAL collision decisions.

Native collision decision fixture: pal.raw 8056fdf8..8057073c, retaining only
branches up to the damage paths. Native geometry/damage/network effects are
not emulated. Run from the repo root after linking.
"""
from pathlib import Path
import itertools
import re
import struct
import subprocess
import tempfile
from check_battle_ai_teams import records, regional, signed

source=Path('PulsarEngine/Gamemodes/Battle/ScoreBased/ScoreBasedBossCollision.cpp').read_text()
predicate=source[source.index('static u32 GetBossCrushMask'):source.index('// PAL CheckKartCollision')]
harness=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
using u8=uint8_t; using u32=uint32_t;
bool active; u8 boss; u32 eliminated;
bool IsBossBattle() { return active; }
u8 GetBossPlayerId() { return boss; }
bool IsEliminated(u8 id) { assert(id<12); return (eliminated&(1u<<id))!=0; }
namespace Kart {
struct Status { u32 bitfield2; };
struct Pointers { Status *kartStatus; };
struct Link { Pointers *pointers; u8 id; u8 GetPlayerIdx() const { return id; } };
}
'''+predicate+r'''
int main() {
    Kart::Status a{},b{}; Kart::Pointers ap{&a},bp{&b}; Kart::Link first{&ap,0},second{&bp,1};
    unsigned cases=0;
    for (unsigned mode=0;mode<2;++mode) for (boss=0;boss<12;++boss)
    for (unsigned x=0;x<14;++x) for (unsigned y=0;y<14;++y)
    for (unsigned shrink=0;shrink<4;++shrink) for (unsigned lost=0;lost<4;++lost) {
        active=mode; first.id=x; second.id=y;
        a.bitfield2=(shrink&1)?0x80:0; b.bitfield2=(shrink&2)?0x80:0;
        eliminated=((lost&1)?1u<<x:0)|((lost&2)?1u<<y:0);
        unsigned expected=0;
        if (active&&x<12&&y<12&&x!=y&&!lost) {
            if(x==boss&&(shrink&2)) expected=1;
            if(y==boss&&(shrink&1)) expected=2;
        }
        assert(GetBossCrushMask(first,second)==expected);
        assert(a.bitfield2==((shrink&1)?0x80u:0)&&b.bitfield2==((shrink&2)?0x80u:0));
        ++cases;
    }
    printf("Boss crush: %u predicate scenarios passed, both directions, shrink/recovery, non-boss contacts, elimination, invalid IDs, disabled mode.\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='boss-crush-') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)


def run_wrapper(code, mask, mega):
    regs=[0xaab00000+i for i in range(32)]
    regs[1],regs[3],regs[30],regs[31]=0x80398000,0x809c0000,0x90002000,0x90003000
    regs[24],regs[23]=mega
    before=regs[:]
    lr=0x8056fdd0
    memory={}
    calls=0
    for word in code:
        op,rt,ra,rb=word>>26,(word>>21)&31,(word>>16)&31,(word>>11)&31
        imm=signed(word&0xffff,16)
        if op in (32,36,37):
            address=((regs[ra] if ra else 0)+imm)&0xffffffff
            if op==32: regs[rt]=memory[address]
            else:
                memory[address]=regs[rt]
                if op==37: regs[ra]=address
        elif op==14: regs[rt]=((regs[ra] if ra else 0)+imm)&0xffffffff
        elif word==0x7c0802a6: regs[0]=lr
        elif word==0x7c0803a6: lr=regs[0]
        elif op==31 and (word>>1)&1023==444: regs[ra]=regs[rt]|regs[rb]
        elif op==21:
            shift,mb,me=(word>>11)&31,(word>>6)&31,(word>>1)&31
            assert mb==me==31
            value=((regs[rt]<<shift)|(regs[rt]>>(32-shift)))&0xffffffff
            regs[ra]=value&1
        elif op==18 and word&1:
            assert regs[3]==before[30]+4 and regs[4]==before[31]
            calls+=1
            for i in (0,*range(3,13)): regs[i]=0xdad00000+i
            regs[3]=mask; lr=0xdeadbeef
        elif word==0x4e800020: break
        else: raise AssertionError(f'Unhandled wrapper word {word:08x}')
    else: raise AssertionError('Wrapper did not return')
    assert calls==1 and lr==0x8056fdd0
    for i in (1,2,3,*range(13,23),*range(25,29),30,31):
        assert regs[i]==before[i], f'Live/preserved r{i} was clobbered'
    assert regs[29]==0
    assert regs[24]==(mega[0]|(mask&1))
    assert regs[23]==(mega[1]|((mask>>1)&1))

for region in 'PEJ':
    data=Path(f'build/Code.{region}.bin').read_bytes()
    linked=records(data)
    hook=linked[65,regional(0x8056fdcc,region)]
    code=struct.unpack_from('>17I',data,0x20+hook)
    for mask in range(3):
        for mega in itertools.product(range(2),repeat=2): run_wrapper(code,mask,mega)
    print(f'{region}: linked collision hook preserves live registers and native Mega flags')

# Execute the actual PAL eligibility branches, stopping at the damage blocks.
fixture={}
for line in Path('scripts/tests/fixtures/boss_crush_decisions.txt').read_text().splitlines():
    address,instruction=line.split(': ')
    parts=instruction.strip().split(' ',1)
    fixture[int(address,16)]=(parts[0],parts[1].split(',') if len(parts)>1 else [])
endpoints={0x8056fe70:('bullet',0),0x8056ff38:('bullet',1),
           0x80570030:('star',0),0x80570208:('star',1),
           0x80570410:('crush',0),0x805705a0:('crush',1),
           0x80570738:('none',None),0x8057073c:('none',None)}

def decide(mega=(0,0),crushed=(0,0),star=(0,0),bullet=(0,0),same_team=0):
    regs=[0]*32
    regs[24],regs[23]=mega; regs[22],regs[21]=crushed
    regs[26],regs[25]=star; regs[20],regs[19]=bullet; regs[27]=same_team
    pc,eq=0x8056fdf8,False
    for _ in range(100):
        if pc in endpoints: return endpoints[pc]
        op,args=fixture[pc]; pc+=4
        if op=='cmpwi': eq=regs[int(args[0][1:])]==int(args[1],0)
        elif op=='li': regs[int(args[0][1:])]=int(args[1],0)
        elif op=='b' or (op=='beq' and eq) or (op=='bne' and not eq): pc=int(args[0],0)
        elif op not in ('beq','bne'): raise AssertionError(op)
    raise AssertionError('Native decision branches did not terminate')

for boss_side in (0,1):
    mega=[0,0]; mega[boss_side]=1
    target=1-boss_side
    assert decide()==('none',None)
    assert decide(mega=mega)==('crush',target)
    crushed=[0,0]; crushed[target]=1
    assert decide(mega=mega,crushed=crushed)==('none',None)
    assert decide(mega=(1,1))==('none',None)
    assert decide(mega=mega,same_team=1)==('none',None)
    for powered_side in (0,1):
        powered=[0,0]; powered[powered_side]=1
        assert decide(mega=mega,star=powered)==decide(star=powered)==('star',1-powered_side)
        assert decide(mega=mega,bullet=powered)==decide(bullet=powered)==('bullet',1-powered_side)
print('Native PAL decision branches: correct squash victim in both directions; crushed/Mega immunity and Star/Bullet priority retained.')
