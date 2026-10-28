/* Sound out on the 3DS: the runtime's audio sink (psp_audio_set_sink) on
 * ndsp, the DSP's default component.
 *
 * The runtime hands every buffer a game outputs through sceAudio over as
 * stereo s16 at 44.1 kHz with the channel volume applied, on the game's own
 * thread, already paced to real time (the sound effects and voices come from
 * sceSasCore this way, the music and movie sound once ATRAC is decoded). Each
 * PSP channel gets an ndsp channel of its own (the DSP mixes them) and a ring
 * of wave buffers in linear memory; a block is copied into the next free one
 * and queued. When none is free (the game ran ahead, or the DSP stalled) the
 * block is dropped and counted rather than waited for: the game's thread is
 * the frame-rate limit, and waiting would only add latency.
 *
 * ndsp needs the DSP firmware dumped from the console (sdmc:/3ds/dspfirm.cdc,
 * e.g. by DSP1). Without it, or with sdmc:/psp2i/nosound, no sink is set and
 * the game runs exactly as before, silent. */
#include "3ds_audio.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include <psprecomp/hle.h>

#define AUDIO_RATE  44100
#define CHANNELS    8          /* the PSP's sceAudio channels */
#define NBUF        4          /* wave buffers per channel */
#define BUF_FRAMES  2048       /* stereo frames per buffer (the largest block PSP2i outputs) */

static ndspWaveBuf g_wb[CHANNELS][NBUF];
static int16_t    *g_pcm[CHANNELS][NBUF];
static int         g_next[CHANNELS];
static int         g_open[CHANNELS];
static int         g_on;
static uint64_t    g_blocks, g_dropped;

static int chan_open(int ch) {
    for (int b = 0; b < NBUF; b++) {
        g_pcm[ch][b] = (int16_t *)linearAlloc(BUF_FRAMES * 4);
        if (!g_pcm[ch][b]) { fprintf(stderr, "audio: no linear memory for channel %d's buffers\n", ch); return -1; }
        memset(&g_wb[ch][b], 0, sizeof g_wb[ch][b]);
        g_wb[ch][b].data_vaddr = g_pcm[ch][b];
        g_wb[ch][b].status = NDSP_WBUF_DONE;
    }
    ndspChnReset(ch);
    ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
    ndspChnSetRate(ch, (float)AUDIO_RATE);
    ndspChnSetFormat(ch, NDSP_FORMAT_STEREO_PCM16);
    g_open[ch] = 1;
    return 0;
}

static void sink(int ch, const int16_t *pcm, uint32_t frames) {
    if (ch < 0 || ch >= CHANNELS) return;
    if (!g_open[ch] && chan_open(ch) != 0) { g_open[ch] = -1; return; }
    if (g_open[ch] < 0) return;
    g_blocks++;
    while (frames) {
        const uint32_t n = frames < BUF_FRAMES ? frames : BUF_FRAMES;
        ndspWaveBuf *wb = &g_wb[ch][g_next[ch]];
        if (wb->status != NDSP_WBUF_DONE && wb->status != NDSP_WBUF_FREE) { g_dropped++; return; }
        memcpy(g_pcm[ch][g_next[ch]], pcm, n * 4);
        DSP_FlushDataCache(g_pcm[ch][g_next[ch]], n * 4);
        wb->nsamples = n;
        ndspChnWaveBufAdd(ch, wb);
        g_next[ch] = (g_next[ch] + 1) % NBUF;
        pcm += n * 2;
        frames -= n;
    }
}

int psp2i_audio_3ds_init(void) {
    FILE *f = fopen("sdmc:/psp2i/nosound", "r");
    if (f) { fclose(f); fprintf(stderr, "audio: off (sdmc:/psp2i/nosound)\n"); return -1; }
    if (R_FAILED(ndspInit())) {
        fprintf(stderr, "audio: no DSP firmware (sdmc:/3ds/dspfirm.cdc), silent\n");
        return -1;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    g_on = 1;
    psp_audio_set_sink(sink);
    fprintf(stderr, "audio: ndsp, %d channels of %d x %d frames\n", CHANNELS, NBUF, BUF_FRAMES);
    return 0;
}

void psp2i_audio_3ds_report(FILE *out) {
    if (g_on) fprintf(out, "audio: %llu blocks, %llu dropped\n", (unsigned long long)g_blocks, (unsigned long long)g_dropped);
}

void psp2i_audio_3ds_exit(void) {
    if (!g_on) return;
    psp_audio_set_sink(NULL);
    ndspExit();
    for (int ch = 0; ch < CHANNELS; ch++)
        for (int b = 0; b < NBUF; b++) if (g_pcm[ch][b]) { linearFree(g_pcm[ch][b]); g_pcm[ch][b] = NULL; }
    g_on = 0;
}
