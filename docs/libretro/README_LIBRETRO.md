# GP32emu libretro core

This is a separate libretro backend for GP32emu. It builds a normal libretro shared object named `gp32emu_libretro.so` on Linux and does not replace the SDL, Qt, Win64, or WASM frontends.

## Build on Linux

```sh
make -f Makefile.libretro clean all
```

The output is:

```text
gp32emu_libretro.so
```

Install it into your RetroArch cores directory, for example:

```sh
mkdir -p ~/.config/retroarch/cores
cp gp32emu_libretro.so ~/.config/retroarch/cores/
cp gp32emu_libretro.info ~/.config/retroarch/cores/
```

## BIOS placement on Linux

Put the GP32 BIOS in the RetroArch system directory with this exact filename:

```text
~/.config/retroarch/system/gp32166m.bin
```

Example:

```sh
mkdir -p ~/.config/retroarch/system
cp "[BIOS] GamePark GP32 (Europe) (v1.6.6).bin" ~/.config/retroarch/system/gp32166m.bin
```

The core option `Boot mode` defaults to `auto`: it uses `gp32166m.bin` when available and falls back to BIOSless direct SmartMedia boot for `.smc` content if the BIOS is missing. For maximum compatibility, put the BIOS in the system directory and leave Boot mode on `auto`, or set it to `require_bios` if you want missing BIOS to be treated as an error.

## Running a game

With RetroArch installed and the core copied into RetroArch's core directory:

```sh
retroarch -L ~/.config/retroarch/cores/gp32emu_libretro.so "Little Wizard (Europe).smc"
```

When running from the current build directory, include `./` so RetroArch/dlopen treats the core as a path rather than a core name:

```sh
retroarch -L ./gp32emu_libretro.so "Little Wizard (Europe).smc"
```

`--libretro=./gp32emu_libretro.so` also works, but `-L ./gp32emu_libretro.so` is the most common RetroArch command-line form.

From RetroArch's UI: Load Core -> GP32emu, then Load Content -> select a `.smc`, `.fxe`, or `.fpk` file.

## Core options

The core exposes these options through RetroArch's Core Options menu:

- Dynamic recompiler: enabled by default.
- LCD persistence / GP32 FLU ghosting: disabled by default.
- Frame interpolation: disabled by default.
- Boot mode: `auto` by default. `auto` uses BIOS when found, `require_bios` fails clearly if the BIOS is missing, and `direct_hle` forces BIOSless direct/HLE loading.


## Controls

Port 1 maps to a standard RetroPad:

- D-pad -> GP32 stick directions
- A -> GP32 A
- B -> GP32 B
- L -> GP32 L
- R -> GP32 R
- Start -> GP32 Start
- Select -> GP32 Select

Use RetroArch's normal input remapping for controller-specific remaps.

## Save behavior

Libretro serialization is implemented through GP32emu's native savestate path, so RetroArch save states are supported. SmartMedia writeback is attempted on unload into the RetroArch save directory as `<game>.gp32.smc`.

## Troubleshooting

If RetroArch immediately returns to the shell, run with logging enabled:

```sh
retroarch --verbose -L ./gp32emu_libretro.so "Little Wizard (Europe).smc"
```

Also make sure the core path includes either an absolute path or `./`. On Linux, `gp32emu_libretro.so` without a slash may be interpreted as a core name or searched through RetroArch's configured core directories rather than the current directory.
