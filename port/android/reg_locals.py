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
FPU and VFPU ones) take values or touch only hi, lo, f[] and v[].

With vregs (split_gen.py's `vlocals`), the vector registers the body names
(psp_cpu.v[N], always a constant N: the recompiler resolves the lanes)
become float locals xv_N the same way. They
are written back and reloaded around the calls above and around each VFPU
runtime call that is left (psp_v*, psp_lv_* / psp_sv_*: these read and write
psp_cpu.v; after the recompiler's prefix proof most are dead fallbacks).
Three refinements (3DS_PROGRESS.md T4, "the game's thread"):
  - a call to a game function that never touches the VFPU (vfpu_free: its
    body and everything it calls, worked out over the whole file) keeps the
    vector locals: no write-back or reload around it;
  - a call followed directly by `return;` (a tail call) doesn't reload the
    locals, and the return doesn't write them back again;
  - in a body with one entry, vector registers assigned before any read,
    call, branch, label or return aren't loaded at entry -- only where
    PSP_VFPU_NOPFX_CONST says the fallbacks that would store them are dead.

The RAM base pointers of the inline memory access (mem.h, PSP_MEM_RD_BASE /
PSP_MEM_WR_BASE) are locals too, x_mrd / x_mwr: a guest store is a char
write the compiler must assume can change any global, the pointers included;
they only change inside mem.c (watches, module mapping), reached through the
game / HLE calls above, after which they are loaded again.

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
VCALL_RE = re.compile(r'\b(psp_v(?!fpu_set_prefix\b)\w+|psp_[ls]v_[sq])\(')
VREG_RE = re.compile(r'\bpsp_cpu\.v\[(\d+)\]')
BODY_RE = re.compile(r'^(?:static )?void psp_body_([0-9A-F]{8})\(uint32_t _entry\) \{$')


def load_list(path):
    out = set()
    for line in open(path, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if line:
            out.add(int(line, 16))
    return out


def next_call(line, i, vregs):
    """The next game / HLE call (True) or VFPU runtime call (False, only with
    vregs) from i."""
    a, b = CALL_RE.search(line, i), VCALL_RE.search(line, i) if vregs else None
    if a and (not b or a.start() <= b.start()):
        return a, True
    return b, False


def wrap_calls(line, vregs, vfree=frozenset(), tail=False):
    """Each call statement in the line as { FL_ FLV_ call; RL_ RLV_ } (a game
    or HLE call), or { FLV_ call; RLV_ } (a VFPU runtime call). A game call to
    a function in vfree leaves out FLV_ / RLV_; with tail (the next statement
    is the return), a game call doesn't reload anything after it."""
    out, i = [], 0
    while True:
        m, game = next_call(line, i, vregs)
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
        if game and tail:                            # everything back before it; nothing after
            out.append('{ FL_ FLV_ ' + line[m.start():k + 1] + ' }')
        elif game:
            keep_v = m.group(1) in vfree
            out.append('{ FL_ ' + ('' if keep_v else 'FLV_ ') + line[m.start():k + 1] +
                       ' RL_' + ('' if keep_v else ' RLV_') + ' }')
        else:
            out.append('{ FLV_ ' + line[m.start():k + 1] + ' RLV_ }')
        i = k + 1


ASSIGN_V_RE = re.compile(r'\bpsp_cpu\.v\[(\d+)\]\s*=(?!=)')
ELSE_RE = re.compile(r'\} else .*')
SPCALL_SP_RE = re.compile(r'(PSP_SP_CALL\([^;]*?, )x_sp\)')
# an unconditional call statement, as emit.c writes them
TAIL_RE = re.compile(r'^(\{ uint32_t _spc = r_sp; )?psp_(?:func|at)_[0-9A-F]{8}\(\);( PSP_SP_CALL\([^;]*\); \})?$')


def written_first(lines):
    """Vector registers a single-entry body assigns before reading them, with
    no call, branch, label or return before: they needn't be loaded at entry.
    (Dead fallbacks -- after `} else` -- are ignored: the caller only uses
    this where PSP_VFPU_NOPFX_CONST says they are dead.)"""
    if any(re.match(r'\s+case 0x[0-9A-F]+u: goto L_', l) for l in lines):
        return set()
    first = {}
    for line in lines[1:-1]:
        live = ELSE_RE.sub('', line)
        st = live.strip()
        if st.startswith('/*') or not st:
            continue
        if (re.match(r'L_[0-9A-F]+:', st) and first) or 'goto ' in live or 'return' in live or \
                CALL_RE.search(live) or VCALL_RE.search(live) or 'if (' in live.replace('if (PSP_VFPU_NOPFX)', ''):
            break
        for m in re.finditer(r'psp_cpu\.v\[(\d+)\](\s*=(?!=))?', live):
            first.setdefault(int(m.group(1)), 'w' if m.group(2) else 'r')
    return {v for v, k in first.items() if k == 'w'}


def transform_body(lines, vregs=False, vfree=frozenset()):
    """lines: the body from its opening line to its closing '}' (inclusive).
    Returns the new lines, with the FL_ / RL_ macros defined around it."""
    used, vused = set(), set()
    for line in lines[1:]:
        used.update(REG_RE.findall(line))
        if vregs:
            vused.update(int(n) for n in VREG_RE.findall(line))
    regs = [r for r in REGS if r in used and r != 'zero']
    vregs = sorted(vused)
    wfirst = written_first(lines) if vregs else set()
    body = []
    inner = lines[1:-1]
    after_tail = False
    for k, line in enumerate(inner):
        # a tail call: an unconditional call statement, the next line the return
        tail = (k + 1 < len(inner) and inner[k + 1].strip() == 'return;' and len(CALL_RE.findall(line)) == 1 and
                TAIL_RE.match(line.strip()) is not None)
        line = REG_RE.sub(lambda m: 'x_' + m.group(1), line)
        if vregs:
            line = VREG_RE.sub(lambda m: 'xv_' + m.group(1), line)
        if tail:
            line = SPCALL_SP_RE.sub(r'\1psp_cpu.r[29])', line)   # the stack check reads the callee's sp
        line = wrap_calls(line, vregs, vfree, tail)
        line = RET_RE.sub('return;' if after_tail else '{ FL_ FLV_ return; }', line)
        after_tail = tail
        body.append(line)
    idx = {r: REGS.index(r) for r in regs}
    fl = ' '.join('psp_cpu.r[%d] = x_%s;' % (idx[r], r) for r in regs)
    rl = ' '.join('x_%s = psp_cpu.r[%d];' % (r, idx[r]) for r in regs)
    flv = ' '.join('psp_cpu.v[%d] = xv_%d;' % (v, v) for v in vregs)
    rlv = ' '.join('xv_%d = psp_cpu.v[%d];' % (v, v) for v in vregs)
    rl += ' x_mrd = psp_mem_fast_rd; x_mwr = psp_mem_fast_wr;'
    out = ['#define FL_ ' + (fl or '(void)0;'), '#define RL_ ' + rl.strip(),
           '#define FLV_ ' + (flv or '(void)0;'), '#define RLV_ ' + (rlv or '(void)0;'),
           '#undef PSP_MEM_RD_BASE', '#undef PSP_MEM_WR_BASE',
           '#define PSP_MEM_RD_BASE x_mrd', '#define PSP_MEM_WR_BASE x_mwr', lines[0]]
    if regs:
        out.append('    uint32_t ' + ', '.join('x_%s = psp_cpu.r[%d]' % (r, idx[r]) for r in regs) + ';')
    out.append('    uint8_t *x_mrd = psp_mem_fast_rd, *x_mwr = psp_mem_fast_wr;')
    loaded = [v for v in vregs if v not in wfirst]
    if wfirst:
        out.append('#if PSP_VFPU_NOPFX_CONST')
        out.append('    float ' + ', '.join('xv_%d' % v for v in sorted(wfirst)) + ';   /* assigned before read */')
        out.append('#else')
        out.append('    float ' + ', '.join('xv_%d = psp_cpu.v[%d]' % (v, v) for v in sorted(wfirst)) + ';')
        out.append('#endif')
    if loaded:
        out.append('    float ' + ', '.join('xv_%d = psp_cpu.v[%d]' % (v, v) for v in loaded) + ';')
    if 'zero' in used:
        out.append('    const uint32_t x_zero = 0u;')
    out += body
    out.append('    FL_ FLV_')
    out.append(lines[-1])
    out += ['#undef FL_', '#undef RL_', '#undef FLV_', '#undef RLV_',
            '#undef PSP_MEM_RD_BASE', '#undef PSP_MEM_WR_BASE',
            '#define PSP_MEM_RD_BASE psp_mem_fast_rd', '#define PSP_MEM_WR_BASE psp_mem_fast_wr']
    return out


def transform_lines(lines, chosen, vregs=False, vfree=frozenset()):
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
        out += transform_body(lines[i:j + 1], vregs, vfree)
        n += 1
        i = j + 1
    return out, n


# Helpers that read and write only their arguments, guest memory, or the
# integer / FPU state (recomp_rt.h, mem.h): a body that calls nothing else
# (and no game function that does) never reads or writes psp_cpu.v.
PURE = {'psp_read8', 'psp_read16', 'psp_read32', 'psp_read_f32', 'psp_write8', 'psp_write16', 'psp_write32',
        'psp_write_f32', 'psp_sll', 'psp_srl', 'psp_sra', 'psp_slt', 'psp_sltu', 'psp_lwl', 'psp_lwr', 'psp_swl',
        'psp_swr', 'psp_ins', 'psp_ext', 'psp_mult', 'psp_multu', 'psp_msub', 'psp_msubu', 'psp_madd', 'psp_maddu',
        'psp_div', 'psp_divu', 'psp_seb', 'psp_seh', 'psp_rotr', 'psp_min', 'psp_max', 'psp_bits_to_f32',
        'psp_f32_to_bits', 'psp_fpu_set_cond', 'psp_fcmp', 'psp_fpu_cond', 'psp_fabs', 'psp_f2w', 'psp_clz', 'psp_clo',
        'psp_wsbh', 'psp_wsbw', 'psp_bitrev'}
NAME_RE = re.compile(r'\b(psp_[a-z][a-z0-9_]*)\(')
GAME_RE = re.compile(r'^psp_(?:func|at)_([0-9A-F]{8})$')
AT_RE = re.compile(r'^void psp_at_([0-9A-F]{8})\(void\) \{ psp_body_([0-9A-F]{8})\(')


def vfpu_free(lines):
    """The names (psp_func_X / psp_at_X) of the game functions whose bodies,
    and everything they call, never touch the VFPU, the HLE or a computed
    target."""
    bodies, owner, i = {}, {}, 0
    while i < len(lines):
        m = BODY_RE.match(lines[i].rstrip('\r'))
        if m:
            j = i + 1
            while lines[j].rstrip('\r') != '}':
                j += 1
            bodies[m.group(1)] = lines[i + 1:j]
            i = j + 1
            continue
        a = AT_RE.match(lines[i])
        if a:
            owner[a.group(1)] = a.group(2)
        i += 1
    calls, ok = {}, set()
    for addr, body in bodies.items():
        callees, clean = set(), True
        for l in body:
            if 'psp_cpu.v[' in l or 'psp_scc_next' in l:
                clean = False
                break
            for n in NAME_RE.findall(l):
                g = GAME_RE.match(n)
                if g:
                    callees.add(owner.get(g.group(1), g.group(1)) if n.startswith('psp_at_') else g.group(1))
                elif n not in PURE and not n.startswith('psp_body_'):
                    clean = False
            if not clean:
                break
        if clean:
            calls[addr] = callees
            ok.add(addr)
    changed = True
    while changed:                                   # the largest set closed under calls
        changed = False
        for addr in list(ok):
            if any(c not in ok for c in calls[addr]):
                ok.discard(addr)
                changed = True
    names = {'psp_func_' + a for a in ok}
    names.update('psp_at_' + at for at, b in owner.items() if b in ok)
    return frozenset(names)


def main():
    src, dst, lst = sys.argv[1], sys.argv[2], sys.argv[3]
    lines = open(src, encoding='utf-8').read().split('\n')
    out, n = transform_lines(lines, load_list(lst))
    open(dst, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    print('reg_locals: %d bodies' % n)


if __name__ == '__main__':
    main()
