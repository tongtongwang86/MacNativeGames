# Local multiplayer and island editing with visitors: plan

> **Status:** plan only, written 2026-10-20. No code changed and no game started.
> Each statement is marked **[confirmed]** (read from this repository, the 3.0.0 image or its romfs) or
> **[expected]** (from knowledge of the game or the SDK, still to be checked).

The goal has two parts:

1. Local play between copies of the port (Mac ↔ Mac, Mac ↔ iPad) over the LAN, through the game's own "Local Play"
   at the airport.
2. Removing the "not while visitors are here" restrictions so that the host and the visitors can change the island
   during a session.

Part 2 depends on part 1, and its real risk is not the checks themselves. It is whether the game's network protocol
can carry the changes the checks block (section 4).

---

## 1. Prerequisite: local play in the port

### 1.1 What the game uses [confirmed]

- **main imports `nn::ldn` by name (25 functions).** These are Initialize, Finalize, GetState, GetIpv4Address,
  AttachStateChangeEvent, GetNetworkInfo, Open/CloseAccessPoint, Open/CloseStation, SetStationAcceptPolicy,
  SetAdvertiseData, MakeBroadcastAddress, GetSecurityParameter, GetDisconnectReason, CreateNetwork(Private),
  DestroyNetwork, Scan(Private), Connect(Private) with ConnectOption, GetNetworkConfig, Disconnect and Reject. It
  imports no ActionFrame or Monitor functions.
- **main imports about 38 `nn::socket` functions by name.** These are Initialize(void*,size,size,int), Finalize,
  Socket, Bind, Connect, Listen, Accept, Send/Recv/SendTo/RecvFrom, Poll, Fcntl, Get/SetSockOpt, GetSockName,
  Shutdown, Close, GetLastErrno/GetLastError, the Inet* helpers, GetAddrInfo and FreeAddrInfo (both overloads).
- **main imports `nn::nifm` by name.** These are Initialize, SetLocalNetworkMode(bool), SubmitNetworkRequest(AndWait),
  CancelNetworkRequest, IsNetworkAvailable, IsNetworkRequestOnHold, HandleNetworkRequestErrorResult,
  GetCurrentIpConfigInfo, GetCurrentPrimaryIpAddress and GetCurrentProxySetting.
- **The sdk module exports these names.** Its strings name the services behind them: `ldn:u`, `bsd:u`, `nifm:u`,
  `sfdnsres`, `nsd:u`.
- **pia is linked into main with its RTTI.** It has the namespaces `nn::pia::local` (67 classes, including
  `LdnNetworkFactory`, `LdnCreateNetworkJob`, `LdnScanNetworkJob`, `LdnInputStream`/`LdnOutputStream` and
  `LdnHostMigrationJob`), `lan`, `mesh`, `session`, `transport` and `clone` (EventProtocol, ClockProtocol), plus
  `nn::nex` for online play. The transport protocols are `ReliableProtocol`, `BroadcastReliableProtocol`,
  `StreamBroadcastReliableProtocol` and `UnreliableProtocol`. The game's own classes have **no** RTTI.
- **The runtime can replace an imported function by name.** In `loader.c` `relocate()`, `runtime_override(name)` goes
  through `runtime_hle_wrap_named`, as `applet_host.c` does for `nn::nfp`, `nn::oe` and `nn::prepo`. This happens at
  relocation (in data), so it **works on iOS unchanged**: no code patch is needed.
- **The IPC registry is a fixed array of 32 services** (`ipc.c`), with about 14 in use. An unknown service makes
  `sm:` fail the runtime. Today `nifm.c` answers every request with NoInternetConnection.

### 1.2 Design: replace nn::ldn and nn::socket by name, not as IPC services

acnh-recomp's `LOCAL-WIRELESS.md` designed `ldn:u` and `bsd:u` as IPC services, because the recompiler had to serve
the SDK's IPC. Here, main calls the SDK functions by name, so the simpler and smaller route is to **replace those
functions directly**, the way `applet_host.c` replaces `nn::nfp`:

