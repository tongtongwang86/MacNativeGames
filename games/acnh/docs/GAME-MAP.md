# ACNH 3.0.0: a map of the game for the co-op work

What we know about how Animal Crossing: New Horizons 3.0.0 is put together, kept as a reference for the co-op work
([LAN-COOP-DESIGN.md](LAN-COOP-DESIGN.md)). Each entry says how it is known:

- **confirmed**: seen in the running game or read in its code.
- **expected**: inferred, not yet checked.

Addresses are `main+0x…` offsets into the game's `main` module (Ghidra's addresses, see section 1). Heap addresses
change from launch to launch and are given only as examples.

## 1. Conventions and tools

| What | How |
|---|---|
| Module base | The log prints `[acnh] main at 0x…`. A guest pointer is a host pointer, so a vtable in an object reads `base + offset`. |
| Decompiling | Run `scripts/ghidra_prep.py`, then `ImportModule.java`, once: this builds `build/ghidra/project`, with 175,846 functions. After that one function takes seconds: `analyzeHeadless build/ghidra/project acnh -process main.flat -noanalysis -readOnly -scriptPath scripts/ghidra -postScript Decompile.java out.c 0x…` (run from `games/acnh`). The output stays in `build/` and is never committed. |
| Reading the running game | `TOTK_INPUT_FILE` lines (runtime/input.c), Mac only:<br>• `peek` / `poke` / `memfind`<br>• `memsnap <dir> <lo> <hi>`: writes every writable region to files<br>• `watch <addr>`: a hardware write watchpoint<br>• `coop host|visit`<br>• `key` / `down` / `up` (keyboard → controller)<br>• `shot` |
| Code probes | `ACNH_SITE_PROBE=<hex>,…` counts how often each instruction runs (24 sites at most; not PC-relative instructions). `ACNH_TRACE_SITE=<hex>` prints x0–x3 and the call chain on each hit, with `ACNH_TRACE_MAX` / `ACNH_TRACE_SKIP` / `ACNH_TRACE_NEW` / `ACNH_TRACE_X1`. |
| Finding a value | Take `memsnap` snapshots while the player does something known (stand, walk down, walk right), then run `scripts/memsnap_diff.py`, which lists the floats that move that way. This is how section 5 was found. |
| Do not | Attach lldb: it froze the game once, and after a second detach the game exited. Send a diagnostic command to an older binary: an unknown command ends the game (`FAIL: invalid diagnostic input command`). |

## 2. Memory layout

- **main:** mapped size 0x8268000. The text, rodata and data of the file come first (to about 0x5a10000), then a large
  .bss. Static managers live there, e.g. the object behind `*(main+0x4ab3f08)`. [confirmed]
- **Guest heap:** shared-memory regions about 3 GB wide. Two launches put it at 0x7030000000–0x70f0000000 and at
  0x7b30000000–0x7bf0000000, so the place changes; find it with `vmmap <pid> | grep "shared memory"`. Actors, field
  data and skeletons all live here. [confirmed]
- **Player actors:** two objects of one class, vtable at main+0x496d180, exist from boot on (the title-screen player
  is one of them). Their update is main+0x1604330, called once per frame for each object, through the actor dispatch
  at main+0x5faa90. [confirmed by trace: x0 = each object, every frame]
  - The class is at most 16 KB and holds the player's state and action graph. It does **not** hold the world position
    directly; the position lives in records that the object reaches through pointers (section 5).
  - Fields seen so far: +0x48 owner, +0x60 field manager, +0x70 state machine, +0x1138 animation state machine, +0x1480
    held-item record (two 0x20 buffers chosen by a flag; empty item 0xfffe), +0x18e0 target record, +0x191e "may
    turn" byte, +0x1ee8 player index. [confirmed, from the furniture work in LAN-COOP-DESIGN.md 8.7–8.10]
- **Per-player network table:** `*(main+0x4ab3f08) + 0x20 + index*0x740`. Its contents did not change while a visitor
  walked, so it holds per-session data (identity, permissions), not movement. [confirmed: no change in two 0x740
  entries over a walk]

