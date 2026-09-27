"""Actual item-box wrapper with native-call stubs and regional link checks.

This does not run the game's box collision, physics or network delivery.
"""
from pathlib import Path
import struct
import subprocess
import tempfile
from check_battle_ai_teams import records, regional

source=Path('PulsarEngine/Extra/Extra.cpp').read_text()
body=source[source.index('static void RemoveSpecialItem('):source.index('kmCall(0x80828d70')]
harness=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
using u8=uint8_t; using u16=uint16_t; using u32=uint32_t;
namespace Pulsar {
using PulsarId=unsigned;
bool active, eliminated, finished; u8 boss; const char *filename;
namespace ScoreBased {
bool IsBoss(u8 id) { return active&&id==boss; }
bool IsEliminated(u8) { return eliminated; }
bool HasFinished() { return finished; }
}
struct CupsConfig {
    static CupsConfig *sInstance;
    PulsarId GetWinning() const { return 0; }
    static bool IsReg(PulsarId) { return false; }
    static unsigned GetCurVariantIdx() { return 0; }
    const char *GetFileName(PulsarId,unsigned) const { return filename; }
};
CupsConfig config; CupsConfig *CupsConfig::sInstance=&config;
}
namespace Item {
struct Player {
    u8 id; bool isRemote; unsigned heldItem,heldCount,roulette;
    unsigned rolls; u16 humanTable,cpuTable; u32 lottery;
    void DecideItem(u16 h,u16 c,u32 l) { ++rolls; humanTable=h; cpuTable=c; lottery=l; }
};
}
unsigned boosts; Item::Player *expected;
void ActivateMushroom(Item::Player *player) { assert(player==expected); ++boosts; }
#define kmRuntimeAddr(address) (&ActivateMushroom)
''' + body + r'''
int main() {
    unsigned cases=0;
    for(unsigned flags=0;flags<32;++flags) for(unsigned held=0;held<4;++held)
    for(unsigned course=0;course<3;++course) {
        Pulsar::active=flags&1; Pulsar::boss=0;
        Pulsar::eliminated=flags&8; Pulsar::finished=flags&16;
        Pulsar::filename=course==0?"CookieLand":course==1?"Z129":nullptr;
        Item::Player player{};
        player.id=(flags&2)?1:0; player.isRemote=flags&4;
        player.heldItem=held; player.heldCount=1; player.roulette=99;
        expected=&player; boosts=0;
        RemoveSpecialItem(&player,7,8,9);
        bool boss=Pulsar::active&&player.id==0;
        assert(boosts==unsigned(boss&&!player.isRemote&&!Pulsar::eliminated&&!Pulsar::finished));
        assert(player.rolls==unsigned(!boss));
        assert(player.heldItem==held&&player.heldCount==1&&player.roulette==99);
        if(!boss) {
            assert(player.humanTable==(course==1?0:7));
            assert(player.cpuTable==(course==1?0:8)&&player.lottery==9);
        }
        // A later valid collision can activate another boost, with no sticky flag.
        RemoveSpecialItem(&player,7,8,9);
        assert(boosts==2*unsigned(boss&&!player.isRemote&&!Pulsar::eliminated&&!Pulsar::finished));
        ++cases;
    }
    std::printf("Boss item boxes: %u wrapper scenarios passed, held grants preserved and ordinary item-box paths retained.\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='boss-box-') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
for region in 'PEJ':
    data=Path(f'build/Code.{region}.bin').read_bytes(); linked=records(data)
    start=linked[65,regional(0x80828d70,region)]
    assert start==linked[65,regional(0x80828da4,region)]
    words=[]
    for offset in range(start,struct.unpack_from('>I',data,0xc)[0],4):
        word=struct.unpack_from('>I',data,0x20+offset)[0]; words.append(word)
        if word==0x4e800020: break
    else: raise AssertionError('Missing wrapper return')
    native=regional(0x8079864c,region)
    assert (0x3d800000|(((native+0x8000)>>16)&0xffff)) in words
    assert (0x398c0000|(native&0xffff)) in words
    assert 0x4e800421 in words
    print(f'{region}: both item-box hooks and native Mushroom activation verified')
