# H3 plan: the deko3d renderer (started 2026-11-14 by Sudo, covering for Claude)

State at handoff: the interposer is ported and wired. The first FW_SWITCH_DEKO=ON build is DONE
(NRO 37,185,733 B, sha256 b2746c071f0e97f8f51602af4610428e8584ab373d2fbc7cd20d7d2c4936fc07,
staged and hash-verified; one fix on the way: fw_deko raced Dawn's generated headers on the fresh
build dir, FrightWorksSwitch.cmake now add_dependencies(fw_deko webgpu_dawn)). The console test is
queued behind the SPM perf chat's console session (claim held since 2026-11-14T04:36:25Z). Nothing is committed; everything below is in the C:\dev\no100f-nx working tree. Backups of
every edited pre-existing file are in WSL ~/no100f-h3-backup (flattened names).

## What H2 left us (confirmed on hardware 2026-11-14)

The game boots and runs on Dawn's Null device: Aurora runs surfaceless (webgpu::kSurfaceless is
true under __SWITCH__ in lib/webgpu/gpu.hpp), the presenter ticks jobs and drops the images, audio
plays, the debug server answers. The screen is black because nothing presents. H3 is the renderer:
Dawn stays for bookkeeping and Tint WGSL-to-GLSL; a link-time --wrap interposer shadows the wgpu
calls into deko3d, and the presenter's finished image is handed to a deko swapchain.

## What was done tonight

1. switch/deko/, switch/deko3d/, switch/uam/ imported from spm-nx at commit 5aae42d (the last
   renderer-core state before SPM's Nov perf-diag accretion: no spm_*_set bridges, no Aurora-side
   diag setters, no GX-worker hooks). The import includes by ancestry SPM's hardware-proven fixes:
   uniform translation by byte offset (4b4082d, the fix that made SPM's picture correct), swapchain
   acquire outside the queue lock (09811f3), the BC mip tile fix (CustomTileSize handling in
   memory.cpp), and the diag switches of that era (dk-diag, efbclear, bufdump, texdump, shaderdump,
   nomerge, uvlog, mtxlog, perftimer, arrayup), all interposer-local.
   Deliberately NOT imported: SPM's current deko (applymemo/hit-index/spin/gpupass/dstep bridges
   into SPM's Aurora and game code do not exist here).
2. Deko SD paths repointed from /switch/spm-nx to /switch/no100f-nx (dk_diag.txt in device.cpp,
   dksh_cache.bin and the Logs dir in shaders.cpp, texdump path in wgpu_encode.cpp). The front-face
   escape hatch env is FW_DK_FLIP_FRONT (wgpu_objects.cpp). The spm_tex identifiers in
   shader_translate.cpp were left alone on purpose: internal generated-GLSL names, zero visibility,
   renaming risks the shader name contract.
3. Aurora hooks, all under #if defined(__SWITCH__) && defined(FW_SWITCH_DEKO):
   - lib/webgpu/gpu.cpp: aurora_switch_dk_init() right after g_queue = g_device.GetQueue();
     aurora_switch_dk_shutdown() as the first statement of webgpu::shutdown().
   - lib/aurora.cpp: aurora_switch_dk_present(job.image->texture.texture.Get()) inside the
     kSurfaceless branch of present_presentation_job, right after the wait_until_precise pacing
     wait. It blocks to vsync inside, which paces that branch. (This Aurora DOES have the
     surfaceless branch; the research doc's warning applied to the PC tree at its target commit.
     PresentationImage.texture is a TextureWithSampler, hence .texture.texture.Get().)
4. switch/cmake/FrightWorksSwitch.cmake: the FW_SWITCH_DEKO FATAL_ERROR is gone. In GCC scope
   right after the Dawn include (mirroring SpmSwitch.cmake): NX_ROOT default, add_subdirectory for
   the vendored deko3d and uam, fw_deko static lib (C++20) with Dawn's source/binary dirs on its
   include path, linking deko3d_release, uamlib, tint_api, tint_lang_glsl_writer,
   tint_lang_wgsl_reader, tint_lang_wgsl_inspector, nx. fw_switch_enter_project_scope adds
   FW_SWITCH_DEKO=1 to the Aurora targets when the option is on. fw_switch_configure_products
   links fw_deko + deko3d_release into WiiCompiled and adds the --wrap list, generated from the
   imported sources: 64 functions, identical count to SPM's.
5. switch/scripts/build-switch.sh: new --deko mode (build-deko dir, -DFW_SWITCH_DEKO=ON), same
   pattern as --audit. Staging lands in the usual Downloads\no100f-nx-sd.
6. switch/THIRD_PARTY.md: rows added for deko3d (zlib), uam (zlib + MIT Mesa parts) and the
   interposer itself (MPL-2.0). switch/licenses/ already carried MPL-2.0.txt and SwitchWaker-MIT.

switch/deko/HANDOFF.md is SPM's own doc, kept for provenance; its paths and status lines describe
spm-nx, not this tree.

## First hardware test (the H3 exit gate from the research doc)

Under the console claim, with captures per the claim protocol:
1. Boot and read stderr: [dk] init lines (device/queue/swapchain 3x RGBA8 1280x720), shader
   compiles starting, dk_present summaries, no queue errors.
2. Capture the screen: the expected first picture is the game's own boot/title flow plus the F10
   overlay path, NOT a test pattern (the interposer presents Aurora's image directly). Confirm
   orientation and colours on the capture: Y-flip errors and sRGB darkness are the two classic
   first-picture faults (checklist below).
3. Ten minutes stable, HOME close clean, crash.txt untouched.
If a deko call fails opaquely (release libdeko3d aborts 2359-xxxx without the error callback):
switch the two deko3d_release links to the vendored debug target, log and flush before deko
creation calls, and check dkQueueIsInErrorState before submit/acquire/present. SwitchWakerHD kept
its debug lib on through its whole bring-up for exactly this reason; SPM ships release.

## Optimization checklist (everything the other ports learned, in the order to use it)

Correctness first (already in the port or first suspects when the picture is wrong):
- One shader worker thread, 8 MB stack, priority below the game: uam is not reentrant (a static
  gl_context in its frontend crashes with multiple workers even under a mutex). A draw whose
  shader is not ready is skipped and the combo memo must not remember the failure.
- Offline DKSH cache is a product feature, not an optimization: harvest shaders by playing,
  ship UserData/dksh_cache.bin (about 1.8 KB per shader; SwitchWakerHD loaded about 7,680 in
  0.26-0.29 s and covered 96.2%). A cold cache compiles at roughly 70-95 ms per shader on a
  1020 MHz CPU, so first boot is slow by design; say so in the README.
- Shader translation failures dump to UserData/Logs/shaderfail_<key>_<stage>.{wgsl,glsl}; that
  dump loop is how Tint-GLSL vs uam friction gets fixed.
- sRGB: deko3d 0.5.0 may never write Maxwell register 0x56E; if the picture is too dark, the
  present path's sRGB handling is the first suspect.
- Depth texture sampling returns (D, D, D, 1) where GL returns (D, 0, 0, 1): a shader sampling
  depth and using .g or .b breaks silently.
- Culling: front face is mapped as-is; if geometry vanishes in sheets, set FW_DK_FLIP_FRONT
  before touching shaders.
- MSAA is ignored by the interposer (single-sample textures, resolves become copies), as in SPM;
  correct-looking there. Revisit 2x only after parity.

Performance (H6 order, profile in this sequence):
1. EFB copies first. mkdd-nx priced 15 copies at 9.35 ms of a 17 ms frame, and skipping the
   final copy when nothing draws after it was its single biggest win. Expect the same shape here.
2. Fragment/GPU time with per-pass timestamps, then render thread microseconds per draw. Count
   game frames, not screen presents; a GPU time that does not move when the workload changes is
   timing waits, not work (the mkdd trap).
3. Barriers: the interposer starts conservative (full barriers after target changes, uploads and
   copies). Relax one at a time by A/B switch; lazy fragment barriers on real RAW/WAR hazards
   were worth 0.3-0.8 ms GPU per frame in SwitchWakerHD.
4. Zcull: keep the runtime toggle; the default is decided by THIS game's A/B at certified stock
   clocks. SPM kept it on with no regression; SwitchWakerHD shipped it off after decal flicker
   with polygon offset. Neither default imports.
5. Resolution: native 1280x720 first. If a lever is needed, dynamic resolution is the answer
   (mkdd), never clocks. SPM's 1.5x default was a Wii-game choice, not a transferable one.
6. Memory: Dawn Null allocates its own backing for every buffer, so every buffer exists twice,
   and the deko pools are roughly 250 MB in SwitchWakerHD terms. If memory gets tight, Aurora's
   staging size is the first lever (SPM's was 3x 63 MB).
7. CPU: PGO is the proven lever on translated code (SPM and JSRF both; profile game+runtime,
   never the renderer, Dawn, deko3d or uam, which compile in GCC scope). FW has no PGO wiring
   yet; it is post-parity work.
8. Clocks discipline for every number: certified stock 1020/384/1331 handheld, readback-verified
   per segment; recycle boots that read GPU 307.2; a number without its clocks is void.
   ApmCpuBoostMode_FastLoad drops the GPU to 76.8 MHz: if load boost is ever used, end it at the
   first presented frame and verify by readback.
9. Threads stay joined and owned (std::thread::detach ended SPM's game); the 4 MiB stack wraps
   are already in this build.

## Known open items carried over

- The Dawn SQLite cache commit wart in UserData/Cache (present in H2 boots, parked).
- The gather-pipe pages at 0xCC008000/0xCD008000 stay RW scratch (the no-access punch returned
  0xD401 in the crash-3 fix); GX FIFO writes still route to HLE via the inline policy check.
- FrightWorks' [video] features cut for v1 in the research doc (frame interpolation, scene-stage
  effects, depth peek compute, HD pack, HDR) stay cut; the interposer at this revision has no
  compute/storage-texture shadows, so anything re-enabled must be checked against that first.
- gpu.hpp's comment points at a switch/README.md Graphics section that does not exist yet in
  this tree; writing it (build modes, dk_diag keys, cache behaviour) is a follow-up.


## H3 FIRST BOOT: PASS (Nov 14, 2026, ~01:40-01:51 NST, Sudo covering for Claude)

Build b2746c071f0e97f8f51602af4610428e8584ab373d2fbc7cd20d7d2c4936fc07 (37,185,733 B) launched via Brandon's "?" Sphaira forwarder tile. Full success on the first attempt:

- [dk] init clean: device, queue, swapchain 3x RGBA8 1280x720, set-up 116 MiB heap, 61 ms.
- Cold shader cache by design: 107 shaders compiled this run, 0 failed, 9,827 ms total, longest 282 ms. dksh_cache.bin written (260,292 B at close), so the next boot is warm.
- 10,800+ presents over ~11 minutes, 0 skipped draws in the summaries, 0 queue errors. Images heap grew to 233 MiB in 21 chunks as content streamed.
- Visual confirmed by capture: in-game at Mystic Manor, upright, correct aspect (the letterbox is the game's own Cinematic 2.39:1 setting), colours sane. Launcher overlay (VIDEO page) also renders correctly through the path.
- Saving works: Brandon saved in game; 78-GIHE-ScoobyDoo00.gci (41,024 B) landed in UserData/Saves/MemoryCard/USA/Card A at 01:50:04 NST. The card layer's "Failed to open file: ScoobyDoo01..07" lines at exit are the game enumerating empty slots, not errors.
- Clean HOME close: "userAppExit: clean shutdown done". crash.txt untouched. Console claim released 01:51 NST; JSRF chat has the console next.
- Evidence: switch-logs/h3-test/ (stderr-h3-boot1.log 149,218 B, capture-h3-boot1.jpg, capture-h3-overlay2.jpg, session-h3.log).

Settings seen in the overlay: Resolution 2.25x (1080p), Picture shape Cinematic 2.39:1, internal render 2581x1080, ink outlines off, frame interpolation off, motion blur off. Perf work later should note the internal render is above 1080p ultrawide before the present downscales to the 720p swapchain.

### Finding: settings overlay takes no controller input (root cause located)

The overlay opens and closes but nothing inside responds to the pad. In-game input is fully working, so this is the overlay's own input feed:

- The overlay is drawn by Aurora's ImGui layer (wiicompiled/aurora-main/lib/imgui.cpp) via the ImGui WGPU backend. Drawing works.
- Its input arrives only via ImGui_ImplSDL3_ProcessEvent, called from imgui::process_event, which is only called from Aurora's SDL window event pump (aurora-main/lib/window.cpp lines 362-365, pump at 458-465).
- The Switch build is surfaceless (webgpu::kSurfaceless): no SDL window, the window event pump never runs, so no event ever reaches ImGui. settings_overlay.cpp reads ImGuiBackendFlags_HasGamepad and the ImGuiKey_Gamepad* key data (lines 307-310, 2120); both stay empty forever on this build.
- Open/close works because the toggle is watched from the runtime's own pad layer (g_panelOpenedByPad in settings_overlay.cpp), not through ImGui events.

Fix sketch: in the surfaceless frame path, feed ImGui the runtime pad state directly each frame while the panel is open (io.AddKeyEvent for the ImGuiKey_Gamepad* set plus the HasGamepad backend flag), sourced from the same pad layer the toggle uses. Contained to aurora-main/lib/imgui.cpp plus a call from the surfaceless present path in aurora.cpp (near line 1191, beside the dk_present hook). No renderer changes.

### Finding: in-game button prompts show GameCube glyphs

Cosmetic. The game draws GC button prompts while the pad is a Switch controller. Prompt glyphs are texture work (see UserData/texture_replacements and the interposer texdump path), or a prompt-set option in the launcher config if one exists. Not investigated.

### Next steps from here

1. H4 gate (research doc): title screen side-by-side against the PC build, zero skipped draws with a warm cache, clean session log. The warm-cache half is now testable on the next boot.
2. Overlay input fix above.
3. Optimization checklist below applies from the second boot onward (EFB copies first).


## Desk block after the first boot (Nov 14, 2026, ~01:55-02:05 NST, Sudo)

### Overlay input fix: WRITTEN AND COMPILE-VERIFIED (hardware test still owed)

Root cause refined past the first report: the pad layer itself is SDL (aurora-main/lib/dolphin/pad/pad.cpp reads SDL_GetGamepadButton/Axis and works in game), and the vendored ImGui SDL3 backend polls gamepads itself in ImGui_ImplSDL3_NewFrame via UpdateGamepads. But its gamepad LIST only refreshes when WantUpdateGamepadsList is set, which after start-up only happens on SDL_EVENT_GAMEPAD_ADDED delivered through ProcessEvent. The surfaceless Switch build has no event pump, so if the first enumeration (at ImGui init) ran before SDL had the pad, the backend stays pad-less forever: HasGamepad clear, settings_overlay.cpp PadButtonDown() always false, no nav. The overlay toggle works because it is watched from the runtime pad layer.

Fix (in the tree, backup at WSL ~/no100f-h3-backup/aurora-lib-imgui.cpp.orig): in aurora-main/lib/imgui.cpp new_frame(), before ImGui_ImplSDL3_NewFrame(), when webgpu::kSurfaceless and HasGamepad is clear, call ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll) to re-arm enumeration. It stops firing once a pad registers, and resumes if the pad disconnects (hot-plug recovery). Verified: incremental --deko build compiled aurora_core imgui.o, relinked, and produced NRO 37,185,733 B sha256 eac13dc10c2d66fe038d8f0af702af215278ad49233f997742e5a468d753de02 in the build tree (~/no100f-switch/build-deko). NOT staged: staging and the card still hold the proven H3 build b2746c07; build-switch.sh's staging step exited 1 (stage by hand when wanted). Hardware test when the console next frees: boot eac13dc1, open the overlay, d-pad should navigate and B should back out (settings_overlay.cpp:2057 even has a NavGamepadCancel close path waiting for this).

### Button prompts: FIXED AT THE DESK (asset pack generated and staged)

The prompt system exists and is wired: hle/input/pad.cpp picks a family from the first pad's SDL type every 30 reads (Nintendo types -> style 2 "nintendo") and calls aurora_set_button_icons; aurora-main/lib/gfx/texture_replacement.cpp composes the prompt atlas from <resourcesPath>/button_icons (resourcesPath = SDL_GetBasePath() = the NRO's folder, /switch/no100f-nx/). The Switch package simply never shipped that folder, so no layout.txt was found and the game's own GameCube atlas showed. Generated with the project's own tools/make-button-icons.py (Kenney CC0 art in tools/button-art, layout from texture_pack/ScoobyHD) into Downloads/no100f-nx-sd/switch/no100f-nx/button_icons (layout.txt + xbox/playstation/nintendo/keyboard PNGs; nintendo south=B, east=A verified visually). Next card sync + boot should show Switch prompts. Residual risk: if SDL reports the pad type as UNKNOWN on Switch, family stays -1 and prompts stay GameCube; texture_replacement.cpp has an s_buttonIconForced override if that proves out.

### Dawn cache commit wart: located, still parked

The "Failed to commit transaction: disk I/O error" lines come from aurora-main/lib/webgpu/gpu_cache.cpp (Module aurora::gpu::cache), Aurora's own SQLite-backed store behind Dawn's cache callbacks (wired at gpu.cpp:942-945). It is NOT the pipeline cache (separate file, custom SDL VFS, works) and NOT the DKSH cache (works). Impact: Dawn re-derives its cache entries each boot; some start-up cost, no correctness issue. Next step when someone takes it: read gpu_cache.cpp's commit path for its journal/fsync/locking assumptions against Horizon file semantics.


## Handover to Claude (Nov 14, 2026, ~02:57 NST, at Brandon's instruction)

Console claim RELEASED and verified absent (it was already absent when the release ran; no game was running). Sudo stakes no further claims; Claude has the project.

State of play for the overlay fix:
- Fix v1 (aurora imgui.cpp SetGamepadMode re-arm) was a no-op on this build and has been REVERTED byte-identical (backup ~/no100f-h3-backup/aurora-lib-imgui.cpp.orig). Cause: the Switch build replaces the ImGui SDL3 backend with switch/aurora/imgui_sdl_backends_horizon.cpp, where SetGamepadMode is an empty stub.
- Fix v2 is IN THE TREE and BUILT: switch/aurora/imgui_sdl_backends_horizon.cpp NewFrame now polls the shim's gamepad and feeds ImGui's gamepad keys every frame (vendored backend's mapping; backup ~/no100f-h3-backup/imgui_sdl_backends_horizon.cpp.orig). Full root-cause chain is in the desk-block section above, refined: the menu navigates on ImGui gamepad keys, the shim only synthesizes an F10 tap for the toggle, and nothing fed the gamepad keys.
- Fix v2 NRO 37,189,829 B sha256 e99948fe3468866a7f67d04ef761f78bf23706ae359ead7739c2938a768802c1 is ON THE CARD (hash-verified). Its navigation test is STILL OWED: boot 3 was staged at 02:28 NST but no launch or verdict happened before this handover. Do not treat the overlay as fixed until someone boots e99948fe and drives the menu.
- Boot 2 (fix v1 build eac13dc1) results, for the record: prompts CORRECT (button_icons pack works), warm cache perfect (present 1: 107 shaders from the cache file, 0 compiled, 0 failed), 3,600+ presents, clean HOME close, crash.txt untouched. Log: switch-logs/h3-test/stderr-h3-boot2.log.
- Card contents: no100f-nx.nro = e99948fe (fix v2), button_icons/ present (26 files), UserData intact (boot-1 save 78-GIHE-ScoobyDoo00.gci, warm dksh_cache.bin 260,292 B).


## Perf session results (Nov 14, 2026, Sudo, Claude's 3-boot plan)

Build on card: 7e306a5f (guestcore/bgprio dk_diag switches). Fixed input route at the
Mystic Manor spawn from Brandon's 10% save: mark, 12 stick legs (4 directions x 3,
15 s each, sys-agent set-stick), mark. All numbers below are the route windows only,
every boot certified STOCK by its own log line (1020.0/384.0/1331.2).

| boot | config | slow frames | frame mean | median | max | game mean | gx mean |
|---|---|---|---|---|---|---|---|
| A | baseline (no dk_diag) | 90 | 53.51 ms | 51.3 | 68.4 | 53.49 | 12.50 |
| B5 | guestcore | 90 | 58.39 ms | 57.7 | 76.4 | 58.37 | 14.46 |
| C2 | guestcore+bgprio | 90 | 53.82 ms | 51.1 | 66.2 | 53.81 | 12.35 |

Read: guestcore alone made slow frames about 9% worse (mean +4.9 ms, gx +2.0 ms).
Adding bgprio brought it back to baseline within noise. Verdict on this evidence:
guestcore alone is a regression; guestcore+bgprio is neutral. Caveats: slow-frame
stats only (frames over the log threshold), one run per config, present counters
too coarse for an fps delta (one counter inside each window). Brandon's own boot 1
baseline is a weak sample (free play, 26 slow frames whole log) and is not in the
table. Aurora Slow render frame lines never occur inside route windows; whole-log
means are about 20 ms in all boots (the 78 to 105 ms samples are first-frame
outliers).

Switch fingerprints verified in-log: guestcore moves helper threads to mask 0x6
ideal 2 (core 0 reserved for the guest scheduler); bgprio puts background threads
at prio 0x2F vs the normal 0x2C.

Session cost: the GPU clock lottery voided 5 boots (B, B2, B4 guestcore; C
guestcore+bgprio; plus B3 sacrificed to an early clocks check at 384). There is
still no early clocks check: the stderr log is locked by the running game and the
debug server log commands return empty, so a roll is only known after close+pull.
Brandon read B5's 384 live off the console, which matched its log line.

Findings for Claude:
- With guestcore on, debug-server input injection (press) did nothing at the
  settled title screen while sys-agent controller input worked instantly. All
  guestcore navigation used sys-agent. Mechanism unproven.
- Log lines occasionally interleave binary junk where two writers hit stderr at
  once (seen in boot C and Brandon's boot 1). Log writes are not synchronized.
- Title arming recipe that works: launch, idle about 240 s into the attract demo,
  B to exit to the title, held Start (PLUS) 1.2 s opens the menu, clicks only
  after that (holds repeat in menus).

Logs: switch-logs/h3-test/stderr-perf-bootA-baseline.log,
stderr-perf-bootB5-guestcore.log, stderr-perf-bootC2-guestcore-bgprio.log,
stderr-perf-boot1-baseline.log (Brandon's), plus the void 307 logs (bootB, B2, B4,
bootC). dk_diag.txt deleted after the session (card back to no-file baseline).
Console claim released and verified absent. crash.txt unchanged all session
(1735 B, sha256 9d83c322...).

## Perf session 2 results (Nov 14, 2026, afternoon, Sudo driving, build 97742649)

Build: 37,185,733 B, sha256 97742649e8bda4c76349a367b05d881830e7c25adcbaeabe9a69b073f3cc6834 (overlay A/B swap fix, overlay Resolution setting, dbg clocks command, cachedstaging switch, profile.txt profiler). Same fixed Mystic Manor route and slow-frame window method as session 1.

FINDING (bug): changing Resolution in the overlay did NOT persist. The log shows "[runtime-config] Unable to write /switch/no100f-nx/UserData/Config.toml" twice at the moment of the change, and Config.toml still read resolution_multiplier = 2.25 afterwards. The first boot A therefore ran at 2.25x, not 1.5x (log saved as stderr-boot-225x-baseline.log; it doubles as a same-build 2.25x sample). Workaround used: edited Config.toml directly on the card (game closed) to resolution_multiplier = 1.5, readback verified; original backed up as Config.toml.bak-225x in the h3-test folder. The card's Config.toml is LEFT at 1.5.

Also learned: all of session 1 (guestcore/bgprio) ran at 2.25x; the persisted setting predates both sessions.

dbg clocks reroll workflow: works exactly as intended. Boot A took 3 launches (307, 307, STOCK), boot B 1 launch (STOCK); each reroll costs about a minute via a clean HOME close. For contrast, session 1 burned five full boots on the lottery.

| config (all STOCK, route window, 90 slow frames) | mean | median | max |
|---|---|---|---|
| 2.25x baseline (same build) | 57.73 ms | 55.3 | 68.3 |
| 1.5x baseline (boot A) | 50.56 ms | 49.9 | 65.7 |
| 1.5x + cachedstaging (boot B) | 50.59 ms | 49.5 | 68.5 |

Verdict: 2.25x to 1.5x cuts the slow-frame mean by about 12% (7.2 ms) on this route. cachedstaging is neutral here (50.56 vs 50.59, within noise, one run each); its own counters show it flushing about 10 MB per frame of CPU cache for copy sources during the route. Slow-frame subset only, as before.

Profiler (profile.txt "60:20" in boot B): ran at about 60 s in for 20 s, so it sampled the boot/title phase, not the route. 19,792 samples, 47.3% in translated code. Top: elf+13DC80C 15.6%, guest 80124668 YUV_32_4x2_even 9.2% (FMV decode), 8010E2EC DVDLowRead 3.2%, 80122BFC ExpandPlane 2.3%, 8010D28C PADControlMotor 2.3%. Full list in the boot B log, lines 687-750ish. If a route-window profile is wanted, the delay value needs to cover settle plus load time (route started at 323 s in boot B).

A/B swap fix: confirmed working in the overlay this session (A enters/selects, B backs out, d-pad navigates). That also closes the long-owed overlay navigation hardware test.

State left: dk_diag.txt and profile.txt deleted from the card; Config.toml at 1.5 (see above); console claim released and verified absent; crash.txt unchanged all day (1,735 B, sha256 prefix 9d83c322).

Logs in switch-logs/h3-test: stderr-resA-15x-baseline.log (177,973 B; marks 324 s to 506 s), stderr-resB-15x-cachedstaging.log (184,591 B; marks 323 s to 505 s), stderr-boot-225x-baseline.log (200,393 B; marks 419 s to 602 s).

## Storagecache session results (Nov 14, 2026, evening, Sudo driving, build c0080a6f)

Build: 37,189,829 B, sha256 c0080a6f0401320a0ad4613deb93a152c6d6d3ff7cd0832f2546e4d83eb93ce9 (pad-shim lock fix and disc keep-files-open fix on by default; storagecache dk_diag toggle). Same fixed Mystic Manor route at 1.5x, both boots STOCK on first launch, no rerolls.

Boot 1 (no dk_diag, profile.txt 330:20): the profile landed exactly on the route. Sampling began one line after the route START mark (route 329 s to 511 s), 19,778 samples, 62.3% in translated code. Top: guest 80102390 GXEndDisplayList 14.3%, elf+13DD45C 5.7%, guest 80144E8C 4.0%, guest 800FD914 GXSetArray 3.6%. Full list in the log from line 1581. Route slow-frame mean 50.91 ms (n=90, median 50.1, max 67.6), essentially unchanged from the previous build's 50.56, as expected: the pad and disc fixes target loads and input, not this route.

Boot 2 (dk_diag storagecache): diag line proves storagecache=1. Route slow-frame mean 50.19 ms (n=90, median 49.9, max 62.1), about 0.7 ms under boot 1, within single-run noise but directionally positive. Route-window counters: 1,925 hits vs 7,153 misses, 11,211 KiB of uploads skipped; the skip fires on about a fifth of array uploads, so most arrays really are rewritten per frame. Geometry checked at three captures (spawn, mid-route in motion, route end): all clean, no spikes, stretching or frozen models. On this evidence storagecache is safe here and mildly positive, not the several-ms win hoped for.

State left: dk_diag.txt deleted, profile.txt deleted, console claim released and verified absent, Config.toml still at 1.5, crash.txt unchanged (1,735 B, sha256 prefix 9d83c322). FrightWorks is all-clear for the vhdx compact: no FrightWorks processes or builds running on the LOQ.

Logs in switch-logs/h3-test: stderr-sc1-profile330-baseline.log (184,267 B), stderr-sc2-storagecache.log (179,092 B). Captures: capture-sc2-spawn.jpg, capture-sc2-midroute.jpg, capture-perf-bootS2-end.jpg.

## cull_off A/B (Nov 14, 2026, late evening, Sudo driving, build c0080a6f)

Single boot after SPM released the console (their 1.0x window was killed by Brandon; the FrightWorks watch job was stood down and this ran directly). dk_diag cull_off, diag line proves cull_off=1, stock clocks first launch, 1.5x, same Mystic Manor route (marks 320 s to 502 s), clean shutdown, crash.txt unchanged.

Result: route slow-frame mean 51.56 ms (n=90, median 50.10, max 68.0) against the same-build culling-on baseline of 50.91 ms (median 50.10). The medians are identical and the mean delta (+0.65 ms) is inside tonight's same-config run spread (50.91 vs 50.19 between the two culling-on boots), so call it flat. Draw counts per 600 frames in the route window are also unchanged: 851-909 with culling off vs 863-882 with it on. Whatever cull_off toggles, it does not change what this route submits, and culling is not a lever here. The zcull question from the H3 checklist is answered for this game on this route: nothing to gain.

Log: stderr-c1-culloff.log (178,068 B) in switch-logs/h3-test. Card restored (dk_diag deleted, claim released and verified absent, Config still 1.5). FrightWorks FINAL all-clear for the vhdx compact posted to the board with this result: no FrightWorks console work remains queued.


## H4 parity gate - status (Nov 14, 2026 late evening, Sudo)

Log halves PASS on the c0080a6f build, warm cache, all three of tonight's boots
(S1 baseline, S2 storagecache, C1 cull_off):
- Skipped draws: [dk] replay cumulative summaries end at 2.56M-2.71M draws per
  boot, (0 skipped) in every summary of every boot.
- Session log: no new error classes. Only the three known ones: Dawn pipeline
  cache transaction failures (parked sqlite wart), the gather-pipe page note
  from the crash-3 fix (x2 per boot), and aurora::card "Failed to open file:
  ScoobyDooNN" x82 per boot, which is the game enumerating save slots 01-07+
  that do not exist on the card. Benign, identical every boot.

Visual half (title side-by-side vs the PC build): ATTEMPTED, BLOCKED on setup.
The Play-folder PC build (FrightWorks Game.exe) has never been configured:
it opens a first-run dialog (454x353) saying the game needs your own extracted
copy of Scooby-Doo! Night of 100 Frights (USA, GIHE78) - extract the disc via
Dolphin to a folder and pick it, or put the extracted folder next to the
program named "game"; the launcher can also fill paths.dvd_root from an
.iso/.gcm/.ciso. No game files were ever pointed at it, so no PC title screen
exists to compare against. Two automated capture attempts (22:29 and 22:34)
only ever captured the dialog and the desktop. Side effects: none - the
generated Play/UserData/Config.toml is the untouched default, dvd_root still
commented out; the two ENTER presses the capture script sent at the dialog set
nothing. Brandon closed the window himself; both scheduled tasks deleted.
Visual half waits on the PC first-run setup (Brandon's files, Brandon's call,
or Claude's). Switch-side title captures for the comparison are already in
this folder (capture-c1-settle.jpg, capture-perf-bootA-title.jpg).


## H4 parity gate - COMPLETE (Nov 14, 2026, ~22:50 NST, Sudo)

Brandon configured the PC build himself (dvd_root = C:\dev\no100f-nx\Disc\DATA,
set ~22:40) and booted it; captures pc2-title-01..05.png taken while it ran.
Composite: switch-logs/h3-test/h4-parity-sidebyside.jpg.

- Top pair: PC (Dawn) attract demo at the Mystic Manor gate vs Switch (deko,
  build c0080a6f) Mystic Manor spawn. Same manor model and lit windows, same
  sky/fog, sign, trees, fence, Scooby model, ground textures. No missing or
  garbled geometry, no texture corruption on either side. Differences are
  camera (demo vs spawn), HUD state, and render scale (PC 2.25x + HD texture
  pack, Switch 1.5x), not content.
- Bottom pair: PC main menu (lab interior, full detail, correct text) vs
  Switch title screen (logo, gang, background, port CONTROLLER hint) - both
  render correctly.
- Caveat: not a pixel-identical frame match, and the literal logo-title frame
  was not captured on the PC side (cadence caught its menu and demo instead).
  For the gate's purpose - does the deko renderer draw the same picture as the
  known-good build - this passes.

H4 GATE: PASS (all three parts: side-by-side, 0 skipped draws warm, clean log).

## fifofast session (Nov 15, 2026, ~00:48-02:05 NST)

Build c1100f19 (37,189,829 B, sha256 c1100f199a9a...41fe). Three commissioned boots at 1.5x, stock clocks, standard Mystic Manor route. The watch runner completed boot 1 after two voided attempts (menu navigation, save slot; logs stderr-ff1b-voidmenu.log, stderr-ff1c-voidslot.log). Brandon then loaded boots 2 and 3 by hand and Sudo drove the routes from the FrightWorks chat; the runner was stopped because its 90-minute window would have cut boot 3 off mid-boot, and the watch job was removed once the session went manual.

- Boot 1 (no dk_diag, profile.txt 330:20): STOCK, marks 324 s to 506 s, route n=90 mean 56.52 ms, median 51.90, max 68.3. Profile: 19,783 samples, 57.6% in translated code, 6,037 translated functions. Top entries: guest 800F8C34 PPCMfhid2 24.1%; elf+13DDB9C svcSleepThread 7.7% (VBlank wait, counted as idle per Claude); guest 80144E8C 4.2%; guest 800419D0 2.8%; guest 80029710 2.6%; memcpy group about 4.9% total; elf+1C46A68 memset 1.3%; guest 8018A128 1.3%; guest 8015F974 1.0%; memcmp 0.6%; svcSendSyncRequest 0.6%; steady_clock::now 0.7%. Note the profile window (330 s + 20 s) overlaps the first ~26 s of the route window, so boot 1's route mean carries profiler cost the other boots do not.
- Boot 2 (fifofast=2): SELF-CHECK PASS. Every [vi] gxfifo summary in the whole boot reads 0 mismatches, route included. Route n=90 mean 51.54 ms, median 48.35, max 66.1 (both paths running; not a timing sample). Geometry clean at spawn, mid-route and end captures: no spikes, no stretched geometry, no frozen animation.
- Boot 3 (fifofast): diag line confirms fifofast=1 with every other switch 0. First attempt void: GPU-307-LOTTERY at boot (log stderr-ff3a-void307.log, closed cleanly, not measured). Second attempt STOCK, marks 84 s to 267 s, route n=90 mean 47.54 ms, median 45.00, max 62.7. Against the c0080a6f S1 baseline (50.91 ms) that is about 3.4 ms faster, inside Claude's expected 2-4 ms for the per-word logging removal plus the packet fast path plus the disc memcpy. Against boot 1 on this same build (56.52) the gap is 9.0 ms, but boot 1's number is inflated by the profiler overlap noted above; the S1 comparison is the cleaner one, and all of these are single runs.
- FIFO counts: route-window gxfifo summaries show roughly 36-39k pipe writes per frame against roughly 4.9-5.4k bursts, with only 70-134 writes per frame going into display lists on this route. Boot 3's fast path appended about 31k words per frame. Boot 1 (fifofast off) appended 0, as expected.
- crash.txt unchanged all session (1,735 B, sha256 prefix 9d83c322). Every boot closed cleanly via HOME. Closeout: dk_diag.txt deleted, profile.txt absent, Config left at 1.5, claim released and verified absent. Logs: stderr-ff1-profile330.log, stderr-ff2-fifofast2-check.log, stderr-ff3-fifofast.log (plus the void log), in switch-logs/h3-test and the chat's evidence folder.

## usedrange + mfhid2 session, Nov 15, 2026 (Sudo, in-house)

Build 34585267 (usedrange toggle + mfhid2 milestone counter on top of c1100f19). Three boots, 1.5x, stock clocks read back before every route, standard Mystic Manor route.

- UR1 (no dk_diag, everything off): slow-frame mean 58.64 (n=120), overall 50.48 ms/frame (19.8 fps), gx 13.37. mfhid2 milestones: 0.
- UR3 (fifofast only): slow mean 47.27 (n=90), overall about 44.3 ms/frame (about 22.6 fps), gx about 13.3. Reproduces the c1100f19 fifofast boot (47.54) on the new build. mfhid2 milestones: 0.
- UR2 (fifofast + usedrange): slow mean 55.22 (n=90), overall about 52.2 ms/frame, gx about 20.9. Storage uploads cut from about 8,100 to 1,290 KiB/frame (about 1,221 subrange uploads + 250 fallbacks per frame, about 6.6 MB/frame not uploaded). Geometry clean at spawn, mid-route and end; gxfifo 0 mismatches; clean shutdown; crash.txt unchanged. mfhid2 milestones: 0.

Verdicts:
- usedrange: mechanism works, net loss as built. The per-draw vertex-stream walk costs about 7.6 ms/frame in the gx bucket, more than the upload copies it saves. Parked (toggle stays in the tree, default off). Only worthwhile retry: compute (min, max) once per draw from the compact index buffer instead of strided per-attribute stream walks; the ceiling is the roughly 3.5 ms total re-upload cost, so expected value is low.
- mfhid2: CLOSED. Three boots, zero milestone lines; the call counter never reached 1,000,000 in any boot. The 24.1% in the c1100f19 route profile was profiler attribution (exact-address labelling), not execution. No fix needed.
- fifofast on the new build reproduces 47.3 to 47.5 ms; nothing regressed.

Card closeout: dk_diag deleted, no profile.txt, Config left at 1.5, claim released and verified absent. Logs: stderr-ur1-baseline.log, stderr-ur2-usedrange.log, stderr-ur3-fifofast.log.

## Attract/title pacing question, Nov 15, 2026 (Sudo, in-house): ANSWERED

Desk study (ATTRACT-PACING-STUDY.md in the evidence folder) plus one profile boot (AP1, profile.txt 150:20, build 34585267, stock; log stderr-ap1-titleprofile.log). The game's main loop never sleeps on VIWaitForRetrace; it polls VIGetRetraceCount until two retraces pass (proven in the generated code, shard_b7beac14, loc_800AD68C). "Busy" in the [vi] accounting is a residual (frame minus named waits) and idle only accrues in the guest scheduler's idle loop, so the poll books as busy. AP1's window landed on the attract front-end (Bink-driven): Bink decode family about 25% of samples, XXH64 texture hashing about 5%, svcSendSyncRequest 7.8%; the frame sat at exactly 33.37 ms with idle 0.00, matching the sc1 title signature. Verdict: the 33 ms is the game's own 30 fps limiter spinning plus real Bink work, not miscounted pacing and not hidden GPU time. No fix planned (faithful; no in-game effect). The study's one miss: it predicted the poll symbol would top the profile at 55 to 75%; it smears below per-symbol visibility instead.


## RP1 route profile - 2026-11-15 (Sudo, FrightWorks chat)

- Boot RP1: build 34585267 (NRO 37193925 bytes, sha256 345852677c979df9a2a948fee61947f3e682ee1bc6fa918f370fcf9cddaeaa45, verified on card), fifofast only, profile 330:20, resolution_multiplier 1.5, stock clocks.
- Gate/claim: no prior claim, running=0 at gate; claimed as Sudo (FrightWorks chat) 2026-11-15T16:44:09Z, released at closeout (verified absent). dk_diag.txt and profile.txt deleted at closeout (verified absent); Config stays 1.5.
- Diag: fifofast=1, all other diag flags 0. Clocks: [dk] clocks boot STOCK (1020.0/384.0/1331.2 MHz), server clocks at 301 s STOCK, every [dk] cpu window in the route STOCK.
- Load: HOME capture showed SPM focused, DRIGHT to Scooby-Doo (verified), launch A, running=1 titleId 050FCFAF9892B000 at 45 s, attract verified, file list verified top entry 11/14/26 10% The Mystery Machine, spawn verified Mystic Manor (manor, fountain, Scooby on path) before uptime 320 (301 s poll).
- Route: marks PERF ROUTE START at 331 s / END at 523 s, 12 LEFT-stick legs (3 cycles, 15 s each, 0.5 s centred), captures capture-rp1-midroute.png (after cycle 1) and capture-rp1-end.png (after cycle 3), flush done. HOME close verified (Close focused), running=0 after. crash.txt sha256 unchanged 9d83c322fb35a0fc5b96e3f550d00803ed850973fa46b9803e691012b6cc4f80.
- Route timing vs UR3 (same build, fifofast only, no profile): slow frames in window n=90, mean 46.66 ms, median 44.05 ms, max 60.40 ms (UR3 slow mean 47.27 ms); [vi] frames windows in window (7 x 600): 46.64, 42.03, 45.28, 44.46, 42.64, 47.04, 41.80 ms/frame, mean 44.27 ms/frame (UR3 about 44.3 ms/frame); gx in busy mean 13.25 ms (UR3 about 13.3 ms), slow-frame gx mean 12.54 ms. Profile sampling did not materially distort route timing.
- Staging in window: per frame vert 614-744 KiB, storage 7506-9160, index 371-492, draws 898-941, push_storage 1352-1718 calls; storagecache and usedrange both 0 (off in diag). GPU per [dk] cpu windows: avg 21.12-22.44 ms, replay 0.75-0.83 ms, Dawn submit 0.04 ms.
- Profile: [runtime] 19777 samples of the game thread, 51.0% in translated code, 6037 translated functions, sampled for 20 s from the route start mark (331 s).
  Top entries (main list): 13.0% PPCMfhid2 (IGNORE: proven attribution smear, closed), 8.1% elf+13DEA6C = svcSleepThread (idle VBlank, ignore as cost), 4.7% guest fn_80144E8C, 3.1% guest fn_800419D0, 2.7% elf+1C3D058 = memcpy, 2.6% guest fn_80029710, 1.8% guest fn_8018A128, 1.3% elf+1C47968 = memset, 1.2% guest fn_8015F974, 0.9% elf+1C3D04C = memcpy, 0.9% guest fn_80139FF0, 0.8% elf+1BD3654 = steady_clock::now, 0.8% guest __ieee754_rem_pio2, 0.7% elf+1C52E00 = memcmp, 0.7% elf+13DEB3C = svcSendSyncRequest, 0.5% elf+D1C88 = AX DSP ExecuteGcCommandList.
  Grouped (top-60 list, sum 60.1%): guest named ex-mfhid2 25.9%, mfhid2 smear 13.0%, svcSleepThread idle 8.1%, memcpy cluster 6.9%, memset cluster 2.4%, steady_clock 0.8%, memcmp 0.7%, svcSendSyncRequest 0.7%, AX DSP 0.5%. Pair grouping: memcpy+memset called from aurora::gfx::push (elf+1328F30 / elf+1328F98) = 8.5%. PC grouping: HleFifoWrite 2.3%, FifoFastMode 0.8%, handle_draw_unmerged 0.3%, aurora gx fifo process 0.2%, gx_vertex_ndc 0.2%.
  Full addr2line for 66 elf offsets: switch-logs/h3-test/rp1-addr2line.txt and ~/workspace/no100f-h3-evidence/rp1-addr2line.txt locally in the watch evidence.
- Biggest real cost: after removing mfhid2 smear and svcSleepThread idle, no single host function dominates. The frame is guest game code first (51.0% translated overall; top guest fn_80144E8C / fn_800419D0 / fn_80029710 / fn_8018A128) with gx about 13 ms inside the game thread, and the largest resolved host bucket is staging copies: memcpy+memset about 9.3% combined, driven by aurora::gfx::push. GPU (about 21-22 ms avg) still finishes before the game-thread frame (about 44-47 ms).
- Log: stderr-rp1-routeprofile.log in switch-logs/h3-test and in the evidence folder.


## SC3 - storagecache A/B (2026-11-15, Sudo / FrightWorks watch)
- Boot SC3: build 34585267 (NRO 37193925, sha256 345852677c979df9a2a948fee61947f3e682ee1bc6fa918f370fcf9cddaeaa45), dk_diag fifofast+storagecache, profile.txt absent, Config resolution_multiplier = 1.5, 1.5x.
- Diag: fifofast=1 storagecache=1, all else 0. Clocks: boot STOCK and dbg clocks STOCK (1020.0/384.0/1331.2 MHz). Title 050FCFAF9892B000, running=1. Load verified: file list top entry "11/14/26 10% The Mystery Machine"; spawn verified Mystic Manor (manor, fountain, Scooby on path).
- Route: PERF ROUTE START/END boot SC3 marks at 315 s / 507 s, 12 LEFT-stick legs (3 cycles, 15 s each). Midroute and end captures clean geometry, no garbled or missing meshes.
- Slow frames in route window: n=90, mean 47.81 ms, median 45.05 ms, max 60.90 ms. Baselines same build: UR3 (fifofast only) slow mean 47.27 ms, ~44.3 ms/frame; RP1 slow mean 46.66 ms, 44.27 ms/frame. SC3 [vi] frames route windows: 46.00, 42.05, 43.74, 45.57, 41.67, 46.59, 42.05 ms/frame, mean 43.95 ms/frame. Slow gx mean 12.53 ms; [vi] busy gx mean 13.17 ms.
- Staging in route window (per-frame window averages, mean across 7 windows): push_storage 1212 calls, 6347 KiB/frame (UR3 route ~1545 calls, ~8260 KiB/frame); storagecache hits 327/frame, misses 1212/frame, hit rate 21.2%, skipped 1887 KiB/frame.
- Crash: crash.txt sha256 unchanged 9d83c322fb35a0fc5b96e3f550d00803ed850973fa46b9803e691012b6cc4f80.
- Verdict: DOES NOT BANK. Storagecache cuts ~1.9 MB/frame of storage uploads at a 21.2% hit rate, but the slow-frame mean is +0.54 ms worse than UR3 (47.81 vs 47.27), failing the bank bar (beat UR3 by >=0.4 ms with clean geometry). Flat/worse on the primary metric, despite the upload saving and clean geometry.
- Closeout: game closed via HOME/X/A, running=0; dk_diag.txt deleted and verified absent; Config stays 1.5; /agent-claim.txt deleted and verified absent. Log: switch-logs/h3-test/stderr-sc3-storagecache.log; evidence in ~/workspace/no100f-h3-evidence/ (stderr-sc3-storagecache.log, capture-sc3-midroute.png, capture-sc3-end.png, plus home/focus/title/file/spawn/close captures).


## G10: GPU cost at 1.0x (Nov 15, 2026, Stage 0 gate)

Boot G10 on build 34585267, stock clocks (every clock line in the log reads STOCK; dbg clocks checked live before the route). Config.toml resolution_multiplier set to 1.0 by direct file edit and read back before launch; the log's display line reads "render 480 lines (1x)" and the gameplay render target height is 480, confirming 1.0x applied. Diag: fifofast=1, all other keys 0. Standard Mystic Manor route from Brandon's save (11/14/26, 10%, The Mystery Machine), marks at 303 s to 537 s, 9 full [vi] windows (frames 7802-13201).

- GPU at 1.0x: [dk] gpu avg 11.07 to 12.19 ms per 600-present window, mean 11.54 ms. Pass split: main 1147x528 pass 7.46 to 8.23 ms/f, 1280x720 present pass 0.71 ms/f, 459x256 pass 0.19 to 0.20 ms/f.
- GPU at 1.5x (same build, UR3 baseline): 21 to 22 ms. So 1.0x saves about 9.5 to 10.5 ms of GPU and fits the 16.7 ms frame budget with about 5.2 ms of headroom. Stage 0 GPU gate: PASSED.
- Frame at 1.0x: [vi] windows 41.47 to 47.71 ms/frame, mean 43.87; busy mean 40.99. Slow frames in the window: n=120, mean 47.81, median 45.25, max 61.10. Against the 1.5x baseline (about 44.3 ms/frame, slow mean 47.27) the frame is flat, as predicted: the frame is CPU-paced, so the resolution lever banks about 10 ms of GPU that only cashes once the CPU chain drops below GPU time.
- Route captures (midroute and end) render correctly at 1.0x, no corruption. crash.txt sha256 unchanged (starts 9d83c322). Log: switch-logs/h3-test/stderr-g10-gpu1x.log.
- Note on attempts: the first launch's route was interrupted in cycle 1 by a LOQ WSL reboot (parallel vhdx compact work stopped WslService mid-route; the stick held and Scooby ended off-route at the Mystic Playground gate). That game was closed cleanly via HOME, the title was relaunched, and the second route ran all 12 legs with WSL uptime continuous across the window. The pulled log is the second, complete boot only (the card log is per-boot).
- Closeout: Config.toml restored to resolution_multiplier = 1.5 and read back; dk_diag.txt deleted; console claim released.

## 60 fps campaign — Build round 1 (Nov 15, 2026)

Approved by Brandon Nov 15, 2026 ~15:48 NST; executed in-house (Claude out until Nov 20). Full checkpoint log: ~/workspace/no100f-h3-evidence/BUILD-ROUND1-LOG.md (local evidence). Working tree only, no commits, no console contact; LOQ build claim held for the round and released at close.

- Step 1, instruments (per PHASE1-DESK): bbox counters (armed verts + accumulate calls, disarm counting), pipeline memo counters (bumps/hits/misses/table), six gated RAII stage timers printing a gxstage line. Gate: dk_diag `stagetimers`. No behaviour change.
- Step 2, probe + libm: DEVIATION from the desk spec — wrap-and-call-original is impossible in this machinery (PPC_NATIVE_OVERRIDE makes the translator skip the address; no translated original exists). GXCallDisplayList (0x80102464) and iModelCull (0x8000E220) are therefore faithful native reimplementations transliterated from the translator's own build shards, permanent in the tree; only their logging is gated (dk_diag `s0probe`). libm bindings: sprintf (0x800EB118) to the existing RuntimeHle::FormatGuestPrintf with a PPC varargs adapter; sin/cos + opportunistic riders as thin host bodies; all 15 addresses MAP-verified; __kernel_sin/__kernel_cos/__ieee754_rem_pio2 deliberately NOT bound. Differential tests (wiicompiled/runtime/tests/no100f_libm_bindings_tests.cpp): sin/cos max 1 ULP at every band incl. |x| <= 1e15 (no rem_pio2 divergence), riders max 2 ULP, floor exact, sprintf byte-identical to host snprintf on the full corpus incl. the game's real format strings from main.dol. No value-range fallback (nothing to fall back from at 1 ULP; no translated original remains). Translator re-run clean; registrations verified against control cases.
- Step 3, PGO: SPM spec E ported whole as FW_PGO_* (FrightWorksSwitch.cmake options/blocks, switch/source/pgo_instr_switch.c writer with weak hooks in fw_switch_init.cpp, build-switch.sh --variant pgo|pgo-use, pgo-fit-check.py, manifest discipline; region GIHE78-USA, clang 19, scope game+runtime+aurora). Capture build: 55,883,973 B, sha256 18a9e4690f0cc49b1d15e8165b1874cde9073a0f11a2e76c5fbf686195ba6b24, ELF carries __llvm_prf_* sections; lives in WSL ~/no100f-switch/build-deko-pgo/. Manifest: C:\dev\no100f-nx\pgo\manifest.txt.
- Default build after the round (instruments + probe + libm, PGO off): 37,189,829 B, sha256 2f36e6d5939ad753585fac7aa06b7d4eba53994d07aec715d9444b43dd624591, staged in Downloads\no100f-nx-sd and verified byte-identical.
- Queued console work (behind Brandon's hands-on claim, Nov 15 ~16:40 NST): (1) probe route boot on the default build, dk_diag fifofast + s0probe + stagetimers — settles the native-scene seam (A vs B), bbox armed window, memo rates, gx stage split, and doubles as the libm A/B vs UR3 slow mean 47.27 (regression flag above ~47.7); (2) PGO training route boots on the capture NRO (writes UserData/pgo/fw-*.profraw every 5 min and at clean exit; never quote its frame rates); (3) copy last profraw per session into C:\dev\no100f-nx\pgo\, run --variant pgo-use (merge, build, fit-check, stage), then A/B the USE build on the standard route.

## 60 fps campaign — G10 GPU at 1.0x (Nov 15, 2026)

Recorded by the G10 watch worker; see the results section it appended and GOAL.md. GPU 11.54 ms/frame at 1.0x vs 21-22 at 1.5x (Stage 0 GPU gate PASSED); frame still CPU-paced (route 43.87 ms/frame), resolution lever banked for Phase 3. Config restored to 1.5, claim and dk_diag deleted, crash.txt unchanged.

## PR1 probe route boot — VOID (GPU-307) + new-build init hang (Nov 15, 2026)

Build under test: round-1 default 2f36e6d5 (37,189,829 B; instruments + s0probe + libm bindings), dk_diag fifofast/s0probe/stagetimers, Config 1.5x. Session log: ~/workspace/no100f-h3-evidence/PR1-SESSION-LOG.md; boot log: stderr-pr1-void307.log (evidence + h3-test).

- Clocks: the boot rolled the GPU-307 lottery (the log's own [dk] clocks boot line and a live dbg clocks read both 1020.0/307.2/1331.2, GPU-307-LOTTERY). Per campaign rules the boot is VOID for measurement; the route was not run. The 307 roll alone is not a build failure.
- HANG (the substantive finding): the new build never presented a guest frame. Three captures at ~150/180/240 s were byte-identical all-black (sha256 711b4760...). The log's watchdog fired at 12 s and 30 s: "0 guest frame(s) presented, VI black=1". The guest main thread's stack sits in atof/__strtold under __init_cpp (static init), immediately after the sd2.ini read; only one [dk] replay summary (1 draw, the initial clear) and one present occurred before the stall. No [s0probe] lines and no staging/gxstage lines (no 600-frame window completed). crash.txt unchanged (9d83c322...) — a hang, not a crash. HOME close worked and the exit was clean.
- Consequence: seam verdict, bbox/memo counters, gxstage split and the libm route A/B all remain unmeasured. Diagnose the init hang first (stack points at the guest strtold path as regenerated by the Step 2 translator re-run; the libm HLE registrations are the other new variable, though atof/strtold themselves were not bound). PR1 then needs a re-run on a fixed build with a STOCK clock roll.
- Closeout verified: resident NRO restored to 34585267 (size + sha256 read-back match), dk_diag.txt deleted, claim deleted, Config still resolution_multiplier = 1.5.

## PR2 — probe route boot re-run (Nov 15, 2026)

Fixed build b6bbe08a (instruments + Stage 0 probe + corrected libm bindings), standard route at 1.5x, STOCK clocks on the first roll (no relaunch used), route marks 254-445 s. Log: switch-logs/h3-test/stderr-pr2-probe.log (also in local evidence).

- FIX PROOF: the libm fix holds on hardware. Title/attract presented frames (watchdog: 242 guest frames at 12 s, 786 at 30 s, VI black=0 — the exact inverse of the PR1 hang signature); 10,800 presents over the session, clean HOME close, crash.txt unchanged (9d83c322...).
- SEAM VERDICT: SEAM A. The iModelCull probe logged one render callback for every atomic: cb=0x801488B8 on all 297 parsed cull calls, 15 distinct atomics, 15 distinct geometries, no atomic ever used a second callback. GXCallDisplayList traffic arrives through two guest chains: the world-sector chain (x261: 8015E840, 80185168, 801517CC, 80151038, 80151228, 801540B4) and the model chain (x29: 8015E2E0, 80185168, 801488F4, 8000E4A0, 80030828, 800305F0), where 801488F4 sits inside the callback (cb+0x3C). DL sizes 27-41,483 B (mean 2,136), 136 distinct DL pointers in the first 300.
- Title atomic (RpGeometry dump): +0x10 nTris=76, +0x14 nVerts=152, +0x20 materials=0x8137F6E0, +0x24 nMorphTargets=1 — all matching the desk-verified values. Texture: ptr 0x812390A0, name "text_title", word+0x18=0x6C650000, word+0x50=0x00001302 (the operative sampler word, as the desk study predicted).
- BBOX: never armed on the route. Every 600-frame window reports clears 0, reads 0, disarms 0, armed 0 verts in 0 calls, and gxstage bbox = 0.00 ms. The desk study's 0 to ~2-3 ms unknown resolves to ZERO for the manor route; the bbox-skip has no prize here.
- PIPELINE MEMO: route totals bumps 13,223, memo hits 2,888, memo misses 3,574, table misses 0 → counted-resolve hit rate 44.7%. The table never overflowed; the miss path (~0.85 counted misses/frame) is the bounded prize the Phase 1 desk pack estimated.
- GXSTAGE (route average, per frame): process 13.98 ms = admit 2.20 (resolve 2.36, diagnostic overlap inside admit) + uploads 4.44 + uniform 0.72 + bind/record 1.93 + bbox 0.00 + remainder 4.68. Uploads are the largest named stage; the diffuse remainder is the second block, as the per-draw study predicted.
- PERF: route slow frames n=90, mean 48.47, median 45.40, max 61.00 vs UR3 47.27 → the pre-set regression flag (~47.7) is TRIPPED; no bank. [vi] windows inside the marks: 45.30 / 42.20 / 49.28 / 41.92 / 47.27 / 44.34 / 44.78 → mean 45.01 ms/frame (UR3 ~44.3), busy 42.04, gx 13.74 (UR3 ~13.3), GPU 21.67 (inside the 21-22 baseline band). Draws 891-930 per window; replay summaries 0 skipped everywhere; fifofast 0 mismatches in all 18 windows. The expected libm gain did not materialize on the route; this build also carries always-on counters whose cost is not separable without a same-build dk_diag-off A/B. Baseline fifofast boots themselves spread 46.66-47.81 on slow mean (RP1/UR3/SC3), so PR2 sits ~+0.7 to +1.2 over that cluster — flagged, not banked, and not explained away.
- Closeout verified: card restored to resident 34585267 (read-back sha256 match), dk_diag.txt deleted, claim deleted, Config.toml still at resolution_multiplier = 1.5.

## PR3 — same-build A/B route boot (Nov 15, 2026): VOID, two GPU-307 rolls

Purpose: PR2 (build b6bbe08a, dk_diag fifofast+s0probe+stagetimers) measured route slow mean 48.47 vs UR3 baseline 47.27 and tripped the pre-set regression flag. PR3 was to run the identical build with dk_diag = fifofast only (probe logging off, stage timers off) to separate instrument overhead from any real delta. Pre-registered bands: slow mean <= ~47.3 clears the build (PR2 flag = instrument overhead + variance); >= ~48.0 means the fixed build itself is slower than resident 34585267 and gets bisected.

Result: NO MEASUREMENT. Both launches rolled the GPU-307 lottery (dbg clocks 1020.0/307.2/1331.2, GPU-307-LOTTERY, at the pre-route gate each time). Per the session rules (two launches max, second 307 = void), the route was never run and nothing was measured. The A/B question stays open; the boot needs another slot on a stock roll.

Session facts: claim held 23:18-23:29Z. Fixed build staged and verified by sha256 (b6bbe08a...; note the resident build is the same byte size, 37,193,925 B — hashes, not sizes, were used at every step). dk_diag was exactly "fifofast\n" (read back). Both launches loaded Brandon's 10% save (file list capture-verified both times) and reached the Mystic Manor spawn rendering cleanly — the fixed build runs the full load path with instruments off; the PR1 init hang did not recur. Log diag line: fifofast=1, all classic toggles 0. Surviving card log is launch 2's (launch 1's was overwritten at relaunch); its boot clocks line and every [dk] window confirm 307.2 throughout, as did both live gate reads. Log: stderr-pr3-ab.log (h3-test + local evidence). crash.txt unchanged (9d83c322...).

Closeout verified: card NRO restored to resident 34585267 (read-back sha256 match), dk_diag.txt deleted, claim deleted, Config.toml still resolution_multiplier = 1.5.

## PR3B — same-build A/B attempt 2 (Nov 15, 2026, build b6bbe08a, dk_diag fifofast ONLY)

Second attempt at the PR3 A/B (attempt 1 voided on two GPU-307 rolls). Queue gate waited ~10 min for SPM's zfreeze-combo claim to release, then claimed (since 2026-11-15T23:41:21Z). STOCK clocks on launch 1 (1020.0/384.0/1331.2, boot line + pre-route gate), one launch total, no relaunch used. Full load path clean (attract, file list "11/14/26 10% The Mystery Machine", Mystic Manor spawn); mid/end route captures clean. crash.txt unchanged (9d83c322...).

Route (marks at uptime 242-434 s):
- Slow frames: n=120, mean 53.68, median 52.00, max 66.30. (PR2 same build, instruments ON: n=90, mean 48.47. UR3 baseline 34585267: n=90, mean 47.27.)
- [vi] 600-frame windows (7): overall mean 44.82 ms/frame (41.71-49.06), busy 41.93, gx 13.78. (PR2: 45.01 / gx 13.74. UR3: ~44.3 / gx ~13.3.)
- GPU avg mean 21.68 ms (21.03-22.55), inside the 21-22 baseline band; draws 898-938/window, +0 merged; replay summaries 0 skipped everywhere (5,330,026 draws cumulative).
- BBox never armed in any window (all counters 0), same as PR2. Pipeline memo: bumps 13,416, hits 2,938, misses 3,615, table misses 0, hit rate 44.8% (PR2 44.7%).

A/B READING (pre-registered bands: <= ~47.3 clears the build, >= ~48.0 indicts it): the slow mean 53.68 lands far above the indict band, and above PR2's instruments-on 48.47 — removing the gated instruments did NOT recover the delta, so instrument overhead is excluded as its cause. BUT the slow sampler is not sampling-identical between boots (120 samples here vs 90 in PR2/UR3 over the same 192 s route), and the [vi] window means — the campaign's fairer cross-boot comparison — are flat vs PR2 and within ~0.5 of UR3, with gx/GPU/draws all in family. The two statistics disagree; the slow-mean regression flag against the fixed build stands unresolved on this evidence and needs a parent call (bisect candidates if pursued: always-on counters, libm bindings, probe reimplementations).

Closeout verified: card restored to resident 34585267 (read-back sha256 match), dk_diag deleted, claim deleted, Config 1.5. Log: switch-logs/h3-test/stderr-pr3b-ab.log.

## PGO TRAIN 1 — capture build regenerated, boot stood down (Nov 15, 2026)
- Capture build regenerated from the current tree (libm fix included): 55,916,741 B, sha256 62e8a8eb4c76c288145352f86fbf1ea09637877b875a2633f52a872ff73c9d7d (round-1 capture was 55,883,973 B / 18a9e469; tree changed since). ELF carries __llvm_prf_names/__llvm_prf_cnts/__llvm_prf_data. Staged byte-identical in Downloads/no100f-nx-sd; manifest refreshed at pgo/manifest.txt.
- The training boot did NOT run: JSRF's pricing phase took the console back under Brandon's priority grant before the console phase began (console was never claimed by FrightWorks; card untouched, resident 34585267 in place). The capture build is ready to stage for the next console gap; one standard-route training boot (~9-11 min in game) will produce the first fw-*.profraw set for C:\dev\no100f-nx\pgo\.


## STAGE 1 PARITY BOOT (Nov 15, 2026, Sudo FrightWorks chat)

Build 812f3f9e (37,206,213 B) staged + hash-verified on card; Boot 1 dk_diag =
fifofast + nativescene, Boot 2 = fifofast only. Clocks STOCK both gates
(1020.0/384.0/1331.2). Both boots: clean load of the 10% save, clean HOME
closes, [dk] replay 0 skipped everywhere, crash.txt unchanged (9d83c322).

VERDICT: STAGE 1 FAILS at the engagement gates. Boot 1's log
(stderr-s1-nativeon.log) contains ZERO [nativescene] lines: no
first-engagement line, no counter lines, no fallbacks, no parse_invalid. The
native path never engaged on hardware; the target texture name was never
learned; native draws = 0.

- Gate 1 (engagement/texture name): FAIL (no engagement at all).
- Gate 2 (native draws > 0): FAIL (0).
- Gate 3 (spawn A/B parity): frames pixel-equivalent (mean abs diff 0.43;
  diffs confined to dynamic elements) but VACUOUS: both frames are GX renders
  because Boot 1 never engaged. Native-draw parity UNTESTED.
- Gate 4 (route survival): captures clean, 0 skipped, but the prop rendered
  via GX throughout; native survival UNTESTED.
- Gate 5 (save/load): UNTESTED (no name learned in Boot 1 to re-confirm).

Safety held: gate-on rendering identical to gate-off, no corruption in any
capture, card restored to resident 34585267 (read-back sha verified),
dk_diag + claim deleted, Config 1.5.

Diagnosability note: the log cannot distinguish "nativescene key not reaching
the gate" from "pin never matched a rendered atomic" - counters print only at
first engagement/conversion/600 arms and there is no boot-time gate-state
line. Next desk step: add a boot-time [nativescene] gate-state + pin-scan
diagnostic line, then re-check the pin against a live cull/render census.
Evidence: ~/workspace/no100f-h3-evidence/stderr-s1-nativeon.log,
stderr-s1-nativeoff.log, capture-s1-spawn-nativeon/off.png,
capture-s1-midroute.png, capture-s1-end.png, STAGE1-PARITY-LOG.md.


## PGO TRAIN 1 (console) - Nov 15, 2026 night: ROUTE COVERAGE FAILED (load wedge); partial profraw harvested

Capture build 62e8a8eb (55,916,741 B) staged from build-deko-pgo, read-back
sha256 verified; dk_diag fifofast only; clocks STOCK by roll (record only -
training does not require stock). Title, menu and file list all clean; save
"11/14/26 10% The Mystery Machine" capture-verified as the top entry.

FAILURE: the save load wedged on the capture build. The LOADING warning
screen stayed static for ~2.5 min (four byte-identical captures); presented
frames stop after frame 4801 (last [vi] window 4202-4801); the log tail is a
repeating memory-card slot scan (CARDOpen ScoobyDoo00..07, ScoobyDoo00 opens,
01-07 return -4) and then guest silence until the HOME close, which itself
took ~15 s to exit. crash.txt unchanged (9d83c322) - a hang, not a crash.
The default build loads this save in ~18-20 s, so the wedge is specific to
the instrumented capture build's load path; cause not diagnosed in this
session.

PARTIAL DATA: the [pgo] interval writer fired at 300 s uptime and wrote
fw-1794799738.profraw (5,655,704 B) in 234 ms, no writer errors. Build tag:
"[pgo] [build] pgo=instrumented capture build: 4374 KiB of counters, 14164
functions". The profraw covers boot/title/menu/load-attempt execution only -
NO Mystic Manor route coverage, so the PGO training objective is NOT met;
a USE build trained on this file alone would profile the front end only.
Harvested to C:\dev\no100f-nx\pgo\ and switch-logs\h3-test\; left on the
card. Log: stderr-pgo-train1.log (evidence + h3-test).

Closeout verified: card restored to resident 34585267 (read-back sha256
match), dk_diag.txt and /agent-claim.txt deleted and confirmed absent,
Config still resolution_multiplier = 1.5. Session record:
~/workspace/no100f-h3-evidence/PGO-TRAIN1-LOG.md.

## STAGE 1 DIAG BOOT — Nov 16, 2026 (diagnostics build 8f4715d3, STOCK, one launch)
- Boot executed clean under the claim: gate line, load flow, 12-leg route (marks 266 s / 457 s), clean HOME close, 11,400 presents, resident restored, claim released. Log: stderr-s1diag.log (190,143 B, sha f846f60c).
- VERDICT: PRIME SUSPECT CONFIRMED. The nativescene gate reads ON (NO100F_NATIVESCENE=1, frame_interpolation_fps=0), the 0x801488B8 callback override is entered (20/20 entry lines), and pin-scan evaluates the pinned target at BOTH iModelCull and the callback: geom=8137A4E0 atomic=8137CFA0 counts=204/200 w18=1 w1C=1 w24=5 w28=5 meshes=5 tex="baseplate_12" verdict=FAIL materials_w24. The ONLY failing pin condition is [geom+0x24]==1 (real value 5); the design's mesh-count==1 condition fails on the same evidence (meshes=5). Both were calibrated on the single-mesh title fixture.
- Cross-scan invariant (89 pin-scan lines, 45 distinct geometries, machine-checked): w18==1 everywhere; w24==w28==mesh-header mesh count everywhere. In this fork [geom+0x24] tracks the mesh count. Verdicts overall: 87 FAIL counts, 2 FAIL materials_w24, zero PASS, zero engaged/learned lines; counters (first fallback) shows pin_structure=1, arms=0, native_draws=0.
- Texture name learned by the scan without engagement: "baseplate_12". Alternate 204/201 (tex "owas2301") also w24=5/meshes=5, still excluded by vert count.
- Corrected pin the evidence supports: counts 204/200 + w18==1 + w24==5 + meshes==5, or counts + w18 + name "baseplate_12" with the ==1 structure words dropped. Re-base + rebuild is the next step (separate job).
- Session facts: STOCK clocks throughout; route [vi] mean 45.18 ms/frame (baseline band, diagnostics boot, not a measurement); replay 0 skipped (5,143,833 draws final); fifofast 0 mismatches; bbox never armed again; crash.txt unchanged.


## PGO TRAIN 2 — Nov 16, 2026 (capture build 62e8a8eb) — WEDGE DETERMINISTIC (Branch B)
- Retry of the train-1 save-load wedge under a full ~6-minute fuse: the LOADING screen stayed byte-identical across five captures spanning the whole wait; debug server responsive throughout; HOME close completed (slow-exit signature).
- Alignment vs train 1 (stderr-pgo-train2.log, 156,000 B, sha256 0e55306d...): total card ops 400 = 400; the final load sequence is 131 ops token-for-token identical; junction identical — last op CARDOpen ScoobyDoo05 -> -4 (train1 line 1265 / train2 line 1491), then the ScoobyDoo06/07 aurora lines, then zero guest lines; h001.HIP dvdreads 0 in both; presented frames stop at 11,401 (train 2 idled longer in attract).
- Verdict: the wedge is DETERMINISTIC under the capture build (2/2 boots, identical junction, full fuse). The diagnosis's step-1 retry question is closed. Next PGO steps (dispatch decision): step 2 new-game discriminator boot and/or step 3 scope-trim capture build (drop aurora_gd/os/ms/card to SPM's seven targets).
- New profraw banked: fw-1794807077.profraw, 5,655,704 B, sha256 2ebb5654..., copied to C:\dev\no100f-nx\pgo\ and h3-test, hash-verified. Coverage stays front-end only (2 banked profraws; a USE build must NOT be trained on them alone). Writer fired at 300 s / 600 s (mid-wedge) / exit, all clean.
- Session log: ~/workspace/no100f-h3-evidence/PGO-TRAIN2-LOG.md. Closeout verified (resident 34585267 restored, dk_diag deleted, crash.txt unchanged).

## STAGE 1 ENGAGEMENT — Nov 16, 2026 (re-based build 4a56f980, STOCK) — MECHANISM PROVEN, 1 NATIVE DRAW
- Boot A (gate ON): pin-scan verdict PASS at both sources for geom 8137A4E0 (204/200, w18=1, w24=5, meshes=5, tex "baseplate_12"); engaged line confirms the name (name_mismatch=0); parse + validation battery passed; cache built; learned DL ptr=80A76020 size=355; first conversion native_draws=1. Then dl_drift=1, then a second distinct DL (ptr=80A76183, size=39) inside the target window triggered the design's disable-for-boot rule. Arms reached 1200+ (periodic prints at 600/1200), all other fallbacks 0.
- Timeline: the entire engagement sequence sits in the TITLE/MENU phase, before the save load. Arms never advanced another 600 across load + spawn + the full 12-leg route; zero conversions on the manor route. The 204/200 "baseplate_12" target is a FRONT-END prop, not a manor-route prop (the PR2 cull census window closed during title/menu/load).
- Boot B (gate OFF): gate line confirms (unset) -> OFF; zero engagements; spawn capture pair diff: mean abs 0.279, 0.42% of pixels >16, 0.01% >64 (dynamic elements only). Caveat: both spawn frames are GX renders (path self-disabled pre-load), so the diff proves build safety, not the converted draw's pixels.
- Gates: (1) engagement + name PASS; (2) native draws > 0 PASS (exactly 1); (3) spawn parity PASS as build safety (caveat above); (4) route survival for the native path NOT ESTABLISHED (target absent from route; build route survival proven: 0 skipped over 7.83M draws, fifofast 0 mismatches, clean close); (5) save/load: engagements precede the load only; load completed normally afterwards.
- Stage 2 inputs: (a) multi-DL windows — convert the learned geometry DL, pass other in-window DLs through on GX (do not disable); (b) choose the next target from a census taken during gameplay; (c) the engaged line prints texel=00000000 (raster 81206980, 64x64) — resolve before scaling conversions.
- Logs: stderr-s1e-nativeon.log / stderr-s1e-nativeoff.log; session log STAGE1-ENGAGE-LOG.md in ~/workspace/no100f-h3-evidence/. Closeout verified (resident restored, dk_diag deleted, crash.txt unchanged, claim released).


## PGO TRAIN 3 — Nov 16, 2026 (capture build 62e8a8eb) — NEW-GAME DISCRIMINATOR: WEDGES WITHOUT THE CARD SEQUENCE
- Purpose (wedge diagnosis Q5 step 2): NEW GAME exercises the DVD/HIP scene-load machinery without the save-load CARD sequence. Result: after choosing NEW GAME (dbg B at uptime 158 s; no save prompt ever appeared), the screen went black and stayed byte-identical across captures spanning the full ~6-minute fuse (uptime 165 -> 517 s, all sha e990a6cc...); debug server responsive throughout; clean HOME close. Wedge declared on the new-game path.
- Alignment vs train 2 (stderr-pgo-train3.log, 81,899 B, 857 lines, sha256 9830ca82...): (a) 12 dvdreads total, HIP reads only boot.HIP + MN/mnu4.HIP + MN/mnu3.HIP — ZERO level HIP reads (no h001.HIP, no first-level HIP); last dvdread of any kind is the attract demo1.bik, before the menu selection. (b) Card ops: 71, ALL in the boot-phase card check (ending CARDUnmount -> 0); NO card sequence ran at/after the NEW GAME selection (train 2's transition was preceded by the completed 131-op save-load sequence). (c) Presented frames stop at 3,601 (last [vi] window 3002-3601); guest silence from selection to HOME close. (d) The profraw writer fired at uptime 300 s during the wedge (229 ms, clean) — exonerated a third time.
- VERDICT: the wedge is NOT specific to the save-load card->scene handoff. The common denominator across trains 1/2/3 is the scene transition never issuing its first stream read, with no card traffic at the train-3 transition — suspicion shifts to the scene-load/DVD side generally (diagnosis hypothesis (d), a poll-driven async completion never delivered at the scene-transition boundary). Caveat: no guest stack dump exists in this build, so identity of the internal wait is inferred from the identical external signature, not proven at the call level. The step-3 scope-trim experiment is weakened for the card modules specifically (no card traffic at this wedge) but OS/GD scope and general instrumentation timing remain live.
- New profraw banked: fw-1794809826.profraw, 5,655,704 B, sha256 fb3780a5..., in C:\dev\no100f-nx\pgo\ and h3-test, hash-verified. Coverage: boot/title/menu/attract + NEW GAME selection path only — still NO gameplay/route coverage (3 banked front-end profraws; a USE build must NOT be trained on them alone).
- Save safety: Brandon's ScoobyDoo00.gci verified UNCHANGED post-boot (41,024 B, sha a90864fa...); no new file in Card A; new game never prompted and created nothing.
- Session log: ~/workspace/no100f-h3-evidence/PGO-TRAIN3-LOG.md. Closeout verified (resident 34585267 restored, dk_diag deleted, crash.txt unchanged, claim released). Clocks this boot: STOCK 1020/384/1331.2 (recorded; training not stock-gated).


## PGO CONTROL — Nov 16, 2026 (RESIDENT build 34585267, no dk_diag) — NEW GAME HEALTHY UNINSTRUMENTED; TRAIN 3 CONFOUND CLOSED
- Purpose: train 3's new-game discriminator had a confound — the port's NEW GAME path had never been exercised on ANY build in our records, so its wedge could have been an unrelated port bug. This control ran NEW GAME on the resident build exactly as train 3 did (same menu path: dbg PLUS to menu from attract, dbg B on NEW GAME, no save prompt at any point; nothing staged, dk_diag absent, Config 1.5).
- Result (stderr-control-newgame.log, 186,605 B, 1,441 lines, sha256 c59df508...): mark at line 1006 (uptime 318 s); line 1009 — three lines later — [dvdread] H0/h001.HIP, the first level read that never issued in trains 1/2/3; line 1018 fmv/cin1.bik. Total dvdreads 15 (train 3: 12, zero after its selection). ZERO [os] [card] ops after the mark — the transition shape matches train 3 exactly except the scene read issued. [window-creeper] restored h001 material: Mystic Manor loaded live.
- Visual record: intro cinematic playing at +10 s after the press (train 3 was static black by +7 s), continuous at +40/+80/+155 s; four B presses skipped the cinematic into controllable gameplay at the Mystic Manor front yard; ~30 s of stick movement (porch, then the Mystic Playground gate with a live PRESS Y TO ENTER prompt). Presented frames ran to the end (last [vi] window 15002-15601; train 3 stopped at 3,601). Clean HOME close; log tail "clean shutdown done" (no error tail).
- VERDICT: CONTROL PASSES. The port's new-game path is healthy uninstrumented; train 3's wedge belongs to the instrumented capture build, not a port bug. Train 3's discriminator was NOT confounded — its conclusion stands: the capture build wedges at the menu->scene transition's first stream read on both paths, consistent with wedge-diagnosis hypothesis (d). Caveat: this control rolled GPU-307 (train 3 rolled STOCK); a functional pass at the slower GPU clock does not weaken the verdict (the wedge is a hard guest stop, not a timing margin; the resident build transitioned in seconds).
- Gate note: the pre-claim capture showed a leftover system error dialog ("The software was closed because an error occurred", running=0, no claim) — the known tail signature of a wedged build's exit (cf. Nov 16 00:07 incident). Claimed first via FTP, dismissed the dialog with A under the claim, re-captured HOME with the Scooby tile focused before launching.
- Save safety: ScoobyDoo00.gci UNCHANGED (41,024 B, sha a90864fa...); Card A still holds only that file — the new game created no save file. Closeout verified (card NRO still resident 34585267, dk_diag absent, crash.txt sha 9d83c322... unchanged, claim deleted and verified absent). Clocks this boot: GPU-307-LOTTERY 1020/307.2/1331.2 (recorded; functional control, not a measurement).
- Session log: ~/workspace/no100f-h3-evidence/PGO-CONTROL-LOG.md.


## STAGE 2 PROVING — Nov 16, 2026 (build 53dbb7bb, native-scene target rma00001) — ALL FIVE GATES PASS

- Setup: build 37,218,501 B sha256 53dbb7bb... (Stage 2: multi-DL fix + per-pointer identity + pin re-based to rma00001 302/198, meshes=1). Three boots under one claim: Boot A gate ON (dk_diag fifofast+nativescene) with the standard 12-leg route, Boot A2 relaunch gate ON (save/load re-confirmation), Boot B gate OFF (dk_diag fifofast only) for the spawn parity pair. Session record: ~/workspace/no100f-h3-evidence/STAGE2-PROVE-LOG.md (incl. the recovery section — the first proving worker died after A2; a recovery session verified live state, found Boot B already launched and sitting at the main menu, and completed it from there).
- Boot A (stderr-s2-nativeon.log, 203,197 B, sha 9bab76ae...): clocks STOCK 1020/384/1331.2 first roll (route numbers measurement-valid). Pin-scan PASS at BOTH sources for geom 813D23A0 (the expected instance) in the instantiation burst right after the h001.HIP load read; bound as binding #1; engaged with parse + validation battery PASSED (serial=179, tex "rma00001", raster 80FABA80, 256x256, nidx=482). Conversions ran ~1 per prop frame continuously from load through spawn/settle: counters arms=600 native_draws=598, then arms=1200 native_draws=1198 before the route BEGIN mark; the spawn capture (uptime ~289 s) sits inside that span, so Boot A's spawn frame contains converted rma00001 draws. One dl_drift at arms=2 — the same DL re-learned and conversions resumed: the per-DL recoverable drift path works as designed, no boot disable. All other fallbacks 0 (pin_structure/name_mismatch/parse_invalid/gen_changed/dl_unlearned/interp_on/ambiguous). Route window: conversions are view-dependent (camera wedges at the manor sign, prop leaves frustum); final native_draws bounded [1198, ~1797]. Route [vi] census ~44.7 ms/f mean, baseline band — one 302-tri prop is a mechanism proof, not a perf lever. Replay 0 skipped in every summary; fifofast 0 mismatches in all 21 windows.
- Boot A2 (stderr-s2-reload.log, 145,590 B, sha 9fb11522...): relaunch re-engaged IDENTICALLY (same geometry, serial=179, same addresses and first tri; texture name re-confirmed post-load). INSTANCE #2 bound live: geom 810691A0 (the exact second heap instance from the s1diag census) as binding #2 with its own cache (cache_builds=2), its own learned DL, and its own conversions (native_draws=597 at arms=600); one dl_drift per instance, both recovered. Per-pointer identity PROVEN with two live instances converting in one boot.
- Boot B (stderr-s2-nativeoff.log, 164,536 B, sha 279b34f4...): gate line reads NO100F_NATIVESCENE=(unset) -> OFF; exactly ONE [nativescene] line in the whole log (the gate line) — bound/engaged/learned/counters/pin-scan all 0. Replay 0 skipped (778,750 draws final summary); fifofast 0 mismatches in all 17 windows; clean HOME close ("clean shutdown done").
- PARITY (spawn pair, 1280x720, fresh diff script s2_parity_diff.py): mean abs diff R 0.338 / G 0.343 / B 0.350; pixels >16 = 0.641%; >64 = 100 px (0.011%), scattered across the middle band with no coherent object-sized blob (grid census; centroid 465,321). Reference: the Stage 1 spawn pair, where BOTH frames were GX renders, measured 0.279 mean / 0.42% >16 / 110 px >64. Verdict: converted-draw parity at the established dynamic-noise floor (fountain spray, smoke, window glow, sparkles, idle animation). Caveat: no per-pixel prop mask exists, so this is a noise-floor verdict, not bit-exactness.
- GATES: (1) engagement on a route prop PASS; (2) recurrence PASS (1,198 native draws pre-route, ~1/prop-frame); (3) converted-draw spawn parity PASS at the noise floor; (4) route survival PASS (STOCK clocks, 0 skipped, 0 mismatches, clean captures); (5) save/load re-confirmation PASS (identical re-engagement post-load + second instance bound live). No save was written in any boot (no save point on the route); Brandon's ScoobyDoo00.gci verified byte-identical at closeout (41,024 B, sha a90864fa...; Card A holds only that file).
- Closeout verified: resident 34585267 restored (card read-back sha match), dk_diag deleted, crash.txt sha 9d83c322... unchanged, Config 1.5 / interp 0 / fps60 false untouched, claim deleted and verified absent; SPM chat relayed the release.


## PGO TRIM BOOT — Nov 16, 2026 (scope-trim capture build 21c3d734 + watchdogat=420) — BRANCH B: TRIM WEDGES IDENTICALLY; SCOPE EXONERATED; FIRST WEDGE STACK EVIDENCE: GUEST SPINNING
- Purpose (wedge diagnosis Q5 step 3 + watchdog decider): boot the capture variant trimmed to SPM's seven Aurora targets (aurora_gd/os/ms/card un-instrumented) on the SAVE-LOAD path, with the watchdog extra-mark armed (dk_diag watchdogat=420) so a wedge yields a guest-thread dump at 420 s uptime. Setup verified: trim NRO staged, card read-back sha256 21c3d734... 55,859,397 B MATCH; dk_diag.txt = "fifofast\nwatchdogat=420\n" read back verbatim; log line 1 confirms the trim build in-log ("4367 KiB of counters, 13971 functions"); [dk] diag line 42 ends "... usedrange=0 watchdogat=420" — mark armed as written. Clocks this boot: STOCK 1020/384/1331.2 (recorded; training not stock-gated).
- Result (stderr-pgotrim-wedge.log, 101,681 B, 1,297 lines, sha256 dfa5f489...): title/menu/load flow normal; file list capture-verified "11/14/26 10% The Mystery Machine" on top; LOAD pressed at uptime 184 s (dbg mark TRIM-LOAD-PRESS). The LOADING warning screen then stayed BYTE-IDENTICAL across captures at uptimes 210 / 245 / 320 / 380 / 444 / 499 s (all sha256 2af93be5...) — the trains 1+2 signature. Boot held open past the mark on purpose; HOME close at ~500 s (Close focused, A, running=0).
- Junction (4th identical observation): post-press card stream 141 ops with 6 CARDMounts (repeated mount/check/probe cycles; CARDOpen ScoobyDoo00 -> 0 + CARDRead -> 0 included); final op CARDOpen ScoobyDoo05 -> -4 (line 1256) followed by aurora ScoobyDoo06/07 attempts with NO [os] result lines; then ZERO guest-origin lines to the end of the log. h001.HIP reads: 0; total dvdreads 12 (front-end only). Last card op ~uptime 300 s; presented frames froze at 4,292 (last completed [vi] window frames 3602-4201).
- WATCHDOG DUMP (lines 1268-1296, the decider): "[vi] [watchdog] 420 s: 4292 guest frame(s) presented, VI black=0" -> "[fiber] 1 guest thread(s); current 0x8026D568" -> "thread 0x8026D568 entry 0x0 state 2 (scheduler) (current)" -> "OSThread state=2 suspend=0 priority=16 queue=0x0". Stack code refs include __AXOutNewFrame+0xCC, __AXOutAiCallback+0x40, fn_800D90AC+0x34, fn_8003D25C+0xAC, fn_800CFD14+0x6C, fn_800BFA64+0x2C, fn_800BE410+0x1EC, fn_800BEFDC+0x440, __init_cpp+0x30, fn_80005304+0x8/+0x134; PC=0x00000000 LR=0x800166B0<fn_800166A0+0x10>.
- JUDGMENT — SPINNING, not blocked (caveats stated): exactly one guest thread exists (the scheduler fiber) and it is the running thread — suspend=0, queue=0x0, not parked on any guest wait queue; every card op returned; the guest issued ZERO HLE-visible calls for ~200 s after the final card op while presents stayed frozen and the host (dbg server, profraw writer) stayed responsive. That is a busy-wait in translated guest code making no HLE calls — the shape hypothesis (d) predicts (the completion the IO manager waits on sits in the poll-driven drain, DvdProcessPendingCallbacks / QueueGuestCallback, pumped only from DVD status HLE calls or the scheduler alarm service, which a pure spin never calls). CAVEATS: the dump walks the running fiber's SAVED context (PC reads 0 — an artifact also present in this boot's healthy 30 s dump), and the stack scan resolves stale return addresses on the shared scheduler stack (the init-era frames __init_cpp / fn_800BEFDC / fn_800BE410 / fn_80005304 and the RunQueueBits/retraceQueue words appear byte-identical in the healthy 30 s dump), so the dump CANNOT name the spinning function; the AX refs are stack residue, not proof the guest sits in audio code. The spin verdict rests on thread state + zero HLE traffic + frozen presents.
- VERDICT B: the trimmed scope does NOT cure the wedge — aurora_gd/os/ms/card instrumentation is EXONERATED as the wedge's enabler, and the trim does not become the training config on this evidence. With the card corridor already removed by train 3 and Aurora scope now removed here, suspicion concentrates on core runtime/guest instrumentation timing interacting with the poll-driven async-completion pump (hypothesis (d) mechanics). The profraw writer fired at uptime 300 s during the wedge (212 ms, clean) — exonerated a fourth time.
- New profraw banked: fw-1794815647.profraw, 5,632,920 B, sha256 55507f1c..., in C:\dev\no100f-nx\pgo\ and h3-test, hash-verified (smaller than the untrimmed 5,655,704 B files — fewer instrumented functions, as expected). Coverage: front-end + load-attempt ONLY, no route. Banked total: 4 front-end profraws; a USE build must still NOT be trained on them alone.
- Save safety: Brandon's ScoobyDoo00.gci verified UNCHANGED post-boot (41,024 B, sha a90864fa...). Closeout verified (resident 34585267 restored, card read-back sha match; dk_diag deleted; crash.txt sha 9d83c322... unchanged; Config 1.5 / interp 0 / fps60 false untouched; claim deleted and verified absent; SPM chat relayed the release).
- Session log: ~/workspace/no100f-h3-evidence/PGO-TRIM-BOOT-LOG.md.

## PGO TRACE BOOT (Nov 16, 2026) — wedge reproduced; [io] discrimination failed on trace cap saturation

Boot: trace capture build f71a03d0 (pgo-trim + traceio rider + watchdogat),
dk_diag = fifofast + watchdogat=420 + traceio (both armed, confirmed in the
[dk] line 42 verbatim). Save-load path; load pressed at uptime 370 s after
dbg mark TRACE-LOAD-PRESS. Log: stderr-traceboot.log (222,607 B, 3,074 lines,
sha256 fb529626...) in switch-logs/h3-test + the local evidence folder.
Full record: TRACE-BOOT-LOG.md (no100f-h3-evidence).

- WEDGE REPRODUCED, 5th identical junction: card stream completes, final op
  CARDOpen ScoobyDoo05 -> -4 (line 3041) + aurora ScoobyDoo06/07 attempts,
  then zero guest-origin lines; h001.HIP reads 0 (all 15 dvdreads are
  front-end); LOADING warning screen byte-identical from ~375 s to 489 s
  (sha 623a83a6...). Clocks rolled GPU-307 (recorded; training not gated).
- WATCHDOG DECIDER FIRED at 420 s (line 3050): 7,762 frames presented, dump
  followed (no "frames advancing" line). The dump reproduces the trim
  boot's spin signature exactly: one guest thread (scheduler 0x8026D568)
  RUNNING, state=2, suspend=0, queue=0x0; same stale stack residue (cannot
  name the spinning function — same caveat). Spin verdict now reproduced
  on two different capture builds.
- [io] CENSUS: queue cb 400 (CAP), drain deliver 400 (CAP), drain DROP 0,
  drain early-return 400 (CAP, all depth=1).
- WHY THE DISCRIMINATION FAILED: the caps saturated in the front-end. Last
  queue cb line 1217, last deliver 1218, last early-return 1845; ZERO [io]
  lines after 1845, while the junction sits at lines 2870-3043. The
  400-line budget (the original SCOOBY_TRACE_IO enqueue cap; the rider
  mirrored it per category, sized on the pump study's <400 pre-junction
  assumption) was consumed by logo/FMV streaming alone: 374 of the 400
  traced enqueues are the 32 KB chunked stream reader (cb 0x8000A46C).
  At the junction the trace was blind in all four categories.
- WHAT THE TRACE DID CLEAR: in the traced window the drain was perfectly
  healthy — deliveries are the exact same multiset as enqueues
  (374/23/2/1 by callback), interleaved 1:1 right up to the caps, and DROP
  never fired (0 of 400 pops): the silent registry drop (dvd.cpp:494) is
  NOT evidenced anywhere observable.
- THE AMBIGUOUS SIGNAL: 400x "[io] drain early-return depth=1", lines
  1361-1845, starting immediately after the WBLogo.bik read (line 1360)
  amid FMV streaming + ARAM traffic. That is the poisoning shape (depth
  stuck at 1 -> every later pump early-returns -> junction completion
  never delivered) AND equally the depth guard's designed re-entrancy
  behaviour under heavy callback traffic. The deliver counter had capped
  at 1218, before the burst began, so stopped-vs-silent deliveries are
  indistinguishable in this log. No exception/fault line exists anywhere.
  POISONING: undecided. STARVATION: undecided (junction enqueues beyond
  the cap). NOT-THIS-DRAIN: cannot be claimed (absence is unobservable).
- NEXT INSTRUMENT STEP (one boot): raise/remove the trace caps, or window
  [io] logging to the junction (arm at a dbg mark / after the title), or
  exclude cb 0x8000A46C from the enqueue trace. The mechanism question is
  where the pump study left it; the trace needs a bigger budget, not a
  new theory. The Q4 stall-gated host-pump fix remains spec'd behind it.
- Input note (this boot): short presses were unreliable in the menu —
  dbg press DOWN x2 did not move the selection (capture-verified), a
  sys-agent DDOWN click did; confirms needed dbg hold B (0.7 s). Held
  inputs are the reliable path on this build.
- Profraw banked: fw-1794817807.profraw (5,632,984 B, sha 308656ef...),
  front-end + menu coverage only; banked total 5 front-end profraws — a
  USE build must still NOT be trained on them alone.
- Closeout verified: resident 34585267 restored (read-back sha match),
  dk_diag deleted, crash.txt + ScoobyDoo00.gci unchanged, Config 1.5,
  claim deleted and verified absent.

## TRACE BOOT V2 (Nov 16, 2026) — NOT-THIS-DRAIN: the DVD/CARD completion drain is cleared

Boot: trace v2 capture build 69b6016f (pgo-trim + traceio + watchdogat=420,
[io] caps raised to 100,000), dk_diag = fifofast + watchdogat=420 + traceio,
save-load path. Wedge reproduced (6th identical junction): load pressed at
uptime 238 s, LOADING screen frozen (captures byte-identical, sha 623a83a6...)
from ~258 s to the HOME close at 491 s; watchdog dump fired at 420 s (4,547
frames presented; single scheduler thread state=2, suspend=0, queue=0x0 —
the spin signature, third reproduction). Log: stderr-trace2boot.log
(1,906,560 B, 38,322 lines, sha256 4fa81ec5...).

The full-visibility [io] trace decides what v1's saturated trace could not:
- Census: 15,988 enqueues, 14,029 deliveries, 0 DROP lines, 6,587
  early-returns (all depth=1). No cap approached.
- Accounting balances EXACTLY: every non-NULL enqueued callback was
  delivered exactly once (FIFO walk; unmatched delivers = 0; per-callback
  counts identical enqueue vs deliver for all nine real callbacks). The
  only undelivered class is cb=NULL flag completions (1,959), whose last
  instance is ~4,900 lines before the junction; none occur in the junction
  window.
- DROP refuted (zero across 14,029 pops). POISONING refuted: deliveries
  resume after every early-return cluster (first deliver 2 lines after the
  first early-return; 2,381 delivers after the last one) — the burst is the
  depth guard's designed re-entrancy, not a stuck guard.
- STARVATION refuted for this drain: the junction's own scan callbacks
  (0x80011D70, one per mount/open cycle) enqueue and deliver four times
  post-mark; the final one (line 38287) delivers on the next line (38288).
  The queue is EMPTY when the guest spins.
- The h001.HIP read is never issued (11 dvdreads, all front-end). After
  line 38288 there are zero guest-origin lines: the guest polls a memory
  flag for ~190 s with no HLE calls. The awaited event never enters
  DvdProcessPendingCallbacks — the wait is in the non-drain class (the
  pump study's Q5(3) family: a retrace-callback or AX-callback counter).
  Consequence: the Q4 stall-gated host pump, as spec'd against this drain,
  would not have cured this wedge; its queue is already empty.

Profraw fw-1794821779 banked (5,632,984 B, sha 5c7368a0...; front-end +
menu only; 6 banked total, still not USE-trainable alone). Closeout
verified: resident 34585267 restored (read-back sha match), dk_diag
deleted, crash.txt + save unchanged, claim released.

## TRACECNT BOOT (Nov 16, 2026) — A AND B JOINTLY LIVE; C REFUTED: the wait is in the retrace/AX feeder family

Boot: pgo-trim capture build db10178b (55,867,589 B, sha256
db10178bc745afd6fe57eb1e02035e8049dde996796ee69d585785acf84faa64;
card read-back sha verified) with dk_diag = fifofast +
watchdogat=420 + traceio + tracecnt. The [dk] line confirms all
three arms (watchdogat=420 traceio=1 tracecnt=1); the [pgo] line
confirms 13,976 functions / 4,367 KiB. Save-load path, mark
TRACECNT-LOAD-PRESS at uptime 218 s. First dispatch stood down at
the gate on SPM's qdeep claim (zero console writes); this run
fired on SPM's release relay. Full record:
~/workspace/no100f-h3-evidence/TRACECNT-BOOT-LOG.md; log
stderr-tracecntboot.log (2,072,977 B, 38,640 lines, sha256
4c925bbfc9ee201e49e97fe524d0a66269af0a6fd35a6b6ba761024e95b06d42).

Wedge reproduced — 7th identical junction. LOADING screen
byte-identical from ~240 s to the close at ~500 s uptime (all
hold captures sha 1a3041a0...); held past the 420 s watchdog
mark; clean HOME close, running=0. Junction: 141 post-mark card
ops ending CARDOpen ScoobyDoo05 -> -4 (line 38537) + aurora
ScoobyDoo06/07 failures; the scan's final completion callback
enqueues and delivers (38540-38542); then zero guest-origin
lines. h001.HIP reads: 0 (11 dvdreads, all front-end). [io]
census: 15,681 enqueues / 13,737 deliveries / 0 DROP / 6,587
early-returns; the gap is exactly the 1,944 NULL-callback
completions — the trace v2 drain verdict stands untouched. The
watchdog fired at 420 s (line 38593: 4,344 frames) and
reproduced the spin signature a fourth time (single scheduler
thread 0x8026D568, state=2, suspend=0, queue=0x0; stack residue
caveat unchanged).

The [cnt] readout (908 samples): in the healthy phase every
counter advances every 250 ms (guest word 0x80271398 equals the
host retraceCount at every sample; ai_cb == ai_blocks always).
Presents stall first at the LOADING screen (frames flat at 4343
from sampler t=215285 ms) while retrace and ai_cb creep through
the scan's last HLE calls; then frames, retrace, guest word,
ai_cb and ai_blocks ALL make their last change at the SAME
sample (line 38545, t=216043 ms: frames=4344 retrace=12936
guest=12936 ai_cb=43130 ai_blocks=43130) and hold identical for
318 s of stall-cadence samples through the dump and the exit.
ai_busy (__AID_Active) is 0 at the freeze and last changed
before the mark — no callback is stuck mid-flight; the AX
delivery machinery simply never runs again. The alarm queue is
EMPTY from t=115586 ms (~100 s before the mark) through the
whole wedge (head 0x0). Retrace callbacks constant at pre
0x800168F0 / post 0x80175BB0.

Verdict per the counter study's signatures: Class A (VI
retrace / EGG::AsyncDisplay) — necessary condition MET (the
retrace counter freezes at the junction). Class B (AX
frame-callback fade counter) — LIVE: ai_cb freezes IN STEP
with presents at the same sample; the "still advancing = B
dead" outcome did not occur. Class C (OS alarm) — REFUTED in
the strong form: no alarm is queued at all. A-vs-B cannot be
separated by this instrument, stated plainly: AdvanceRetrace
and Audio_HLE_Tick share the same guest-activity pump
topology, so both feeders die at the same instant the guest
enters its pump-free poll, and any counter fed by either
freezes identically. The "guest-internal counter" branch is
not supported — two of the three feeders demonstrably freeze
at the junction. The ambiguity does NOT block the fix: the
study's Q5 stall-gated host-thread service runs the existing
deferred machinery for all three feeders
(VI_HLE_ProcessRetracesDeferred, Audio_HLE_PollDeferred,
OS_HLE_ProcessAlarmsDeferred) and delivers whichever one the
guest waits on; the sampler proves the stall state it gates
on is exactly the wedge's state from t=216043 ms onward.

Profraw fw-1794825065 banked (5,633,624 B, sha 35b2e231...;
written mid-wedge at 300 s, writer exonerated a 7th time;
front-end + menu only; 7 banked total, still not
USE-trainable alone). Closeout verified: resident 34585267
restored (read-back sha match), dk_diag deleted, crash.txt +
save unchanged, Config untouched, claim released, SPM relayed.

## STALL SERVICE PROVING BOOT (Nov 16, 2026)

Build b9e739a3 (pgo-trim + stall service), dk_diag fifofast +
watchdogat=420 + traceio + tracecnt (all three arms confirmed on
the [dk] line 42 of both attempt logs). Session log:
STALLSVC-BOOT-LOG.md; logs stderr-sv2-earlyexit.log (attempt 1,
188,978 B, sha ac4abd47...) and stderr-sv2-earlyexit2.log
(attempt 2, 70,846 B, sha 005dbd26...), copies in h3-test.

VERDICT: THE FIX AS BUILT IS NOT BOOT-SAFE — THE JUNCTION WAS
NEVER TESTED. The load was never reached on either launch.

- Attempt 1 (launch 11:25:44Z, titleId confirmed): the service
  fired 57 times across 3 boot-phase episodes (Ep1 frames=2, 50
  firings, closed on resume at frame 4; Ep2 frames=6, 2 firings,
  closed; Ep3 frames=168, 5 firings, never closed). The [cnt]
  counters DID resume under service in Ep3 (retrace 753 -> 771
  -> 773, guest word in step, ai_cb 3224 -> 3320). Then
  userAppExit began mid-episode (~13 s uptime) and crash.txt held
  a NEW crash: data abort (0x101), esr read, guest address
  0x42e531c0 through the flat window (nothing mapped), main
  thread (guest scheduler), pc elf+0x84027c.
- Attempt 2 (relaunch, same claim): died INSIDE Ep1 — 1 episode,
  32 firings at frames=2, zero closes, exit at ~5 s. crash.txt:
  data abort (0x101), esr WRITE, guest address 0x83000000, main
  thread, pc elf+0x1380b64. Different address, fault type and PC
  from attempt 1: corruption, not a single bad delivery.
- Failure mode, named: the gate opens on ANY present stall
  > 250 ms after the first frame, including ACTIVE-WORK stalls
  (the frames=2 stall is shader compilation, not a wait). The
  host thread then delivers guest retrace/audio callbacks
  concurrently with the guest main thread's active work, and
  guest state corrupts — the race the counter study flagged.
  At the wedge junction the guest is in a pure pump-free poll
  (the designed-for case), but this build cannot survive the
  boot phase to reach it. Control: the tracecnt build
  (identical minus the service) passed this boot phase in all
  seven prior capture boots. h001.HIP reads: 0 in both attempts
  (no load press was ever made); the 420 s mark was never
  reached. A retry needs the gate narrowed to wait-state
  stalls — a design change, not a boot variation.
- Profraws banked (pgo\ AND h3-test, hash-verified):
  fw-1794828376 (5,633,984 B, sha a1cdc7a6...) and
  fw-1794828512 (5,633,984 B, sha 7028f532...). Both <=13 s of
  boot, front-end only, NO route coverage. Banked total: 9.
- Closeout verified: resident 34585267 restored (read-back sha
  match); dk_diag deleted/absent; crash.txt was overwritten by
  the two crashes and then restored byte-exact from the LOQ
  copy switch-logs/fix-boot/card-crash3.txt (1,735 B, sha
  9d83c322..., read-back verified); save 78-GIHE-ScoobyDoo00.gci
  unchanged (41,024 B, sha a90864fa...), Card A holds only it;
  Config untouched; claim deleted/absent; SPM relayed.

## STALL SERVICE V2 PROVING BOOT (Nov 16, 2026)

Build 72cbdfc9 (feeder-silence gate), dk_diag fifofast +
watchdogat=420 + traceio + tracecnt, save-load path intended.
Session log: STALLSVC2-BOOT-LOG.md; log stderr-stallsvc2boot.log
(2,009,375 B, 34,968 lines, sha 25b94e5b...).

- Boot phase PROVEN SAFE under the narrowed gate: [stallsvc]
  lines in the whole boot: ZERO. The game presented through both
  v1 death windows (frames=2 shader badge at ~6-8 s; Heavy Iron
  logo at 31 s) and the whole front-end. [dk] line 42 arms all
  confirmed; [pgo] line 1: 13,980 functions (v2 scope); clocks
  STOCK (recorded only). In-log: "shaders 131 from the cache
  file, 0 compiled".
- The boot then stalled at the Intro->title transition and the
  junction was NEVER REACHED (no menu, no load press, no h001.HIP;
  11 dvdreads, all front-end, last Intro.bik line 9919). Presents
  froze at frame 2801 (first frozen [cnt] sample line 33624,
  t=118161 ms; the healthy tracecnt boot was at frames=2843 and
  advancing at the same instant). Captures uptime ~145-446 s all
  byte-identical (near-black frame); a dbg PLUS probe changed
  nothing.
- This is a THIRD stall shape: retrace and ai_cb NEVER froze
  (7091 -> 26873 and 23636 -> 89578 across the ~360 s freeze,
  guest word tracking retrace at every sample; alarm queue
  empty), so the v2 gate's all-feeders-silent opening condition
  was never met and it correctly stayed shut — the junction half
  of the prediction is UNTESTED, not failed, and no study
  falsifier fired. The 420 s watchdog dump differs from the
  wedge signature: the single guest thread reads OSThread
  state=4, queue=0x802716E4 — PARKED on a wait queue (wedge:
  state=2, queue=0x0, spinning). [io] census healthy: 13,692
  enqueues / 11,736 deliveries / 0 DROP (gap = NULL-callback
  completions); enqueues stop because frames stop. What wakes
  that queue is not identifiable from this log; whether the
  capture build's timing caused the park is undecidable from
  one boot (no control ever idled at this transition).
- No crash: crash.txt byte-identical to baseline throughout,
  never overwritten. Clean HOME close past the 420 s mark;
  userAppExit clean shutdown.
- Profraw: fw-1794830317.profraw (5,634,264 B, sha 965eb5a3...)
  banked to pgo\ AND h3-test, hash-verified. Front-end only, no
  route coverage. Banked total now 10 (all front-end).
- Closeout verified: resident 34585267 restored (read-back sha
  match); dk_diag deleted/absent; save unchanged (41,024 B, sha
  a90864fa...), Card A holds only it; Config untouched; claim
  deleted/absent; SPM relayed. NEXT: re-run the proving boot to
  reach the junction; if the front-end park recurs, it becomes
  its own diagnosis target.

## STALL SERVICE V2 PROVING BOOT RERUN (Nov 16, 2026) — gate vindicated; episode opened at the junction; cure failed, guest crashed

- Session shape: the first re-run worker died after recon; a
  recovery worker verified live state first (claim present,
  since 2026-11-16T12:11:56Z; v2 build already staged on the
  card, read-back sha 72cbdfc9...; dk_diag absent; nothing
  running), adopted the claim, and ran the boot. dk_diag
  exactly fifofast + watchdogat=420 + traceio + tracecnt
  (41 B, read back verbatim); [dk] line 42 confirms all three
  arms; [pgo] line 1 confirms 13,980 functions. Clocks rolled
  STOCK (line 43; recorded only). Log:
  stderr-stallsvc2boot-rerun.log (2,976,648 B, 54,469 lines,
  sha f5c9b319...). Session log STALLSVC2-BOOT-RERUN-LOG.md.
- Boot phase: ZERO [stallsvc] lines before the load — in
  fact all 8 in the boot sit at lines 54443-54464, after the
  load press. Second v2 boot in a row with a silent boot
  phase.
- The attempt-1 Intro->title PARK DID NOT RECUR: [cnt]
  samples at t=117089-117848 ms (lines 33574-33636) show
  frames 2786 -> 2808 advancing through the exact point
  attempt 1 froze (2801); title reached, menu flow completed,
  frames reached 7517. One false start documented: the first
  file-list press (mark 50460, 317 s) bounced back to the
  slot screen without loading (frames advanced throughout);
  the effective load press is mark STALLSVC2R-LOAD-PRESS2
  (line 54390, uptime 397 s).
- Junction: the scan ran; its completion callback 0x80011D70
  delivered for the last time at line 54441. Freeze sample
  (line 54442, [cnt] t=395002 ms): frames=7517,
  retrace=23680, guest=23680, ai_cb=78942, alarm queue
  empty. EPISODE ENTRY line 54443: feeder silence >=2000 ms
  (frames 2100, retrace 2000, ai_cb 2000, enqueues 2000)
  with work due (retrace=1 alarm=0) — the gate opened at
  exactly the designed-for state, ~2.1 s after the last
  presented frame.
- Under service the counters resumed (line 54453:
  retrace 23684, ai_cb 78966; line 54465: retrace 23688,
  ai_cb 78994 — vs freeze 23680 / 78942), but frames NEVER
  resumed (7517 throughout) and h001.HIP was NEVER read
  (zero occurrences; 13 dvdreads, all front-end). After
  firing #5 (line 54457), userAppExit at line 54460; the
  process exited via its crash handler. CRASH: crash.txt
  overwritten by this boot — 2,104 B, sha 3c1c8302...:
  data abort (0x101) esr=0x92000006 (write), guest
  0x389dfff4 through the flat window (wild pointer), main
  thread (guest scheduler), pc elf+0x5279ec, lr
  elf+0x1d5f180. Restored byte-exact at closeout (below).
  [io] census: 22,546 / 20,022 / 0 DROP / 8,455
  early-returns. The 420 s watchdog mark was never reached
  (boot ended ~400 s uptime).
- VERDICT: EPISODE OPENED, CURE FAILED — ENDING IN A CRASH.
  The feeder-silence gate is a proven discriminator (zero
  episodes outside the junction across two v2 boots; opened
  only at the true four-feeder-silent junction). The
  delivery still does not cure: counters move, presents and
  the level read do not follow, and this time the guest
  crashed ~2.4 s into the episode — seven prior wedge boots
  sat in the identical spin for minutes without crashing.
  The GATE-DISCRIMINATION-STUDY falsifier "any crash under
  the narrowed gate" has fired; its prescribed fallback is
  counter-only delivery under the same gate.
- Profraw: fw-1794831292.profraw (written 300 s + at exit)
  banked to pgo\ AND h3-test: 5,634,264 B, sha b2932b22...,
  hash-verified. No route coverage. Banked total now 11
  (all front-end).
- Closeout verified: resident 34585267 restored (read-back
  sha match); dk_diag deleted/absent; crash.txt restored
  byte-exact from the LOQ copy (1,735 B, sha 9d83c322...;
  the overwrite is recorded above, not papered over); save
  unchanged (41,024 B, sha a90864fa...), Card A holds only
  it; Config untouched; claim deleted/absent; running=0;
  SPM relayed the release.

## STALL SERVICE V3 STAGE A PROVING BOOT (Nov 16, 2026)
- Build 55fc8619 (stallsvc v3, pgo-trim), dk_diag fifofast +
  watchdogat=420 + traceio + tracecnt + stallsvc=count, read
  back verbatim (56 B); [dk] line ends "stallsvc=count",
  [pgo] 13,983 functions; boot clocks STOCK (recorded only).
  One launch (15:02:16Z) under the claim protocol; the first
  dispatch had stood down on the CTTR PS2 claim, this one
  fired on their release. Log stderr-stagea.log (849,966 B,
  8,267 lines, sha 2f3e8532...); session log
  STAGEA-BOOT-LOG.md.
- THE JUNCTION WAS NEVER REACHED: presents froze at frame
  410 at t=20,942 ms, during fmv/THQLogo.bik (last [dvdread],
  line 2669; all 8 dvdreads front-end). Title, menu, load
  press and card scan never happened; h001.HIP absent (0).
- [stallsvc]: ZERO episodes, zero firings, zero witnesses —
  the only [stallsvc] line in the boot is the [dk] line. The
  feeder-silence gate NEVER OPENED because its condition was
  never met: across 2,728 [cnt] samples retrace advanced
  1,258 -> 41,592 (guest word tracking exactly) and ai_cb
  4,191 -> 138,634 while frames sat at 410 for ~11.5 min.
- Watchdog dumps (30 s and the 420 s extra mark) show the
  SAME PARK SIGNATURE as v2 attempt 1: single guest thread
  0x8026D568, OSThread state=4, queue=0x802716E4. The park
  has now recurred (attempt 1: frame 2801, end of Intro.bik;
  here: frame 410, THQLogo.bik — one FMV earlier; both
  FMV-adjacent, same wait queue). Per the attempt-1 rule,
  queue 0x802716E4 is now its own diagnosis target.
- VERDICT: the raw-word vs callback-counter DECIDER IS NOT
  ANSWERED — counter-only delivery never fired, so Stage A
  is untested, not falsified. NOT a falsifier boot: no crash
  (crash.txt byte-exact baseline, never overwritten), and
  the ai_cb movement was the guest's own pumping outside any
  episode, exactly what the gate is designed to stay shut
  for. The gate's negative discrimination held for the third
  consecutive v2/v3 boot (it has only ever opened at the
  true junction, v2 rerun). Guest exited cleanly on HOME
  close ("clean shutdown done"). [io] census: 1,951
  enqueues / 1,768 deliveries / 0 DROP / 763 early-returns.
- Profraw fw-1794841368.profraw banked to pgo\ AND h3-test
  (5,634,832 B, sha fddab57d...), FRONT-END ONLY. Banked
  total now 12 (all front-end).
- Closeout verified: resident 34585267 restored (read-back
  sha match); dk_diag deleted/absent; crash.txt baseline
  (no restore needed); save unchanged (41,024 B, sha
  a90864fa...), Card A holds only it; Config untouched;
  claim deleted/absent; running=0; release relayed to the
  CTTR PS2 chat (Brandon's reorder: CTTR diagnostic next,
  SPM regpath behind CTTR).

## COMBINED PROVING BOOT (Nov 16, 2026) — Stage A decider + tracewake park catch, build 83f79ed8

ONE launch of the pgo-trim capture build 83f79ed8 (55fc8619 + the tracewake seam instrument; 55,879,877 B, sha256 83f79ed8...) with dk_diag = fifofast + watchdogat=420 + traceio + tracecnt + stallsvc=count + tracewake (66 B, read back verbatim; the [dk] line ends "stallsvc=count tracewake=1", [pgo] 13,986 functions, boot clocks STOCK recorded only). Session log: COMBINED-BOOT-LOG.md; log stderr-combined.log (721,244 B, sha 6f8f16c4...).

VERDICT A — JUNCTION-NOT-REACHED. The front-end park recurred for the THIRD time: presents froze at frame 410 during fmv/THQLogo.bik (t=21,197 ms; the same frame and movie as the Stage A boot), the title was never reached, h001.HIP never issued, and the stall service never fired (zero [stallsvc] episodes — the feeder-silence gate stayed shut correctly while retrace advanced 1,273 -> 28,028 and ai_cb 4,243 -> 93,428 across ~8.2 minutes of [cnt] samples). Stage A remains untested; the raw-word vs callback-counter question is still open. Not a falsifier.

VERDICT B — PARK-WITHOUT-SIGNATURE on the brief's literal test, with the park caught at the seam in the same family. The tracewake census for the whole boot: 408 [wake] wakeup lines, ALL from the advancer (lr=0x80175A50) and ALL result=wake-to-empty (397 inrestore=1, 11 inrestore=0; zero result=woke), and exactly ONE [wake] sleep line — the fatal one (line 5954: thr=0x8026D568, lr=0x80176B98 inside the waiter func_80176B30, A=0 B=2, guest=1269), after which no wakeup exists. The deciding pair as specified (wake-to-empty inrestore=1 immediately followed by an out-of-wait-set sleep snapshot) did NOT occur: the wakeup immediately before the sleep (line 5952, guest=1268, A=0 B=1 arm=1 latch=1) has inrestore=0 — the advancer's advance of A (2 -> 0) delivered via the draw-done callback drain — and the sleep snapshot A=0 B=2 gives diff = B - A = +2, IN the wait set {-1, 2}. Per the study's own framing this is its named secondary path (same shape, different delivering context), and it refutes the restore-synchronous variant as the proven path for this instance; suspicion redirects to the advancer's gate / the empty-queue discard itself. What is now observed directly rather than inferred: the wake channel on this queue is discarded 408/408 times in an FMV boot (the waiter does not sleep per frame during FMV — ~1 seam line per presented frame, all wakeups), forward progress normally comes from the waiter finding its condition already satisfied at check time, and the one drain wait at the movie's end parked permanently because the satisfying advance's wake had already been discarded and no successor signal can be generated. A wake latch or deliver-before-suspend at the HLE seam addresses both paths. Watchdog dumps at 30 s and 420 s show the identical park signature (thread 0x8026D568, state=4, queue=0x802716E4); [io] census 1,951/1,768/0 is numerically identical to the Stage A boot; no crash (crash.txt byte-exact baseline throughout); clean HOME close. Profraw fw-1794845203.profraw banked (front-end only); banked total now 13, all front-end. Closeout fully verified: resident 34585267 restored + read-back match, dk_diag deleted, save untouched, claim deleted + verified absent, releases relayed to the SPM and CTTR chats.

## PARKWATCH PROVING BOOT (Nov 16, 2026) — PARTIAL/OTHER + JUNCTION-NOT-REACHED

One launch of d64ed2b9 (83f79ed8 + parkwatch), dk_diag = fifofast +
watchdogat=420 + traceio + tracecnt + stallsvc=count + tracewake +
parkwatch (76 B, read back verbatim; [dk] line ends "stallsvc=count
tracewake=1 parkwatch=1"). Session log PARKWATCH-BOOT-LOG.md;
stderr-parkwatch.log (3,553,254 B, 58,107 lines, sha b479ca77...).

VERDICT P: PARTIAL/OTHER. At the THQLogo boundary there was no freeze
to cure: fine [cnt] samples flow through frames 401 -> 412 -> 413 ->
417 with no >=2 s stall and no [wake] sleep; fmv/HILogo.bik OPENED
(log line 5970 — never opened in the Stage A / combined boots) and the
full movie chain played (WB -> THQ -> HI -> RW4 -> Intro -> attract
Intro replay). The race simply did not fire at THQ this boot.
Parkwatch neither fired nor had anything to cure there. The park then
recurred at a NEW boundary — the TITLE screen: frames froze at 4,473
from t~215 s for the rest of the boot (~250 s of samples; retrace
12,879 -> 27,505 and ai_cb 42,927 -> 91,681 advancing, guest_ok=1).
The boot's ONLY [wake] sleep (line 54758: thr=0x8026D568,
lr=0x80176B98, A=1 B=0 arm=0 latch=0, guest=12445) is the identical
seam shape as the THQ parks, and the 420 s watchdog dump shows the
thread in state=4 on queue=0x802716E4. Yet [parkwatch] produced ZERO
lines in the whole log — no signature, no fire, no outcome — across
~250 s of continuous log-visible signature conditions. The detector
never formed its signature (internal cause not determinable from the
log: raw thread-word offsets +0x2C8/+0x2DC vs the scheduler-read
fields, the documented DetClock::Enabled() suppression, or its
sampled frame counter). NOT a Q3 falsification: no fire was ever
attempted, so the targeted wake remains untested. Whole-boot seam
census: 4,471 wakeups, all advancer wake-to-empty, zero result=woke,
exactly one sleep. [io] 21,862 / 18,897 / 0 DROP. No crash; clean
HOME close past the 420 s mark; no relaunch.

VERDICT A: JUNCTION-NOT-REACHED. The title was reached — a first on
the capture line — but the presenter parked at the title before the
menu/load sequence could run (all menu inputs landed on the parked
guest; the settings overlay never opened on a MINUS hold). Zero
[stallsvc] episodes; h001.HIP never issued; Stage A remains
UNTESTED, not falsified. Open accounting note: the boot's [pgo] line
reads 13,987 functions / 4,368 KiB of counters vs the build log's
ELF-section 13,991 / 559,551 — the staged NRO was sha-verified
d64ed2b9 pre-launch, so the binary is not in doubt; the counts
evidently measure different sets.

Profraw fw-1794848895 (5,635,584 B, sha e02c333a...) banked to pgo\
and h3-test, hash-verified: full front-end movie chain + title/
attract coverage (4,473 presented frames; no save-load, no
gameplay). Banked total now 14. Closeout fully verified: resident
34585267 restored byte-exact, dk_diag deleted, crash.txt and save
at baseline, Config untouched, claim deleted 17:18:35Z and verified
absent, releases relayed to the SPM and CTTR chats.

## PARKWATCH PROVING BOOT RE-FIRE (Nov 16, 2026) — build af546dfc, RECOVERED SESSION

Session log: PARKWATCH2-BOOT-LOG.md (written by the recovery worker; the
original boot worker died mid-session without a report). One launch,
dk_diag seven keys (fifofast / watchdogat=420 / traceio / tracecnt /
stallsvc=count / tracewake / parkwatch), save-load path. A claim
collision with CTTR occurred at session start (~17:37-17:41Z: CTTR's
claim write overwrote the file, one stray DRIGHT into the boot phase,
then CTTR stood down and deferred in writing); the claim as found by
the recovery carried the re-asserted FrightWorks text, and the game was
found still running, frozen byte-identical at the LOAD GAME warning
screen since the load press (~35 min). The recovery completed the owed
closeout (HOME close, running=0 verified).

VERDICT P — PARKWATCH NOT EXERCISED: the front-end movie chain
(WBLogo -> THQLogo -> HILogo -> RW4 -> Intro) and title completed
cleanly, frames advanced continuously to 6,259, and the display-sync
waiter never slept once in the whole boot (6,259 [wake] wakeups, ZERO
sleeps) — no park arose, so the cure remains unproven. The af546dfc
detector fix is VERIFIED ALIVE: the new [parkwatch] diag line fired at
the first freeze window (boot phase, frames=2) with sane values through
the detector's own reads (stateOk=1 state=2, queueOk=1 queue=0x00000000,
retrace=174, ai_cb=578), correctly not a signature. No signature/fire
lines exist in the boot; parkwatch also correctly stayed out of the
junction freeze (feeders frozen there; its signature requires feeders
advancing).

VERDICT A — STAGE A NO-WAKE (the decider is ANSWERED): at the junction
(mark PARKWATCH2-LOAD-PRESS at 323 s; card scan completed; h001.HIP
never issued, 0 occurrences in 71,054 lines), count-mode ran TEN
complete 600-firing episodes plus an 11th interrupted at firing #315 —
6,315 count-only deliveries, guest retrace word advanced 19,187 ->
25,500 tracking the delivery 1:1 — and the guest NEVER woke: frames
frozen at 6,259, enqueues never moved (every re-entry certifies a fresh
2,000 ms enqueue silence), ai_cb frozen at 63,967 with zero falsifier
witnesses. The junction wait does NOT read the raw retrace count word;
it is CALLBACK-MAINTAINED. Stage A is falsified as the cure; Stage B
(stallsvc=burst) is the indicated next test. Watchdog 420 s dump
confirms the wedge silhouette at the junction (thread 0x8026D568
state=2 spinning, queue=0x0 — not a park).

Closeout (all verified by the recovery): stderr-parkwatch2.log
(4,547,865 B, sha 1d143bd4...) in evidence + h3-test hash-identical;
profraw fw-1794850767 banked to pgo/ + h3-test (5,635,576 B, sha
2413e3e2...; periodic 5-minute write; front-end/title/menu/junction
coverage, NO route; banked total now 15); resident 34585267 restored +
read-back sha match; dk_diag deleted; crash.txt baseline, never
overwritten; save baseline, Card A only that file; Config untouched;
claim deleted + verified absent; release relays sent to SPM and CTTR.

## STAGE B PROVING BOOT (Nov 16, 2026) — JUNCTION NOT REACHED; PARKWATCH CURE PROVEN 8/8, BOOT LOST TO THE FIRE CAP

Attempt 1 stood down at the gate behind CTTR's D2 window (claim file arbitrated; zero console writes). Attempt 2 (build af546dfc, dk_diag seven keys with stallsvc=burst; claim 18:23:58Z; launch 18:25:28Z; log stderr-stageb.log, 492,606 B, sha a9215d98...) never reached the junction: the boot-phase present crawl made the display-sync waiter sleep — and park — on nearly every present cycle. Parkwatch formed NINE signatures and fired EIGHT times (its per-boot cap); every fire produced a result=woke wakeup on 0x802716E4 and an outcome line "frames resumed (N -> N+2, 50 ms after fire #k)" — the targeted wake is PROVEN in the act, eight for eight, exactly as PARK-STUDY-2's Q3 post-sleep analysis predicted (the first result=woke lines ever recorded on this line). The ninth park (frame 30, fmv/WBLogo.bik, signature line 2045) got no fire: the 8-per-boot cap, which counts successful cures against the same budget as failures, was exhausted in the boot phase. Frames stayed at 30 for ~455 s with feeders live (retrace 1,598 -> 27,176, ai_cb 5,327 -> 90,587); watchdog dumps at 30 s and 420 s both show thread 0x8026D568 state=4 on queue 0x802716E4. Clean HOME close, clean exit tail, no crash. VERDICT P: CURED per-instance (8/8), boot lost to cap exhaustion — the cap policy, not the wake, is the failure; the "3 dry fires" stop never engaged (no dry fires), and cap exhaustion logs no line (observability gap). VERDICT B: JUNCTION-NOT-REACHED — burst mode was armed and delivered ZERO bursts (the feeder-silence gate never held in the front-end; the services are disjoint by construction and both behaved as designed); BURST-WAKE / BURST-NO-WAKE / CRASH-FALSIFIER all unrun, burst neither proven nor falsified. New log-supported reading of the park: it is not FMV-boundary-specific — during any sufficiently slow present stretch the presenter exercises the sleep path and the lost-advance defect fires on nearly every sleep (contrast the parkwatch2 boot: zero sleeps in 6,259 fast-presented frames). The front-end park storm is now the dominant failure mode for reaching the junction at all; the practical path to a Stage B verdict is a parkwatch fire budget that does not spend successful cures (or a boot that rolls a fast front-end). Profraw fw-1794853561 banked (5,635,576 B, sha 635bc9a3...; boot-phase front-end only; total now 16) — with a recorded banking correction: the first pass used the text-mode FTP helper and banked a corrupt 6,267,003 B copy, caught by size mismatch and overwritten at both destinations from a binary-safe download. Closeout fully verified: resident 34585267 restored (read-back match), dk_diag absent, crash.txt baseline never overwritten, save baseline, Config untouched, claim deleted + verified absent, release relays sent. Full record: STAGEB-BOOT-LOG.md attempt 2.

## PARKWATCH BUDGET FIX (Nov 16, 2026) — BUILT 1f173d34, NOT BOOT-TESTED

The Stage B attempt-2 verdict named the failure: parkwatch's 8-per-boot fire cap counted successful cures against the same budget as failures, so eight proven cures in the boot-phase park storm exhausted it and the ninth park (frame 30, WBLogo.bik) got no fire; cap exhaustion also logged no line. Fix (hle/vi.cpp only; pre-edit backup C:\dev\no100f-nx\park3-backup\, sha ae7a3909... byte-verified before editing): the fire gate now consults a new pwDryTotal counter — dry fires this boot in total — capped at 8; ++pwDryTotal sits in the dry-outcome block beside ++pwDryFires, and a fire whose outcome is "frames resumed" spends nothing, so total fires are unbounded for cures (pwFires remains only as the serial printed in the outcome lines). The runaway guards are unchanged: 1 fire per episode, and the 3-consecutive-dry stop (pwStopped plus the existing "wake not taking" line). Observability: when the budget cap engages, the dry-outcome block logs once per boot: "[parkwatch] fire budget exhausted: %u dry fires this boot; no further fires". Everything else byte-identical (detector signature, feeder-baseline fix, stallsvc count/burst, tracewake, watchdog, diag line); a marker sweep of the source tree confirms vi.cpp is the only file changed vs the af546dfc tree state. Rebuilt pgo-trim as a small incremental under Brandon's Nov 9 parallel rule (the JSRF cfgfix build claim left untouched; board-noted): exit 0, one TU recompiled plus link. NRO 55,879,877 B, sha256 1f173d340f9d4f5663c506640f0e346ae3db09ea207734174fe9b8eecb3fd970 (same size as af546dfc; the hash is the identity). ELF profile sections: 13,987 functions (__llvm_prf_data 895,168 B / 64) / 559,200 counters (__llvm_prf_cnts 4,473,600 B / 8) / names 266,696 B — vs af546dfc +0 functions, +2 counters, +0 names bytes; the new format string is present in the ELF. h3-test copy refreshed cmp-identical; Downloads staging untouched (53dbb7bb). NOT boot-tested. Next: the Stage B re-fire on this build (stallsvc=burst at the junction), which now has to survive the front-end park storm on a parkwatch whose budget cures cannot spend before the junction decider can run. Full record: PARKWATCH3-BUILD-LOG.md.

## STAGE B ATTEMPT 3 — PARKWATCH BUDGET FIX PROVEN AT 714 CURES; JUNCTION STILL NOT REACHED (Nov 16, 2026)

Build 1f173d34 (budget fix), ONE launch, dk_diag seven keys with
stallsvc=burst. Full record: STAGEB-BOOT-LOG.md attempt 3;
stderr-stageb3.log (4,274,614 B, sha 3956e114...).

VERDICT P: CURED-AND-SUSTAINED. The front-end park storm this
boot was two orders of magnitude beyond attempt 2's: the
display-sync waiter parked on essentially every present cycle
from frame ~714 to the end. Parkwatch census: 714 signatures,
714 fires, 713 confirmed "frames resumed" outcomes (fire
#714's wakeup is result=woke; its outcome window was cut by
the close), 0 dry fires, 0 budget-exhausted lines, 0
wake-not-taking. [wake] census: 714 sleeps, 714 result=woke,
712 wake-to-empty. The old cap killed attempt 2 at park #9;
the dry-only budget cured 714 parks in one boot and the boot
was still alive when closed. The full movie chain completed
(WB/THQ/HI/RW4/Intro) and the title was reached — a capture-
line first under a full storm.

THE CRAWL (the boot's other headline): presentation was
paced entirely by the cure. Each park costs the detector's
fixed ~2.05 s (2,000 ms freeze + 500 ms hold), then ~4 frames
present before the next sleep: steady-state ~1.12 fps (114
frames per 101.5 s), 2,853 frames in 1,966.7 s, while retrace
ran at ~59.8 Hz and ai_cb at ~199/s throughout. The guest's
work rate was never the limit; the lost-advance defect fired
every cycle and the cure's latency became the frame rate.

VERDICT B: JUNCTION-NOT-REACHED. Zero bursts (feeder-silence
gate never held; disjoint by construction), h001.HIP never
issued, the load press never happened: at ~1.1 fps the PRESS
START window (<20 s) repeatedly lapsed into the attract demo
faster than inputs could be driven, and the van menu was
never reached. Brandon, watching live, killed the game from
the console himself (clean HOME close; exit tail complete)
and directed salvage. Burst remains neither proven nor
falsified; the decider needs a boot that rolls a fast
front-end (park-free, like the parkwatch2 boot) or a
menu-driving method that tolerates the parkwatch cadence.
Profraw fw-1794854838 banked (front-end + title/attract;
17 total). Closeout fully verified (resident restored,
dk_diag deleted, crash.txt/save at baseline, claim deleted,
relays sent with Brandon's queue change: CTTR next, SPM
behind).

## SEAM FIX — design + build (Nov 16, 2026)

Desk job (no console contact). Full records: SEAMFIX-DESIGN.md and
SEAMFIX-BUILD-LOG.md in the VM evidence folder
(~/workspace/no100f-h3-evidence/). Build: **c8da98d8** (55,883,973 B,
sha256 c8da98d8d548e315951e1dcc57d1c2b30ec4fe27e9bd02b24024785db95a0381),
pgo-trim capture line, in switch-logs/h3-test/no100f-nx.nro. Downloads
staging stays 53dbb7bb. Backups of all touched files:
C:\dev\no100f-nx\seamfix-backup\ (byte-verified pre-edit).

**Design verdict (the honest part).** PARK-STUDY's fix (a) as literally
specified — deliver-after-link at the OSSleepThread seam, over today's
queues — does **not** cure the convicted park, and does not cure the
refuted restore-race subclass either. At the fatal sleep the final
draw-done has already been delivered and spent (combined log: drain
delivery precedes the sleep line); the deliveries still due at link
time (retraces) run the post-callback, which gates out on arm == 0
forever. What was built instead is fix (a) **plus** the minimal variant
the brief anticipated:

- **Element 1 — the seam pass** (hle/os/os_sleep.cpp): after the thread
  links into its wait queue (post-tracewake block, pre-SelectThread
  tail), run the idle pump's delivery set once on the sleeper's
  context — held GX class, due alarms + general drain, due retraces —
  inside the deferred-callback scope (blocking callbacks busy-return;
  no fiber switch can occur inside the pass), with the sleeper's
  register file saved/restored around it. A delivery that wakes the
  sleeper pops it through WakeupThreadQueue; the unmodified tail
  re-selects it and the sleep returns instead of suspending.
- **Element 2 — PE-interrupt class deferral** (hle/storage/dvd.cpp,
  hle/gx/gx_fifo.cpp): the three callbacks queued from
  RaisePeInterruptForBp (token, draw-done, GX finish-queue wake) go to
  a held deque instead of the eager general drain, delivered only at
  the seam pass and the scheduler idle pump (both pump sites,
  DeterministicIdleStep included, so deterministic mode cannot starve
  the class; the seam pass itself stays DetClock-gated off). The GX
  breakpoint callback is deliberately NOT held. All other classes
  (DVD/CARD/ARAM/EXI/Nand/alarms/retraces/audio) keep today's queue
  and timing exactly — the junction wedge's classes are untouched.

**Why it covers the convicted instance** (traced instruction-exact in
the design): the final present's draw-done stays pending in the held
deque when the waiter links; the seam pass delivers it against the
final counter state — mismatch → the advancer advances A and wakes
the *linked* thread; match → arm = 1, and the same pass's retrace poll
runs AF0 (latch) + BB0 (gate open) → advance → wake. The one sub-case
where AF0 declines in that pass leaves arm = 1 charged (BB0 clears arm
only on the advancing path — verified in the shard), so the idle
pump's next retrace completes the cure. The waiter never re-tests
after OSSleepThread, so the wake restarts the chain with certainty.
The attempt-3 storm shape (714 sleeps, all A=1 B=0 arm=0 latch=0) is
the same shape and cures the same way, at delivery speed instead of
parkwatch's ~2.05 s.

**Not cured, stated plainly:** the restore-race subclass (advance
consumed inside the waiter's own restore before the sleep call) —
refuted for the only instrumented instance, never observed in 724
snapshot sleeps, and unreachable by either element (its delivery is
spent before any seam exists). Its cure would be the per-queue wake
latch (PARK-STUDY-2 (d)), deliberately not built; parkwatch remains
the armed net for that shape.

**Arming:** dk_diag key `seamfix` (SCOOBY_SEAMFIX), beside parkwatch;
the [dk] line tail is now `… parkwatch=%d seamfix=%d`. Unarmed =
behaviour identical to 1f173d34 (GX entries fall through to the
general deque; no seam pass). Observability: `[seamfix] post-link
delivery …` and `[seamfix] woke thr=… on q=…` lines beside tracewake's
[wake] lines; parkwatch stays armed as the falsifier net (expected
fires ≈ 0 on a park-storm boot if the seam works).

**Scope note:** ELF arithmetic reads 13,986 functions / counters
−1 / names +42 B vs 1f173d34's 13,987 — a one-small-function delta
(scope string and shards fingerprint unchanged; both new GX
functions present as symbols; the arm getter inlined away). The
proving boot's runtime [pgo] line is the final word. Details and the
disclosure of the first (wrong-variant, non-deko) build attempt are
in SEAMFIX-BUILD-LOG.md.

**Status: BUILT, NOT BOOT-TESTED.** Next: seam proving boot
(seamfix + tracewake + parkwatch, no stallsvc for clean
attribution), then the Stage B burst decider re-fire.

## SEAM-FIX PROVING BOOT (attempt 2, Nov 16, 2026) — S: SEAM-PARTIAL; B: JUNCTION-NOT-REACHED

One launch of seam-fix build c8da98d8 (dk_diag: fifofast / watchdogat=420 / traceio / tracecnt / stallsvc=burst / seamfix / tracewake / parkwatch; claim 20:36:44Z; launch 20:37:58Z; log stderr-seamfix.log, 102,755 lines, sha b3a60484…). Arm verified: [dk] tail "stallsvc=burst tracewake=1 parkwatch=1 seamfix=1". Runtime [pgo] count VERBATIM: 13,990 functions (4369 KiB of counters) — the runtime line settles the scope question (desk ELF arithmetic said 13,986). Clocks 1020/307.2/1331.2 GPU-307-LOTTERY (record only).

The boot ran in two phases. PHASE 1 (guest t 0→457.8 s, frames 0→8,535, ≈18.6 fps): full movie chain (WB/THQ/HI/RW4/Intro), title at ~195 s, menu open and navigable (selection moved NEW GAME -> LOAD GAME, pixel-diff verified), ZERO display-queue sleeps; watchdog 420 s "frames advancing, no dump". PHASE 2 (t 457.8→1,279.5 s, frames 8,535→9,447, ≈1.11 fps): the park storm returned during the attract loop — all 305 display-queue sleeps (A=1 B=0 arm=0 latch=0) live here.

Censuses: [seamfix] 119 post-link deliveries (72 retrace queue 0x802713A0 / 47 display queue 0x802716E4); 18 wakes, ALL on the retrace queue in phase 1; on the display queue 47 passes, gx=0 and woke=0 EVERY time — the held GX class was already empty at every display-queue seam, so the seam never cured a display-queue park. [parkwatch] 305 signatures / 304 fires / 304 frames-resumed / 0 dry / 0 budget lines — parkwatch alone paced phase 2. [wake] 305 sleeps / 8,837 wakeups / 304 result=woke (all parkwatch) / 8,533 wake-to-empty. [stallsvc] 0 lines; h001.HIP 0.

VERDICT S: SEAM-PARTIAL — the success signature (seam wakes on 0x802716E4, parkwatch ≈ 0) did not occur, but the front-end was transformed vs Stage B attempt 3 (storm from frame ~714, menu never reached): 8,535 park-free frames, chain+title+menu in ~3.5 min, and real seam work on the retrace queue. Phase 1's health cannot be attributed to the seam vs a lucky roll from one boot; what is certain is that in the storm the seam contributed nothing on the convicted queue (gx=0 at all 47 seams — the design's worst sub-case recurring systematically). VERDICT B: JUNCTION-NOT-REACHED — burst unrun, neither proven nor falsified; the blocker was the LOAD GAME confirm, which defeated seven attempts across both input paths and hold lengths (click / 700 ms / 1.0 s / 1.5 s) inside healthy menu windows (input-side flakiness, total this boot), plus the ~2 min menu->demo timeouts. VERDICT P: parkwatch carried phase 2 alone, 304/304.

Close by Brandon's hand at ~21:00Z (clean HOME close in the exit tail; "No more console boots rn, we'll pick up later" — console boots PAUSED at his word). Salvage: profraw fw-1794861511 banked (5,636,496 B, sha 28a2d45f…; total now 18; front-end+title/attract coverage, no junction/route); resident 34585267 restored + verified; dk_diag deleted; crash.txt/save at baseline, crash.txt never overwritten; claim deleted + verified absent; relays sent. Full record: SEAMFIX-BOOT-LOG.md attempt 2.

## SEAM-FIX FOLLOW-UP STUDY (Nov 16, 2026; desk only — SEAMFIX-FOLLOWUP-STUDY.md)

Q1 (the empty deque) CONVICTED: the held GX class never received a single entry all boot, because the callback the park turns on (AA4, the engine's frame-swap completion) is queued by No100f_GXEnableBreakPt (gx_fifo.cpp:36, callback read from the BreakPointCB slot 0x802712A4) through QueueGuestCallback into the GENERAL deque — the one site the seam design deliberately left unrerouted — and is spent at the waiter's own restore points (advancer wakeups predominantly inrestore=1). The PE-interrupt class the design held never fired once: zero [io] queue lines for the finish wake 0x801087F4 (queued unconditionally beside every draw-done at gx_fifo:84) against 8,534 AA4 queues, and no token-branch lines either; RaisePeInterruptForBp's interrupt branches are dead in this title's observed flows. Seam-pass ordering and the idle pump are exonerated (pass precedes the pump in the same OSSleepThread call; 119 logged passes, gx=0 everywhere, because the deque was in fact empty). Second layer: after the first park, no completion is generated at all (0 AA4 queues across phase 2's 912 frames; advancer never runs; identically on the unseamed attempt-3 build, whose queues stop at its first sleep, and parkwatch2 — never parked — queued on all 6,259 frames): draw completion in this HLE is synthesized only at guest-triggered sites, and the post-park present cycle triggers none, where hardware's draw-done interrupt is unconditional. FIX SPECCED, NOT BUILT: (1) reroute gx_fifo:36 to QueueGxInterruptCallback when armed (one call site) — the design's own section-3 trace then applies to the generating flow; (2) present-time completion synthesis for the storm state specced but NOT recommended yet (parkwatch, proven 304/304 and 714/714, remains its rescue); (3) a two-counter instrument (GXEnableBreakPt calls, RaisePeInterruptForBp hits by reg, held-deque high-water) specced to close the last guest-side unknown. Q2 (menu confirm): NEVER REGISTERED guest-side — zero card opens across the menu region against parkwatch2's 106 CARDOpens for the identical recipe; the four principal attempts (sys-agent click B, dbg holds 0.7 s and 1.5 s, sys-agent press 1.0 s, later escalations to 3 s holds — eleven-plus attempts, both paths) were verifiably inside menu windows at ~23 fps with the selection diff-proven on LOAD GAME; window-racing into the attract demo explains only the later attempts (menu9's press demonstrably landed on the demo). No seam mechanism that could eat the input was found in source (the pass touches no pad state; element 2 held nothing all boot; burst retrace delivery cannot defeat a 3 s hold), and the known flakiness record does not cover total failure of the certified dbg-hold method while PLUS/DDOWN landed through the same paths. Named open item: a guest pad-state visibility instrument. Next boot's menu work is specced as a single-script recipe (one SSH session, diff-verified selection, dbg hold B 900 ms, one sys-agent fallback, hard stop — no retry loop).

## REROUTE BUILD — Nov 16, 2026 (build c4e334bb)

Desk build only (no console contact). Full record: REROUTE-BUILD-LOG.md
(VM evidence). Base c8da98d8 (seam fix); provenance chain extends
… → 1f173d34 → c8da98d8 → c4e334bb. Backups of all seven touched files,
byte-verified pre-edit, in C:\dev\no100f-nx\reroute-backup\.

THE REROUTE (SEAMFIX-FOLLOWUP-STUDY fix 1 — the only behavioural delta,
and only when seamfix is armed): No100f_GXEnableBreakPt (gx_fifo.cpp:36)
now queues the BreakPointCB callback (the display-sync advancer
func_80175AA4) via QueueGxInterruptCallback (the held GX deque) when
seamfix is armed, instead of QueueGuestCallback (the general deque).
Unarmed the call is identical to c8da98d8. The follow-up study convicted
the seam build's empty deque: the advancer was never in the held PE
class (RaisePeInterruptForBp never fired all boot); it queued here, into
the general deque, and was spent at the waiter's own restore points.
With the reroute, the design's post-link delivery trace finally applies
to the callback that actually exists. Element 1 (seam pass), element 2
(the RaisePeInterruptForBp holds) and every standing instrument are
unchanged.

INSTRUMENT 1 (closes the last Q1 unknown): per-boot counters —
No100f_GXEnableBreakPt calls, RaisePeInterruptForBp hits by register
(0x47/0x45/0x48/other), held-deque high-water mark — printed as a
[gxcnt] companion line by the tracecnt sampler in hle/vi.cpp (the [cnt]
line itself byte-unchanged; [gxcnt] prints when the counters moved and
on the sampler's stall cadence regardless). One storm boot now separates
"the guest stopped calling GXEnableBreakPt" from "the FIFO stopped
reaching the site".

INSTRUMENT 2 (the Q2 separator): dk_diag key padwatch → SCOOBY_PADWATCH;
PAD__Read_HLE logs every change of the composed per-channel pad words
(the exact guest-visible state, after all input sources merge) as
[padwatch] lines (PADRead index + ms stamp, changes only, cap 20,000).
The [dk] line tail is now "… parkwatch=%d seamfix=%d padwatch=%d".

BUILD: build-switch.sh --deko --variant pgo-trim --out none, held
foreground session, 24/24 steps, clean link. NRO 55,888,069 B, sha256
c4e334bbfffc2e822aa343ca7aef38e331e3b32d6f07f68fa1e5845c37778ed0, placed
at switch-logs\h3-test\no100f-nx.nro only (hash-verified); Downloads
staging still 53dbb7bb. Scope: ELF arithmetic 13,996 functions
(c8da98d8: 13,986 desk / 13,990 runtime) — +6 is the new getter
functions (all symbol-verified), +4 the recorded bucketing wobble;
configure scope string unchanged; the proving boot's runtime [pgo]
line is the final word. All three new log strings verified in the NRO.
NOT boot-tested; the proving boot is a separate dispatch. Its dk_diag
is nine lines: fifofast / watchdogat=420 / traceio / tracecnt /
stallsvc=burst / seamfix / tracewake / parkwatch / padwatch, and the
menu work uses the follow-up study's single-script recipe.

## REROUTE PROVING BOOT (Nov 17, 2026, attempt 4 — build c4e334bb, STOCK clocks)

Full record: REROUTE-BOOT-LOG.md ATTEMPT 4; stderr-reroute.log
(1,167,629 B, 13,643 lines, sha256 33f233dd…). One launch,
01:47:08Z, claim 01:45:42Z, clean HOME close by the session at
the watchdog point; 809 frames over 445.5 s guest time.
Runtime [pgo] count verbatim 13,996 (matches desk arithmetic).
[dk] tail verbatim "stallsvc=burst tracewake=1 parkwatch=1
seamfix=1 padwatch=1".

VERDICT S2: REROUTE-PARTIAL — the reroute is proven in the
act, the boot still stormed through the layer-2 door. In the
generating flow (frames 0→359, guest t 0→~20.6 s, ≈17.4 fps):
177 [seamfix] wakes on the display queue 0x802716E4, 179/207
display-queue passes with gx≥1, parkwatch zero fires, breakpt
advancing, gxheld_hw=1 — the follow-up study's predicted
proving signature in full, against the seam build's 0 wakes /
47 passes / gx=0 every time. THE CLIFF, exact: last
GXEnableBreakPt call at [gxcnt] t=20623 ms (breakpt=358,
frames=359); breakpt frozen for the remaining 425 s while
pe47/pe48 tracked all 809 presents 1:1 and pe_other advanced
to 15,304 (pe45=0 throughout; zero 0x801087F4 queue lines —
the element-2 PE class remains unfed in this title). All 28
gx=0 display-queue passes are post-cliff (first at retrace
2092 vs cliff ~1238): in-flow seam record 177/177, no park
uncured while a held AA4 existed. Phase 2 (frames 359→809,
≈1.06 fps) was parkwatch-carried: 151 signatures/fires, 150
resumes (151st overtaken by the close), 0 dry, 0 budget;
signature shape A=0 B=2 arm=1 latch=0 (a different storm
sub-state — the visible symptom was a frozen pure-black
frame, byte-identical captures, while the counter crawled).
[wake] display queue: 329 sleeps, all woken (177 seam + 151
parkwatch + 1 advancer). Logo chain crawled (WBLogo/THQLogo/
HILogo/RW4 opened; Intro never); title never reached.

VERDICT B: JUNCTION-NOT-REACHED (fourth consecutive armed
boot; 0 bursts; the cliff hit mid logo-chain at ~20.6 s,
before the title existed). VERDICT Q2: NOT EXERCISED (menu
never opened; recipe precondition never held; padwatch armed
and alive, 4 baseline lines, nothing to observe). VERDICT P:
parkwatch carried the tail alone, 151/151.

Profraw fw-1794880062 banked (5,637,160 B, sha 835deb85…;
banked total 19; coverage: front-end logo chain only).
Closeout all verified: resident 34585267 restored (read-back
match), dk_diag absent, crash.txt/save at baseline, Config
untouched, claim deleted + verified absent, running=0, HOME.
Open items carried forward: the guest-side micro-reason
GXEnableBreakPt ceases at the cliff frame (counters convict
the call site, not the reason); Fix 2 (present-time
completion synthesis) remains specced-but-unrecommended with
parkwatch as the proven rescue; the burst decider still
needs a boot whose front-end survives to a load press.

## CLIFF STUDY (Nov 17, 2026, desk — SEAMFIX line, post-reroute boot)

Full record: ~/workspace/no100f-h3-evidence/CLIFF-STUDY.md. Why the
guest stops calling GXEnableBreakPt — the layer-2 micro-reason the
follow-up study left open is now CONVICTED. GXEnableBreakPt has
exactly two guest call sites, both downstream of the advancer
func_801759DC: (1) the advancer's own re-arm branch (advance leaves
A != B -> GXEnableBreakPt; A == B -> GXDisableBreakPt +
*(0x802716D0) = 0), and (2) the waiter/presenter func_80176B30's
present path, flag-gated: if (*(0x802716D0) == 0) { =1;
GXEnableBreakPt }. The arming is a relay between the sites (~1
call/frame in steady flow: 358 calls / 359 presents), not
unconditional. The flag word's ONLY clearer is the advancer's
catch-up branch, so the calls continue only while the advancer
runs. Cliff sequence (reroute boot, stock clocks): last advancer
run at guest=1227 ended in the catch-up branch (A==B, flag
cleared, D4 recharged); frame 358's present re-armed (flag=1,
AA4 #358 into the held deque); the unpaced drain burst presented
frame 359 with no advancer run between (AA4 hostage in the held
deque); AF0/BB0 spent the latch charge on the next retrace pass
(arm still 0, BB0's unconditional clear); the waiter slept
(A=0 B=2) and the seam pass delivered AA4 #358 into the
token-match exit — arm=1, no advancer run, woke=0. From there
every producer is dead: no AA4 queued ever again (0 queue lines
after log line 6361 vs 358 before), site 2 blocked by the flag
standing at 1 across all 450 post-cliff presents, BB0 gated on
latch, latch on AF0, AF0 on D4, D4 on the advancer. The FIFO,
present path (pe47/pe48 exactly 1 per present for all 809
presents; pe_other ~19/present throughout), retrace chain
(1,239 -> 25,420), audio chain (ai_cb 4,125 -> 84,736) and Bink
streaming (273 further cb queues) all ran to the end of the
boot. Only the call stopped. At the cliff the guest was late in
THQLogo.bik's drain window; HILogo/RW4 opened during the
parkwatch crawl; Intro never opened.

Q2 (seam cadence): SUPPORTED structurally, refuted as
perturbation/crescendo. Reroute-boot sleep rate is flat
(~0.5/frame; 47/39/39/37 per 200-retrace buckets) from flow
start — no acceleration into the cliff. The reroute makes the
pipeline-full check unsatisfiable pre-link (the pending AA4 is
hostage in the held deque; restore points drain only the
general deque), so the marginal regime is this build's normal
mode and the cliff is a constant-hazard draw (hit at trial
~178). Every eager build slept ~never in-flow (seam build 0 in
8,535 frames; parkwatch2 0 in 6,259 AT STOCK CLOCKS — the clock
null is refuted as the driver; variance explains the landing
frame, not the regime). Decider: one c4e334bb boot with the
seamfix key OFF (arm-selected reroute returns AA4 to the
general deque) — prediction ~0 in-flow sleeps.

Q3 (the A=0 B=2 arm=1 latch=0 sub-state + black frame): SAME
lost-advance defect, stranded half moved. Earlier storms
stranded arm (advance spent pre-sleep); this one strands latch
(BB0's unconditional clear spent it one retrace before the seam
charged arm). The missing advance is BB0's, dead at the D4
link. The seam delivery working as designed moved the strand
one link deeper — progress information, not a new defect. The
28 post-cliff gx=0 seam passes reconcile exactly: arm is guest
state, not a queued item; the held deque was genuinely empty
because its only feeder was dead. The frozen black frame is
the un-flipped display buffer (A frozen at 0) holding
THQLogo's black transition frame; attempt 3's visible crawl
vs this boot's black differ only in which buffer was on
display when the chain died.

Q4 (way to the junction, ranked): (1) SEAM-WAKE — in the seam
pass, after the GX and retrace steps, if the sleeper is still
linked on 0x802716E4 AND (B-A) mod 3 == 2, fire
OS_HLE_WakeupThreadNoReschedule on the queue (parkwatch's
proven actuation moved into the pass; the waiter's post-sleep
present is unconditional, so it cures EITHER stranded
sub-state at delivery speed; falsifier: a fire with no frame
resumption within one [cnt] sample, or any fire off
pipeline-full). It paces the stranded state at ~full speed
(3 presents per sleep cycle) — enough to finish the chain in
seconds and hold a drivable title cadence for the menu
recipe; it does NOT restart generation (flag stays 1,
advancer stays dead) — whether the title regime re-enters
generation is the proving boot's empirical question.
Parkwatch stays as the net. (2) GPU-307 FUNCTION ROLL on the
seam build c8da98d8 — legitimate for the burst decider (a
function boot, nothing priced, so the stock-clock measurement
rule does not bind); the only front-end on record that reached
the menu, its blocker was the Q2 confirm that padwatch + the
one-script recipe now attack; confounds attribution
completely and is lottery-priced. (3) PARKWATCH RE-ROLL on
c4e334bb as-is — this boot is its counterexample (cliff at
frame 359, 425 s of crawl did not finish the chain; attempt
3's title was reached but undrivable). FIX 2 (present-time
completion synthesis): STRENGTHENED by the flag finding (the
guest presents without re-arming because its arming call is
gated on a word only the dead advancer clears — synthesis
stands exactly where the guest's call should have) but NOT
compelled; it becomes the route only if a seam-wake proving
boot shows the load-side flow stranding in a state the wake
cannot pace. HOLD Fix 2.

## SEAM-WAKE BUILD (Nov 17, 2026)

Build 6db2f31c (55,888,069 B, sha256 6db2f31c390ffebcd68040a5dd68e321f9b1042079abb3c8ff2430fa1e96e28f), pgo-trim, base c4e334bb; record: SEAMWAKE-BUILD-LOG.md. CLIFF-STUDY Q4 rank 1 implemented: in the seam pass (os_sleep.cpp), after the GX/alarms/retrace delivery steps, if the sleeper is still linked on the display queue 0x802716E4 (state WAITING, queue word unchanged - so a delivery wake can never be double-fired) AND a fresh sync snapshot reads pipeline-full ((B-A) mod 3 == 2), fire OS_HLE_WakeupThreadNoReschedule on the queue - parkwatch's proven actuation at delivery speed, curing either stranded gate sub-state (arm-charged or latch-charged), since the waiter's post-sleep present is unconditional. Two files touched, additions only (os_sleep.cpp +50 lines, hle_stubs.h +6 for the helper declaration); backups byte-verified at seamwake-backup\. Unarmed = the whole seam pass does not run, behaviour identical to c4e334bb. Log: [seamwake] fire #N with A/B/arm/latch/guest snapshot + result=woke|still-linked, carrying the study's falsifier (any fire off pipeline-full, or any fire with no frame resumption within one [cnt] sample). Scope: __llvm_prf_data 895,744 B = 13,996 functions, identical to c4e334bb; +13 counters. NRO in h3-test only; Downloads staging still 53dbb7bb. Proving boot rides the same nine dk_diag keys as the reroute boot (seam-wake rides the seamfix arm, no new key) and doubles as the junction attempt (burst decider + one-script menu recipe).


## SEAM-WAKE PROVING BOOT (Nov 17, 2026) — SEAMWAKE-PARTIAL; guest abort in the paced Intro regime

Attempt 2 fired (attempt 1 stood down behind CTTR's round-13
claim). One launch of 6db2f31c, claim 2026-11-17T02:24:48Z,
STOCK clocks 1020/384/1331.2, runtime [pgo] 13,996 verbatim.
Full record: SEAMWAKE-BOOT-LOG.md attempt 2;
stderr-seamwake.log (1,916,162 B, 32,709 lines, sha 1174ef6f).

Phase 1 repeated the reroute boot exactly (frames 0-355 at
~17.4 fps; 177 gx=1 display-queue seam passes, 175 seam-delivery
wakes), and the cliff landed at the same place: breakpt froze
at 354 at t=20,369 ms (THQLogo drain) and never moved again.
Phase 2 is the new result: **816 [seamwake] fires, all
result=woke, all on pipeline-full snapshots (A=2 B=1 arm=1
latch=0), zero falsifier trips** — frames 355 -> 2,802 at
~25.0 fps (vs the ~1.06 fps parkwatch crawl on the same tail
in the reroute boot), parkwatch 0 fires (one signature, cured
inside its window by seam-wake fire #85), and the logo chain
completed guest-side for the first time on an armed boot:
HILogo, RW4 and Intro.bik all opened (Intro at t~43.7 s).
[wake] census reconciles: 992 display-queue sleeps, all woken
(816 seam-wake + 175 seam-delivery + 1 advancer/restore).

Limits, measured: generation did NOT restart (breakpt frozen
354 for ~98 s while pe47/pe48 tracked all 2,802 presents) —
the wake paces, it does not revive the advancer; and the
display never flipped post-cliff (A is the advancer's word),
so every capture was the same pure-black frame and the title
was never visibly reached. Verdict B JUNCTION-NOT-REACHED
(fifth armed boot, 0 bursts); Q2 NOT EXERCISED (no visible
menu; padwatch idle).

The boot ended at t=118,155 ms (frames=2,802, ~74 s into
Intro under pacing) in a guest wild-pointer data abort
(guest VA 0xc8a10094 through the flat window; addr2line:
guest func_801850E4 from func_80151760, chain to
RuntimeMain). crash.txt was overwritten by the handler
(2,618 B; preserved at h3-test/crash-seamwake.txt; baseline
restored byte-exact at closeout) and the runtime exited via
userAppExit to a clean HOME with no error dialog. The fault
is in guest code executing a guest pointer, not at a seam or
in HLE machinery; whether the paced regime caused it is
undecided on this boot. Profraw fw-1794882395 banked
(front-end full chain incl. Intro); banked total now 20.
Closeout fully verified (resident restored, dk_diag absent,
save/config at baseline, claim deleted, relays sent).

Line state: seam-wake is a proven pacing carry with measured
limits. The junction blocker has moved — it is now the dead
display chain (no flips without the advancer) and the guest
defect at frames=2,802, not front-end arrival guest-side.

---

## FIX 2 DESIGN STUDY — Nov 17, 2026 (desk, read-only; FIX2-DESIGN-STUDY.md)

Part A — the seam-wake boot's Intro abort, identified. The crash
record (h3-test/crash-seamwake.txt): host data abort esr=0x92000006
(a flat-window LOAD, translation fault) at guest VA 0xc8a10094 on
the main thread. addr2line: faulting instruction in guest
func_801850E4 (memory_access.h:569 = FlatLoad), called from
func_80151760. Shard-level read: func_801850E4 is a handler
DISPATCHER — it publishes (handle, object) into a fixed global
current-op slot at 0x80270228 (busy flag at +8), clears the
object's state field (+16), and invokes a method pointer loaded
through two dereferences: desc := *(handle+8); inner := *desc;
fn := *(inner+4). The faulting load is the last one (host x9 =
0xC8A10090, x10 = 0xC8A10094 = base+4): the descriptor's first
word held the wild value 0xC8A10090 — outside every guest memory
window (RAM 0x80000000–0x81800000, mirror 0xC0000000–0xC1800000),
not an MMIO page. func_80151760 is its resolve-or-create caller
(object+144 halfword fast path; object+136 stored handle; else a
global registry at *(0x8027167C) indexed by *(0x80271758)); the
chain runs RuntimeMain → func_8000A0C8 → func_80151E90 →
func_8015406C (a first-claimant table walk) → func_80151210
(get-or-create) → func_80151760 → func_801850E4, synchronously on
the main thread. NOT Bink and NOT a streaming/IO callback: no
address in 0x8015xxxx/0x8017Bxxx/0x8018xxxx appears in any of the
boot's 10,448 [io] queue lines (the .bik streaming runs through
DVD cb 0x80117F34 and streaming cb 0x801126A4). It is a front-end
handler-dispatch manager whose data sits in the display module's
own .sdata block. A2 VERDICT: REGIME-CAUSED — the paced-no-flip
state (2,447 presents, A frozen, zero retirements, breakpt
frozen at 354) is the only condition separating the aborting
boot from four-plus generation-alive Intro traversals with no
abort, and from attempt 3's generation-dead but parkwatch-paced
(~1.12 fps) full traversal with no abort; the seam-wake boot is
the only one that combined generation-dead with full-rate
(25 fps) pacing. Seam-wake's actuation itself is exonerated
(bare wake, no guest code, 816/816 clean). Deciding measurement
named: a descriptor-watch instrument (per-[cnt]-tick walk of
the fixed slot 0x80270228, logging the first tick the inner
pointer leaves the RAM window) plus replication. A3: the abort
bounds seam-wake as a carry (~2,447 paced presents, n=1); Fix 2
removes the cause on this verdict (it abolishes the dead-display
state rather than outrunning it).

Part B — Fix 2 designed (NOT built). Synthesis point: the
runtime's VI frame-present point, where the presented-frames
counter advances — measured 1:1 with guest presents (frames ==
pe47 == pe48 at every [gxcnt] sample on record). Payload: queue
the registered frame-swap callback (BreakPointCB slot
0x802712A4, fallback DrawDoneCB 0x802712B4) with the genuine
(r3=0, r4=0) signature into the HELD GX deque via
QueueGxInterruptCallback — never the general deque (eager
deliveries are spent at restore points; that is the park's own
mechanism). No token state is carried; AA4 sorts its exit from
live memory. Relay trace closes: the retrace half is verified
LIVE post-cliff from the seam-wake log (guest retrace word
1,223 → 7,086, guest_ok=1 throughout; AF0/BB0 run every retrace
and gate out — only the D4/latch charge is spent). The
match-forever trap is answered at shard level: the token word
freezes post-cliff but the presenter's buffer word F8
(0x802716F8) rotates on EVERY present (variant-1 tail: F8 :=
*(0x80271700), or *(0x802716FC) if already equal), and each
present's critical section writes table[B].word0 := F8 before
rotating — so table[A].word0 alternates against the frozen
token and synthesized AA4s reach the mismatch (advancer) exit
within a few presents; synthesis retries per present until one
lands. Then: advancer runs (breakpt counter increments — site
1), A advances, D4 recharges, AF0 flips and sets latch, BB0's
advancer run reaches the catch-up branch and clears the flag
0x802716D0, and the presenter's site-2 GXEnableBreakPt call —
the genuine one — resumes generation; the detector stands
down. Two open links named, both latency-grade and observable:
the state-flag-dependent D4 recharge rate, and func_8017BA70's
return inside AF0. Contingency if match-forever is observed
(falsifier F1): synthesizing the advancer invocation itself —
deliberately NOT this design (it impersonates guest logic
rather than delivering its registered event). Gating: NEW
dk_diag key `fix2` (the seamfix-OFF control A/B stays clean);
a five-condition dead-detector (armed; generation lived at
least once this boot; a callback is registered; the breakpt
counter unchanged across K=3 consecutive presents; presents
advancing — which keeps fix2 disjoint from the junction wedge
by construction); a per-present completion flag set at both
gx_fifo queue sites and cleared only at the present hook, so
at most one completion exists per present and the only failure
direction is a missed synthesis, never a double delivery. The
reroute and seam pass are unchanged; seam-wake is kept,
subordinated (its fires should fall to ~0 after engagement);
parkwatch stays the outer net. The hook executes no guest code
and writes no guest memory — the anti-v1 safety shape.
Proving signature: a VISIBLE, flipping, drivable title
(captures differ, luma > 0, PRESS START reached); syntheses
that begin at the cliff and STOP when [gxcnt] breakpt resumes;
A advancing in [wake] snapshots. Falsifiers: F1 match-forever
(≥10 deliveries, breakpt + A still frozen), F2 detector
false-positive, F3 abort recurrence in the dispatch chain
(flips A2), F4 double-advance, F5 synthesis while frames are
frozen. Full text: no100f-h3-evidence/FIX2-DESIGN-STUDY.md.

## FIX 2 BUILD (Nov 17, 2026)

Build af677c10 (55,892,165 B, sha256 af677c100c6ec1951dbb8e414b3ac5bac49169b74d4d683562fc266acf786c6d), pgo-trim, base 6db2f31c (seam-wake); FIX2-DESIGN-STUDY Part B implemented as specced. Backups byte-verified pre-edit at fix2-backup/ (6 files); semantic diff additions-only apart from the draw-done brace expansion and the [dk] tail field; the Fix 2 hook is the only behavioural delta vs 6db2f31c. NRO in switch-logs/h3-test only; Downloads staging untouched (53dbb7bb). Scope: 13,999 functions by ELF arithmetic (+3 vs base); [fix2] strings and SCOOBY_FIX2 verified in the NRO.

The change: a present hook in VI_HLE_PresentFrame (hle/vi.cpp), called immediately after the presented-frames counter advances. Armed by the new dk_diag key fix2 (SCOOBY_FIX2, OS_HLE_Fix2Armed in os_scheduler.cpp; [dk] tail gains fix2=%d), default off - unarmed behaviour identical to 6db2f31c. Dead-detector at each present: (b) breakpt counter ([gxcnt], gx_fifo.cpp) nonzero at least once this boot (monotonic, so a nonzero sample is the proof; boot-phase presents excluded by construction); (d) breakpt unchanged across K=3 consecutive presents counting the current one (run-length state, resets whenever breakpt moves); (c) BreakPointCB slot 0x802712A4 nonzero, else DrawDoneCB slot 0x802712B4, else no synthesis; (e) presents advancing by construction (the hook is the present; the junction wedge never reaches it - disjoint from stallsvc). Synthesis: QueueGxInterruptCallback(cb, 0, 0) into the held GX deque, at most one per present, repeating on later presents while the detector holds (retry-until-mismatch). Per-present completion flag (relaxed atomic in gx_fifo.cpp): set at No100f_GXEnableBreakPt when the BreakPointCB queues (either deque) and at the RaisePeInterruptForBp draw-done branch when the DrawDoneCB queues; read + cleared only at the hook, once per present, as the decision input - set suppresses synthesis, synthesis sets it; double delivery is structurally impossible, the only failure direction is a missed synthesis. The hook executes no guest code and writes no guest memory; its only lock is the held deque mutex. Log forms: [fix2] engaged / [fix2] synth #N (cb, frozen breakpt, presents, plus the B2 snapshot A / token 0x802716F4 / table[A].word0 at 0x80270150 + A*8, read-only at queue time) / [fix2] revived (episode synth count). Seam-wake kept, subordinated; element 1, element 2, the reroute, parkwatch, stallsvc untouched.

Proving boot: TEN dk_diag keys (the seam-wake nine + fix2); [dk] tail ends "stallsvc=burst tracewake=1 parkwatch=1 seamfix=1 padwatch=1 fix2=1". Success signature and falsifiers F1-F5 per FIX2-DESIGN-STUDY B4 (headline: visible flipping title at a drivable cadence; syntheses that begin at the cliff and stop when [gxcnt] breakpt resumes). Full record: FIX2-BUILD-LOG.md in no100f-h3-evidence. Not boot-tested; the proving boot waits for the console queue.

## CONTROL BOOT (Nov 17, 2026) — c4e334bb with seamfix OFF: PREDICTION-CONFIRMED

One launch of the reroute build c4e334bb with the seamfix key OFF
(dk_diag exactly eight keys; the [dk] line reads seamfix=0), the
deciding measurement CLIFF-STUDY Q2 named for the delivery-timing
explanation. Disclosure: both staging copies named in the brief
(WSL build dir, h3-test) had been overwritten by the seam-wake
build (6db2f31c); c4e334bb was reconstructed from the byte-verified
seamwake-backup sources (the only 2 files the seam-wake change
touched) and the rebuild reproduced sha256 c4e334bb... EXACTLY —
build determinism proven in both directions (restoring the
seam-wake files reproduced 6db2f31c... exactly); tree and build
dir left in the exact found state. Stock clocks 1020/384/1331.2;
runtime [pgo] 13,996 verbatim. RESULT: ZERO display-queue sleeps
across 4,833 frames / 234.6 s guest (armed boots: 177 in-flow
sleeps, cliff at ~frame 359), 4,832 display-queue wakeups 99.1%
inrestore=1, breakpt 0 -> 4,833 never frozen, gxheld_hw=0
throughout, parkwatch 0 signatures / 0 fires, full logo chain
(WBLogo/THQLogo/HILogo/RW4/Intro) + title/attract reached
eagerly, clean HOME close, no abort. The reroute's held deque —
not clocks, not cadence perturbation — is what makes the
pipeline-full check unsatisfiable pre-link; with eager delivery
restored the front-end flows like every eager build on record.
Profraw fw-1794884025 banked (total 21; full chain + title
coverage). Full record: CONTROL-BOOT-LOG.md in the evidence dir.

## FIX 2 PROVING BOOT (Nov 17, 2026) — VERDICT F2: FALSIFIER F1 (match-forever); B: JUNCTION-NOT-REACHED; Q2: NOT EXERCISED

One launch of af677c10 (claim 2026-11-17T03:01:26Z, STOCK clocks
1020/384/1331.2, ten-key dk_diag read back verbatim; runtime [pgo]
13999 functions verbatim; [dk] tail "... seamfix=1 padwatch=1
fix2=1"). Full record: FIX2-BOOT-LOG.md; log stderr-fix2.log
(2,239,732 B, 36,899 lines, sha256 b7bd386f...).

The detector engaged correctly on a genuine cliff — drawn EARLY
this boot: breakpt froze at 134 at present ~136 (guest t ~10.1 s,
WBLogo end-of-movie drain; 67 in-flow display-queue sleeps before
it, the armed marginal regime). [fix2] engaged t=10150ms, then
1,334 syntheses over the remaining 2,666 presents — and EVERY one
took the token-match exit: token=0x00000000 and tableA=0x00000000
on 1,334/1,334 synth lines (100% match, 0 mismatch). Breakpt
stayed 134 and A stayed 1 to the final sample; zero [fix2]
revived lines. F1 (>=10 deliveries with breakpt + A frozen) is
tripped decisively. The B2 alternation premise failed in the real
state: with the token word and table[A].word0 both 0, equality is
absorbing — the presenter's per-present table[B] writes never made
table[A] differ. No F2/F4/F5 trips (the freeze was genuine; A
never advanced; all syntheses at advancing presents).

With revival dead, seam-wake carried the boot exactly as in the
seam-wake proving boot: 889 fires, all result=woke; 955
display-queue sleeps all woken (65 seam-delivery + 889 seam-wake
+ 1 advancer/restore); the logo chain completed guest-side
(HILogo frames~405, RW4 ~706, Intro ~868) at ~22 fps pacing; the
display never flipped post-cliff (all in-game captures the frozen
black frame). Parkwatch: 1 signature (frames=573), 0 fires.

The end repeated the seam-wake boot exactly: at frames=2,802
(guest t ~129.0 s; 1,934 presents into Intro — the Intro-end
transition; seam-wake boot: 2,802 / 1,938) the guest aborted,
data abort esr=0x92000006, guest address 0xc8a10094, x9=0xc8a10090
— the identical wild-pointer fingerprint in the func_801850E4 /
func_80151760 dispatch chain. crash.txt was overwritten (2,618 B,
sha 25690ed9...; preserved at h3-test/crash-fix2.txt, baseline
restored byte-exact). Recorded plainly on F3: the abort recurred
in the same chain while synthesis was active, but generation
never restarted (F1), so the dead-display regime Part A convicted
persisted throughout — the recurrence is consistent with the
regime verdict; F3's premise (a revived regime) never held.

B: JUNCTION-NOT-REACHED (sixth consecutive armed boot, 0 bursts,
0 [stallsvc] lines; CARDOpen stayed at the 22 boot-phase probes).
Q2: NOT EXERCISED (title never visibly reached; padwatch armed,
4 baseline lines only). Profraw fw-1794884550 banked (5,637,744 B,
sha a792d1a1...; card/pgo/h3-test identical) — banked total now
22; coverage front-end full chain incl. Intro (guest-side).
Closeout fully verified (resident 34585267 restored + read-back
match, dk_diag absent, save/Config at baseline, claim deleted +
verified absent, relays sent).

STATE OF THE LINE: the Fix 2 payload as designed cannot revive
this state — AA4 delivered with token == table[A].word0 == 0
always takes the arm-only exit. The named contingency from the
design study (B2) is the advancer-invocation payload; that is a
desk study, not a build, until it is specced. The seamfix-OFF
control remains the only configuration that has ever shown a
visible title on this line.


## ADVANCER-INVOKE DESIGN (Nov 17, 2026, desk study; full text: no100f-h3-evidence/ADVANCER-INVOKE-DESIGN.md)

Commissioned by the Fix 2 proving boot's verdict F1 (1,334 synthesized AA4s, zero revived, token=0 and tableA=0 on every synthesis). Read-only study: token mechanics from the shards (func_801759DC / func_80175AA4 / func_80175AF0 / func_80175BB0 / presenter func_80176B30 / dispatcher func_80175FC4 ops) plus the fix2, seam-wake and control logs; payload design against the runtime source (hle/storage/dvd.cpp, hle/gx/gx_fifo.cpp, hle/vi.cpp, hle/os/os_sleep.cpp). Nothing built.

### Part A: why zero

- Writers (whole-tree FlatWrite32 census): token (0x802716F4) has exactly 3 write sites: AF0's rotation and the display setup op (loc_80176204, two copies). table[B].word0 is written only by the presenter's critical section (:= F8). F8 is written by the setup op and the presenter tail rotation. The rotation pool P1 = *(0x80271700), P0 = *(0x802716FC) is written only by the setup op and the teardown op (func_801762A8).
- Rotation rule (AF0 for token, presenter tail for F8): new := P1; if current == P1 then new := P0. Distinct pool values give strict alternation; P0 == P1 pins the value forever.
- The pool words are framebuffer addresses installed by the setup op (fb0 aligned base at 0x80271704, fb1 at 0x80271700, a third address at 0x802716FC; token := fb1's address, F8 := the third address). The TEARDOWN op zeroes 0x802716FC, 0x80271700, 0x80271704 and the raw base word 0x802716BC, and nothing else in the relay.
- Healthy flow (control boot census): 4,833 AA4 queues, 4,832 advancer wakes, 4,787 arm=0 (AA4-mismatch-driven) vs 45 arm=1/latch=1 (BB0-driven): mismatch is the norm (~99%), match the exception. FIX2-DESIGN B2's alternation premise was mechanically right.
- Stranded flow: the teardown runs in the end-of-movie window before the fatal drain sleep (proven by token/tableA already reading 0 at the first synthesis, three presents after breakpt froze: both rotations had already collapsed through the cliff's final flip/present cycle). With the pool at {0,0}, F8 := 0 at the next presenter rotation, token := 0 at the next AF0 rotation, the table fills with 0, and AA4's comparison is 0 == 0 permanently. The setup op that would repopulate the pool never re-runs while the advancer is dead (2,666 presents, four more movie opens, all zeros): the boundary sequence containing it is gated on the drain the advancer completes.
- The zero state is a property of the teardown window, not of the logo regime: strandings are on record in the logos, the title drain and the attract loop, all in drain/teardown windows; the control boot crossed all five logo drains with zero display-queue sleeps, so its zero windows never coincided with a dead advancer. The hazard window and the zero window are the same window, so every realistic stranding in this title is a zero-pool stranding; AA4 synthesis remains the right cure only for the populated-pool class this front-end never produces.
- Intro-end abort (A4): both paced boots aborted at frames=2,802 (1,938 / 1,934 presents into Intro) despite different cliffs, pacing and wall times: the abort tracks the movie timeline. Model: the first post-movie consumer of state the dead regime left stale; the transition's resolve-or-create / dispatch walk (func_8015406C -> func_80151210 -> func_80151760 -> func_801850E4, current-op slot 0x80270228) is synchronous CPU work unexercised during playback. Revival before Intro end plausibly avoids it (five generation-alive traversals of the same transition, no abort); the descriptor-watch remains the decider; F3 stands.

### Part B: the advancer-invocation payload (design only)

- B1 vehicle: the existing held GX deque via the existing QueueGxInterruptCallback; the only change is inside Fix2PresentHook (hle/vi.cpp), which queues guest address 0x801759DC with (r3=0, r4=0) in an escalated episode. No new queue path, deque or delivery point. dvd.cpp verified: no callback allowlist; delivery checks only TranslatedFunctionRegistry::FindByAddressPtr (0x801759DC passes; it is in the translated scope) and delivers FIFO until empty on a context copy with gpr[3]/gpr[4] taken from the entry, so the invoked advancer's own queued AA4 is delivered later in the same pass and match-exits harmlessly. Register contract: the advancer reads no argument registers (its prologue overwrites r3 with the mod-3 magic constant and r4 with A; genuine call sites pass ignored values: AA4 passes table[A].word0 / A*8, BB0 passes the latch value); r13/r1/lr come from the delivery context as for every callback. No100f_GXEnableBreakPt discards its token argument ((void)breakPt), so the stale table[newA].word1 the invoked advancer passes cannot leak into any delivery. Detector condition (c) stays as the episode admission gate; the advancer address is a title constant like the HLE's own hardcoded slot addresses (0x802712A4, 0x802712B4, 0x801087F4).
- B2 escalation machine (hook statics; detector unchanged): every episode starts SYNTH_AA4 (current behaviour). At episodeSynths == N=10 with breakpt frozen (F1's own registered trip line; the F1 boot ran to 1,334), the payload switches one-way to SYNTH_ADV: queue (0x801759DC, 0, 0) at each synthesis point, GxFifo_HLE_NotePresentCompletion as today, distinct "[fix2] adv #n" log lines carrying A/B/arm/latch, one "[fix2] escalated" line at the edge. Revival edge unchanged (first breakpt movement ends the episode; counters zero; the next episode starts in SYNTH_AA4). Cap: M=30 advancer queues with breakpt frozen AND A unchanged trips F-ADV-DEAD: log once, suppress all synthesis for the rest of the episode, degrade to the seam-wake shape. If A has moved but breakpt has not revived, keep invoking (host-paced advancer) and read the sub-verdict from the snapshots. No new dk_diag key; the [dk] line is unchanged.
- B3 safety and trace: the hook's shape is unchanged (no guest code executed, no guest memory written; counter loads, read-only guest reads, the host completion flag, one mutex-guarded deque push). The payload executes only at the runtime's existing guest-callback delivery points (the OSSleepThread seam pass after the waiter links, and the scheduler idle pump), the discipline the v1 hazard violated and the seam architecture established. Cold-call trace from the stranded state (state-generic: all three observed stranded states are pipeline-full, (B-A) mod 3 == 2, which forces the enable branch): delivery -> prologue discards r3/r4 -> newA != B -> GXEnableBreakPt (breakpt 134 -> 135; AA4 queued) -> A advances -> OSWakeupThread wakes the linked waiter -> D4 recharge (immediate if *(0x802716B8) == 0, else on the toggle word's alternate run) -> the same-pass AA4 match-exits -> the waiter presents unconditionally -> AF0 with D4=1 runs its flip body (the token rotation is a value no-op at pool {0,0}; the VI calls execute; latch := 1) -> BB0 calls the advancer genuinely -> the interleave reaches a catch-up run (newA == B: GXDisableBreakPt, flag := 0) -> the next present's site 2 fires genuinely. Relay closed end-to-end. Two named uncertainties: func_8017BA70's return inside AF0 (pre-named in FIX2-DESIGN B2) and the B8/toggle recharge latency (at most one extra advancer run).
- B4 interactions: reroute keep, untouched; seam pass and idle pump keep, untouched; seam-wake keep (expected to fall silent once SYNTH_ADV begins, since the advancer wakes the waiter itself; continued fires at the old rate would mean the invocations are not landing as wakes); parkwatch keep (outer net, silent in this regime); stall service disjoint; the AA4 payload keeps as every episode's first line.
- Proving signature: synth #1..#10 zero-shaped, the escalated line, adv #1, breakpt moving at the next [gxcnt] sample, revived with advs counted, A advancing in [wake] snapshots, seam-wake fires to ~0, captures changing frame to frame, PRESS START visible and drivable, Intro end crossed with no abort.
- Falsifiers: F-ADV-DEAD (30 invocations with breakpt and A frozen: the invocation is not equivalent to the mismatch exit; audit the delivery context next); F-ADV-AF0 (A advances but latch never sets and captures stay frozen: a second, downstream defect in the retrace half); F-ADV-DBL (A advancing by 2 within one present interval from an invoked plus a genuine advancer; bounded and hardware-shaped, watch the [wake] arithmetic); F-ADV-ALIVE (any synthesis while breakpt moved within the K=3 window: detector defect, a regression check); F3 and F5 carried from Fix 2.
- Build sketch: one function touched (Fix2PresentHook in hle/vi.cpp): statics episodeAdvs / advancing / advDead, the N=10 escalation edge, the payload switch, the adv and extended revived log lines, the M-cap stand-down. No other files; no new key.

## FIX 2 ESCALATION BUILD (Nov 17, 2026) — build b5173503 (ADVANCER-INVOKE-DESIGN Part B implemented)

- Result: build-switch.sh --deko --variant pgo-trim --out none from the Windows tree in WSL, held foreground, BUILD_EXIT=0, clean link (only the vi.cpp unity bucket recompiled). NRO 55,896,261 B, sha256 b5173503f8f94895f4c2a3e1a1e8c9cfd6b3e590fdfd7ffe98d0d9e58468e622 (+4,096 B vs base af677c10, one alignment step), placed hash-verified at switch-logs/h3-test only; Downloads staging still 53dbb7bb. Provenance: ... -> 6db2f31c -> af677c10 -> b5173503. af677c10 stays reproducible: its tree state is the fix2esc-backup copy of vi.cpp over the fix2-backup chain. NOT boot-tested; the proving boot is a separate dispatch.
- The change (ONE file): wiicompiled/runtime/src/hle/vi.cpp (122,619 -> 127,452 B), all inside Fix2PresentHook plus its header comment. Backup byte-verified pre-edit at C:\dev\no100f-nx\fix2esc-backup (tree-relative path). Semantic diff reviewed in full: additions only, except the revived format string gaining the appended advs=%u field. The detector (five conditions, K=3, completion flag), condition (c) as the episode admission gate for both payloads (design B1, no deviation), the reroute, seam pass / idle pump, seam-wake, parkwatch and the stall service are all untouched. No new dk_diag key; the [dk] line is unchanged.
- Escalation machine as built (design B2): new hook statics episodeAdvs / advancing / advDead / advBaseA / advAMoved. Every episode starts SYNTH_AA4 (af677c10 behaviour exactly). At a synthesis point with episodeSynths >= 10 (N=10) and breakpt frozen, the edge logs "[fix2] escalated", samples advBaseA from that point's snapshot, and the same point queues the first advancer payload — one-way for the rest of the episode. SYNTH_ADV queues (0x801759DC, 0, 0) via the existing QueueGxInterruptCallback into the held deque and calls GxFifo_HLE_NotePresentCompletion exactly as the AA4 path does; "[fix2] adv #n" lines (numbered per episode) carry the A/B/arm/latch snapshot. "A unchanged" is defined over the advancer phase exactly: every queue-time A sample must equal the escalation-edge sample; any differing sample latches A-moved for the episode, even if a later sample returns to the reference value. Cap: at episodeAdvs >= 30 (M=30) with A unmoved, F-ADV-DEAD trips — "[fix2] adv-dead" logged once (the advDead flag gates every later synthesis point of the episode) and ALL synthesis is suppressed for the rest of the episode, degrading the boot to the seam-wake shape. If A moved but breakpt has not revived, the cap does not trip: invocation continues while the detector holds (host-paced advancer) and the sub-verdict is read from the snapshots. Release edge: "[fix2] revived ... synths=%u advs=%u" (old fields kept, advs appended), and episodeAdvs / advancing / advDead / advAMoved all reset; the next episode re-earns escalation from SYNTH_AA4. Counting note: synthTotal / the synth #n numbering count AA4 queues only, exactly as in af677c10, so the two payload classes stay separately countable.
- Log forms (all six verified present in the NRO by strings): engaged and synth as af677c10; escalated "t=%lldms breakpt=%u presents=%u synths=%u A=%u B=%u arm=%u latch=%u"; adv "#%u t=%lldms breakpt=%u presents=%u A=%u B=%u arm=%u latch=%u"; adv-dead "t=%lldms breakpt=%u presents=%u advs=%u A=%u"; revived gains advs=%u. The advancer constant is verified in the ELF: exactly one movz/movk materialisation of 0x801759DC in w0 (the callback-argument register) in the whole binary, and the new episodeAdvs static is present as a symbol.
- Scope: __llvm_prf_data 895,936 B = 13,999 functions, identical to base af677c10 (the escalation adds no functions, only branches inside the existing hook); __llvm_prf_cnts +240 B = +30 counters; names unchanged; scope string unchanged. The proving boot's runtime [pgo] line remains the final word.
- Behaviour guarantees: unarmed (no fix2 key) the hook returns on the arm check, identical to af677c10; armed but never escalated, identical to Fix 2 as built.
- Proving boot (separate dispatch): the same ten dk_diag keys as the Fix 2 proving boot; the [dk] tail is unchanged ("stallsvc=burst tracewake=1 parkwatch=1 seamfix=1 padwatch=1 fix2=1"). Expected signature and falsifiers (F-ADV-DEAD / F-ADV-AF0 / F-ADV-DBL / F-ADV-ALIVE, plus carried F3/F5) per ADVANCER-INVOKE-DESIGN B4 and FIX2ESC-BUILD-LOG.md. Record: FIX2ESC-BUILD-LOG.md in no100f-h3-evidence.

## EAGER JUNCTION BOOT (Nov 17, 2026)

One launch of af677c10 with seamfix AND fix2 OFF (eager delivery), stallsvc=burst armed, eight dk_diag keys exactly. [dk] tail "...stallsvc=burst tracewake=1 parkwatch=1 seamfix=0 padwatch=1 fix2=0"; [pgo] 13,999 functions; clocks STOCK.

- **Verdict B: CRASH-FALSIFIER.** The menu recipe reached the load (Q2 below); the card scan completed (ScoobyDoo00 -> 0, 01-07 -> -4) and h001.HIP was NEVER issued (0 occurrences after the load mark). Feeder silence >=2 s with work due opened the stall-service gate; burst 1 delivered 3 rounds with NO wake (frames frozen at 2,603, no enqueue witness, presents never resumed), and the process died inside the burst window: host data abort on the guest scheduler thread, esr=0x92000005, guest VA 0x389dfff4 through the flat window (wild guest pointer; preserved at h3-test/crash-eagerjunction.txt). "burst 1 complete: 3 round(s), stopped: process exiting". No relaunch (abort rule). Bursts 2/3 never happened.
- **Verdict Q2: LOAD LANDED.** Title visible; menu opened by the PLUS opener; selection proven by glyph-row brightness. D-pad DOWN (dbg press, sys-agent click, dbg hold) was guest-visible (padwatch btn=0x0004) but did NOT move the selection; left stick -Y (stick=0,-127) moved NEW GAME -> LOAD GAME. All three B confirms landed first try (padwatch btn=0x0100 x3): slot screen -> file list ("11/14/26 10% The Mystery Machine" on top, capture-verified) -> load press.
- **Verdict P: PARK STORM, parkwatch-carried.** breakpt froze at 409 by guest t=30 s (logo chain; the cliff's hazard draw hit early). 731 parkwatch signatures / 731 fires / 731 resumes, 0 dry; the entire session ran at ~1 fps cure cadence. The menu was driven at that cadence because the stills-based cadence check cannot distinguish ~1 fps from full rate — future boots must check cadence by frame counter, not captures.
- Profraw fw-1794885448 banked (pgo + h3-test, hash-verified); coverage front-end/title/menu/junction, NOT route. Banked total 23. Route not run. Full record: EAGER-JUNCTION-BOOT-LOG.md in the evidence folder; stderr-eagerjunction.log (4,610,837 B, sha 877ac518...).
- Closeout: resident 34585267 restored (sha-verified), dk_diag deleted, crash.txt overwrite recorded + preserved + baseline restored, save unchanged, Config untouched, claim released.

## BURST FORENSICS (Nov 17, 2026 — desk study; BURST-FORENSICS-STUDY.md)

The eager junction boot's death inside burst 1, resolved. F1 THE FAULT:
addr2line over crash-eagerjunction.txt (pc/lr exact) — pc = func_80104724
(OSAllocFromHeap's worker; EPISODE-FORENSICS' alloc-path identification),
a FlatStore (memory_access.h:576); lr = func_80015F10 (the OS heap-wrapper
cluster beside the OSFree wrapper 0x80015F74); walk-order callers
func_800D85EC <- func_800CFD14; delivery spine func_80016958 (VI
retrace-callback module; x21=x27=0x80016944 is a return address in the
same module) via InvokeIndirectCpu <- RuntimeMain, on the guest scheduler
thread. The store wrote the next-link field (+4) of a free-list cell at
guest 0x389dfff0 (x24 = cell, x22 = cell+4, x9 = cell+0x20, the 32-byte
header size). 0x389dfff4 is THE SAME wild value that killed the v2 rerun
(EPISODE-FORENSICS: OSFreeToHeap's unlink store through a link field of
0x389dfff4) — two boots, two delivery regimes, two heap operations, one
wild cell: a deterministic corruption signature, not garbage, and not a
pointer into any guest mapping. Subsystem: the guest OS heap manager; no
HLE bookkeeping structure implicated (alarm queue empty, DVD/CARD queue
empty at the freeze).

F2 CAUSALITY: a burst delivery ran the guest into the wild pointer. The
fatal chain executed during round 3's delivery set (rounds 1-2 done,
round-3 done line printed, userAppExit next, the round-4 start check
reported "process exiting"); the delivery spine convicts the RETRACE
delivery of round 3 (the audio poll in the same round has no supporting
frame and cannot be excluded — a round has no per-delivery markers). The
guest was NOT observably dying before burst 1: it ran the front-end, the
menu (LOAD LANDED) and the full card scan cleanly with guest_ok=1
throughout, and the wedge alone never killed a guest in any prior
observation (trains 1-3, trim, trace v2, tracecnt). NOT timestamped by
any instrument: WHEN the cell at 0x389dfff0 entered the heap lists —
best-supported candidate is rounds 1-2's own deliveries (the v2
mechanism, same value), with this boot's 731-parkwatch history and the
transition's own code as the unexcluded alternatives. The deciding
instrument is the heap canary in F4.

F3 THE NO-WAKE: per-round values are not logged (rounds log only
"round N done"); the bracketing [cnt] samples give Δframes 0,
Δenqueues 0 (proven by the burst's own round-start stop checks passing
against the gate-open base — the enqueue witness never fired),
Δbreakpt 0, Δretrace +6 (= exactly 3 rounds x 2 deferred retrace
deliveries, guest word mirrored), Δai_cb +32 across the 3 audio polls.
The deliveries LANDED AND EXECUTED — callbacks ran, counters moved by
exactly the delivered amounts — and the wait was never approached (no
enqueue, no present, no h001.HIP). Two claims kept separate: BURST-NO-WAKE
as a verdict on the full v3 design is NOT established (the dose was 3 of
18 rounds, bursts 2-3 never fired; the boot's CRASH-FALSIFIER verdict
stands), and the rounds that landed show the v2 lesson in v3's bounded
form — counters move, the guest dies in the heap, faster (round 3 vs ~5
firings) and via the alloc path instead of the free path. Feeder coverage
is NOT the failure: the burst covers both counter-study classes (A:
retrace deliveries running the pre/post callbacks; B: the audio poll's
AI callback chain), and the dropped alarm feeder was empty at this
junction in every observation. The failure is exposure: guest code
executed on the service thread against a wedged state, with stop checks
that sample only between rounds. Instrument caveat recorded: the gate's
2000 ms silence durations (its own 50 ms samples) and the [cnt] sampler's
final pre-gate increments (+2..+10 per ~255 ms) reconcile only as
catch-up bursts around the last parkwatch-resumed presents followed by
true silence; future entry lines should log feeder VALUES at gate-open.

F4 THE SAFE JUNCTION INSTRUMENT (specced): the "JUNCTION-COUNT boot
with heap canary". Payload = v3 Stage A stallsvc=count, ALREADY BUILT —
one VI_HLE_AdvanceRetraceCountOnly() per 50 ms tick under the unchanged
gate; it replicates AdvanceRetrace's locked section and delivers nothing:
no wake, no callbacks, NO GUEST CODE — it cannot run the guest into a
wild pointer, and it decides the A-feeder question (wake + witness lines
+ h001.HIP = the wait reads the raw count word and Stage A is the cure;
600 firings with witnesses silent = the wait is callback-maintained).
This was the v3 plan's Stage-A-first order; the campaign skipped to
burst. Rejected: a per-delivery-checked 1-2-round burst (keeps the
hazard class; a check between deliveries cannot retract one already
executing). Folded in: the passive-only boot, as carrier for the ONE
build item — a heap-list canary (new dk_diag key, spec in the study):
a 250 ms read-only sampler that resolves __OSCurrHeap, walks the
allocated/free lists up to 64 cells via TryRead32 validating every link
against the RAM window (a wild link terminates the walk), logs ONLY the
first out-of-window transition with the feeder values at that instant,
plus a clean-walk heartbeat while frames are frozen. It timestamps the
wild cell's first appearance against gate/deliveries and decides F2's
open question on the next junction visit in any mode. Falsifiers:
F-C1 any crash in a count episode (junction reclassified self-dying);
F-C2 canary logs the wild link before the gate opens (junction arrives
corrupt); F-C3 600 silent firings (callback-maintained wait; next
design is retrace-callback delivery at scheduler-safe points); F-C4
ai_cb moves with no enqueue witness (the A/B model is incomplete).
STANDING RULE: no further burst boots until the canary has timestamped
the wild cell at least once.

## FIX 2 ESCALATION PROVING BOOT (Nov 17, 2026) — ESCALATION-PARTIAL, F-ADV-AF0 in substance

One launch of b5173503, nine dk_diag keys (no stallsvc; junction observe-only after the eager boot's burst crash-falsifier), GPU-307 lottery clocks (function boot). Full record: FIX2ESC-BOOT-LOG.md; stderr-fix2esc.log (40,433 lines); runtime [pgo] 13,999 verbatim; [dk] tail "stallsvc=off tracewake=1 parkwatch=1 seamfix=1 padwatch=1 fix2=1".

Episode 1 was the design's textbook signature: cliff at breakpt=256 (WBLogo drain), [fix2] engaged t=15419ms, synth #1..#10 all token=0/tableA=0, escalated t=16766ms, adv #1, then **revived t=16837ms breakpt=258 synths=10 advs=1** — one advancer invocation moved breakpt; the payload is equivalent to the mismatch exit in the act (F-ADV-DEAD refuted). The revival did not hold: re-freeze 59 ms later inside the same drain; episode 2 escalated immediately (state-carry note: episodeSynths persists across the revived edge, so ep2 logged no engaged line and no AA4 synths and went straight to SYNTH_ADV) and host-paced the advancer for **894 queues** over ~116 s — breakpt crept 258 -> 1,749 (~1/present), A cycled, no adv-dead (A moved) — but **latch=0 in all 895 adv snapshots** and the display never flipped (byte-identical frozen-black captures). That is **F-ADV-AF0 in substance**: the break is downstream of the advancer, in the retrace half at AF0's func_8017BA70 gate; the advancer alone cannot recharge the latch in this regime, so generation never self-sustains. Seam-wake stayed active (352 fires, all woke; it did not fall to ~0 as B4 expected); parkwatch 0/0; display-queue 1,082 sleeps / 2,107 wakeups, all woken. The guest aborted at **frames=2,802** (~1,934 presents into Intro, the Intro-end transition; identical 0xc8a10094 fingerprint) — the third consecutive abort at exactly 2,802; F3's revived-regime premise never held (host-paced dead-display regime), same bookkeeping as the Fix 2 boot. Verdicts: E = ESCALATION-PARTIAL (F-ADV-AF0 in substance); Q2 = NOT EXERCISED (no visible title; recipe corrections untested); J = NOT-REACHED (menu stage; CARDOpen 22 = boot probes only). Profraw fw-1794887997 banked (front-end incl. Intro, guest-side); banked total now 24. Closeout fully verified (resident 34585267 restored, crash.txt overwrite preserved at crash-fix2esc.txt + baseline restored, claim released).

## RETRACE-HALF STUDY (Nov 17, 2026, desk; RETRACE-HALF-STUDY.md)

Commissioned after the escalation proving boot (b5173503, FIX2ESC-BOOT-LOG.md)
scored F-ADV-AF0 in substance. The study recomputed the boot's full [wake]
census and read AF0 / the advancer bookkeeping / func_8017BA70 in the shards.
**F-ADV-AF0's mechanism conviction is REFUTED on the full evidence.** Verdicts:

- **R1 (D4):** D4 = *(0x802716D4), AF0's first test (== 1). Exactly three
  writers in the whole generated tree: the advancer's post-wake bookkeeping
  (D4 := 1 on every run if the mode flag B8 = *(0x802716B8) is 0; on alternate
  runs via the toggle word E0 = *(0x802716E0) if B8 != 0) and AF0's own clear
  at the end of a completed flip body. B8 is written only by a dispatcher
  mode op (two sites, 1/0); its stranded value is unobserved by any current
  instrument and is at most a 2x charge-rate factor. Healthy cadence: ~1
  charge per present (advancer ran 4,832x over 4,833 presents in the control
  boot), consumed by AF0 at the next retrace where BA70 passes.
- **R2 (BA70):** func_8017BA70 is a draw-completion query. It calls
  GXReadDrawSync (0x800FDE88, HLE override in gx_fifo.cpp returning
  g_gxPeToken — an HLE-owned value; the guest argument is discarded),
  stores the token at 0x80271720, and returns 0 iff
  storedToken < buffer C's frame stamp <= frame counter *(0x802706A0).
  AF0's test order is D4 == 1 first, BA70 != 0 second; on pass: token
  rotation, the VI display calls, latch := 1, C advances, D4 := 0. The
  deciding input is HLE-owned but LIVE in the stranded regime (pe47/pe48
  tracked presents 1:1 to 2,802; >=605 nonzero returns proven by R3). No
  circular dependency: BA70's inputs are produced by the present path,
  never by the flip. The design study's "one soft link" is closed.
- **R3 (episode 2 reconstructed):** post-cliff advancer runs total 1,500 =
  895 host-invoked (arm=1/latch=0 snapshots) + 605 GENUINE BB0-driven
  (arm=1/latch=1) + 0 AA4-driven (zero pool: match exits only). Catch-up
  (newA == B) was reached 588 times (294 invoked + 294 BB0); the flag word
  cleared 588 times and presenter site-2 GXEnableBreakPt fired ~583 times
  (breakpt reconciliation: 1,495 = 912 enable-branch + ~583 site-2). AF0
  was invoked ~7,056 times post-cliff (retrace word 963 -> 8,019) and its
  flip body COMPLETED >= 605 times (each (1,1) wakeup snapshot catches a
  latch AF0 set at the end of its body, mid-BB0-call; BB0 clears latch in
  the same pass, so 605 is a floor bounded by the D4 charge count). B moved
  2,544 times post-cliff (~848 rings), exclusively by the presenter's own
  present path. **The latch=0 in all 895 [fix2] adv snapshots is a sampling
  artifact**: latch is set and consumed inside one retrace pass (~60/s)
  while the hook samples at presents (~22/s); the healthy control boot
  shows latch=1 in only 45/4,832 wakeups for the same reason. The true
  episode-2 failure is the ONE-DELIVERY LAG ON ARM: a D4 charge yields
  exactly one latch pulse, in the first retrace pass after the charge,
  consumable only if arm == 1 in that same pass; arm's only setter is an
  AA4 match exit, and AA4s ride the held deque (drains: seam pass + idle
  pump only). Per seam drain the chain therefore produces ~1 genuine BB0
  run (954 post-cliff sleeps vs 605 BB0 runs), then starves until the next
  drain — a delivery-cadence-bound regime, not a dead gate. Separating
  instrument specced (rides the next build): extend the TraceWake snapshot
  with d4/b8/e0 guest reads in the [wake] and [fix2] adv formats.
- **R4 (criterion fixes, specced):** (a) SUSTAIN TEST — breakpt movement
  under invocation is tautological (enable branch calls GXEnableBreakPt
  itself; catch-up arms site 2). Revival is redefined: on the first
  breakpt movement in PRIME_ADV, enter SUSTAIN — K=24 consecutive presents
  with the hook queueing NOTHING (seam-wake/parkwatch keep running; they
  are not synthesis). PASS iff sustainBreakptMoves >= 2 AND
  sustainAAdvances >= 2 (A is written only by the advancer); only a pass
  may print [fix2] revived (with sustain=pass moves=/aadv= fields). FAIL
  logs [fix2] sustain-fail and returns to PRIME_ADV with escalation
  retained; 3 fails -> [fix2] sustain-giveup, episode closes honestly
  host-paced. K=24 is >2.5x the measured burst persistence (<= ~9
  presents) and ~1.1 s at stranded cadence. (b) EPISODE RESET — ALL
  episode state (synths, advs, payload mode, advBaseA/advAMoved/advDead,
  sustain state) zeroes at the revived (= sustain-pass) edge, not the
  engaged edge; episode 2's SYNTH_AA4 bypass (episodeSynths carried at
  10, no engaged line) is the defect this fixes. The 59 ms ep1->ep2
  re-freeze is the SAME episode by definition: an episode ends only at
  a passed sustain or adv-dead, so the question dissolves — no timed
  grace window; sustain IS the grace mechanism, measured in guest
  movement.
- **R5 (next mechanism, specced): DRAIN-AT-RETRACE.** Evaluated and
  rejected: (i) B-management for catch-up (reached 588x — not the
  constraint); (ii) delivering an AF0 input (none is missing); (iii) a
  host write synthesizing the D4 charge (rejected on evidence — charges
  land and are wasted — and on class: host impersonating a guest writer,
  the category PARK-STUDY-2 Q4(b) rejected; it executes no guest code so
  it is not the v1 hazard in kind, but it buys nothing here); (iv)
  combined payloads aimed at AF0's preconditions (present already).
  CHOSEN: deliver the genuine held AA4s at retrace cadence — inside
  AdvanceRetrace(ctx) (vi.cpp:1035), immediately before the pre-retrace
  callback invocation (:1136), if a sticky latch set at the boot's first
  [fix2] engaged edge is set and the held GX deque is non-empty, call
  DvdProcessPendingGxCallbacks(ctx) under the seam pass's deferred-
  callback scope. NEVER in AdvanceRetraceCountOnly (the stall-service
  thread must not execute guest code — the v1 line). Cycle per pass:
  drain (arm := 1) -> AF0 (D4 charge -> latch) -> BB0 (advancer: AA4
  queued, D4 charged) -> next pass drains that AA4 in time for that
  charge. The existing escalation remains the primer; the sustain test
  is the verdict; drains continue during sustain (genuine callbacks,
  not synthesis). Logging: [fix2] drain-engaged once; drains=/drained=
  counters on the sustain/revived/adv-dead lines (per-retrace lines too
  hot). Falsifiers: F-DRAIN-DEAD (drain live, sustains still fail 3x —
  read d4/b8/e0 next), F-DRAIN-PARK (parkwatch rate returns inside
  episodes — pre-named mitigation: restrict the drain to passes where
  the waiter is linked), F-AF0-DEAD redefined on the [wake] census
  (zero (1,1) wakeups across a drained episode; ep2 baseline is 605),
  F3/F5 carried. Proving signature: engaged -> escalated -> adv #1 ->
  drain-engaged -> sustain=pass within 24 withheld presents -> revived
  with advs ~= 1, seam-wake -> ~0, visible flipping chain, PRESS START,
  Intro end crossed.

## HEAP CANARY BUILD (Nov 17, 2026) — build 6e1a5c9b

BURST-FORENSICS-STUDY F4's one build item is built: a read-only
heap-list canary on the b5173503 escalation line. New dk_diag key
`heapwatch` (SCOOBY_HEAPWATCH; dk.h Diag.heapWatch + device.cpp parse /
setenv / [dk] tail `heapwatch=%d` after fix2). The sampler is a host
thread in hle/vi.cpp beside the tracecnt sampler, on the same 250 ms
cadence: it resolves the guest OS heap descriptor — __OSCurrHeap at
0x80270788 (the wrappers func_80015F74/func_80015FA0 load it from
r13 − 31480; SDA base r13 = 0x80278280, anchored by the VI retrace word
and the display A word), heap table base pointer at 0x80271308
(r13 − 28536), 12-byte descriptors (+4 free-list head, +8 allocated
head), cells {+0 prev, +4 next, +8 size} — and walks both lists up to
64 cells per list via TryRead32, validating every cell and link
against the cached RAM window (0x80000000–0x81800000). A wild link
terminates that list's walk and is logged once per distinct bad value
as `[heapwatch] list=<alloc|free> depth=<n> cell=0x… link=0x…` with the
four feeder values at that instant (frames / retrace / ai_cb /
enqueues); clean walks heartbeat at most once per 5 s while frames are
frozen. Read-only only: no guest code, no guest writes, no guest lock
held across the walk; unarmed the thread does not exist and behaviour
is identical to b5173503. Build 6e1a5c9b (55,896,261 B, sha256
6e1a5c9b0f5e6859a51f693df308950bb75dbb1ab065107eca8775e93998cacd),
pgo-trim, 3 files changed (backups at canary-backup\), scope 13,998
functions by ELF arithmetic (+31 counters; the runtime [pgo] line at
the proving boot is the final word); NRO in switch-logs/h3-test only,
Downloads staging still 53dbb7bb. The junction-count boot it enables
(stallsvc=count, eager regime, nine dk_diag keys ending in heapwatch)
and falsifiers F-C1–F-C4 are specced in CANARY-BUILD-LOG.md. Standing
rule: no further burst boots until the canary has timestamped the wild
cell at least once.

## DRAIN-AT-RETRACE BUILD (Nov 17, 2026)

Build **1105fd45** (55,904,453 B, sha256 1105fd4512f38dd08cda19ed0fad2764d854d05fc80b094e1787e0c4cea3e7f9), pgo-trim, base 6e1a5c9b (heap canary). RETRACE-HALF-STUDY R5 + R4 implemented; record: DRAIN-BUILD-LOG.md in no100f-h3-evidence. NRO in switch-logs/h3-test only; Downloads staging still 53dbb7bb. Backups byte-verified at drain-backup\ (5 files); 6e1a5c9b reproducible from them over the canary-backup chain.

- DRAIN-AT-RETRACE (R5): a sticky latch set at the boot's first [fix2] engaged edge (fix2 armed; live-flow timing untouched; unarmed identical to 6e1a5c9b). Inside AdvanceRetrace, immediately before the pre-retrace callback invocation: if the latch is set and the held GX deque is non-empty, DvdProcessPendingGxCallbacks(ctx) runs under the seam pass's deferred-callback scope (OS_HLE_Begin/EndDeferredGuestCallbacks) — per-pass order drain → AF0 → BB0 by construction. NEVER in AdvanceRetraceCountOnly (untouched, delivery-free; the stall-service thread executes no guest code). [fix2] drain-engaged logs once at the first delivering drain; counters drains (passes that drained) / drained (callbacks delivered) ride the escalated/adv-dead/revived lines. dvd.cpp gained the delivered-total counter + pending-count/delivered-total accessors (the drain's bool return carries no count; justified in the build log).
- SUSTAIN TEST (R4a): PRIME_AA4 → PRIME_ADV as built; the first breakpt movement in PRIME_ADV enters SUSTAIN — K = 24 consecutive presents with the hook queueing nothing (drain/seam-wake/parkwatch keep running; they are not synthesis). Per present: sustainMoves += breakpt delta, sustainAAdv += (A_new − A_old) mod 3. PASS (evaluated at K's end) iff moves ≥ 2 AND aadv ≥ 2 → only then [fix2] revived prints, extended sustain=pass moves=%u aadv=%u drains=%u drained=%u, and the episode closes. FAIL → [fix2] sustain-fail, back to PRIME_ADV with escalation retained (re-entry needs a movement after ≥ 1 further advancer queue). 3 fails → [fix2] sustain-giveup, episode closes unrevived. The old revived-on-first-movement behaviour is replaced wholesale.
- EPISODE RESET (R4b): ALL episode state zeroes at the closing edges only (sustain-pass, adv-dead, sustain-giveup) via one reset path; the engaged edge resets nothing (the as-built episodeSynths=1 line is gone). Named interpretations in the build log: the adv-dead/giveup stand-downs outlive their episodes as an admission hold cleared by breakpt movement (the same freeze cannot re-admit a fresh episode; a new episode requires a fresh cliff after movement), giveup closes the episode per the brief over R4a's looser pacing wording, and a breakpt movement in PRIME_AA4 (study silent) closes the episode silently.
- SNAPSHOT (R3 closer): TraceWakeSync gains d4 = *(0x802716D4), b8 = *(0x802716B8), e0 = *(0x802716E0); appended as d4=%u b8=%u e0=%u to both [wake] lines (wakeup + sleep, uniform) and the [fix2] adv line; existing fields/order unchanged. escalated/adv-dead gain drains=%u drained=%u.
- Untouched: detector (5 conditions, K=3), completion flag, seam pass, seam-wake, parkwatch, stall service, heapwatch, [gxcnt], padwatch, tracewake. Scope 13,997 functions by ELF arithmetic (−1 vs base, unity-bucket inlining variance; +66 counters). Proving boot: nine dk_diag keys (the Fix 2 ten minus stallsvc — front-end boot, junction rides later); expected [dk] tail stallsvc=off tracewake=1 parkwatch=1 seamfix=1 padwatch=1 fix2=1 heapwatch=0. Falsifiers carried: F-DRAIN-DEAD / F-DRAIN-PARK / F-AF0-DEAD (redefined) + F3/F5. NOT boot-tested.

## JUNCTION-COUNT BOOT (Nov 17, 2026) — count does not cure; the heap stays clean

One launch of heap-canary build 6e1a5c9b (pgo-trim, eager regime, dk_diag nine keys with stallsvc=count + heapwatch; runtime [pgo] count 14,002 — the runtime line governs over the build log's 13,998 ELF arithmetic; GPU-307 lottery boot, function-class). Full record: JUNCTION-COUNT-BOOT-LOG.md + stderr-junctioncount.log in the evidence set.

**Verdict C — COUNT-NO-WAKE (F-C3).** The load landed and the junction wedge formed in the classic pattern (card scan complete, h001.HIP never issued). The stall-service gate opened five times; four episodes ran to the 600-firing cap and the fifth stood at firing #230 at log end — **2,630 Stage A count-only deliveries total**. Across all of them: frames frozen at 10817, ai_cb frozen at 295575, enqueues silent, no witness of either kind anywhere in the log, no wake. The junction wait does not read the raw retrace count word; it is callback-maintained. Next design per the study: a retrace-callback delivery at scheduler-safe points (seam/idle-pump class). Count's safety claim is proven in the act: 2,630 deliveries, zero guest code executed, guest alive at every sample, crash.txt never overwritten, clean HOME close — the controlled contrast with burst (dead in round 3 at the same junction) is now on record.

**Verdict H — NO TRIP.** Zero [heapwatch] list= lines in the whole boot (103,882 log lines); 215 clean-walk heartbeats from guest t=5 s to t=1646 s covering the front-end flow, the cliff/parkwatch stretch, the menu, the card scan and all count episodes. Free list (18 cells) walked in full every sample; allocated-list coverage caveat: the walk cap (64) truncates the allocated list every sample, so only its first 64 cells were validated. F-C2 and F-C4 do not trip. The wild cell evidently is made by delivery-executed guest code, not found waiting: it has only ever appeared under callback-executing deliveries (v2, eager burst), never under passive observation or count. Burst remains shelved per F4's standing rule (the canary-timestamp precondition is still unmet).

**Verdict Q2 — LOAD LANDED.** Title at server t=164 s; menu driven at parkwatch cadence (~1.08 fps by frame counter; the front-end flow phase ran ~17.2 fps before the cliff froze breakpt at 9817, guest t~576 s). Input lessons, recorded in full in the session log: full-scale stick tilts do not step singly in this menu (wrap steps land; the NEW GAME -> LOAD GAME step took a short -127 excursion after opposite-sign excursions); dbg hold LATCHES in this server build (the demo-exit hold B left btn=0x0100 set for ~110 guest-seconds and ate a confirm edge; release B cleared it; all landed confirms were sys-agent press B with explicit release); the menu timed out to the attract demo twice and remembered its selection both times. The debug server's logtext command returns the live log ring (contrary to the campaign's standing note) and was used for near-live [cnt]/[padwatch] reads. Final confirms were gate-verified (LOAD GAME glyph gate 61.70; file list capture-verified with "11/14/26 10% The Mystery Machine" on top) before the load confirm at 04:47:25Z.

**Verdict P.** Parkwatch 333 signatures / 333 fires / 333 resumes / 0 dry; 333 display-queue sleeps, 10,149 wakeups; [seamfix]/[seamwake]/[fix2] 0 lines; gxheld_hw=0 throughout.

Gameplay/route not run (no cure, no spawn). Profraw fw-1794889389 (5,638,360 B, sha 6c1d5341...) banked to pgo\ and h3-test; **banked total now 25** (coverage: front-end/title/menu/junction). Closeout fully verified: resident 34585267 restored + read-back, dk_diag absent, crash.txt and save at baseline, Config untouched, claims deleted + verified absent, console at clean HOME (the wedged-exit error dialog was dismissed under a brief second claim). Note: the card stderr.log ends mid-episode at guest t=1646133 ms — the HOME close's final flush never landed; all verdict evidence precedes that point.

## JUNCTION-CALLBACK DESIGN (desk study, Nov 17, 2026)

Full text: ~/workspace/no100f-h3-evidence/JUNCTION-CALLBACK-DESIGN.md.
Question: how to cure a callback-maintained wait whose guest thread
spins in a pump-free poll, without repeating the host-initiated-
delivery heap corruption (the wild cell 0x389dfff4).

- **J1 (execution points in the wedge).** Only the spinning guest
  thread executes guest code. SelectThread's idle loop NEVER runs (the
  spinner is runnable and never reaches a switch point; 318 s of
  frozen feeder counters in the tracecnt boot prove it), and
  AdvanceRetrace never runs — retrace advance in this HLE is
  pump-driven, not present-driven or free-running (due-ness is
  wall-clock, consumed only at guest-activity call sites). The stall
  service, tracecnt/heapwatch samplers, watchdog dump thread, debug
  server (grep-verified: no guest-invocation call in its sources),
  AX mix worker and profiler all run but execute no guest code;
  parkwatch's actuation point is the idle loop, so it cannot cure a
  spin. No existing point can deliver safely: the spinner executes
  continuously, so any delivery elsewhere is a second executor by
  construction.
- **J2 (the invariant).** Single-executor rule: guest code may run
  only on the scheduler's stream, on a scheduler-owned context at a
  defined suspension/delivery point, under the deferred-callback
  discipline. Candidates: (a) never-concurrent — convicted as
  necessary (burst was fatal against a guest "only polling": the
  delivery flips the poll's exit and the spinner runs transition code
  concurrently with the delivery's tail); (c) scheduler-owned context
  — convicted jointly (v1/v2/burst all ran on the service thread on a
  persistent-context seed; GuestInterruptCallbackContext falls back to
  it when no ambient context exists); (b) re-entrancy — cleared as
  the separator (the same nesting is the healthy restore-delivery
  mechanism: 4,832 wakeups at inrestore=1 in the control boot), kept
  as an aggravator (thread_local audio cursor desync); (d) some other
  maker — cleared (the identical cell across two heap ops and two
  regimes, absent across 2,630 count visits and thousands of
  scheduler-stream executions of the same callback trees). Caveat:
  the planting write is still unobserved; heapwatch has never been
  armed during a service-thread delivery episode.
- **J3 (the poll).** NOT the raw retrace count word 0x80271398
  (AdvanceRetraceCountOnly verified in source: count + guest-word
  write only; 2,630 advances, no wake). Code-identified poll:
  func_800D8164 (called only by func_80044F7C), two unbounded loops on
  the completion flag 0x80271020 — wait-clear at 0x800D859C,
  wait-set at 0x800D85B4 — flipped only by the stream completers
  (func_800D85E0 clears; func_800DA7F0 / func_800D9D24 set) off the
  AI/AX DMA chain (aid_cb 0x800F704C -> __AXOutNewFrame -> AX user
  callbacks). Eliminated by full reads: 0x80271730 (a render-mode
  pointer; bounded XFB fills), 0x80270450 (a hash-table entry count;
  bounded probe loops), A/B (queue sleep, not a poll),
  VIWaitForRetrace (native override; sleeps). Retrace pair: pre
  0x800168F0 (stores the count at 0x80270810, chains to the engine
  callback at *(0x8027080C) — the func_80016958/func_800CFD14
  housekeeping tree), post 0x80175BB0 (display gate -> advancer).
  Order/multiplicity: v2's release (<=6 retraces + ~40 ai_cb) cannot
  separate the classes; burst's no-wake is not a bound.
- **J4 (design).** BORROW-THE-SPINNER is a STOP: (1) no preemption
  point — guest execution is host call-stack recursion and the poll
  blocks call nothing; the seam borrow is a self-borrow at a voluntary
  suspension point and does not transfer; (2) the spinner's register
  file is live host locals, not the thread record's stale snapshot —
  there is no live PC to gate on either; (3) executor state lives
  outside the context (call stack, fiber record, ambient scope,
  thread_local audio cursor and drain guards); (4) success creates
  the fatal configuration — the callback's flag flip releases the
  spinner into transition code concurrently with the delivery's tail.
  The sound long-term shape is an executor safepoint (recomp-emitted
  poll checks), a translator feature, not an instrument. DECIDING
  INSTRUMENT instead: `stallsvc=probe` in the existing stall service
  — v2 gate byte-for-byte + heapwatch REQUIRED + spinner signature
  (thread 0x8026D568 state=2, queue 0x0, two consecutive ticks);
  doses sequential by class: 1 retrace pair -> 10 s observation ->
  1 audio poll -> 10 s -> capped multiplicity (8 pairs / 16 polls,
  caps exceed v2's releasing dose); one episode per boot; stop on any
  heap trip, enqueue/frames movement, or process exit. Falsifiers:
  F-J1 (heap trip <=10 s after any dose, or the 0x389dfff4 crash —
  foreign delivery convicted at minimum dose, family closed),
  F-J2 (no wake at full dose), F-J3 (wake with no dose), F-J4 (wake
  without flag 0x80271020 moving — func_800D8164 cleared, J3 reopens).
  Outcome matrix and boot shape in the full text.

## DRAIN PROVING BOOT (Nov 17, 2026) — DRAIN-PARTIAL

One launch of 1105fd45 (nine keys, no stallsvc/heapwatch; STOCK clocks;
runtime [pgo] 14,005 functions — the runtime line governs over the build
log's 13,997 ELF arithmetic). Full record: DRAIN-BOOT-LOG.md;
stderr-drain.log (1,932,281 B, sha 7ede31ba…). Gate note: the boot first
stood down for CTTR's round-18 window and took the slot on their release.

Verdict D: DRAIN-PARTIAL. Three episodes, all in logo-chain drain windows.
Episode 1 (engaged breakpt=234): synths #1..#10 zero-shaped, drain-engaged
7 ms after the engaged edge (drains already 10/10 by escalation — the
drain delivered the synthesized AA4s during PRIME_AA4), advs=3, two
sustain-fails then **revived sustain=pass (moves=2 aadv=2, drains=21
drained=23)** — the campaign's first sustain-verified revival. Episode 2
engaged 32 ms later at the next drain boundary and also **revived
sustain=pass (advs=2, drains=38 drained=41)**. Episode 3 failed three
sustains at moves=1/aadv=1 each and hit **sustain-giveup** (drains=61
drained=66); the admission hold then blocked all further episodes —
breakpt never moved again (final 263, frozen ~93 s) — and the boot
reverted to the seam-wake carry (851 fires, all woke), display frozen
black throughout, chain completed guest-side, and the guest aborted at
**frames=2,802 for the fourth consecutive time** (identical fingerprint
esr=0x92000006 / VA 0xc8a10094; crash-drain.txt preserved, baseline
restored).

Falsifiers: F-DRAIN-DEAD trips in episode-3 shape only (episodes 1–2
passed, so the coincidence model survives as a revival mechanism but is
refuted as a durable cure on this evidence); its half-rate-charge
follow-up is answered by the new fields — **b8=0 in every [wake] line
and adv snapshot: D4 charges at full rate, the B8 toggle hypothesis is
refuted** (d4=1 in only 2 wake lines; e0=0 throughout). F-DRAIN-PARK not
tripped (parkwatch 0/0). F-AF0-DEAD not tripped (141 (arm=1, latch=1)
wakeups). F3 not tripped in its strict sense — at the Intro-end crossing
the regime was post-giveup dead-display, not sustain-passed-live; whether
a DURABLY revived regime crosses Intro end remains untested. F5 not
tripped. Verdict Q2: NOT-EXERCISED-CADENCE (no visible title ever
existed; padwatch 4 baseline lines). Verdict J: NOT-REACHED. Verdict P:
parkwatch 0/0; seam-wake 851/851; display queue 968 sleeps / 1,116
wakeups; [seamfix] 331 post-link deliveries, 119 gx≥1; [gxcnt] final
breakpt=263 pe47=pe48=2,802 pe45=0.

Profraw fw-1794891995 banked (front-end chain incl. Intro, guest-side);
**banked total now 26**. Closeout fully verified (resident 34585267
restored + read-back, dk_diag absent, crash.txt overwrite recorded and
baseline restored, save/Config untouched, claim deleted + verified,
relays sent).

Open thread this leaves: revival is real but per-boundary — each movie
drain re-cliffs within a present-set of the previous revival, and after
a giveup the admission hold (by design) prevents re-priming for the
rest of the boot. Candidate questions for the desk: whether the hold
should release on a fresh K=3 freeze after a long dead interval (the
hold's clearing condition — breakpt movement — is exactly what a dead
relay cannot produce), and whether sustain's ≥2/≥2 bar over 24 presents
is measuring durability or merely twitch in a sputtering relay.

## DRAIN DURABILITY STUDY (Nov 17, 2026, desk; DRAIN-DURABILITY-STUDY.md)

**Headline: the drain works; the relay still dies at the catch-up, by a phase-slip that is R3's one-delivery lag rotated one link further.** Full per-window census of stderr-drain.log recomputed: all 8 sustain windows of all 3 episodes run exactly one priming burst (2-6 advancer runs, 1 delivered + rest BB0), and every burst terminates in a catch-up run (A==B snapshot, 8/8). The catch-up is asymmetric: its bookkeeping charges D4 immediately, but it queues NO AA4 and clears flag (0x802716D0:=0), so arm's next recharge must come from the presenter's site-2 call - present-paced, not retrace-paced. AF0 spends the catch-up's D4 charge in the first retrace pass after the burst (proven: D4's only clear is AF0's body-tail write, and post-burst sleep snapshots read d4=0 with no advancer run intervening), into a gate whose arm half BB0 already cleared - the charge is wasted; when the site-2 AA4 is finally drained (arm:=1), D4 is spent and its only producer is downstream of the coincidence. The state (arm=1, d4=0, latch=0) is absorbing and is verbatim the state of every post-burst snapshot. Deque supply exonerated: drained rose ~+4-5 per window throughout, spent on token-match exits. The drain closed R3's lag for the advancer's own AA4; the catch-up reintroduces it on the presenter's AA4. Flag is inferable from the existing log (catch-up => flag=0; post-burst breakpt +1 with no wake line => site-2, flag 0->1); the delta still adds flag to the TraceWake snapshot to make it a direct read.

**Sustain verdict: the bar measures twitch.** Both episode 1-2 passes were burst tails - zero advancer runs occur after any window's opening burst, including both passing windows - and both "revived" episodes re-froze within 32-33 ms of closing. Counted moves (0,1,2 / 0,2 / 1,1,1) track where the sustain-entry present fell inside the burst, not burst size: a phase lottery. Durability criterion (proving line): sustain pass enters a 72-present WATCH (hook queues nothing); a K=3 freeze in WATCH prints [fix2] watch-fail and re-opens PRIME_ADV in the same episode; a freeze-free WATCH with >=2 moves and >=2 A-advances in its final third prints [fix2] durable and only then closes the episode.

**Admission hold v2 (fixes the observed deadlock - breakpt frozen at 263 for ~93 s because the as-built hold clears only on movement a dead relay cannot produce):** primary clear is fresh-cliff (breakpt != holdValue); deadlock-breaker is a persistent clear (breakpt unchanged across K=3 presents AND >= holdBackoff presents elapsed; backoff 240, doubling per persistent cycle, cap 960, reset on fresh clear/durable close) admitting a resumed=1 episode with a 1-fail sustain budget. Deadlock-free while presents advance; F-ADV-DEAD stays a per-episode verdict; long-run synthesis in a dead relay decays instead of leaking (pure cooldown) or silencing (as-built).

**Design centre: a standing service, not per-episode revival.** The hazard is per-boundary and perpetual (every movie teardown re-strands), so the cure must be too: (1) the drain, latched as built; (2) a RE-PHASE PRIMER - at a present, if fix2 armed + drain latched + snapshot reads arm=1 and d4=0 and breakpt frozen x3 and A != B, queue ONE advancer via the existing SYNTH_ADV path ([fix2] rephase line with flag field); cooldown until movement+refreeze or 48 presents; per-boot cap 600. The gate can only fire into the proven slip state of a frozen relay (live flow never satisfies the conjunction), the payload is the guest's own advancer delivered at existing points (895 escalation deliveries, no corruption - F-ADV-ALIVE shape preserved), and one queue from the slip state re-phases both supplies. The sustain machine stays for the proving line (with WATCH); in the shipping shape it demotes to a [fix2] health line every 240 presents. Stated limit: the service does NOT restart generation (setup op / CopyDisp geometry) - no hook-layer mechanism on record does; it keeps A advancing and AF0 flipping at burst cadence for the whole session.

**Build delta (on 1105fd45; detector, completion flag, drain, seam/seam-wake, parkwatch, stallsvc, heapwatch, count-only path, N=10/M=30 all untouched):** (1) flag=*(0x802716D0) added to TraceWakeSync, printed on [wake] + [fix2] adv/rephase lines; (2) admission hold v2 per above; (3) WATCH stage + [fix2] durable / watch-fail lines; (4) re-phase primer per above; (5) [fix2] health line. Proving signature: rephase lines between boundaries without episode opens, >=1 [fix2] durable OR health movement in every 240-present block across >=3 movie boundaries, PRESS START visible, Intro end crossed with the regime recorded (F3's fair test); no 90 s frozen-breakpt stretch anywhere. Falsifiers: F-REPHASE-DEAD (>=10 fires across a boundary, zero burst movement - read flag/d4), F-REPHASE-RUNAWAY (any fire within 3 presents of movement - implementation defect), F-HOLD-STARVE (fresh cliff not admitted within 3 presents), F-DURABLE-TWITCH (durable close then re-freeze within 72 presents), plus carried F-DRAIN-PARK / F3 / F5. If bursts advance A but the frame never unfreezes, the remaining defect routes to the display-setup / CopyDisp layer, not the hook - named in advance.

## PROBE BUILD (Nov 17, 2026) — stallsvc=probe built: build 8fdc8ca5

Built Nov 17, 2026 (NST), desk only — no console contact, nothing launched
on the PC. Spec: JUNCTION-CALLBACK-DESIGN.md J4.2. Base build: 1105fd45
(drain-at-retrace + sustain test; heapwatch aboard). Provenance chain
extends the drain chain:
… → 6e1a5c9b → 1105fd45 → **8fdc8ca5 (stallsvc=probe)**.

- **NRO: 55,908,549 B, sha256
  `8fdc8ca536f5e5eeb993f565db20464e04f436180cf3f04edb5d2eb7dc352b31`**
  (+4,096 B vs 1105fd45 — one NRO alignment step). Invocation:
  `bash switch/scripts/build-switch.sh --deko --variant pgo-trim
  --out none` from `C:\dev\no100f-nx` in WSL, held foreground, clean link
  (BUILD_EXIT=0). Placed hash-verified at
  `switch-logs\h3-test\no100f-nx.nro` only; Downloads staging verified
  still `53dbb7bb`. 1105fd45 remains reproducible: its tree state is
  exactly the probe-backup contents over the drain-backup chain.
  Backups at `C:\dev\no100f-nx\probe-backup\` (byte-verified pre-edit).
  Full record: PROBE-BUILD-LOG.md in the evidence set.
- **Files changed (3):** `hle/vi.cpp` (147,743 → 168,397 B) — svcMode 3
  parse, the probe state + snapshot helper beside parkwatch's state, the
  probe block ahead of the parkwatch sampling, the gate guard's mode-3
  skip, and the heapwatch trip wire (below); `switch/deko/device.cpp` —
  the `stallsvc=probe` parse (→ 3), the NO100F_STALLSVC="probe" setenv,
  and the [dk] line printing `stallsvc=probe`; `switch/deko/dk.h` —
  the stallSvc comment gains mode 3. Semantic diff (backup → current,
  CRLF-normalised) reviewed in full: additions only apart from the parse
  lines, the gate guard, and comment lines. Count and burst behaviour is
  unchanged (mode 3 never enters the gate block; the probe code is inert
  unless svcMode == 3).
- **Reused delivery entries (verified in the burst code before
  building):** dose of the retrace class = `VI_HLE_ProcessRetracesDeferred(1)`
  — the exact call the burst loop makes twice per round, one full
  pre+post retrace pair through the existing deferred machinery; dose of
  the audio class = `Audio_HLE_PollDeferred()` — the burst loop's
  per-round audio call. Both are callable in isolation; no helper was
  extracted and no delivery code was hand-rolled.
- **Gate, as implemented:** the v2 feeder-silence gate's conditions,
  replicated in the probe block (same feeder change stamps, frames != 0,
  ≥2000 ms on all four feeders, `aurora_frame_worker_idle()`, the same
  due test, and the same DetClock / interrupts-enabled guards that
  precede the gate in the service loop) — replicated rather than shared
  because the probe episode spans minutes of ticks and cannot live
  inside the one-shot gate block. PLUS (i) heapwatch armed: without
  SCOOBY_HEAPWATCH the probe logs `[stallsvc-probe] refused: heapwatch
  not armed` once and never delivers; PLUS (ii) the spinner signature —
  main thread 0x8026D568 state==2 (RUNNING) with queue word 0x0, read
  through parkwatch's thread-record access (state at +0x2C8, queue at
  +0x2DC), on two consecutive 50 ms ticks.
- **Trip-wire link (brief item 3, justified):** the canary sampler
  exposed no host-visible trip state (its `tripped` was lambda-local),
  so the build adds the minimum: a file-scope atomic
  `s_heapWatchTripped`, set by the sampler inside `reportTrip` at any
  detected trip, read by the probe's stop checks via `HeapWatchTripped()`.
  The sampler's behaviour is otherwise unchanged; nothing else consumes
  the atomic.
- **Dose machine, as implemented:** one episode per boot, never
  re-arms (a second full silence window is logged and ignored). Episode
  open logs the feeder values, the J3 candidate flag 0x80271020, and the
  A/B/arm/latch snapshot at gate-open. Dose 1 = one retrace pair, then a
  10 s observation window (1 s summary lines with the same value set).
  Dose 2 (only if no wake) = one audio poll, second 10 s window. Dose 3
  = capped multiplicity, one delivery per 2 s: retrace pairs first to a
  cumulative 8, then audio polls to a cumulative 16 (dose 1/2 deliveries
  count toward the caps). WAKE = frames advance OR the enqueue counter
  moves OR the flag flips, checked every tick against the episode-open
  bases, with the witness named in the wake line; movement after the v2
  gate completed but before dose 1 logs `wake dose=0
  witness=spontaneous` (F-J3's shape). Stop checks before every dose and
  every observation tick: a heapwatch trip logs the abort line and
  permanently suppresses delivery; process exit closes the probe.
- **Scope:** ELF arithmetic — `__llvm_prf_data` 895,808 B = **13,997
  functions** (identical to base; the probe adds no named functions),
  `__llvm_prf_names` 267,000 B (identical), `__llvm_prf_cnts` 4,476,240 B
  (**+48 counters** vs base). All eleven [stallsvc-probe] log forms
  verified present in the NRO by strings. The proving boot's runtime
  [pgo] line remains the final word.
- **Probe boot readout:** dk_diag exactly nine lines — `fifofast` /
  `watchdogat=420` / `traceio` / `tracecnt` / `stallsvc=probe` /
  `tracewake` / `parkwatch` / `padwatch` / `heapwatch` (eager regime,
  seamfix/fix2 OFF). Expected [dk] tail: `stallsvc=probe tracewake=1
  parkwatch=1 seamfix=0 padwatch=1 fix2=0 heapwatch=1`. Outcome matrix:
  wake after dose 1 = the retrace pair is on the release path;
  wake after dose 2 (flag flips in the dose-2 window) = the audio class
  is, per J3's func_800D8164 identification; no-wake at full dose
  (8 pairs + 16 polls) = F-J2; a heapwatch trip within 10 s after any
  dose, or a crash carrying 0x389dfff4 = F-J1; a wake with the flag
  never moving = F-J4; a dose-0 spontaneous wake = F-J3. A clean
  single-delivery wake proves existence, not safety (n=1): a confirming
  boot is required before any adoption.

## DURABILITY BUILD (Nov 17, 2026) — the standing service: build 23069139

Build **23069139** (55,912,645 B, sha256 230691395f4ba8ad04eaa36f126ccc28bd652ccac920bc5262427e63f384f29f), pgo-trim, base 8fdc8ca5 (probe). DRAIN-DURABILITY-STUDY Q4 implemented; record: DURABILITY-BUILD-LOG.md in no100f-h3-evidence. NRO in switch-logs/h3-test only; Downloads staging still 53dbb7bb. Backups byte-verified at durability-backup\ (4 files); 8fdc8ca5 reproducible from them over the probe-backup chain.

Four files (hle/vi.cpp, hle/os/os_scheduler.cpp, hle/os/os_sleep.cpp, wiicompiled/runtime/include/hle_stubs.h); semantic diff (backup -> current, CRLF-normalised) reviewed in full. (1) The flag word *(0x802716D0) joins TraceWakeSync (one TryReadGuestScalar beside the existing reads) and prints appended on both [wake] lines, the [fix2] adv line and the new rephase line. (2) Admission hold v2 replaces the v1 clear-on-movement rule (which deadlocked in the drain boot): FRESH clear when breakpt moves off the hold value (backoff resets to 240); PERSISTENT clear when breakpt == holdValue, frozen across K = 3 presents, and the backoff interval elapsed (240, doubling per persistent cycle, cap 960; a durable close also resets it) — the admitted episode is resumed on a one-fail sustain budget. adv-dead and sustain-giveup set the hold with holdValue/holdStartPresent; F-ADV-DEAD remains a per-episode verdict. (3) WATCH: a sustain pass prints revived as before but no longer closes the episode — it enters WATCH (72 presents, hook queues nothing); a K = 3 freeze (reason=freeze) or a final third (presents 49–72) short of >= 2 breakpt moves / >= 2 A advances (reason=movement) prints [fix2] watch-fail and re-opens PRIME_ADV in the same episode (not a sustain fail; the fail budget is untouched); a freeze-free watch with the final-third movement prints [fix2] durable (moves/aadv = the final-third totals) and only then closes the episode. (4) The re-phase primer: fix2 armed, drain latched, no episode open, and the slip signature (arm=1, d4=0, A != B, breakpt frozen across 3 presents) -> one SYNTH_ADV queue through the escalation's held-deque path; cooldown until breakpt moves and re-freezes (x3) or 48 presents pass; per-boot cap 600 with a one-time [fix2] rephase-capped line. The primer is evaluated ahead of the hold, so a standing hold does not suppress the service (a primer burst is also what fresh-clears a hold); it queues no draw completion, so the completion flag is not noted. (5) [fix2] health every 240 presents while armed (episodes/durables/rephases/seamwakes); seamwakes reads the seam pass's own fire counter via a new read-only accessor OS_HLE_SeamWakeFires() — the counter's storage moved to file scope in os_sleep.cpp, incremented exactly where it always was (named deviation, flagged in the build log: the counter was a function-local static, invisible from vi.cpp).

Untouched (verified against the study's list): the detector and its conditions, the completion flag, the drain itself and its latch, the sustain machine's PRIME_AA4/PRIME_ADV phases and N = 10 / M = 30, seam/seam-wake logic, parkwatch, ALL stallsvc modes (count/burst/probe incl. the heapwatch trip wire), heapwatch, tracecnt, AdvanceRetraceCountOnly. Unarmed (no fix2 key) behaviour is identical to 8fdc8ca5: every new path sits behind OS_HLE_Fix2Armed() in the present hook or behind the tracewake arm in the [wake] loggers.

Scope (ELF arithmetic): __llvm_prf_data 895,872 B = 13,998 functions (+1 vs base — the OS_HLE_SeamWakeFires accessor, a named function this time, not bucket variance), __llvm_prf_names 267,014 B (+14), __llvm_prf_cnts 4,476,584 B (+43 counters). All new log forms verified present in the NRO by strings; no compiler diagnostics on the edited files.

Proving boot readout: dk_diag exactly nine lines — fifofast / watchdogat=420 / traceio / tracecnt / seamfix / tracewake / parkwatch / padwatch / fix2 (stallsvc ABSENT, heapwatch absent). Expected [dk] tail: "stallsvc=off tracewake=1 parkwatch=1 seamfix=1 padwatch=1 fix2=1 heapwatch=0". Proving signature: rephase lines between boundaries without episode opens; >= 1 durable close or health-line movement in every 240-present block across >= 3 movie boundaries; PRESS START visible; the Intro end crossed with the regime recorded; no 90-second frozen-breakpt stretch anywhere. Falsifiers: F-REPHASE-DEAD / F-REPHASE-RUNAWAY / F-HOLD-STARVE / F-DURABLE-TWITCH, plus carried F-DRAIN-PARK / F3 / F5. If bursts advance A but the frame never unfreezes, the remaining defect routes to the display-setup / CopyDisp layer (the study's named routing), not the hook.

## PROBE BOOT (Nov 17, 2026) — J = NOT-REACHED; Q2 = LOAD NOT LANDED; H = NO TRIP

One launch of 8fdc8ca5 (stallsvc=probe + heapwatch, eager regime) at STOCK clocks (the [dk] clocks line read 1020.0/384.0/1331.2 STOCK). Runtime [pgo] line: 14,005 functions (desk 13,997, +8). Full record: PROBE-BOOT-LOG.md in the evidence folder; stderr-probe.log (7,361,508 B, sha 2d413f94...).

- J = NOT-REACHED (no wedge): the load never confirmed, so no card scan, no junction, and zero [stallsvc-probe] lines in the full log. No dose was delivered; no F-J falsifier tested. The probe remains armed for the next boot that reaches the junction.
- Q2 = LOAD NOT LANDED — the session's actionable finding. Menu reached and opened (opener correction: sys-agent PLUS never registered at parkwatch cadence this boot; dbg hold PLUS 2.5 s opened it, clean releases). Selection driven NEW GAME -> LOAD GAME with bright-fraction gate proof (+Y then -Y excursion pair; LG gate stable across ~200 s). But SIX confirm attempts never fired the choose action: sys-agent press B 1.5 s, chained B presses x2, sys-agent press A (0x0200), dbg hold B 10 s across 10 pad reads, sys-agent START — card activity in the menu phase: none (the only CARDOpen group is the boot-time probe). This contradicts the eager boot (press-B confirmed x3 first try) and the count boot (press-B confirms landed); button, duration, input path, latch and selection state were all ruled out in-session. A NEW GAME confirm was deliberately not tested (card-write risk to Brandon's save). Open discrepancy, unexplained.
- H = NO TRIP: 0 [heapwatch] list= lines; 615 clean walks across the session (alloc 64 / free 18 every sample; allocated list capped at 64 cells per walk as designed). The wild cell never appeared without callback-executing deliveries, consistent with the count boot's inference.
- P: parkwatch 1,174 signatures / 1,173 fires / 1,173 outcomes, all "frames resumed", 0 dry. Cliff at breakpt=409 early; cadence 1.1 fps by frame counter for the whole post-cliff session; final frames=3,929. Watchdog at 420 s: frames advancing, no dump. Clean HOME close; userAppExit clean.
- Profraw fw-1794893164 banked (5,639,688 B, sha c30669df...), coverage front-end/title/menu; banked total now 27.
- Claim-write race with CTTR round 19 at ~05:23-05:24Z: CTTR stood down and ceded; their one A press was inert guest-side (padwatch); my staging verified undisturbed; claim rewritten in my name with the race noted and held for the session. Lesson adopted: re-verify a boot's claim immediately before the first input, not only at claim time. Closeout fully verified (resident 34585267 restored + hash match, dk_diag deleted, crash.txt at baseline, save and Config untouched, claim deleted, relays sent; console free for CTTR round 19).
- Next: the probe episode still needs a boot whose load confirms — re-prove the menu confirm path first (the count boot's menu opened in flow at ~17 fps and its confirms landed; this boot's menu lived its whole life at 1.1 fps). The durability proving boot (23069139) is unaffected and remains queued behind CTTR round 19.

## MENU-CONFIRM STUDY (Nov 17, 2026) — why the probe boot's confirms refused

Desk study, read-only (full text: MENU-CONFIRM-STUDY.md in the VM evidence folder). Three-way comparison of every confirm attempt in the eager (af677c10), junction-count (6e1a5c9b) and probe (8fdc8ca5) boots, plus the guest input path traced in the shards. Method facts banked: padwatch t is HOST WALL ms since the first padwatch call, not guest time; n is the global PAD__Read_HLE call count; PAD sampling rate is a regime fingerprint (~100/s in flow, ~1-3 per park cycle in the parked menu state, 200-600/s in attract/demo spin-poll).

- C1: NO logged state variable separates landed from refused. Killed with numbers: the display-sync snapshot (A=0 B=2 arm=0 latch=0 before every attempt in all three boots, landed and refused alike); press duration (landed delta-n in {1,3,6} vs refused {1,2,10}; landed wall 2594 ms vs refused 2595 ms; ~2-4 guest frames everywhere); spanning a parkwatch resume boundary (count #1 landed spanning one, probe #1 refused spanning one, probe #5 spanned ~4); menu-open path (eager storm-opened via sys-agent PLUS and landed; probe dbg-PLUS-opened and refused); stick/trigger state (0,0 and 0/0 on every press line); menu age/demo cycling. The PADRead-level state gate also provably accepted on every press frame (below). Of the probe's six attempts, #2 was never sampled (park gap) and #3 was delivered during a spin-poll window (8,100 reads in the 13.25 s inter-press gap = ~611/s, the attract/title fingerprint, not the parked menu's) — the active screen was not the menu. The hard residue (#1, #5, #6): edges verifiably written to the pad object's action word while LOAD GAME was brightness-gate-verified on screen, B held across ~4 park cycles in #5, and no choose and zero card activity followed. The eater is downstream of the pad object, inside the frontend screen layer, in state no current instrument logs. Only unseparated residual on the build axis: the drain lineage (probe) vs pre-drain (both landed boots) — with seamfix/fix2 off the drain never latches and its retrace path is inert, so no mechanism convicts it; recorded, not charged.
- C2 (the gate, with addresses): the input path is fully traced. PADRead 0x8010CEE0 has exactly ONE direct caller, the pad update func_8000F5E8 (HLE No100f_PadUpdate), which consults the state gate func_8004421C first: it reads the game-state word W at 0x80270A70 (r13-30736, r13=0x80278280) and SKIPS pad input iff W is in {7, 9, 10, 11, 12} (accept for 0-6, 8, 13+); gate callers 0x8000F5E8, 0x800BF4A8, 0x800D9170, 0x800D9770, 0x800DA8F8. Proven OPEN during every attempt (padwatch lines exist, and PAD__Read_HLE runs only after the gate accepts). Above it, the input update func_80031DDC calls the pad update with out = its caller's stack slot (loc_80032088) and post-processes the action word in the slot: swaps the kB (0x80000) and kY (0x40000) bits (loc_800320BC-DC), synthesizes stick-direction bits (threshold +/-50) with repeat timers (floats at pad+92/+96/+100), gated by the pad flag word at pad+64 bit 16. NO edge computation there — actions are LEVEL bits; consumers edge-detect themselves. The screen frame helper func_800CCBD4 computes the frontend frame delta from the tick at 0x800000F8, CLAMPS it (r13-32328), calls the input update twice, then func_80032E60(delta) (tween/timer advance), then func_800CE760; it serves TWELVE screen handlers (0x800C9168, 0x800C9E50, 0x800CA054, 0x800CA3BC, 0x800CAAD0, 0x800CAD0C, 0x800CB184, 0x800CB4D0, 0x800CB97C, 0x800CBD58, 0x800CC2E0, 0x800CC920) — the frontend screen cluster; func_800BF4A8 is the leading orchestrator candidate above them. The choose follow-through is a state transition through func_8004429C with the pending-state word at 0x80239060 (busy flag 0x80270A80, helper func_80044190); zero card opens in the probe menu phase means it was never applied. NAMED GATE: the confirm is eaten between the post-processed action word (func_80031DDC loc_800320BC) and the transition machinery (func_8004429C / 0x80239060), inside the active screen handler. Ranked mechanisms: (1) wrong-screen consumption — the screen drawing the list was not the screen owning input (proven in the act for attempt #3); (2) tween/fade starvation via the clamped delta at parkwatch cadence (strained by attempt #1 at +104 s); (3) edge phase — killed standalone by the 10 s hold, survives only as a sub-case of (1).
- C3 (PLUS anomaly): SAME phenomenon as the sampling side of C1, not a path difference. At parkwatch cadence PAD is sampled only inside wake bursts (duty cycle ~3-8% of a 2,050-2,700 ms cycle): a ~100 ms sys-agent press has a ~92-97% chance of falling in a park gap, a 1.2 s hold ~50% per attempt; the probe boot's two consecutive sys-agent PLUS misses are ~45-50% joint — unremarkable. The dbg route sets persistent host pad state, so the ~2.6 s dbg PLUS holds span a full cycle and always sample. Both paths compose to the identical padwatch word: a sys-agent B tap WAS sampled in the same boot (demo exit, t=2251355), and sys-agent PLUS was sampled in the eager boot's title phase — but that phase was spin-poll (delta-n=591 over 2.7 s), where a hold cannot be missed. Banked rule: at parkwatch cadence any input shorter than a full park cycle (~2.7 s) is a coin flip; use holds >= 3 s and confirm sampling via padwatch before judging effect.
- C4 (operational answer): PRIMARY = the pre-cliff drive. The count boot proves the window: title at server t~164 s, menu opened t~209 s, cliff at guest t~576 s — ~6.5 min of ~17 fps flow after menu-open; the full title-to-menu-to-load drive takes well under a minute in flow (each confirm 1-2 frames; count's only flow failure was the dbg-hold latch, avoided by press/release pairs). A flow-phase load also gives the stall-service probe its cleanest ride to the junction. Spec: drive on title with no attract idling (PLUS hold 1.2 s + click; SHORT stick excursions, bright-fraction gate per step; confirms as sys-agent press B with explicit release 1.5 s later x3; verify the 10% save before the final confirm; whole drive inside ~2 min of menu-open against the ~150 guest-s menu timeout). FALLBACK storm recipe: dbg PLUS hold >= 3 s to open; dbg stick steps, gate-verified; confirm as dbg B hold >= 3 s; judge only after two parkwatch resume lines with the button released; if two refuse, cycle one full demo timeout and retry on the returned menu (the count boot's landed shape); never chain a demo-exit tap and a confirm in one script. DECIDING EXPERIMENT (one boot, held in reserve): a dk_diag-gated menuwatch logging at func_80031DDC's post-processing point and at func_800CCBD4 entry (caller LR = which screen, W at 0x80270A70, the post-swap action word, the pending-state word 0x80239060, display-sync snapshot), with two dbg 3 s-hold confirms in two named sub-states: (A) menu freshly storm-opened (probe shape), (B) the same menu after one demo-timeout-and-return cycle (count shape). If (A) refuses and (B) lands, whichever logged word differed IS the gate; if both refuse on this lineage, the build lineage is convicted and the follow-up is a canary-build bisect boot.

## DURABILITY PROVING BOOT (Nov 17, 2026)

One launch of build 23069139 (the standing service) at STOCK clocks
(1020/384/1331.2, readback). Runtime [pgo] 14,006 verbatim. Full log:
DURABILITY-BOOT-LOG.md; stderr-durability.log (2,098,675 B).

Verdict: SERVICE-PARTIAL. The service ran the whole front-end as
designed and the display never flipped. 25 episodes engaged (250
synths, 83 advs, 72 sustain-fails, 24 giveups, 0 adv-dead); 10
sustain passes ALL watch-failed reason=freeze within 2 presents —
zero durable closes; WATCH refused every twitch, exactly as Q3
predicted of the sustain bar. The re-phase primer fired 24 times,
every one on the exact slip signature (arm=1, d4=0, A!=B, flag=1),
cap untouched. Hold v2 cleared 24 times, all reason=fresh — no
deadlock (the drain boot's ~93 s admission freeze does not recur).
Health lines: breakpt moved in every 240-present block (+27..+37);
no 90-second frozen-breakpt stretch anywhere; breakpt crept
106 -> 463 across the run. Parkwatch near-silent (3/3/3); seam-wake
834/834 woke. flag= first direct reads: 886 x flag=1 vs 22 x flag=0
on display-queue sleep snapshots.

The chain completed guest-side (WBLogo/THQLogo/HILogo/RW4/Intro all
read) and the guest aborted at frames=2,802 — the fifth consecutive
abort at the Intro-end position, identical fingerprint
(esr=0x92000006, VA 0xc8a10094). Regime at the crossing, from the
log: CARRIED (zero durable closes; final breakpt=463 vs pe=2,802;
seam-wake firing in the last second). F3 does not trip in its strict
sense — its premise (a durably live regime) has never existed on an
armed boot. F-REPHASE-DEAD / F-REPHASE-RUNAWAY / F-HOLD-STARVE /
F-DRAIN-PARK / F5 do not trip; F-DURABLE-TWITCH does not trip in its
letter (no durable close existed to falsify). Q2 NOT-EXERCISED-
NO-TITLE: PRESS START never visible, menu not driven. Pre-named
routing now applies: bursts advanced A and breakpt crept all run
while the frame never unfreezes — the remaining defect routes to
the display-setup / CopyDisp layer, not the hook. Profraw
fw-1794947637 banked; total now 28. Closeout fully verified
(resident 34585267 restored, crash overwrite preserved at
crash-durability.txt, baseline restored, claim released).

## PROBE RE-RIDE (Nov 17, 2026) — RACE-LOST; probe still unfired

One launch of build 23069139 with the eleven-key probe configuration
(stallsvc=probe + heapwatch + seamfix + fix2 all armed; [dk] tail
verified verbatim; runtime [pgo] 14,006 functions; GPU-307 lottery
clocks — a function boot). Full record:
switch-logs/h3-test/stderr-probe2.log and
~/workspace/no100f-h3-evidence/PROBE2-BOOT-LOG.md.

- RACE-LOST: the first cliff drew at guest t=9,087 ms (breakpt=106,
  the WBLogo drain — the same draw as the durability boot), and with
  seamfix+fix2 armed the display froze on the frozen black frame
  from that moment (two captures byte-identical, 22,161 B). The
  title was never visible; zero menu inputs were sent (padwatch:
  the 4 baseline lines only). The standing service does NOT keep
  the display alive — it holds the advance half only, replicating
  the durability boot. The remaining front-end defect stays routed
  to the display-setup / CopyDisp layer.
- J = NOT-REACHED again: no load, no wedge, zero [stallsvc-probe]
  lines; F-J1..F-J4 remain untested, not failed.
- H = NO TRIP: zero heapwatch trips; 9 logged clean-walk heartbeats
  (alloc 64 / free 18 throughout).
- S census: 22 episodes, 220 synths, 85 advancer deliveries, 64
  sustain-fails, 21 sustain-giveups, 20 sustain passes all
  watch-failed, 0 durable closes, 21 rephase fires, 21 fresh
  hold-clears, 11 health lines; breakpt moved in every 240-present
  block (125 -> 497 final); no 90 s frozen-breakpt stretch; the
  movie chain completed guest-side in order.
- The end: guest abort at frames=2,802 with the identical Intro-end
  fingerprint (esr=0x92000006, VA 0xc8a10094) — the SIXTH consecutive
  abort at that position, in a CARRIED regime (zero durables;
  seam-wake fire #830 in the final second). Record preserved at
  h3-test/crash-probe2.txt; baseline restored byte-exact.
- P: parkwatch 2 signatures / 2 fires / 2 resumes, 0 dry;
  seam-wake 830/830 woke; display queue 897 sleeps / 1,373 wakeups.
- Profraw fw-1794948480 banked (5,640,016 B, sha e3bacd61...);
  banked total now 29, front-end class throughout.
- Next junction attempt, per this boot's readout: combine the
  probe's specced eager regime (seamfix/fix2 OFF — the original
  nine-key shape, where parkwatch resumes keep the chain visible)
  with MENU-CONFIRM-STUDY C4's pre-cliff drive. The service-armed
  shape cannot present a driveable title.
- Closeout fully verified: resident 34585267 restored + read-back
  match; dk_diag deleted/absent; save unchanged; Config untouched;
  console at clean HOME, running=0; claim deleted + verified
  absent; release relays sent to SPM, CTTR and JSRF.


---

## COPYDISP STUDY — the display never unfreezes because the boundary sequence never runs (Nov 17, 2026)

Desk study (COPYDISP-STUDY.md, local evidence folder). Traced the present path end to end and the display module's op dispatcher in the shards. Verdicts:

- **D1 — what is presented post-cliff:** a black display-copy texture, re-resolved and re-presented every frame — NOT a held pre-cliff frame (captures are pure black, luma 0.00; the pre-cliff frame was a logo). Aurora's GXCopyDisp discards the guest destination (`(void)dest`, GXFrameBuffer.cpp:448) and resolves the EFB into a host texture using guest-programmed copy geometry, installed as the present-source override (gx.cpp:1337; latched at seal, aurora.cpp:1887; cleared each frame start, gfx/common.cpp:1360). Post-cliff that geometry is degenerate — src width 0 / height 24000, from the current-mode struct at 0x80271710 — so every resolve is empty. Only three [band] geometry lines exist in the whole durability boot (boot, cliff x2); the [vi] slow-frame `disp 1` deltas prove GXCopyDisp executes once per frame to the abort. The guest's copy destination F8 (0x802716F8) is 0 all post-cliff run, so the VI retrace submission gate (`readyXfb != 0 && readyXfb == currentFb`, vi.cpp ~1624) also never fires again. HLE accepts SetXfbReady(0) silently — a robustness divergence, not a semantic one.
- **D2 — the setup op's gate:** setup (loc_80176204; the pool's only writer) is reached only via the display op dispatcher func_80175FC4 (jump table 0x801EE9DC; NO static callers — front-end state machine via the module interface). Post-cliff it NEVER runs again, proven three ways: pool zero at every sample of every armed boot (1,334/1,334 Fix 2 synths token=0/tableA=0); VIConfigure logged exactly once per boot (apply never re-runs, no [band] restoration line); the movie chain advances via the streaming/DVD callback path (cbs 0x80117F34/0x801126A4) while the display module is re-entered only by the retrace/present cycle, which issues no ops. In healthy flow teardown is always followed by setup within the same boundary sequence (control boot: breakpt 0→4,833, never frozen). Leading gate candidate (theory): the state machine sequences boundary display work on the flag word 0x802716D0, which reads 1 in 886/908 durability sleep snapshots.
- **D3 — candidates:** (a) CLEARED — setup is a state-machine op, not a callback; no deque carries it. (b) CLEARED — VI_HLE_SetXfbReady forces nextFrameBuffer := copy dest on every copy (vi.cpp:2941); VI tracking self-heals on the first nonzero copy. (c) REFUTED in letter, CONFIRMED in substance — the presenter copies to F8 itself (shard lines 22627/22810); no base+B computation; but the dest is a pool word zeroed by teardown and restored only by setup. (d) CLEARED — no present dedup exists; presents are unconditional per copy. The freeze is a GUEST-STATE FACT faithfully presented. The hardware step with no HLE counterpart: the asynchronous draw-done interrupt landing inside the boundary sequence, keeping the teardown → setup sequence runnable.
- **D4 — fix layer + deciding boot:** no hook-layer delivery can re-open a state-machine gate; host-writing the pool is the rejected impersonation class (and teardown freed the block — the addresses are stale). Deciding instrument: ONE dispwatch boot on the durability config — a heapwatch-shaped host sampler (pool words, mode pointer 0x80271710 + its +4/+6/+8 halfwords, config ptr 0x80271708, flag 0x802716D0) plus a runtime log at dispatcher entries (op index + arg). Falsifiers named: F-DISP-1 (setup op entered post-cliff but pool stays zero → op internally gated), F-DISP-2 (mode struct reads 640x480 at the cliff while geometry is degenerate → provenance reopens), F-DISP-3 (dispatcher never entered post-cliff, mode state static → gate convicted; fix is prevention-only). Expected outcome F-DISP-3: the front-end fix is PREVENTION — the held-deque delivery model strands the display state machine at the boundary; the eager regime (control + eager boots) never does. The standing service is a carry for a dead-display regime, not a cure.
- **Abort relationship:** the Intro-end abort is downstream of the STRANDING, not the blackness (5/5 at frame 2,802 in the dead-display regime; never in a generation-alive traversal; its data sits in the display module's own dispatch neighbourhood, five boundaries stale). Unfreezing by restoring the boundary sequence would plausibly clear the 2,802 wall; unfreezing the pixels alone would not.

## DISPWATCH BUILD (Nov 17, 2026) — build 7f6cefd6

COPYDISP-STUDY D4's deciding instrument, built on the durability lineage
(23069139). Full record: DISPWATCH-BUILD-LOG.md (no100f-h3-evidence/).

- NRO 55,916,741 B, sha256 7f6cefd650ffedfa3995117e9b7d778cd7a7bf6cea71e49134d14b4e3cab568a,
  h3-test only; Downloads staging still 53dbb7bb. Backups at
  C:\dev\no100f-nx\dispwatch-backup\ (byte-verified). 23069139 remains
  reproducible from the durability-backup chain plus these backups.
- Instrument, gated by the new dk_diag key dispwatch (SCOOBY_DISPWATCH;
  [dk] tail gains dispwatch=%d; unarmed identical to 23069139):
  (1) a dispatcher-entry log — a prologue call in the generated display
  shard at func_80175FC4's first instruction to a new HLE helper in
  runtime hle/vi.cpp (the dispatcher has exactly one definition and every
  entry path lands on it; a hook in InvokeIndirectCpu would cover only
  the indirect path and tax every indirect call). Each entry logs the op
  index (r3), the argument pointer (r4), and a TryRead32 snapshot of the
  display words. Cap 20,000 logged entries per boot; suppressed entries
  stay counted and the total rides the sampler's steady lines — no
  silent sampling.
  (2) a heapwatch-shaped sampler (250 ms, TryRead-only) change-logging
  the pool words (base/token/F8/P0/P1/fb0), the site-2 flag, the
  current-mode pointer and its +4/+6/+8 geometry halfwords (read only
  through an in-window pointer), and the config pointer, with the
  feeder values; the first sample logs as the baseline; a steady
  heartbeat (at most once per 5 s while frames are frozen) evidences a
  static post-cliff state. Address reconciliation: the shard proves
  F8 = 0x802716F8 (presenter loc_80176C64, r13 − 27528) and
  P0 = 0x802716FC; both are watched.
- Log forms: [dispwatch] op=… / entry log cap reached / change / steady —
  verbatim in the build log, all verified in the NRO by strings.
- Scope: ELF arithmetic __llvm_prf_data 0x0dae80 = 14,014 functions
  (+16 vs base), names +73 B, counters +76; the proving boot's runtime
  [pgo] line remains the final word.
- Proving ride: probe attempt 3 (eager regime) on this build — dk_diag
  ten keys: fifofast / watchdogat=420 / traceio / tracecnt /
  stallsvc=probe / tracewake / parkwatch / padwatch / heapwatch /
  dispwatch (seamfix and fix2 ABSENT); expected [dk] tail:
  stallsvc=probe tracewake=1 parkwatch=1 seamfix=0 padwatch=1 fix2=0
  heapwatch=1 dispwatch=1. Readout per COPYDISP-STUDY D4.2: dispatcher
  entries per movie boundary with op indices, the mode pointer/struct
  fields across the cliff instant, and the flag's value at each
  boundary; falsifiers F-DISP-1/2/3 as specced there. NOT boot-tested.

## PROBE ATTEMPT 3 — the combined ride (Nov 17, 2026)

One launch of build 7f6cefd6 (dispwatch build, durability base) in the
EAGER regime (seamfix/fix2 absent) with stallsvc=probe + heapwatch +
dispwatch armed (ten dk_diag keys, read back verbatim). Runtime [pgo]
14,010 functions (desk 14,014; runtime governs). STOCK clocks. Full
record: PROBE3-BOOT-LOG.md; stderr-probe3.log (6,155,954 B, sha
5ac0b0cf…).

DRIVE: the primary pre-cliff window never opened — the generation
cliff drew at guest t≈34.9 s (breakpt froze at 713, THQLogo). The C4
storm FALLBACK landed the whole drive at parkwatch cadence (~1.1 fps,
chain visible throughout, no 2,802 abort): B-tap demo exit, two dbg
PLUS holds (3.5 s) to the menu, brightfrac gate NG 0.0483 at open,
stick excursion pair (+Y/−Y, 3.5 s holds, padwatch-sampled) to LG
0.0205, and three dbg B-hold (4 s) confirms — slot, file list
(capture-verified "11/14/26 10% The Mystery Machine" on top), load —
ALL first try. The storm recipe is proven end to end against attempt
1's six refusals.

WEDGE + PROBE: the load's card scan completed (ScoobyDoo00 -> 0, then
01–07 -> -4), h001.HIP never issued, frames froze at 3318 (retrace
142638, ai_cb 475454, enqueues 18224). The probe episode opened
(t=2,379,638 ms; flag 0x80271020 = 0, A=1 B=0 arm=0 latch=0). Dose 1
(one retrace pair) and dose 2 (one audio poll): delivered, NO WAKE in
either 10 s window (frames/enqueues/flag static). Dose 3: retrace
pairs #2–#6 at 2 s intervals; after pair #6 the guest CRASHED — host
data abort esr=0x92000005, guest VA 0x389dfff4, x24=0x389dfff0 (the
heap free-list walk; record preserved at h3-test/crash-probe3.txt).
VERDICT J = F-J1: a crash carrying 0x389dfff4 inside the dose
programme — foreign-thread delivery is convicted at probe scale and
the service-thread delivery family closes permanently (v2, burst,
probe: three for three). Dose 3 never reached full dose (6/8 pairs,
1/16 polls), so F-J2 is NOT established; no wake occurred, so
F-J3/F-J4 do not apply. The flag never moved at any dose.

DISPWATCH (W): 9 dispatcher entries ALL BOOT, all at t≤5,397 ms
(frames 0–2), op indices 4, 0, 11, 10, 6, 2, 17, 10, 6; the pool is
installed between the op=0 entry (t=1 ms) and the op=11 entry
(t=158 ms) — setup ran exactly once, at init. ZERO entries post-cliff
or at any boundary. Sampler (954 change + 459 steady): mode
0x80270168, modehw [640,480,480] in every post-init line; the pool
NEVER zeroes post-init — at the cliff instant it rotates healthily
(80934540/809CA540) with the flag toggling (1 in 1,340 of 1,422
lines). F-DISP-3's letter confirmed (never entered post-cliff, mode
static) WITH the caveat that this is also the healthy shape: the
dispatcher is an init-only surface, so the armed boots' defect is
the teardown zeroing with no restore path, not a missing op call.
F-DISP-1 no; F-DISP-2 no.

HEAPWATCH (H): NO TRIP — 464 clean walks, including one inside the
episode (t=2,406,028 ms, between dose-3 pairs #5/#6): the cell was
planted and hit inside pair #6's delivery chain, between sampler
walks; the F-J1 conviction rests on the crash clause as written.

PARKWATCH (P): 868 signatures / 868 fires / 868 resumes / 0 dry.
[gxcnt] final breakpt=713, pe47=pe48=3318.

PROFRAW: fw-1794950215 (5,640,664 B, sha 1c6a32f8…) banked to pgo\
and h3-test — the bank's first junction-class profraw with a landed
load. Banked total now 30.

CLOSEOUT: resident 34585267 restored + read-back verified; dk_diag
deleted (raw FTP DELE; curl -X DELE downloads instead — noted);
crash.txt overwrite preserved + baseline restored byte-exact; save
unchanged; Config untouched; console clean HOME, running=0; claim
deleted + listing-verified absent; relays sent (CTTR A/B retry next).

## EXECUTOR-SAFEPOINT DESIGN (Nov 17, 2026; desk study, SAFEPOINT-DESIGN.md)

Commissioned after PROBE ATTEMPT 3's F-J1 closed the service-thread delivery
family permanently (dose 3 died on 0x389dfff4 inside pair #6's chain; a
heapwatch walk inside the episode was clean — the cell was planted and hit
within one delivery). The study designs the one remaining cure shape: an
executor safepoint, the shape JUNCTION-CALLBACK-DESIGN J4.1 named sound.

PLACEMENT (S1): the translator's backward-branch emission. At a back-edge
closing a CALL-FREE cycle (decidable at emission by scanning the function's
instruction list between target and branch for any call-lowering instruction;
the generator already tracks loc_ labels as first-class data and runs
per-function analysis passes), emit
`if (GuestSafepointRequested()) GuestSafepointServe(ctx, <target>);` before
the goto. Pivotal simplification: NO register write-back/reload brackets the
call — the delivery entries seed their private GuestInterruptCallbackContext
from the ambient ctx's r1/r2/r13/FP mode, which are ABI-invariant inside a
function body (the generator's own addressing assumes r13 stability), and the
spinner's other registers are host C++ locals preserved across an ordinary
host call; the delivery never writes the spinner's ctx. The seed is no staler
than the one VI_HLE_ProcessRetracesDeferred already uses from mid-function
callers in healthy flow. Cost when the flag is clear: one relaxed atomic load
+ one predicted branch per qualifying loop iteration, zero elsewhere;
frame-level expectation sub-1% to ~2% (THEORY — A/B owed as F-SP3);
countdown amortisation (serve every 1024th trip) is the priced refinement if
it bites. Runtime-only placements CANNOT deliver: there is no block-dispatch
loop (direct calls are emitted host calls; indirect calls resolve via the
registry and call directly), and the one surface executing inside the spin —
the FORCE_INLINE memory helpers (FlatRead32, memory_access.h:587) — has no
ctx, cannot reach the caller's host locals, and has undefined resumption; it
is a detection surface only, and the gate already detects for free. Existing
interrupt machinery (DeliverPendingInterruptsOnEnable, os_interrupt.cpp:82 —
the runtime's own statement of the root defect) fires only at interrupt
re-enable edges, which func_800D8164's two call-free cycles never reach
(shard-verified: load + compare + goto only, at loc_800D859C/85A4 and
loc_800D85B4/85BC); the safepoint is that proven delivery point generalised
to loops that never toggle interrupts. Translator route requires
REGENERATION (build-switch.sh only compiles the pre-generated tree;
tools\build-no100f.ps1 runs the translate step) plus a full shard-diff audit.

DELIVERY (S2): GuestSafepointServe(ctx, target) runs on the scheduler thread
from inside the spinning thread's own code. It re-validates the witnesses
against the gate's episode bases before every dose (movement = benign
wake-before-dose; also refuses on heapwatch trip via the existing
HeapWatchTripped() atomic, process exit, or interrupts disabled), delivers
under OS_HLE_BeginDeferredGuestCallbacks (the scheduler-disable counter,
os_alarm.cpp:442 — no fiber switch can occur inside the delivery;
WakeupThreadQueue's effects land as pending-mask state honoured at the
guest's own next switch point), and runs the PROBE'S dose programme at the
guest's cadence: dose 1 = one VI_HLE_ProcessRetracesDeferred(1), >=1 s
observation, dose 2 = one Audio_HLE_PollDeferred(), then multiplicity at a
100 ms rate limit to the probe's caps (8 pairs / 16 polls cumulative).
Alarms excluded (queue empty in every junction observation; the named
reopener if F-SP2 fires). On a wake the released transition code runs only
after the handler has returned in full — delivery and released execution
are strictly ordered on one thread, the property v2 lacked.

TRIGGER (S3): the probe's gate UNCHANGED (four-feeder silence >=2000 ms +
frame-worker idle + work due + spinner signature state==2/queue==0 on two
consecutive ticks + heapwatch armed), except the service thread SETS an
atomic request and records the episode bases instead of delivering — it
executes no guest code (count-mode discipline). The handler verifies the
tripping thread is the gate's spinner (GetCurrentGuestThread) and, in the
proving build, that the back-edge target is one of the two convicted poll
cycles (0x800D859C / 0x800D85B4). False-positive cost, priced: if the gate
ever misfires during a long call-free compute, interrupts-disabled doses
no-op inside the delivery entries (hardware would pend; the re-enable edge
delivers), and interrupts-enabled doses are one faithful early interrupt
set on the thread's own stream — the same exposure as
DeliverPendingInterruptsOnEnable at every re-enable and the control boot's
4,832 restore-point wakeups (99.1% inrestore). Useless, not corrupting;
every request is logged with feeder values so any such trip is auditable
(F-SP4).

LEGALITY (S4): the single-executor invariant holds on every point —
scheduler-owned stream (the check runs in the spinner's own translated
code; the service thread only sets an atomic), scheduler-owned context
(ambient scope binds the spinner's own ctx; callbacks run on the private
copy, as everywhere in this runtime), defined point (the emitted back-edge;
guest memory coherent there, deferred scope held), no concurrency (wake
and released execution strictly sequential on one thread), re-entrancy =
the healthy nesting, plus one new guard (a thread_local in-delivery flag
so a callback entering its own call-free spin cannot nest a second dose).
J4's four grounds answered mechanically: (1) no preemption point needed —
the point is created in the code the spinner already executes; (2) the
context objection targeted a foreign thread reading the thread record —
nothing reads the record; the executor itself publishes the state the
delivery consumes; (3) executor state outside the register file — the
host call stack is the spinner's own (the handler is a nested call on
it), the fiber record is untouched, the ambient scope binds correctly
on this thread, and the audio tick's thread_local cursor is THIS
thread's cursor, the one all pumps share (audio.cpp:538) — the v2 desync
cannot occur; (4) success cannot create the fatal configuration because
the spinner is inside the handler call when the flag flips and cannot
run transition code until the delivery has fully returned.

ROUTES (S5): Route A = translator emission + regeneration + shard-diff
audit + whole-tree rebuild (days-class; the general fix). Route B =
hand-emit the same two checks into func_800D8164's two back-edges in
shard_9e6d0891680e24b492f4994a.cpp + the identical runtime handler/gate
work (hours-class for the emission; build-switch.sh compiles shards
as-is, NO regeneration; the edit must be carried as a documented patch
and covers exactly one poll). RECOMMENDED ORDER: B to prove, A to ship —
Route B's runtime is Route A's runtime, proven early. SEPARATELY, the
default-line capture experiment is worth ONE console slot BEFORE either
route: a --variant pgo (FW_PGO_ALL) capture build on the default line
ridden through the transition, zero new code. Prior stated plainly: it
probably wedges (trains 1-3 wedged on full-scope 62e8a8eb before any
delivery machinery existed; the trim wedged identically) — but a
play-through would mean the wedge is a property of the trim lineage's
accumulated state, not instrumentation, and PGO training (route
coverage -> USE build) proceeds WITHOUT the safepoint; a wedge confirms
the safepoint as the only road and settles its proving config. If the
safepoint proves: it unlocks route coverage on capture builds (the
30-profraw bank is front-end/junction class only and must never train a
USE build alone), and, the root defect being runtime-wide (interrupts
delivered only from guest activity), it belongs in the DEFAULT build
after proving, armed by a gate that cannot open in healthy flow.

FALSIFIERS (S6), named in advance: F-SP1 a safepoint delivery crashes
on 0x389dfff4 (the executor model reopens below executor count); F-SP2
full dose delivered at safepoints, counters moving, spinner never
exits, flag never flips (the wait needs a class neither feeder provides
even legally; J3 reopens with legal-executor per-class data); F-SP3 the
check itself regresses the standard route beyond noise with the flag
never set (countdown/narrower classification is the next iteration;
Route A does not ship until priced away); F-SP4 a false-positive trip
corrupts an active-work state (one occurrence ends the gate's
authority); F-SP5 the trip never fires at a confirmed wedge
(emission-coverage failure — the scan-back classification is convicted,
not the mechanism). PROVING SHAPE: Route B build on the pgo-trim line,
dk_diag key `safepoint` (env SCOOBY_SAFEPOINT; unarmed = behaviourally
identical — the gate never sets the request), eager regime, heapwatch +
tracecnt riding; one launch: eager ride -> C4 drive -> landed load ->
wedge -> gate sets request -> spinner trips within one loop iteration
-> dose programme -> full-cure witness h001.HIP issuing -> route if
gameplay is reached. Second arm when console time allows: the F-SP3
A/B on a non-instrumented emission build vs resident route numbers —
which is also Route A's own proving boot.

## DEFAULT-LINE CAPTURE BUILD (Nov 17, 2026)

Build **152b4f63** (55,994,565 B, sha256 152b4f6375613ccebf600faa10db776da2dacf5213f684f152e6456afe301699), the SAFEPOINT-DESIGN S5 experiment: a `--variant pgo` capture build (FW_PGO_ALL=ON, FW_PGO_TRIM_AURORA=OFF) on the default line — the last untested cell in the instrumentation x lineage matrix. Cold build-deko-pgo (the dir was deleted in the Nov 16 cleanup), tree compiled exactly as it stands after the dispwatch build (all machinery aboard, unarmed-inert in the proving config), zero new code. Desk scope: 14,211 functions / 560,601 counters (ELF arithmetic; the boot's runtime [pgo] line governs). Staged to h3-test ONLY; Downloads staging still holds 53dbb7bb. Full record: CAPTURE-DEFAULT-BUILD-LOG.md (VM workspace, no100f-h3-evidence/), including the proving-boot spec: seven eager dk_diag keys (fifofast / watchdogat=420 / traceio / tracecnt / tracewake / parkwatch / padwatch), expected [dk] tail "stallsvc=off tracewake=1 parkwatch=1 seamfix=0 padwatch=1 fix2=0 heapwatch=0 dispwatch=0", and the S5 decision rule — h001.HIP issues + spawn reached = the default line PLAYS THROUGH (route runs; PGO proceeds without the safepoint); card scan completes + h001.HIP never issues + guest spinning = it WEDGES (watchdog dump + tracecnt readout are the evidence; the safepoint is confirmed as the only road). NOT boot-tested.

## DEFAULT-LINE CAPTURE BOOT (Nov 17, 2026) — VERDICT: WEDGES

One launch of build 152b4f63 (FW_PGO_ALL full-scope capture, default
line, zero new code) in the eager regime (seven dk_diag keys:
fifofast / watchdogat=420 / traceio / tracecnt / tracewake /
parkwatch / padwatch), ridden through the save-load transition per
SAFEPOINT-DESIGN S5's experiment. Full record:
CAPTURE-DEFAULT-BOOT-LOG.md; stderr-capturedefault.log
(5,883,668 B, sha 85405e8c…).

- Boot facts: [dk] tail as specced (stallsvc=off … dispwatch=0);
  clocks STOCK 1020/384/1331.2; runtime [pgo] line verbatim
  "4380 KiB of counters, 14211 functions" (matches desk
  arithmetic exactly).
- Front-end: flow ~14 fps for the first ~30 guest-s; cliff by
  guest t≈30 s (breakpt frozen at 409 all run); storm cadence
  ~1.1 fps, chain visible throughout; crossed the Intro end
  (frame 2,802) with NO abort; parkwatch 931 fires / 931
  resumes / 0 dry.
- Drive: the pre-cliff window never existed; the C4 storm
  fallback landed the full load (all three confirms first try;
  file list capture-verified "11/14/26 10% The Mystery
  Machine"). Recipe note: separate-connection dbg `press` does
  not persist button state on this lineage (two PLUS holds,
  zero padwatch samples) — the dbg `hold` form with
  padwatch-verified releases is the working opener.
- Wedge: load confirm at guest t≈2,529 s; card scan completed
  (ScoobyDoo00 -> 0 ×3 load context, probes through ScoobyDoo07
  failing at log line 62902); last IO delivery the stream
  completer 0x800DA71C (lines 62907–62908); first frozen [cnt]
  at line 62909 (t=2,534,412 ms, frames=3203); counters frozen
  identically to the log's end (frames=3203, retrace=152048,
  ai_cb=506817, guest_ok=1); h001.HIP 0 occurrences in the full
  log; LOADING capture byte-identical across ~9 min of
  observation; clean HOME close (the close left the known
  wedged-exit error dialog, dismissed under claim).
- Decision (S5's rule): the wedge is owned by instrumentation
  itself, not by scope (trim wedged identically) and not by
  lineage state (a zero-new-code default-line build wedged on
  the current tree). The instrumentation × lineage matrix is
  complete. The executor safepoint is confirmed as the only
  road to route coverage; its proving configuration is this
  boot's (default-line capture, eager, seven keys).
- Profraw fw-1794954735 banked (5,665,960 B, sha 6db71fe5…;
  pgo\ + h3-test, hash-verified): front-end + title/menu + full
  load + junction wedge, first default-line profraw. Banked
  total: 31.
- Closeout verified: resident 34585267 restored (read-back
  match), dk_diag deleted/absent (listing-verified), crash.txt
  NOT overwritten (baseline 9d83c322…; the guest wedged, never
  crashed), save unchanged, Config untouched, claim deleted +
  verified absent, console at clean HOME, relays sent.

## SAFEPOINT ROUTE B BUILD (Nov 17, 2026)

Build **3336409f** (55,924,933 B, sha256
`3336409f320c661b5b510eb4b01e983a36d8d274d97efe5c81ccd034f8de8fb6`), on the
pgo-trim lineage over base 7f6cefd6 (dispwatch), staged in h3-test only
(Downloads staging still 53dbb7bb). SAFEPOINT-DESIGN S1–S4 implemented as
Route B: the proving step that hand-emits the executor-safepoint checks
instead of regenerating. Commissioned after the default-line capture boot's
WEDGES verdict (CAPTURE-DEFAULT-BOOT-LOG) confirmed the safepoint as the
only road to route coverage. Full record: SAFEPOINT-BUILD-LOG.md.

- EMISSION (hand patch, shard_9e6d): at func_800D8164's two call-free
  back-edges, immediately before each goto —
  `if (GuestSafepointRequested()) GuestSafepointServe(ctx, 0x800D859Cu);`
  (wait-clear, loc_800D85A4) and the same with 0x800D85B4u (wait-set,
  loc_800D85BC) — plus the two extern "C" declarations above the function
  (the dispwatch style). No register bracketing (study S1). HAND LINES IN A
  GENERATED FILE, flagged as the dispwatch build flagged its own: a
  regeneration drops them; the reapplication record is
  safepoint-backup/shard-safepoint.patch (the exact three-hunk diff).
- RUNTIME (hle/vi.cpp, additions only — 438 lines, 0 removed): the request
  atomic + episode bases at file scope; GuestSafepointServe, the handler on
  the spinner's own thread — identity checks (tripping thread ==
  0x8026D568; target in {0x800D859C, 0x800D85B4}, logged once, request
  stands), a thread_local in-delivery guard against nested doses,
  stop checks (heapwatch trip → abort + permanent suppression; process
  exit → close), witness checks against the gate's bases before and after
  every dose (frames / enqueues / flag 0x80271020), the probe's dose
  programme at guest cadence (dose 1 one retrace pair, ≥1 s observation,
  dose 2 one audio poll, ≥1 s, then multiplicity at the 100 ms rate limit
  to cumulative 8 pairs / 16 polls; alarms excluded), delivery under the
  deferred-callback scope, refusal-with-close if interrupts are disabled
  at the delivery point. Caps with no wake log the F-SP2-shaped no-wake
  line.
- GATE (stall-service thread): the probe's gate replicated condition for
  condition PLUS heapwatch armed (refusal logged once, never requests)
  PLUS the spinner signature (state==2, queue 0x0, two consecutive ticks) —
  but on completion it SETS the request atomic and records the episode
  bases instead of delivering; it executes no guest code. One episode per
  boot, never re-arms; a second silence window after a closed episode is
  logged and ignored. New dk_diag key `safepoint` (SCOOBY_SAFEPOINT;
  [dk] tail gains safepoint=%d after dispwatch).
- UNARMED, stated honestly: with no key the gate never opens and the
  handler never serves — behaviourally identical to 7f6cefd6 — but the
  two checks are compiled into the shard regardless (one relaxed atomic
  load + predicted branch per poll-loop iteration); NOT byte-identical at
  the shard level, and the check-free control arm for F-SP3 is the
  existing probe-lineage build.
- Untouched per the semantic diff: all stallsvc modes, seam/seam-wake/
  Fix 2/drain/durability, parkwatch, heapwatch + trip wire, dispwatch,
  tracecnt. Backups byte-verified at C:\dev\no100f-nx\safepoint-backup\.
  Scope (desk arithmetic; runtime governs): 14,016 functions (+2 vs
  base), +161 counters.
- PROVING BOOT (the parent's): one launch, eager regime, nine dk_diag
  keys (fifofast / watchdogat=420 / traceio / tracecnt / tracewake /
  parkwatch / padwatch / heapwatch / safepoint; stallsvc, seamfix, fix2,
  dispwatch ABSENT); expected [dk] tail "stallsvc=off tracewake=1
  parkwatch=1 seamfix=0 padwatch=1 fix2=0 heapwatch=1 dispwatch=0
  safepoint=1". Shape per study S6: eager ride → C4 drive → landed load →
  wedge → [safepoint] request → spinner trips → dose programme → h001.HIP
  full-cure witness → standard route if gameplay is reached. Falsifiers
  F-SP1..F-SP5 as named in the study (a crash on 0x389dfff4 is F-SP1, not
  a generic failure). NOT boot-tested.

## SAFEPOINT PROVING BOOT (Nov 17, 2026)

One launch of build 3336409f (Route B safepoint; nine dk_diag keys:
fifofast / watchdogat=420 / traceio / tracecnt / tracewake /
parkwatch / padwatch / heapwatch / safepoint; eager regime).
Verdict **F-SP5 — the trip never fired at a confirmed wedge.**
The load landed (the drive took the long way: generation stayed
alive to frame 10,757 before the cliff; the menu excursion pair
was found to desync highlight vs confirm index in flow — banked
in SAFEPOINT-BOOT-LOG.md — and a spaced single +Y step re-synced
it). The wedge formed in the classic pattern (card scan complete,
first frozen [cnt] t=992,436 ms, frames=11229, retrace=59525,
ai_cb=198421, guest_ok=1). The gate opened on feeder silence +
the spinner signature and logged `[safepoint] request
t=994415ms frames=11229 retrace=59525 ai_cb=198421
enqueues=42292 flag=0x00000000 A=1 B=0 arm=0 latch=0` — and that
is the ONLY [safepoint] line in the 118,085-line log: no dose, no
obs, no wake, no trip-ignored. h001.HIP never issued. Per S6,
F-SP5 convicts the hand-emission's placement (emission
coverage), NOT the mechanism — the handler and dose programme
were never exercised. F-SP1/F-SP2/F-SP4 did not trip; heapwatch
never tripped (clean walks span the wedge); no crash (crash.txt
baseline intact). Parkwatch 157/157/157, 0 dry. Runtime [pgo]
14,014 functions (desk said 14,016; runtime governs). Clocks
GPU-307 lottery (function boot). Profraw fw-1794958842 banked
(5,641,736 B, sha 4ace89cd…); banked total 32. Closeout fully
verified (resident 34585267 restored, dk_diag absent, claim
deleted, relays sent). Full record:
~/workspace/no100f-h3-evidence/SAFEPOINT-BOOT-LOG.md. Open: the
spinner's actual cycle in this wedge was never PC-sampled — a
wedge-time PC instrument or Route A's structural coverage are
the two named next steps, the parent chat's call.


---

## SPINNER-PC STUDY (Nov 17, 2026) — full text: ~/workspace/no100f-h3-evidence/SPINNER-PC-STUDY.md

Desk study after the safepoint boot's F-SP5 (gate fired correctly at a classic
A+B wedge; zero trips; the spinner never passed either instrumented back-edge).
All guest code read in full from the generated shards; a widened whole-tree scan
(3,373 backward-goto-with-load rows) supplied the candidate space.

Q1 — func_800D8164, whole: cycle census = bounded chunk loops A (loc_8210←8294)
and B (loc_82A0←8398) with calls, bounded call-free C (loc_83E4←841C) and D
(loc_8434←8494), the outer chunk loop E (loc_84CC←85B0), the instrumented
wait-clear G (loc_859C←85A4) and H (loc_85B4←85BC), plus ONE MISSED WAIT: cycle F
(loc_800D8540 ← loc_800D852C), a call-CONTAINING flag wait (per iteration:
func_800166C0 engine/VI step, func_800441D0 state apply, conditionally
func_800AC8E0) that the call-free scan could never see. Corrections to the
inherited record: (1) cycle H is NOT a wait-set — the CR idiom (cr & 0x20000000
== EQ bit, cross-checked against func_80044F7C's alloc-null branch) makes it
loop while flag != 0, wait-clear-shaped like G; no wait-set phase exists in the
function. (2) No cycle iterates on the counter 0x8027101C (read at entry and at
loc_8420, incremented once at loc_85C0 on exit). (3) Flag writers, complete
census: set (|=1) func_800D8164 ×2 and func_800D9D24 ×1; clear (=0) the bare
callbacks func_800D85E0 and func_800DA7F0; no others. (4) The caller
func_80044F7C has NO backward branch — one call to func_800D8164, no retry, no
caller-side spin. VERDICT: every unbounded wait in func_800D8164 spins only
while flag 0x80271020 != 0, and the safepoint gate logged flag = 0x00000000 at
the wedge; the bounded loops are excluded by a duration bound (32-bit count vs
≥247 guest-seconds of freeze). THE SAFEPOINT BOOT'S SPINNER IS NOT IN
func_800D8164. The J3 identification is dead as a universal answer (it remains
possible for other boots); its own stated residual gap — no boot ever sampled
the spinner's PC — is now the operative fact.

Q2 — widened re-scan (any addressing form; cycles with exactly one call;
stream/sound family + the completer cluster's module), ranked by callback-class
writers / load-tree reachability / wedged-value checkability:
(1) func_800D875C — delay wait on stream counter 0x80271024 to a target of 140:
entry resets the counter to 0; inner call-free cycle loc_8784←878C spins while
word < word-at-entry + 14; outer loc_8798←87A0 steps via func_800D90AC. Callers:
func_800CFD14 (retrace housekeeping) and func_800CEF98. The counter's only
advancer is func_800D9EF8 (entry store of its argument), a callback-dispatched
stream function with NO static callers. Wedged prediction: counter frozen in
[0,140), flag = 0 — consistent with the safepoint log; NO existing instrument
reads this word (the gate does not). Emission cost 2 back-edges.
(2) func_800D85EC — the same delay shape to a target of 400 (reset at
loc_800D8680; inner loc_86AC←86B4; outer loc_8708←8718; step call func_800D90AC
at loc_86B8); only caller func_800CFD14. Same advancer, same prediction with
bound 400. Emission cost 2. Candidates 1+2 are one mechanism, two instances.
(3) func_800D9D24 @ loc_9E84←9E8C — an uninstrumented call-free poll on flag
0x80271020 (sets it |=1 itself, AX kick pair, same CR idiom); only caller
func_8003D5DC. Refuted for the safepoint boot (flag = 0); live for any flag = 1
wedge. Emission cost 1.
(4) func_800D8164 cycle F @ loc_852C — priced in Q1; refuted for this boot.
Emission cost 1. Total B-2 set: 6 back-edges, 4 functions.
Excluded on the record: func_800DA63C (contains loc_800DA71C, the last pre-wedge
delivery) has NO cycle — read in full; func_800D9EF8 has no qualifying cycle (it
is the advancer); the frontend screen family (func_800CB184/CC2E0/CC920 polling
0x80270F28 via the frame helper func_800CCBD4) is excluded by reachability (the
transition has left the frontend before the wedge); the object-relative scan
space is all self-advancing/self-written bounded walks.

Q3 — SPINWATCH (the deciding instrument; buildable cheaply from existing
mechanism): (i) NO readable live guest PC exists — CpuContext has no live PC
for translated code; the fiber record's context is the at-last-switch snapshot
(the trim dump's PC=0); RecompMod::CurrentTranslatedExecutionAddress() is
thread_local and written only on the indirect-dispatch path
(recomp_mod_loader.cpp:357), so it is neither cross-thread nor current in a
direct-call spin. (ii) The crash reporter's stack resolution
(DumpGuestStack/DumpGuestStackHints) consumes the SAVED context and is PROVEN
unable to name a spinner — the trim boot's wedge dump was byte-identical to
that boot's healthy dump (stale scheduler-stack residue; the campaign's own
standing judgment). (iii) The watchdog path inherits that limit in full. THE
WORKING MECHANISM ALREADY SHIPS: GuestProfiler's Switch sampler pauses the
game thread (svcSetThreadActivity), reads PC+LR (svcGetThreadContext3), resumes
— ~1000 samples/s, no heap use while paused, host PC mapped to the translated
guest function via the TranslatedFunctionRegistry (kind != Native excluded,
translated-range bound, raw elf+offset retained for addr2line). SPINWATCH =
that sampler re-pointed: dk_diag key `spinwatch`; trigger = the safepoint
gate's open (same evaluation, unchanged); game-thread Handle captured at
presents while armed (OnFramePresented hook); ONE shot of 64 samples at 10 ms
from a detached thread; witness words read host-side (TryRead32) immediately
before/after the shot — flag 0x80271020, counter 0x80271024, op counter
0x8027101C, A/B/arm/latch; modal PC function reported with sample counts, LR
function, minorities, and raw elf+offsets. Executes no guest code, takes no
guest lock across the walk. Failure modes stated in the study: nearest-entry
misattribution (mitigated by kind-exclusion + range bound + modal vote +
elf+offset), function-level naming only (cycle = desk follow-up via Q2's
census), HLE-code modal PC is itself a verdict (F-SW2), uncaptured handle and
SVC failure are named log paths, not silent ones.

Q4 — emission decision: Route B-2 = 6 back-edges / 4 functions / hours-class,
but its coverage argument is the same elimination method that just failed once,
and its top candidates fire the safepoint INSIDE AdvanceRetrace's callback
chain — delivering the dose's retrace pair from inside a retrace callback is a
new legality claim (s_inAdvanceRetrace guard; v2 recorded one nested retrace
escaping it) that needs its own walk against the named context. Route A =
structural coverage for call-free cycles (would have caught candidates 1–3
unnamed) at days-class cost, and still does NOT cover cycle F (non-leaf calls)
— stated, not discovered later. RECOMMENDATION: SPINWATCH FIRST, riding the
next build alongside the existing Route B checks on the same gate — one boot
returns the spinner's name, the counter's wedged value, and (if the name is
func_800D8164 after all) the first evidence about the compiled check itself;
then B-2 into a NAMED set with the callback-context legality walk done against
that context; Route A to ship. Falsifiers named for the next boot: F-SW1
(spinwatch names a function outside the candidate set — the set reopens with
its name in hand), F-SW2 (modal PC in runtime/HLE code — the poll model
reopens), F-SW3 (spinwatch names func_800D8164/9D24 with the request standing
and no trip — the compiled check is convicted; disassemble the built shard
before any new emission), F-SW4 (counter 0x80271024 reads ≥400 or moves between
the gate-open reads — the delay model dies); F-SP1/F-SP2 govern if a trip
fires and the dose programme runs.

## SPINWATCH BUILD (Nov 17, 2026)

Build **30082a0e** (55,941,317 B, sha256 `30082a0ebebb797b4b4b21a7f131dc8f95fd4e466f2b2c266fa3853aed436a24`), pgo-trim capture line on the safepoint base 3336409f, staged to switch-logs/h3-test only (Downloads staging still 53dbb7bb). Implements SPINNER-PC-STUDY Q3: the GuestProfiler Switch pause-and-sample mechanism (svcSetThreadActivity pause, svcGetThreadContext3 PC/LR, registry resolution with Native kinds excluded) extracted into three shared helpers in guest_profiler.cpp (SpinwatchSampleGameThread / SpinwatchResolve / SpinwatchGameThreadHandle — the profiler's sampler lived in an anonymous namespace and could not be called from vi.cpp, stated plainly in the build log), re-pointed at the safepoint gate's open. The game-thread handle is captured at the first present inside GuestProfiler::OnFramePresented while SCOOBY_SPINWATCH is armed (the presenting thread is the game/scheduler thread — the profiler's own identity; the tree records no main-thread handle anywhere else). At the gate's request point (gate conditions, request and handler byte-untouched; a one-shot latch beside the request store), SpinwatchGateOpen logs the gate witnesses plus stream counter 0x80271024 and op counter 0x8027101C, names a missing handle instead of skipping silently, and spawns one detached shot thread: 250 ms settle, bracketing host-side witness reads (flag 0x80271020 / counter 0x80271024 / op 0x8027101C / TraceWakeSync display words, never while paused), 64 samples at 10 ms spacing into a caller-owned buffer (no allocation; the sampler executes no guest code and takes no guest lock), then the modal PC function + modal LR + minorities (>=4 samples, up to 3) resolved exactly as the profiler's Switch report resolves. New dk_diag key `spinwatch` (SCOOBY_SPINWATCH; [dk] tail gains spinwatch=%d after safepoint=%d). Semantic diff additions-only apart from the two [dk] plumbing lines; the shard checks, gate/handler, all stallsvc modes, seam/Fix 2/drain machinery, parkwatch, heapwatch + trip wire, dispwatch and tracecnt are untouched (diff-verified; log forms spot-verified in the NRO). Desk scope arithmetic: 14,030 functions / 559,945 counters (runtime [pgo] governs). One build fix recorded: the first pass failed on an unclosed anonymous namespace in guest_profiler.cpp (the edit anchor consumed its close line); a one-line sha-asserted fix restored it and the resume completed clean (BUILD_EXIT=0). Proving boot specced in SPINWATCH-BUILD-LOG.md (ten eager keys incl. safepoint + spinwatch + heapwatch; expected [dk] tail "stallsvc=off tracewake=1 parkwatch=1 seamfix=0 padwatch=1 fix2=0 heapwatch=1 dispwatch=0 safepoint=1 spinwatch=1"): the boot's primary deliverable is the spinner's NAME plus the counter's wedged value; falsifiers F-SW1..F-SW4 with F-SP1/F-SP2 carried. NOT boot-tested; the boot is the parent's.

## SPINWATCH PROVING BOOT (Nov 17, 2026)

One launch of build 30082a0e (spinwatch on the safepoint base) in
the eager regime, ten dk_diag keys (safepoint + spinwatch +
heapwatch armed), STOCK clocks, runtime [pgo] 14,030 verbatim.
Full record: SPINWATCH-BOOT-LOG.md; stderr-spinwatch.log
(5,169,953 B, sha 8a08b4b5...).

Verdict SPINNER-NAMED: the generation cliff drew at guest
t=11.4 s (breakpt frozen at 164); parkwatch carried the front-end
at ~1.11 fps (879/879 fires, 0 dry) through the whole logo chain
and the Intro — and at frame 2,802, the Intro-end frame, the
guest entered a full spin (retrace + ai_cb frozen together at
142,981 / 476,582, enqueues 15,260). The wedge is at the
Intro-to-title TRANSITION: the title never existed guest-side,
the load was never driven, and no input was sent after the
launch press until the closeout. The gate opened at
t=2,384,911 ms (flag=0, A=1 B=0 arm=0 latch=0) and the shot
fired on the same tick:

[spinwatch] pc fn=0x800D2328 fn_800D2328 samples=64/64
lr fn=0x00000000 pc_raw=elf+0xF776C8 lr_raw=elf+0x0;
witnesses: flag 0x80271020 = 0, counter 0x80271024 = 476,576
(frozen, identical pre/post), op7101C = 3. No minorities, no
failed/no-handle line.

Falsifiers: F-SW1 TRIPS (func_800D2328 is outside the candidate
set {800D875C, 800D85EC, 800D9D24, 800D8164} — the set reopens
with its name in hand). F-SW4 TRIPS in its >=400 arm (476,576,
frozen — a spinner inside either delay wait would have reset
the counter to 0 at entry and frozen below 140/400; the
delay-wait model is dead for this wedge). F-SW2 no (translated
guest code named). F-SW3 no. F-SP1/F-SP2 no (no dose ever ran).
No Route B trip fired — exactly one [safepoint] line in the log
(the request), which stood unconsumed ~388 guest-seconds; the
handler remains unexercised. Heapwatch never tripped. crash.txt
not overwritten. Profraw fw-1794962048 banked (front-end +
Intro-end transition wedge + spinwatch shot); banked total 33.
Closeout fully verified; resident 34585267 restored.

Open: whether the post-load junction wedge spins in the same
function is NOT established (the load was never reached; the
shot is one-per-boot). The desk follows the name per F-SW1
(cycle census + writers + legality walk for func_800D2328)
before any emission.

## SPINWATCH RE-RUN — Nov 17, 2026 (build 30082a0e, second shot)

One launch of the spinwatch build (30082a0e, same build and ten-key dk_diag as the
proving boot), eager regime, stock clocks (1020.0/384.0/1331.2 read back; runtime
[pgo] 14,030 functions verbatim). Gate note: the console was held by the CTTR chat's
stage-present boot at first check — zero writes under the foreign claim, queue notice
sent, claimed on their release (claim re-verified immediately before the first input).

**OUTCOME SHAPE (a) — THE JUNCTION SHAPE. Verdict: SPINNER-NAMED — func_800D875C,
64/64 samples (SPINNER-PC's ranked candidate #1); the delay-wait model is CONFIRMED
for the junction.** The front-end ran in flow the whole way — no generation cliff at
any point, zero parkwatch fires, a first for a capture-line boot — crossed the Intro
end (2,802) cleanly, and the load landed via the in-flow drive (PLUS out of the
attract demo, PLUS to the menu, brightfrac-gated stick steps to a verified LOAD GAME
highlight bf=0.1385, B through slot A to the file list with the 10% save on top, load
confirm sampled at guest t=390,952 ms; no input after that press). Card scan
completed (84 CARDOpen lines, final op CARDOpen ScoobyDoo05 -> -4); h001.HIP never
issued (0 occurrences). Frames froze at 7,404 (first frozen [cnt] t=396,557 ms,
retrace=23766 ai_cb=79229, A+B signature, alarm queue empty); the gate opened at
t=398,550 ms and the shot fired on the same tick:

```
[vi] [safepoint] request t=398550ms frames=7404 retrace=23766 ai_cb=79229 enqueues=25542 flag=0x00000000 A=0 B=0 arm=0 latch=0
[vi] [spinwatch] gate-open t=398550ms frames=7404 retrace=23766 ai_cb=79229 enqueues=25542 flag=0x00000000 counter71024=0 op7101C=3 A=0 B=0 arm=0 latch=0
[vi] [spinwatch] shot handle=0x000081FF source=present-path samples=64 spacing_ms=10 settle_ms=250
[vi] [spinwatch] pc fn=0x800D875C fn_800D875C samples=64/64 lr fn=0x800CFD14 fn_800CFD14 pc_raw=elf+0x1C246A4 lr_raw=elf+0x1388B9C
[vi] [spinwatch] witnesses pre flag=0x00000000 counter71024=0 op7101C=3 A=0 B=0 arm=0 latch=0 post flag=0x00000000 counter71024=0 op7101C=3 A=0 B=0 arm=0 latch=0
```

- The name: **func_800D875C**, the calibrated delay loop on stream counter
  0x80271024 (target 140), unanimous 64/64, no minorities. LR **func_800CFD14** —
  the spin sits inside AdvanceRetrace's pre-callback housekeeping chain, the
  guard-set context the desk-wave-2 legality walk priced.
- **Counter 0x80271024 = 0**, identical at gate-open and both witness reads: the
  loop reset it at entry and its only advancer (callback-dispatched func_800D9EF8)
  never ran — not one 14-tick quantum climbed. Falsifiers: F-SW1 does NOT trip
  (the name is candidate #1); F-SW2 no; F-SW3 no (not func_800D8164 — the
  compiled-check question stays open, the checks were never expected to cover
  875C); **F-SW4 does NOT trip — the delay model is confirmed for the junction**,
  the mirror image of the transition wedge, where the same falsifier's >=400 arm
  (counter 476,576) killed it. The two wedges have different spinners:
  transition = func_800D2328 (arithmetic poison, delivery-unreachable);
  junction = func_800D875C (a true callback-maintained wait). SPINNER-2328's
  pre-registered prediction P-2328-2 (the junction spinner is not func_800D2328)
  is CONFIRMED by the shot.
- No trip, no dose: exactly one [safepoint] line (the request), standing
  unconsumed to the close; F-SP1/F-SP2 not engaged. Heapwatch: no trip all
  session. The wedge persisted unchanged to the final [cnt] at t=749,207 ms
  (~350 guest-s of confirmed wedge); the frozen screen was the LOAD GAME
  warning/loading screen. Clean HOME close (Close focused -> A -> running=0;
  the known wedged-exit error dialog dismissed under the claim).
- **Emission set, decided:** checks at func_800D875C's call-free back-edges
  (inner loc_800D8784, outer loc_800D8798), with func_800D85EC's pair
  (loc_800D86AC / loc_800D8708) as the same mechanism's second instance; the
  trip's dose programme is the legality walk's **audio-first** programme (the
  audio poll is the only class that can advance 0x80271024, via func_800D9EF8;
  retrace doses held unspent and UNCOUNTED while the guard reads set — the
  handler change the walk specced), counter 0x80271024 in the witness set. The
  one open empirical item for the tripped proving boot: whether the audio poll
  actually dispatches func_800D9EF8 in the wedged state (a wake with the
  counter climbing closes it; no wake at the full audio cap with the counter
  still at 0 refutes the dispatch path, not the legality).
- Profraw **fw-1794966824** banked (5,644,880 B, sha256 74c3b13c…) to pgo/ AND
  h3-test, hash-verified both ends; coverage = front-end in flow + landed load
  + junction wedge with the gate/shot (the bank's first junction profraw from a
  cliff-free ride, and the first whose wedge's spinner is named). **Banked
  total: 34.** Full record: SPINWATCH2-BOOT-LOG.md +
  stderr-spinwatch2.log (4,663,952 B, sha256 a30d838c…) in the evidence dir and
  h3-test. Closeout fully verified (resident 34585267 restored + read-back
  match; dk_diag deleted/absent; crash.txt at baseline, not overwritten; save
  unchanged; Config untouched; claim deleted + verified absent; release relays
  sent to SPM/CTTR/JSRF).

## ROUTE B-2 BUILD (Nov 17, 2026) — build 6f51f8f4, on the spinwatch base 30082a0e

Full record: B2-BUILD-LOG.md in the evidence set. Spec: SPINWATCH2-BOOT-LOG (the junction name) + desk-wave-2 fw-legality-delayloop (the programme). Backups of both edited files byte-verified pre-edit at C:\dev\no100f-nx\b2-backup\ (incl. shard-b2.patch, the reapplication record for the shard hunks). Semantic diff reviewed in full pre-build: shard delta exactly four one-line insertions, zero deletions; vi.cpp delta exactly the handler programme. Build: switch/scripts/build-switch.sh --deko --variant pgo-trim --out none, BUILD_EXIT=0, 0 FAILED; LOQ claim held and released. NRO 55,941,317 B, sha256 6f51f8f4433962ee4c85dd7f02fac69a00658244f2e1198d6dc0b7501887dbc5, in h3-test only; Downloads staging still 53dbb7bb. Scope (desk arithmetic, runtime governs): 14,030 functions (+0 vs base), 559,968 counters (+23).

Emission (shard_9e6d; the as-built shape — check in the cycle's branch block immediately before its backward goto; serve target = the goto's own destination, read from the shard): func_800D875C inner, block loc_800D878C, target 0x800D8784; func_800D875C outer, block loc_800D87A0, target 0x800D877C; func_800D85EC inner, block loc_800D86B4, target 0x800D86AC; func_800D85EC outer, block loc_800D8718, target 0x800D86A0. Naming note: the studies name the outer cycles by their compare blocks (8798/8708), but each outer goto lands on the cycle's body-entry block (the original do-while back-edge), so the outer targets are 877C/86A0; the inner targets match the studies' names. The 8164 pair is byte-untouched; the Route B declarations above func_800D8164 already cover both functions (same TU), so no declaration hunk was needed. The junction spinner lives in 875C's inner cycle — the 878C check is the predicted trip point.

Handler (GuestSafepointServe, vi.cpp): the allowlist widens to the six targets. Every serve past the witness throttle reads VI_HLE_IsAdvancingRetrace() before the dose decisions and the dose line logs guard=%d. Guard-clear serves run the Route B programme exactly as built (pair, >=1 s, poll, >=1 s, multiplicity pairs-to-8 then polls-to-16 at >=100 ms; full-dose close unchanged). Guard-set serves (the delay loops' context): dose 1 = ONE audio poll; retrace pairs are HELD UNSPENT — never attempted, never counted (the handler fix: the as-built handler counted calls, which would have spent the class on provable no-ops); multiplicity is polls only to the 16 cap; if a later serve reads the guard clear, the standard programme resumes from the counters as they stand (the owed pair goes at the dose-2 decision, pairs-first in multiplicity). Witness set gains counter 0x80271024 and op counter 0x8027101C (host-side reads in SafepointSample at every witness check; handler-side bases from its first sample; witnesses counter71024 / op7101C in both pre- and post-dose chains; both words logged in the dose/wake/wake-before-dose/obs/refused lines). Closes: polls exhausted with pairs 0 and no guard-clear serve all episode -> the distinct form "[safepoint] no-wake: retrace class guard-blocked (untested)" (F-SP2 NOT claimable against an untested class); polls capped with pairs owed and the guard still set -> hold (one "polls capped ... pairs held for a guard-clear serve" line, no delivery, no close). Gate, spinwatch, heapwatch, dk plumbing untouched; [dk] tail unchanged. All log forms strings-verified in the NRO, incl. the two new forms.

Proving boot (the parent's): one launch, ten eager keys (fifofast / watchdogat=420 / traceio / tracecnt / tracewake / parkwatch / padwatch / heapwatch / safepoint / spinwatch), spinwatch re-confirms the name at gate-open while trips begin. Falsifiers: F-SP1 (a dose crashes on 0x389dfff4); F-SP2 PROPER (full dose in a guard-CLEAR context only); the guard-set close rule (polls exhausted guard-set = retrace class untested, not F-SP2); the cure witness (counter 0x80271024 advances after a poll -> the delay completes -> h001.HIP issues -> the load completes). A no-climb outcome across the full audio cap answers the legality walk's open item the other way: the audio class does not dispatch the counter's advancer func_800D9EF8 in the wedged state.

## ROUTE B-2 PROVING BOOT (Nov 17–18, 2026) — SAFEPOINT-WAKE

One launch of build 6f51f8f4 (pgo-trim, spinwatch base + the four
delay-loop checks + the guard-aware handler), eager regime, ten
dk_diag keys, STOCK clocks (1020.0/384.0/1331.2), runtime [pgo]
14,030 functions verbatim. Full record:
~/workspace/no100f-h3-evidence/B2-BOOT-LOG.md; stderr-b2.log
(5,851,073 B, sha256 3baa6345…).

**Verdict: SAFEPOINT-WAKE — the executor safepoint's first trip,
first dose, and first wake, proven in the act; the load did not
complete (a second wedge formed ~2 s after the wake and the
one-episode-per-boot rule left it unserved).**

The ride drew the early cliff (breakpt froze at 409) and ran the
front-end in the carried regime at ~1.1 fps (parkwatch 929/929),
crossing the Intro end cleanly (no 2328 spin, no abort). The menu
drive at storm cadence showed the highlight/index desync again
(highlight verified LOAD GAME → confirm opened OPTIONS; after the
round-trip and one up-step, the confirm acted on LOAD GAME — the
index sat one step below the highlight at both confirms). The
load flow started (22 CARDOpen lines) and the wedge formed in
the transition at frames=3,196, before the file list rendered.

The episode, in brief: gate request t=2528590 ms (frames=3196
retrace=151601 ai_cb=505317 enqueues=16790 flag=0 A=0 B=1 arm=0
latch=0, counter71024=0); spinwatch named func_800D875C (55/64,
LR fn_800D875C itself); the spinner tripped at a B-2 check and
the handler served dose 1 with **guard=0** — the trip context
was guard-CLEAR (no retrace pass live), so dose 1 was the
retrace pair, delivered legally with full effect: post-dose
retrace 151602, ai_cb 505321, **counter71024 0 → 4** — the
delivered retrace's callback chain dispatched the counter's
advancer, the delay completed, `[safepoint] wake dose=1
witness=counter71024`. The guard-aware programme handled the
dynamic context correctly; the guard-set audio-first branch
was not exercised, and whether an audio poll dispatches
func_800D9EF8 from inside a retrace pass remains open. At
t=2530646 ms a second silence window matched and was refused
(`second silence window ignored (one episode per boot)`); the
guest never moved again (final frames=3196 retrace=151602,
guest_ok=1). h001.HIP: 0 occurrences. Heapwatch NO TRIP all
session (clean walks across the dose and both wedges);
F-SP1 and F-SP2 do not trip; crash.txt at baseline.

Profraw fw-1794974477 banked (5,645,064 B, sha256 5c3760c4…;
coverage: carried front-end + menu drive + load transition +
the served episode — the bank's first trip/dose/wake profraw).
**Banked total: 35.** Closeout fully verified (resident
34585267 restored byte-exact, dk_diag deleted, save and Config
unchanged, claim deleted + relays sent, console at clean HOME).

Next design question, named by this boot: episode policy. One
wake released the delay loop and the chain reached its next
callback-maintained wait ~2 s later; a shipping cure needs the
gate to re-arm for subsequent silence windows (per-episode caps
and heapwatch discipline unchanged), or a programme that runs
the chain further per episode. The second wedge's spinner is
unnamed (the shot is one-per-episode too); the released pass
advancing exactly one retrace points at the housekeeping
chain's next wait, func_800D85EC's 400-target delay the
standing candidate.


## ROUTE B-3 BUILD - episode re-arm (Nov 18, 2026)

Build **97f152a5** (55,945,413 B, sha256 97f152a59399e3dd7b3c89296b063376ea8333aaf8e50c6c8563d4c3af18c887;
pgo-trim lineage) on the B-2 base 6f51f8f4, in switch-logs/h3-test only;
Downloads staging verified still 53dbb7bb. Desk only - no console
contact, nothing launched on the PC. Spec: B2-BOOT-LOG's stated limit
(the B-2 wake released the delay loop and the chain wedged again ~2 s
later, refused by the one-episode-per-boot rule). POLICY ONLY - the
mechanism (six shard checks, serve, dose classes, guard programme,
witnesses) is byte-untouched; the whole delta is in
wiicompiled/runtime/src/hle/vi.cpp (232,904 B / 26e684c6 -> 237,690 B /
eadf48fe; backups byte-verified pre-edit at b3-backup/; semantic diff
reviewed in full: gate/episode bookkeeping + the spinwatch latch +
ep= fields, nothing else).

- EPISODE RE-ARM: after an episode closes (wake, no-wake, or
  guard-blocked close) the gate returns to watching (state 1 -> 0,
  gate-ready latch cleared). A new episode opens only on a FRESH
  silence window - the gate's unchanged conditions re-evaluated, the
  spinner signature re-confirmed, the episode bases recorded at that
  open - never on the heels of a close. The handler keys its dose
  machine off the gate's episode number (new shared atomic
  s_safepointEpisode, stored before the request): per-episode dose
  counters (pairs/polls), phase, guard/hold latches and handler-side
  bases reset on the first serve of each new episode; the per-episode
  caps (8 pairs / 16 polls) are unchanged.
- BOOT CAP: at most 16 episodes (kSafepointMaxEpisodes). The close of
  the 16th logs "[safepoint] episode cap reached episodes=%u" and the
  gate spends (state 3); dosing stops for the boot. A further silence
  window in the cap state logs the ignored-window form with the cap
  named, once: "[safepoint] silence window ignored (episode cap
  reached) t=%lldms frames=%u". Named proving outcome if the cap is
  reached without crossing: EPISODE-CAP.
- SHOT RE-ARM: spinwatch's latch is per-episode - the gate resets
  s_spinwatchFired at re-arm, so every episode's wedge is shot and
  named. The shot's own forms are unchanged.
- PERMANENT STOPS unchanged: a heapwatch trip latches the handler's
  suppression and, on the close, parks the gate in its spent state
  (no re-arm); the refusal conditions (thread identity,
  interrupts-disabled, exit) and the in-delivery nested-dose guard
  stand.
- LOG FORMS: the current forms lacked an episode field, so ep=%u was
  ADDED (stated plainly, per the brief): request, dose, wake,
  wake-before-dose, obs, no-wake at full dose, the guard-blocked
  close, the polls-capped hold, the interrupts-disabled refusal and
  the heapwatch abort all carry ep=. All new/changed forms
  strings-verified in the NRO; the [dk] tail is unchanged
  (safepoint=%d spinwatch=%d last). Desk scope arithmetic: 14,030
  functions (+0 vs B-2), 559,973 counters (+5) - the runtime [pgo]
  line remains the final word.
- PROVING BOOT (the parent's): one launch, the same TEN dk_diag keys,
  eager. Success ladder: episode 1 wake -> re-arm -> further episodes
  as needed -> feeders keep moving -> h001.HIP issues -> load
  completes -> spawn -> the standard route. Falsifiers F-SP1 (any
  dose crashes on 0x389dfff4) and F-SP2 PROPER (a guard-CLEAR
  full-dose no-wake) unchanged. NOT boot-tested. Full record:
  no100f-h3-evidence/B3-BUILD-LOG.md.

## ROUTE B-3 PROVING BOOT (Nov 18, 2026) — EPISODE-CAP

One launch of 97f152a5, eager, ten dk_diag keys, STOCK clocks
(1020.0/384.0/1331.2), runtime [pgo] 14,030 functions. Full
record: switch-logs/h3-test/stderr-b3.log (5,308,105 B, sha
76836732…) and ~/workspace/no100f-h3-evidence/B3-BOOT-LOG.md.

Carried ride (parkwatch 783 fires / 783 resumes), Intro end
crossed cleanly, attract demo reached; menu drive with NO
highlight/index desync (LOAD GAME brightfrac-verified, slot A,
10% save on top capture-verified); load landed; card scan
completed (final op CARDOpen ScoobyDoo05 -> -4); junction wedge
at frames=3,063.

The re-arm policy ran sixteen episodes: every episode's shot
named func_800D875C, every dose 1 was a guard-clear retrace
pair advancing counter 0x80271024 by +4, every episode woke on
dose 1 (16 requests / 16 wakes), requests spaced exactly
2,055 ms (fresh ≥2 s silence windows), per-episode counters
reset, per-episode shots fired. The wedge is ONE delay
re-entered per retrace: it completes at 140 ticks; the counter
reached 80 when `[safepoint] episode cap reached episodes=16`
spent the gate. h001.HIP 0 occurrences — no crossing.
Heapwatch no trip all session; F-SP1/F-SP2 untripped. Verdict
EPISODE-CAP: the remaining gap is dose-programme depth per
episode (one delivered retrace per wake), not policy,
legality, or delivery. Profraw fw-1794978115 banked (banked
total 36). Closeout fully verified (resident 34585267
restored, dk_diag deleted, crash.txt at baseline, save
unchanged, claim deleted + relays sent).

## ROUTE B-4 BUILD — dose-depth continuation (Nov 18, 2026)

Build **43786cda** (55,945,413 B, sha256
`43786cdac91750ca19321ba39b635ab52bf5546de935ef70165854565a0cedbb`),
pgo-trim lineage on the B-3 base 97f152a5. Provenance: … → 30082a0e →
6f51f8f4 → 97f152a5 → 43786cda. One file changed:
`wiicompiled/runtime/src/hle/vi.cpp` (237,690 → 242,285 B; backup
byte-verified pre-edit at `b4-backup/`). Semantic diff +92/−9, confined
to the safepoint header comment, the episode-cap constant, and
GuestSafepointServe's dose machine. Shard byte-untouched (093b918a…).
Downloads staging still 53dbb7bb. Desk scope: 14,030 functions (+0),
559,983 counters (+10). Detail: B4-BUILD-LOG.md.

THE CHANGE — one behavioural change plus one constant. The B-3 proving
boot's sixteen episodes each bought exactly one delivered retrace: the
wake check closed the episode on the first counter movement, so the
140-tick delay accumulated ~5 ticks per ~2 s episode and the cap
stopped the programme at 80. Now, when a dose's wake witness is
counter71024 specifically and frames remain frozen at the post-dose
observation, the episode does NOT close: the handler stays in the
episode (new flag spDepthContinue) and keeps delivering retrace pairs
at the programme's ≥1 s observation spacing — never the 100 ms
multiplicity rate — re-witnessing after each dose, with the counter
witness re-based per dose so each dose is measured on its own
movement. Counter movement seen between doses (a released pass) is
likewise not a wake. Exits: frames advance (the ordinary wake), a
dose moves nothing (the flag clears and the standard programme
proceeds from the standing phase — the audio poll dose and
multiplicity as built), or the pair cap is reached with the counter
still short (ONE new close form: `[safepoint] ep=%u dose-depth cap:
counter71024=%u frames frozen`, strings-verified in the NRO). The
continuation delivers pairs only: a guard-set serve holds the pair
unspent and uncounted (the B-2 rule) and waits for a guard-clear
serve; continuation deliveries do not advance the standard phase;
the phase-3 multiplicity block is gated off while the flag stands.
Per-episode caps unchanged (8 pairs / 16 polls); per-episode reset
clears the flag. Boot cap: kSafepointMaxEpisodes 16 → 32, the cap
forms untouched apart from the number. The counter is guest state
and carries across episodes, so even a DEPTH-CAP episode's climb is
permanent progress toward the 140 target.

PROVING BOOT (the parent's): one launch, the same ten eager dk_diag
keys. Ladder: landed load → wedge → episode 1 dose 1 (+4, frames
frozen) → continuation pairs at ≥1 s spacing, the dose lines showing
the climb → counter reaches 140 → the delay chain completes →
h001.HIP → load completes → spawn → the standard route. Falsifiers
F-SP1 / F-SP2 PROPER unchanged; DEPTH-CAP is a named outcome, not a
falsifier. NOT boot-tested at build time.

## ROUTE B-4 PROVING BOOT (Nov 18, 2026) — VERDICT: CROSSING

Build 43786cda, one launch, eager, ten dk_diag keys, stock clocks
(1020.0/384.0/1331.2 by dbg readback and the [dk] clocks line). Runtime
[pgo] verbatim: "pgo=instrumented capture build: 4374 KiB of counters,
14030 functions". Full record: switch-logs/h3-test/stderr-b4.log
(5,427,840 B, sha e3cf6ea0…) and B4-BOOT-LOG.md in the evidence set.

The load completed on a capture build for the first time in the campaign:
Scooby spawned at the Mystic Manor front yard from the 10% save, and the
standard 12-leg route ran end to end (marks route-start-b4b 1075 s →
route-end-b4b 1278 s, flush ok) — the bank's first route coverage.

The ride: front-end in flow past the Intro (frames=3,273 at t=155,438 ms,
breakpt=frames, zero parkwatch fires); the generation cliff drew at
frames≈4661 (breakpt froze at 4660 for the rest of the boot) and the tail
ran carried at ~1.1 fps (parkwatch 378 fires / 378 resumes). Menu landed
via gated single steps (up-step 0.70 s = one step; no desync); file list
capture-verified (11/14/26 10% / The Mystery Machine) before the confirm.

The wedge formed at frames=5,144 after a completed card scan (final op
CARDOpen ScoobyDoo05 -> -4; save opened ScoobyDoo00 -> 0). Four episodes,
32 retrace-pair doses, all guard-clear, all in func_800D875C's delay on
counter 0x80271024 (+4 per dose): ep=1 climbed 0→40, ep=2 40→80, ep=3
80→120 — each closing DOSE-DEPTH CAP with the counter carrying across
episodes exactly as built. In ep=4, doses 1-4 climbed 120→136 and a
released pass in the observation window carried the counter to 140: the
delay completed, the chain re-entered the delay inside the same open
episode (counter reset to 0), and doses 5-8 fed the fresh wait 0→16.
After dose 8 the audio stream started (ai_busy=1; [cnt] t=704,602 ms
retrace +115 in 1 s) and pumped the counter itself; frames resumed at
t=705,110 ms and never froze again. Ep=4 logged no close — the guest
outran the machinery between serves. Episode cap 32 never approached.

Spinwatch shots (one per episode) resolved modal fn=0x00000000 at
elf+0x27F9C54 (the serve path — the shot settles after dose 1 trips) with
fn_800D875C the top named minority (6-8/64); the delay identity rests on
the counter witness. Heapwatch NO TRIP all session (clean walks to
t=1,313,588 ms); F-SP1 dead, F-SP2 never engaged.

Profraw fw-1794981478 banked (5,645,184 B, sha 7d8a3b0d…), hash-verified
in pgo/ and h3-test; banked total 37; coverage class: carried front-end +
crossing + spawn + full standard route (first route coverage). Note: the
h001.HIP string appears in this log only in its two front-end lines (the
[dvdread] tag logs header probes); the completion is proven by the save
open, resumed frames, and the capture-verified controllable spawn.

Closeout fully verified: resident 34585267 restored (read-back match),
dk_diag deleted (listing-verified), crash.txt at baseline (never
overwritten), save unchanged, Config untouched, claim deleted
(listing-verified), relays sent. Still open: the guard-set audio-first
branch remains unexercised (every trip on record was guard-clear); a
crossing in flow (this one was carried) and full-cadence route profraws
remain ahead. Next: PGO training over the 37-bank; Route A ship decision.

## PGO TRAINING BOOT T1 (Nov 18, 2026) — NOT-REACHED (transition shape)

One launch of build 43786cda (the B-4 crossing build), eager, ten dk_diag
keys (fifofast / watchdogat=420 / traceio / tracecnt / tracewake /
parkwatch / padwatch / heapwatch / safepoint / spinwatch). Full record:
T1-BOOT-LOG.md; stderr-t1.log (5,182,258 B, sha a47694d0…). Runtime [pgo]
verbatim: 14,030 functions. Clocks: GPU-307-LOTTERY (1020.0/307.2/1331.2)
— training boot, not clock-gated, recorded as rolled.

Queue: the gate found CTTR's r66-r69 session live (claim since
2026-11-18T06:27:25Z); zero writes, queue notice sent, read-only watcher;
claimed on their release (claim absent 09:01:48Z) at 09:02:08Z, read back,
re-verified before the first input; foreground capture-verified (Crash
tile focused → DRIGHT → Scooby tile focused, verified). Staging verified
(NRO read-back 43786cda…; dk_diag read back verbatim; Config 1.5/0/false;
crash baseline; save snapshot a90864fa…).

The ride: the generation cliff drew at the very start of the logo chain —
breakpt reached 164 by t=11,723 ms and never moved again (the same
breakpt=164 freeze point as the first spinwatch boot). Parkwatch carried
the front-end at ~1.11 fps: 879 fires / 879 resumes, zero dry, the final
resume carrying frames 2799 → 2802. The Intro end was crossed with NO
2,802 abort.

The wedge: at frame 2,802 the guest entered the Intro-end transition
spin. Counters froze (frames=2802, retrace=143097, ai_cb=476969,
enqueues=15260) and never moved again. The gate opened ep=1 at
t=2386861ms and the spinwatch shot named the spinner verbatim:
`pc fn=0x800D2328 samples=64/64`, counter71024=476965 frozen pre/post.
func_800D2328's loop carries no safepoint check on this build (the B-2
emission set covers 875C/85EC/8164 only), so the request stood
unconsumed — the log's ONLY [safepoint] line — and no dose ever fired.
Polls at ~+3.5/+6.5/+8.6 min post-gate read identical frozen counters;
the final [cnt] (t=2965725ms) is ~579 s post-gate with zero movement.
Heapwatch NO TRIP all session (clean walks throughout); the wild cell
never appeared. No menu drive, no load (the 22 CARDOpen lines are
boot-phase slot probes), no spawn, no route.

Profraw: fw-1794992642.profraw banked (5,645,184 B, sha256 ebf75cb6…)
to pgo\ AND h3-test, hash-verified both ends — the session's only dump
file (9 interval writes to one cumulative filename; banked copy is the
final state). Banked total: 38. Coverage class: carried front-end +
Intro-end transition wedge — Class E universe, but ZERO route frames;
the bank's route counts still come solely from fw-1794981478 (B-4).

Closeout (all verified): HOME → X → Close focused → A; wedged-exit
error dialog dismissed with A under the claim; running=0; clean HOME
capture-verified. Resident 34585267 restored + read-back match;
dk_diag deleted (listing-verified); crash.txt at baseline, never
overwritten; save unchanged, Card A only the one file; Config
untouched; claim deleted + verified absent; release relays sent to
SPM, CTTR and JSRF.

Verdict: NOT-REACHED. The transition wedge (func_800D2328) is a
recurring stochastic tax on carried rides (spinwatch boot 1 and T1
poisoned; B-3/B-4 carried crossings clean) and is structurally
unservable until the 2328 back-edge checks or Route A coverage exists.
The T1 prize (in-flow route) remains open; this boot counts as one of
the programme's three additional training boots and adds a sixth
front-end sample, no route weight.

## T1 LEFTOVER CLOSEOUT - Nov 18, 2026 (~09:59-10:10Z)

The T1 training boot's reported close did not hold. The title (build 43786cda) was found LIVE and unclaimed: game status running=1, pid 0x13A, titleId 050FCFAF9892B000, front-end progressing through the attract demo at carried cadence - the frame-2,802 transition spin (func_800D2328) is a finite arithmetic loop and exited after the close attempt. Claimed as Sudo (FrightWorks perf chat) at 2026-11-18T09:59:49Z (note "T1 leftover closeout") and closed via HOME -> X (two attempts; the dialog-confirm step was not cleanly captured on camera). Verified closed: game status errors "applicationStatus running=0" on three consecutive polls, and final captures show clean HOME, stable, no Playing badge. Card verified exactly as the T1 closeout claimed: card NRO = resident 34585267 (37,193,925 B, sha256 345852677c979df9a2a948fee61947f3e682ee1bc6fa918f370fcf9cddaeaa45); dk_diag.txt absent; crash.txt at baseline (1,735 B, sha256 9d83c322fb35a0fc5b96e3f550d00803ed850973fa46b9803e691012b6cc4f80); save unchanged (41,024 B, sha256 a90864fafb2ebb614844a0db9e59b35be4ef5cdee4112058292b67485e178f2d, Card A holds only that file). Claim deleted and verified absent; console handed to CTTR.

## PGO TRAINING BOOT T2 (Nov 18, 2026) - VERDICT: T2-CARRIED

Second training boot on the crossing build 43786cda (55,945,413 B, sha 43786cda...), one launch, claimed 2026-11-18T10:56:09Z after a read-only standby behind the T1-leftover closeout and CTTR's r70 boot. Clocks STOCK 1020/384/1331.2 by readback. Runtime [pgo] line verbatim: [pgo] [build] pgo=instrumented capture build: 4374 KiB of counters, 14030 functions.

The cliff drew in the first minute (breakpt froze at 713); the ride was carried at ~900 ms/frame (1.1 fps), parkwatch 1,245 fires / 1,245 resumes. The Intro end at frame 2,802 crossed CLEANLY (no func_800D2328 transition poison - the shape that killed T1). Menu drive at storm cadence was the session's fight: dbg B holds desynced (OPTIONS twice on a gate-verified LOAD GAME highlight) and refused (twice, one verified sampled); after one full demo-timeout cycle per C4, sys-agent press/release pairs landed every confirm (LOAD GAME, slot A in three presses, file list capture-verified with the 10% save on top). Banked rule: at parkwatch cadence, confirms go by sys-agent press/release, steps stay brightfrac-gated.

Crossing (autonomous): frames froze at 3825; spinwatch named fn=0x800D875C; 4 episodes x 8 doses = 32 doses, all retrace class, guard=0; counter71024 climbed 0-40-80-120 across ep1-3 (dose-depth caps), reached its 140 target in ep4 (counter reset to 0 in the ep4 dose-4 obs), ep4 doses 5-8 served the follow-on request (0-16); frames resumed, load completed, spawn capture-verified at Mystic Manor. Standard route: marks route-start-t2 (guest 3039 s, frames 4002) to route-end-t2 (3235 s, frames 4218) = 216 route frames, +18 per leg, carried cadence ~1.10 fps; [vi] window 3602-4201 = 969.20 ms/frame (contains the junction freeze; coverage record, not a price). Soak ~3 min: +213 frames. Heapwatch NO TRIP (666 clean walks).

Profraw: the session's single dump file fw-1794999470.profraw (5,645,184 B, sha256 d816e992...) banked to pgo\ and h3-test, hash-verified both ends; no same-session dumps left unbanked. BANKED TOTAL: 39. Coverage class E: carried front-end + clean 2,802 + junction crossing + full route and soak at carried cadence. The in-flow route sample is still unbanked - T3's prize under the programme stop rule.

Closeout fully verified: stderr-t2.log (7,965,220 B, sha 0916afa3...) in evidence + h3-test; resident 34585267 restored and read back; dk_diag deleted/absent; crash.txt not overwritten; save unchanged, Card A single file; Config untouched; claim deleted and verified absent; release relays sent to SPM/CTTR/JSRF. Full record: ~/workspace/no100f-h3-evidence/T2-BOOT-LOG.md.

---

## PGO TRAINING BOOT T3 - 2026-11-18 (build 43786cda, Route B-4) - VERDICT: T3-IN-FLOW

The final training boot (programme cap; no T4) drew the in-flow ride. The
front-end ran at full cadence from launch to the load (breakpt == frames at
every check; windows 48.33 / 40.87 / 43.90 / 48.25 ms/frame), frame 2,802
crossed clean with no gate, and PARKWATCH FIRED ZERO TIMES IN THE WHOLE BOOT -
the cliff never drew. Menu drive per the T2 banked rule (brightfrac-gated
single step NEW GAME -> LOAD GAME; all confirms by sys-agent press/release
pairs; no desync; 10% save capture-verified on top). The junction crossing was
autonomous: request at frames=4903 (flag=0, A=1 B=1, arm=0 latch=0,
op7101C=3), 4 episodes x 8 doses, all class=retrace guard=0, counter
0->40->80->120 on the episode dose-depth caps, ep4 crossed 140 mid-episode
(counter reset; doses 5-8 fed the follow-on wait to 16); frames resumed and
spawn was capture-verified at Mystic Manor. Route: marks 383->573 s, ~2,886
frames over the 12 standard legs; [vi] windows 64.07 / 67.04 / 63.45 / 67.62 /
67.62 / 68.02 ms/frame at GPU-307-LOTTERY clocks (coverage only, not a price).
Soak 588->758 s, ~3,200 more frames; session ~12,312 frames, guest_ok=1
throughout. Heapwatch 19 clean walks, zero trips; crash.txt baseline intact.
Runtime [pgo] line verbatim: "[pgo] [build] pgo=instrumented capture build:
4374 KiB of counters, 14030 functions". Banked: fw-1795006339.profraw
(5,645,184 B, sha256 88da3947...) to pgo/ and h3-test, hash-verified - the
session's only new file (interval dumps overwrote it in place). BANKED TOTAL
40. Coverage: Class E, the set's first fully in-flow sample; the training set
now rests on B-4 (~650 carried) + T2 (~429 carried) + T3 (~6,100 in-flow
gameplay frames). TRAINING COMPLETE - next: set curation (8 Class E files at
the pgo/ top level), manifest adoption (manifest-trim.txt over manifest.txt,
logged), the pgo-use build, and the A/B. Closeout under the strengthened
standard: running=0 polled first, then a final clean-HOME capture (no Playing
badge), claim deleted after; resident 34585267 restored and read-back
verified; dk_diag removed; save and Config untouched. Full record:
T3-BOOT-LOG.md + stderr-t3.log (6,036,467 B, sha256 e1afa02d...) in the
evidence folder and h3-test.

## MANIFEST ADOPTION — 2026-11-18T13:11:01Z

Ahead of the pgo-use build, pgo/manifest.txt was deliberately replaced with the
training set's true provenance manifest, per the PGO training programme (Q4:
the use-build machinery reads only pgo/manifest.txt, which held the full-scope
capture manifest, while the Class E training set's provenance is the trim
manifest; commit and shards_fingerprint are identical in both files).

- manifest.txt sha256 BEFORE (full scope, scope=game+runtime+aurora, trained_utc=2026-11-17T22:25:08Z): d4ac459b3e44c513b656a1484619da1b12bd0877da97398c9d827daf3b16a77c
- manifest-trim.txt sha256 (source, untouched): 66fe061af1dd67bfa0fe21b5f2217e3557dbe34a10b6a8d1aa95593aebaaf5a3
- manifest.txt sha256 AFTER (copy of manifest-trim.txt; scope=game+runtime+aurora-trim-spm7, trained_utc=2026-11-18T05:49:58Z): 66fe061af1dd67bfa0fe21b5f2217e3557dbe34a10b6a8d1aa95593aebaaf5a3
- Set curation in the same step: pgo/ top level now holds exactly the 8 Class E
  profraws; the other 32 banked profraws were MOVED (not deleted) to pgo/archive-nonE/.

## PGO-USE BUILD — STOPPED AT THE MERGE TRIPWIRE (2026-11-18T13:12:33Z)

Set curation and the manifest adoption (above) completed. The pgo-use build itself
was NOT run: a pre-merge check of the curated set (llvm-profdata merge + show in the
build container, read-only) read **Total functions: 14037**, not the programme's
14030 tripwire, and the job's instructions were to stop and report on any other value.
No LOQ build claim was taken and nothing was built or staged.

Diagnostics (same container, read-only): merging fw-1794981478 alone and
fw-1794962048 alone each read Total functions: 14030 exactly. The 8-file merged
profile holds 14,037 records over 14,027 unique names. The difference is per-build
hash fragmentation inside the Class E line — the programme's own Q1 mechanism
(records keyed by name+hash; older builds' versions of an edited function ride as
dead extra records). Fragmented names measured: GuestSafepointServe x4 hashes;
func_800D875C, func_800D85EC, SafepointSample, the StartStallWatchdogOnce thread
lambda, InvokeIndirectCpu, RuntimeNandPath::FailNandRoot and
RuntimeConfigFile::ParseConfigDocument x2 hashes each (+10 extra records, offset
by 3 fewer unique names than a single file's record count). No foreign-universe
records: every input was one of the 8 verified Class E files. Whether 14037 is
accepted as this set's correct merged total (the fit check remains the build's own
gate) is the parent's call; the build waits on it.

## PGO-USE BUILD RUN (Nov 18, 2026)

The pgo-use build ran under the coordinator ruling accepting merged Total functions: 14037 (the programme's 14030 tripwire was mispredicted; the +10 records over 14,027 unique names are enumerated in-family hash fragmentation - see the PGO-USE BUILD LOG in the evidence dir). First invocation failed at the source-copy step because the script was launched from the WSL build tree (root == work: cp "same file" on the launcher csproj, exit 1, no harm - the self-rsyncs were no-ops and the pgo-use block never ran); relaunched from the Windows tree, BUILD_EXIT=0 (13:14-13:24Z; LOQ build claim held 13:14:06Z and released right after the build).

Gates, verbatim from the build:
- Merge (the build's own, 8 Class E profraws): Total functions: 14037.
- [pgo-fit] profile: 14037 functions; out of date in this build: 0 (0.0%, limit 25%); matched: 14037; compiled without profile data: 0 (informational)
- [pgo-fit] OK: 14037 of 14037 profiled functions fit this tree.
- CMake provenance: PGO USE build: profile /work/pgo/no100f.profdata sha 308c2ca4f04cf6ce (trained on tree ae442e15ddac46b2b21932141eb855920c0c3d7a, clang 19, scope game+runtime+aurora-trim-spm7).

Product: build-deko-pgo-use/no100f-nx.nro, 37,304,517 B (the ~37 MB resident class, not the 55,945,413 B capture size), sha256 cc38f942103cdbca1f0506dd0043dbc2010e2afe4926e43b7e0e296e8a0d90ac; built with --out none and copied manually to switch-logs/h3-test/no100f-nx-pgo-use.nro, read-back hash identical. Downloads staging verified untouched (53dbb7bb, 37,218,501 B). Sources are the B-4 tree state: the use build carries the safepoint machinery compiled in, inert unless armed, with no PGO instrumentation. Next step is the programme's A/B: resident 34585267 vs cc38f942 on the standard route at stock clocks, dk_diag fifofast only (no safepoint key); parity requires the load to complete unarmed.

## PGO A/B (Nov 18, 2026) — VERDICT: PARITY FAILURE on the use arm; decision rule rejects

One claim (2026-11-18T15:26:46Z, Sudo (FrightWorks perf chat)) after standby
behind CTTR's r72-r74 session; gate free confirmed twice, foreground
capture-verified. Both arms: dk_diag exactly one key fifofast (read back
verbatim per arm), Config 1.5/0/false verify-only, stock clocks by dbg
readback per arm (1020.0/384.0/1331.2 STOCK both arms; no 307 recycle),
save and crash.txt snapshotted per arm (save a90864fa unchanged throughout;
crash.txt at baseline 9d83c322 throughout, never overwritten).

ARM R (resident 34585267, card already held it, hash verified pre-launch):
load unaided, spawn capture-verified at Mystic Manor, standard route (marks
at guest 301 s / 492 s). [vi] windows spanning the route: 7802-8401 44.07,
8402-9001 41.58, 9002-9601 46.72, 9602-10201 42.69, 10202-10801 44.84,
10802-11401 44.78, 11402-12001 41.53 — route-span mean 43.74 ms/frame
(7 windows, 4,200 frames), a fresh baseline consistent with the 44.3-44.8
anchor. Drive note: a 0.35 s stick excursion double-stepped NEW GAME to
OPTIONS; a 0.2 s up-step landed LOAD GAME exactly (row-profile verified);
confirms by sys-agent B press/release pairs. Clean HOME close (running=0,
then final capture). stderr-ab-r.log (183,066 B, 2351d582).

ARM U (pgo-use cc38f942, staged from h3-test, card read-back hash verified):
PARITY FAILURE. The guest presented exactly 2 frames and froze in the boot
phase, unarmed (dk tail: fifofast=1, safepoint=0, all instruments off).
Watchdog: 12 s and 30 s both "2 guest frame(s) presented", then the thread
dump (1 guest thread 0x8026D568, state=2, queue=0x0, lr=0x800166B0
fn_800166A0+0x10); the guest logged nothing after the dump. Screen black
with the "1 shader compiling" overlay badge; three captures byte-identical
(24,077 B). Zero [vi] census windows. The menu never formed; no load, no
spawn, no route. No intervention (nothing armed). Closed via HOME dialog;
the exit took ~20 s and left the known wedged-exit error dialog, dismissed
under the claim; final capture clean HOME, running=0. stderr-ab-u.log
(428 lines, 72fb2018) + logtext ring snapshots in h3-test. Observation, not
a root-cause claim: both arms logged Aurora Dawn-cache sqlite errors
(R once at route start, U repeatedly in the ring; cache init succeeded in
both) — whether the stall is a property of the use build or of the cache
state it met is a desk question; the boot establishes only that the use
build, flown exactly as specced, did not achieve parity.

DECISION RULE (programme Q4) applied: parity failure = reject (the delta
is undefined; the <3% tiebreaker does not apply). Arm R's 43.74 stands as
the fresh resident baseline. No adoption action taken.

Closeout: card restored to resident 34585267 (read-back hash match),
dk_diag deleted (listing-verified), crash.txt baseline, save unchanged,
Config untouched, claim deleted and verified absent, release relays sent
to SPM/CTTR/JSRF. Full record: AB-BOOT-LOG.md in the evidence folder.

PGO-USE FREEZE DIAGNOSIS (Nov 18, 2026; desk only, PGO-USE-FREEZE-DIAGNOSIS.md): arm U's frame-2 freeze is the guest's only thread parked inside the host VIWaitForRetrace HLE (vi.cpp:4243), entered via the func_800166A0 thunk (shard_aa0635) from fn_800BEFDC+0x440; the HLE loop sleeps on the retrace queue 0x802713A0 and wakes only when AdvanceRetrace moves the host g_vi.retraceCount - the delivery-defect family in a new costume, and delivery ceased after frame 2 for a reason the watchdog dump cannot separate (its OS-global fields read garbage). The compiled-code hypothesis is ACQUITTED at every compared point: func_800166A0 is instruction-equivalent in both builds (only its PGO counter block differs), VIWaitForRetrace's sleep loop re-reads the identical field (page+1592 = &g_viMutex+132) in both ELFs with no hoisted load, OSSleepThread's call set matches, and the pump differs only by inlining; the one dramatic transform (func_800166C0, 0x638 -> 0xE18, .text.hot, inlined cached dispatch) is not on the frozen path. The Dawn-cache errors are acquitted as the cause: the (14) commit-failure spam is shared by both arms (R 55, U 27 - correcting the A/B log's 'one pair' line), and U's single (11) pipeline_cache corruption is a fast worker-thread failure with no path to the retrace pump. Next discriminators, in order: a cache-cleared re-fly of arm U, then a tracewake-armed diagnosis re-fly to split pump-stopped from wake-lost. The programme's reject verdict stands on its own rule regardless.

## ARM U RE-FLY — cache discriminator (Nov 18, 2026; AB2-REFLY-LOG.md)

Resume of the interrupted re-fly: the predecessor claimed at 2026-11-18T19:36:04Z, then the Switch dropped off the LAN (battery); Brandon plugged it in, it returned, and this session proceeded under the untouched claim (foreground capture-verified clean HOME, running=0). Pre-flight cache record: dawn_cache.db 16,384 B sha256 0c3c6b00dd57dd60776dcb374080b1e55494eab86f4e48e38bf40b7b2e3f7469; dawn_cache.db-journal 12,824 B sha256 d28bbf6137e6e7eda34886500bb17a994ea6be032ab9a8b2581adacd299f93d5 (the set; pipeline_cache.db pair untouched). PHASE 1 (cache aside, use build cc38f942 staged + read-back verified, dk_diag fifofast only, clocks STOCK by dbg readback): the set was renamed to .preAB in place, the build launched, created a FRESH cache — and FROZE IDENTICALLY at frame 2 (watchdog "2 guest frame(s) presented" at 12 s and 30 s; dump thread 0x8026D568 state=2 queue=0x0, LR 0x800166B0 fn_800166A0+0x10, caller fn_800BEFDC+0x440; capture black with the "1 shader compiling" badge, 24,077 B, the arm U byte size). VERDICT: PHASE-1 PARITY FAILURE — per the diagnosis's discriminator the cache is FULLY ACQUITTED; the freeze is a property of the use build as flown, now n=2 under two cache states. Phase 2 (price, original cache) was conditional on parity and NOT flown; the tracewake diagnosis flight was not in the brief and not flown. Closed per the strengthened standard (Close focused, running=0 at +25 s, error dialog dismissed under the claim, final clean HOME). Closeout verified: fresh cache set deleted, .preAB set restored with BOTH hashes matching pre-flight exactly, no .preAB left; resident 34585267 restored + read-back match; dk_diag deleted (listing-verified); crash.txt at baseline (never overwritten); save unchanged; Config untouched; claim deleted + verified absent (root listing + FTP 550). stderr-u2.log 30,455 B sha256 352bcd9f8660ae2d7ef05f1923bd88a8d0597e03a348159b10169a581a0e2ba3. The programme's reject verdict stands; arm R's 43.74 ms/frame remains the fresh resident baseline. Battery note: console returned at 4% charging and climbed to 7% across the session; no low-battery prompt appeared.

## SAME-TREE DEFAULT FLIGHT (Nov 18, 2026) — VERDICT: FREEZES; TREE CONVICTED, PGO ACQUITTED

Discriminator for the pgo-use frame-2 freeze (cc38f942, frozen twice, cache acquitted): a DEFAULT build (no --variant: no PGO, no instrumentation, no profdata; build log contains zero "pgo" lines) compiled from the verified use-build tree state (vi.cpp 242,285 B sha 995e108cd8c35af714b50f7d8d841f3522bba0e3bbd760986ddeacf9e0586e4d; shard_9e6d 093b918a8c664d543297dccad8aa6bfce53072e267b78b7be62c12ced7d6f67f; WSL build tree post-rsync hash-identical). Product: build-deko/no100f-nx.nro, 37,275,845 B, sha256 9fb13f80732968465664b84526d5dc0a33dd13dee0c3118c46329533bf801e8b, staged to switch-logs/h3-test/no100f-nx-sametree-default.nro (read-back match); Downloads staging untouched (53dbb7bb); LOQ build claim taken 19:51:29Z and released right after the build.

Flight: console claim 2026-11-18T19:58:52Z; battery gate held the launch (17% charging at gate, 21% at +5 min, 25% at +10 min — launched only at >=25%); setup under claim (crash/save/Config snapshots at baseline values); NRO read-back 9fb13f80 on card; dk_diag exactly fifofast (aef842a6); claim re-verified before first input; clocks dbg-verbatim STOCK 1020.0/384.0/1331.2. The build froze at frame 2 with the use build's exact signature: [vi] seq -> logo stage 967515525 (frame 2) then silence; watchdog 12 s and 30 s "2 guest frame(s) presented, VI black=0"; fiber dump thread 0x8026D568 state 2, lr=0x800166B0<fn_800166A0+0x10>, stack +14 0x800BF41C<fn_800BEFDC+0x440>; captures black with the "1 shader compiling" badge at 24,077 B (both). [dk] line: fifofast=1, safepoint=0, all instruments off. No intervention; closed per the strengthened standard (Close focused -> A -> running=0 at +15 s -> wedged-exit error dialog dismissed under claim -> running=0 re-read -> final clean HOME capture, battery 30%). stderr-std.log 30,265 B sha 442c7b6776e82d222005009ff14d664d4bb3463cf507ad9a0310627d5f3ba11a (evidence + h3-test).

Closeout verified: resident 34585267 restored (read-back 345852677c...aa45); dk_diag deleted (listing-verified); crash.txt at baseline, never overwritten; save unchanged; Config untouched; claim deleted and verified absent; release relays sent.

Meaning: the frame-2 VIWaitForRetrace park is a property of the current tree state (B-4), reproduced by three builds (pgo-use x2 cache states, default x1); the resident predates the defect and flies fine. The PGO reject stands but its cause is reassigned — the profile was never the trigger, and PGO cannot be priced until the tree defect is fixed. The defect also blocks every future build from this tree (Route A ship vehicle, native-scene builds). Bisection field: the resident-vs-B-4 delta (the safepoint-lineage vi.cpp machinery and the six hand-emitted shard checks), acting through timing/state — the wait site itself is byte-faithful in both builds. Full record: no100f-h3-evidence/SAMETREE-DEFAULT-FLIGHT-LOG.md.

TREE-FREEZE BISECTION STUDY (desk, Nov 18, 2026; full study: ~/workspace/no100f-h3-evidence/TREE-FREEZE-BISECTION-STUDY.md on the VM): the freeze field is the stage-2 tree state to B-4 tree state delta only, because every uninstrumented build that ever flew (through stage-2 build 53dbb7bb) already carries the whole round-1 and stage-1/2 delta, while every build of the watchdog-through-B-4 lineage was flown instrumented. The Band-2 delta is 19 files, dominated by vi.cpp (78,892 to 242,285 B, nine pure-insertion hunks; VIWaitForRetrace itself byte-identical old vs new). The boot-phase audit acquits every armed element on code evidence: all env- or latch-gated, the freezing boot's log contains zero armed-block lines, and the gx_fifo reroute re-gates at its callee in dvd.cpp. Suspect #1 is the stall-service/gate thread in vi.cpp: spawned unconditionally (vi.cpp:1558) from the first AdvanceRetrace, ticking at 50 ms and taking g_viMutex on every tick, the only Band-2 element whose unarmed execution is not provably inert. Bisection spec: Arm A (one-line gate-thread spawn kill) first, then Arm C (both generated shards reverted), Arm A+C (covers a jointly-necessary pair or two independent sufficient causes), Arm B (vi.cpp full revert plus shards, for link closure), Arms D/E for the OS-layer and counter tails, and Arm 0 (full Band-2 revert) as the model-check control; the verdict point is the logo sequence, no route needed. Desk-only, read-only: nothing built, launched, or edited in any tree.

## BISECTION (2026-11-18, tree-freeze bisection campaign, flown)

Full record: ~/workspace/no100f-h3-evidence/BISECT-CAMPAIGN-LOG.md
(study: TREE-FREEZE-BISECTION-STUDY.md in the same folder). All
flights at stock clocks by dbg readback, dk_diag exactly fifofast,
short-form at the logo sequence.

- Arm A (gate-thread spawn killed at vi.cpp:1558, build f2cc25f2):
  FREEZES, signature-identical (2 frames, watchdog 12/30 s,
  lr=0x800166B0). The stall-service/gate thread is exonerated as a
  necessary cause of the frame-2 freeze.
- Arm C (both generated shards reverted to their pre-instrument
  backup contents, build e7dbc2dc): FLIES. Watchdog 256 frames at
  12 s and 800 at 30 s; front-end formed in flow; the standard
  load completed (final load step driven by Brandon hands-on) and
  the game reached the Mystic Manor spawn of the 10% save. The
  shard pair is convicted.
- Split C9 (shard_9e6d only reverted to its safepoint-backup
  content, shard_4cbf at B-4, build 99a14107): FLIES, full
  front-end (255/798 watchdog frames, title formed). Split C4
  (shard_4cbf only, build e533a3e5) was built but not flown; the
  split was already decided.

VERDICT: shard_9e6d0891680e24b492f4994a.cpp is convicted alone.
Its B-4 content, the safepoint-regenerated shard, is the element
of the stage-2 to B-4 delta that freezes uninstrumented builds at
frame 2. The mechanism reading stays timing coupling (the study's
Q4): the safepoint checks woven through this shard changed guest
timing at the boot junction, the same junction the executor
safepoint cured from the other side on the instrumented builds.
No single safepoint site is convicted, and no shard fix was
attempted: the fix shape is a design call, and Route A
(translator plus regeneration) regenerates the shards anyway.
Any interim uninstrumented build must either carry the
pre-safepoint shard_9e6d or re-time the junction. Arms A+C, B and
0 never flew; the tree decided first. The Windows tree ends in
the exact B-4 state, hash-verified after every arm, with no edit
left in place.

## COUNTDOWN TEST (Nov 18, 2026)

Build 70d315b9 (default line, --deko --out none): the six hand-emitted safepoint checks in shard_9e6d wrapped in per-site countdowns (function-local statics; GuestSafepointRequested consulted only every 1024th trip of each back-edge; pre-edit snapshot + six-site diff preserved in countdown-backup\). VERDICT: FREEZES at frame 2, signature-identical ([vi] seq -> logo (frame 2); watchdog "2 guest frame(s) presented" at 12 s and 30 s; Guest refs lr=0x800166B0; frozen black capture; stock clocks 1020/384/1331.2). Consultation frequency is therefore NOT the operative coupling: amortising the request load 1024:1 does not cure the boot. What remains per trip is a counter decrement plus the wrapped blocks' code-layout footprint, so the coupling lives in the shard's compiled timing/layout footprint at the boot junction, not in how often the request executes. Phase 2 (the capture-build junction ride) was NOT flown - it was gated on Phase 1 flying. Route A consequence: emission density alone will not de-poison a regenerated shard; the regenerated tree needs an early uninstrumented flight, and if it still freezes the lever is placement/layout, not density. Full record: no100f-h3-evidence/COUNTDOWN-TEST-LOG.md; stderr-countdown-d.log in h3-test. Tree restored to exact B-4 state (hash-verified); countdown-backup/ is the only tree addition.

## ROUTE A PHASE 0 — baseline regeneration audit + early flight (Nov 18, 2026)

Full record: no100f-h3-evidence/ROUTE-A-PHASE0-LOG.md.

BASELINE VERDICT: CLEAN. Baseline regeneration with the unmodified translator ran in a
scratch copy only (C:\dev\no100f-nx-regen-scratch; the live tree was never written to;
its three anchors — vi.cpp 995e108c, shard_9e6d 093b918a, shard_4cbf f9d63df6 — verified
identical before and after). Unit-level audit: 6,193 function units on both sides, sets
identical, 6,073 byte-identical after path/shard-name normalisation; the 120 differing
units are 116 shard-boundary epilogue artifacts plus exactly the 4 known hand patches
(func_800D8164 10 lines, func_800D85EC 2, func_800D875C 2 — shard_9e6d's 14 safepoint
lines; func_80175FC4 5 lines — shard_4cbf's dispwatch hook). Translation metadata
byte-identical raw. Shard boundaries are a FROZEN TABLE (base_common_shard_map.json):
a fresh-pack regen deterministically repacks (62/72 renamed, zero unit changes, run1 ==
run2), while a regen over the existing generated tree reuses the table — 193/195 files
pair identical, and the regenerated 9e6d/4cbf shards are byte-identical to the bisection
backups (4b6c83a1 / c42a092d). PHASE 1 RULE: regenerate IN PLACE over the existing
generated tree (or seed the frozen map); the frozen-table route preserves shards.cmake
and the PGO shards_fingerprint.

BUILD: scratch-tree default build (--deko --out none, BUILD_EXIT=0) produced
37,271,749 B, sha256 e7dbc2dc4f54525e9f4e2abaad930f8af3748b7c1128e66b7d9b9ec548064da7 —
byte-identical to the bisection Arm C build. Build determinism proven across trees.

FLIGHT VERDICT: FLIES — Route A's first gate PASSED. Full gate discipline (claim
23:22:43Z, re-verified before first input; dk_diag exactly fifofast; clocks by dbg
readback 1020/384/1331.2 STOCK). No frame-2 freeze (watchdog 249 frames at 12 s, 792 at
30 s); front-end in flow the whole way (logos, intro, title, menu, load); the 10% save
loaded and the Mystic Manor spawn was capture-verified; session reached guest frame
23,400+. Drive note: on this boot the menu drive only landed via dbg holds (≥3 s);
sys-agent clicks/holds did not open or confirm, and a 0.2 s stick step double-stepped
(the five-band brightfrac gate caught it; a 0.1 s step stepped singly). Closeout per
the strengthened standard (3x running=0 polls + 2 clean-HOME captures); resident
34585267 restored with read-back match; crash.txt at baseline throughout; save and
Config hashes unchanged; dk_diag deleted and verified absent; stderr preserved as
h3-test/stderr-routea0.log (264,627 B, eaeb165b). At Brandon's request the console was
NOT released: the claim was rewritten in his name (owner=Brandon (hands-on),
2026-11-18T23:39:49Z) and the console sits at HOME with the resident build in place,
held for his hands-on session.

NEXT: Route A Phase 1 — PlanSafepoints planner in the generator, in-place regen over
the frozen table, unit-level audit (only check insertions may differ), rebuild, proving
programme. The staged pgo-use build cc38f942 still contains the poisoned shard; never
fly it as a pricing candidate.

## ROUTE A PHASE 1 (DESK) — 2026-11-19

PlanSafepoints is implemented in the translator (new partial
CxxLinearCodeGenerator.Safepoint.cs, planned beside PlanFlagElision,
consulted at every emitted goto) and the scratch tree
(C:\dev\no100f-nx-regen-scratch) was regenerated with it through the
frozen-table route. Full record:
~/workspace/no100f-h3-evidence/ROUTE-A-PHASE1-DESK-LOG.md. Generator diff
preserved at C:\dev\no100f-nx-regen-scratch\routea1-generator.patch
(561 lines, 5 files); pristine generator at routea1-pristine\ in the
scratch tree. The live tree was not written (anchors re-verified after:
vi.cpp 995e108c, shard_9e6d 093b918a, shard_4cbf f9d63df6). No console
contact.

Planner semantics: a check is emitted immediately before the backward
goto of a back-edge whose natural loop is call-free at emission
(whitelist of expression/inline-helper lowerings; calls, indirect calls,
jump tables, returns and ResolveGuestMemoryRange disqualify; functions
with LR-continuation or non-returning calls are abandoned whole).
Deviation forced by evidence: IrTracePpc is transparent, because its
lowering is an empty case; disqualifying it rejected 3,800 of 4,041
back-edges and planned zero checks. State-free clones strip the checks
(canonical body only), per the scoping caveat. Tests: 66 passed
(Safepoint 7 new + FlagElision + GpuFifoBurst + EmittedOutputShape).

Audit (vs the Phase 0 baseline census, 6,193 units): 761 units differ,
756 by pure check insertions, 5 by insertions plus state-free
length-suffix renames (the state_free_result_ suffix is a buffer-length
value at the call site), 0 unexplained. 1,300 check lines emitted in 761
functions; planner census 1,335 in 779 (the 35-site delta is 18
functions planned in translation but not emitted into any shard).
84 shards renamed (content-hash filenames), traits contents unchanged;
shard boundaries frozen as qualified.

Hand sites: 4 of 6 covered (func_800D8164 both, func_800D85EC inner
0x800D86AC, func_800D875C inner 0x800D8784). The two OUTER sites
(0x800D86A0, 0x800D877C) are NOT covered: each outer natural loop runs
through a block calling the translated float-conversion helper
0x800D90AC (blocks loc_800D86B8 / loc_800D8790), so the cycles are not
call-free under the design rule; the B-2 hand emission exceeded the
rule there. Brandon's call: accept 4/6, amend the rule for
proven-non-delivering leaf helpers, or keep hand-adding the two checks
per regen. Desk recommendation: accept for the first flight.

Builds (scratch, claim discipline, board-noted): default 2b63aaa3
(37,181,637 B) and capture 7f3c9baf (55,920,837 B), staged to
switch-logs/h3-test as no100f-nx-routea1-default.nro and
no100f-nx-routea1-cap.nro with read-back hash matches. NOT flown.
Capture NRO strings-verifies the B-4 [safepoint] log forms; scratch
vi.cpp byte-identical to live. Next: Brandon's go for the live-tree
regen, then the proving programme (early uninstrumented flight first,
per the Route A gate).

## Route A Phase 2 (LOQ half) — planner shipped to the live tree (2026-11-19)

Brandon ruled ACCEPT 4/6. The Phase 1 generator patch was applied to the live tree (5 files; post-image hashes byte-identical to the scratch Phase 1 state; pre-state preserved in phase2-pristine/), the translator was rebuilt explicitly (0 warnings / 0 errors; the Core dll carries PlanSafepoints; translator tests 58 passed / 0 failed), and the live tree was regenerated in place via the frozen-table route (ra2-shards.log: reused frozen table; planner census identical to Phase 1: 1335 planned in 779 functions). Audit: vs the Phase 1 scratch regen, 6,193/6,193 units identical; vs the pre-regen live tree, 761 units differ = 752 pure check insertions + 5 insertions/state-free-suffix renames + 4 hand-patch units — the hand extern block, both outer safepoint checks and the dispwatch hook are gone, the four inner sites are covered, the two outer sites are absent (0 occurrences tree-wide), 0 unexplained. Runtime untouched (vi.cpp 995e108c). Builds (each under the LOQ build claim, released immediately after): default 2b63aaa3 (37,181,637 B) and capture 7f3c9baf (55,920,837 B), staged to switch-logs/h3-test as no100f-nx-routea2-default.nro / no100f-nx-routea2-cap.nro with read-back hash matches — both byte-identical to the Phase 1 scratch builds. The capture build rewrote pgo/manifest.txt (ff5a856f; the build-written copy is preserved at switch-logs/h3-test/routea2-capture-manifest.txt) and the curated trim manifest (66fe061a) was restored. The capture NRO is strings-verified for the B-4 [safepoint] log forms (counts identical to the Phase 1 capture NRO). Nothing flown; no console contact of any kind. Full record: no100f-h3-evidence/ROUTE-A-PHASE2-LOQ-LOG.md. Next: the Phase 2 proving programme (console, Brandon's go) — the early uninstrumented flight of the default build first.

## ROUTE A F1 — EARLY UNINSTRUMENTED FLIGHT (Nov 19, 2026): F1-FREEZE

- Build: Phase 2 default 2b63aaa3 (37,181,637 B, sha256 2b63aaa3c584cd4a...) — the live-tree regenerated build carrying the translator-emitted safepoint checks (1,300 check lines tree-wide; hand sites 4 of 6 per Brandon's ruling), runtime inert unless armed. Staged from h3-test\no100f-nx-routea2-default.nro (byte-identical to the Phase 1 scratch build); card read-back hash matched before launch.
- Queue and gate: stood by read-only behind CTTR r78's claim (taken 23:52:15Z) with a queue notice sent; claim freed 01:09:31Z. Claimed 01:09:49Z (owner Sudo (FrightWorks chat), note "Route A F1 early flight"), read back verbatim, and re-verified immediately before the launch press. Gate capture: clean HOME, battery 100% — but HOME focus sat on the CRASH tile (CTTR leftover); focus was moved one tile right and the Scooby tile capture-verified focused before launching (the exact wrong-tile failure CTTR hit earlier in the night, avoided). Save hash recorded BEFORE staging: a90864fa... (41,024 B), unchanged — CTTR's stray menu sit left no trace. dk_diag.txt = fifofast only (9 B, aef842a6). Config.toml untouched.
- Flight: launched 01:10:56Z. Clocks by dbg readback verbatim: "clk cpu/gpu/mem 1020.0/384.0/1331.2 MHz STOCK". [dk] line: fifofast=1, safepoint=0, all instruments off.
- VERDICT F1-FREEZE: the build presented exactly 2 guest frames and parked. "[vi] seq -> logo (frame 2)" is the last transition; watchdog read "2 guest frame(s) presented, VI black=0" at 12 s and again at 30 s; captures are black with the "1 shader compiling" badge at 24,077 B. The watchdog fiber dump is register-for-register the convicted signature: single thread 0x8026D568, state=2, queue=0x0, lr=0x800166B0 (fn_800166A0+0x10 — the VIWaitForRetrace park), stack +14 = 0x800BF41C (fn_800BEFDC+0x440). No intervention was attempted. No load, no patrol, no route figure.
- Reading: the check-free regenerated tree (Phase 0, e7dbc2dc) FLEW; the same regenerated tree WITH the translator-emitted checks freezes identically to the hand-emitted shard builds (pgo-use cc38f942 twice, the same-tree B-4 default, the countdown build). The poison is the safepoint checks' compiled presence at the boot junction — now spread across 761 functions instead of one shard — i.e. placement/layout coupling, not check density and not the six hand sites specifically. Per the proving programme's gate, F2/F3 do not proceed on this result; the emission shape itself is the open problem.
- Closeout (strengthened standard): HOME -> X -> Close (dialog focus read from capture) -> A. The exit ran through a "Closing software..." spinner (~40 s; three polls during it still read running=1) before three consecutive game-status polls read running=0; the wedged-exit error dialog was then dismissed with A under the claim; two final captures show clean HOME with no "Playing" badge. stderr.log pulled to h3-test\stderr-routea-f1.log (30,187 B, sha256 04bd518c...). crash.txt at baseline (1,735 B, 9d83c322...; no overwrite, no restore needed). Save re-verified unchanged (a90864fa...). Resident 34585267 restored to the card with read-back hash match. dk_diag.txt deleted (listing-verified). Claim deleted via raw FTP DELE (listing-verified). Release relays sent to the SPM, CTTR and JSRF chats.

## Route A variant-scoped emission (Nov 19, 2026) — switch built, tree flipped check-free, pgo-use rebuilt

Ruling (Brandon, delegated call): safepoint checks are emitted ONLY for the capture variant; default and pgo-use trees are generated check-free. Full record: ROUTE-A-VARIANT-SCOPE-LOG.md.

- **Generator switch**: a `--safepoints` flag on translate-recursive (CommandSpec + usage + HasFlag, the CLI's existing conventions), threaded BuildEmissionOptions → TranslationOptions.EnableSafepoints (new field, default false) → FunctionTranslator.EmitCxx → EmitWithFacts `enableSafepoints = false`, gating the planner call (off = SafepointPlan.Empty). Chosen over an env var (the translator has no env-var convention anywhere) and over the recomp.yml project policy (per-project and shared with mods; the ruling is per-regen). Default OFF: a plain regen can never silently produce a with-checks tree; State B now requires the deliberate `vs-translate.ps1 -Safepoints`. Files: Program.cs db535ca1…, FunctionTranslator.cs dea437fc…, CxxLinearCodeGenerator.cs fe4da492…; pre-switch state in variant-scope-pristine\. Translator rebuilt explicitly (0 warnings / 0 errors; Cli d0615271…, Core b5a70050…); tests 58 passed / 0 failed.
- **Flip (State A)**: frozen-table in-place regen, flag OFF (vs-shards.log: reused frozen table). Audit: vs the Phase 0 baseline census **6,193/6,193 units identical, 0 differing** (the four hand-patch units back at baseline contents; GuestSafepointServe/Requested grep over the generated dir = 0); vs the Phase 2 with-checks census exactly 761 differ = 756 pure check removals + the 5 known suffix renames reversed, 0 unexplained; exactly 1,300 check lines removed. Translation identity restored (base_translation_output.json e975c4aa…, sources.bin dd7be7df…). vi.cpp untouched (995e108c…).
- **Default build — decisive gate PASSES**: claim 2026-11-19T02:09:32Z, released after; `--deko --out none` BUILD_EXIT=0; NRO **e7dbc2dc** (37,271,749 B), byte-identical to Phase 0's build / bisection Arm C. Staged h3-test/no100f-nx-checkfree-default.nro, read-back verified. Not flown (it is the Phase 0 flight's exact NRO).
- **PGO-use build**: curated trim manifest verified 66fe061a… before and after; claim 2026-11-19T02:12:21Z, released after; `--deko --variant pgo-use --out none` BUILD_EXIT=0. Fit check verbatim: "[pgo-fit] profile: 14037 functions; out of date in this build: 0 (0.0%, limit 25%); matched: 14037; compiled without profile data: 0 (informational)" / "[pgo-fit] OK: 14037 of 14037 profiled functions fit this tree." — 0 stale, not the ~761 estimated (the profile was trained on the B-4 hand-check lineage; no planner-tree profraw exists, and nothing in the profile is stale against the check-free tree). NRO **39b5736c** (37,300,421 B) staged h3-test/no100f-nx-pgo-use-v2.nro, read-back verified. Not flown; the pricing A/B is the console step under the programme's rule.
- **Tree state**: live tree ends in State A (check-free). State B is reproduced by a flag-ON frozen-table regen; its builds are already staged and untouched here (capture 7f3c9baf at h3-test/no100f-nx-routea2-cap.nro, hash re-verified). Downloads staging untouched (53dbb7bb). LOQ claim free at the end.

## FLIGHT F2 — RESIDENT RE-BASELINE ON THE NEW PATROL ROUTE (Nov 18-19, 2026)

VERDICT: F2-BASELINE — new-route resident baseline 46.77 ms/frame (21.4 fps), stock
clocks, 7 x 600-frame windows (frames 10802-15001, 4,200 frames). The old-route
anchor (43.74) belongs to the old route; 46.77 is the comparison anchor for every
future new-route A/B. Full record: ROUTE-A-F2-BASELINE-LOG.md.

QUEUE: CTTR's r79 CT-3 claim was live at gate entry (since 01:16:22Z); zero writes,
queue notice sent, stood by read-only ~1 h 46 min until it cleared (~03:03Z).
Claim taken 03:03:36Z, read back verbatim. One standby check was killed by a
service restart and one /tmp wipe; both recovered with plain re-checks.

GATE: clean HOME, battery 100%. Focus incident: gate capture showed the CRASH tile
focused (CTTR's r79 left focus there). A bare sys-agent `press DRIGHT` LATCHED and
auto-repeated focus to the ">" button; explicit releases freed it and clean `click`
steps walked singly to Scooby. BANKED LESSON: on HOME navigation use `click` only;
bare `press` latches until an explicit release. Card verified pre-flight: resident
34585267 (37,193,925 B, sha 34585267...), save a90864fa... unchanged, Config
6a629806... untouched, crash.txt baseline, dk_diag written exactly `fifofast`.

CLOCKS: first launch read GPU-307.2 LOTTERY (1020.0/307.2/1331.2); closed cleanly
and relaunched ONCE; second launch STOCK 1020.0/384.0/1331.2 verbatim.

LOAD: in flow throughout. PLUS holds to title then menu; one 0.1 s stick step
(-Y) NEW GAME -> LOAD GAME (brightfrac 0.1046 vs ~0.000); sys-agent B
press/release confirms first try (Slot A; 10% save "The Mystery Machine"
11/14/26 on top); card scan completed; spawn capture-verified at Mystic Manor.

ROUTE: marks route-start-f2 (guest 414 s) / route-end-f2 (605 s), 191 s span; 12
legs DOWN/LEFT/UP/RIGHT x 3, 15 s each, 0.5 s centred between. End capture shows
Scooby at the manor porch (close-in view).

WINDOWS: 47.47, 49.73, 40.91, 51.20, 43.53, 47.49, 47.06 ms/frame; mean 46.77
(21.4 fps); range 40.91-51.20. GX busy 11.78-17.04 ms across windows, view-varying.
Old 43.74 -> new 46.77 (+3.03, +6.9%): the new route is the harder, wider view, as
Brandon predicted (his hands-on read: ~16-17 fps widest, ~20 close in; slowest
window 51.20 ms = 19.5 fps). Per-leg attribution is NOT possible from 600-frame
windows (each straddles ~2 legs); the window spread is the view-dependence
evidence.

CLOSEOUT: strengthened standard (3x running=0 + two clean-HOME captures); resident
re-verified in place; save unchanged; crash.txt baseline; dk_diag deleted (250 OK,
verified absent); stderr-f2.log pulled (209,815 B, sha 18c5ec40...); claim deleted
(250 OK, verified absent); relays sent to SPM/CTTR/JSRF.

## ARM U RE-FLY (pgo-use v2, 39b5736c) — VERDICT: ADOPT — 2026-11-19

**Verdict: ADOPT.** pgo-use v2 (check-free tree, B-4 training profile, fit
14,037/14,037 at 0.0% stale) flies the boot, the load, and the full patrol
unaided and prices **39.15 ms/frame (25.5 fps) vs the F2 resident baseline
46.77 ms/frame (21.4 fps): −7.62 ms/frame, −16.28%**, clean parity. The
programme's pre-set rule (adopt at ≥5% with clean parity) is met with
margin. This reverses the Nov 18 rejection: the original arm U (cc38f942)
parity-failed at frame 2 on the poisoned tree; the variant-scoped rebuild
removes the poison and the build flies. Full record:
`no100f-h3-evidence/AB2-V2-LOG.md`; log `switch-logs/h3-test/stderr-ab-u2.log`
(202,566 B, sha ce5db957…).

Flight facts: queue behind CTTR's r80 claim (live 03:23:22Z; zero writes,
stood by read-only ~45 min until r80 released ~04:08Z). Claim taken
04:08:51Z, read back, re-verified before launch. HOME focus sat on Crash
(CTTR leftover); one `click DRIGHT` moved it to Scooby, capture-verified.
Battery 94% at gate, 89–90% at closeout. Clocks certified STOCK
1020/384/1331.2 on the first try (no lottery recycle). Parity passed: title
screen rendered; watchdog 12 s: 257 frames, 30 s: 801 frames; [vi] 602–1201
at 33.17 ms/frame. Load unaided (10% save "The Mystery Machine", Slot A;
one 0.1 s stick step refused, second landed LOAD GAME); spawn
capture-verified at the Mystic Manor front yard; end capture shows Scooby at
the side porch close-in. Marks route-start-u2 at guest 290 s, route-end-u2
at guest 557 s; route span is the 12 × 600-frame windows 7802–15001 (7,200
frames): 41.38/42.92/39.85/35.03/37.54/44.04/40.30/34.78/36.91/43.81/37.65/35.64,
mean 39.15, range 34.78–44.04, gx busy mean 12.99 ms (F2: ~14.9 ms).
Robustness: the 7 windows overlapping F2's exact span mean 39.02 (−16.57%);
the first 7 of my span mean 40.15 (−14.15%). The span holds 12 windows vs
F2's 7 because my inter-leg gaps ran ~3.5 s (ssh round-trip + sleep 1) not
0.5 s, plus turn-to-turn latency frames; the gap frames are stationary
gameplay at mixed yard views. Closeout to the strengthened standard (3×
running=0, two clean-HOME captures): resident 34585267 restored read-back
matched, save a90864fa… unchanged, crash.txt at the 1,735 B baseline,
dk_diag deleted, claim deleted via raw FTP DELE and verified absent,
relays sent to SPM/CTTR/JSRF. Adoption of pgo-use v2 as the shipping
profile is the next programme decision.

## ROUTE A F3 — CAPTURE JUNCTION RIDE — VERDICT: F3-CROSSED — 2026-11-19

**Verdict: F3-CROSSED.** The load completed via translator-emitted
safepoint episodes on the Phase 2 capture build 7f3c9baf (55,920,837 B;
1,300 check lines, 761 functions). The wedge formed at frames=6687 after
the card scan; four episodes (32 doses) carried counter 0x80271024 from 0
to 140 across func_800D875C's delay; the delay completed, the counter
reset, frames resumed, and Scooby spawned at the Mystic Manor front yard.
The B-2/B-3/B-4 crossing shape reproduces on planner-emitted checks. Full
record: `no100f-h3-evidence/ROUTE-A-F3-FLIGHT-LOG.md`; log
`switch-logs/h3-test/stderr-f3.log` (8,433,895 B, sha b909ec91…).

Flight facts: floor taken after CTTR's r81 released. Claim taken
05:26:37Z, read back, re-verified before launch; Scooby tile
capture-verified focused before the launch press. Battery 97% at gate,
97-98% at closeout, charging. Clocks STOCK 1020/384/1331.2 by readback
(function boot, not clock-gated). Front-end in flow to frames ~5,448;
menu steps needed 3 s full-tilt stick holds (short steps and D-pad
failed); slot A + single file confirmed; card scan completed. Wedge
t=926796ms, frames frozen 6687, retrace + ai_cb frozen together; ~35 s
to frames resuming at t=961731ms (6690). Episode table: EP1 0->36, EP2
40->76, EP3 80->116, EP4 120->136 then the guest ticked 136->140 on its
own, counter reset, doses 5-8 fed the fresh wait 0->16 (each dose +4;
the guest ticks +4 between dose 4 and dose 5 and between episodes, so
each episode nets +40). Heapwatch 0 trips. Parkwatch 1,433 fires.
Route: marks route-start-f3 / route-end-f3, 12 legs DOWN/LEFT/UP/RIGHT
x 3, 15 s each; frames 6954 -> 7992 (carried cadence, coverage not a
price); end capture shows Scooby displaced in the yard. Closeout
anomaly: press HOME + 0.5 s + release opened Quick Settings (long-press
read); atomic `click HOME` is the way, and a suspended title still reads
running=1, so running=0 needs the HOME-menu Close Software flow (X,
dialog, A). Closeout to the strengthened standard (3× running=0, two
clean-HOME captures): resident 34585267 restored read-back matched,
save a90864fa… unchanged, crash.txt at the 1,735 B baseline, dk_diag
deleted, stderr-f3.log pulled, profraw fw-1795066099 banked to pgo\ AND
h3-test (5,680,872 B, sha 9362da0d…, hash-verified both ends), claim
deleted via raw FTP DELE and verified absent, relays sent to
SPM/CTTR/JSRF.

## RESIDENT ADOPTION SWAP (Nov 19, 2026)
Brandon's decision (~01:21 NST Nov 19): pgo-use v2 IS the resident/benchmark.
- Out: 34585267 (37,193,925 B, sha256 345852677c979df9a2a948fee61947f3e682ee1bc6fa918f370fcf9cddaeaa45)
- In: 39b5736c (37,300,421 B, sha256 39b5736c6d708c304864f2006eb67737d45563e16749e298a418e4d746cd37d0), source C:\dev\no100f-nx\switch-logs\h3-test\no100f-nx-pgo-use-v2.nro
- Queue: stood by behind CTTR r81 (synth-mip premise boot, claim since 04:27:16Z) then F3 (capture junction ride, claimed 05:26:37Z, verdict F3-CROSSED); claimed 06:15:53Z, read back verbatim.
- Precondition verified before the write: card held 34585267 (size + sha match) - F3's closeout restore confirmed.
- Swap verified by read-back: card NRO 37,300,421 B, sha 39b5736c exact match.
- Alongside: save 78-GIHE-ScoobyDoo00.gci 41,024 B / a90864fa... unchanged; Config.toml 3,111 B untouched; crash.txt 1,735 B baseline sha 9d83c322...; dk_diag.txt absent.
- No launch. Release: 3x running=0 polls; two clean-HOME captures (no Playing badge, Scooby tile focused, battery 99%); claim deleted via raw FTP DELE (250 OK), verified absent in a fresh listing. Relays sent to SPM/CTTR/JSRF.
- 34585267 backup kept at h3-test/no100f-nx.nro.bak-34585267 (NOT deleted).
- New-route anchor: 39.15 ms/frame (25.5 fps). Old anchors retained for history: 46.77 (resident, new route) / 43.74 (old route).

## PHASETIME FLIGHT (pt-cap 431d0999) — VERDICT: INSTRUMENT CLOSES, PROFILE MISSED THE ROUTE — 2026-11-19

**Verdict: instrument trusted, route split is span-level only.** The
`[phase]` identity closes exactly in all 7 completed windows (game =
pre-submit + submit within ±0.01 ms; tail = busy − gx − named − audeliv
within ±0.02 ms). But the route produced no completed 600-frame window
(207 route frames; window 4202–4801 stopped at 4673 frames) and the
profile sampled the title/demo window (uptime 150–270 s), not the route.
Full record: `no100f-h3-evidence/PHASETIME-LOG.md`; log
`switch-logs/h3-test/stderr-pt.log` (4,601,566 B, sha 87e7a8b9…).

**Route split (frames 4433–4640, n=90 slow-frame phase spans):** in-frame
game 66.02 ms/frame (sd 4.02) = pre-submit 0.84 ms (1.3%) + submit 65.18 ms
(98.7%); of submit, Aurora process (GX) 20.90 ms (31.7%) and
submit-minus-Aurora 44.27 ms (67.1%) = traversal + decode + DL playback +
interleaved guest, jointly; named ~0.14 ms. Headline: the route frame is
submission-span-bound, not pre-submit-bound — the first GX packet arrives
~0.84 ms after the present, and the ~45 ms non-GX busy sits inside the
submit span. The function-level split (update vs traversal vs decode vs
audio vs other-threads) cannot be separated without a route-window
profile — that leg is owed a re-fly (suggested profile window after
spawn, e.g. `700:240` on this boot's timing). Audio host delivery
unmeasured on route (title windows 2.87–4.10 ms); other-threads 0.00 in
all 7 windows. No route frame-time price is claimed (capture build,
carried cadence: 207 frames / 187.4 s = 1.10 fps wall; the 66.02 ms is
in-frame work, not comparable to the resident's 39.15 in-flow mean).

**Title-screen shape for contrast:** windows 2402–3601 run pre-submit
24–27 ms + submit 28–31 ms — the title DOES front-load ~25 ms before the
first packet, the opposite of the yard route. Phase shape is
scene-dependent.

**Profile (missed):** 59,724 samples, 9.1% translated, top leaves 60.1%
elf+1F7AA58 + 16.2% elf+1F7AB28 (host park/idle wait), 1.2% YUV_32_4x2_even,
1.1% PPCMfhid2. No addr2line join done (wrong scene).

Flight facts: claim taken 11:54:03Z (file ABSENT at gate + running=0 +
clean-HOME capture); first launch GPU-307.2 LOTTERY, closed and relaunched
ONCE → STOCK 1020/384/1331.2 ([dk] + [server] agree). Front-end drew the
early cliff (parkwatch from ~frames 3863); four lone PLUS holds each
landed in the attract demo (~60 s timeout) and only returned to title —
the menu opened on a CHAINED double-PLUS. Menu brightfrac-gated (NEW GAME
0.0850 → LOAD GAME 0.0874, one stick step); confirms by sys-agent B pairs
(4 s holds); Slot A 0.1831; file "11/14/26 10% / The Mystery Machine".
Load wedge: 4 episodes, 32 doses, counter 0→140 (F3 shape), frames
resumed by t=604110 ms; spawn capture-verified. Route: 12 legs
DOWN/LEFT/UP/RIGHT × 3, marks route-start-pt 746 s (frames 4433) /
route-end-pt 933 s (frames 4640); end capture shows Scooby displaced.
Stray route-start-pt mark at 734 s (frames 4421) from a pre-check slip —
true start is the second mark. Heapwatch 0 trips. Closeout to the
strengthened standard (3× running=0, two clean-HOME captures): resident
39b5736c restored read-back matched, dk_diag deleted, crash baseline,
save unchanged, profraw fw-1795089563 banked to pgo\ AND h3-test
(5,682,440 B, sha 6f36e8fa…), claim deleted via raw FTP DELE and verified
absent, relays sent to SPM/CTTR/JSRF. Note: the brief's identity formula
(game ≈ pre+submit+named+audeliv+tail) double-counts; the design's
closure (game = pre+submit) is the correct check.


## PHASETIME RE-FLY — PROFILE MISSED AGAIN (Nov 19, 2026; log: ~/workspace/no100f-h3-evidence/PHASETIME-REFLY-LOG.md)

VERDICT: PROFILE MISSED AGAIN — the function-level five-way split of
the submit-minus-Aurora 44.27 ms is STILL OWED. The profile window
`700:240` sampled uptime 700–940 s, frames 1166–1433 — the title screen
and the attract demo. The game never got past the title/demo: the menu
never opened, the load never started, no wedge, no route. The
instrument closes exactly in both completed windows (game = pre+submit
diff 0.00; tail = busy − gx − named − audeliv closes to ≤0.01), so this
is a coverage failure, not an instrument failure — second time.

- Profile: 119,541 samples at ~498 Hz; 2.5% in translated code, 6,020
  functions. Top leaves: 85.4% elf+1F7AA58, 2.6% elf+1F7AB28 (host
  park/idle — the game was parked ~97.5% of the window). No addr2line
  join (wrong scene).
- WHY IT MISSED: (1) the front-end ran parked from frame 0 this boot
  (parkwatch 447 fires), ~100+ s later than flight 1's timing; (2)
  PLUS produced ZERO menu opens and ZERO demo exits across 9 attempts
  (chained double, 10 s hold, 8 s / 20 s dbg holds), while B exited
  the demo twice — a new anomaly vs flight 1, where the chained
  double-PLUS opened the menu. Padwatch logged only boot lines.
  (3) profile.txt is read ONCE at the first present (guest_profiler.cpp
  static guard) — the brief's "arm it from the spawn capture" is NOT
  implementable; the window must be pre-set and drive timing varies
  boot to boot by minutes. Capacity note: a window longer than ~264 s
  at 500 Hz silently samples only its first ~264 s.
- [phase] windows (logo/title/demo only): 2-601: 321.68 ms/f, game
  321.39 = busy 42.53 + waits 278.86; pre 34.22 + submit 287.17
  (aurora 7.57); tail 11.50. 602-1201: 894.14 ms/f, game 893.88 =
  busy 43.99 + waits 849.89; pre 33.96 + submit 859.92 (aurora 7.91);
  tail −21.75 (audio delivery overlapping idle — arithmetic closes,
  not an instrument error). No route price claimed.
- Flight: pt-cap 431d0999 (55,929,029 B), 11-line dk_diag (+phasetime);
  clocks STOCK first launch (1020/384/1331.2); claim 12:25:32Z;
  watchdog 12 s: 171 frames, 30 s: 419 frames; heapwatch 0 trips;
  closeout to the strengthened standard (3× running=0, two clean-HOME
  captures); resident 39b5736c restored read-back matched; dk_diag +
  profile.txt deleted (raw FTP DELE, verified absent); crash baseline;
  save unchanged; stderr-pt2.log in h3-test (2,982,678 B, sha
  97704d13…); profraw fw-1795091232 banked to pgo\ AND h3-test
  (5,682,440 B, sha f2a5314f…).
- THIRD FLIGHT NEEDS: (1) a later window (~1150–1200 s start, since
  this boot's route would have started ~1250+ s) — but any fixed
  window is a gamble; (2) the PLUS/menu-open anomaly resolved first
  (desk look at the title-screen input path and whether the parked
  regime starves it) — until the menu opens reliably, no window
  placement helps. Parent decision, not yet dispatched.

## DEFAULT RE-VERIFICATION (phasetime tree) — VERDICT: GATE-PASS — 2026-11-19

**Verdict: GATE-PASS.** The check-free default build from the phasetime
tree flies the load uninstrumented at stock clocks. The phasetime runtime
did not change boot timing in a way that breaks the load; the tree is
shippable as-is. Full record:
`no100f-h3-evidence/DEFAULT-REVERIFY-LOG.md`; log
`switch-logs/h3-test/stderr-rv.log` (167,462 B, sha a92bc865…).

**Phase A — build + audit:** `--deko --out none` from the live tree
(12:26:49Z → 12:28:55Z, BUILD_EXIT=0; 0 "pgo" lines). NRO 67b587ec
(37,275,845 B) staged to `switch-logs/h3-test/no100f-nx-default-rv.nro`
only, read-back hash match. Audit: generated tree check-free at Phase 0
identity (safepoint grep 0, base_translation_output.json e975c4aa,
base_translation_sources.bin dd7be7df); source delta vs the pre-phasetime
backup is phasetime runtime only (7 files, 347 diff lines, all key-gated);
binary strings "phasetime" ×4, "[phase]" ×1; size delta vs e7dbc2dc
+4,096 B exactly. Build claim taken 12:55:00Z, released after the build;
board-noted. Downloads staging still 53dbb7bb.

**Phase B — gate flight:** stood by ~25 min behind the phasetime re-fly
(claim since 12:25:32Z); claimed 12:50:30Z, read back, re-verified before
the launch press; Scooby tile capture-verified focused. Clocks STOCK
1020/384/1331.2 first try (no lottery recycle). [dk] line fifofast=1,
everything else 0 (safepoint=0, phasetime=0 — runtime dormant).
Watchdog 12 s: 251 frames; 30 s: 794 frames (Phase 0 flew 249/792) —
no frame-2 freeze. Front-end formed (logo splash, title screen). Load:
menu opened on the second chained double-PLUS (the first pair's result
was the demo; see anomaly 1); 0.1 s stick step NEW GAME → LOAD GAME
(first try); sys-agent B press/release confirms first try (Slot A, file
"11/14/26 10% / The Mystery Machine" on top); load-confirm-rv mark at
291 s; spawn capture-verified at the Mystic Manor front yard. Patrol:
one cycle DOWN/LEFT/UP/RIGHT, 15 s legs, marks route-start-rv 333 s /
route-end-rv 396 s; end capture shows Scooby displaced at the manor side
porch — genuine traversal. Gate only; no [vi] price claimed.

**Anomalies:**
1. The menu idled back to the attract demo between the first successful
   menu open and the stick step (~90–100 s wall; the "~150 guest-s" idle
   figure is approximate, and a service restart on the VM mid-step
   stranded the drive). The chained double-PLUS reopened it; the step
   then landed first try. Lesson: drive the menu promptly once open —
   do not leave it sitting between steps.
2. A VM service restart mid-flight wiped /tmp (proxyhost.txt, helper
   scripts, local capture copies). The LOQ-side scripts and captures
   survived; the proxy host was re-derived from $HTTPS_PROXY and the
   drive resumed from a fresh probe capture. Lesson: keep helper
   scripts in WSL home or re-scp them; never assume /tmp survives.
3. The first menu-open pair's PLUS presses seemingly did nothing (demo
   still playing on the capture) — consistent with the re-fly's new
   finding that PLUS can be dead on some boots while B still exits the
   demo; on this in-flow boot the chained pair worked on the second
   attempt. The menu-open retry is the working recovery, not a new
   mechanism.
4. The re-fly's MEMORY.md note (added during this flight) records its
   own anomaly: PLUS did NOTHING across 9 attempts on its
   parked-from-frame-0 boot while B exited the demo twice. My boot was
   in flow (251/794 frames), so the two observations are not the same
   regime; the title-screen input path + parked-regime starvation desk
   study is owed before a third phasetime flight.

**Closeout (strengthened standard):** atomic `click HOME` → X → A;
running=0 on the first poll and three consecutive confirms; two
clean-HOME captures (no Playing badge, Scooby focused, battery 100%).
Resident 39b5736c restored read-back matched (37,300,421 B); dk_diag
deleted (raw FTP DELE, verified absent); crash.txt at the 1,735 B
baseline (9d83c322…); save a90864fa… unchanged; Config 6a629806…
untouched; claim deleted via raw FTP DELE, verified absent in a fresh
listing; relays sent to SPM/CTTR/JSRF.

## PLUS ANOMALY — desk study (Nov 19, 2026)

Full study: `~/workspace/no100f-h3-evidence/PLUS-ANOMALY-STUDY.md`.

Question: on the phasetime re-fly the front-end ran PARKED from frame 0
and PLUS did nothing across 9 attempts (holds to 20 s), while B exited
the demo twice and flight 1 opened the menu in flow on a chained
double-PLUS.

Verdict: PLUS is not starved and not unsampled. Padwatch on the re-fly
boot, re-verified against /tmp/stderr-pt2.log (sha256 97704d13...,
exact match), shows 25 padwatch lines: 7 PLUS press edges (btn=0x1000)
and 3 B press edges (btn=0x0100), sampled in BOTH regimes — four PLUS
presses at ~400-525 reads/s during the demo spin-poll, three at ~1/s
during the title phases. The gate is open (padwatch exists, so
func_8004421C returned 1). The pad update and input update ran and
produced the action word. What PLUS never got is a CONSUMER: the guest
code that runs on the re-fly boot honors kA (demo exit) and never acts
on kStart (menu open). The title-menu handler did not execute a kStart
test on this boot. Leading mechanism: wrong-screen consumption — the
wake bursts run the attract/demo consumer (or the second frame step
func_800BF4A8), not the title handler. Second mechanism, unseparated:
edge-phase death — func_80031DDC recomputes the pressed-edge word
(pad+48 = cur & ~prev) on EVERY call, so a held button's edge lives for
exactly one input-update call, and a handler polling in a different
burst than the one that sampled the press sees edge=0. The demo driver
polls AND consumes in the same tight loop (~500/s), so B always lands;
the title handler consumes in a different iteration than the poller.

Corrections to MENU-CONFIRM-STUDY.md (code-verified): (1) gate direction
was inverted — func_8004421C returns 1 (accept) iff W in {7,9,10,11,12},
0 iff W==8, W<7, or W>=13. (2) "No edge computation in func_80031DDC"
is false — loc_80032260 computes pad+48 = cur & ~prev (pressed),
pad+52 = prev & ~cur (released), pad+44 = cur (level). (3) The pending
value the applier consumes is at 0x80239070, not 0x80239060 (table base
0x80239068, +8 field of index 0). (4) C3 sampling does not cover the
re-fly: all 9 attempts were sampled.

Recommendation: NO third blind flight — the re-fly already contains the
strongest PLUS test (500/s and 1/s, holds to 20 s, all sampled, zero
effect) and flight 1 is the in-flow positive control. Deciding test is
the menuwatch (MENU-CONFIRM C4, plus logging func_800BF4A8 entry):
one capture-line build off the cured tree + one title-phase boot, 2x dbg
4 s PLUS holds; readout names the consumer (handler vs second step vs
neither) and picks the fix (dispatch fix vs tween-starvation fix vs
edge-lifetime fix). The input path needs no change in any case. The
phasetime five-way split third flight, when it happens, is the re-fly's
config (pt-cap, window 700:240) with flight 1's pre-cliff in-flow drive
(load ~540 s, route 746-933 s, inside the window) — do not drive the
menu on a parked boot.

## MENUWATCH DECIDING EXPERIMENT (Nov 19, 2026) — VERDICT: WRONG-SCREEN CONSUMPTION, DISPATCH FIX

Build ff6f682f (56,031,429 B; plain capture variant, State A generated + menuwatch hand lines, NO --safepoints regen; LOQ claim 13:45:00Z, released after the build). Instrument: dk_diag-gated menuwatch (SCOOBY_MENUWATCH) — runtime MenuwatchArmed/Note/Edge in vi.cpp + hand-emitted calls at func_800CCBD4 entry, func_800BF4A8 entry, func_80031DDC entry (mwCallerLr) + edge point at loc_80032260. Backups at menuwatch-backup\; tree restored byte-exact after the build (all 6 files hash-match MANIFEST-pre.txt; menuwatch grep 0, safepoint grep 0); trim manifest 66fe061a restored. Staged to h3-test/no100f-nx-mw-cap.nro only; Downloads untouched (53dbb7bb).

Flight: stood by ~50 min behind CTTR r85 (queue notice sent); claimed 14:00:36Z on its release. HOME gate caught focus on the CRASH tile (r85 leftover); one click DRIGHT + capture-verify put the Scooby tile focused before launch. Clocks STOCK 1020/384/1331.2 first try. dk_diag = ten standard keys + menuwatch; phasetime OFF, profile.txt ABSENT. Title logo verified; demo playing by pre-h1; parked/carried cadence throughout. Drive: 2x dbg 4 s PLUS holds, spaced; no load; boot ended at the title/menu as briefed.

READOUT (full stderr-mw.log, 105,337 lines, 6,560,613 B, afa7da83…; only 5 menuwatch instrument lines in the whole boot):
- step=0x800BF4A8 lr=0x800BE5F4 W=8 pend=0x2 — the ONLY step entry. func_800CCBD4 NEVER RAN (zero entries). Its caller is func_800BE410.
- Hold 1 press: edge poller=0x800DACA4 act=0x1 (kStart) — consumed by func_800DA8F8 (stream/demo driver; call site shard_9e6d:21742, loc_800DAC98). Did nothing.
- Hold 1 release + hold 2 press/release: edge poller=0x800BF870 act=0x1/0x0 — consumed by func_800BF4A8 (second frame step; call site shard_4c50:10197, loc_800BF85C). Hold 2's kStart OPENED THE MENU (capture-verified: NEW GAME / LOAD GAME / OPTIONS / THEATRICAL TRAILER / CREDITS).
- Padwatch: both holds sampled exactly once each (press 0x1000 / release 0x0000; t=325419/329456 and 340093/344122 ms). W=8 and pend=0x00000002 at every sample.

VERDICT: wrong-screen consumption CONFIRMED. In the demo the kStart goes to the demo loop (800DA8F8) which ignores it; on the title it goes to the title loop (800BF4A8) which opens the menu. The title handler under 800CCBD4 never runs on this path — the study's §1 item closes in the negative; the consumer is 800BF4A8. Edge-phase death REFUTED (each press: exactly one edge, consumed exactly once, correct action word). Tween-starvation RULED OUT (menu opened with no handler/tween involvement). The gate is NOT the blocker (W=8 throughout, menu opened anyway). FIX: dispatch — the demo loop must route kStart to the title path (exit the demo to the title on kStart, as on kA), or the main loop must give the title consumer the input while the demo is up. The input path needs no change.

OPEN (one item): whether hold 1's kStart itself exited the demo or the demo ended naturally during hold 1's window is NOT separated — the consumer switched between hold 1's press and release edges, and wall-time estimates put the demo's ~200 s natural end in the same window; the log has no screen-transition marker. The re-fly's 9 dead PLUS attempts favor natural end. Discriminator: one boot, single PLUS hold early in the demo, screen capture immediately after release (title up = demo loop exits on kStart; demo still playing = it ignores it).

Closeout verified: 3x running=0 + two clean-HOME captures (10:41, battery 100%, no Playing badge); resident 39b5736c restored read-back exact; dk_diag deleted (raw FTP DELE, listing-verified); crash baseline; save a90864fa; Config untouched; claim deleted listing-verified; relays sent. Profraw fw-1795096970 (5,672,360 B, db409fb7…) banked to pgo\ AND h3-test (bank total 44; title-phase only, not part of the 8-file Class E set). Consequence: the phasetime third flight must drive the menu in flow and must not rely on PLUS from the demo — the demo eats the first PLUS; the chained double-PLUS works only because the first press exits (or outlasts) the demo and the second lands on the title. Full record: no100f-h3-evidence/MENUWATCH-LOG.md.

## GATE REVISIT (desk study, Nov 19, 2026; GATE-REVISIT-STUDY.md)

The PLUS study's §5 correction #1 is itself WRONG and is reversed: func_8004421C
returns 1 iff W ∈ {7,9,10,11,12} (values traced correctly), but nonzero means
SKIP, not accept — the HLE pad update returns without reading the pad on nonzero
("nonzero in the game states that skip pad input"), and four of five guest-side
callers treat nonzero as skip/don't. Correct mapping is the ORIGINAL MENU-CONFIRM
C2 one: pad input SKIPPED iff W ∈ {7,9,10,11,12}; READ iff W==8, W<7, or W≥13.
The menuwatch's W=8 is therefore no paradox: W=8 is an ACCEPT state. The gate is
a state-class predicate with five callers (HLE pad update; 800BF4A8 ×2, timer only;
800D9170 flag bit; 800D9770 guarded action; 800DA8F8 ×2, nonzero → apply pending
state), and it does NOT sit on the menu-open path: in 800BF4A8 the input update
runs unconditionally (lr=0x800BF870), and the pending/applier path is a no-op for
the observed states (pending=2 and W=8 both map to the applier's return-0 case;
jump table at 0x801C5120 read from main.dol: states 2,3,6,8 → 0x800443A8).
W is the frontend state ID 0–12 (setter func_80044250; companion 0x80270A74: 6 =
freshly set, 5 = unchanged); 8 is the frontend steady state (title/demo/menu),
12 is set by the DVD callback (load transition). Second correction: func_8004427C
writes ONLY slot 0 (0x80239070), ONLY on r3 == 0 — the PLUS description was
inverted too. PLUS §5 corrections #2 (edge computation) and #3 (pending at
0x80239070) re-verified and stand. Drive rule: never judge a confirm/menu-open
by W; judge by padwatch (edge sampled?) and the menuwatch poller (800BF4A8 vs
800DA8F8). Open: exact screen name per W value; what 0x801119E0 tests (W=8
selector); the 800BF4A8 step-entry count puzzle (1 logged entry vs 3 edges);
the exact kStart test line inside 800BF4A8.

## PHASETIME FLIGHT 3 — PROFILE MISSED, THIRD TIME; ABORTED PER PRESCRIPTION (Nov 19, 2026)

Verdict: the function-level five-way split is STILL OWED. The measurement boot parked from (nearly) frame 0 — watchdog 165 frames at 12 s / 186 at 30 s (flight 1 in flow: 251/794); parkwatch fire #40 by t≈120 s, 124 fires total; frames 2 at ~1 s → 285 at 120 s → 351 at exit (t=186 s), retrace at full rate throughout. Screen at the abort probe (uptime 124 s): THQ logo, still in the logo sequence. Per the brief's prescription (never drive the menu on a parked boot) no menu drive was attempted, no load, no wedge, no route; profile window 700:240 never ran (uptime at close 186 s); 0 [phase] windows, 0 profile samples, 0 safepoint episodes, heapwatch 0 trips. The demo-exit discriminator passenger was NOT run (aborted before the title phase).

First launch was the GPU-307.2 lottery — closed cleanly (3x running=0, clean HOME) and relaunched ONCE to STOCK 1020/384/1331.2. Claim 14:17:21Z; gate clean HOME, Scooby focused, battery 100%; card pre-checks all exact (save a90864fa, resident 39b5736c, Config 6a629806, crash baseline). Closeout verified to the strengthened standard (3x running=0, two clean-HOME captures 10:54, battery 100%); resident restored read-back exact; dk_diag + profile.txt deleted via raw FTP DELE, verified absent; save/config/crash untouched; stderr-pt3.log pulled (549,259 B, f665f92c…); profraw fw-1795098040 (5,682,440 B, 1616e1de…) banked to pgo\ AND h3-test (bank total 45); lottery-boot fw-1795097956 NOT banked per the last-only rule; claim deleted listing-verified; relays sent; LOQ scratch cleaned.

The early park is a BOOT LOTTERY, not a build property: all three phasetime boots flew the same pt-cap build 431d0999 — flight 1 in flow to frame 3863, re-fly and flight 3 parked from frame 0. A fourth flight with the same prescription is another gamble on the early park, not a drive refinement. What changes the odds is understanding/fixing why the cliff draws in the logo phase on some boots, or accepting the slow-frame spans as the route instrument (flight 1 already gave the span split; only the function-level join needs a route-window profile, which needs an in-flow boot). Fourth-flight dispatch is the parent's call. Demo-exit discriminator still open. Full record: no100f-h3-evidence/PHASETIME-FLIGHT3-LOG.md.

## EARLY-CLIFF DESK STUDY — TRIGGER FOUND, PREDICTOR FOUND, NOT PURE LOTTERY (Nov 19, 2026)

Full record: no100f-h3-evidence/EARLY-CLIFF-STUDY.md. Desk only; evidence is the three raw phasetime stderr logs (pt / pt2 / pt3), all the same build 431d0999, EAGER regime (seamfix=0 fix2=0, gxheld_hw=0 throughout — the CLIFF-STUDY held-deque mechanism does NOT apply here).

Verdict: the early park is a legitimate pipeline-full sleep into a dead relay. In all three boots the fatal sleep was the FIRST display-queue sleep of the boot, with the same micro-sequence: a non-catch-up BB0 advancer run (A+1, diff=1, flag stays 1), then the presenter presents one more frame (diff back to 2) WITHOUT a GXEnableBreakPt call (breakpt = frames−1 at the park: 3862/3863, 409/410, 164/165), then the waiter parks with arm=0 latch=0 d4=0 flag=1 and no AA4 in flight. After the park breakpt freezes, AA4 traffic stops, and the advancer never runs again (post-park wakeups are all parkwatch actuations). The PARK-STUDY Q3 lost-wakeup race is REFUTED for these boots: the sleep snapshots show diff=2 (a live wait state), so no advancer run occurred in the fatal restore gap. Parks drew at three different points: flight 3 at the WBLogo end drain (frame 165, t≈11.4 s), re-fly at the THQLogo end drain (frame 410, t≈21.1 s), flight 1 mid-demo1 (frame 3863, t≈185 s, ~31 s into the demo, no boundary event) — the movie drain is a high-hazard moment, not the trigger. Corrections: flight 1 watchdog is 169/601 (not 251/794 — those are the default-reverify counts); the brief's "cliff at ~34.9 s guest time" is wrong, the park was at t≈185 s wall.

Predictor: the 5-second frame rate at 25→30 s separates cleanly — in-flow ~106 frames/5 s (flight 1), parked 6–8 (re-fly, flight 3); threshold 25 frames/5 s. The 12 s watchdog does NOT separate (169/171/165). Probe spec in the study (Q1).

Recommendation, in order: (1) RUNTIME FIX — parkwatch-cure: in the parkwatch actuation (vi.cpp), on a confirmed park, write 0 to the flag word 0x802716D0 before waking the waiter, so the next rescued present re-arms the breakpoint and the relay restarts. One line; verify on one parked boot (breakpt advancing again, AA4 traffic resuming, ~25 fps, no further signatures for ≥60 s). Deliver-before-suspend and wake-latch do NOT cure these parks (no delivery pending, no wake lost); no dk_diag key exists for this. (2) If the fix is deferred, flight 4 flies with the 30 s probe and a re-roll policy (up to 3 launches; ~70% at the observed 1-in-3 in-flow rate, n=3 so the interval is wide). A blind fourth flight with no probe is priced at ~1 in 3 per launch. Passing the probe does not guarantee the route (flight 1 passed 30 s and parked at 185 s), but a late park is survivable — the menu drive still works once the title is up and the profile measures per-frame costs.

## Parkwatch-cure verification flight (Nov 19, 2026) — VERDICT: CURE FAILED (durability)

Build: capture NRO ada044e1 (55,929,029 B), `--deko --variant pgo --out none`, BUILD_EXIT=0, State B
regen. The change is one line in `wiicompiled/runtime/src/hle/vi.cpp` (~2179, parkwatch fire block,
before `OS_HLE_RequestParkWake()`): read flag word 0x802716D0, write 0 to it, log
`[parkwatch] cure: flag 0x802716D0 %u -> 0 before fire #%u (frames=%u)`. vi.cpp 0cf01b35 →
333c7432 (live-tree hash after the State A restore). Live tree restored to State A post-build
(safepoint grep 0, translation identity e975c4aa/dd7be7df); trim manifest 66fe061a restored.
Full record: `~/workspace/no100f-h3-evidence/PARKWATCH-CURE-LOG.md`.

Flight: 3 launches, all STOCK clocks first try. Launches 1–2 ran in flow at the 25–30 s probe
(deltas 139/134 per 5 s) and were closed cleanly. Launch 3 also ran in flow at the probe
(delta 112) — the brief's policy would have ended the flight INCONCLUSIVE — but it was
observed past the probe window (no 4th launch, no forced park) and parked late at frames=3164,
t≈159 s, exactly as phasetime flight 1 did (parked at 3863 after passing the probe). The
policy premise ("the cure never engages" on an in-flow boot) is refuted: two boots now
parked late after passing the probe.

Three parks, three cures on launch 3 (all thread 0x8026D568 state=4 on queue 0x802716E4,
frames frozen 2050 ms, retrace/ai_cb advancing; eager regime, gxheld_hw=0; heapwatch 0 trips):
#1 frames=3164 (A=0 B=2 arm=0 latch=0, breakpt frozen 3163, flag 1→0, resumed 3164→3166 in
55 ms); #2 frames=5089 (A=2 B=1 arm=0 latch=0, breakpt 5089, flag 1→0, resumed 5089→5091 in
52 ms); #3 frames=5093 (A=0 B=2 arm=1 latch=0, breakpt 5094, flag 1→0, resumed 5093→5095 in
53 ms). After cure #1 the boot ran 74 s signature-free at ~26 fps (breakpt 3163→4053→5095,
AA4 flowing) — that PASSES the study's ≥60 s bar for the interval. After cure #2 the relay
died again within 2 frames. After cure #3 the guest aborted 2 frames later on the known
0xc8a10094 descriptor fault (esr 0x92000006, x9=0xc8a10090, x11=0x80270238; crash.txt
preserved as h3-test/crash-pwc.txt, 2,618 B, sha 2eedff28…; baseline restored after).
The flag write cannot have written the descriptor (0x802716D0 vs 0x80270228) — the abort is
the pre-existing front-end defect, reached because the cure let the front-end advance at
full speed; the descriptor-watch remains the discriminator for C1/C2/C4.

Reading: the mechanism is proven 3/3 (flag write → resume in ~55 ms, breakpt advancing,
AA4 resuming, ~26 fps) but the restart is not durable — the gate halves fail in different
combinations across the three parks, so clearing the flag does not repair the arm/latch
coordination, and the inter-death interval collapses (74 s → 0.5 s). Keep the cure as a
rescue (74 s of full-speed running per cure beats ~1 fps parkwatch carrying); it is not
the durable restart. Next steps are the parent's: a second layer on the latch/arm side, the
descriptor-watch, or accept rescue-only and move on to phasetime flight 4 (split still owed).

Closeout verified: game exited on its own (clean userAppExit); 3x running=0 + two clean-HOME
captures; resident 39b5736c restored read-back exact; dk_diag deleted; crash baseline
restored (1,735 B, 9d83c322); save a90864fa; Config 6a629806; stderr-pwc.log pulled
(3,736,557 B, a52fce2a…); profraw fw-1795100442 banked (5,682,520 B, 1fe3cbbc…; bank total 46;
launches 1–2 profraws stay on the card per the last-only rule); claim deleted listing-verified;
relays sent to SPM/CTTR/JSRF. Anomalies: early-park rate 0/3 this session vs the study's 2/3;
the claim file's `since` was written ~37 min fast (15:32:00Z vs actual ~14:55Z); the T+60
snapshot read empty because the game had already exited at t=239.708 s.

## PHASETIME FLIGHT 4 — STOPPED: GPU-307.2 LOTTERY RECURRED (Nov 19, 2026)

Verdict: STOPPED per the brief's clock-gate condition. No measurement flown; function-level
five-way split STILL OWED (fourth miss — this time a clock-gate stop, not a park or a window miss).

- Build: cure capture NRO ada044e1 (55,929,029 B), staged read-back matched. dk_diag exactly
  the 11 lines (ten standard + phasetime); profile.txt 700:240 (never ran).
- Claim 2026-11-19T15:15:13Z, owner Sudo (FrightWorks chat). Gate clean HOME, Scooby focused,
  battery 100%. Read back, re-verified before the launch press.
- Launch 1: dbg `clocks` GPU-307.2 LOTTERY — closed via HOME → Close Software flow (3x running=0).
  Launch 2 (the one permitted relaunch): GPU-307.2 LOTTERY again, confirmed three ways ([dk]
  clocks boot, [server] clocks at 1 s, live `clocks` re-read all agree). Per the brief ("relaunch
  ONCE; if it recurs, report and stop") the flight was closed out. No menu drive, no load, no
  wedge, no route, no profile. Cure engagements: 0 (no park on either boot). Demo-exit
  discriminator passenger NOT run (no boot reached the title phase).
- Closeout (strengthened standard): 3x running=0 + two clean-HOME captures (11:47/11:48 AM,
  battery 100%); resident 39b5736c restored read-back exact; dk_diag AND profile.txt deleted
  via raw FTP DELE (250 OK), verified absent; crash.txt baseline 9d83c322, no overwrite; save
  a90864fa unchanged; Config 6a629806 untouched; stderr pulled as h3-test/stderr-pt4.log
  (170,761 B, de174a04…); NO profraw banked (last-only rule: no measurement boot — both
  lottery profraws stay on the card, as flight 3's fw-1795097956); claim deleted listing-verified;
  relays sent to SPM/CTTR/JSRF. Bank total remains 46.
- Full record: ~/workspace/no100f-h3-evidence/PHASETIME-FLIGHT4-LOG.md.
- Open: (1) split still owed; (2) demo-exit discriminator still open; (3) the cure has still
  not carried a phasetime route — ada044e1 remains the flight-5 carrier. Recommendation to the
  parent: raise the clock re-roll allowance to 3 launches in the flight brief — the
  one-relaunch rule ended a flight a third launch had even odds of saving (same shape as the
  park re-roll policy, ~6 min max pre-drive cost).

## PHASETIME FLIGHT 5 — PROFILE MISSED (window closed before the route; Nov 19, 2026)

Verdict: PROFILE MISSED. The function-level five-way split of the 44.27 ms
submit-minus-Aurora is STILL OWED (third profile miss of the route — flight 1,
re-fly, flight 5). The profile window (700:240, server 700–940 s) closed ~135 s
before the route could start; the route never ran. The demo-exit discriminator
is ANSWERED, and the closeout produced a new banked rule.

- Build: cure capture ada044e1 (55,929,029 B), NO rebuild. dk_diag eleven lines
  (ten standard + phasetime); profile.txt 700:240.
- Claim 2026-11-19T15:24:44Z, owner Sudo (FrightWorks chat). Card pre-checks all
  exact (resident 39b5736c, save a90864fa, Config 6a629806, crash baseline).
  (Save pull fix: literal space in "Card A" breaks curl URLs — use Card%20A.)
- Launch 1: STOCK first try (1020/384/1331.2, [dk] clocks boot + dbg clocks
  agree). No lottery recycle. The "second boot" below was a closeout relaunch
  artifact, not a flight launch.
- DISCRIMINATOR ANSWERED: the demo loop DOES exit on kStart. One dbg PLUS hold
  4 s early in the second demo cycle; padwatch sampled press btn=0x1000
  (t=319334ms) and release btn=0x0000; the immediate post-release capture shows
  the MAIN MENU. Demo was ~30–60 s into its ~200 s cycle, so a natural end is
  excluded. Refinement of the menuwatch: the demo driver (func_800DA8F8)
  consumes the kStart press and exits to the title; the menu opened on the
  release edge via the title consumer (func_800BF4A8). The "demo ignores
  kStart" reading is revised — it exits on kStart, it just does not open the
  menu directly.
- Menu drive: the 4 s dbg stick hold AUTO-REPEATED (NEW GAME → CREDITS).
  Corrected with three short (0.15 s) up-steps, each brightfrac-verified:
  CREDITS → TRAILER (0.1508) → OPTIONS (0.0587) → LOAD GAME (0.0868). Slot A
  confirmed; file list verified (11/14/26, 10%, "The Mystery Machine"); load
  confirmed. Spawn capture-verified (Mystic Manor).
- Load/wedge: the retained logtext ring (17,492 lines, newest t=1,074.9 s)
  contained ZERO [safepoint], ZERO parkwatch, ZERO cure lines — the load
  crossed with no wedge, no park, no safepoint episode, no cure engagement.
  The junction wedge does not form on every load.
- Why the profile missed (honest cause): server time at spawn verification was
  t=1,074.9 s (frames=16,001, in flow); the window ended at 940 s. The window
  covered the load + spawn idle, not the route. Lost to (a) the slow title/demo
  drive (~300 s: two demo cycles, discriminator, menu-stepping correction) and
  (b) my logtext diagnostic detour AFTER spawn verification (~230 server-s)
  instead of starting the patrol immediately. The window was still open at
  spawn verification (t≈842 s). The first boot's profile data file is absent
  from the card (only base_1795103204, the second boot's file, 0 bytes).
- First boot's stderr LOST: stderr.log truncates per boot; the relaunch
  overwrote it. stderr-pt5.log (2,597,064 B, sha 954d7ef7…) is the SECOND boot
  only (one clocks-boot line, [cnt] t=250 ms→176 s). First-boot [phase] spans
  (7 lines counted in the ring) were not captured with values.
- CLOSEOUT ANOMALY — the `click A` relaunch: close1's `click A` on the Close
  dialog closed the game (running=0 on three polls) AND relaunched it — close2
  polls read running=1, captures showed the WB logo then the intro movie, and
  the logtail confirmed a FRESH BOOT (t=113 s, frames=2,445). No other inputs
  in the window; the relaunch is temporally locked to the A click. Best
  reading: the click's trailing edge landed on HOME's focused Scooby tile.
  Re-closed with an EXPLICIT press/release pair (close3): verified by FOUR
  running=0 polls (three immediate + one delayed 30 s re-poll) and TWO
  clean-HOME captures (no Playing badge, battery 100%). BANKED RULE: never use
  `click A` on the Close dialog — use an explicit press A / release A pair,
  then poll, then re-poll after a delay to catch a relaunch.
- Closeout (strengthened standard): resident 39b5736c restored read-back exact;
  dk_diag AND profile.txt deleted (250), verified absent; claim deleted (250),
  verified absent; save/config/crash baseline, no overwrite; stderr pulled as
  h3-test/stderr-pt5.log (second boot only — see above); profraw fw-1795102004
  banked (5,682,520 B, sha 9517735a1791dacfeab9abd045601ec91d2b28b94f0b37705da932ac61a41991)
  to pgo\ AND h3-test, hash-matched. Bank total now 47. Last-only rule held:
  the relaunch boot's fw-1795103204 NOT banked; flight 4's lottery-boot
  profraws fw-1795101375 / fw-1795101448 not banked. Relays sent to
  SPM/CTTR/JSRF. Console free.
- Full record: ~/workspace/no100f-h3-evidence/PHASETIME-FLIGHT5-LOG.md.
- Open: (1) split still owed; (2) flight 6 prescription — drive the menu on the
  FIRST title appearance, use short stick steps from the start, START THE
  PATROL THE MOMENT the spawn is capture-verified (before any log analysis),
  close with an explicit A press/release pair + delayed re-poll, keep 700:240
  if the drive is tight (flight 1's route ran 746–933 s on it) else move the
  window later within the ~264 s cap; (3) check-free default re-verification
  flight still owed (vi.cpp now 333c7432).

## PHASETIME FLIGHT 6 — PROFILE MISSED (Nov 19, 2026)

Verdict: PROFILE MISSED. No route, no five-way split. Three launches, zero
productive boots. Full record: ~/workspace/no100f-h3-evidence/PHASETIME-FLIGHT6-LOG.md.

- Boot 1 (STOCK first try): parked at the title, frames=3166, t~157 s
  (A=2 B=1 arm=0 latch=0). 70 cure fires, +2 frames each, ~1 fps. Screen frozen
  on one black frame (3 captures byte-identical); 0 padwatch lines after boot;
  menu never opened. Closed clean.
- Boot 2 (STOCK first try): parked early, frames=714, t~97 s, logo phase
  (A=1 B=0 arm=0 latch=0). 74 cure fires, +1 frame each. Never reached the
  title. Closed clean.
- Boot 3a (GPU-307.2 LOTTERY, confirmed twice): closed in logo phase. Its close
  produced boot 3b (see the close anomaly).
- Boot 3b (STOCK re-roll): parked early, frames=410, logo phase, first park
  read the study's exact signature (A=0 B=2 arm=0 latch=0). 40 cure fires,
  +1 frame each. Closed clean by the corrected close.

CURE DURABILITY FINDING: the signature does NOT predict cure durability. Boot 3b
parked with the study's exact signature (the verification boot's curable combo)
and the cure still held 0 s (+1 frame/fire). The verification boot's 74 s hold
does not reproduce. The cure is a rescue-only net, and on these boots not even a
usable carry: the guest never runs a PADRead in the rescued frames, so no input
can reach it and the menu cannot open. Driving the menu on such a boot is futile;
the flight-3 prescription (never drive the menu on a parked boot) applies when
the cure holds 0 s.

CLOSE ANOMALY — NEW BANKED RULE: on boot 3a the close script's X click did NOT
open the Close dialog (dialog capture: plain HOME, no dialog, no "Playing"
badge — the title had already exited, see the crash below). The script pressed A
blind and relaunched a fresh boot (3b). Same class of error as the flight-5
`click A` relaunch, one layer up. NEW BANKED RULE (amends the flight-5 close
rule): verify the Close dialog is actually showing — capture, Close focused —
BEFORE the A press/release pair. If the dialog capture shows plain HOME, do NOT
press A; re-check state (HOME click + capture) and diagnose instead.

CRASH: crash.txt at closeout was the known 0xc8a10094 front-end descriptor abort
(esr=0x92000006, far 0xc8a10094), 2,618 B, sha 7bc6440f. Preserved to
h3-test/crash-pt6.txt; baseline restored (1,735 B, sha 9d83c322) and read-back
verified. Best reading: boot 3a crashed during the close3 sequence (parked
lottery boot closed while the guest sat parked). Boot 3a's stderr is LOST
(stderr.log truncates per boot; 3b's launch overwrote it). None of the three
pulled stderrs carries an abort line (their "abort" hits are SQLite rollback
noise + one malformed pipeline_cache line per boot, the exonerated spam family).

CLOSEOUT (all verified): resident 39b5736c restored read-back exact (37,300,421 B;
first attempt failed on a Windows-style path through the WSL route, rc=26,
re-ran with /mnt/c); dk_diag + profile.txt deleted (550 on re-check); save
a90864fa unchanged (first pull failed on the unencoded space in "Card A",
re-pulled with %20); Config 6a629806 unchanged; stderrs per boot pulled
(boot1 2,810,657 B 85f30f61; boot2 988,496 B c59870ba; boot3b 646,931 B 4dd9d1c9);
no profraw banked (no measurement boot; bank stays 47); claim deleted via raw
FTP DELE, listing-verified; console free, relays sent.

OWED: the five-way split is still owed. The profile has missed the route on four
consecutive attempts after flight 1's span-only delivery. Recommendation: desk
first on why the cure holds 0 s here vs 74 s on the verification boot (the
signature does not predict it); if flying again, raise the launch allowance AND
gate each launch on the 30 s probe (close a parked boot early instead of carrying
it at 1 fps).

CURE-HOLD STUDY (Nov 19, 2026; full report ~/workspace/no100f-h3-evidence/CURE-HOLD-STUDY.md,
desk-only, agent d8c52148). Question: why does the parkwatch-cure hold 74 s on the
verification boot and 0 s on flight 6's parks? Read from the raw logs (stderr-pwc.log,
stderr-pt6-boot1/2/3.log, stderr-pt3.log) and the relay source (shard_4cbf).

VERDICT: the cure is NOT failing — the relay is starved after it. The flag write lands
every fire (flag 1->0 logged 70/74/40 times on flight 6); the waiter wakes, the advancer
catches up, 1-3 frames present; then the pipeline refills because almost no new
breakpoints are armed (breakpt advances 69 times over 208 presents on boot 1) and almost
no AA4s fire (73 callbacks over 208 presents), so the advancer runs 4 times over 208
presents. On the verification boot's hold, AA4 callbacks = advancer runs = frames
(1,926 / 1,925 / 1,923 over 74 s, breakpt ~1/frame, DVD ~130/s). The machinery is
identical when it runs (leg mix 98.8% AA4-mismatch / 1.2% post-callback on ALL four
logs). The park signature does NOT predict curability (boot 3b carries the study's
exact signature A=0 B=2 arm=0 latch=0 and holds 0 s; pwc #1 carries the same signature
and holds 74 s; boot 1 parked at frames=3166 t=157 s, nearly identical to pwc #1's
3164 / 159 s, and holds 0 s). PADRead resumption does NOT distinguish them either
(zero padwatch lines after the park on EVERY boot, including the 74 s hold — that
"flow" was non-interactive movie playback; the menu could never have opened on it
either). The post-fire sequences are identical line for line (pwc fire #2 == boot 1
fire #1), and the pre-park transition is identical too (212 vs 216 DVD callbacks, 0
AA4 callbacks, 5 frames in 0.76 s). What differs is what the guest does next.

BEST-SUPPORTED DISCRIMINATOR (not proven): token/table pool liveness at the park.
The one durable cure is a movie (re-)start with an installed pool (Intro, re-opened
at t=157.7 s); every 0 s cure is a first-open movie (boot 1 demo1, boot 2 RW4), a
movie-end drain (boot 3b THQLogo, boot 2 HILogo, pwc #2 Intro end, flight 3 WBLogo
end — teardown func_801762A8 zeroes the pool), or the title (pwc #3). With a live pool
AA4s mismatch and the advancer runs every frame; with a stale/zeroed pool AA4s match
(0==0), arm=1, and the relay depends on the latch leg, which evidently does not keep
up. Caveat: boot 1 shows one certain AA4-mismatch run after fire #1, so the pool is
not fully zeroed there — the hypothesis needs a token/table sampler to test (250 ms
sampler on token 0x802716F4 + table 0x80270150 + mode struct 0x80271710 at each park
and cure fire; the same TryRead32-only shape as heapwatch).

LIVE DISCRIMINATOR (proven, cheap): the post-fire 5-second frame rate — the 30 s
probe applied after a cure fire. Holding reads ~130 frames/5 s; starved reads 2-3.
Separation >40x across the existing threshold 25.

SECOND LAYER: none exists at the fire point. The flag is not the dead half (the cure
clears it every fire); arm is not dead either (re-park reads arm=1); the dead half is
the AA4 supply. Running the advancer from the fire point is guest code on the service
thread (F-J1, closed permanently); pool/token surgery is fragile guest-state surgery;
deliver-before-suspend was refuted for these parks (legitimate waits, no delivery
pending). The durable fix, if pursued, is at the pool lifecycle (setup op re-installs
the pool on movie start, or teardown does not zero it while the presenter needs it) —
a guest-behaviour design question, named not specced.

POLICY (prescribed, not yet decided by Brandon): (a) RETIRE "launch once, cure
carries" — falsified for early parks; the cure buys 1-3 frames per fire, not a carry.
(b) Launch allowance 3, each gated on the 30 s probe (close a parked boot early instead
of carrying it at ~1 fps); a late park gets ONE post-fire probe (one cure fire, 5 s
frame-rate check; <25 -> close and relaunch, counting as a used launch; >25 -> carry,
but do not drive the menu until PADReads resume — a held boot with no PADReads is movie
playback, not a driveable boot). (c) Carry/close decision = post-fire 5 s frame rate
(>25 carry, <25 close); breakpt advance (~1/frame vs ~1/3) and DVD rate (~130/s vs
~5/s) corroborate; never use the signature or PADRead resumption for that decision.
(d) The cure STAYS as a rescue-only net: gated on the parkwatch signature, inert in
the check-free default (dk_diag = fifofast only), rescues 1-3 frames per fire and keeps
a parked boot alive for diagnosis and clean closeout; it becomes redundant only if the
pool-lifecycle fix lands.

---

## CHECK-FREE DEFAULT RE-VERIFICATION #2 (Nov 19, 2026 — GATE-PASS)

**Why:** vi.cpp changed again (0cf01b35 → 333c7432, the parkwatch-cure
line), so the check-free default from the current tree differs from the
verified 67b587ec by exactly that line. Full record:
`~/workspace/no100f-h3-evidence/DEFAULT-REVERIFY-2-LOG.md`.

**Phase A — build + audit (LOQ, no console).** Build
`switch/scripts/build-switch.sh --deko --out none` from
`C:\dev\no100f-nx`, 15:57:56Z → 15:59:52Z, BUILD_EXIT=0, 0 "pgo"
lines. LOQ build claim taken 15:57:56Z, released immediately after,
board-noted. Product: NRO sha256
`43ad307c836cefcde5f05bec52858d96c123e8013b5b54efa3a7ed9509bc01b6`
(37,275,845 B), staged to
`h3-test/no100f-nx-default-rv2.nro` with read-back hash match.
Downloads untouched (53dbb7bb). Audit vs 67b587ec: generated tree
check-free at Phase 0 identity (GuestSafepointServe/Requested grep 0,
base_translation_output.json e975c4aa, base_translation_sources.bin
dd7be7df); source delta vs the pre-cure backup
(`parkwatch-cure-backup/`) is the cure line only — exactly 18 added
lines, 0 removed (vi.cpp ~2179–2196); binary strings `[parkwatch]
cure` ×1, `SCOOBY_PARKWATCH` ×2, `SCOOBY_PHASETIME` ×2, `[phase]` ×1,
GuestSafepointServe ×0; size delta 0 B.

**Phase B — gate flight.** Stood by read-only ~29 min behind phasetime
flight 6 (claim since 15:57:26Z); claimed 16:26:59Z after the claim
cleared (16:26:47Z), read back, re-verified before the launch press.
Gate capture: clean HOME, Scooby focused, no Playing badge, battery
100%. Card prep verified: resident 39b5736c, save a90864fa… (41,024 B),
Config 6a629806… (3,111 B), crash.txt baseline 9d83c322… (1,735 B),
dk_diag absent; staged 43ad307c read-back matched; dk_diag =
`fifofast` only.

**Launch 1 (16:29:16Z): GPU-307.2 LOTTERY** (clocks 1020.0/307.2/1331.2)
— but watchdog 12 s read 250 frames presented: no frame-2 freeze even
on the lottery boot. Closed cleanly and relaunched ONCE per the brief.
**Launch 2 (16:31:57Z): STOCK** (1020.0/384.0/1331.2). Watchdog 12 s:
252 frames; 30 s: 795 frames (re-verify 1 flew 251/794; F1 froze at 2).
**GATE-PASS.** [dk] line: fifofast=1, all others 0.

**Load:** menu opened on the second hold of the chained double-PLUS
(the first PLUS hold landed mid-intro-movie and did nothing; the first
single hold on the title also did nothing). Stick-stepping: sys-agent
set-stick steps 0 or 2 rows, never 1 (0.15 s = 0; 0.2/0.3/0.5 s = 2);
the dbg stick 0.15 s UP step moved exactly one row (OPTIONS → LOAD
GAME, brightfrac 0.0878). Sys-agent B press/release pairs (1.5 s)
confirmed first try: LOAD GAME → Slot A → file list "11/14/26 10% /
The Mystery Machine" on top. Spawn capture-verified at Mystic Manor
(~20 s after confirm). [vi] frames 15002–15601: 49.43 ms/frame
(coverage only, no price claimed).

**Patrol (one cycle, gate only):** DOWN / LEFT / UP / RIGHT, 15 s each,
0.5 s centred, dbg sticks, foreground. Marks `route-start-rv2` at
608 s / `route-end-rv2` at 670 s. End capture: Scooby at the manor side
porch — genuine traversal, in-game function proven.

**Closeout (strengthened standard):** HOME click → capture (Playing
badge) → X click → capture (Close dialog verified showing, Close
focused — flight 6 rule) → explicit press A / release A pair (NOT
click A — flight 5 rule). 3× running=0 + delayed 30 s re-poll (no
relaunch). Two clean-HOME captures (1:15 PM, battery 100%). Resident
39b5736c restored read-back matched; dk_diag deleted (listing-verified
absent); crash.txt at baseline; save re-verified; Config untouched;
stderr pulled to h3-test as stderr-rv2.log (226,957 B, sha
ec440163…) — STOCK/SECOND BOOT ONLY (the lottery boot's stderr was
overwritten by the relaunch). Claim deleted via raw FTP DELE,
listing-verified. Release relays sent to SPM, CTTR, JSRF.

**Consequence:** the cure-tree check-free default is verified shippable
as-is. The tree's check-free default now stands at 43ad307c (vi.cpp
333c7432). NEW DRIVE FINDING (banked): on an in-flow boot, use the dbg
stick for menu steps (0.15 s = exactly one row); sys-agent set-stick
double-steps and cannot be trusted for single steps.

## PHASETIME FLIGHT 7 — PROFILE MISSED under the adopted cure-hold policy (Nov 19, 2026; PHASETIME-FLIGHT7-LOG.md)

Verdict: function-level five-way split STILL OWED. First flight under the adopted
policy (Brandon: "go with your recommendation"); the policy worked as designed —
3 launches in ~35 min of console time, the lottery caught twice, launch 1's early
park caught at the 30 s probe, post-fire probe gave a clean starved verdict.

- Launch 1 (18:20:39Z): GPU-307.2 LOTTERY; probe Δ=6 (171→177) PARKED. Closed clean.
- Launch 2 (18:23:20Z): GPU-307.2 LOTTERY; probe Δ=129 (300→429) IN FLOW. Closed
  clean (probe passed but clocks void the boot).
- Launch 3 (18:26:41Z): STOCK; probe Δ=84 (328→412) IN FLOW. Passed both gates;
  parked late at the HILogo end drain (frames ~412–505); 71 cure fires, +1 frame
  each, signatures A=2 B=1 arm=1 latch=0; post-fire probe Δ=6 (616→622 over 5 s)
  → STARVED → closed clean. Allowance exhausted (3/3); no 4th launch under the
  adopted policy.
- Policy compliance: every launch gated on the 30 s probe; the carry/close decision
  was the post-fire frame rate ONLY (signature and PADRead resumption recorded,
  not used); both banked close rules followed on all three closes (dialog
  capture-verified before the A press/release pair; explicit pair, never click A;
  3× running=0 + delayed 30 s re-poll); focus verified on the Scooby tile before
  every launch press (launch 1 needed one DRIGHT click from the SPM tile).
- Closeout verified: resident 39b5736c restored read-back exact; dk_diag +
  profile.txt deleted (250, listing-verified); claim deleted (250,
  listing-verified); save/config/crash baseline all verified; stderr boot 2
  (607,103 B, 26156b06) and boot 3 (875,669 B, f1d97f8a) pulled; boot 1's stderr
  LOST (overwritten by launch 2 before any pull); no profraw banked (bank stays 47).
- Owed: the split. Binding constraint is now arithmetic (a ~50% lottery rate +
  high early-park rate → <1 productive boot per 3-launch flight). Options for
  Brandon (not decided): raise the launch allowance; keep flying 3-launch
  flights; or pursue the pool-lifecycle fix (cure-hold study §7 design pass owed).

## PHASETIME FLIGHT 8 — SPLIT DELIVERED (PARTIAL) (Nov 19, 2026)

Verdict: SPLIT DELIVERED (PARTIAL). First productive phasetime flight and
first route-window profile: 119,273 samples over 240 s (frames
11,293–14,815), 42.9% in translated code, 6,020 translated functions.
Eight completed 600-frame [phase] windows (4,800 route frames) close the
identity exactly. The split is priced at the profile-window game mean
68.52 ms/frame (windows 2–7). Full record:
~/workspace/no100f-h3-evidence/PHASETIME-FLIGHT8-LOG.md.

Span level (8 windows): game 67.79 = pre 1.13 + submit 66.67 (aurora
17.83 + submit-minus-Aurora 48.84); named 0.18; audeliv 5.32; tail 41.78.
Profile window (2–7): game 68.52 = pre 1.00 + submit 67.52 (aurora 18.07
+ submit-minus 49.45); audeliv 5.38; tail 42.19. Submission-span-bound
holds on 4,800 in-flow frames (was 90 slow-frame spans in flight 1).

Function-level five-way split (top leaves only; shares x 68.52):
- Bucket 1 guest update: 8.7% → 5.96 ms (scene driver 800CFECC 1.78,
  entity transform 8004B340 1.51, + 13 smaller update fns).
- Bucket 2 guest traversal/DL-record: 8.4% → 5.76 ms (RpSkin skin kernel
  80144E8C 2.33 — largest real guest leaf; frustum test 8018A128 1.03;
  + 11 smaller render fns).
- Bucket 3 runtime FIFO intake + DL decode: 7.4% → 5.07 ms (memcpy
  cluster from push_storage 3.22, memcmp 0.34, ByteBuffer 0.75, mutexes
  0.27, vector-init 0.48).
- Bucket 4 audio (game-thread): 2.6% → 1.78 ms (AX HLE 1.64 +
  __AXSyncPBs 0.14); host delivery audeliv 5.38 ms is a separate span,
  not additive without double-count risk.
- Bucket 5 non-main threads: 0.00 ms (measured).
- Shared math lib: 2.5% → 1.71 ms (serves buckets 1+2; pro-rata +0.87/+0.84).
Separated: 32.55 ms (47.5%). Unseparated: tail 40.0% (27.41 ms) + phantom
12.5% (8.57 ms) = 35.98 ms (52.5%). NOT a closed split.

Two corrections: (1) PPCMfhid2 12.5% is PHANTOM — zero million-call
milestones in the whole boot, true cost ~0.00 ms (the Nov 15 instrument
answers its own question). (2) The Aurora bucket is ~45% IPC waiting:
svcSendSyncRequest 11.8% (8.09 ms/frame), assigned to the GPU-submission
path by elimination (runtime opens no service sessions; AX HLE uses no
audio service; disc 0.08 ms; pad is shared-memory) plus the share check
(without it Aurora leaves sum to ~7.8% vs the 26.4% span bucket). Exact
service (nvhost submit vs vi present) NOT identified.

Flight: claim 2026-11-19T19:36:56Z; allowance 5 launches, used 2.
Launch 1 STOCK + parked at the 30 s probe (Δ=6), closed clean; its stderr
LOST (overwritten by launch 2 — pull-after-each-boot rule not followed
for launch 1) and its profraw fw-1795117217 NOT banked (last-only rule).
Launch 2 STOCK + in flow (probe Δ=138). Menu opened on the second PLUS;
first menu attempt lost to the idle timeout (drive gap ~41 s); recovered
via a PLUS during the re-run demo (menu on the release edge). Load wedged
at frames=8601 and crossed in the exact F3 shape: 4 safepoint episodes /
32 doses, counter 0→140, frames resumed t≈508 s. Front-end took 3
parkwatch-cure fires (frames 4704/5270/5555; 54/56/59 ms resumes). Route
6 cycles x 4 legs, marks 601 s / 977 s; zero parks/episodes on the route;
heapwatch 0 trips. Spawn verified Mystic Manor (frames 9333, t=563.8 s).
Closeout verified (resident 39b5736c restored read-back exact; dk_diag +
profile.txt deleted; crash baseline; save unchanged; claim deleted
listing-verified; relays sent). Profraw fw-1795117362 banked (5,682,520 B,
sha 545955ec…); bank total 48. stderr-pt8.log 9,354,635 B (launch 2 only).

Follow-ups: (1) close the split with a profraw reader (none in the tree
or on the LOQ — llvm-profdata absent); desk job, no further flight
needed. (2) The Aurora IPC wait (8.09 ms/frame) is the largest identified
single cost — design pass on the submit/present path, exact service
pinned first. (3) The phantom's true owner (8.57 ms) is unknown.
(4) Split shape (update ≈ traversal ≈ FIFO intake at the top) is the
input for native-scene and FIFO work: no one bucket is the whole 49.45 ms.

## AURORA IPC DESIGN PASS (Nov 19, 2026; AURORA-IPC-DESIGN.md)

Desk-only study, no console, no builds, no tree edits. Parallel to the
close-the-split desk job (Brandon: "Do both in parallel").

VERDICT: the 8.09 ms is NOT GPU submission. It is synchronous SD file
writes of the runtime's own diagnostic log. The service is pinned:
flight-8 profile leaf elf+1F7ABF8 (11.8%) is svcSendSyncRequest; its sole
pair above cutoff is <- elf+1F832A4 (11.7%), which addr2line calls
serviceDispatchImpl (libnx 4.12.0, inlined) — and the return address sits
inside fsFileWrite (0x1F831B0, +0xF4). Only two BL sites in the ELF call
fsFileWrite (fsdev_write / fsdev_write_safe, libnx newlib _write).
RT_LOGF is fprintf(stderr); userAppInit freopens stderr.log ("w") and the
transcript init sets _IONBF. The transcript pipe was supposed to make
this async, but libnx pipe() is an ENOSYS stub (return -1), so
InstallTranscriptPipe fails, the transcript is disabled, and the game
thread writes stderr.log directly. Card proof: Logs/ holds only
stderr.log (9,354,635 B) + crash.txt + one EMPTY run folder
(base_1795117362_pid18446744073709551615, garbage pid); no console.log.
Route window: 14,910 lines / 1,357,972 B / 3,522 frames = 4.23
lines/frame, 386 B/frame; 8.09 / 4.23 = ~1.9 ms per write. Emitters:
[dvd] [io] 9,240 (traceio), [os] [wake] 3,518 (tracewake),
[vi] [cnt]+[gxcnt] 1,891 (tracecnt) — all SCOOBY_* env-gated, capture
only. DVD reads are synchronous on the game thread (no fsFileRead pair:
reads are fast, writes are slow). The resident pays ~0: F2/AB2 stderr
logs are 1,862/1,803 lines per boot with no per-frame line type.

CORRECTIONS to flight 8: (1) "Aurora ~45% IPC waiting" is wrong; the
11.8% is log stalls, not Aurora IPC. (2) The share check is invalid:
stalls land wherever the thread logs. Corrected Aurora reconciliation:
span 18.07 ms = ~17.8 ms real decode work inside outermost
fifo::process() (ProcessTimer, command_processor.cpp ~600) + ~0.3 ms
renderer-lock wait (census gx lock 0.21-0.35); profile leaves are
bucket-3 intake 5.07 (inside process) + identified GX 0.89 +
unidentified elf leaves ~2.7 + tail decode handlers ~9.1 predicted
(profraw-reader job to confirm). Stall-free capture frame ~60.4 ms;
split buckets scale ~0.88 (shares unchanged). Submit wall span
~58.8 ms stall-free (stalls sit ~7.9 ms in submit, ~0.14 ms in
pre-submit; pre = prev-present-end to first GX packet, submit = first
packet to present start, vi.cpp ~3940).

GPU IPC BOUND: all non-fsFileWrite svcSendSyncRequest sites combined
under ~0.2 ms/frame (pair list prints to ~0.2-0.3%, only fsFileWrite
appears). deko submit (memory.cpp flush(): fence-signal +
dkCmdBufFinishList + dkQueueSubmitCommands + dkQueueFlush) and present
(device.cpp: acquire / dkQueueWaitFence / dkQueuePresentImage, swap
interval 1) are real IPC but return after queueing / on signaled
fences. Latent hazard: flush() fence-ring-full path does dkFenceWait
up to 10 s; never triggers on the route. dkQueueWaitIdle only in
shutdown + bufdump diagnostic.

FIXES RANKED: (1) Nothing on the resident (already ~0; the phantom
8.09 ms leaves the queue). (2) Async logging for Switch — a real ring
(or socketpair) replacing the ENOSYS pipe in the existing transcript
machinery; saves up to the full 8.09 ms/frame on capture builds; risks
are cross-thread ordering, crash-tail loss (flush in exception/
watchdog paths), ring backpressure. (3) Rate-limit traceio/tracewake/
tracecnt (per-second token bucket; ~4.3 ms/frame at 10 lines/s caps;
loses per-event forensic detail). (4) Buffer stderr (_IOFBF 64 KiB;
saves most of 8.09; crash-tail loss + logtext staleness). (5) Ring-only
logging on capture builds (full saving; worst crash loss). KILLED:
batching/overlapping GPU submission (premise dead; would save ~nothing
and risk fence ordering).

NEXT: (a) write-count/time instrument in __wrap__write_r (log-fd only,
per window, with thread id) to pin 1.9 ms/write exactly; (b) implement
candidate 2 (or 3 as interim) on the capture tree; (c) re-fly one
phasetime flight — success is svcSendSyncRequest under 0.3% + submit
down ~8 ms + other buckets unmoved + stderr.log complete to the last
present; (d) profraw-reader job confirms the tail's Aurora share.
Housekeeping: reconcile the two stderr buffering owners
(fw_switch_init _IOLBF vs main.cpp _IONBF); fix the garbage transcript
pid in run-folder names.

## Close the Split (desk study, Nov 19, 2026 evening)

Flight 8's partial split is closed. Full study: `~/workspace/no100f-h3-evidence/CLOSE-THE-SPLIT-STUDY.md` (this VM).

**Verdict:** separated share rises 47.5% (32.55 ms) to **54.9% (37.63 ms)** at the 68.52 ms/frame basis. The phantom is SOLVED and the two big update leaves are NOT real.

**Phantom solved.** The 12.5% (8.57 ms) attributed to PPCMfhid2 is the GX/FIFO HLE runtime in [0x300f00, 0x3d16e0), misattributed because PPCMfhid2's HLE wrapper is the lowest-host entry in the profiler's table and the catch-all label for the 854 KB of runtime code after it. PPCMfhid2 ran twice all boot. The span's top functions are HleFifoWrite (1.92e9 counts), GX__CallDisplayList (1.03e9), WriteDisplayListData (0.65e9), plus the GX_HLE_FIFO_Write* family and GxCpDecode. Apportioned by span counts: bucket 3 +4.12 ms, bucket 2 +3.01 ms, shared-math +0.72 ms, host/runtime +0.40 ms, bucket 1 +0.12 ms, remainder 0.21 ms unresolved.

**Two update leaves are phantoms.** func_800CFECC (2.6%, 1.78 ms) has 489 block counts all boot; func_8004B340 (2.2%, 1.51 ms) has 0 (never executed a single block). Both come OUT of bucket 1 (3.29 ms to unresolved; true owner unidentified from this data, needs a route-window profile that persists every raw PC). The 80162204 span (0.21 ms) is InvokeDirectCpu trampolines for rotation helpers, moved bucket 1 to shared-math. 80178B60 / 8016792C / 8015CE88 stay bucket 2; 8017BAFC stays shared-math (real: called 4.46M times).

**Tail does not close by counts.** The tail (27.41 ms, 6,435 functions) is count-concentrated (top-200 = 99.1% of tail counts) but time-flat in the route window (every function under 0.2% = 0.137 ms). Its count leaders are load/front-end code: func_80016958 is a bulk memory fill called 2,050 times (route ~0); func_80005274 is a byte-wise memmove called 187M times (~8 B/call); func_800D875C is the junction delay (wedge/load counts). No single-function lever lives in the tail.

**Closed split (ms/frame):** bucket 1 update 2.58 (was 5.96); bucket 2 traversal/DL-record 8.76 (was 5.76); bucket 3 FIFO intake/decode 9.19 (was 5.07); bucket 4 audio 1.78; bucket 5 non-main 0.00; shared math 2.64 (was 1.71); host/runtime + Aurora + waits 12.67 (was 12.27); separated 37.63 (54.9%); unseparated tail 27.41 (40.0%); unresolved misattribution 3.50 (5.1%).

**Implication:** buckets 2+3 (GX/FIFO path) are now 17.95 ms (26.2% of the frame), the dominant cost; bucket 1 collapses to 2.58 ms. FIFO/DL-record first, update second. The DL-record + FIFO-intake prize (17.95 ms) is the ceiling for what native-scene conversion can reclaim on the game thread. The Aurora IPC wait (8.09 ms, svcSendSyncRequest) is unchanged and still the single largest leaf.

**Anomalies:** GuestSafepointServe entry 191,702,742 vs 4 served episodes on record (request-flag lifecycle needs a look; Serve's own time is 0.2%, does not move the split). 273 translated functions in the profraw have no registry record (small extra misattribution source). Phantom apportionment carries about +-30% per-bucket uncertainty; direction (buckets 2 and 3 dominate) is robust.

---
## N1 — Native-Scene Generalization Design (desk, Nov 19, 2026)

Full design: `~/workspace/no100f-h3-evidence/NATIVE-SCENE-N1-DESIGN.md`.
Brandon's decision Nov 19 ~17:55 NST: hold the dedicated async-logging flight; start N1 design (desk). Design only; nothing built.

**Verdict.** The allowlist is buildable from data in hand, and it reorders the work. Two findings change the FW-7 picture:
1. Scoping §5's prize (11-16 ms net) belongs to seam-A interception (skip the guest pipeline). The as-built Stage 1/2 mechanism records at the DL point: the guest runs 100% of its own pipeline code on every path (the callback transliteration calls `func_801850E4` unchanged); only the DL's Aurora-side processing is replaced. The as-built ceiling is ≈12.2 ms gross on the capture frame (bucket 3's 9.19 + bucket 2's HLE ≈3.01), ≈10-11 ms net, ≈6 ms net on the resident frame if bucket shares transfer (unproven). The §5 number is an upper bound for a deeper interception, not a prediction for the as-built mechanism.
2. The census covers ~5% of route vertex throughput (10,089 verts vs ~215k verts/frame). The gap is world-sector geometry, invisible to the census. The atomic allowlist is Wave 1 by mechanism readiness, not by prize size. The sector share — the likely larger prize — is N2's measurement, and the sector seam is a follow-on design, not part of N1.

**Allowlist (28 route geometries; totals 12,089 tris / 10,089 verts / 44 meshes).**
- Wave 1 (N4a, meshes=1 static, parse as-is): 15 geometries, 2,222 tris, 1,603 verts. #2/#3/#4/#6/#8/#9/#10/#11/#12/#13/#16/#17/#19/#28 + #20 rma00001 (PROVEN anchor). Model reclaim ~0.2 ms — mechanism proof at scale, not a perf lever.
- Wave 1P (pending N5 skin check): #14 ofee0001 (129/243), #15 rlii0006 (324/654) — verts ≈2x tris, split-vertex/skinned profile. Convert iff N5 clears; else Wave 3.
- Wave 2 (N4b, after N3): #1 obbe0001 (1496/1035 ×3), #7 rfp00001 (354/467 ×2, pending N5), #18 "?" (1188/1180 ×4), #21 "?" (1704/1631 ×9). 4 geometries, 4,742 tris, 4,313 verts, 18 meshes. The two "?" chunks are admitted on counts+serial+generation, no name (§1.4 of the design); if N2 shows them sector-submitted, they leave the allowlist for the sector seam.
- Wave 3 (N7, after N5): #22/#23/#27 cgang002 (3,876 tris, 2,759 verts). Kernel prize 2.33 ms (RpSkin fn_80144E8C) is N7's, not N4's.
- Excluded, stay GX: #5 (cull-only, never rendered), #24/#25/#26 (billboard/glow class — camera-facing sprites; native strip submission would not reproduce billboard facing).
- Front-end 16 (N8 scope, enumerated here from the s1e pin-scan lines): 5 cgang002 character models (~14.2k tris, N7 class), baseplate_12 (204/200 ×5 — the N3 diagnostic geometry), owas2301 (204/201 ×5), 8 small meshes=1 props (Wave 1 class, N8), radar_load1 (2/4, name-less at cb). 81402160 (2796/1922 ×8, "?", w1C=0) is EXCLUDED until identified — w1C=0 where every other geometry reads 1 suggests a different structure class.

**Identity model (decided):** per-pointer cache entries (proven for two live rma00001 instances); single-target pin constants become an allowlist table (countA, countB, meshCount, texture name, wave, name-mode) matched in ArmBegin and PinScan. A geometry not in the table takes the pure transliteration path silently.

**N3 spec (§3 of the design).** Extension points in the live Stage 2 code: `ParseGeometry` (:231, mesh[0]-only — loop meshes, cap 16, census max 9); `Cache` struct (:157-171 — single packed stream → `std::vector<MeshCache>`); `FoldGeneration` (:186-207 — fold every mesh entry + index array); `ReadTextureNameViaMesh` (:135-145 — per-mesh material chain); validation battery per mesh (index checks per mesh, union max-index, cross-mesh material check; failure poisons that mesh only, not the geometry); pin's three `kPinMeshCount` uses → table lookups; `TryGetPacked` → mesh list; `TryConvertDl` (gx_dl.cpp:1206) → one `submit_raw_draw` per mesh in DL order. **Design step before building:** a diagnostic variant logging the player's scan verdict per DL for baseplate_12 (menu phase, no route needed) answering three questions — (1) interleaved per-mesh state in the DL? (2) one DL with all meshes vs one DL per mesh? (3) per-mesh texture binds correct at each draw? Outcome (A) = submit N draws, no state work; outcome (B) = reproduce interleaved state via the player's Apply* path between mesh draws. Known waste (N3 optimization candidate, separate from its gate): `ApplyAuroraArraysForDisplayList` runs before the conversion site at both submission sites (:1557/:1580), so converted DLs pay array staging they never use — consult learn state before staging, fall back to staging + GX submit if conversion then fails.

**N2 census design (§4.1).** Counters in the runtime DL path, keyed by submission caller (guest return address at the GXCallDisplayList override; probe pattern already exists). Per class per route leg: draws (mayContainDraw), vertices (sum of index counts), DL bytes (FIFO-intake proxy). Classes: Atomic (func_801402F0 ×11 sites), Sector (func_80158F70 ×3, func_80142B74 ×2), Im3D/particles, HUD/2D, Other (shadow-pass EFB, Bink). Config: resident lineage, eager, fifofast only + counter key; stock by readback; standard 12-leg route. **Kill criterion (falsifiable):** D = (atomic+sector) draws/total, V = vertices/total, T = (atomic+sector) share of buckets 2+3 time. If D < 50% AND T < 50%, re-scope before building N4 — options: (a) sector seam becomes the campaign, (b) re-size the prize to the measured atomic share and proceed at that size, (c) park the direction. If D ≥ 50% or T ≥ 50%, proceed at the measured share. N2 also delivers the static-share prediction S (ms) for the N4 gate.

**N4 gate (§4.2, falsifiable, stated before the boot).** Baseline B = resident anchor route slow mean on the same protocol (39.15 ms new-route anchor; re-measure if the tree changed since AB2). S = N2-measured static-atomic share of the route frame in ms. R_pred = predicted as-built removal fraction, stated from the N2 numbers before the boot (≈ the Aurora-side portion of S). **Gate: (B − N4 route slow mean) ≥ 0.6 × S × R_pred.** Pass → N7. Fail → find the missing ms with N9 counters before N7. Verification per FW-7 N4 (full-route capture set, shadow-pass in-frame, second scene + return, menu/portrait bbox captures, zero parse_invalid, no dl_drift storm, 2,802 abort watch). Note: FW-10 — Stage 2 proved load/relaunch reproducibility, not a save written by the native build and read back; N4's gate should include a genuine save round trip or state explicitly that it reuses the reduced form. **Closeout restores resident 39b5736c (FW-7 §2 still names 34585267 — STALE).**

**RpSkin (N5) and FIFO intake (§5).** fn_80144E8C (2.33 ms) is a kernel, not a geometry — no allowlist row; N5 runs parallel (locate the GC RpSkin plugin-offset global + skin data layout; promote the kernel identity from ~90% to proven; verify pose matrices are readable in composed form). Realized only by N7 GPU skinning. The FIFO write path (HleFifoWrite 2.82 + memcpy 3.22 ≈ 6.0 ms, host-side runtime) is **not** removed by native-scene conversion as built — the native draw still submits a vertex stream and the guest still writes the CALL_DL + state commands. It is a separate, runtime-side lever (push_storage copy reduction, batching, the §3.5 skip-staging optimization), parallel work priced by its own study or N9 telemetry — not claimed by N1.

**Instrumentation (§6).** N4 needs [phase] windows + [nativescene] counters + N9 per-class telemetry (lands inside the N4 build). Async logging (Aurora candidate 2) rides as a passenger on the next capture-tree change (N3 or N4 build, whichever touches the capture tree first), proven on that build's flight under the Aurora criteria (svcSendSyncRequest < 0.3%, submit down ~8 ms, other buckets unmoved, stderr.log complete). The `__wrap__write_r` write-count/time instrument (Aurora §4 item 1) is a passenger candidate for the same build. **No dedicated instrument flight is requested by this design.**

**Open questions (§7).** Sector seam identification (follow-on design after N2); per-geometry recurrence (N2 measures; the §2.4 model assumes ~1 draw/frame/instance); the "?" identity (decided in §1.4, moot if sector-submitted); array-staging waste (§3.5, own parity boot); §5 vs as-built prize correction (§2.1(a) — first suspect if N4's gate comes in low); GuestSafepointServe anomaly (191,702,742 profraw entries vs 4 served episodes); split-vertex profile (N5 decides); multi-mesh DL interleaving + mapping (N3 diagnostic decides); front-end 81402160 identification; save→load round trip (N4); GPU at 1.0x (N6, still owed since scoping).

**Anomalies noted during this design.** (1) Wave-sum arithmetic: the census mesh total is 44, not 43 as STAGE2-DESK's Q1 summary line states (recomputed per geometry: 3+1+1+1+1+1+2+1+1+1+1+1+1+1+1+1+1+4+1+1+9+1+1+1+1+2+2+1 = 44; wave sums 15+2+18+4+5 = 44 check). (2) The split study's §7 line ("the Aurora IPC wait (8.09 ms) is unchanged and still the single largest leaf") is STALE — the Aurora design pass proved it is synchronous SD log writes; resident pays ~0. (3) FW-7 §2's closeout target 34585267 is STALE — resident is 39b5736c.

**N2 census flight (Nov 19, 2026) — CENSUS NOT PRODUCED, gate OFF, kill criterion UNEVALUATED.** Full record: `~/workspace/no100f-h3-evidence/NATIVE-SCENE-N2-CENSUS.md`; boot log `stderr-n2-boot1.log` (103,554 B, sha256 `c93283d3…`). Build flown: n2-counter d6abe935 (37,279,941 B, hash verified on LOQ pre-stage and by card read-back). Claim 22:02:49Z, Launch 1 22:05:57Z (pid 383, titleId 050FCFAF9892B000), clocks STOCK 1020.0/384.0/1331.2, watchdog 253 frames @ 12 s / 795 @ 30 s (in flow, no freeze, no park), dk_diag `fifofast + dlcensus` staged and read back — but the boot's stderr holds exactly one `[dlcensus]` line: `gate read: NO100F_DLCENSUS=(unset) -> OFF`. Zero `on:` / `caller` / `window #N` lines. No route driven (no point on a dead counter); 1 of 5 launches used. **Root cause (build defect, verified in the live tree):** `no100f_native_scene.cpp:931` reads the gate via `getenv("NO100F_DLCENSUS")`, but nothing ever sets it — `switch/deko/device.cpp` `diag_init()` has one explicit strstr line + one explicit setenv per key (nativescene pattern at :58/:96) and no `dlcensus` parse line, no `dlCensus` field in `switch/deko/dk.h`'s diag struct, no `setenv("NO100F_DLCENSUS", …)` anywhere. The build log's "same pattern as nativescene/s0probe" covered only the census side; the dk_diag → g_diag → setenv half was never written. The binary check (format strings present) cannot catch this. **Fix for N2-counter v2:** dk.h add `bool dlCensus = false;` near nativeScene; device.cpp parse loop add `if (std::strstr(line, "dlcensus")) g_diag.dlCensus = !off;`; setenv block add `if (g_diag.dlCensus) setenv("NO100F_DLCENSUS", "1", 1);` (optionally add `dlcensus=%d` to the `[dk] diag:` line). Desk-verify by grep before staging; re-fly's FIRST logtext pull (within 1 min of launch, before driving the menu) must show `NO100F_DLCENSUS=1 -> ON` plus a `[dlcensus] on:` line — if OFF, close immediately, do not drive the route. The d6abe935 build must NOT be re-flown. **Owed:** rebuild (v2, new hash), re-fly (same spec, fresh 5-launch allowance — parent's call), then the actual N2 deliverables: D/V/T kill-criterion verdict (§4.1), S for the N4 gate (§4.2), sector submitter census. Kill-criterion method banked for the re-fly: T = N2 counters × §2.4 model (≈11 µs/draw net, byte-weighted for big DLs) × flight 8 buckets 2+3 top-leaf (15.8%, ≈10.83 ms at the 68.52 ms game mean; 52.5% tail+phantom bounds T). **Closeout:** resident 39b5736c restored (read-back hash match), dk_diag DELE'd (250, absence verified), crash.txt baseline 1,735 B / `9d83c322fb35…`, save unchanged 41,024 B / `a90864fa…`, Config 3,111 B / `6a629806…`, 3× running=0 + delayed 30 s re-poll + 2 clean-HOME captures (no Playing badge, 6:42 PM), claim DELE'd (250, absence verified). Console free.

## N2 CENSUS RE-FLY (v2) — CENSUS PRODUCED (Nov 19, 2026)

Full record: ~/workspace/no100f-h3-evidence/NATIVE-SCENE-N2-CENSUS.md
(Re-fly section R1–R7). Evidence: ~/workspace/n2v2-captures/
(stderr-n2v2-boot1.log, 252,802 B, sha256
70805d67df907cc66325f7e9e2bec367de12313c8b2914b75f651750e33df859).

- v2 build (plumbing fix per N2-CENSUS §3): NRO 9be85c9a,
  37,279,941 B. v1 d6abe935 superseded, never re-flown.
- One launch (1 of 5), stock clocks 1020/384/1331.2, gate ON
  ([dlcensus] gate read: NO100F_DLCENSUS=1 -> ON, dlcensus=1 in
  the [dk] diag: line). Load landed first try; 12-leg route
  DOWN/LEFT/UP/RIGHT; 10 completed 600-frame windows #20–29
  (frames 12000–17999, 6,000 frames). No park, no crash, no abort.
- Route aggregate: atomic D 59.0% / V 57.8% / B 61.9%
  (1,448,116 draws); sector D 41.0% / V 42.2% / B 38.1%
  (1,004,618 draws); im3d 0, other 0, hostsubs 0, unnoted 0.
  Draws/frame 408.8; verts/frame 247,265. Per-window atomic D%
  53.2–63.9%, consistent within noise.
- Kill criterion (N1 §4.1, as written): D = (atomic+sector) =
  100.0%; T = 100% top-leaf, 68–100% tail-bounded. NOT FIRED —
  N4 proceeds at the measured share. (Atomic-only D = 59.0% gives
  the same verdict.)
- Per-draw cost corrected: §2.4 assumed ~900 draws/frame;
  measured 409 → ≈29.8 µs/draw gross (≈27.8 net, capture frame),
  not 13.5/11 µs. Scoping's ~900 draws/frame is retired.
- Sector census: sectors submitted ONLY via func_80158F70
  (lr 801590C0, 801590FC); 0x80142B74 never appears. Sector is
  149–187 draws/frame, steady; atomic 174–330, view-dependent.
  Front-end is sector-only (99.4%). Correction to N1 §2.3:
  sectors do NOT dominate in-game vertex flow (42.2% V).
- S (N4 gate input): Wave 1+2 = 19–34 draws/frame of 241.35
  atomic (7.9–14.1%; range is the N3 diagnostic's DL/mesh
  question). S ≈ 0.3–0.6 ms net on the resident frame (0.55–1.0
  ms capture; resident scaling assumes bucket shares transfer —
  UNPROVEN). R_pred ≈ 0.9. N4 gate: beat the 39.15 ms anchor by
  ~0.2–0.3 ms. Wave 1+2 are ~8–14% of atomic draws; ~85% of
  atomic draws are uncatalogued geometry (broader-atomic-census
  follow-on, separate from the sector seam).
- Closeout verified: resident 39b5736c restored hash-verified,
  dk_diag deleted, crash.txt at baseline, save unchanged,
  Config.toml untouched, claim released. Console free.
- Next, per Brandon's Nov 19 order (N2 then N3): the N3
  diagnostic (baseplate_12, menu phase, N1 §3.3). N3 does not
  start building until the diagnostic's verdict is in hand.
