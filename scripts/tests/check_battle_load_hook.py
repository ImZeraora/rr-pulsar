"""Check the emitted SetFFAmode wrapper against its native continuation.

Run after linking: python3 scripts/tests/check_battle_load_hook.py build/Code.P.bin
The helper is replaced with an ABI-legal volatile-register clobber. This checks
register preservation, not the helper's game behavior or a Dolphin track load.
"""
import struct
import sys
from pathlib import Path


def check(path):
    data = Path(path).read_bytes()
    # Stable prefix shared by the broken and repaired wrappers.
    prefix = bytes.fromhex('9421ffe0900100107c0802a6900100249061000890c1000c')
    offset = data.find(prefix)
    assert offset >= 0 and data.find(prefix, offset + 1) < 0, 'Expected one SetFFAmode wrapper'
    code = struct.unpack_from('>32I', data, offset)
    base, stack = 0x90a05df0, 0x80398fc8
    initial = [0xabc00000 + i for i in range(32)]
    initial[0], initial[1], initial[3], initial[6], initial[31] = 14, stack, 0x7703, 0x78503bd9, base
    initial[4], initial[5] = base + 0x1788, base + 0xb98
    for mode_flags in (0, 2, 0x1234):
        regs = initial[:]
        lr = return_address = 0x80530570
        memory = {base + 0x178c + 4 * i: 0xace00000 + i for i in range(28)}
        calls = 0
        for word in code:
            op, rt, ra = word >> 26, (word >> 21) & 31, (word >> 16) & 31
            imm = word & 0xffff
            if imm & 0x8000:
                imm -= 0x10000
            if op in (32, 36, 37):  # lwz, stw, stwu
                address = ((regs[ra] if ra else 0) + imm) & 0xffffffff
                if op == 32:
                    regs[rt] = memory[address]
                else:
                    memory[address] = regs[rt]
                    if op == 37:
                        regs[ra] = address
            elif op == 14:  # addi
                regs[rt] = ((regs[ra] if ra else 0) + imm) & 0xffffffff
            elif word == 0x7c0802a6:  # mflr r0
                regs[0] = lr
            elif word == 0x7c0803a6:  # mtlr r0
                lr = regs[0]
            elif word == 0x7fe3fb78:  # mr r3,r31
                regs[3] = regs[31]
            elif op == 18 and word & 1:  # bl helper
                assert regs[3] == base
                calls += 1
                memory[base + 0xb90] = mode_flags
                for reg in (0, *range(3, 13)):
                    regs[reg] = 0x50602d6a + reg
                lr = 0xdeadbeef
            elif word == 0x4e800020:  # blr
                break
            else:
                raise AssertionError(f'Unexpected wrapper instruction {word:08x}')
        else:
            raise AssertionError('Missing wrapper return')
        assert calls == 1 and lr == return_address
        for reg in (0, 1, 3, 4, 5, 6, 31):
            assert regs[reg] == initial[reg], f'r{reg} clobbered: {regs[reg]:08x}'
        # PAL 80530570..8053058c: pending stores followed by 14 double-word copies.
        memory[base + 0xb94] = regs[6]
        memory[base + 0xb98] = regs[3]
        for _ in range(regs[0]):
            regs[3] = memory[regs[4] + 4]
            regs[0] = memory[regs[4] + 8]
            regs[4] += 8
            memory[regs[5] + 4] = regs[3]
            memory[regs[5] + 8] = regs[0]
            regs[5] += 8
        assert memory[base + 0xb90] == mode_flags
        assert memory[base + 0xb94] == initial[6]
        assert memory[base + 0xb98] == initial[3]
        for i in range(28):
            assert memory[base + 0xb9c + 4 * i] == 0xace00000 + i
    print(f'{path}: emitted hook preserves live registers and completes the native scenario copy')


if __name__ == '__main__':
    for filename in sys.argv[1:] or ['build/Code.P.bin', 'build/Code.E.bin', 'build/Code.J.bin']:
        check(filename)
