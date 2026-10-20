/* Animal Crossing: New Horizons 2.0.6 (base game + update). No game-code patches yet (acnh_patches.c); the TOTK
   address table this file held is gone, so GAME_ADDR is deliberately undefined: a TOTK-only code path that still
   uses it fails to compile instead of patching the wrong code. */
#pragma once
#define TOTK_GAME_VERSION "ACNH 2.0.6"
#define TOTK_GAME_DIR "."
#define ACNH_TITLE_ID 0x01006F8002326000ULL
