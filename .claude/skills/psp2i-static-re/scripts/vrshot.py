"""Look at VR captures (F11, or PSP2I_VR_SHOT_EVERY=N: <capture dir>/vrshot_<vblank>*).

For each capture: the two eye pictures side by side as vrshot_<vblank>.png
(half size unless --full), and a summary of the report: the head pose, the
HUD frame, the settings, and the frame's traced draws grouped by what the
eyes did with them (hud / shrink / same), with the scissors that moved.

usage: python -I vrshot.py [--full] [--draws] CAPTURE_DIR [VBLANK...]
"""
import argparse
import glob
import os
import re

from PIL import Image


def pictures(d, vb, full):
    l, r, h = (os.path.join(d, 'vrshot_%s_%s.ppm' % (vb, e)) for e in 'LRH')
    if not (os.path.exists(l) and os.path.exists(r)):
        return None
    pics = [Image.open(l), Image.open(r)]
    if os.path.exists(h):                     # the HUD panel's picture (over dark grey)
        pics.append(Image.open(h))
    out = Image.new('RGB', (sum(p.width for p in pics) + 8 * (len(pics) - 1), max(p.height for p in pics)), (255, 0, 255))
    x = 0
    for p in pics:
        out.paste(p, (x, 0))
        x += p.width + 8
    if not full:
        out = out.resize((out.width // 2, out.height // 2))
    path = os.path.join(d, 'vrshot_%s.png' % vb)
    out.save(path)
    return path


def report(d, vb, show_draws):
    path = os.path.join(d, 'vrshot_%s.txt' % vb)
    if not os.path.exists(path):
        return
    kinds, lines, moved_sc = {}, [], 0
    with open(path, encoding='utf-8', errors='replace') as f:
        for ln in f:
            m = re.match(r'\s+#\d+\s+(\w+)', ln)
            if m:
                kinds[m.group(1)] = kinds.get(m.group(1), 0) + 1
                sc = re.search(r'scissor (\S+) -> (\S+)', ln)
                if sc and sc.group(1) != sc.group(2):
                    moved_sc += 1
                if show_draws or (sc and sc.group(1) != sc.group(2)):
                    lines.append(ln.rstrip())
                continue
            if not ln.startswith('  '):
                print('  ' + ln.rstrip())
    print('  draws by kind: %s; scissors moved: %d' % (kinds, moved_sc))
    for ln in lines[:200]:
        print('  ' + ln)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--full', action='store_true', help='full-size pictures')
    ap.add_argument('--draws', action='store_true', help='list every traced draw')
    ap.add_argument('dir')
    ap.add_argument('vblank', nargs='*')
    a = ap.parse_args()
    vbs = a.vblank or sorted({re.match(r'vrshot_(\d+)', os.path.basename(p)).group(1)
                              for p in glob.glob(os.path.join(a.dir, 'vrshot_*.txt'))})
    for vb in vbs:
        vb = '%06d' % int(vb)
        print('=== vrshot %s' % vb)
        p = pictures(a.dir, vb, a.full)
        print('  picture: %s' % (p or 'no eye pictures'))
        report(a.dir, vb, a.draws)


if __name__ == '__main__':
    main()
