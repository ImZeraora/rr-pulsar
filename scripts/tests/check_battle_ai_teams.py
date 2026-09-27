"""Native PAL team-builder regression and regional emitted-patch checks.

The PPC fixture is createTeamInfo at 80727f60..80728084, read from pal.raw.
Its allocation/kart lookup calls are stubbed; its real stores/loop run below.
This detects the old heap overrun without claiming to simulate Dolphin.
Run from the repo root after linking: python3 scripts/tests/check_battle_ai_teams.py
"""
import re
import struct
from pathlib import Path

START = 0x80727f60
NATIVE = bytes.fromhex('''
9421ffd0 7c0802a6 3cc0809c 90010034 bf210014 7c7f1b78 7c9a2378 7cb92b78
3860001c 80c62be8 83860014 4bb01e41 2c030000 41820024 93430018 38000000
90030000 90030004 90030008 9003000c 90030010 90030014 907f00c0 3860001c
4bb01e0d 2c030000 41820024 93230018 38000000 90030000 90030004 90030008
9003000c 90030010 90030014 907f00c4 3b200000 3b600000 3b400000 3fc0809c
3fa0809c 48000068 5720063e 807dd728 1c0000f0 7c630214 800300f4 2c000000
40820024 807e18f8 7f24cb78 4be680d5 4be69091 809f00c0 7c64d92e 3b7b0004
48000028 2c000001 40820020 807e18f8 7f24cb78 4be680ad 4be69069 809f00c4
7c64d12e 3b5a0004 3b390001 7c19e000 4180ff98 bb210014 80010034 7c0803a6
38210030 4e800020
''')
# Original instructions verified at every native producer/consumer. The only
# change is the allocation immediate (0x1c -> 0x34) or count offset (0x18 -> 0x30).
ORIGINAL = {
    0x80727f80: 0x3860001c, 0x80727fbc: 0x3860001c,
    0x80727f98: 0x93430018, 0x80727fcc: 0x93230018,
    0x80727d20: 0x80a30018, 0x80727d34: 0x80a30018,
    0x80727dc8: 0x83a50018, 0x80727e08: 0x83830018,
    0x80727eac: 0x83a30018, 0x807281dc: 0x80180018,
    0x807284e4: 0x83c30018, 0x807288b4: 0x83c30018,
    0x807288c4: 0x83a30018, 0x80728ad0: 0x83c30018,
    0x80728ae0: 0x83a30018, 0x80728fa4: 0x83830018,
    0x80728fb4: 0x83a30018, 0x80728cf0: 0x80650018,
}


def signed(value, bits=32):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def records(data):
    assert data[:8] == b'Kamek\0\0\2'
    pos = 0x20 + struct.unpack_from('>I', data, 0xc)[0]
    result = {}
    while pos < len(data):
        header, = struct.unpack_from('>I', data, pos)
        pos += 4
        cmd, address = header >> 24, header & 0xffffff
        if address == 0xfffffe:
            address, = struct.unpack_from('>I', data, pos)
            pos += 4
        assert cmd in (1, 4, 5, 6, 10, 32, 33, 34, 35, 36, 37, 38, 64, 65), cmd
        value, = struct.unpack_from('>I', data, pos)
        pos += 8 if cmd in (35, 36, 37, 38) else 4
        assert (cmd, address) not in result, (cmd, hex(address))
        result[cmd, address] = value
    assert pos == len(data)
    return result


def regional(address, region):
    current = None
    for line in Path('GameSource/versions.txt').read_text().splitlines():
        if line.startswith('['):
            current = line[1:-1]
        match = re.match(r'([0-9a-fA-F]+)-(\*|[0-9a-fA-F]+): ([+-]0x[0-9a-fA-F]+)', line)
        if current == region and match:
            start, end, delta = match.groups()
            if int(start, 16) <= address <= (0xffffffff if end == '*' else int(end, 16)):
                return address + int(delta, 16)
    raise AssertionError(f'No {region} mapping for {address:08x}')


