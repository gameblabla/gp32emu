# Blue Angelo stage-1 shadow LUT correction

## Symptom

In Blue Angelo, stage 1 shows an emulation-only shadow colour error when the player sprite passes in front of the moon.  The shadow next to/behind the character becomes yellow instead of staying in the intended purple/lavender shadow ramp.  MAME documents the same GP32 software-list problem as an additive-blending bug that is known not to occur on real GP32 hardware.

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
