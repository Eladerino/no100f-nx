# Third-party code

The repo-level roll-up. The Switch port's authors are FrightWorks and Elad.
The Switch layer's detail is in `switch/THIRD_PARTY.md`; the license texts
live in `switch/licenses/`.

## Switch layer

Paths below are relative to `switch/`.

Most of `switch/` comes from [spm-nx](https://github.com/Eladerino/spm-nx) at `12ec5e7`, the Switch port of
another WiiCompiled game. spm-nx's own code is GPLv3, the same license as FrightWorks. Some of spm-nx's
switch layer comes from SwitchWaker and SwitchWakerHD, under their own licenses, listed below. Files
starting with `fw_`, `sdl3_shim/sdl3_shim_frightworks.c`, `cmake/FrightWorksSwitch.cmake` and
`scripts/build-switch.sh` were written for this port. Everything not listed below is GPLv3.

The MPL-2.0 code in the table below (`source/debug_server.h`, `source/debug_server.cpp`, the
`deko` wgpu interposer) is SwitchWakerHD-derived; the source as used in this port lives at
[Eladerino/no100f-nx](https://github.com/Eladerino/no100f-nx).

| Path | Origin | License |
|---|---|---|
| `sdl3_shim/` (all files but `sdl3_shim_wiicompiled.c` and `sdl3_shim_frightworks.c`) | [SwitchWaker](https://github.com/centollOS/SwitchWaker) `switch/aurora/sdl3_shim/` at `92307ff`, through spm-nx, with the changes in `sdl3_shim/PATCHES.md` | MIT (`licenses/SwitchWaker-MIT.txt`) |
| `sdl3_shim/sdl3_shim_wiicompiled.c` | spm-nx | GPLv3 |
| `dawn/dawn.cmake` | SwitchWaker `switch/dawn/dawn.cmake` at `92307ff`, through spm-nx; the dependency fetch is now an option | MIT (`licenses/SwitchWaker-MIT.txt`) |
| `dawn/patches/` | SwitchWaker `switch/dawn/patches/` at `92307ff`, unchanged; patches to Dawn, Tint and abseil | same as what they patch: BSD-3-Clause (Dawn, Tint), Apache-2.0 (abseil) |
| `aurora/sqlite_horizon.c` | SwitchWaker `switch/aurora/sqlite_horizon.c` at `92307ff`, through spm-nx | MIT (`licenses/SwitchWaker-MIT.txt`) |
| `runtime/horizon_pthread.c` | SwitchWaker `switch/native/source/thread_wrap.c` at `92307ff`, through spm-nx | MIT (`licenses/SwitchWaker-MIT.txt`) |
| `cmake/clang-launcher.sh` | SwitchWaker `switch/native/clang-launcher.sh` at `92307ff`, through spm-nx | MIT (`licenses/SwitchWaker-MIT.txt`) |
| `scripts/Containerfile` | spm-nx, based on SwitchWaker `scripts/switch/Containerfile.native` (same image digest and packages, plus rsync) | MIT (`licenses/SwitchWaker-MIT.txt`) |
| `source/debug_server.h`, `source/debug_server.cpp` | [SwitchWakerHD](https://github.com/centollOS/SwitchWakerHD) `runtime/src/platform/` at `ac84576`, through spm-nx, unchanged | MPL-2.0 (`licenses/MPL-2.0.txt`) |
| `source/fw_switch_init.cpp` | adapted from spm-nx `switch/source/spm_switch_init.cpp` | GPLv3 |
| `source/fw_debug_server.cpp` | shaped after spm-nx `switch/source/spm_debug_server.cpp` | GPLv3 |

Fetched at build time, not stored here:

| Dependency | Version | License |
|---|---|---|
| [Dawn](https://github.com/encounter/dawn) (with Tint and abseil) | `266c1cf`, SwitchWaker's pin (`dawn/dawn.cmake`) | BSD-3-Clause, Apache-2.0 (abseil) |
| [deko3d](https://github.com/devkitPro/deko3d) | `switch/deko3d/`, vendored 0.5.0 + SwitchWaker patches, via spm-nx | zlib |
| [uam](https://github.com/devkitPro/uam) | `switch/uam/`, vendored 1.1.0 + SwitchWaker patches 1-9, via spm-nx | zlib + MIT (Mesa parts) |
| deko wgpu interposer | `switch/deko/`, ported from spm-nx's switch/deko (itself from SwitchWaker/SwitchWakerHD) | MPL-2.0 |
| [SDL 3](https://github.com/libsdl-org/SDL) headers | 3.4.4, Aurora's pin | Zlib |
| devkitPro devkitA64, libnx | the image digest in `scripts/Containerfile` | GPL (toolchain), ISC (libnx) |

## Credits

Scooby-Doo! Night of 100 Frights is (c) THQ / Warner Bros. Interactive,
developed by Heavy Iron Studios. Scooby-Doo is a trademark of
Hanna-Barbera / Warner Bros. This is an unofficial, non-commercial fan
project, not affiliated with or endorsed by them; it contains no game
data and works only with files from your own copy of the game
(GameCube USA, GIHE78).

- **FrightWorks** - Loneranger419 and contributors; the PC port this
  Switch port is built from.
- **Scooby-Doo: Refrightened** - created by **JosiahG**. FrightWorks
  split off from Refrightened on October 31, 2026 and continues it.
- **WiiCompiled** - by **patchzyy and contributors**: the recompilation
  runtime and translator. GPLv3.
- **Aurora** - by **Luke Street**: the GX compatibility layer. MIT.
- **spm-nx** - by **Elad**: the Switch port layer this port derives
  from (at commit 12ec5e7). GPLv3.
- **SwitchWaker / SwitchWakerHD** - by **centollOS**: the SDL 3 shim,
  the debug-server core (MPL-2.0), and the deko3d interposer lineage.
  MIT and MPL-2.0.
- **Dolphin Emulator Project** - the DSP coefficient ROM and the
  WiiConnect24 bootstrap files shipped beside the binary, and the
  Riivolution code the runtime ports. GPL-2.0 or later.
- **Dear ImGui** - Omar Cornut. MIT.
- **deko3d and UAM** - fincs / devkitPro: the Switch GPU API and shader
  compiler. zlib licence. UAM also builds on the **Mesa 3D** GLSL
  compiler and nouveau backend. MIT.
- **Dawn** - the Chromium WebGPU implementation (BSD-3-Clause), with
  Tint (BSD-3-Clause) and abseil (Apache-2.0).
- **SDL 3** - libsdl-org. zlib licence (headers only on Switch; the
  runtime links this port's own libnx shim).
- **pugixml** - Arseny Kapoulkine. MIT. **toml11** - Toru Niina. MIT.
  **Crypto++** - Wei Dai and contributors. Boost Software License 1.0.
  **libco** - byuu and the higan team. ISC.
- **SQLite** - public domain.
- **libnx** - devkitPro. ISC. **devkitPro** - the Switch toolchain.
- **Luckiest Guy** - Astigmatic, Apache 2.0; **Roboto** - Christian
  Robertson, Apache 2.0: the two fonts built into the in-game menus.
- **Kenney** - Input Prompts (CC0): the button prompt icons.
- **magic_enum** - Daniil Goncharov. MIT. (Inside Aurora.)
- **HD texture pack** - dogerki. (User-supplied add-on, not shipped.)
- Everyone who has tested a build and sent a log back.
