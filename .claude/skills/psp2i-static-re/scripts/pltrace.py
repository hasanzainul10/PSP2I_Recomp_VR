"""Read a PSP2I_PL_TRACE file and search the player object for its facing.
usage: python -I pltrace.py TRACE [facing|dump OFF|buttons]"""
import sys, struct, math
import numpy as np

HDR = struct.Struct('<5I4B2I3f')
PL, CAM = 0x1000, 0x200

def load(path):
    data = open(path, 'rb').read()
    rec = HDR.size + PL + CAM
    n = len(data) // rec
    hdr, pl, cam = [], np.zeros((n, PL), np.uint8), np.zeros((n, CAM), np.uint8)
    for i in range(n):
        o = i * rec
        hdr.append(HDR.unpack_from(data, o))
        pl[i] = np.frombuffer(data, np.uint8, PL, o + HDR.size)
        cam[i] = np.frombuffer(data, np.uint8, CAM, o + HDR.size + PL)
    return hdr, pl, cam

def wrap(a):
    return (a + np.pi) % (2 * np.pi) - np.pi

def main():
    hdr, pl, cam = load(sys.argv[1])
    mode = sys.argv[2] if len(sys.argv) > 2 else 'facing'
    n = len(hdr)
    flips = np.array([h[1] for h in hdr])
    plad = np.array([h[2] for h in hdr])
    f32 = pl.view('<f4')
    pos = f32[:, 0x4B0 // 4: 0x4B0 // 4 + 3]
    print(f'{n} frames, flips {flips[0]}..{flips[-1]}, player objects {sorted(set(plad.tolist()))[:6]}')
    if mode == 'facing':
        k = 3
        ok = (plad[k:] == plad[:-k]) & (flips[k:] - flips[:-k] <= k + 2)
        d = pos[k:] - pos[:-k]
        sp = np.hypot(d[:, 0], d[:, 2])
        ok &= sp > 0.3
        mv = np.arctan2(d[:, 0], d[:, 2])          # moved direction, atan2(x, z)
        idx = np.nonzero(ok)[0] + k                # frame at the end of the move
        mv = mv[ok]
        print(f'{len(idx)} moving samples')
        res = []
        u16 = pl.view('<u2')
        for s in range(PL // 4):
            v = f32[idx, s].astype(np.float64)
            if not np.all(np.isfinite(v)) or np.ptp(v) < 1e-3:
                continue
            for name, a in (('rad', v), ('-rad', -v)):
                if np.max(np.abs(a)) > 50: continue
                e = wrap(a - mv)
                R = abs(np.mean(np.exp(1j * e)))
                res.append((R, f'+0x{s*4:03X} f32 {name} offset {math.degrees(np.angle(np.mean(np.exp(1j*e)))):.0f}'))
            if np.max(np.abs(v)) <= 1.01:
                for name, t in (('sin', np.sin(mv)), ('cos', np.cos(mv))):
                    c = np.corrcoef(v, t)[0, 1]
                    res.append((abs(c), f'+0x{s*4:03X} f32 ~{name} corr {c:+.3f}'))
        for s in range(PL // 2):
            v = u16[idx, s].astype(np.float64) * (2 * np.pi / 65536)
            if np.ptp(v) < 1e-3: continue
            for name, a in (('u16', v), ('-u16', -v)):
                e = wrap(a - mv)
                R = abs(np.mean(np.exp(1j * e)))
                res.append((R, f'+0x{s*2:03X} {name} offset {math.degrees(np.angle(np.mean(np.exp(1j*e)))):.0f}'))
        res.sort(reverse=True)
        for r, t in res[:25]:
            print(f'{r:.3f}  {t}')
    elif mode == 'dump':
        off = int(sys.argv[3], 0)
        step = int(sys.argv[4]) if len(sys.argv) > 4 else 1
        for i in range(0, n, step):
            h = hdr[i]
            print(f'{h[1]:6d} bits {h[4]:04X} st {h[5]-128:+4d},{h[6]-128:+4d} fp {h[9]} pos {pos[i,0]:8.2f} {pos[i,2]:8.2f} '
                  f'v {f32[i, off//4]:9.4f} w {pl[i, off:off+4].view("<u4")[0]:08X}')
    elif mode == 'changes':
        # slots that change when a button bit goes down vs frames without
        off0, off1 = 0, PL
        bits = np.array([h[4] for h in hdr])
        for b in range(16):
            down = np.nonzero(((bits[1:] >> b) & 1) & ~((bits[:-1] >> b) & 1))[0] + 1
            if len(down): print(f'bit {1<<b:04X}: {len(down)} presses at flips {flips[down][:12].tolist()}')

if __name__ == '__main__':
    main()
