#include <stdint.h>
#include <stdio.h>
/* ACNH game-code patches (none yet): applied to main after relocation, before its text is sealed. Each future site
   must check the original instruction word first, as TOTK's game_patches.c does. */
#include "game_patches.h"

void runtime_game_patches(Module *main_module) {
    (void)main_module;
    fprintf(stderr, "[acnh] no game-code patches\n");
}

/* TOTK's frame-step, output-resolution and aspect patches have no ACNH counterpart yet: report "unavailable". */
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
uint64_t runtime_output_size_current(void) { return 1920ull | (1080ull << 32); }
int runtime_aspect_toggle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
