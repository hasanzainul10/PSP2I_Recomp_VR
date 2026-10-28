/* Movies on the 3DS: H.264 for sceMpeg (psp_mpeg_set_video_codec) on the New
 * 3DS's hardware decoder, mvd:STD (libctru's mvdstd*).
 *
 * The runtime demultiplexes the PSMF stream and hands over one access unit
 * (Annex B) at a time, then asks again (up to 16 units) until a picture comes
 * out, so a decoder that holds pictures back works. Each NAL unit is copied
 * into a linear-memory buffer behind a 3-byte 00 00 01 (what MVD takes) and
 * processed; when MVD says a frame is ready it is rendered as YUYV 4:2:2 into
 * a second linear buffer, and converted here to the planar 4:2:0 the runtime
 * takes (psp_video_frame). The pts handed out is the smallest of the units
 * not yet matched to a picture: pictures come out in display order, which is
 * increasing pts.
 *
 * MVD's work buffer (~9 MB) is linear memory the GE backend's textures
 * otherwise use, so it is taken only while a movie is open, and the movie
 * plays black (with sound) when there isn't room for it. Old 3DS: no MVD, no
 * codec set. PSP movies are 480x272 (17 macroblock rows: no cropping). */
#include "3ds_video.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psprecomp/hle.h>

#define VID_W     480
#define VID_H     272
#define IN_SIZE   (512 * 1024)          /* the largest NAL unit taken */
#define NPTS      16
#define LINEAR_SPARE (2u << 20)         /* left for the GE backend after MVD's buffers */

typedef struct {
    uint8_t *in;                        /* linear: 00 00 01 + one NAL unit */
    uint8_t *out;                       /* linear: the rendered picture, YUYV */
    uint8_t *y, *u, *v;                 /* the picture handed out, 4:2:0 */
    MVDSTD_Config cfg;
    int64_t pts[NPTS];
    int npts;
    int64_t last_in;                    /* the newest unit's pts */
    unsigned pictures, nals, errors, units, readies, drained, repeated;
} vdec;

static int g_open;                      /* MVD has one session: one movie at a time */

static void *vid_open(void) {
    if (g_open) return NULL;
    const u32 before = linearSpaceFree();
    if (before < MVD_DEFAULT_WORKBUF_SIZE + IN_SIZE + VID_W * VID_H * 2 + LINEAR_SPARE) {
        fprintf(stderr, "video: %lu KiB of linear memory free, MVD needs ~%lu: this movie plays black\n",
                (unsigned long)(before >> 10), (unsigned long)((MVD_DEFAULT_WORKBUF_SIZE + IN_SIZE) >> 10));
        return NULL;
    }
    vdec *d = (vdec *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->in = (uint8_t *)linearAlloc(IN_SIZE);
    d->out = (uint8_t *)linearAlloc(VID_W * VID_H * 2);
    d->y = (uint8_t *)malloc(VID_W * VID_H + 2 * (VID_W / 2) * (VID_H / 2));
    if (!d->in || !d->out || !d->y) goto fail;
    d->u = d->y + VID_W * VID_H;
    d->v = d->u + (VID_W / 2) * (VID_H / 2);
    Result rc = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_YUYV422, MVD_DEFAULT_WORKBUF_SIZE, NULL);
    if (R_FAILED(rc)) {
        fprintf(stderr, "video: mvdstdInit failed (0x%08lX): this movie plays black\n", (unsigned long)rc);
        goto fail;
    }
    mvdstdGenerateDefaultConfig(&d->cfg, VID_W, VID_H, VID_W, VID_H, NULL, (u32 *)d->out, (u32 *)d->out);
    g_open = 1;
    fprintf(stderr, "video: MVD open, linear memory free %lu -> %lu KiB\n",
            (unsigned long)(before >> 10), (unsigned long)(linearSpaceFree() >> 10));
    return d;
fail:
    if (d->in) linearFree(d->in);
    if (d->out) linearFree(d->out);
    free(d->y);
    free(d);
    return NULL;
}

/* The picture MVD rendered, YUYV 4:2:2, to planar 4:2:0 (each pair of rows
 * takes the upper row's chroma). */
static void to_420(vdec *d) {
    for (int y = 0; y < VID_H; y++) {
        const uint8_t *s = d->out + (size_t)y * VID_W * 2;
        uint8_t *py = d->y + (size_t)y * VID_W;
        for (int x = 0; x < VID_W; x++) py[x] = s[x * 2];
        if (y & 1) continue;
        uint8_t *pu = d->u + (size_t)(y / 2) * (VID_W / 2), *pv = d->v + (size_t)(y / 2) * (VID_W / 2);
        for (int x = 0; x < VID_W / 2; x++) { pu[x] = s[x * 4 + 1]; pv[x] = s[x * 4 + 3]; }
    }
}

/* The smallest pts not yet matched to a picture: display order. */
static int64_t take_pts(vdec *d) {
    if (!d->npts) return 0;
    int b = 0;
    for (int i = 1; i < d->npts; i++) if (d->pts[i] < d->pts[b]) b = i;
    const int64_t p = d->pts[b];
    d->pts[b] = d->pts[--d->npts];
    return p;
}

