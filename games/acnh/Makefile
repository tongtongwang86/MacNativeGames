# Animal Crossing: New Horizons 2.0.6 on the MacNativeGames native runtime (Apple silicon, Metal 4).
# Game files: extracted/ -> ../../../extracted (exefs/ and romfs/ of base + 2.0.6). Everything made from them stays
# local (rewritten/, analysis/, build/: .gitignore).
TP := ../totk/third_party
GFX_SRC := $(wildcard $(TP)/d2r_gfx/gfx/nvn/*.c $(TP)/d2r_gfx/gfx/nvn/*.m $(TP)/d2r_gfx/gfx/nvn/*.S $(TP)/d2r_gfx/gfx/tex/*.c $(TP)/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(patsubst $(TP)/%,build/gfx/%.o,$(GFX_SRC))
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -I$(TP)/d2r_gfx/gfx/nvn/include
NATIVE_OPT ?= -O2
PY := .venv/bin/python

RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
RUNTIME_HDR := $(wildcard runtime/*.h)

.PHONY: help dependencies rewrite tables boot verify-startup clean
help:
	@echo 'targets: dependencies rewrite tables build/acnh-native boot verify-startup'

dependencies:
	@test -x "$(PY)" || python3.12 -m venv .venv
	"$(PY)" -m pip install -q lz4==4.4.5 capstone==5.0.9 unicorn==2.1.4

# Rewrite main/subsdk0/sdk with d2r's patch_nso.py and verify them (structural + emulator equivalence)
rewrite: analysis/rewrite-validation.json
analysis/rewrite-validation.json: scripts/validate_rewrites.py
	"$(PY)" scripts/validate_rewrites.py --rewrite

tables: build/runtime_tables.h
build/runtime_tables.h: scripts/prepare_runtime.py analysis/rewrite-validation.json
	"$(PY)" scripts/prepare_runtime.py

# N1 placeholder: the system fonts are replaced in N2; until then any use stops with a clear message.
build/shared_fonts_config.h:
	@mkdir -p build
	printf '#define SHARED_FONT_COUNT 6\n#define SHARED_FONT_ROWS {"",0,0,""},{"",0,0,""},{"",0,0,""},{"",0,0,""},{"",0,0,""},{"",0,0,""}\n' > $@

build/gfx/%.c.o: $(TP)/%.c
	@mkdir -p $(dir $@)
	xcrun clang $(GFX_FLAGS) -O2 -g -c $< -o $@
build/gfx/%.m.o: $(TP)/%.m
	@mkdir -p $(dir $@)
	xcrun clang $(GFX_FLAGS) -O2 -g -c $< -o $@
build/gfx/%.S.o: $(TP)/%.S
	@mkdir -p $(dir $@)
	xcrun clang -c $< -o $@

build/acnh-native: $(RUNTIME_SRC) $(RUNTIME_HDR) build/runtime_tables.h build/shared_fonts_config.h $(GFX_OBJECTS) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -I$(TP)/d2r_gfx/gfx/nvn/include \
	    -Iruntime -Ibuild -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include \
	    $(RUNTIME_SRC) $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a \
	    -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO \
	    -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa \
	    -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework GameController \
	    -framework CoreHaptics -framework IOKit -framework CloudKit -o "$@"

# Headless boot until nnMain is entered (TOTK_VERIFY_STARTUP: the runtime exits there with a JSON line)
verify-startup: build/acnh-native
	TOTK_VERIFY_STARTUP=1 TOTK_HEADLESS=1 build/acnh-native rewritten boot

boot: build/acnh-native
	build/acnh-native rewritten boot

clean:
	rm -rf build
