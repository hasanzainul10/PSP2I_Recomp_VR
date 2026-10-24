"""Print one recompiled function's MIPS disassembly from port/gen/recomp_funcs.c.

The generated C keeps every original instruction as a comment
(`/* 08AFCE90  addiu  $sp, $sp, -48 */`). This prints those lines for the
function at ADDR, up to its first `jr $ra` and the delay slot (or --all for the
whole generated body, which may hold tail-call fragments).

usage: python -I disasm.py [--gen port/gen/recomp_funcs.c] [--all] ADDR [ADDR...]
"""
import argparse
import re

INSN = re.compile(r'^    /\* ([0-9A-F]{8})  (.*?) \*/\s*$')


def body(gen, addr):
    start = re.compile(r'^static void psp_body_%s\(' % addr)
    out = []
    with open(gen, encoding='utf-8', errors='replace') as f:
        inside = False
        for line in f:
            if not inside:
                inside = bool(start.match(line))
                continue
            if line.startswith('static void psp_body_'):
                break
            m = INSN.match(line)
            if m:
                out.append((m.group(1), m.group(2)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default='port/gen/recomp_funcs.c')
    ap.add_argument('--all', action='store_true', help='the whole generated body, not just to the first jr $ra')
    ap.add_argument('addr', nargs='+')
    a = ap.parse_args()
    for addr in a.addr:
        addr = addr.upper().replace('0X', '')
        insns = body(a.gen, addr)
        print('=== %s (%d instructions in the generated body)' % (addr, len(insns)))
        if not insns:
            print('  not found: not a function start the recompiler knows')
        for i, (pc, text) in enumerate(insns):
            print('%s  %s' % (pc, text))
            if not a.all and re.match(r'jr\s+\$ra', text):
                if i + 1 < len(insns):
                    print('%s  %s   (delay slot)' % insns[i + 1])
                break


if __name__ == '__main__':
    main()
