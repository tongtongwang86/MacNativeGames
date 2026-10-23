# LAN co-op on one island: design

> **Status:** design only, written 2026-10-21. No code changed, no game started, no save touched.
> It builds on [MULTIPLAYER-EDITING-PLAN.md](MULTIPLAYER-EDITING-PLAN.md) (called "the plan" below: its section
> numbers are written "plan 2.1" and so on). Statements are marked **[confirmed]** (read from this repository, the 3.0.0
> image `rewritten/main.patched.bin`, its romfs or the live save folder) or **[expected]** (from knowledge of the game
> or the SDK, still to be checked). Addresses are offsets in `main.patched.bin`, which is laid out flat (file offset =
> offset from main's base; text ends at 0x3f05ab0).

## 0. The goal

> "play on one island, edit the land and all of the single player function concurrently on LAN"

Several copies of the port (Macs and the iPad) on one network, all on **one island at the same time**. Every player
can do what the island's owner can do alone: terraform, lay paths, build and move houses, bridges and inclines, place
and move furniture and items outside, decorate, shop, donate to the museum, deal with residents, craft. The island
stays consistent for everyone and is **saved once**.

Short version of the recommendation (section 3): use the game's own local play for presence and for everything it
already replicates, keep the host's save authoritative, and add a small **edit-replication channel in the runtime**
that re-runs the game's own edit functions on every copy, in an order the host decides. Lifting the visitor
restrictions alone is not enough, and syncing whole islands at the save level cannot give "together".

---

## 1. What the game's local play already does

### 1.1 Session and transport

| Claim | Status |
|---|---|
| Local play goes `nn::ldn` → pia `local` (Ldn*Job) → pia `mesh` → pia `transport` (Reliable, BroadcastReliable, StreamBroadcastReliable, Unreliable) → pia `clone::EventProtocol` / `ClockProtocol`. Pia, PiaLocal, PiaLan, PiaClone, PiaCommon are all **5.31.5**; NEX is 4.6.11. | **confirmed** (RTTI `_ZTSN2nn3pia…`, SDK MW strings at 0x4722d8f–0x4722e43) |
| Pia **clone** is linked only for `EventProtocol`, `EventMessageData/Header` and `ClockProtocol`; none of pia's replicated-object classes (`CloneElement`, `ReliableCloneElement` …) has RTTI. So game state is **not** replicated as shared objects: the game sends its own **event messages** and keeps a shared clock. | confirmed (absence of RTTI); "not used" is **expected** (classes without virtuals would leave no RTTI) |
| **pia `lan`** (UDP LAN matchmaking: `LanNetworkFactory`, `LanMatchCreateSessionJob`, …) is also linked. If the game can be pointed at it, local play would run over plain sockets with no `nn::ldn` at all. | confirmed (RTTI); whether any game path reaches it is **expected-unlikely** (a retail game normally selects only Ldn and Nex) |
| Game-side managers: `NetSystemMgr` (with `NetSystemMgr::Catalog pack`), `NetMultiPlayMgr`, `NetStageMgr`, `NetTransportMgr`, `NetStatusMgr`, `NetFramework::preSceneCalc/postSceneCalc`, `PlayerNetMove`, `NpcNetTurn`, `MultiPlaySession`, `MultiPlayStatus`. | confirmed (strings, each referenced from code, e.g. `NetMultiPlayMgr` at 0x1c43a6c, `NetTransportMgr` at 0x1baa914) |
| `NetFramework::preSceneCalc` / `postSceneCalc` are the per-frame network steps around the scene update: the natural place for a runtime hook that applies remote edits at a safe point. | names confirmed; that they run on the main thread once per frame is **expected** |

### 1.2 Arrival, departure and saving: everyone commits together

The session state machine has explicit **two-phase commit** states [confirmed, strings referenced from code]:
`State::cHostDataMatching` (0x27b953c), `cHostSaveSync`, `cHostSyncCommit` (0x1ce534c), `cHostSyncRollback`
(0x1ce53a0), `cHostWaitOthersSave`, `cVisitSaveSync` (0x1cc1b40), `cVisitSaveSyncWait`, `cVisitCommit`,
`cVisitWaitCommit`, `cRollback`, `cRollbackHost`, `cRollbackVisitor`, `IsAfterRollback`, plus `cHostAutoSave` /
`cVisitorAutoSave` and `AutoSaveMgr::VisitorSetupDecision`.

So, **[expected, consistent with the states and DIALOG_Msg 805 "save data will be reverted"]**:

- At every arrival and departure all players save, and either all commit or all roll back to the previous save.
- The **host saves the island** (`main.dat`); each visitor saves **only their own data** (their `personal.dat` inside
  *their own* island's save: pockets, bells, miles, recipes, catalog).
- `cHostDataMatching` is where the arriving visitor receives the host's island. From then on **every copy holds a
  full copy of the host island in memory** and simulates it; the session keeps those copies in step with event
  messages. This is the property section 3 builds on.

### 1.3 What travels live today [expected, from game knowledge; to be confirmed by stage L3]

- Player position and animation (`PlayerNetMove`), emotes, chat, tool actions.
- The **item layer**: dropping, picking up, fruit, flowers, weeds, shells, buried items, holes.
- Trees and other field objects changed by tools (chopping, shaking, digging up, planting where allowed).
- NPC (resident) movement and conversation turns (`NpcNetTurn`): the host runs the residents, visitors see them.
- Shops on the host's island: visitors can buy and sell at Nook's Cranny and Able Sisters (the shops' stock is the
  host's).
- The catalog (`NetSystemMgr::Catalog pack`: items seen by visitors get catalogued).

### 1.4 What it blocks, and how the blocks are written

The full table is plan 2.1 (host side, H1–H19) and plan 2.2 (visitor side, V1–V7). New from this analysis:

| Finding | Status |
|---|---|
| The block reasons are **named error kinds** in code, not only message ids: `FenceBuildError_PlayingNet` (0x2389ea8), `FtrOperateError_ExistsVisitor` (0x22dda78), `FtrIndoorOperateError_ExistsVisitor` (0x22dd838, 0x23262dc, 0x2328c7c), `FtrPickupError_ExistsVisitor` (0x22c1630, …), `RugPickupError_ExistsOtherP`, `WallFtrPickupError_ExistsOtherP`, `CeilingFtrPickupError_ExistsOtherP` (around 0x2326xxx–0x2328xxx), `Towel_ExistVisitor` (0x2224378), `ChkGateOpen` (0x23231b8), `EquipDustClothWithGateOpen` (0x2322bec). The code copies the name into a fixed string when it picks the error, so the **branch just before each copy is the check**. | confirmed (strings and references); "the branch before is the check" is **expected** |
| Two kinds of furniture block exist: **"a visitor exists"** (a global rule: not while company is here) and **"another player is on it"** (`…ExistsOtherP`: someone stands on the rug, under the ceiling item). Only the first kind is a policy to lift; the second protects consistency and **must stay**. | names confirmed; the meaning is **expected** |
| `IsExistVisitor` is a serialized status field (0x24b24d4, written next to the bool at +0xc of a status struct), so "visitors present" is one flag many checks read. | confirmed (reference); its readers **expected** |
| Terraforming, fences and furniture placement share **one edit state machine**: `EditState::cMultiDrag` (registered at 0x13c1dc8), `cFencePut` (0x13c2210), `cLandMakingPut` (state 0x14, registered at 0x13c2308 with handlers 0x13e0aac / 0x13e0b24 / 0x13e0b28 from the table at 0x4abc0a0). Also `cLandMakingWait`, `cLandMakingCleanUp`, `cFenceWait`, `cTrans`, `cDrop`, `cRotate180`, `cRemake`. | confirmed |
| `FieldLandMakingError.bcsv` (18 terraforming errors) has no multiplayer row: the multiplayer refusal sits outside the terraforming tables (plan 2.3). | confirmed (plan) |

### 1.5 What local play cannot give, whatever is patched

- **Visitors are not residents.** A visitor's house, house interior, storage, residence (Nook Miles+ for the island,
  island evaluation credit) live on *their own* island. On the host island they have no house to decorate.
  The island save has slots `Villager0`–`Villager7` for up to 8 resident players [confirmed: the live save has
  `save/device/Villager0/personal.dat` 477,760 bytes and `main.dat` 10,161,808 bytes; the 8-slot limit is
  **expected** from game knowledge].
- **Structural edits have no message** [expected, plan 4.1]: terrain height, water, cliffs, paths, fences,
  building positions, bridges and inclines. Live messages exist only for the item layer and field objects.
- **Representative-only actions.** On a single console, only the Resident Representative gets the Island Designer
  app and Tom Nook's island projects [expected]. Lifting the visitor checks therefore also has to lift the
  "representative" checks for co-op players.

---

## 2. Architectures

### 2.1 (A) The game's local play, restrictions lifted

Implement `nn::ldn` / `nn::socket` / `nifm` by name (plan 1.2–1.3), then patch the checks (plan 3, 5).

| | |
|---|---|
| **Works** | Seeing each other, moving, chat, items, trees, shops, residents' AI and time (host runs both), the game's own save commit. Everything the game already replicates stays exactly right. |
| **Breaks** | Anything without a live message: a lifted terraform/fence/building check changes only the editor's copy. The others see the old island until they re-join; collision differs meanwhile (items placed on a spot only one side thinks is valid can be refused or misplaced). A visitor's structural edit is not in the host's save, so it is **lost** at the next commit. |
| **Who saves** | The host (island) and each visitor (own player data). Unchanged from the game. |
| **Same tile** | Item layer: the game already arbitrates (host decides). Structural: undefined, last local edit wins on each copy separately. |
| **Effort / risk** | Transport ~2–3 weeks; check patches cheap (one-word patches). Low technical risk, but it **meets the goal only for the item layer**. |

### 2.2 (B) Runtime island-state sync between single-player islands

Each player runs a normal single-player game on a copy of the same island. The runtime watches world state (memory
or the save image), diffs it, sends the diff, and the others write it in and reload.

| | |
|---|---|
| **Works** | Every single-player function works for every player (they are all "the owner"), no visitor checks at all. |
| **Breaks** | **No presence**: the game never draws the other players; making them visible means writing avatar replication from scratch. **Residents and time diverge**: each copy runs its own villagers, weather, RNG, shop stock, daily events; reconciling them is a merge of two different islands. **Reload cost**: applying a terrain diff needs a field reload (seconds, a loading screen) per change. **Conflicts**: two edits to the same acre must be merged at byte level in a 10 MB encrypted, versioned structure whose layout (NHSE) is known only for save files, not for live memory. **Saving**: whose `main.dat` wins at the end? |
| **Effort / risk** | Months; high risk; result is "taking turns on a shared island, synced every few seconds", not playing together. |

### 2.3 (C) Hybrid: local play + edit replication by the runtime (recommended)

Use (A) for the session, presence and everything already replicated. For each **structural edit kind**, hook the
game's own *apply* function (the code that runs when the edit is committed: the end of `EditState::cLandMakingPut`,
`cFencePut`, the building placement, …). On every copy the hook turns the call into a **record** (edit kind +
arguments: tile coordinates, kind, rotation, item id) and the runtime sends it over the side channel that
`ldn_lan.c` already has (plan 1.3: one TCP connection per station).

- **Host-sequenced.** A visitor's hook does *not* apply locally: it sends the record to the host. The host checks it
  with the game's own validity checks (the code that produces `FieldLandMakingError` rows, `FenceBuildError_*`), applies
  it, appends it to its ordered log, and sends it to **all** copies including the sender, which then apply it. On a
  LAN the round trip is a few milliseconds, so the visitor sees no lag worth hiding.
- **Applied at a safe point.** Remote records are applied in the hook on `NetFramework::preSceneCalc` (main thread,
  once per frame), never from the network thread.
- **Same code, same data.** Every copy holds the same island (section 1.2) and runs the same game code, so calling
  the same apply function with the same arguments, in the same order, yields the same result, including the terrain
  mesh rebuild the game does for its own edits.
- **Saved once.** Only the host's `main.dat` is the island; since every edit is applied on the host, the game's own
  commit saves it. Visitors' copies are thrown away at departure, as in the real game.
- **Fallback per kind:** if an edit kind cannot be replayed safely, the host sends a "re-sync" instead: the visitor
  goes through the game's own departure and arrival (plan 4.3 B), which re-copies the island in `cHostDataMatching`.

| | |
|---|---|
| **Works** | Everything in (A), plus terraforming, paths, water, cliffs, fences, bridges/inclines, building moves, outdoor furniture by any player, saved once by the host. |
| **Breaks / open** | Effects of an apply function on *player-local* state (the editing player's animation, UI, tool durability) must not be replayed on other copies: the hook must sit below the UI, at the world-state write. Tom Nook projects and resident move-ins are multi-day state machines in `main.dat`; they need the "request" step replicated, and the overnight completion then runs on the host and reaches visitors at the next arrival. Visitors still have no house on this island (decision D4). |
| **Same tile** | Solved by the host's order: the second edit is validated against the island *after* the first one and refused with the game's own error if it no longer fits. Item layer stays with the game's arbitration. A structural edit on a tile where an item lies is checked by the game's own rule on the host. |
| **Residents and time** | The host's clock and residents, as in the real game. Nothing new to sync. |
| **Effort / risk** | (A) plus ~1–2 weeks per edit kind for finding the apply function and its arguments, then little code each (a record struct, a hook, a replay call). The risk is concentrated in one question (does replaying the apply function leave the copies identical?), and it is **tested per kind** with a memory comparison (stage L5), so a failure stops one kind, not the project. |

### 2.4 Other variants considered

- **(D) Edits only on the host, visitors re-sync by re-joining** (plan 4.3 A+B). The cheapest version of C; it is
  stage L5a below and the permanent fallback per kind. 20–40 s per re-join makes it unpleasant as the main mode.
- **(E) Send changed save blocks mid-session** (plan 4.3 D): needs the live in-memory layout and a field reload entry
  point. More fragile than replaying functions, and slower to see. Kept only as a fallback for kinds C cannot do.
- **(F) Co-op players as real residents** (a `VillagerN` slot each on the host's island). This is the only way they get
  a house, storage and resident status *on this island*. It is orthogonal to A/C and affects how a visitor identifies
  itself at arrival; see decision D4.

---

## 3. Recommendation and roadmap

**Recommended: (C), built in stages so that (A) and (D) fall out on the way and each stage is useful alone.**

Rules for every stage:

- Every patch is a row `{offset, original word, patched word, group, what}` checked before writing (plan 5.1), under
  `ACNH_MP_EDIT=0|host|full` (plan 5.2); default off until the stage's check passes.
- Every run is on **test profiles** (`TOTK_PROFILE_DIR=user-data-coop-host`, `user-data-coop-b`), never on
  `user-data/save` or anything SaveSync uploads. SaveSync stays off in those profiles.
- Two copies at once on one Mac only after decision D1.

### L0. Tooling (static, no game)

- **Work:** move the MSBT dumper and the xref finder into `scripts/` (`msbt_dump.py`, `xref.py`; the plan's copies
  were left in an earlier session's notes folder, not in the repository), add `scripts/mp_sites.py` that lists the error-name references in
  1.4 with the branch before each, and a patch-overlap checker against SpeedFix, aspect and `rewritten/main.sites.tsv`.
- **Result:** a generated `docs/mp-sites.tsv` (site, original word, error name, condition).
- **Verify:** reproduces every address in section 1.4 of this document.
- **Stop if:** none (pure static).

### L1. One copy hosts local play (Mac)

- **Work:** `runtime/ldn_host.c` (access-point half of the 25 `nn::ldn` imports), `runtime/socket_host.c` (UDP/TCP
  core, errno map, virtual 10.114.0.n subnet), `runtime/nifm.c` (local network mode accepted, internet still refused),
  `runtime/ldn_lan.c` (UDP 11452 scan reply). Wire them in `loader.c` `relocate()` through `runtime_override(name)` as
  `applet_host.c` does for `nn::nfp`. Add a Python client `scripts/ldn_scan.py`.
- **Result:** in the test profile, Dodo offers "Via local play", the gate opens and closes; `ldn_scan.py` on the
  same Mac (and on the iPad's network) lists the island's name from the advertise data.
- **Verify:** log shows AccessPointCreated; no `ipc_unimplemented`; gate closes cleanly and the game saves; a
  reload of the test profile works.
- **Stop / switch if:** pia insists on an ldn security mode or a call the name replacement cannot satisfy (then
  look at serving `ldn:u` over IPC instead, plan 1.2), or nifm's local-mode path needs IPC states we cannot fake.

### L2. Two copies see each other (first co-op session, game rules unchanged)

- **Work:** station half of `ldn_host.c`, the TCP control channel in `ldn_lan.c`, address mapping for two copies on
  one host (port stride), `ACNH_LDN_PEER` for the iPad later. Second test island for the visitor (decision D2).
  Two-slot mode in `game_slot.c` if D1 = yes.
- **Result:** the visitor lands on the host island; both see each other walk; the visitor drops a piece of furniture
  outside, the host picks it up; chat works; the visitor leaves; both save.
- **Verify:** after the session restart both test profiles: the host island has the item where the host put it;
  the visitor's pockets match. Arrival and departure commits succeed both ways; a forced quit of the visitor
  mid-session produces the game's own rollback (DIALOG_Msg 805) on the host and nothing worse.
- **Stop / switch if:** joining fails inside pia's crypto (`PacketReader::BeginDispatch decrypting failed`) and the
  key material comes from a service we cannot answer; or the session is unstable on loopback. Mac + iPad is the
  fallback path if two copies on one Mac misbehave.

### L3. Inventory of messages and apply functions (static + probes)

- **Work:** log size and first 16 bytes of every pia `EventProtocol` send/receive (plan 4.2) while doing each allowed
  action. Statically, from the `EditState` table at 0x13c1dc8–0x13c2338, find the commit point of `cLandMakingPut`,
  `cFencePut`, `cDrop`/`cTrans`/`cRotate180` (furniture outside), and from the Tom Nook EventFlow actions, the
  building move/bridge/incline request functions. For each: arguments and which `main.dat` layer it writes.
- **Result:** `docs/mp-edits.md`: per edit kind, "carried by the game" or "needs replication", and the apply function
  with its signature.
- **Verify:** `ACNH_SITE_PROBE` on each candidate apply function counts exactly one hit per edit in a single-copy
  session (Island Designer on the host, gate closed).
- **Go / no-go:** if the edit kinds have a clean apply function (arguments only, world-state writes only), go to L5
  with (C). If they are tangled with UI and player state, use (D) for those kinds.

### L4. Visitors do what an owner does on the item layer (tier 1)

- **Work:** patch group `tier1`: Best Friend tools for visitors (V1–V3), Decorating Mode with visitors (H4), the
  `…ExistsVisitor` furniture blocks, planting near others (H5). Keep every `…ExistsOtherP` block.
- **Result:** a visitor places and moves furniture outside with Decorating Mode; the host sees it live; after a
  departure commit and a reload of the host's profile, it is still there.
- **Verify:** compare item layers of the host's saved `main.dat` and the visitor's in-memory copy (NHSE offsets);
  equal.
- **Stop if:** a visitor-placed item appears on the host but not in the saved island (the game replicated it as a
  visual only); then route furniture placement through the L5 channel too.

### L5. Structural edits by the host, live for visitors

- **L5a (fallback first):** lift H1/H2/H6/H7 for the host (patch group `host-edit`) and, after each structural edit,
  the host runtime triggers a re-join of each visitor. **Result:** host terraforms, the visitor sees it after the
  re-join with matching collision.
- **L5b (replication):** the edit channel in `runtime/coop_edits.c` (record format, host log, apply hook in
  `NetFramework::preSceneCalc`), one kind at a time: paths → terrain height → water → fences → bridges/inclines →
  building moves. **Result:** host lays a path; the visitor sees it appear within a frame or two and can walk it.
- **Verify, per kind:** after a scripted sequence of 50 edits, dump the terrain/structure blocks of the island from
  both copies' memory and compare: identical. Then departure commit, reload, compare with the saved `main.dat`.
- **Stop if:** a kind's memory differs after replay and the difference cannot be traced to player-local state within
  a few days: leave that kind on L5a.

### L6. Visitors make structural edits (the goal)

- **Work:** visitor-side patches (V5: Island Designer and permits on another island, representative-only checks), and
  the hook in "request" mode on visitors (send to the host instead of applying).
- **Result:** a visitor terraforms the host island; host and other visitors see it live; it is in the host's save.
- **Verify:** the L5b comparison with the edits coming from a visitor; plus two players editing the same tile within
  the same second: one edit wins, the other gets the game's own error, copies stay identical.
- **Stop if:** the visitor's Island Designer UI depends on its own island's data in ways that break on the host's
  (map size, permits stored in `personal.dat`); then keep visitor terraforming as "ask the host" (a request queue
  shown to the host).

### L7. The rest of single-player, by kind (tier 2)

Museum donations (H13), Tom Nook services and building plots (H7, H8), Resetti reset (H9), custom designs and
Nook Shopping (H10, H11), hotel (H17). Each is a request/record through the L5 channel or a lifted check, verified the
same way (identical state on both copies, saved once). Each player's own progress (miles, recipes, catalog, museum
credit) stays in their own `personal.dat`.

### L8. iPad

`Info.plist` local-network keys, Bonjour (`_acnh-ldn._udp`) or `ACNH_LDN_PEER`, patch groups baked in by
`build_ipa.py --mp-edit=<level>` (iOS cannot patch at run time; plan 5.2). **Result:** Mac host, iPad visitor, the
L4–L6 checks again; then iPad host.

### L9. Optional: co-op players as residents (decision D4)

Only if wanted: each co-op player gets a `VillagerN` slot and a house on the host island, and arrives as that
resident. Large: it changes how the game identifies a visitor at arrival, and needs its own design once L6 works.

---

## 4. Decisions for the user

| # | Question | Options | Recommendation |
|---|---|---|---|
| D1 (plan 5.4.1) | **Two games at once on this Mac** for testing from L2 on. `game_slot.c` allows one game because exhausted memory panicked the kernel four times on 2026-10-12. | (a) add an explicit two-slot test mode, (b) test only Mac + iPad / second Mac | **(a)**, as an opt-in test mode (`TOTK_GAME_SLOT_PAIR=1`, taken by one wrapper for both copies), with a memory check before the second copy starts. This Mac reports 96 GB; the panic happened under memory exhaustion, so measure one copy's peak first. Iterating on one Mac is several times faster than deploying to the iPad, and the iPad's IPA must be rebuilt for every patch change. |
| D2 (plan 5.4.2) | **Which save the visitor uses** | (a) a new game in a test profile, clock moved forward with `TOTK_TIME_OFFSET`, (b) a second imported island | **(a)** for testing: a fresh island past day 2 is enough to fly and cannot damage a real save. For actual play, each co-op player keeps their own island; it only holds their character (see D4). |
| D3 (plan 5.4.3) | **Ryujinx / ldn_mitm compatibility** | (a) yes, (b) no, port ↔ port only | **(b)** now, keeping the packet layout of the public LAN protocol so (a) stays possible. Compatibility only works on 3.0.0 and needs testing against Ryujinx; it does not help the goal. |
| D4 (new) | **Are co-op players visitors or residents?** | (a) visitors from their own small island, (b) residents of the host island (L9) | **(a)** first. Everything in the goal except "my own house on this island" works as visitors once L6 is done; (b) is a separate project. |
| D5 (new) | **Who hosts** | (a) always the Mac that owns the island save (SaveSync's copy), (b) any device | **(a)**. Only the host's save is the island; the iPad can host later by the same rule, but never two devices with the same island at once (SaveSync would see two writers). |
| D6 (new) | **Scope of "everything"** | (a) all of L4–L7, (b) stop after L6 (items + terraforming + building moves) | **(b)** as the first target; L7 items one at a time by what the user misses most. |

### 4.1 What the user decided (2026-10-21)

- **D1: yes**, two games at once on this Mac for testing (a two-slot test mode).
- **D2, D3:** testing may use whatever saves are convenient (test profiles only); **no ldn_mitm / Ryujinx
  compatibility**, so the discovery protocol is the port's own.
- **D4, D5, D6:** as recommended (visitors first; the save owner hosts; stop after L6 first).
- **New requirement: it must work across a LAN *and* over a virtual network such as ZeroTier**, not only on one
  subnet (section 6).
- **New requirement: an island browser** in the port's own UI, to discover and join hosts without walking to the
  airport every time (section 7).
- **Progress data (decided 2026-10-22):**
  - **Each player's own progress stays theirs.** Critterpedia, catalog, recipes, Nook Miles and pockets stay in their
    own `personal.dat`. The game's save commit already writes them; the runtime does not sync them.
  - **The island's shared progress has one copy, in the host's `main.dat`.** This covers the museum, Town Hall and
    buildings. Only the host's game changes it: a visitor's request, such as a donation, is sent to the host's copy,
    which carries it out with the visitor credited, and the host saves it at the commit.
  - **No merging of saves after a session.**
  - **Accepted consequence of visitors (D4 a):** a visitor's Critterpedia shows their home museum's "donated" marks,
    not the shared museum's.

---

## 6. Networks: LAN, ZeroTier and the like

The game only ever sees the virtual LDN subnet 10.114.0.0/24 handed out by `ldn_host.c` (plan 1.3). Real addresses
live in the runtime only, so any IP network between the devices works as long as the runtime can reach the peers:

| Network | Discovery | Game traffic |
|---|---|---|
| Same LAN / Wi-Fi | UDP broadcast on every interface's broadcast address, plus Bonjour (`_acnh-coop._udp`) | unicast UDP |
| **ZeroTier** (virtual Ethernet: broadcast and mDNS usually pass) | same as LAN on the `zt*` interface; manual address as backup | unicast UDP over the ZeroTier address |
| Tailscale / WireGuard / routed networks (no broadcast) | **manual address** (`host:port`), remembered in the browser's list | unicast UDP |
| Two copies on one Mac | loopback, each copy with its own port offset | unicast UDP on 127.0.0.1 |

Rules that make this work:

- **No broadcast in the data path.** A send to the virtual broadcast address 10.114.0.255 becomes one unicast per
  member (plan 1.3), so networks without broadcast carry the session.
- **The host's control port is the only fixed one** (TCP and UDP **11452 + offset**); members report their real
  address and port offset when they join, and the host sends the node table to everyone.
- **Addresses the peers see.** A member announces the address it reached the host from (its socket's local address),
  so a ZeroTier member is reached on its ZeroTier address, not on a LAN address the host cannot route to.
- **MTU.** ZeroTier's MTU is 2800 and Tailscale's 1280; pia's packets are below 1400 bytes [expected], so at worst
  they are fragmented by IP on Tailscale. Nothing to do unless measurements show loss.
- **Security.** The session is for people the user trusts on their own network. Joining needs the host's island in
  the list and, optionally, a passphrase set in the browser (sent as a hash in the join request). Nothing listens
  unless the user hosts or browses.

## 7. The island browser: join without the airport

**What the user sees.** A *Local play* section in the Settings window (Mac) / sheet (iPad):

- **Host:** "Open my island to local play" (a switch), with the island name and the address others can type.
- **Islands found:** live list (island name, host player, players n/8, address, network: LAN / ZeroTier / manual),
  a **Join** button per row, and **Add by address…** for networks without discovery. The list is kept between runs.
- **Status:** connected players, ping, "waiting for the game".

**How it drives the game.** Two levels, the second built on the first:

1. **J1, browser + quick airport.** The browser does discovery and keeps the chosen host. When the player then talks
   to Dodo and picks local play, the game's own scan (`nn::ldn::Scan`) returns **only the chosen island**, so the
   game's list has one entry and is confirmed with one button. Hosting from the browser makes the next "open the
   gate" use those settings. This needs no game patches and works on iOS at once.
2. **J2, no airport at all.** The runtime starts the game's own session flow from anywhere on the player's island:
   the EventFlow actions `EventFlowActionNetCreateSessionVillage` (host) and `EventFlowActionNetJoinSessionVillage`
   (visit) are the game's entry points [confirmed names, plan 3.1]. A small custom EventFlow file in the romfs overlay
   (the runtime already serves romfs) calls them with the game's own departure / arrival demo; the runtime starts it at
   a safe point (no menu open, player idle, outdoors), the way a phone app starts its flow. The game then runs the
   normal join, including the save commit (section 1.2), so nothing about saving changes.
   - **Verify:** from the browser, a visitor on their own island presses Join and lands on the host island with the
     game's arrival scene; leaving with "−" works; both saves are right after a restart.
   - **Stop / switch if:** starting the flow outside the airport scene breaks the game's scene state (the departure
     demo may expect the airport). Fallback: the browser warps the player to the airport and answers Dodo's
     questions itself (the quick airport of J1, made automatic).

J1 belongs to stage L2 (it replaces the manual `ACNH_LDN_PEER`); J2 is its own stage between L2 and L3.

## 5. Risks in one place

1. **Pia join needs something we cannot provide** (L1/L2): the only blocker for every option except (B). Known early
   and cheaply.
2. **Replaying apply functions is not deterministic** (L5b): caught per kind by the memory comparison; fallback (D).
3. **The game's commit fails with modified checks** (L4–L6): visible as rollbacks; the test profiles make it harmless.
4. **Memory with two copies** (D1): measured before the two-slot mode is enabled.
5. **A 3.x update** moves every address: all patches check their original word, so the runtime fails loudly instead
   of corrupting code (plan 5.1).

## 8. Progress (2026-10-21)

| Piece | State |
|---|---|
| `runtime/ldn_host.c` (25 `nn::ldn` functions by name), `runtime/socket_host.c` (`nn::socket` by name: Linux errno numbers, virtual 10.114.0.x addresses, 6-byte datagram header), `runtime/coop_lan.c` (discovery on UDP 11452 + offset over every interface's broadcast, loopback and typed addresses; TCP join / node table), `nifm.c` local network mode, `friend:u` 10700-10703, `csrng` | Written. `tests/coop_loopback.c` passes: discovery, join as node 1, ping / broadcast pong, leave. |
| **L1 in the game** (one copy, test profile) | **Passed**: Orville → "I want visitors" → "Fling 'em open" creates the network (`[coop] Hosting iLand Pro: 1 of 8 players, port 11452`). The "I wanna fly" path scans (0 islands when nobody hosts). Found and fixed on the way: `friend:u` 10700, `csrng`, the state-change event the game destroys at `nn::ldn::Finalize`. |
| Two copies (`TOTK_GAME_SLOT_PAIR=a/b`, `ACNH_COOP_PORT_OFFSET=100`) | Slot rules tested (a third game waits, a lone `b` is refused). Both copies ran together (~2 GB resident each), but the session test is **not done yet**: the second window was closed, and the first then stopped in the renderer ("encode stage did not drain in 2 s", two games on one GPU), not in network code. |
| Island browser UI (section 7) | Back end only (`coop_browser_*`, `<profile>/coop-islands.txt`, `ACNH_COOP_PEERS`); the Settings section is not written. |

Test setup: profiles are copies of the island in the scratch folder with `SAVESYNC=0`; the user's own save and app are
not touched (the test binary is `build/acnh-coop`).

### 8.1 First two-copy session (2026-10-21, the user drove both windows)

- Host (copy A) opened the gate; the visitor (copy B, a **clone of the same save**) found the island at once
  ("1 island answering, 1 returned") but the game showed **"there aren't any islands"**: its advertise data carries the
  host's island ID (+0x18, `f7b800e3`, "iLand Pro") and player ID (+0x34), and a clone has the same IDs, so the game
  hides "its own" island [confirmed by the next step].
- With `ACNH_COOP_TEST_ALTER_IDS=1` (diagnostic: one byte of each ID changed in the visitor's scan results) the island
  was listed and the visitor **joined over the port's layer** (node 1, 10.114.0.2). The host's pia then broadcast its
  176-byte packets (header `32ab9864`), all received by the visitor, but the visitor's game never sent a packet and
  left after the timeout. No send was refused by `socket_host.c` (every send is logged), so the visitor's game itself
  discarded them, consistent with the IDs it joined with not matching the host's [expected].
- **Next:** the visitor needs a save of its own (a new island in its profile, or another real save). Two clones also
  share one player ID, which the arrival would trip over anyway.

### 8.2 Visitor with its own identity (2026-10-21)

- `scripts/acnh_save_reid.py` gave the visitor's **test copy** its own island ID, player ID and island name ("iLand
  Two"): it decrypts each `.dat` with `scripts/acnh_save_crypt.py`, finds the Murmur3 checksum regions
  (`acnh_save_regions.c`: 19 regions cover all of `main.dat` in 3.0.0, 2 in `personal.dat`), replaces the IDs, rewrites
  the checksums and re-encrypts. The game loads it, and the host's island is then listed without any trick.
- The join still stops in pia: the visitor receives the host's broadcasts (96 packets, header `32ab9864 89`) and never
  sends one; none of pia's packet-drop branches runs (`ACNH_SITE_PROBE=1a7c7c,1a7e44,1a8018`: crypto parameters,
  decryption, invalid packet: all 0); the host never sends the new node a unicast packet; after the timeout the game
  shows **error 2618-0502** (now logged by an `nn::err::ShowError(ErrorCode)` replacement instead of crashing).
- **Next (static, no flights needed):** follow pia's receive path from `RecvFrom` (main+0xe0xxx socket wrapper) to the
  dispatcher, and the host's reaction to `nn::ldn` state changes (`LdnBackgroundProcessJob`, the node list it reads),
  to see which check both sides fail. Candidates: the NodeInfo fields pia matches (MAC, version, the station's own
  entry), or a packet source check before `PacketReader::BeginDispatch`.

### 8.3 First visit lands (2026-10-21)

- The visitor uses a separate real save ("pantheon", imported into its test profile with `scripts/import_save.py`;
  the port's `account.uid` is the save's UID bytes reversed with the halves swapped).
- Tracing `GetState` / `GetNetworkInfo` showed the cause of 2618-0502: the **visitor never saw itself in the node
  list**. The host sends the NetworkInfo that includes the new station right after accepting it, and that update could
  arrive before `connect_info` marked the station connected, so it was dropped. It is now kept and used when the join
  completes (`ldn_host.c`, `pending`).
- With that, pia talks both ways (the host sends the station unicast packets), the host shows the arrival board
  ("Panthea, arriving from pantheon"), the island streams over (7,000+ packets) and **the visitor lands on iLand Pro**.
  Both copies show the host island's time.

### 8.4 Full session (2026-10-21): **stage L2 passed** on one Mac

The user played both windows: Panthea (pantheon) dropped an item on iLand Pro, the host picked it up, Panthea left
through the airport. No error on either side. The arrival save (14:15:38) and the departure saves (host 14:21:27,
visitor 14:21:56 after landing home) were written by both copies; afterwards all four files decrypt, every Murmur3
region is valid (19 in `main.dat`, 2 in `personal.dat`), and each island records the other (iLand Pro's `main.dat`
mentions pantheon, pantheon's mentions iLand Pro). So the game's two-phase save commit (section 1.2) runs over the
port's layer. Not yet checked: which pocket holds the item (no 3.0 pocket offsets yet), Mac + iPad, ZeroTier.

### 8.5 House editing and joining without the airport: findings so far (2026-10-21)

**Furniture.** A save diff of one moved item (host alone) shows the edit is "clear 4 cells, write 4 cells" of 8-byte
items in a 192-cell-high grid in the second checksum region; nothing else changes. The "rude" message is SYS_Telepathy
005, shown by `Player_FtrActionError.bfevfl` at the entry the code picks (`FtrIndoorOperateError_ExistsVisitor`,
`FtrOperateError_ExistsVisitor`, `FtrPickupError_ExistsVisitor`). main+0x21cc50c decides furniture operations (0 ok,
1 someone else's home in a session, 3/4 another player near; queries main+0x15e7060 / 0x15e7260); 8 callers, among
them the placement checker main+0x21cd2c0 (result into player state +0x66c, copied to +0x20f0). `ACNH_MP_EDIT=furniture`
makes it return 0: picking furniture up with a visitor present then works, **placing from the pockets still shows
005** through a path not yet found (none of the eight name-loading sites ran in one probe round that had no visitor;
to repeat with one). Diagnostics added: `ACNH_TRACE_SITE=<hex>` (one of the `ACNH_SITE_PROBE` sites) prints the guest
call chain and x0-x3 at that instruction; the first trace showed the message coming from the EventFlow interpreter.
`nn::socket::Finalize` now closes the game's sockets (a session that ended abruptly kept pia's port bound).

**Joining without the airport (J2).** `SNPC_dod_11_Airport_LocalVisitor` runs every network step on the
`EventFlowSystemActor`: NetInitLocal, NetFindSessionVillage, OpenVillageList (the game's island list), NetJoinSessionVillage,
NetDemoLock; then `SNPC_dod_30_Airport_DepartVisitor` walks the player through the gate and calls NetEnterVillage
(system actor) for the flight. The host side (`SNPC_dod_10_Airport_LocalHost`) is NetInitLocal, SaveSimpleSave,
NetCreateSessionVillage, OpenVillageGate, all system actor. So a small system script in the RomFS overlay could do a
visit or host from anywhere. Missing: the call that starts a script by name (the loader formats `EventFlow/%s.bfevfl`
at main+0x1085a54; `System_*` scripts are started through {script, entry} tables), and a safe point on the game
thread to call it.

### 8.6 Local Play without the airport (2026-10-21)

**Working on one Mac (two copies):** host from in front of a house, the other copy visits from its own island and
lands at the host's airport, no airport walk on either side.

- `scripts/make_coop_flows.py` (`make romfs-coop`) builds `build/romfs-coop`: two flowcharts of the port's own,
  `Coop_LocalHost` (entry `Coop_Host`: save, NetInitLocal, NetCreateSessionVillage cOpenLocal, OpenVillageGate) and
  `Coop_LocalVisitor` (entry `Coop_Visit`: save, NetInitLocal, NetFindSessionVillage, the game's island list when
  several answer, Dodo Code prompt when needed, NetJoinSessionVillage, NetDemoLock, NetEnterVillage), with only the
  `EventFlowSystemActor`, plus their entries in `EventFlowInfoProduct.byml`. `romfs.c` reads that folder first
  (`ACNH_COOP_ROMFS`).
- `runtime/coop_flow.c` queues one with the game's event request, main+0x107fed0(manager, &request), as the game does
  for `System_GameClose` (main+0x1c63760); called on the game thread from the controller read. Found the hard way:
  main+0x10805a0 is a *synchronous* check-script runner (it refused every event); and the request's player byte must
  be 0 (0xff, "no player", makes every Net action a silent no-op).
- `runtime/coop_ui.m`: **Local Play… (⌘L)** window: status, Host My Island, the islands found (LAN broadcast,
  ZeroTier, other copies on this Mac, ones added by address), Visit (double-click), Add by Address, Remove. Visit
  selects the island in `coop_lan.c` so the game's search returns only it. The window lists no copy of one's own
  island. Diagnostics: `TOTK_INPUT_FILE` lines `coop host`, `coop visit`, `coop flow <flowchart>:<entry>`.
- Not done: the iPad (no Local Play sheet yet; `coop_ui.m` is excluded from the iOS project), leaving from the menu
  (players still leave through the game's "−" menu or the airport), and the user's own app (`build/acnh-native`) is
  not rebuilt.

### 8.7 Furniture with company: unblocked and synced (2026-10-21)

- **Placing from the pockets with a visitor present now works, indoors and outdoors, and the visitor sees the item
  appear live**: the game already replicates item placement; it only refused it. Picking furniture up already worked
  (8.5). Both are `ACNH_MP_EDIT=furniture` (5 words).
- How it was found: the pocket menu stores the refusal reason in a field the message window later reads; a hardware
  write watchpoint on that field (new diagnostic: `watch <hex>` in `TOTK_INPUT_FILE`, armed on every thread by the
  runtime itself, reported by its SIGTRAP handler; an external lldb froze the game) stopped at main+0x3073c84, which
  stores the result of **main+0x3353540**, "may this pocket item be placed here?". It returns 0x10 indoors and 0x11
  outdoors when the session has 2+ players (SYS_Telepathy 005 / 012). Three branches now skip only those two tests.
- Both tests read **the session's player count**, the byte +0x476 of the object behind main+0x4ab1760.
  `scripts/mp_count_checks.py` lists every such read: **369 checks in 328 functions**, the map for lifting the
  other restrictions one feature at a time (museum, Town Hall services, Island Designer, building moves, Lost and
  Found / recycle box, villager reactions...). A blanket "count = 1" is not an option: the game also uses the count to
  run the session (arrivals, the save commit, who simulates what).
- Also: Host / Visit in the Local Play window are refused while a session is running (pressing one again restarted
  local play and broke the session for everyone).

### 8.8 The visitor places and picks up furniture (2026-10-21)

On someone else's island the pocket submenu (built by main+0x307cf70) left out "Place Item" (CmnSubWin 0111) after
asking main+0x2567d40, "is this player on their own island?" (the value the game also exposes as `IsInMyVillage`).
Counters on the menu's eleven calls of it showed three run when the menu opens; those three now answer yes (this menu
only, the player's real location is untouched). With that, Panthea placed furniture on the host's island and the host
saw it; picking up works too. `ACNH_MP_EDIT=furniture` is now 8 words.

### 8.9 Visitor picking up / turning the host's furniture: not yet (2026-10-21)

- Lifting main+0x21cc50c completely (8.5) let a visitor grab furniture, and turning it **crashed the visitor** at
  main+0x3847ca0 (called from main+0x1458fd4): a structure at +0xa0 of the held object is null, used again right after,
  so a null check would not do; the visitor's copy never builds it for the owner's furniture. The patch now lifts only
  results 3 / 4 (the host's "company is near"); result 1 ("not your home", visitors) stays, so the visitor gets the
  game's refusal again. A patch of the grab code's IsInMyVillage call (main+0x2327dd8) did not help and was removed.
- Counting showed none of the 16 IsInMyVillage callers seen on the visitor runs while it presses Y / holds A, so the
  visitor's pick-up refusal is decided elsewhere (expected: the item layer's own owner / "placed vs dropped" rule).
- Next: find who builds the +0xa0 structure when the owner grabs furniture (watch on it, as in 8.7), and decide between
  building it for visitors too and having the host do the grab and replicate it (the design's host-sequenced channel).

### 8.10 Visitor grab / turn / pick up: findings, and the switch to host-authoritative edits (2026-10-22)

- **Grab:** main+0x21cc50c fully lifted again (`furniture`) for host and visitor. The visitor's crash when holding the
  owner's furniture was a failed render-target allocation ("ExtendGBuffer", pool search main+0x3865630 returning null),
  not missing ownership; with the output patch's larger pools (the Mac default) it holds furniture without crashing.
  (Narrowing the patch to results 3 / 4 had also taken result 2 away from the host, who could then no longer turn.)
- **Turn, visitor:** `State::cFtrRotateReady` (0x52) runs main+0x22e4ab8 each push. Gate 1: byte 0x3e of the target
  record copied into the player at main+0x225aeb8 (player +0x191e) is 0 on the visitor (host: set). Ignoring it
  (`cbz w25` at main+0x22e4dd8) reaches main+0x22e552c, where gate 2, main+0x1292974 (the player's action-condition
  graph), answers "back to ready" (1 = turn, 3 = wait). Each layer showed the visitor's copy lacking owner data, so the
  user chose the host-authoritative route: the visitor's refused action is sent to the host's copy, which does it.
- **Where the island's items live:** the outdoor item layer (8-byte cells, 0x600 per column of 192; `0xfffd` extension
  cells carry their column/row offset) has six identical copies in memory, all save-format buffers. Turning the train
  set (item 0x0055, a 3 x 3 footprint) as the host, then saving, changed **nothing** in its cells or anywhere in the item
  layer, so outdoor rotation is stored elsewhere, and the live layer is not one of these copies. Open: find the live field
  item representation (watch on a host turn or pick-up) before the host can apply a visitor's request.
- Tools added: Ghidra project of the game's own `main` (`scripts/ghidra_prep.py`, `scripts/ghidra/*.java`, decompile
  any function in seconds); `memfind` / `peek` / `poke` and `watch` in `TOTK_INPUT_FILE`.

### 8.11 Player coordinates in a session (2026-10-22)

- Mapped in [GAME-MAP.md](GAME-MAP.md) section 5. Each copy moves its own player. The others replay a fixed-size
  state snapshot (244 / 260 bytes, about 20 per second, whether the player moves or not) through the `PlayerNetMove`
  animation state, with their own skeleton.
- The host's copy of a visitor trails her by about 0.2–0.3 s and settles within a unit of where she stops. A
  host-authoritative edit therefore acts on the host's view of her position, which is close enough to pick a tile.
- New tool: `memsnap` (runtime/mem_diag.c) with scripts/memsnap_diff.py.

### 8.12 Visitor museum donations, carried out by the host's copy (2026-10-22): works

The first edit done the host-authoritative way (decision on progress data, 4.1). Enabled with `ACNH_MP_EDIT` group
`museum`.

- **Two script gates, lifted by an overlay.** The overlay lives in `build/romfs-coop/museum/` and the runtime reads it
  only with the group on (scripts/make_coop_flows.py):
  - `SNPC_owl` asks `IsMyVillage` and sends a visitor to small talk (`SNPC_owl_90_OtherTown`). That branch now takes
    the resident's path.
  - `SNPC_owl_01_Museum_Donation` Event55 asks `NetHasVisitor` and refuses **everyone** while the island has a visitor,
    the host too (`SP_owl_00_Common:101`). That branch now goes on to the donation. This file has entry-point
    variables, which the evfl package cannot write, so its case table is patched in place (1 byte).
- **The donation's native code.** `EventFlowActionOwlDonationDone` is found in the action table, keyed by its
  Murmur3 hash, at main+0x1023760. Its execute method is main+0xafb0f0. For each selected item it:
  - checks with main+0x26bd5b0(museum, item), "not yet donated and a museum item";
  - records with main+0x26bdd90(museum, item, donor);
  - computes the donor with main+0x24819f0(player), the player's **resident slot** on this island, or −1 for anyone
    else. With −1 nothing is recorded, so the save format cannot hold a visitor as a donor.
- **The route.**
  1. A stub on the record call (main+0xafb53c, runtime/acnh_patches.c) replaces −1 with the host's slot, kept up to
     date by runtime/coop_edit.c. It records in the visitor's copy, so she sees the exhibit, and queues the item.
  2. coop_edit.c sends the item to the host (an `EDIT` message on the control connection, coop_lan.c).
  3. The host's copy makes the same calls on its own island data and records the item.
- **Tested with two copies:** Panthea donated a bluegill (0x08b8) and a salmon (0x08c2).
  - Both copies' live museum records gained them.
  - After she left, the host's saved `main.dat` held 131 items ending in those two, donor slot 0, dated 2026-10-22.
  - Her pockets lost them as for any donation.
- **Accepted limits:**
  - The plaque credits the host (decision 4.1).
  - Mac only: the stub sits beside the Mac-only probes.
  - If the host's museum already has the item, the host logs it and records nothing, but the visitor's item is still
    gone.

### 8.13 Leave Island and Stop Hosting in the Local Play window (2026-10-22): works

The ⌘L window now has **Host My Island**, **Stop Hosting** and **Leave Island**. Each button is enabled only where it
applies: outside a session, as the host, and as a visitor. Each also has a diagnostic command (`coop stop`,
`coop leave`).

- **Leave Island** runs `Coop_LocalLeave:Coop_Leave` (scripts/make_coop_flows.py):
  1. "Getting ready to return home…"
  2. `NetDemoLock` with Situation 1
  3. the error check
  4. `NetLeaveSessionVillage`

  These are the steps of the − menu's `System_InetClose` `OkaeriVisitor_Core`. Three things had to change before it
  worked:
  - **The menu-choice branch is gone.** `System_InetClose` itself ends on a branch that only that menu sets, so started
    from the window it stops after the message.
  - **It has its own flowchart.** A start from `Coop_LocalVisitor` did nothing while the visit event was still in use.
  - **It runs as the copy's own player.** The request now names this copy's node as the player inside a session
    (runtime/coop_flow.c). As player 0, which is the host, the session lock fails.
- **Stop Hosting** works in two steps:
  1. With visitors present, the host's runtime sends each a `LEAVE` control message (coop_lan.c). Each visitor's copy
     then runs Leave Island, so everyone saves and flies home as normal.
  2. Once nobody is left, the host runs `Coop_LocalClose:Coop_Close`: `NetEndSessionVillage`, then `CloseVillageGate`.
     These are Orville's close-gate steps.

  If a visitor has not left after 2 minutes, the gate stays open and the window says so. Two other ways failed:
  - Ending the session under a visitor gave her "Communication with the destination locale has been interrupted". It
    also left the host dropping later joins until a restart.
  - The − menu's own host ending (`NetDemoLock` with Situation 2) sends visitors "Seems like it's time to go home", but
    then waits on the flight screen: that menu's own code does the rest after its script.
- **Tested with two copies on one Mac:**
  - Leave: Panthea flew home.
  - Stop: Panthea was sent home, the host saved and closed the gate, and both copies went back to idle.

### 8.14 Visitor turn and pick-up: what actually blocks them (2026-10-22)

Measured with counters on the player's state entries on both copies (`ACNH_SITE_PROBE`) and a trap on the disabled
state (`ACNH_TRACE_SITE`). Corrections to 8.10 first.

- **State ids.** In the registration code (main+0x2286xxx–0x2287xxx) each state's name string comes **before**
  `mov w1, #id`. The state-change call main+0x3db9620 takes **id − 1**, as do the enter functions'
  main+0x2274140(player, id − 1).

  | State | Id | Enter |
  |---|---|---|
  | cPickupWait | 0x23 | main+0x22918c0 |
  | cPickup | 0x24 | main+0x2291a64 |
  | cFtrPickup | 0x26 | main+0x2291dd4 |
  | cFtrHold | 0x48 | main+0x22957ac |
  | cFtrRotateReady | 0x52 | main+0x2296bec, run main+0x2296c2c → main+0x22e4ab8 |
  | cFtrRotateDisable | 0x53 | main+0x2296d94, run main+0x2296dd4 → main+0x22e552c |
  | cFtrRotate | 0x55 | main+0x22970e4 |

- **Players' actions travel as states.** main+0x2259760 fills the player's network state record at player+0x18e0:
  player number, stage, state id, position (x×1000, y×4, z×1000 as integers), angle, speed and flags. Every copy runs
  the other players' states itself; state counters on the host's copy step exactly with Panthea's. So when a player's
  own copy turns furniture, the others replay the turn.
- **Turning.** Panthea goes hold → rotate-ready → rotate-disabled, every time. The byte check in main+0x22e4ab8
  (player+0x191e) is not her gate: she already takes the normal path there. A patch forcing it changed nothing, and was
  removed. Her gate is main+0x22e552c, which:
  1. marks her entry in the per-player session table, `*(main+0x4ab3f08)+0x20+player*0x740`, as requesting
     (+0x5b4 = 2);
  2. polls the request at +0x658 with main+0x1292974, a job in the table under `*(main+0x4abb1a8)` (0x518-byte
     entries, state at +0x2c): 1 granted → cFtrRotate, 3 pending → wait, otherwise failed → back to ready.

  **Turning furniture with company needs a lock that is asked for over the network, and Panthea's request fails.**
  Forcing "granted" on her copy (8.10) could not work: no copy holds the lock. Next: find where the request is
  answered (expected: the host's copy, refusing a visitor) and grant visitors there.
- **Y pick-up.** The host's Y on placed outdoor furniture enters cPickupWait (0x23). Panthea's Y enters no pick-up
  state at all, so her refusal is decided before any state change; it is probably the same lock request, still to
  confirm.
- **More on the "lock" (same day).**
  - The request slot +0x658 of the per-player session table is polled the same way before every furniture action:
    hold, push, pull, turn, and the pick-ups (main+0x22de49c to main+0x22e5be8).
  - The table it lives in, `*(main+0x4abb1a8)` (= main+0x50a8970), is managed by code next to the script runner's
    (strings "Root", "AI_NPC_", "NNPC_Talk_Sequence" at main+0x128af5c). So the answer may come from a script rather
    than from the network; open.
  - `Player_FtrActionError` only holds the refusal messages (SYS_Telepathy 005 / 013 / 014 …), not the decision.
  - The "lock" reading above is therefore still a hypothesis. Next: trap main+0x1292974 on the visitor to get the job's
    script name, or watch the job's state word (+0x2c of its 0x518-byte entry) to find what writes "failed".
- **Seen by the user:** when the host moves furniture, the host's screen shows the smooth push, but on the visitor's
  screen the furniture jumps to its new place while the host's character plays the animation.

### 8.15 Visitor turns and Y pick-ups of the owner's furniture: works (2026-10-22)

Found by two read-only agents working from the decompiled code (notes kept outside the repo), then tested with two
copies. **Panthea turned the host's train set and picked up a placed item with Y.** Enabled with `ACNH_MP_EDIT`
group `visitor-turn`.

- **The "lock" is the field-action job system.** It is a singleton at `*(main+0x50a8970)` (created by main+0x1290c84),
  and is neither the script runner nor the AI. Every furniture action starts a job first:
  - turn: main+0x21cd2c0 (`entry+0x658`, direction)
  - push: main+0x21cb9e4
  - pull: main+0x21cc798
  - Y: main+0x21cdc60

  On a visitor's copy the job asks the host (command 1). The host answers granted, or denied when the furniture is
  already locked (main+0x1296670). The job state (+0x2c) reads 0 = no job, 1 = granted, 2 = denied, 3 = pending.
- **Why a visitor got nothing: two IsInMyVillage tests stopped her copy before any job started**, so no request ever
  reached the host:
  - The placement check main+0x1412c8c (reached through main+0x1375040; used by turn, push and pull) requires
    IsInMyVillage on maps with flag bit 3.
    Patch: main+0x14130ec `tbz w0, #0` → `nop`.
  - main+0x140ea70 (may this item be picked up; used by Y) answers no for "not my village and furniture-class".
    Patch: main+0x140efbc `tbnz w0, #0` → `b`.

  Both tests are always true on the island's own copy, so the host's behaviour does not change. IsInMyVillage itself
  stays as it is: it has about 200 callers.
- **Corrections to 8.14:**
  - Player byte +0x191e is not furniture data. It is byte 0x3e of the action record, "the job was started", written
    in hold-wait. Its 0 on the visitor was the result of the failed start, so the patch at main+0x22e4dd8 was dropped.
  - The state ids are 0-based, with the name table at main+0x4807320: cFtrRotateReady 0x51, cFtrRotateDisable
    0x52, cFtrRotateReadyWait 0x53, cFtrRotate 0x54, cPickupWait 0x22, cFtrPickup 0x25.
- **Still to check:**
  - that the host's copy shows the change and keeps it after the host saves;
  - that the picked-up item stays in Panthea's pockets after she leaves;
  - how placed furniture is stored live (the third agent stopped before finishing).
- **Confirmed by the user:** the visitor's turns and Y pick-ups show on the host's screen with their animation.
  **Pushing or pulling (A)** moves the item on both copies, but on the other player's screen it jumps to its new place
  while the player plays the push animation. This happens both ways, host and visitor, so the replay of push / pull
  does not run the item's slide. Next to look at: the push and pull states (ids 0x49–0x50) and what their enter / run
  do when the player is remote (+0x178 == 1), compared with the turn's.
