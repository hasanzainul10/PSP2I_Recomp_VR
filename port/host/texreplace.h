/* psp2i -- texture packs in PPSSPP's format (textures.ini + PNGs).
 *
 * psp2i_display.ini texture_packs= lists pack folders, each holding a
 * textures.ini; packs later in the list win where both replace a texture,
 * and a file a pack names but doesn't have is looked for in the earlier
 * packs (so an add-on pack whose textures.ini also lists the base pack's
 * files works next to it, unmerged). Empty = off: nothing is hashed or
 * loaded and the backends draw exactly as before.
 *
 * The keys are PPSSPP's own (psp_gpu_pack_key), so packs made for PPSSPP's
 * NPJH50332 load unchanged. Supported: [options] hash = xxh64, ignoreAddress;
 * [hashes] for level 0, including PPSSPP's wildcard keys (no data hash, no
 * CLUT hash). Not supported (logged and ignored): other hashes, reduceHash,
 * [hashranges], [reducehashranges], [filtering], mip levels above 0,
 * textures.zip and hash-named files without a textures.ini line. */
#ifndef PSP2I_TEXREPLACE_H
#define PSP2I_TEXREPLACE_H

#include <stdint.h>
#include <stdio.h>

/* Load the packs: `packs` is a ';'-separated list of folders, relative ones
 * relative to `base_dir` (the exe's folder). Returns the number of textures
 * the packs replace (0 = off). */
int texreplace_init(const char *packs, const char *base_dir);

/* Nonzero when a pack is loaded. */
int texreplace_on(void);

/* From a backend's draw, when it is about to decode the current texture:
 * the replacement for it, 0 if none. The id is stable for the run (one per
 * PNG), so a backend can keep its upload and skip the PNG next time. */
uint32_t texreplace_find(void);

/* The PNG of a replacement, w*h pixels 0xAABBGGRR (the layout of
 * psp_gpu_decode_texture), cached once decoded. Valid until the next
 * texreplace_ call. NULL while it is being decoded (on a worker thread,
 * queued by the first call: texreplace_pending is then nonzero) or if the
 * file can't be read: draw the original meanwhile. */
const uint32_t *texreplace_pixels(uint32_t id, int *w, int *h);

/* Nonzero while replacement `id`'s PNG is queued or being decoded: ask
 * texreplace_pixels again once this is 0. */
int texreplace_pending(uint32_t id);

#ifndef PSP2I_PROD
/* Right after a texreplace_find that returned 0: with PSP2I_TEXREPLACE_DUMP
 * set, save the decoded original under its pack key (once per key). */
void texreplace_dump_miss(const uint32_t *px, int w, int h);
#endif

/* One line of counters for the exit report. */
void texreplace_report(FILE *out);

#endif
