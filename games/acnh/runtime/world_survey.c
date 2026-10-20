/* TOTK's world survey (teleport test tool) has no ACNH equivalent: stubs keep the shared runtime linking. */
#include "world_survey.h"

_Atomic int runtime_survey_pending;
const int32_t runtime_survey_sync[2];
void runtime_survey_init(Module *main_module) { (void)main_module; }
int runtime_survey_command(const char *line) { (void)line; return 0; }
void runtime_survey_tick(uint64_t frame) { (void)frame; }
void runtime_survey_cancel_user(void) {}
