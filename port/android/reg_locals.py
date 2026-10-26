"""Keep the MIPS registers of chosen game functions in C locals.

The generated code names the registers r_v0, r_sp ... (recomp_funcs.h:
psp_cpu.r[n]), so every use is a load or store of the global psp_cpu. A
compiler must reload them after every call it can't see into (on the PC each
guest load and store is one) and, with inline memory access, after every
guest store (a byte store may alias psp_cpu). Locals whose address is never
taken stay in host registers across both. 3DS_PLAN.md, "T4 speed", O3.

In each chosen psp_body_XXXXXXXX:
  - r_<name> becomes the local x_<name>, loaded from psp_cpu.r at entry
    ($zero: the constant 0);
  - a call to game code or the HLE (psp_func_*, psp_at_*, psp_import_*,
    psp_dispatch, psp_syscall, psp_hle_call, psp_unimplemented,
    psp_park_thread) writes the locals back first and reloads them after:
    the callee reads and writes psp_cpu, and may switch PSP threads;
  - a return, and the end of the body, write them back.
The helpers the body calls otherwise (psp_sll, psp_lwl, memory access, the
FPU and VFPU ones) take values or touch only hi, lo, f[] and v[], which stay
in psp_cpu.

  python reg_locals.py <in.c> <out.c> <list>    (out may be in)
transforms the bodies of the functions in <list> (one PSP address per line,
# comments: port/3ds/hot_functions.txt). split_gen.py uses transform_body.
"""

import re
import sys

REGS = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3',
        't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
        's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7',
        't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']
REG_RE = re.compile(r'\br_(' + '|'.join(REGS) + r')\b')
CALL_RE = re.compile(r'\b(psp_(?:func|at|import)_[0-9A-F]{8}|psp_dispatch|psp_syscall|'
                     r'psp_hle_call|psp_unimplemented|psp_park_thread)\(')
RET_RE = re.compile(r'\breturn;')
BODY_RE = re.compile(r'^(?:static )?void psp_body_([0-9A-F]{8})\(uint32_t _entry\) \{$')


def load_list(path):
    out = set()
    for line in open(path, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if line:
            out.add(int(line, 16))
    return out


def wrap_calls(line):
    """Each call statement in the line as { FL_ call; RL_ }."""
    out, i = [], 0
    while True:
        m = CALL_RE.search(line, i)
        if not m:
            out.append(line[i:])
            return ''.join(out)
        depth, j = 0, m.end() - 1
        while j < len(line):
            if line[j] == '(':
                depth += 1
            elif line[j] == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        k = j + 1
        while k < len(line) and line[k] == ' ':
            k += 1
        if j >= len(line) or k >= len(line) or line[k] != ';':
            raise SystemExit('reg_locals: a call that is not a statement: ' + line.strip())
        out.append(line[i:m.start()])
        out.append('{ FL_ ' + line[m.start():k + 1] + ' RL_ }')
        i = k + 1


def transform_body(lines):
    """lines: the body from its opening line to its closing '}' (inclusive).
    Returns the new lines, with the FL_ / RL_ macros defined around it."""
    used = set()
    for line in lines[1:]:
        used.update(REG_RE.findall(line))
    regs = [r for r in REGS if r in used and r != 'zero']
    body = []
    for line in lines[1:-1]:
        line = REG_RE.sub(lambda m: 'x_' + m.group(1), line)
        line = wrap_calls(line)
        line = RET_RE.sub('{ FL_ return; }', line)
        body.append(line)
    idx = {r: REGS.index(r) for r in regs}
    fl = ' '.join('psp_cpu.r[%d] = x_%s;' % (idx[r], r) for r in regs)
    rl = ' '.join('x_%s = psp_cpu.r[%d];' % (r, idx[r]) for r in regs)
    out = ['#define FL_ ' + (fl or '(void)0;'), '#define RL_ ' + (rl or '(void)0;'), lines[0]]
    if regs:
        out.append('    uint32_t ' + ', '.join('x_%s = psp_cpu.r[%d]' % (r, idx[r]) for r in regs) + ';')
    if 'zero' in used:
        out.append('    const uint32_t x_zero = 0u;')
    out += body
    out.append('    FL_')
    out.append(lines[-1])
    out += ['#undef FL_', '#undef RL_']
    return out


def transform_lines(lines, chosen):
    """All of a file's lines, the chosen functions' bodies transformed."""
    out, i, n = [], 0, 0
    while i < len(lines):
        m = BODY_RE.match(lines[i].rstrip('\r'))
        if not m or int(m.group(1), 16) not in chosen:
            out.append(lines[i])
            i += 1
            continue
        j = i + 1
        while lines[j].rstrip('\r') != '}':
            j += 1
        out += transform_body(lines[i:j + 1])
        n += 1
        i = j + 1
    return out, n


def main():
    src, dst, lst = sys.argv[1], sys.argv[2], sys.argv[3]
    lines = open(src, encoding='utf-8').read().split('\n')
    out, n = transform_lines(lines, load_list(lst))
    open(dst, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    print('reg_locals: %d bodies' % n)


if __name__ == '__main__':
    main()
