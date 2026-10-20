# Animal Crossing: New Horizons 2.0.6 on the MacNativeGames native runtime (Apple silicon, Metal 4).
# Game files: extracted/ -> ../../../extracted (exefs/ and romfs/ of base + 2.0.6). Everything made from them stays
# local (rewritten/, analysis/, build/: .gitignore).
TP := ../totk/third_party
GFX_SRC := $(wildcard $(TP)/d2r_gfx/gfx/nvn/*.c $(TP)/d2r_gfx/gfx/nvn/*.m $(TP)/d2r_gfx/gfx/nvn/*.S $(TP)/d2r_gfx/gfx/tex/*.c $(TP)/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(patsubst $(TP)/%,build/gfx/%.o,$(GFX_SRC))
# NVN API 53.313: what ACNH 2.0.6 (SDK 10.9) checks for; TOTK builds keep 55.15. ACNH relies on NVN's default depth
# mode (near = -w, GL clip z) without calling nvnDeviceSetDepthMode.
GFX_FLAGS := -DNVN_API_MAJOR=53 -DNVN_API_MINOR=313 -DNVN_DEFAULT_DEPTH_NEAR_MINUS_W=1 -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -I$(TP)/d2r_gfx/gfx/nvn/include
NATIVE_OPT ?= -O2
PY := .venv/bin/python

RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S)) vendor/savesync/savesync.m
RUNTIME_HDR := $(wildcard runtime/*.h) vendor/savesync/savesync.h

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


build/gfx/%.c.o: $(TP)/%.c
	@mkdir -p $(dir $@)
	xcrun clang $(GFX_FLAGS) -O2 -g -c $< -o $@
build/gfx/%.m.o: $(TP)/%.m
	@mkdir -p $(dir $@)
	xcrun clang $(GFX_FLAGS) -O2 -g -c $< -o $@
build/gfx/%.S.o: $(TP)/%.S
	@mkdir -p $(dir $@)
	xcrun clang -c $< -o $@

build/acnh-native: $(RUNTIME_SRC) $(RUNTIME_HDR) build/runtime_tables.h $(GFX_OBJECTS) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -I$(TP)/d2r_gfx/gfx/nvn/include \
	    -Iruntime -Ibuild -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include \
	    $(RUNTIME_SRC) $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a \
	    -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO \
	    -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa \
	    -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework GameController \
	    -framework CoreHaptics -framework IOKit -framework CloudKit -o "$@"

# Shader translation (Maxwell -> SPIR-V -> MSL): TOTK's prebuilt Ryujinx-based translator (checked against its SHA-256)
# and SPIRV-Cross at the commit build_app.py pins, built with clang. Both stay local (tooling/, build/).
SPIRV_CROSS_COMMIT := aa217aeb6c9f0ace7a0ab233b28807edf45eb165
SPIRV_CROSS_SRC := main spirv_cfg spirv_cpp spirv_cross spirv_cross_parsed_ir spirv_cross_util spirv_glsl spirv_hlsl spirv_msl spirv_parser spirv_reflect
.PHONY: shader-tools
shader-tools: build/spirv-cross/spirv-cross build/shader-service/totk_xlate
tooling/SPIRV-Cross/.pinned:
	rm -rf tooling/SPIRV-Cross && mkdir -p tooling
	git clone -q https://github.com/KhronosGroup/SPIRV-Cross.git tooling/SPIRV-Cross
	git -C tooling/SPIRV-Cross checkout -q $(SPIRV_CROSS_COMMIT)
	touch $@
build/spirv-cross/obj/%.o: tooling/SPIRV-Cross/.pinned
	@mkdir -p $(dir $@)
	xcrun clang++ -O3 -DNDEBUG -std=c++11 -arch arm64 -Wno-deprecated-declarations -c tooling/SPIRV-Cross/$*.cpp -o $@
build/spirv-cross/spirv-cross: $(patsubst %,build/spirv-cross/obj/%.o,$(SPIRV_CROSS_SRC))
	xcrun clang++ -arch arm64 $^ -o $@
build/shader-service/totk_xlate: ../totk/prebuilt/macos-arm64/totk_xlate ../totk/prebuilt/macos-arm64/totk_xlate.sha256
	test "$$(shasum -a 256 $< | cut -d' ' -f1)" = "$$(cat $<.sha256)"
	@mkdir -p $(dir $@)
	cp $< $@ && chmod +x $@

# Headless boot until nnMain is entered (TOTK_VERIFY_STARTUP: the runtime exits there with a JSON line)
verify-startup: build/acnh-native
	TOTK_VERIFY_STARTUP=1 TOTK_HEADLESS=1 build/acnh-native rewritten boot

# GPU submission on, with the same safety flags the TotK app sets (games/totk/app/launcher.m)
GPU_ENV := TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 TOTK_GPU_BUDGET_MB=5000
boot: build/acnh-native shader-tools
	$(GPU_ENV) build/acnh-native rewritten boot

clean:
	rm -rf build

# rebuild the shared graphics objects when this Makefile (and so GFX_FLAGS) changes
$(GFX_OBJECTS): Makefile