## 3. Network stack

From bottom to top. Details are in [LAN-COOP-DESIGN.md 1.1](LAN-COOP-DESIGN.md).

1. **`nn::ldn` and `nn::socket`.** The runtime replaces them: ldn_host.c / socket_host.c / coop_lan.c give each copy a
   virtual 10.114.0.x address, over UDP on a LAN or ZeroTier.
2. **pia 5.31.5:** `local` (Ldn jobs), `mesh`, `transport`, and `clone::EventProtocol` + `ClockProtocol`. There are no
   pia replicated objects: the game sends its own event messages and keeps a shared clock. pia encrypts the packets,
   so their contents have to be read in game memory, not on the wire.
3. **Game managers:** `NetTransportMgr` (main+0x1baa8ec: a 0x258000 heap and two 30000-byte buffers),
   `NetMultiPlayMgr` (main+0x1c43960), `NetStageMgr` (main+0x1cf22d0), `NetSystemMgr`, `NetStatusMgr`, and
   `NetFramework::preSceneCalc` / `postSceneCalc` around each frame.

**Traffic between two copies** [confirmed, socket trace on the visitor, 2026-10-22]:

| Datagram | Size | Rate | Rate while walking |
|---|---|---|---|
| Visitor → broadcast | 244 bytes | ~20 per second | the same |
| Host → visitor | 260 bytes | ~20 per second | the same |
| Reliable traffic from the visitor | 64–276 bytes | about 1 per second | bursts of 112 / 176-byte messages, about 9 per second, during longer walks and while fishing |
| Reliable traffic from the host | 64–820 bytes, many sizes | about 20 per second | the same (the host also sends the island: residents, the world) |

The per-player stream has a fixed size and a fixed rate whether the player moves or stands. It is a periodic state
snapshot, not a stream of input events or of changes. Actions add reliable messages on top of it (section 5.5).
[the bursts: confirmed in the fishing recording, 2026-10-22; a short 2.5 s walk earlier had shown none]

## 4. Sessions and saving

- The two-phase commit on every arrival and departure, and who saves what: [LAN-COOP-DESIGN.md 1.2](LAN-COOP-DESIGN.md).
- Session player count: byte +0x476 of the object behind main+0x4ab1760; 2 or more means company.
- `IsInMyVillage`: main+0x2567d40(player).
- Starting a script from anywhere: main+0x107fed0. See runtime/coop_flow.c.

## 5. Player coordinates, and how multiplayer handles them

### 5.1 The coordinate system [confirmed, 2026-10-22]

- **Units:** world units in float32. **x** grows to the right of the screen (east), **z** grows toward the bottom of
  the screen (south), and **y** is up.
