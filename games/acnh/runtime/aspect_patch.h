/* Pictures in the window's shape (Mac and iPhone / iPad, 1.2.1 and 1.4.3): by default the camera takes the aspect ratio
   of the window (Mac, following it as the window is resized or goes full screen) or of the screen (iPhone / iPad), so a
   16:10 Mac screen or a 4:3-ish iPad is filled without black bars or stretching. Key 4 switches between that and the
   game's own 16:9. TOTK_ASPECT=<ratio> (21:9, 32:9, 3440x1440, 2.39 ...) fixes a ratio instead of following the window;
   TOTK_ASPECT=0 starts at 16:9 (key 4 then follows the window).
   The game keeps drawing into its 16:9 canvas, but with the camera's aspect ratio set to the window's, so the picture
   holds that view squeezed into the canvas; the present pass then stretches it over the window (nvn_mtl.m
   present_stretch asks runtime_aspect_now), which restores the proportions. UI draws are squeezed back about the canvas
   centre (nvn_mtl.m uw_ui_squeeze / uw_ui_squeeze_y), so the HUD and menus keep their shape in the middle 16:9.
   The community's TOTK-Ultrawide mod for emulators (GPL; none of its code is used here) changes the camera parameter
   DefaultAspect (1.777778 in main's read-only data) and a viewport routine's width / height division. DefaultAspect is
   only copied when a camera is made, so it cannot follow a key; instead each place that hands an aspect ratio to the
   perspective projection (sead's +0xb0, found with a watchpoint on the copies) is patched where the value is produced:
   the instruction branches to a stub in main's text padding that runs it and, while a ratio sits on the data page after
   main (+56, 0 = off), puts that instead. The stub keeps its scratch register on the stack, so no liveness is assumed.
   iPhone / iPad cannot write code at run time: the iOS export (TOTK_EXPORT_IOS) installs the stubs on the Mac, and the
   device checks them and writes the ratio on its data page, as for the render size.
   Not adjusted yet: HUD markers that follow objects in the world and pixel-snapped positions (5 more routines
   TOTK-Ultrawide widens in 1.2.1) sit off. */
#pragma once
#include "runtime.h"
#include <TargetConditionals.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASPECT_DATA_SLOT 56   /* the ratio's float on the data page after main; 0.0 = the game's own */

static _Atomic(uint32_t) *g_aspect_slot;   /* points into that page once the stubs are in */
static float g_aspect_wide;               /* TOTK_ASPECT's fixed ratio; 0 = follow the window */
static _Atomic int g_aspect_on;           /* key 4: the window's (or fixed) ratio instead of the game's 16:9 */
static _Atomic(uint32_t) g_window_ratio;  /* the window's width / height as float bits, 0 until it is known */

/* TOTK_ASPECT as a ratio; 0 when unset, "auto" or "0". A malformed value stops the start. */
static inline float aspect_requested(void) {
    const char *e = getenv("TOTK_ASPECT");
    if (!e || !*e || !strcmp(e, "0") || !strcmp(e, "auto")) return 0;
    double a = 0, w, h; char rest;
    if ((sscanf(e, "%lf:%lf%c", &w, &h, &rest) == 2 || sscanf(e, "%lfx%lf%c", &w, &h, &rest) == 2) && w > 0 && h > 0) a = w / h;
    else if (sscanf(e, "%lf%c", &w, &rest) == 1) a = w;
    if (!(a >= 0.5 && a <= 8)) runtime_fail("TOTK_ASPECT must be <w>:<h>, <w>x<h> or a ratio between 0.5 and 8");
    return (float)a;
}
static inline int aspect_starts_off(void) { const char *e = getenv("TOTK_ASPECT"); return e && !strcmp(e, "0"); }

/* The ratio to put while on: the fixed one or the window's; 0 when that is the game's own 16:9 (within 0.5%). */
static float aspect_target(void) {
    float r = g_aspect_wide;
    if (!r) { uint32_t bits = atomic_load(&g_window_ratio); memcpy(&r, &bits, 4); }
    return r > 0 && (r * 9 / 16 < 0.995f || r * 9 / 16 > 1.005f) ? r : 0;
}
static _Atomic(uint32_t) *g_aspect_vslot;   /* ACNH: the vertical view scale for ratios narrower than 16:9 (0 = none) */
static void aspect_apply(void) {
    if (!g_aspect_slot) return;
    float r = atomic_load(&g_aspect_on) ? aspect_target() : 0; uint32_t bits; memcpy(&bits, &r, 4);
    /* Narrower than 16:9 (a 16:10 Mac screen, an iPad): keep the game's horizontal view and see more vertically
       instead of cutting the sides (and the UI drawn through the same projection) off. */
    float k = r > 0 && r < 16.0f / 9 ? (16.0f / 9) / r : 0; uint32_t kbits; memcpy(&kbits, &k, 4);
    if (g_aspect_vslot) atomic_store(g_aspect_vslot, kbits);
    atomic_store(g_aspect_slot, bits);
}

/* The ratio in force (0 = the game's own 16:9): nvn_mtl.m stretches the picture over the window while it is set. */
float runtime_aspect_now(void) {
    if (!g_aspect_slot) return 0;
    uint32_t bits = atomic_load(g_aspect_slot); float v; memcpy(&v, &bits, 4); return v;
}