static int picture(vdec *d, psp_video_frame *out) {
    if (R_FAILED(mvdstdRenderVideoFrame(&d->cfg, true))) { d->errors++; return 0; }
    GSPGPU_InvalidateDataCache(d->out, VID_W * VID_H * 2);
    to_420(d);
    out->width = VID_W; out->height = VID_H;
    out->y = d->y; out->u = d->u; out->v = d->v;
    out->ystride = VID_W; out->uvstride = VID_W / 2;
    out->pts = take_pts(d);
    d->pictures++;
    /* every 300 pictures: the pts handed out against the newest unit's
     * (pictures come out one unit late: -33 ms, steady, on the console) */
    if (d->pictures % 300 == 0)
        fprintf(stderr, "video: picture %u pts %lld, newest unit %lld (%+lld ms), %d units waiting, %u units %u NALs\n",
                d->pictures, (long long)out->pts, (long long)d->last_in, (long long)(out->pts - d->last_in) / 90,
                d->npts, d->units, d->nals);
    return 1;
}

static int vid_decode(void *p, const uint8_t *au, int size, int64_t pts, psp_video_frame *out) {
    vdec *d = (vdec *)p;
    if (!au) {
        /* The end. MVD holds one picture back (each comes out one unit
         * late), and the game waits for the last one: without it the movie
         * ended on a black screen until a button was pressed (console,
         * 2026-10-29: 2,507 units, 2,506 pictures). An end-of-stream NAL
         * asks MVD for it; if none comes, the last picture again, with the
         * held unit's pts, so every unit has its picture. */
        if (!d->npts) return 0;
        static const uint8_t EOS[4] = { 0, 0, 1, 11 };
        memcpy(d->in, EOS, sizeof EOS);
        GSPGPU_FlushDataCache(d->in, sizeof EOS);
        if (mvdstdProcessVideoFrame(d->in, sizeof EOS, 0, NULL) == MVD_STATUS_FRAMEREADY) { d->drained++; return picture(d, out); }
        if (!d->pictures) { d->npts = 0; return 0; }
        out->width = VID_W; out->height = VID_H;
        out->y = d->y; out->u = d->u; out->v = d->v;
        out->ystride = VID_W; out->uvstride = VID_W / 2;
        out->pts = take_pts(d);
        d->repeated++;
        return 1;
    }
    if (d->npts == NPTS) take_pts(d);   /* units that never gave a picture */
    d->pts[d->npts++] = pts;
    d->last_in = pts;
    d->units++;
    int got = 0;
    /* each NAL unit, between start codes (00 00 01 or 00 00 00 01) */
    int i = 0;
    while (i + 3 <= size && !(au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1)) i++;
    while (i + 3 <= size) {
        const int start = i + 3;
        int end = start;
        while (end + 3 <= size && !(au[end] == 0 && au[end + 1] == 0 && au[end + 2] == 1)) end++;
        if (end + 3 > size) end = size;
        int nal_end = end;
        while (nal_end > start && au[nal_end - 1] == 0 && end < size) nal_end--;   /* a 4-byte start code's leading 0 */
        const int n = nal_end - start, type = n > 0 ? au[start] & 0x1F : 0;
        /* access-unit delimiters, SEI and filler carry no picture */
        if (n > 0 && type != 9 && type != 6 && type != 12) {
            if (n + 3 > IN_SIZE) { d->errors++; }
            else {
                d->in[0] = 0; d->in[1] = 0; d->in[2] = 1;
                memcpy(d->in + 3, au + start, (size_t)n);
                GSPGPU_FlushDataCache(d->in, (u32)n + 3);
                const Result rc = mvdstdProcessVideoFrame(d->in, (size_t)n + 3, 0, NULL);
                d->nals++;
                if (!MVD_CHECKNALUPROC_SUCCESS(rc)) {
                    if (d->errors++ < 8) fprintf(stderr, "video: MVD refused NAL type %d (%d bytes): 0x%08lX\n", type, n, (unsigned long)rc);
                    if (d->errors > 64 && !d->pictures) return -1;      /* never decodes: black from here */
                } else if (rc == MVD_STATUS_FRAMEREADY) {
                    d->readies++;                /* more than one a unit would lose pictures */
                    if (!got) got = picture(d, out);
                }
            }
        }
        i = end;
    }
    return got;
}

static void vid_close(void *p) {
    vdec *d = (vdec *)p;
    if (!d) return;
    fprintf(stderr, "video: MVD closed: %u pictures (%u frame-ready, %u drained at the end, %u repeated) from %u units, %u NAL units, %u errors\n",
            d->pictures, d->readies, d->drained, d->repeated, d->units, d->nals, d->errors);
    mvdstdExit();
    g_open = 0;
    linearFree(d->in);
    linearFree(d->out);
    free(d->y);
    free(d);
}

static const psp_video_codec CODEC = { "the New 3DS's H.264 decoder (mvd:STD)", vid_open, vid_decode, vid_close };

int psp2i_video_3ds_init(void) {
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    if (!n3ds) { fprintf(stderr, "video: not a New 3DS (no MVD): movies play black\n"); return -1; }
    psp_mpeg_set_video_codec(&CODEC);
    fprintf(stderr, "video: movies through mvd:STD\n");
    return 0;
}
