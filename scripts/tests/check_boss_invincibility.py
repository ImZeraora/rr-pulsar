"""Boss blink duration and initial balloons, using actual production bodies.

Native calls are stubbed; this does not simulate damage or network delivery.
Run from the repository root after rebuilding Code.pul.
"""
from pathlib import Path
import struct
import subprocess
import tempfile
from check_battle_ai_teams import records, regional

source = Path('PulsarEngine/Gamemodes/Battle/ScoreBased/ScoreBasedBoss.cpp').read_text()
balloons = source[source.index('u8 GetBossStartingBalloons'):source.index('// Both native blink starters')]
hook = source[source.index('static void *GetBossBattleBlink'):source.index('kmCall(0x8058180c')]
harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
using u8=uint8_t; using s16=int16_t;
bool active; u8 boss, player;
bool IsBoss(u8 id) { return active && id<12 && id==boss; }
namespace Kart {
struct Movement {
    alignas(s16) u8 bytes[0x294];
    u8 GetPlayerIdx() const { return player; }
};
}
unsigned calls;
Kart::Movement *expected;
int blinkObject;
void *GetBlink(Kart::Movement *movement) {
    assert(movement==expected); ++calls; return &blinkObject;
}
#define kmRuntimeAddr(address) (&GetBlink)
''' + balloons + hook + r'''
int main() {
    unsigned cases=0;
    Kart::Movement movement;
    expected=&movement;
    for(unsigned mode=0;mode<2;++mode) for(boss=0;boss<12;++boss)
    for(player=0;player<14;++player) for(s16 original:{s16(0),s16(150),s16(180),s16(299)}) {
        active=mode;
        std::memset(movement.bytes,0xa5,sizeof(movement.bytes));
        s16 &timer=*reinterpret_cast<s16 *>(movement.bytes+0x1a8);
        timer=original;
        calls=0;
        assert(GetBossBattleBlink(&movement)==&blinkObject&&calls==1);
        const bool isBoss=mode&&player<12&&player==boss;
        assert(timer==(isBoss?300:original));
        assert(GetBossStartingBalloons(player)==(isBoss?5:1));
        // Only the two timer bytes may change; the native blink object is retained.
        for(unsigned i=0;i<sizeof(movement.bytes);++i)
            if(i!=0x1a8&&i!=0x1a9) assert(movement.bytes[i]==0xa5);
        GetBossBattleBlink(&movement);
        assert(calls==2&&timer==(isBoss?300:original)); // assign, never compound
        ++cases;
    }
    std::printf("Boss invincibility: %u cases passed; 300-frame boss duration, one/five starting balloons, native fallbacks and field boundaries.\n",cases);
}
'''
# initializer_list is required by the range-for initializer list above.
harness='#include <initializer_list>\n'+harness
with tempfile.TemporaryDirectory(prefix='boss-invincibility-') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)

for region in 'PEJ':
    data=Path(f'build/Code.{region}.bin').read_bytes()
    linked=records(data)
    code_size=struct.unpack_from('>I',data,0xc)[0]
    start=linked[65,regional(0x8058180c,region)]
    assert start==linked[65,regional(0x80581a0c,region)] and start<code_size
    words=[]
    for offset in range(start,code_size,4):
        word=struct.unpack_from('>I',data,0x20+offset)[0]
        words.append(word)
        if word==0x4e800020: break
    else: raise AssertionError('Missing hook return')
    getter=regional(0x8059108c,region)
    assert 0x3d800000|(((getter+0x8000)>>16)&0xffff) in words
    assert 0x398c0000|(getter&0xffff) in words
    assert 0x4e800421 in words
    # Check the emitted timer assignment: li rN,300 then sth rN,0x1a8(rBase).
    registers={word>>21&31 for word in words if word>>26==14 and word>>16&31==0 and word&0xffff==300}
    assert any(word>>26==44 and word>>21&31 in registers and word&0xffff==0x1a8 for word in words)
    print(f'{region}: both blink hooks, 300-frame timer store and native getter verified')
