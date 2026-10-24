"""Read a GE frame dump (F12, or --ge-dump VBLANK: dumps/draws_<vblank>.txt).

Two views:
  seq   the frame as runs of draws (target, clear / 2D / 3D, texture), to see
        its passes (scene, bloom, HUD, final copy)
  hud   every draw after the second clear of the 3D target (PSP2i's HUD
        pass): screen box, clip w (the HUD layer: menus 3-70, field HUD
        84-85.9, labels 86-88, minimap 3D map ~2390), texture, vertex type

usage: python -I ge_frame.py seq|hud DUMP.txt [--fb 0x0CC000]
"""
import argparse
import re


def draws(path):
    cur = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for ln in f:
            if ln.startswith('ge-dump: prim'):
                if cur:
                    yield cur
                fb = re.search(r'fb (0x[0-9A-F]+)', ln)
                tex = re.search(r'tex (0x[0-9A-F]+)', ln)
                cur = {'line': ln.rstrip(), 'fb': fb.group(1) if fb else '?',
                       'clear': ' CLEAR' in ln, 'through': ' through' in ln,
                       'tex': tex.group(1) if tex else '-',
                       'vt': re.search(r'vt (0x[0-9A-F]+)', ln).group(1),
                       'prim': ln.split()[2], 'x': [], 'y': [], 'w': [], 'cz': []}
                continue
            if cur is None:
                continue
            m = re.search(r'screen \((\S+), (\S+), z (\d+)\).*clip \((\S+) (\S+) (\S+) w (\S+)\)', ln)
            if m:
                cur['x'].append(float(m.group(1)))
                cur['y'].append(float(m.group(2)))
                cur['cz'].append(float(m.group(6)))
                cur['w'].append(float(m.group(7)))
    if cur:
        yield cur


def seq(path):
    runs = []
    for d in draws(path):
        kind = 'CLEAR' if d['clear'] else ('2d' if d['through'] else '3d')
        key = (d['fb'], kind, d['tex'] if kind != '3d' else '')
        if runs and runs[-1][0] == key:
            runs[-1][1] += 1
        else:
            runs.append([key, 1])
    for (fb, kind, tex), n in runs:
        print('%5d  fb %s  %-5s %s' % (n, fb, kind, tex))


def hud(path, fb3d):
    clears = 0
    i = 0
    for d in draws(path):
        if d['clear'] and d['fb'] == fb3d:
            clears += 1
            continue
        if clears < 2 or d['fb'] != fb3d or not d['x']:
            continue
        print('%3d p%s vt %s %s tex %-10s x %6.1f-%6.1f y %6.1f-%6.1f w %8.2f-%8.2f cz %5.2f' % (
            i, d['prim'], d['vt'], 'THR' if d['through'] else '   ', d['tex'],
            min(d['x']), max(d['x']), min(d['y']), max(d['y']), min(d['w']), max(d['w']), d['cz'][0]))
        i += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=['seq', 'hud'])
    ap.add_argument('dump')
    ap.add_argument('--fb', default='0x0CC000', help="the 3D target (PSP2i's scene buffer)")
    a = ap.parse_args()
    if a.mode == 'seq':
        seq(a.dump)
    else:
        hud(a.dump, a.fb)


if __name__ == '__main__':
    main()
