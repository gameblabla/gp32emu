# Blue Angelo stage-1 shadow LUT correction

## Symptom

In Blue Angelo, stage 1 shows an emulation-only shadow colour error when the player sprite passes in front of the moon.  The shadow next to/behind the character becomes yellow instead of staying in the intended purple/lavender shadow ramp.  MAME documents the same GP32 software-list problem as an additive-blending bug that is known not to occur on real GP32 hardware.

The test case used for this emulator is `blue_angelo_gp32_state.gp32st.zip`, which resumes in stage 1 during movement near the moon backdrop.  Moving left/right across the moon makes the yellow shadow endpoint visible in the uncorrected build.

## Checks that were ruled out

The failure was first compared against the LCD/palette work used for Little Wizard.  The following alternatives did not match the observed Blue Angelo fault and are not the fix:

- RGB/BGR channel permutations.
- 5:6:5 versus 5:5:5:I LCD palette decoding.
- LCDCON4 palette offset handling.
- BPP8 byte/halfword fetch order.
- A global palette or framebuffer colour transform.

Those tests changed the whole frame or the moon/background colours, but the real failure is local to the shadow table used by the stage-1 sprite blend path.

## Runtime observation

At runtime, Blue Angelo builds a 256-entry 8-bit software shadow/blend lookup table in GP32 RAM.  In the savestate and in fresh direct-HLE runs, the relevant table is at:

```text
0x0c747200
```

The table maps source/background palette indices to shadow palette indices.  Its intended endpoint is a contiguous purple/lavender shadow ramp:

```text
0x71 .. 0x77
```

The bad emulated table, however, allows the highest luminance bucket to spill into palette index:

```text
0x78
```

In Blue Angelo's stage-1 palette, `0x78` is not part of the purple shadow ramp.  It is a normal yellow game colour.  When bright moon pixels are used as the blend source/background, table entries such as offsets `0x9e`, `0x9f`, and `0xbf` resolve to `0x78`, producing the yellow shadow visible beside the player.

## Emulator correction

The correction is intentionally narrow and data-shaped.  It does not alter the LCD palette hardware model and does not remap yellow globally.

`direct_fix_gp32_additive_blend_shadow_endpoint()` checks candidate runtime LUTs and only acts when all of the following are true:

1. The candidate lies in GP32 RAM and is 256 bytes long.
2. At least 248 of the 256 entries are in the `0x71..0x78` range.
3. The top endpoint `0x78` appears only rarely, currently no more than eight entries.
4. The table has enough transitions to be a quantised blend/shadow table rather than a flat fill.
5. The known bright-moon endpoint entries at `+0x9e`, `+0x9f`, and `+0xbf` are all `0x78`.

When that shape is matched, only table values equal to `0x78` are clamped to `0x77`:

```text
0x78 -> 0x77
```

This keeps the shadow on the intended purple/lavender ramp while leaving ordinary palette index `0x78` uses elsewhere untouched, including normal yellow art such as HUD or crescent/moon details.

## Placement

The correction runs at direct-HLE runtime boundaries and after savestate load, so both fresh execution and the supplied stage-1 savestate receive the same table repair.  It is not applied as a ROM patch and it is not keyed to a framebuffer coordinate.

## v59 regression status

v59 keeps this additive-blend endpoint correction on top of the v58 colour-valid baseline.  The full GP32_games.zip SMC set was smoke-tested for boot/render completion, selected HLE-sensitive games were rerun at 5M cycles, and the Blue Angelo supplied stage-1 savestate still has the LUT endpoint repaired while the title/start capture remains pixel-identical to the v50/v58 colour-valid reference.

## Limitation

This documents and constrains the observable hardware compatibility fix.  The exact missing low-level hardware side effect that prevents Blue Angelo's LUT generator from selecting `0x78` on real GP32 hardware is still not identified.  Until that is known, the emulator keeps the correction limited to the canonical Blue Angelo shadow-ramp table shape rather than changing global LCD or palette behaviour.

## Top landscape scanline / portrait edge observation

A separate Blue Angelo savestate, `bug_top_gp32_state.gp32st.zip`, exposed a one-pixel multicolour line at the top of the rotated 320x240 host image while the player is moving in the same stage-1 moon/statue area.

The savestate reproduces the same artifact when loaded with or without a BIOS argument, because the savestate restores CPU, RAM, and LCD register state after boot.  A fresh BIOS run was also checked, but short scripted input only reached the BIOS/menu/title path and did not reproduce this exact stage-1 position.

The raw LCD register state for the savestate is the normal GP32 portrait panel mode: a 240x320 TFT framebuffer that the frontend rotates to 320x240 landscape.  The visible top row after counter-clockwise rotation corresponds to the final raw portrait column, x = 239.  In the failing frame, that final portrait column contains transient/right-edge bytes while the adjacent column, x = 238, contains the expected edge-latched image data.  This makes the top host row look like a corrupted scanline even though the game framebuffer body is otherwise correct.

The emulator models the GP32 panel edge latch after LCD DMA rendering, not as a game-specific framebuffer edit.  v55/v56 copied the final raw portrait column from the preceding column unconditionally, but that was too broad: smooth intro/fade artwork can legitimately differ slightly at the panel edge, and unconditional copying changes those colours.

The latch is now conditional.  For the normal GP32 portrait geometry, 240x320, the renderer compares raw columns x=239 and x=238.  It copies x=238 over x=239 only when the final column is strongly discontinuous from the preceding column across a substantial portion of the screen:

```text
large_delta = count_y(rgb_delta(fb[y,239], fb[y,238]) > 50)
total_delta = sum_y(rgb_delta(fb[y,239], fb[y,238]))
if large_delta >= 64 and total_delta >= 6400:
    for y in 0 .. 319:
        fb[y,239] = fb[y,238]
```

This leaves S3C2400 DMA address progression, palette decoding, BPP8/BPP16 fetch order, and LCD register behaviour unchanged.  It only models the visible panel edge behaviour when the raw final column is demonstrably an edge glitch.  The validation case is `bug_top_gp32_state.gp32st.zip`; after the correction, the top rotated row no longer shows the multicolour line, while Blue Angelo intro frame 5500 remains pixel-identical to v50.