def build_team_lists(teams, patches):
    regs, memory, allocations, overwritten = [0] * 32, {}, [], []
    manager, race, ai = 0x90001000, 0x90002000, 0x90005000
    regs[1], regs[3], regs[4], regs[5] = 0x80399000, manager, teams.count(0), teams.count(1)
    memory.update({0x809c2be8: ai, 0x809bd728: race, 0x809c18f8: 0x90006000, ai + 0x14: len(teams)})
    for player, team in enumerate(teams):
        memory[race + 0xf4 + player * 0xf0] = team
    instructions = dict(zip(range(START, START + len(NATIVE), 4), struct.unpack('>74I', NATIVE)))
    instructions.update({a: w for a, w in patches.items() if a in instructions})
    pc, lr, compare = START, 0x12345678, 0

    def store(address, value):
        for pointer, size in allocations:
            if pointer + size <= address < pointer + size + 0x20:
                overwritten.append(address)
        memory[address] = value & 0xffffffff

    for step in range(5000):
        word, here = instructions[pc], pc
        pc += 4
        op, rs, ra, rb = word >> 26, word >> 21 & 31, word >> 16 & 31, word >> 11 & 31
        imm, xo = signed(word & 0xffff, 16), word >> 1 & 1023
        if word == 0x4e800020:
            assert lr == 0x12345678
            break
        if word == 0x7c0802a6:
            regs[0] = lr
        elif word == 0x7c0803a6:
            lr = regs[0]
        elif op in (14, 15):
            regs[rs] = ((regs[ra] if ra else 0) + (imm << 16 if op == 15 else imm)) & 0xffffffff
        elif op in (32, 36, 37, 46, 47):
            address = ((regs[ra] if ra else 0) + imm) & 0xffffffff
            if op == 32:
                regs[rs] = memory[address]
            elif op == 46:
                for reg in range(rs, 32):
                    regs[reg] = memory[address + 4 * (reg - rs)]
            elif op == 47:
                for reg in range(rs, 32):
                    store(address + 4 * (reg - rs), regs[reg])
            else:
                store(address, regs[rs])
                if op == 37:
                    regs[ra] = address
        elif op == 11:
            compare = signed(regs[ra]) - imm
        elif op == 7:
            regs[rs] = signed(regs[ra]) * imm & 0xffffffff
        elif op == 21:
            assert word == 0x5720063e  # clrlwi r0,r25,24
            regs[0] = regs[25] & 255
        elif op == 31 and xo == 444:
            regs[ra] = regs[rs] | regs[rb]
        elif op == 31 and xo == 266:
            regs[rs] = (regs[ra] + regs[rb]) & 0xffffffff
        elif op == 31 and xo == 151:
            store((regs[ra] + regs[rb]) & 0xffffffff, regs[rs])
        elif op == 31 and xo == 0:
            compare = signed(regs[ra]) - signed(regs[rb])
        elif op == 16:
            condition = {0x4182: compare == 0, 0x4082: compare != 0, 0x4180: compare < 0}[word >> 16]
            if condition:
                pc = here + signed(word & 0xfffc, 16)
        elif op == 18:
            target = here + signed(word & 0x3fffffc, 26)
            if word & 1:
                lr = pc
                if target == 0x80229dcc:
                    pointer = 0x91000000 + len(allocations) * 0x100
                    allocations.append((pointer, regs[3]))
                    regs[3] = pointer
                elif target == 0x80590100:
                    regs[3] = 0x92000000 + regs[4] * 0x100
                else:
                    assert target == 0x805910c0
                    regs[3] += 0x40  # distinct nonzero AI pointer for each player
            else:
                pc = target
        else:
            raise AssertionError(f'Unsupported instruction {here:08x}: {word:08x}')
    else:
        raise AssertionError('Native builder did not return')
    return memory, allocations, overwritten


def main():
    patches = {a: (w & 0xffff0000) | (0x34 if w == 0x3860001c else 0x30) for a, w in ORIGINAL.items()}
    for region in 'PEJ':
        data = Path(f'build/Code.{region}.bin').read_bytes()
        linked = records(data)
        for address, value in patches.items():
            assert linked[32, regional(address, region)] == value, hex(address)
        # Check the scratch-base hook's real opcodes and both relocations.
        hook = linked[65, regional(0x80728534, region)]
        words = struct.unpack_from('>3I', data, 0x20 + hook)
        assert words[0] >> 16 == 0x3fe0 and words[1] >> 16 == 0x3bff
        assert words[2] == 0x4e800020
        assert linked[32, regional(0x80728540, region)] == 0x7fe5fb78
        assert linked[32, regional(0x80728548, region)] == 0x60000000
        scratch = linked[6, hook + 2]
        assert linked[4, hook + 6] == scratch
        bss, code_size = struct.unpack_from('>2I', data, 8)
        assert code_size <= scratch and scratch + 48 <= code_size + bss
        print(f'{region}: all team-layout patches and the 12-entry permutation buffer are linked')
    # Reproduce the old 1-vs-11 overflow, including corruption of the count.
    memory, allocations, overwritten = build_team_lists([0] + [1] * 11, {})
    assert overwritten and memory[allocations[1][0] + 0x18] != 11
    # Exhaust every red/blue split, every boss slot, and ordinary balanced teams.
    for count in range(2, 13):
        for mask in range(1, (1 << count) - 1):
            teams = [(mask >> i) & 1 for i in range(count)]
            memory, allocations, overwritten = build_team_lists(teams, patches)
            assert not overwritten
            for team, (pointer, size) in enumerate(allocations):
                members = [i for i, t in enumerate(teams) if t == team]
                assert size == 0x34 and memory[pointer + 0x30] == len(members)
                assert [memory[pointer + i * 4] for i in range(len(members))] == [0x92000040 + i * 0x100 for i in members]
    print('Old native overflow reproduced; all 2-12-player team splits retain every AI pointer without heap writes')


if __name__ == '__main__':
    main()
