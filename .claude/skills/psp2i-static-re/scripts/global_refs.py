"""Find the functions that address a range of globals (lui + offset).

The game reaches its globals as `lui $r, HI` then `lw/sw/lwc1/swc1/addiu`
with an offset from $r. This tracks the last `lui` per register inside each
function and reports the functions whose accesses land in [LO, HI).

Example (who reads the current projection 0x08FA97A0 .. 0x08FA97E0):
  python -I global_refs.py 08FA97A0 08FA97E0

usage: python -I global_refs.py [--gen port/gen/recomp_funcs.c] LO HI
"""
import argparse
import re

FN = re.compile(r'^static void psp_body_([0-9A-F]{8})\(')
LUI = re.compile(r'/\* ([0-9A-F]{8})  lui\s+\$(\w+), 0x([0-9A-F]+) \*/')
MEM = re.compile(r'/\* ([0-9A-F]{8})  (\w[\w.]*)\s+\$?(\w+), (-?\d+)\(\$(\w+)\) \*/')
ADDIU = re.compile(r'/\* ([0-9A-F]{8})  addiu\s+\$(\w+), \$(\w+), (-?\d+) \*/')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default='port/gen/recomp_funcs.c')
    ap.add_argument('lo')
    ap.add_argument('hi')
    a = ap.parse_args()
    lo, hi = int(a.lo, 16), int(a.hi, 16)
    func, lui, hits = None, {}, {}
    with open(a.gen, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = FN.match(line)
            if m:
                func, lui = m.group(1), {}
                continue
            m = LUI.search(line)
            if m:
                lui[m.group(2)] = int(m.group(3), 16) << 16
                continue
            m = MEM.search(line)
            if m and m.group(5) in lui:
                addr = (lui[m.group(5)] + int(m.group(4))) & 0xFFFFFFFF
                if lo <= addr < hi:
                    hits.setdefault(func, []).append('%s %s:0x%08X' % (m.group(1), m.group(2), addr))
                continue
            m = ADDIU.search(line)
            if m and m.group(3) in lui:
                addr = (lui[m.group(3)] + int(m.group(4))) & 0xFFFFFFFF
                if lo <= addr < hi:
                    hits.setdefault(func, []).append('%s addiu:0x%08X' % (m.group(1), addr))
    for fn in sorted(hits):
        print('%s %d  %s' % (fn, len(hits[fn]), '  '.join(hits[fn][:6])))


if __name__ == '__main__':
    main()
