"""List the call sites of a function in port/gen/recomp_funcs.c.

For each `jal ADDR` (or `j ADDR`, a tail call) it prints the call's address
and the function that contains it.

usage: python -I callers.py [--gen port/gen/recomp_funcs.c] ADDR [ADDR...]
"""
import argparse
import re

FN = re.compile(r'^static void psp_body_([0-9A-F]{8})\(')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default='port/gen/recomp_funcs.c')
    ap.add_argument('addr', nargs='+')
    a = ap.parse_args()
    targets = {x.upper().replace('0X', '') for x in a.addr}
    pat = re.compile(r'/\* ([0-9A-F]{8})  (jal|j)\s+0x([0-9A-F]{8}) \*/')
    func = None
    hits = {t: [] for t in targets}
    with open(a.gen, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = FN.match(line)
            if m:
                func = m.group(1)
                continue
            m = pat.search(line)
            if m and m.group(3) in targets:
                hits[m.group(3)].append((m.group(1), m.group(2), func))
    for t in sorted(targets):
        print('=== %s: %d call sites' % (t, len(hits[t])))
        for pc, op, fn in hits[t]:
            print('  %s %-3s in %s' % (pc, op, fn))


if __name__ == '__main__':
    main()