| New file | Replaces | Notes |
|---|---|---|
| `runtime/ldn_host.c` | the 25 `nn::ldn::*` imports | A state machine: None → Initialized → AccessPoint/Station → AccessPointCreated/StationConnected. It signals the SystemEvent passed to AttachStateChangeEvent (use the same `nn::os` event helpers the `nfp` replacements use). It fills `NetworkInfo` (0x480 bytes: common part, LDN part, 8 × 0x40 NodeInfo, advertise data up to 0x180). Layouts come from libnx `ldn.h` and Ryujinx, checked with `static_assert`. |
| `runtime/socket_host.c` | the ~38 `nn::socket::*` imports | Maps calls to macOS sockets. Switch `sockaddr_in` already has BSD `sin_len`. It translates errno (Switch = FreeBSD numbering, close to Darwin but not identical: check each value the game tests, at least EAGAIN/EWOULDBLOCK, EINPROGRESS and ECONNRESET), `fcntl` O_NONBLOCK, `MSG_*` flags, `SOL_SOCKET` options and `poll` events. `GetAddrInfo` only resolves numeric addresses and fails anything else, which keeps the game off the internet. It keeps the virtual-address table (1.3). |
| `runtime/nifm.c` (extend) | `nn::nifm::SetLocalNetworkMode` etc. by name, *or* the IRequest requirement in IPC | Answer "available" **only** when local network mode was requested. Internet requests keep failing with NoInternetConnection, otherwise the game would start NEX and online flows. First see which path 3.0.0 takes (`TOTK_IPC_TRACE=1`): if the requests still reach `nifm:u`, record SetRequirementPreset / SetLocalNetworkMode in IRequest and let Submit end Accepted for local requests. The exact state values have to be confirmed. |
| `runtime/ldn_lan.c` | (no game counterpart) | Discovery and session control between hosts (1.3). |

Two smaller points:

- **`friend:u`, `pctl`, `prepo`.** `nn::friends::DeclareOpen/CloseOnlinePlaySession` are imported, and pctl's
  `CheckFreeCommunicationPermission` already passes. Any new IPC command the game sends during local play shows up
  as an `ipc_unimplemented` failure with its id. That failure is the intended way to find what is missing.
- **Why not IPC.** Serving `ldn:u` and `bsd:u` would also mean CMIF buffer marshalling, bsd:u's transfer memory and
  `sfdnsres`. Nothing in main needs those once the named functions are replaced. If an sdk function that is not
  replaced turns out to call into `bsd:u` internally (for example `nn::socket::Initialize`, if it is left alone), the
  `sm:` "service not implemented" failure names it.

### 1.3 Discovery, session control and addresses

- **Protocol.** Use the ldn_mitm / Ryujinx LAN protocol as acnh-recomp planned: UDP broadcast scan on port 11452,
  hosts answer with their `NetworkInfo`, and a TCP connection per station carries join, leave and node-list updates.
  The protocol is reimplemented from its description, with no GPL code copied. Talking to Ryujinx or ldn_mitm is a
  stretch goal and works only on the same game version (3.0.0).
- **Addresses: a virtual LDN subnet inside `socket_host.c`.** The game only ever sees addresses that `ldn_host.c`
  hands out. Each node gets 10.114.0.*n*/24, `GetIpv4Address` returns it, and `MakeBroadcastAddress` gives
  10.114.0.255. `socket_host.c` maps them:
  - `bind(10.114.0.n:p)` becomes `bind(0.0.0.0:p + n·stride)`;
  - `sendto(10.114.0.m:p)` becomes `sendto(<host of node m>:p + m·stride)`;
  - a send to the broadcast address goes out once to each member as unicast;
  - `recvfrom` maps the source back to 10.114.0.m:p.

  This one mechanism covers three cases:
  1. **Two copies on one Mac.** Both are on 127.0.0.1 and differ by their port offset. macOS has no 127.0.0.2
     without `sudo ifconfig lo0 alias`.
  2. **Mac + iPad.** Unicast only. iOS needs the restricted `com.apple.developer.networking.multicast` entitlement
     for broadcast and multicast.
  3. **Different subnets.** The host can be given by IP.

  Membership, and with it the node→host table, comes from the TCP control channel. The `stride` only matters when
  several copies share one host (one Mac); across machines it can be 0.
