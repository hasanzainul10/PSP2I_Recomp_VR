/* vr_test: the VR theater without the game. Shows a moving test picture on the
 * VR screen and prints the controllers as a PSP pad, so the OpenXR side
 * (loader, runtime, session, swapchain, bindings) can be checked on its own,
 * e.g. against SteamVR's null headset.
 *
 *   vr_test [seconds]        default 30; Ctrl+C ends it early */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "vr.h"

#define W 480
#define H 272

static uint32_t g_px[W * H];

/* Colour bars, a grid, a border and a bar that sweeps once a second: shows
 * scaling, colour (sRGB), the edges and that frames arrive. */
static void draw(int frame) {
    static const uint32_t BARS[8] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000 };
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t c = y < H / 2 ? BARS[x * 8 / W] : (uint32_t)((x * 255 / (W - 1)) * 0x010101u);   /* bars, then a grey ramp */
            if (x % 40 == 0 || y % 34 == 0) c = 0x404040;
            if (x == 0 || y == 0 || x == W - 1 || y == H - 1) c = 0xFF8000;
            g_px[y * W + x] = 0xFF000000u | c;
        }
    const int bx = (frame * 8) % W;
    for (int y = H / 2; y < H / 2 + 20; y++)
        for (int x = bx; x < bx + 8 && x < W; x++) g_px[y * W + x] = 0xFFFF2020u;
}

int main(int argc, char **argv) {
    const double seconds = argc > 1 ? atof(argv[1]) : 30.0;
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = '\0';
    vr_config cfg = { VR_DISTANCE_DEFAULT, VR_WIDTH_DEFAULT, VR_SCALE_DEFAULT };
    if (vr_start(dir, &cfg) != 0) return 1;
    const ULONGLONG t0 = GetTickCount64();
    int last_status = -1, frame = 0;
    vr_pad last = { 0xFFFFFFFFu, 0, 0, 0, 0, 0 };
    while (GetTickCount64() - t0 < (ULONGLONG)(seconds * 1000.0)) {
        draw(frame++);
        vr_submit_frame(g_px, W, H);
        const int s = vr_status();
        if (s != last_status) { printf("status: %s\n", vr_status_name(s)); fflush(stdout); last_status = s; }
        if (s == VR_NO_LOADER || s == VR_NO_RUNTIME || s == VR_FAILED) break;
        vr_pad p;
        const int live = vr_pad_get(&p);
        if (live && memcmp(&p, &last, sizeof p)) {
            printf("pad: buttons %04X  left %3u,%3u  right %3u,%3u  host %X\n", p.buttons, p.ax, p.ay, p.rx, p.ry, p.host);
            fflush(stdout);
            last = p;
        }
        Sleep(16);                                  /* about the game's 60 fps */
    }
    const int s = vr_status();
    vr_stop();
    printf("done (%d frames, last status %s)\n", frame, vr_status_name(s));
    return s == VR_RUNNING || s == VR_WAITING ? 0 : 1;
}
