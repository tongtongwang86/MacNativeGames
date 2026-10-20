#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
/* ACNH game-code patches: applied to main after relocation, before its text is sealed. Each site checks the original
   instruction first (aspect_patch.h checks that it still produces a float ratio). */
#include "game_patches.h"
#include "aspect_patch.h"
#include "game_version.h"

/* Camera aspect ratio (aspect_patch.h: the picture follows the window's shape, key 4 toggles 16:9).
   ACNH uses sead's PerspectiveProjection (2.0.6: default ctor main+0x271f5e0, ctor(near, far, fovy, aspect) main+0x271f6f0;
   aspect at +0xb0, TOTK's layout). Unlike TOTK, most of its projections are made on the fly (the ctor runs about 850
   times a second, reached through function pointers), and the cameras take their aspect from a viewport rectangle; so
   the ratio is not changed where it is produced but where sead reads it: the perspective matrix, the near-plane size and
   getAspect, which the game's culling and screen picking use (3.0.0: main+0x37e3e28, +0x37e3ef4, +0x37e3e0c; 2.0.6 had the
   same code at main+0x271f8a0 / +0x271f970 / +0x271f870, with the matrix's registers allocated differently).
   Only the game's own screen ratio is replaced: a stored aspect of exactly 16:9 (0x3fe38e39, what 1920/1080 and
   1280/720 give) becomes the window's, while projections with other ratios (the 3D model views in menus, render
   targets) keep theirs. ACNH draws its UI through such a 16:9 perspective too, so the UI follows the same rule: wider
   than 16:9, the view widens and the UI ends up in the middle 16:9 of the window; narrower (16:10, 4:3), the view keeps
   its width and grows taller (the near-plane height, +0xac, scaled by (16/9) / ratio), so neither the world's sides
   nor the UI are cut off.
   Each read branches to an 80-byte stub in main's text padding:
       <the read>                     ldp s<h>, s<d> / ldr s<d> that loads the aspect into s<d>
       stp x10, x11, [sp, #-16]!
       adrp x10, <data page>;  ldr w11, [x10, #60]    the 16:9 bits (written by the host)
       fmov w10, s<d>;  sub w11, w11, w10;  cbnz w11, out
       adrp x10, <data page>;  ldr w11, [x10, #52];  ldr w10, [x10, #56]    vertical scale, ratio (0 = off)
       cbz w10, out
       cbz w11, set;  fmov s<d>, w11;  fmul s<h>, s<h>, s<d>     (ldp sites only: the height scaled)
   set: fmov s<d>, w10
   out: ldp x10, x11, [sp], #16;  b <after the read>
   Only x10 / x11 (restored) and no flags are touched. iPhone / iPad: written by the iOS export, checked on the device. */
typedef struct { uint64_t offset; uint32_t original; unsigned reg; int height; const char *what; } AspectRead;   /* height: s<h>, -1 none */
static const AspectRead aspect_reads[] = {   /* ACNH 3.0.0 */
    {0x37e3e34, 0x2d558801, 2, 1, "perspective matrix: ldp s1, s2, [x0, #0xac]"},
    {0x37e3f3c, 0x2d558c02, 3, 2, "near-plane size: ldp s2, s3, [x0, #0xac]"},
    {0x37e3e0c, 0xbd40b000, 0, -1, "getAspect: ldr s0, [x0, #0xb0]"},
};
#define ASPECT_READ_STUB 80
#define ASPECT_VSCALE_SLOT 52   /* data page: the vertical scale (float, 0 = none) */
#define ASPECT_16_9_SLOT 60   /* data page: the 16:9 float bits the stubs compare with */
#define ASPECT_16_9_BITS 0x3fe38e39u

