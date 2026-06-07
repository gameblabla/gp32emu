# GP32emu

GP32emu is a C11 Game Park GP32 emulator with a portable ARM920T bytecode interpreter, optional x86_64 dynarec, SmartMedia loading, HLE BIOS support for selected software, real BIOS boot support, SDL3 standalone frontend, Win64 frontend, Qt6/Linux frontend, save states, screenshots, and MKV recording.

No BIOS, game, or commercial assets are bundled.

## Supported builds

Headless/core build:

```sh
make -f Makefile.sdl
```

Standalone SDL 1.2 frontend:

```sh
make -f Makefile.sdl sdl12
```

Standalone SDL3 frontend:

```sh
make -f Makefile.sdl sdl3
```

SDL 1.2 and SDL3 accept `--lcd-persistence` and `--frame-interpolation` to enable the optional temporal video effects. They are disabled by default.

Qt6/Linux frontend:

```sh
qmake6 GP32emu.pro && make
```

or:

```sh
make -f Makefile.linux qt
```

Win64 frontend cross-build:

```sh
make -f Makefile.win64
```

WebAssembly/browser frontend:

```sh
make -f Makefile.wasm
make -f Makefile.wasm serve
```

Then open `http://127.0.0.1:8008/`. The WASM build uses the portable bytecode interpreter only; the x86_64 native dynarec is not compiled or exposed for WebAssembly. It supports loading BIOS files and GP32 game/media images from browser file inputs or drag-and-drop. If no BIOS is loaded, HLE/direct-load mode is used and the page warns that compatibility is limited. Browser audio uses an AudioWorklet jitter buffer when available, with a ScriptProcessor fallback. The frontend is rate-limited by elapsed presentation time; it avoids audio-queue-driven catch-up bursts and uses only small emergency catch-up when audio is close to underrun. The browser UI also provides save/load state slots, remappable keyboard controls, remappable Gamepad API controller support, mobile touch controls, video scaling/filter controls, inactive-tab pause, and a fullscreen toggle with an in-page fallback for browsers that do not expose the Fullscreen API. WASM save/load uses a reusable heap allocator and direct savestate staging to avoid rejected states from stale/corrupt VFS buffers or out-of-memory pressure on larger SmartMedia images.

Only Win64 is supported for the Windows UI because the dynarec is x86_64-only. Win32 is intentionally not provided. SDL 1.2 and SDL3 frontends are both present; SDL3 is the newer input/audio path, while SDL 1.2 remains supported for compatibility.

## Headless usage

```sh
./gp32_headless --bios gp32166m.bin --smc game.smc --frames 3000 --jit --dump-frame frame.ppm
./gp32_headless --bios gp32166m.bin --smc game.smc --frames 3000 --jit --record-mkv capture.mkv
./gp32_headless --fxe homebrew.fxe --frames 600 --dump-frame frame.ppm
```

Current headless capture output is Matroska/MKV with ZMBV video and PCM audio via `--record-mkv`. Y4M output and legacy recording aliases are not supported.



## Optional threading

Worker threads are enabled by default on Win64/Linux builds for the parts that can safely run outside the emulated single-CPU machine: Win64 audio pumping and MKV/ZMBV recording. The emulated ARM920T/S3C2400 execution order remains single-threaded for determinism.

To build without worker threads for small or single-core targets:

```sh
make -f Makefile.sdl GP32EMU_ENABLE_THREADS=0
make -f Makefile.win64 GP32EMU_ENABLE_THREADS=0
cmake -S . -B build -DGP32EMU_ENABLE_THREADS=OFF
```

## CPU execution modes

`--no-jit` now uses the portable bytecode interpreter. ARM instructions are decoded into cached basic blocks and executed by C11 bytecode helpers, so the path remains architecture-neutral and accurate while avoiding repeated instruction decode and MMIO/addressing classification. This is the default portable CPU path on non-x86_64 hosts and when the Win64/SDL/Qt JIT option is disabled.

`--jit` still enables the x86_64 native dynarec when executable memory and direct RAM/BIOS fastmem are available. If native JIT cannot be enabled, the core falls back to the same portable bytecode interpreter.

## Frontend features

The Win64 and Qt6/Linux frontends provide BIOS path configuration, automatic HLE BIOS fallback when no BIOS is configured, save/load state, screenshots, start/stop MKV recording, fullscreen, aspect-ratio scaling, integer scaling, optional LCD persistence / FLU ghosting, optional frame interpolation blending, JIT enable/disable with core reset, and WaveOut/WASAPI audio selection on Win64. WaveOut is the default Win64 audio backend. Live audio backends share the same streaming fractional resampler with queue-drift correction and short de-click fades after underruns. On threaded Win64 builds, waveOut/WASAPI are also pumped from a small MMCSS-priority worker thread so UI/render stalls are less likely to starve the audio device.

