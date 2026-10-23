"""The local player's skeleton from a PSP2I_SKEL dump (port/vr/vr_body.c).

usage: skeleton.py DUMP [--snap N]

Each snapshot holds every node matrix of the player's model (4x4, VFPU
columns, model space: from the feet, as the game animates it). This prints:
  - each bone's position in model space, averaged over the snapshots, with how far it moves;
  - its parent: the bone in whose frame its offset stays the same through
    the animation (rigid bones keep their length), with that offset's
    length and its spread (near 0 = a real parent);
  - the chains from the root, so the head, arms and legs can be read off.
"""
import sys
import numpy as np


def load(path):
    snaps, cur = [], None
    with open(path) as f:
        for line in f:
            t = line.split()
            if not t:
                continue
            if t[0] == 'snap':
                cur = {'vb': int(t[1]), 'obj': t[3], 'n': int(t[7]), 'bones': []}
                snaps.append(cur)
            elif t[0] == 'view':
                cur['view'] = np.array([float(x) for x in t[1:17]]).reshape(4, 4).T   # columns -> rows
            elif t[0] == 'feet':
                v = [float(x) for x in t[1:4] + t[5:8] + t[9:12]]
                cur['feet'], cur['x'], cur['z'] = np.array(v[0:3]), np.array(v[3:6]), np.array(v[6:9])
            elif t[0] == 'b':
                cur['bones'].append(np.array([float(x) for x in t[2:18]]).reshape(4, 4).T)
    return [s for s in snaps if len(s['bones']) == s['n']]


def records(path):
    """The skeleton's node records (rec lines, the first set): word 1 =
    parent << 16 | palette slot (0xFFFF = none), word 2 = next sibling << 16 |
    first child (0xFFFF = none), words 3-5 = the rest offset from the parent
    (floats), 6-8 = rest rotation (16-bit angles), 9-11 = scale."""
    import struct
    recs = {}
    with open(path) as f:
        for line in f:
            t = line.split()
            if t and t[0] == 'rec':
                i = int(t[1])
                if i in recs:
                    break
                w = [int(x, 16) for x in t[2:]]
                sh = lambda v: v - 0x10000 if v >= 0x8000 else v
                off = [struct.unpack('<f', struct.pack('<I', x))[0] for x in w[3:6]]
                recs[i] = {'parent': sh(w[1] >> 16), 'sibling': sh(w[2] >> 16), 'child': sh(w[2] & 0xFFFF),
                           'offset': off, 'length': sum(x * x for x in off) ** 0.5}
    return recs


def body_pos(s, m):
    if 'view' not in s:                   # node matrices: already model space
        return m[:3, 3]
    w = np.linalg.inv(s['view']) @ m[:, 3]
    d = w[:3] - s['feet']
    return np.array([d @ s['x'], d[1], d @ s['z']])


def main():
    path = sys.argv[1]
    snaps = load(path)
    if not snaps:
        sys.exit('no complete snapshots')
    n = snaps[0]['n']
    snaps = [s for s in snaps if s['n'] == n]
    print('%d snapshots, %d bones, model %s' % (len(snaps), n, snaps[0]['obj']))
    pos = np.array([[body_pos(s, s['bones'][b]) for b in range(n)] for s in snaps])   # snap, bone, xyz
    mean, spread = pos.mean(0), pos.std(0).max(1)
    # Parent: the bone whose frame keeps this one's origin fixed.
    par, plen, pspr = [-1] * n, [0.0] * n, [0.0] * n
    for c in range(n):
        best = None
        for p in range(n):
            if p == c:
                continue
            loc = np.array([(np.linalg.inv(s['bones'][p]) @ s['bones'][c][:, 3])[:3] for s in snaps])
            sp = loc.std(0).max()
            ln = np.linalg.norm(loc.mean(0))
            # a parent is rigid to it and near it; prefer the nearer of equals
            score = sp + 0.02 * ln
            if best is None or score < best[0]:
                best = (score, p, ln, sp)
        par[c], plen[c], pspr[c] = best[1], best[2], best[3]
    print('\nbone   body x     y     z   moves | parent  length  spread')
    for b in range(n):
        print('%4d  %6.2f %6.2f %6.2f  %5.2f | %4d   %6.3f  %.4f' % (
            b, mean[b][0], mean[b][1], mean[b][2], spread[b], par[b], plen[b], pspr[b]))
    recs = records(path)
    if recs:
        print("\nnode records (the game's own tree): node <- parent, rest length")
        for b in sorted(recs):
            r = recs[b]
            print('  %2d <- %2d  %.3f  (offset %.3f %.3f %.3f)' % (b, r['parent'], r['length'], *r['offset']))
        par = [recs[b]['parent'] if b in recs else par[b] for b in range(n)]
    # Chains: follow parents up from each leaf (a bone nobody calls parent).
    kids = {b: [c for c in range(n) if par[c] == b] for b in range(n)}
    print('\nleaf chains (leaf <- ... <- root):')
    for b in range(n):
        if kids[b]:
            continue
        chain, x, seen = [b], par[b], {b}
        while x >= 0 and x not in seen:
            chain.append(x)
            seen.add(x)
            x = par[x]
        print('  ' + ' <- '.join(str(c) for c in chain) + '   (leaf at %.2f %.2f %.2f)' % tuple(mean[b]))


if __name__ == '__main__':
    main()
