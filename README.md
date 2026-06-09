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