- **Height:** the ground is at y = 0, and the airport pier is at about 0.5.
- **Examples:** Panthea by her house was near (1013, 0, 777). At the host's airport she was near (790, 0, 1065).
- **Speed:** walking moves about 38 units per second.
- **Snapping:** terrain and collision data near the player sits on a 5-unit and 10-unit grid. The camera's shadow
  centre (in main's .bss, main+0x4bed600) follows the player at about 84 units of height.

### 5.2 Where a player's position is kept [confirmed]

There is no single "the position" word. The same x, y, z appears in about 140 places:

- The skeleton's bone matrices: each bone's world position, with y at the bone's height.
- A record of position + facing with the held-item slots (0xfffe) next to it. In one run it was at 0x7071fdc2a8; the
  float after z read 2.0 after a walk to the right [facing: expected].
- A ring of recent positions: 0x30-byte list nodes holding next, prev, x, y, z and a sequence number (0x473, 0x474, …).
  Expected to be footprints or sound.
- A small record `06 06 0e 32 06 02 | 0.005 0.009 | x 0 z | … 13 13`. It has the **same layout on the host and on the
  visitor**, and on each copy it follows the visitor's position as she walks. Expected to be a sound emitter.
  This is the record section 5.3 used to compare the two copies.

Which copy the game treats as the truth, from which the others follow, is still open (section 8).

### 5.3 The visitor's position on the host's copy [confirmed, measured 2026-10-22]

- **The visitor moves her own player.** Her copy runs her input and physics. The host does not run her input: its copy
  of her follows what she sends.
  - Over a 1.3 s walk to the right, the host's value trailed hers by 2–13 units, about 0.2–0.3 s at walking speed.
  - It reached her final x within about 0.4 s of her stopping.
  - Once, the host's copy stopped 0.7 units short of where she stood (790.07 against 790.75), so the copies agree
    closely but not to the bit.
- **The host draws her as a full actor, with her own skeleton.** Her bone positions on the host match her copy to
  within about 0.5 units.
  - The bones cannot be what is sent: 244 bytes at 20 per second has no room for a skeleton.
  - So each copy animates the other players itself, from the state it receives.
- **How the receiving copy plays a remote player.** It uses the animation-state node **`PlayerNetMove`** (string
  main+0x3f6b24d):
  - main+0x1604330, the player update, builds a request named `PlayerNetMove`. It sets the target from the record at
    `*(*(player+0x48)+0x68)+0x278` (x, y, z) and ties the blend to the state machine at player+0x1138 (entries of
    0xe8 bytes, name at +0x68).
  - main+0x2434290 returns early when that state machine's current entry is `PlayerNetMove`.
  - main+0x6a1a40 tests the current state against `CommonNetWait` and `PlayerNetMove`.
  - [confirmed: names and code; the role is expected]
- **What this means for co-op editing:**
  - Movement already works without the runtime doing anything.
  - Each copy owns its own player, and the others replay a snapshot about 0.25 s old.
  - A host-authoritative edit (the plan in LAN-COOP-DESIGN.md 8.10) has to act at the visitor's position **as the
    host sees it**. That position is close enough for choosing a tile (tiles are 5–10 units), but is not exact.

### 5.5 Simultaneous actions: fishing together (recorded 2026-10-22)

**Setup.** The user fished with both players at once, with no patch involved (fishing is not one of the actions the
game blocks for company):
- The host fished at the river by the log bridge.
- Panthea, the visitor, fished at a pond a few tiles away.

A recorder logged both copies' datagrams every 0.5 s and screenshots of both windows every 2 s, for about 140 s.

**What happened on screen** [confirmed]:

| Time | Host | Panthea |
|---|---|---|
| ~32 s | takes out the fishing rod | |
| ~44 s | | takes out the fishing rod |
| ~70 s | Panthea walks past; each copy shows the other player in place | |
| ~72 s | | the host is visible on her screen holding the rod with the line out |
| 84–100 s | fishing | casting at a fish shadow in the pond |
| ~102 s | "I caught a crucian carp!" | still fishing; her game runs on |
| ~120 s | | splash and bite |
| ~124 s | | "I caught a catfish!" |

**What it shows:**
- **Both players fish at the same time, and each catch is the catcher's own.**
  - The host's catch dialog did not pause or block Panthea's copy.
  - Panthea's fish shadow, bite and catch ran on her copy while the host stood elsewhere.
  - So, like movement, the rod action runs on the copy of the player who holds it.
  - The other copy only shows that player holding the rod and casting, as part of the replayed state (5.3).
- **Each action adds a few reliable messages** on top of the 20-per-second snapshots:
  - Panthea's copy sent bursts of 112 / 176-byte messages while she cast and reeled (102–110 s).
  - A new 84-byte message size started at her first cast (~100 s) and repeated while she fished.
  - At her catch (~122 s) it sent a single 224-byte message.
  - The host's traffic showed no burst of its own around its catch, against its steady stream.
  - What these messages contain is unknown: pia encrypts them.

**Not covered yet:**
- Both players at the **same** shadow. The game has `FishNetHostCatchNumResult`, `InsectNetHostCatchNumResult` and
  `DiveFishNetHostCatchNumResult` messages (strings at main+0x3f908d8, 0x3fee3d1, 0x3f66515). That points to the host
  keeping the catch counts and deciding who gets a shared fish, but this is [expected, not seen].
- Whether a shadow that one player catches disappears on the other's screen.
- Whether the catch reaches the catcher's own save (the encyclopedia, pockets) after the session's commit.

### 5.6 How to measure it again

1. Host and visit with `coop host` / `coop visit`.
2. Take three `memsnap` snapshots of both copies' guest heaps, with the visitor standing, after walking down (`key 1
   500`), and after walking right (`key 2 700`).
3. Look for x, y, z rows that move like the walk (scripts/memsnap_diff.py, or the stricter pair search from these
   notes). Then sample both addresses with `peek` while she walks, using `down 2` / `up 2`. Do not use `key`, which
   blocks the command reader for its whole duration.

## 6. Save files: how they are structured (3.0.0)

- **Encryption.** Each `x.dat` is encrypted with AES-128-CTR. The key and counter come from its `xHeader.dat`, as NHSE
  documents. `scripts/acnh_save_crypt.py <header> <dat> <out>` decrypts and also encrypts. Murmur3 checksums cover
  regions (19 in `main.dat`, 2 in `personal.dat`): scripts/acnh_save_regions.c.
- **Items.** Every item is 8 bytes: u16 id, u8 system flags, u8 extra, u32 value (stack count − 1 or uses). 0xfffe is
  empty. The ids are the numbers in the item-name message labels, e.g. `String_USen`
  `Item/STR_ItemName_31_Fish.msbt` label `Fish_00328` "crucian carp" is 0x0148, and `FishingRod_02377` is 0x0949.
- **`personal.dat`** (one per player, 0x74a40 bytes):
  - pocket 2: 20 items at 0x37bf0, then a u32 slot count (0x14) and 20 flag bytes;
  - pocket 1: 20 items at 0x37ca8, with the same trailer;
  - home storage: from about 0x37d74.
- **`main.dat`** (0x9b0e90 bytes): the museum record is at 0x491220, 0x3404 bytes:
  - +0x4: 0x400 dates, u32 = year | month << 16 | day << 24;
  - +0x1004: 0x400 items, in donation order, ending at the first 0xfffe;
  - +0x3004: 0x400 donor bytes, each the donor's resident slot.
- **Player event flags** (`cPlayer:` flags) are in `personal.dat` from 0xC280, right after the player name: one u16 per
  index, 0x800 entries. The index is the u16 at row +12 of `Bcsv/EventFlagsPlayerParam.bcsv` (rows of 212 bytes from
  0x6c, name at +18), e.g. RcoGetTvForCongratsHouse 211, HasLookMyHouseExplain 585, SeqHotelIntroductionDone 1814,
  HotelSpeakTokpm 1817. Code: action main+0xc163b0 / query main+0xc5a0a0 → main+0x264b380 / main+0x264a5f0, array at
  `*(h+0x18308) + *(h+0x18300)` of the player's save object. EventFlagSystemBool answers 0 when a flag is set (as
  NetHasVisitor and the other queries do), so a script's case 0 is the "yes" path.
- **Murmur3 regions of `main.dat`**: 0x114 (0x1e339c bytes, hash at 0x110), 0x1e34b4 (0x3d089c, hash at 0x1e34b0), then
  pairs of 0x37acc / 0x3ce5c (one per resident slot: the residents' copies of personal data), and 0x958f54 (0x57f3c).
  `personal.dat`: 0x114 (0x37acc) and 0x37be4 (0x3ce5c).
- **The island's item layer** in `main.dat` starts near 0x22f800, in the second region (section 7). Plants are field
  items named in `Bcsv/FgMainParam.bcsv` (540 rows of 244 bytes from 0xe4: id at +32, label at +112): e.g. trees
  0xea60 PltTreeOak3, 0xea69 PltTreeCedar3; flowers 0xeb8a / 0xeb8d / 0xeb90 PltFlwYuri2 White / Red / Yellow (bloomed
  lilies); weeds 0xec7f..0xec85 PltWeedAut*. On the test island, replacing its 150 weeds with lilies (template record
  8a eb 00 00 30 02 00 00, the island's own lilies) took Isabelle's evaluation from 2 to 3 stars.
- **In memory** the game keeps several copies of each of these in save format:
  - museum: four on the host (two live, two refreshed when it saves) and two on a visitor;
  - pockets: two on the player's own copy.
  - Live edits (`poke`) must change all of the live ones. That is how a bluegill and a salmon were put in Panthea's
    pockets for the donation test (LAN-COOP-DESIGN.md 8.12).

## 7. The island's item layer

- Each cell is 8 bytes: u16 item, u8, u8, u32. 0xfffe is empty; 0xfffd is an extension cell that holds its column / row
  offset.
- Each column is 0x600 bytes, 192 cells.
- There are six identical copies in memory, all save-format buffers. Outdoor rotation and the live field state are
  kept elsewhere. [confirmed: LAN-COOP-DESIGN.md 8.10]

## 8. Co-op checks found so far

The patch table `mp_edit_sites` in runtime/acnh_patches.c lists each "company" or "not your island" check that has
been lifted, with its group (`ACNH_MP_EDIT=furniture,…`). scripts/mp_count_checks.py lists the readers of the session
player count. The findings behind each entry are in LAN-COOP-DESIGN.md 8.5–8.10.

## 9. Open

- **Which copy of the position is the source.** A `watch` on the 5.2 records during a walk would name the writer. That
  function is where the game's own player position lives.
- **What is in the 244 / 260-byte snapshot.** It is position and state, but the layout is unknown. It could be read
  where `NetTransportMgr` fills its send buffer, before pia encrypts it.
- **Where the live outdoor item and rotation state lives.** The host-authoritative relay needs it
  (LAN-COOP-DESIGN.md 8.10).
- **Two players at one fish shadow.** Who gets the fish, and whether the shadow disappears on both screens (5.5).

## 10. Particle effects (Vfx2) on the Mac port (2026-10-23)

- **Library.** NintendoWare Vfx2 (main+0x4722f37). Effects come from `Effect/Park_Static.Nin_NX_NVN.szs` (a VFXB ptcl)
  through ELink2 (`ELink2/ELink2DB.szs`); e.g. the decorating cursor is the ELink user `FtrEditCursor` with emitter
  sets `Ftr_CursorWait` / `Select` / `Drag` / `Rotate`.
- **Draw path.** VfxMgr::calc main+0x3a075b0; DrawSortBuffer main+0x23e460 → per emitter main+0x222208 → main+0x21f9b0
  → main+0x247e0c, a wrapper of nvnCommandBufferDrawArraysInstanced (4-vertex strip per particle, per-particle data in
  vertex streams with divisor 1, stride 16). Emitter sets are created at main+0x23cdd0. The Vfx2 warning printf
  main+0x22c410 is stubbed to `ret`.
- **Why no effect showed (fixed).** The draws all reached the renderer and landed in the right place, but each particle
  was discarded in its fragment shader: the vertex shaders write only .xyz of a colour varying and the fragment
  multiplies the texture alpha by its .w. The Maxwell GPU starts every generic vertex output at (0, 0, 0, 1); Metal's
  zeroed output gave w = 0. The Metal renderer now starts each float4 vertex output with w = 1
  (`vs_default_outputs` in `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m`). Tested by the user: dust, footprints,
  watering-can water and the decorating cursor show.
- **How it was found.** Site probes on the draw path (the game draws ~350 particles a second); `passdump` (its draw
  lines now print the instance count and base instance) to find the particle shaders; `DM_NVN_HILITE` on their fragment
  keys (geometry shows); edits to the translated `.metal` in `build/shader-cache/rt-*` to print intermediate values as
  colour.