- **Finding a host when broadcast is impossible (iPad).** Provide `ACNH_LDN_PEER=<ip>[:port]` (scan that address by
  unicast) and, later, Bonjour (`_acnh-ldn._udp`, NWBrowser). Bonjour needs `NSBonjourServices` and
  `NSLocalNetworkUsageDescription` in the app's Info.plist, but not the multicast entitlement [expected; check against
  Apple's current rules].
- **macOS firewall:** allow incoming connections for `acnh-native` on first use.

### 1.4 Testing with two copies

| Need | How [confirmed: these hooks exist] |
|---|---|
| Separate account and save | `TOTK_PROFILE_DIR=user-data-b` (account.uid, `save/<uid>`, `save/device`); `ACNH_NICKNAME=Visitor` |
| A second island | The visitor needs its own island with a working airport. Either import a second save (`scripts/import_save.py --uid`) or start a new game in the new profile and move the clock forward with `TOTK_TIME_OFFSET` (seconds) past the first day [expected: Local Play is offered from day 2]. Mac and iPad sharing one island through SaveSync is **not** a valid pair: turn SaveSync off for the test or use a different profile on one side. |
| Two games at once | **Blocked by policy today.** `game_slot.c` lets only one game run on this Mac, because exhausted memory panicked the kernel four times on 2026-10-12. Its comment says 16 GB, but this Mac (Mac14,5) reports 96 GB. Running two copies needs **the user's decision**, then an explicit two-slot mode (for example `TOTK_GAME_SLOT_PAIR=1`, which one wrapper takes for both copies). Never get around it with a second `TOTK_GAME_SLOT_DIR`. Both copies also share one GPU, so frame times mean nothing during such a test. |
| Shared files | `build/shader-cache` would be written by both copies: check that the cache writes are atomic (write to a temp file, then rename), or give the second copy a read-only cache. The log goes to stderr per process. |
| Mac + iPad | Mac host and iPad visitor first. `ACNH_LDN_PEER` on the iPad, or Bonjour later. iOS asks for Local Network permission on first use. Code patches (section 5) must be in the IPA, because iOS cannot patch at run time. |

**What can be checked at each step of part 1:**

1. One copy: Dodo offers "Via local play" (`dod/SP_dod_99_Select` 001_a) and opening the gate succeeds. This is
   `CreateNetwork` → AccessPointCreated with the advertise data stored. Opening the gate alone already turns on the
   "gate open" restrictions, so part 2 can start on a single copy.
2. A UDP test tool (Python, no game) sees the host's scan answer, parses its `NetworkInfo` and shows the island name
   in the advertise data.
3. Two copies: the visitor's scan lists the host's island; Connect; pia's join (`LdnBackgroundProcessJob`,
   `JoinSessionJob` names appear in its log strings); the visitor lands.
4. A session test: drop and pick up an item, chat, leave and come back, save on both sides, restart both and check
   both islands.

