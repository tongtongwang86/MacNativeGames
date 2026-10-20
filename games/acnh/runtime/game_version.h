/* Animal Crossing: New Horizons 3.0.0 (base game + update; 2.0.6 until 2026-10-20). Game-code patches: acnh_patches.c. The TOTK
   address table this file held is gone, so GAME_ADDR is deliberately undefined: a TOTK-only code path that still
   uses it fails to compile instead of patching the wrong code. */
#pragma once
#define TOTK_GAME_VERSION "ACNH 3.0.0"
#define ACNH_MAIN_BUILD_ID "5D913CF71EB24CB22C5105B1FA3EFD97"   /* main's NSO build ID (patch lists name it) */
#define TOTK_GAME_DIR "."
#define ACNH_TITLE_ID 0x01006F8002326000ULL