/* The window's (Mac, runtime/frontend.m) or screen's (iOS, runtime/frontend_ios.m) pixel size, whenever it changes. */
void runtime_aspect_window(double w, double h) {
    if (!(w > 0 && h > 0)) return;
    float r = (float)(w / h); uint32_t bits; memcpy(&bits, &r, 4);
    atomic_store(&g_window_ratio, bits);
    aspect_apply();
}

/* Key 4 (runtime/input.c). Returns 0 where it is not available (other game versions). */
int runtime_aspect_toggle(void) {
    if (!g_aspect_slot) return 0;
    atomic_store(&g_aspect_on, !atomic_load(&g_aspect_on)); aspect_apply();
    float r = runtime_aspect_now();
    fprintf(stderr, "[totk] key 4: aspect ratio %s", atomic_load(&g_aspect_on) ? (g_aspect_wide ? "fixed" : "the window's") : "16:9 (the game's own)");
    if (r) fprintf(stderr, " (%.4f)", r);
    fputc('\n', stderr);
    return 1;
}

static inline uint32_t aspect_branch(uint64_t from, uint64_t to) {
    int64_t d = (int64_t)to - (int64_t)from;
    if ((d & 3) || d < -(1 << 27) || d >= (1 << 27)) runtime_fail("aspect patch: branch out of range");
    return 0x14000000u | ((uint32_t)(d >> 2) & 0x3ffffff);
}

#define ASPECT_STUB 32   /* bytes per patched instruction */

/* sites: instructions that leave the ratio in an s register (fdiv s<d>, ... or ldr s<d>, [x<n>, #imm]). Stubs go to
   main's text padding from stub_off, checked to be zero and inside the last text page; data_off is the page the loader
   reserves after main. On iPhone / iPad the words were written by the iOS export and are only checked. */
static inline void aspect_install(Module *m, const char *version, const uint64_t *sites, unsigned nsite, uint64_t stub_off, uint64_t data_off) {
    g_aspect_wide = aspect_requested();
    uint64_t page_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    if (stub_off < m->text_end || stub_off + ASPECT_STUB * nsite > page_end) runtime_fail("aspect patch: no room in main's text padding");
    if (data_off + ASPECT_DATA_SLOT + 4 > m->size) runtime_fail("aspect patch: no data page after main");
#if !TARGET_OS_IPHONE
    for (unsigned i = 0; i < ASPECT_STUB * nsite; i++) if (m->base[stub_off + i]) runtime_fail("aspect patch: text padding in use");
#endif
    for (unsigned i = 0; i < nsite; i++) {
        uint64_t s = stub_off + ASPECT_STUB * i;
#if TARGET_OS_IPHONE
        /* Patched at build: the site jumps to its stub, whose first word is the game's own instruction. */
        uint32_t ins, site; memcpy(&site, m->base + sites[i], 4); memcpy(&ins, m->base + s, 4);
        if (site != aspect_branch(sites[i], s)) { fprintf(stderr, "[totk] %s aspect: not in this build's code (16:9 only)\n", version); return; }
#else
        uint32_t ins; memcpy(&ins, m->base + sites[i], 4);
#endif
        if ((ins & 0xffe0fc00) != 0x1e201800 && (ins & 0xffc00000) != 0xbd400000)
            runtime_fail("aspect patch: the camera code does not match this game version");
        int64_t pages = (int64_t)(data_off >> 12) - (int64_t)((s + 8) >> 12);   /* the module base is page aligned */
        if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("aspect patch: data page out of ADRP range");
        uint32_t imm = (uint32_t)pages & 0x1fffff;
        uint32_t stub[8] = {
            ins,                                                              /* the game's own ratio into s<d> */
            0xf81f0fea,                                                       /* str x10, [sp, #-16]! */
            0x90000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | 10,         /* adrp x10, <data page> */
            0xb9400000u | ((uint32_t)(ASPECT_DATA_SLOT / 4) << 10) | (10 << 5) | 10, /* ldr w10, [x10, #slot] */
            0x3400004au,                                                      /* cbz w10, +8: off, keep it */
            0x1e270000u | (10 << 5) | (ins & 31),                             /* fmov s<d>, w10: the wide ratio */
            0xf84107ea,                                                       /* ldr x10, [sp], #16 */
            aspect_branch(s + 28, sites[i] + 4)};                             /* b <after the instruction> */
#if TARGET_OS_IPHONE
        if (memcmp(m->base + s, stub, sizeof stub)) runtime_fail("aspect patch: embedded stub does not match");
#else
        memcpy(m->base + s, stub, sizeof stub);
        uint32_t jump = aspect_branch(sites[i], s);
        memcpy(m->base + sites[i], &jump, 4);
#endif
    }
    g_aspect_slot = (_Atomic(uint32_t) *)(void *)(m->base + data_off + ASPECT_DATA_SLOT);
    atomic_store(&g_aspect_on, !aspect_starts_off()); aspect_apply();
    char fixed[32]; snprintf(fixed, sizeof fixed, "%.4f", g_aspect_wide);
    fprintf(stderr, "[totk] %s aspect: key 4 switches 16:9 / %s (TOTK_ASPECT), starts %s; %u camera site(s), stubs +%#llx\n",
            version, g_aspect_wide ? fixed : "the window's shape", atomic_load(&g_aspect_on) ? "on" : "at 16:9", nsite, (unsigned long long)stub_off);
}