static uint32_t adrp_x10(uint64_t at, uint64_t page_off) {
    int64_t pages = (int64_t)(page_off >> 12) - (int64_t)(at >> 12);   /* the module base is page aligned */
    if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("aspect patch: data page out of ADRP range");
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | 10;
}
static void aspect_install_reads(Module *m, uint64_t stub_off, uint64_t data_off) {
    const unsigned n = sizeof aspect_reads / sizeof *aspect_reads;
    g_aspect_wide = aspect_requested();
    if (stub_off < m->text_end || stub_off + ASPECT_READ_STUB * n > m->ro_off) runtime_fail("aspect patch: no room in main's text padding");
    if (data_off + 64 > m->size) runtime_fail("aspect patch: no data page after main");
    for (unsigned i = 0; i < n; i++) {
        const AspectRead *r = &aspect_reads[i]; uint64_t s = stub_off + ASPECT_READ_STUB * i;
        uint32_t stub[20] = {0}; unsigned k = 0, d = r->reg;
        stub[k++] = r->original;
        stub[k++] = 0xa9bf2fea;                                                          /* stp x10, x11, [sp, #-16]! */
        stub[k] = adrp_x10(s + 4 * k, data_off); k++;
        stub[k++] = 0xb9400000u | ((ASPECT_16_9_SLOT / 4) << 10) | (10 << 5) | 11;     /* ldr w11, [x10, #60] */
        stub[k++] = 0x1e260000u | (d << 5) | 10;                                         /* fmov w10, s<d> */
        stub[k++] = 0x4b0a016b;                                                          /* sub w11, w11, w10 */
        unsigned cbnz_at = k++;
        stub[k] = adrp_x10(s + 4 * k, data_off); k++;
        stub[k++] = 0xb9400000u | ((ASPECT_VSCALE_SLOT / 4) << 10) | (10 << 5) | 11;   /* ldr w11, [x10, #52] */
        stub[k++] = 0xb9400000u | ((ASPECT_DATA_SLOT / 4) << 10) | (10 << 5) | 10;     /* ldr w10, [x10, #56] */
        unsigned cbz_at = k++;
        if (r->height >= 0) {
            stub[k++] = 0x34000000u | (3 << 5) | 11;                                     /* cbz w11, set */
            stub[k++] = 0x1e270000u | (11 << 5) | d;                                     /* fmov s<d>, w11 */
            stub[k++] = 0x1e200800u | (d << 16) | ((unsigned)r->height << 5) | (unsigned)r->height;   /* fmul s<h>, s<h>, s<d> */
        }
        stub[k++] = 0x1e270140u | d;                                                     /* set: fmov s<d>, w10 */
        unsigned out = k;
        stub[k++] = 0xa8c12fea;                                                          /* out: ldp x10, x11, [sp], #16 */
        stub[k] = aspect_branch(s + 4 * k, r->offset + 4); k++;
        stub[cbnz_at] = 0x35000000u | ((out - cbnz_at) << 5) | 11;                       /* cbnz w11, out */
        stub[cbz_at] = 0x34000000u | ((out - cbz_at) << 5) | 10;                         /* cbz w10, out */
        uint32_t jump = aspect_branch(r->offset, s), have; memcpy(&have, m->base + r->offset, 4);
#if TARGET_OS_IPHONE
        if (have == r->original) { fprintf(stderr, "[acnh] aspect: not in this build's code (16:9 only)\n"); return; }
        if (have != jump || memcmp(m->base + s, stub, sizeof stub)) runtime_fail("aspect patch: embedded stub does not match");
#else
        if (have != r->original) {
            fprintf(stderr, "[acnh] aspect main+%#llx: found %08x, expected %08x (%s)\n", (unsigned long long)r->offset, have, r->original, r->what);
            runtime_fail("aspect patch: the camera code does not match ACNH 3.0.0");
        }
        for (unsigned b = 0; b < ASPECT_READ_STUB; b++) if (m->base[s + b]) runtime_fail("aspect patch: text padding in use");
        memcpy(m->base + s, stub, sizeof stub); memcpy(m->base + r->offset, &jump, 4);
#endif
    }
    *(volatile uint32_t *)(void *)(m->base + data_off + ASPECT_16_9_SLOT) = ASPECT_16_9_BITS;
    g_aspect_vslot = (_Atomic(uint32_t) *)(void *)(m->base + data_off + ASPECT_VSCALE_SLOT);
    g_aspect_slot = (_Atomic(uint32_t) *)(void *)(m->base + data_off + ASPECT_DATA_SLOT);
    atomic_store(&g_aspect_on, !aspect_starts_off()); aspect_apply();
    char fixed[32]; snprintf(fixed, sizeof fixed, "%.4f", g_aspect_wide);
    fprintf(stderr, "[acnh] aspect: key 4 switches 16:9 / %s (TOTK_ASPECT), starts %s; %u projection read(s), stubs +%#llx\n",
            g_aspect_wide ? fixed : "the window's shape", atomic_load(&g_aspect_on) ? "on" : "at 16:9", n, (unsigned long long)stub_off);
}
#define ASPECT_STUBS_END(m) ((((m)->text_end + 15) & ~15ULL) + ASPECT_READ_STUB * (sizeof aspect_reads / sizeof *aspect_reads))

