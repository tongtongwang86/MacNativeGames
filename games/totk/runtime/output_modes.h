/* Key 3 and the Settings window's Picture (Mac): the game's output size. MetalFX upscaling to the window is a separate
   switch (runtime_mfx_upscale_set, the Settings window's MetalFX upscaling) that works with any size: the output stage
   enlarges whatever size is drawn to the window, and the scene stage replaces the game's own scene upscale.
   A native size is the game's own canvas, UI and scene at that size (its scene render size follows the output, about 5/6
   of it); the swapchain and the canvas-sized texture heaps are made once for the largest size (TOTK_OUTPUT_RES, or
   auto: the largest HeapDef overlay that was built, scripts/make_output_res_overlay.py), so every size up to it can be
   switched to while the game runs. Smaller sizes sit in the swapchain's top-left and the present scales them.
   The Settings window saves the size under "size" (its old "picture" list mixed in MetalFX).
   Included by game_patches.c (1.2.1) and game_patches_v143.c after their output patch, which define output_page (set
   once the patch is in), output_w / output_h (the largest size), pack_size and output_request(w, h) (the game lays out
   again at that size). */
#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { unsigned w, h; const char *name; } OutputMode;
static const OutputMode output_modes[] = {
    {1280, 720, "720p"}, {1600, 900, "900p"}, {1920, 1080, "1080p"}, {2560, 1440, "1440p"}, {3200, 1800, "1800p"},
    {3840, 2160, "4K"}};
#define OUTPUT_NMODE (sizeof output_modes / sizeof *output_modes)

/* TOTK_OUTPUT_RES=auto: the largest output an overlay was built for (0 = none). */
static inline int output_overlay_largest(unsigned *w, unsigned *h) {
    static const unsigned sizes[][2] = {{3840, 2160}, {3200, 1800}, {2560, 1440}};
    const char *dir = getenv("TOTK_ROMFS_PATCHES_OUT");
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s/%ux%u/manifest.tsv", dir ? dir : "extracted/" TOTK_GAME_DIR "/romfs-patches-out", sizes[i][0], sizes[i][1]);
        FILE *f = fopen(path, "r");
        if (f) { fclose(f); *w = sizes[i][0]; *h = sizes[i][1]; return 1; }
    }
    return 0;
}

/* The modes this start can show, in list order; the largest size gets its own entry when the list lacks it. */
static unsigned output_mode_list(OutputMode *out) {
    static char custom[32];
    unsigned n = 0; int largest = 0;
    if (!atomic_load(&output_page)) return 0;
    for (unsigned i = 0; i < OUTPUT_NMODE; i++) {
        if (output_modes[i].w > output_w || output_modes[i].h > output_h) continue;
        largest |= output_modes[i].w == output_w && output_modes[i].h == output_h;
        out[n++] = output_modes[i];
    }
    if (!largest) { snprintf(custom, sizeof custom, "%ux%u", output_w, output_h); out[n++] = (OutputMode){output_w, output_h, custom}; }
    return n;
}
int runtime_output_mode_count(void) { OutputMode m[OUTPUT_NMODE + 1]; return (int)output_mode_list(m); }
const char *runtime_output_mode_name(int k) {
    OutputMode m[OUTPUT_NMODE + 1]; unsigned n = output_mode_list(m);
    return k >= 0 && (unsigned)k < n ? m[k].name : "";
}
int runtime_output_mode_get(void) {
    OutputMode m[OUTPUT_NMODE + 1]; unsigned n = output_mode_list(m);
    uint64_t size = runtime_output_size_current();
    for (unsigned k = 0; k < n; k++) if (pack_size(m[k].w, m[k].h) == size) return (int)k;
    return -1;
}
int runtime_output_mode_set(int k) {
    OutputMode m[OUTPUT_NMODE + 1]; unsigned n = output_mode_list(m);
    if (k < 0 || (unsigned)k >= n) return 0;
    if (runtime_output_size_current() != pack_size(m[k].w, m[k].h)) output_request(m[k].w, m[k].h);
    fprintf(stderr, "[totk] picture: %s\n", m[k].name);
    return 1;
}
/* MetalFX upscaling, on or off at any size: the output stage runs only with MetalFX upscaling on, hence both flags. */
int runtime_mfx_upscale_get(void) { extern int mfxup_output_enabled(void); return mfxup_output_enabled() != 0; }
void runtime_mfx_upscale_set(int on) {
    extern void dm_gfx_set_flag(const char *name, int value);
    dm_gfx_set_flag("mfx_output", !!on); dm_gfx_set_flag("mfx_upscale", !!on);
    fprintf(stderr, "[totk] MetalFX upscaling %s\n", on ? "on" : "off");
}
/* Key 3: the next mode in the list. */
int runtime_output_mode_cycle(void) {
    int n = runtime_output_mode_count();
    if (!n) { fprintf(stderr, "[totk] key 3: switching the picture needs a start with a larger output (TOTK_OUTPUT_RES auto with the HeapDef overlay, or <w>x<h>)\n"); return 0; }
    return runtime_output_mode_set((runtime_output_mode_get() + 1) % n);
}