Online play (NEX, Nintendo's servers) stays out of scope.

---

## 2. The restrictions

Found by dumping every USen MSBT (`Message/*_USen.sarc.zs`, 120,493 messages) and searching for visitor, gate, guest,
company and Best Friend wording, then reading the hits. Message ids are `file|label`. **[confirmed]** means the text
exists; whether a block is on the host's side or the visitor's side comes from the wording and is **[expected]** until
the code is found.

The game has **three host-side states** that trigger blocks (SYS_PhoneApp 212/213/214, 340–342, 413–415 say so
explicitly):

- **G**: the airport gate is open (a session exists, possibly with nobody else on the island);
- **V**: company is visiting;
- **C**: "communications are active", meaning local or online networking is still on after the gate closed.

Some blocks are also **proximity-based**: another player is near the spot.

### 2.1 Host side: on your own island while others are, or could be, there

| # | Restriction | Messages | State |
|---|---|---|---|
| H1 | **Island Designer app** (terraforming: cliffs, water, paths) | SYS_PhoneApp 212, 213, 214; Island Designer permits are valid only on your own island (241_02); 220_01/220_06: "only the cleanup service… cannot be used while playing with others" | G, V, C |
| H2 | **Fences** | SYS_Telepathy 023 ("You have visitors, so maybe don't build any fences"), 417 | V (417 generic) |
| H3 | Ground items that need setup: **simple panels, vines and ladders on cliffs**, and items to "set up later" | SYS_Telepathy 020, 021, 024, 033, 037, 041 | V |
| H4 | **Decorating Mode** outdoors and in houses ("Use A or Y to redecorate instead") | SYS_Telepathy 1111 | V |
| H5 | Planting, burying and resizing trees **near other players** | SYS_Telepathy 030, 031, 042 | proximity |
| H6 | **Moving a house or building, using kits**, construction sites | SYS_Player 920_01 (someone inside), 920_02 (others near the site), 933 | proximity |
| H7 | **Tom Nook services** (building moves, bridges and inclines, plots for new villagers, house upgrades) | rco/SP_rco_00_Common 801, 802_02 ("abuzz… off-island company"); rco_14 101, 102 (remodelling a villager's house: close the gate first) | V, G |
| H8 | **Villager move-outs and move-ins** | No dedicated text found. Expected to go through Tom Nook (H7), the campsite, and villagers in boxes. A visitor can already adopt a villager who is moving out [expected, from game knowledge] | (V) |
| H9 | **Resetti / the cleanup reset** | sza v2 104, mol/SP_mol_FieldTidying 203–206, SYS_Player 2101/2102 | G, V |
| H10 | **Custom designs**: editing, downloading, the portal | SYS_Telepathy 463, 467, 474, 600, 601, 603; SYS_PhoneApp 340–344; OBJ_WorldMyDesignGallery 804 | G, V, C |
| H11 | **Nook Shopping orders** | DIALOG_Msg 1123 | G |
| H12 | **amiibo** at the Nook Stop | OBJ_RcoMachine 543 | G |
| H13 | **Museum donations** | owl/SP_owl_00_Common 101, 210 | V |
| H14 | **Furniture polishing** stops when visitors come | DIALOG_Msg 5211; SYS_Telepathy 064 | V |
| H15 | **Megaphone** | SYS_Player 2004–2006 | V, C, G |
| H16 | **Happy Home Network app** (2.0 HHP companion) | SYS_PhoneApp 413–417 | G, V, C (and while visiting, or in Party Play) |
| H17 | **Hotel guest rooms (3.0)** and Kapp'n's family | SYS_Player 2201/2202; kpm 7001; kpg 055_02, 109 | V, G |
| H18 | Rescue service (warp home) | SYS_PhoneApp 012_01, SYS_PhoneApp2 012_01 | V |
| H19 | Removing a Best Friend while playing together | DIALOG_Msg 1412 | V |

### 2.2 Visitor side: on someone else's island

| # | Restriction | Messages |
|---|---|---|
| V1 | **Tools reserved for Best Friends.** Axe, shovel, and the like [expected; the text names no tool]. | SYS_Telepathy 413 ("only use this tool when you're on a Best Friend's island") |
| V2 | **Planting** without Best Friend status | SYS_Telepathy 415 |
| V3 | **Placing** certain items without Best Friend status | SYS_Telepathy 420 |
| V4 | Picking flowers, fruit and the like without Best Friend status | No text found. In the original game flowers can be picked and fruit shaken by anyone [expected]. Leave it out until the code shows a check. |
| V5 | **Island Designer** on another island. The app is "outside the service area" there, and permits are for your own island only. | SYS_PhoneApp 000, 209, 241_02, 311 |
| V6 | **Customising** with custom designs; home storage not usable while away | FTR_WorkingTable 022; DIALOG_Msg 10101 |
| V7 | Custom Designs Portal and HHN app while visiting | SYS_PhoneApp 343, 416 |

**Best Friend status cannot exist in this port** [expected]. It needs the Switch friend list and an online connection
(DIALOG_Msg 1457 "connect to the internet before using the Best Friends List"), and `friend:u` here has no friends. So
V1–V3 have to be lifted by a patch. Making the peer a "Best Friend" through `friend:u` and the save's list would be a
much larger change.

### 2.3 Not found in the data, and out of scope

- **Happy Home Paradise.** The archipelago itself is a separate single-player area. Only the HHN app (H16) and Kapp'n's
  hotel (H17) mention company. Travel to Paradise Planning while the gate is open is probably refused by Dodo
  [expected]; check in game.
- **Parameter tables.** `FieldLandMakingError.bcsv` (18 terraforming error rows) has no multiplayer row. No other
  `Bcsv/*.bcsv` has a column or value about visitors or Best Friends. The checks live in **code and EventFlow**, not
  in data [confirmed for the tables searched].
- Party Play (several residents on one console), Dream / Slumber Islands, and anything online.

---

## 3. Finding the code that enforces each restriction

### 3.1 Anchors that exist

- **EventFlow (romfs `EventFlow/*.bfevfl`, 876 files) [confirmed].** NPC and object dialogs decide with named
  queries: `EventFlowQueryNetHasVisitor` (53 uses), `EventFlowQueryIsVillageGateOpened` (22),
  `EventFlowQueryIsSharePlay` (52), `EventFlowQueryNetCheckSessionType`, `EventFlowQueryNetAllSessionIsFull`,
  `EventFlowActionNet{Create,Join,Leave,End}SessionVillage`, `EventFlowActionCheckFreeCommunication`, and the
  `IsPlayingWithOther` argument (36 flows: Tom Nook, Lottie, Kapp'n family, HHP NPCs and others).
  - `NetHasVisitor` appears in `AI_NPC_Rco`, `SNPC_rco`, `SNPC_mol`, `SNPC_owl_01_Museum_Donation`,
    `SNPC_sza_00_Common`, `Obj_RcoMachine_Amiibo`, `Player_ConstructionLicenseApp` (Island Designer),
    `Player_ToolActionError`, `Obj_WorldMyDesignGallery`, `Ftr_Working` and more.
  - These class names are **not** strings in main (they are presumably hashed; a quick search for CRC32 of the full names as
    `movz/movk` immediates found nothing, so the hash function or its storage is still unknown). `IsPlayingWithOther` **is** a string at main+0x3f800b8, used once at main+0x1009614 (an AI node
    constructor).
- **Player "telepathy" errors.** `Player_ToolActionError.bfevfl` uses `NetHasVisitor` and is the expected route for
  SYS_Telepathy 023/033/037/041/413/415/420/1111.
- **Message-file strings [confirmed].** `SYS_PhoneApp` (main+0x3fddf98) is referenced once (main+0xc63d30, inside a
  generic "show phone-app message" helper); `SYS_Telepathy` (0x3fa1342) once (0x2cfd550); `SYS_Player` (0x3f9f941)
  once (0xd14e10). `ChkGateOpen` (0x3f7c1a3) and `EquipDustClothWithGateOpen` (0x3fb59db) are further string anchors.
  Labels such as "212" are passed to these helpers (as text or numbers), so the callers that pass 212, 213 and 214
  are the Island Designer check.
- **pia RTTI [confirmed].** vtables for `nn::pia::transport::*Protocol`, `clone::EventProtocol` and
  `local::Ldn*Job` can be found from their `_ZTS`/`_ZTI` strings. These are the ways into the network code
  (section 4).

### 3.2 Expected shape of the checks

Under the dozens of checks there is probably a small number of **session-state queries**: "is a network session
running" (C), "is the gate open" (G), "number of other players > 0" (V), "is the player on someone else's island" and
"is the island owner my Best Friend". EventFlow's `NetHasVisitor` and `IsVillageGateOpened`, the phone app and the
tool-error code should all call the same few functions [expected].

Patching **at the callers** that block an action is preferred to changing what the queries return. Faking "no
visitors" would also break the game's correct behaviour (arrival sync, saving at departure, NPC dialogs).

### 3.3 Method and tools (all static work can run while the game is busy elsewhere)

1. **Data first.** Keep the MSBT dumper and the xref finder as reproducible scripts. They were written for this plan
   and are in the shared notes folder as `msbt_dump.py` and `xref.py`; move them to `scripts/` when the work starts.
   - `msbt_dump.py` turns each language's archive into a TSV: file, label, text.
   - `xref.py` finds ADRP+ADD references to an address in main's text, and `movz/movk` pairs that build a 32-bit
     constant. It uses numpy and scans 66 MB in under a second.
2. **From message to caller.** Follow the helpers above to their callers (capstone in `.venv`, `make dependencies`),
   and find where labels 212/213/214, 023, 413/415/420, 1111 and so on are chosen. Walk up one or two frames to the
   condition (`bl <query>` → `cbz/tbnz w0`). Record each site as `offset, original word, meaning`.
3. **From EventFlow to query.** Find the EventFlow query registration table: a list of {hash, factory} pairs in
   .data, usually next to the `IsPlayingWithOther` node. The hash of `NetHasVisitor` leads to its query class, and
   that class's `Evaluate` method leads to the shared session-state function. Its callers are then the list of every
   check.
4. **At run time, while the slot is free.**
   - `ACNH_SITE_PROBE=<off>,<off>` counts how often an instruction runs (every 5 s in the log). Use it to see which
     candidate runs when the "can't use Island Designer" message shows (single copy, gate open).
   - lldb with **hardware** breakpoints (`breakpoint set -H -a <main+off>`; main's base is in `[acnh] main at`).
     Software breakpoints cannot be used because the code pages are sealed. Watchpoints on the session-state flags
     find who writes them.
   - `TOTK_IPC_TRACE=1` for the service side.
5. **Without a second copy**, everything in state G (gate open, nobody visiting) can be found once part 1 step 1
   works. That covers H1, H9–H12 and part of H15/H16. States V and C, and the visitor side, need two copies.

---

## 4. The real risk: can the protocol carry the changes?

### 4.1 What is known

- **Visible in normal play [expected, from game knowledge].** Dropped and picked-up items, holes, trees cut down,
  fruit, flowers and weeds are seen by everyone in real time. So item-layer and some terrain-object changes have
  messages.
- **Arrival and departure freeze everyone** ("…is arriving") and save. A visitor that is busy blocks it: DIALOG_Msg
  805 "forcibly ended… save data will be reverted"; dod 117_01/118_01 [confirmed text]. This is when the island state
  is copied to the visitor.
- **What the game blocks is telling.** Terraforming, fences, cliff-mounted items, building moves and Decorating Mode
  all change the **terrain, structure or collision layers**, not the item layer. That points to these edits having
  **no mid-session messages** [expected]. Lifting only the checks would then leave the host with a changed island and
  the visitors with the old one. Collision and pathing would differ, and an item later placed on a spot that only one
  side thinks is valid can be refused, misplaced, or crash the visitor.
- **Visitor-side editing of the host's island (V5) is a different problem.** The host owns the save, so any edit a
  visitor makes has to travel to the host and be applied there, or it is lost when the session ends.

### 4.2 How to find out, before patching anything big

1. **Catalogue the messages.** Use `ACNH_SITE_PROBE`, or a logging stub on `ReliableProtocol` /
   `BroadcastReliableProtocol` / `clone::EventProtocol` send and receive (found through their vtables). Log size and
   the first 16 bytes of each game message to a file while doing each allowed action in a two-copy session: drop an
   item, dig a hole, plant, cut a tree, shake a tree. The first bytes give the game's message-type ids.
2. **Read the receive dispatcher.** From the receive callback, find the switch on message type and list every type
   with its handler. For each handler, note which island-state layer it writes: items, terrain, buildings or
   players. The question this answers is whether a type exists for terrain heights, paths, water/cliffs, fences and
   building positions. Island Designer edits on the host's own island are local single-player code; the code should
   show whether they also go through a "send" function.
3. **Experiment, in the safest order.**
   1. Lift H1 only in state G with nobody there. Edit, then let a visitor arrive. If the join copy carries the edit,
      the visitor sees it, and state G is simply safe to unlock.
   2. Then do the same in state V with one visitor watching, and record what each side sees.
   3. After each experiment, save on both sides and compare the host's `main.dat` with the visitor's in-memory copy.
      NHSE-style decryption gives the offsets of terrain, the item layers, buildings and fences.

### 4.3 Fallbacks if an edit has no message

From least to most invasive:

- **A. Only allow it while nobody is on the island.** Unlock state G and C, but keep V. This is already a real gain:
  the gate can stay open while the island is edited. Visitors get the new island when they arrive.
- **B. Re-sync by re-joining.** After a structural edit on the host, send each visitor home and back
  automatically. A runtime-side "rejoin" goes through the normal departure and arrival flow, which re-copies the
  island. It needs no protocol knowledge, only a trigger (the departure path a visitor uses when they press "−").
  It is slow, about 20–40 s per visitor.
- **C. Re-send the island state mid-session.** Call the game's own join-time "send island" routine again for players
  already present, if the visitor side accepts a second copy. The code has to show whether that is possible; it is
  likely that only a freshly arriving visitor accepts it.
- **D. A side channel in the runtime.** `ldn_lan.c` already has a TCP connection between instances. The host's
  runtime could send changed terrain and structure blocks of its save image, and the visitor's runtime could write
  them into the visitor's copy of the host island and ask the game to reload the field. This needs the in-memory
  layout and a reload entry point. It is the most work and the most fragile, so it is the last resort, also for
  visitor-side editing (V5), where it would have to run the other way.

The recommended scope follows from this: host editing in states G and C first, visitor Best Friend tools next, and
live host editing in state V only for what the protocol carries (or with fallback B). Visitor terraforming stays a
stretch goal.

### 4.4 Risk tiers (expected, to be confirmed by 4.2)

| Tier | Restrictions | Why |
|---|---|---|
| 1, likely already carried | V1–V3 (Best Friends already can), H4 (Decorating Mode is UI over item placement), H5 (proximity rules), H14, H15, H18, H19 | The action itself is already replicated for permitted players |
| 2, local to one player | H10–H12, H16, H17, V6, V7; H13 (museum) and H9 (Resetti) change shared buildings and objects | They change one player's data or a building interior that visitors may be standing in: check H13 and H9 for desync |
| 3, structural | H1, H2, H3, H6, H7, H8, V5 | Terrain, structures and collision: probably no mid-session messages |

---

## 5. How the patches are applied

### 5.1 Pattern [confirmed from acnh_patches.c]

- **Order.** `runtime_game_patches()` runs on main after relocation and before its text is sealed. Each site first
  checks its original word and fails loudly if the word differs, as the aspect sites do. `speedfix_apply` runs first.
  New sites must not overlap the SpeedFix word list, the aspect sites or any rewritten site (`rewritten/main.sites.tsv`).
  Check this in a script, as `speedfix.py` does.
- **Code caves.** Stubs live in main's text padding between `text_end` and `ro_off`. That space is small (the aspect
  stubs use 240 bytes, then `ACNH_SITE_PROBE`), so prefer **one-word patches**: `cbz`→`b`, `cbnz`→`nop`, or
  `bl query`→`mov w0, #0` at the caller.
- **Code on iOS.** `export-ios` writes main's patched text (up to `text_end` rounded up to a page), and the app runs that
  as code. Every code patch active at export time is therefore baked into the IPA. Data-page values written by the
  host at run time still work on iOS (the aspect stubs read theirs).

### 5.2 Keeping it switchable

- **Mac:** `ACNH_MP_EDIT=0|host|full`, read once in `runtime_game_patches`.
  - `0`: game behaviour (the default until the work is verified).
  - `host`: lift the host-side G/C checks and tier-1 checks.
  - `full`: everything verified so far.

  Each patch is a table row `{offset, original, patched, group, what}`, like `aspect_reads`, so a 3.x update fails at
  the check instead of corrupting code.
- **iOS:** one-word patches cannot be switched at run time on iOS. Either:
  - `build_ipa.py --mp-edit=<level>` bakes a level in (two IPAs if both are wanted), or
  - for the few central sites, use a stub that tests a flag in the data page (as the aspect stubs do), so the same
    IPA can be switched in settings. This costs about 24 bytes of padding per site.
- **The default stays off** until the stage that unlocks it has passed its check below. The `README.md` table gets a
  row per stage.

### 5.3 Stages, with what can be verified at each

| Stage | Work | Verified by | Needs |
|---|---|---|---|
| S0 | Scripts: `msbt_dump.py`, `xref.py`, and a patch-overlap checker (in `scripts/`) | Reproduces the tables in section 2; overlap check passes against SpeedFix, aspect and rewrite sites | nothing (static) |
| S1 | `ldn_host.c` (access-point half), `socket_host.c` (UDP/TCP core), nifm local mode | One copy: "Via local play" works, the gate opens and closes, no `ipc_unimplemented`; a Python test client finds the host by scan | the slot, briefly |
| S2 | Static RE of states G/C: Island Designer (H1), Resetti (H9), designs (H10), orders (H11), amiibo (H12). Patch group `host-gate` | One copy with the gate open: each action works; close the gate, save, reload: edits kept. `ACNH_SITE_PROBE` shows the patched branch taken | S1 |
| S3 | `ldn_host.c` station half, `ldn_lan.c`, the virtual subnet | Two copies (after the user decides on the slot) or Mac + iPad: arrival, item drop and pickup, chat, departure, saves on both sides | S1; slot decision; second island |
| S4 | Message catalogue and receive dispatcher (4.2 steps 1–2) | A written list of message types and the layers they write. The **go / no-go for tier 3** | S3 |
| S5 | Tier 1: visitor Best Friend tools V1–V3, Decorating Mode H4, proximity H5. Patch group `tier1` | Two copies: the visitor cuts, digs and plants, the host sees it; both saves are consistent after the session | S3, S4 |
| S6 | Tier 2, case by case, especially H13 and H9 | Two copies: museum and reset effects identical on both sides | S5 |
| S7 | Tier 3 according to S4: either enable live (if messages exist) or fallback A/B | Host terraforms with a visitor present; after the edit (or after re-join) the visitor's collision matches. Walk the edited spots, then save on both and compare | S4 decision |
| S8 | iOS: Local Network keys, `ACNH_LDN_PEER`/Bonjour, `--mp-edit` in `build_ipa.py` | iPad visitor to Mac host, then the other way round | S3, S5 |
| S9 (stretch) | Visitor terraforming V5: needs messages from visitor to host, or fallback D | Only if S4 shows a way | S7 |

### 5.4 Open questions for the user

1. **Two games at once on this Mac**, for S3 onwards. This changes the game-slot rule (needs a two-slot mode). The
   alternative is to test only Mac + iPad (or with a second Mac).
2. **Which save the visitor uses**: a second imported island, or a new game started with a clock offset.
3. **Whether Ryujinx / ldn_mitm compatibility matters.** It constrains the discovery packet format; the virtual subnet
   does not stop it, but it has to be tested against Ryujinx 3.0.0.