/* ACNH_SITE_PROBE=<hex>,<hex>,... (Mac, diagnostics): counts how often each main+<hex> instruction runs and logs the
   counts every 5 s. Each site branches to a stub in the text padding after the aspect stubs that bumps a counter on the
   data page after main (+64 + 4 * i), runs the instruction (it must not be PC-relative) and branches back. */
#define PROBE_MAX 8
static volatile uint32_t *probe_counts; static uint64_t probe_sites[PROBE_MAX]; static unsigned probe_n;
static void *probe_log(void *unused) {
    (void)unused;
    for (;;) {
        sleep(5); char line[512]; int n = snprintf(line, sizeof line, "[acnh] site probe:");
        for (unsigned i = 0; i < probe_n; i++) n += snprintf(line + n, sizeof line - n, " +%#llx=%u", (unsigned long long)probe_sites[i], probe_counts[i]);
        fprintf(stderr, "%s\n", line);
    }
    return NULL;
}
static void site_probe(Module *m, uint64_t stub_off, uint64_t data_off) {
    const char *e = getenv("ACNH_SITE_PROBE"); if (!e || !*e) return;
    for (const char *p = e; *p && probe_n < PROBE_MAX; ) { char *end; probe_sites[probe_n++] = strtoull(p, &end, 16); p = *end == ',' ? end + 1 : end; if (end == p && *p) break; }
    uint64_t page_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    for (unsigned i = 0; i < probe_n; i++) {
        uint64_t s = stub_off + 32 * i, site = probe_sites[i];
        if (s + 32 > page_end) runtime_fail("ACNH_SITE_PROBE: no room in the text padding");
        for (unsigned b = 0; b < 32; b++) if (m->base[s + b]) runtime_fail("ACNH_SITE_PROBE: text padding in use");
        uint32_t ins; memcpy(&ins, m->base + site, 4);
        int64_t pages = (int64_t)(data_off >> 12) - (int64_t)((s + 4) >> 12); uint32_t imm = (uint32_t)pages & 0x1fffff;
        uint32_t slot = (64 + 4 * i) / 4;
        uint32_t stub[8] = {0xa9bf2fea,                                                   /* stp x10, x11, [sp, #-16]! */
                            0x90000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | 10,    /* adrp x10, <data page> */
                            0xb940014bu | (slot << 10),                                   /* ldr w11, [x10, #slot] */
                            0x1100056b,                                                   /* add w11, w11, #1 */
                            0xb900014bu | (slot << 10),                                   /* str w11, [x10, #slot] */
                            0xa8c12fea,                                                   /* ldp x10, x11, [sp], #16 */
                            ins, aspect_branch(s + 28, site + 4)};
        memcpy(m->base + s, stub, sizeof stub);
        uint32_t jump = aspect_branch(site, s); memcpy(m->base + site, &jump, 4);
    }
    probe_counts = (volatile uint32_t *)(void *)(m->base + data_off + 64);
    pthread_t t; pthread_create(&t, NULL, probe_log, NULL); pthread_detach(t);
    fprintf(stderr, "[acnh] site probe on %u instruction(s)\n", probe_n);
}

void runtime_game_patches(Module *m) {
    /* Frame pacing: ACNH advances its simulation once per frame and asks for present interval 2, i.e. every second
       60 Hz vblank = 30 fps on the console. The graphics layer otherwise paces by the Mac display's refresh rate
       (TOTK steps its game time by real time), which ran ACNH at 60 fps on a 120 Hz display: twice the speed. A 60 Hz
       pacing base keeps the game's own interval meaning. DM_PRESENT_HZ set by the user still wins. */
    setenv("DM_PRESENT_HZ", "60", 0);
    fprintf(stderr, "[acnh] main at %p\n", (void *)m->base);   /* for debugger offsets (main+0x...) */
    /* Stubs go to main's text padding: 432 zero bytes between text_end and read-only data (ro_off). */
    aspect_install_reads(m, (m->text_end + 15) & ~15ULL, (m->file_size + 0x3fff) & ~0x3fffULL);
#if !TARGET_OS_IPHONE
    site_probe(m, ASPECT_STUBS_END(m), (m->file_size + 0x3fff) & ~0x3fffULL);
#endif
}

/* TOTK's frame-step and output-resolution patches have no ACNH counterpart: report "unavailable". */
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
uint64_t runtime_output_size_current(void) { return 1920ull | (1080ull << 32); }