HLE BIOS is a fallback, not a preferred boot mode. When a real BIOS path is configured, Qt6/Linux and Win64 force normal BIOS boot for games and disable the HLE fallback menu toggle. If the BIOS path is cleared or missing, HLE fallback is enabled automatically so compatible games can still boot. It can boot selected commercial games and homebrew, and it can fail on other commercial titles. Configure a real BIOS for best compatibility.

## Recording format

Recording writes an MKV container containing:

- ZMBV 320x240 video frames;
- 16-bit stereo PCM audio at 44100 Hz.

The bundled ZMBV encoder is a small emulator-specific encoder, not a full FFmpeg dependency. On threaded builds, compression and MKV writes run on a recorder worker queue; if the worker falls behind, video frames can be dropped rather than blocking emulation/audio.

WASM frontend v95 notes:
- The main browser loop is now presentation-time limited instead of audio-queue limited, eliminating the prior 3-6 frame audio fill bursts that caused visible video frame skipping.
- The renderer caches the video layout and avoids per-frame `getBoundingClientRect()`/canvas resize work; sharp-bilinear pre-scaling is capped to keep high-DPI mobile fullscreen from forcing very large canvas blits every frame.
- FPS/PC video overlay is off by default and can be enabled from the Status panel; when enabled, displayed FPS is capped to the GP32 60 Hz video cadence.
- Mobile touch overlay can be disabled from the Input panel for external gamepad use.
- Touch L/R shoulders are slightly lower and wider to avoid the fullscreen/menu buttons.
- Video options include integer scale, keep-aspect scaling, stretch scaling, nearest-neighbor filtering, linear filtering, and sharp-bilinear filtering for smoother scrolling without fully blurred pixels.
- The WASM frontend pauses by default while the browser tab is inactive. This can be disabled from the Video panel.

WASM frontend v96 notes:
- The video path now prefers WebGL for the 320x240 framebuffer upload and scaling pass, with automatic Canvas2D fallback when WebGL is unavailable. This reduces CPU-side canvas work, especially with keep-aspect/stretch/fullscreen and sharp-bilinear modes.
- The Status panel's frame/PC labels and audio queue label are throttled to avoid per-frame DOM writes during gameplay. This reduces main-thread layout pressure without changing the AudioWorklet jitter buffer or resampler path.
- The WASM ARM block cache is larger and allows longer translated blocks for fewer dispatcher returns in larger commercial games.
- Little Wizard was used as the WASM benchmark path for this pass.



Frontend boot-policy / Qt project v103 notes:
- Restored `GP32emu.pro` to valid qmake syntax. It had accidentally been replaced with CMake syntax in the previous package, causing `option() requires one literal argument` and `Missing closing parenthesis` errors under qmake.
- Qt6/Linux and Win64 now prefer the configured real BIOS unconditionally. Stale saved HLE settings are ignored once a BIOS path exists.
- Setting or opening a BIOS path turns HLE fallback off and persists that state. Clearing the BIOS path turns the fallback back on.

Frontend video effects v97 notes:
- Added shared 320x240 temporal video post-processing used by WASM, SDL 1.2, SDL3, Qt6/Linux, and Win64.
- LCD persistence is a mild FLU-style response/ghosting blend intended to mimic the original GP32 front-lit LCD.
- Frame interpolation blends adjacent presented frames to smooth pixel-art motion without changing emulator timing.
- Both effects are optional and disabled by default. WASM/Qt/Win64 expose menu checkboxes; SDL frontends expose `--lcd-persistence` and `--frame-interpolation`.

## Libretro backend

This source package includes a separate libretro backend. It does not replace the SDL, Qt, Win64, or WASM frontends.

Build on Linux:

```sh
make -f Makefile.libretro clean all
```

Output:

```text
gp32emu_libretro.so
```

Install for RetroArch:

```sh
mkdir -p ~/.config/retroarch/cores ~/.config/retroarch/system
cp gp32emu_libretro.so ~/.config/retroarch/cores/
cp gp32emu_libretro.info ~/.config/retroarch/cores/
cp "[BIOS] GamePark GP32 (Europe) (v1.6.6).bin" ~/.config/retroarch/system/gp32166m.bin
```

Run a game:

```sh
retroarch -L ~/.config/retroarch/cores/gp32emu_libretro.so "Little Wizard (Europe).smc"
```

From the build directory, include `./` in the core path:

```sh
retroarch -L ./gp32emu_libretro.so "Little Wizard (Europe).smc"
```

Supported content extensions are `.smc`, `.fxe`, and `.fpk`. Core options expose JIT, boot mode (`auto`, `require_bios`, or `direct_hle`), LCD persistence, and frame interpolation. Boot mode defaults to `auto`, which uses `gp32166m.bin` when available and falls back to direct/HLE SmartMedia boot if the BIOS is missing. LCD persistence and frame interpolation are optional and disabled by default.

Input uses RetroPad port 1: D-pad maps to GP32 directions, A/B to GP32 A/B, L/R to GP32 L/R, Start to GP32 Start, and Select to GP32 Select. Use RetroArch's normal input remapping for controller-specific mappings.
