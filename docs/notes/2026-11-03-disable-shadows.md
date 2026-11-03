# A Disable Shadows patch

Written 2026-11-03. The user asked for more ways to raise the frame rate on their laptop.

The port is CPU-bound:

- six SPU threads full;
- the PPU main thread at ~95%;
- the frame clock thread at 55-85%.

The GPU is not the limit, so a lower resolution or fewer textures would not help. Less work for
the CPU would.

Shadows are about half of a frame's draws:

- at the gate after the first fight: 6 shadow maps, 459 draws into the 1408x1408 atlas and 768
  screen-space shadow-mask draws, of 2,412;
- in the fight: 9 maps, 676 + 530 of 2,461.

RPCS3 has no shadows patch for this title, so this one was found here.

## Leads that went nowhere

- **The debug command names.** The ELF holds a run of 201 debug command names (`0x699490`):
  "Disable Shadows" is entry 185 (`0x699EC6`), next to "Fast Mode", "Honey Badger Mode" and
  "Force 30Hz".
  - No code forms an address in the run: lis/addi, TOC slots, `addis r2` bases.
  - No word in the ELF points into it.
  - In this build it is names only.
- **The compare chain over 184-186.** `func_00234E90` compares with 184, 185 and 186, but its
  cases log "send party invite", "accept party invite" and "kick from party": network messages.
- **`PS3_bShadows`** is referenced only from the shader-parameter name table (`0x8575D0`).

## The FX classes

The Depth of Field and Motion Blur patches act in effects' per-frame updates. Motion Blur's is
slot 2 of `fxMotionBlurProcess`' vtable (`0x6AA2E0`): `func_0043F0D0`, the patched function.

An RTTI walk (`fx_rtti.py`: the type name, its typeinfo, the vtables pointing at it, the OPD
descriptors of their methods) gives the shadow effects:

| class | vtable | slot 2 (the update) |
|---|---|---|
| `WBCascadeShadowFX` | `0x6A5BE8` | `func_00388E58`, returns at once when +0x50 is 0 |
| `fxShadows` | `0x6A58D8` | `func_00376220` |
| `fxNewShadows` | `0x6A5940` | `func_00376748` |

Each update was skipped in turn without a rebuild:

- `GUEST_POKE=<slot>:00828410` points the slot at the descriptor `0x828410`. That is a function
  that only returns (`0x539C1C`, a bare `blr`; the ELF has 369 such).
- The runs were at the gate (profile 2's checkpoint), with Kratos idle, Disable Mesh Trimming
  off and Disable Motion Blur on (`logs/shadow1`).
- The baseline is 14.01 flips/s, 14.04 at 110 s (`logs/slow3`).

| skipped | flips/s | shadow maps | atlas draws | mask draws | frame draws | frame clock |
|---|---|---|---|---|---|---|
| nothing (the user's capture) | 14.01 | 6 | 459 | 768 | 2,412 | 67.5% |
| `WBCascadeShadowFX` | froze while loading the checkpoint, twice (no flips for a minute) | | | | | |
| `fxShadows` | 14.10 | 6 | 253 | 768 | 2,729 | 63.8% |
| `fxNewShadows` | **15.07** | **0** | **0** | **0** | 1,716 | 55.0% |

- **`fxNewShadows` is the title's dynamic shadows.** With its update skipped, a frame draws no
  shadow map and no mask.
  - The mask target keeps its clear (white), so the scene is lit as if unshadowed.
  - The picture is right, without the shadows.
- **At the gate the gain is small.**
  - The frame clock thread does less: 67.5% -> 55%.
  - The SPUs stay full. EDGE's three workloads are still ~115% of a core each, at 6.4-6.8 ms a
    run against 6.8-7.0.
- **In the fight the gain is large.**
  - Megaera's checkpoint from the user's save backup, Disable Mesh Trimming off, Disable Motion
    Blur on (`logs/shadow2`): 23.7 flips/s with shadows, 34-36 without.
  - General6 (w2) fell from 103% of a core to 28%.
  - The bench called that run failed: it takes a median of 30 or more for a menu.
- **But skipping the whole update breaks the picture there.**
  - Where the sky is, giant teal copies of Megaera's wings stand. Kratos and Megaera themselves
    are drawn right.
  - A capture of such a frame (`logs/shadow3`) draws the same non-shadow draws as one with
    shadows (1,122 against 2,296 - 664 - 514 = 1,118).
  - So existing draws get other data. The update does more than shadows, and the systems it
    feeds get out of step.

## The effect's own off switch

`func_00376748` ends at `0x376A04`; the function list joins it to the next one. Its body:

1. Asks the renderer for a pass ("default", 5) when +0x104 is set.
2. Runs the FX process base on `this+16` (`bl 0x3B8078`, `0x3B8388`, a virtual call).
3. Then, as Motion Blur's update does at `0x43F1F8`: `bl 0x3B69B8` on `this+16`, and
   `lwz r4,0x20(r31)` at `0x376894`, `cmpwi r4,0`.
   - **Not 0:** the effect's parameters (30 floats from +0x8C) are copied into the renderer's
     shadow parameter block at `0x982F08`.
   - **0:** that block is cleared, `memset(0x982F08, 0, 0x78)`.

+0x20 is the effects' enable, the field the Depth of Field and Motion Blur patches read as 0.
Reading it as 0 here takes the title's own path for shadows off, and leaves the rest of the
update as it is.

## The patch

`disable_shadows` in `scripts/title_patches.json`: one PPU edit, lifted both ways behind
`g_gowa_patch_disable_shadows`, as the others are.

- `0x00376894`: `lwz r4,0x20(r31)` (`0x809F0020`) -> `li r4,0` (`0x38800000`).
- `gowa.py lift` and `build`. The launcher's Game patches group shows it.

## Results

Megaera's checkpoint (the opening cinematic, then the fight), Disable Mesh Trimming off,
Disable Motion Blur on (`logs/shadow4`).

- Shadows on: 23.27 flips/s (windows 22.7-25.4).
- Disable Shadows on: 28.3-30.1 flips/s over 110-150 s.
  - The system stopped that run for low memory at ~155 s, so it has no median of its own.
- A capture of a frame at ~125 s:
  - no shadow map and no shadow-mask draw;
  - 885 draws, against 2,296 with shadows and 1,122 with the whole update skipped.
- The pictures are right in the cinematic, the fight and the capture: the sky is there, without
  the dynamic shadows.

Not measured with this edit: the gate after the fight. Skipping the whole update gained 7.5%
there.

`gowa.py bench --menu-fps N` (new) sets the median that counts as stuck on a menu (default 30).
The fight runs above 30 without shadows.

## The combination, and on by default

The user asked to test the recommended set and, if it held, make it the default. The set:
Disable MLAA on, Disable Mesh Trimming off, Disable Motion Blur on, Disable Shadows on. It was
tested against the user's settings until then (Disable MLAA and Disable Mesh Trimming on), one
run each:

| | the user's settings | the set |
|---|---|---|
| Megaera's checkpoint: the opening cinematic, the fight (`logs/combo1`) | 21.31 flips/s | 30.10 (windows 28.2-31.1) |
| the gate, walking and jumping on for ~4 min (`logs/combo2`) | 17.61 | 22.06 |

- **Hitches of 150 ms or more:**
  - the level's load (~600 ms, 330 textures) in every run;
  - one of 894 ms at the opening movie's close, with no backend work in it. The title closes its
    video decoder there.
  - None in play.
- **Logs:** no error.
- **Render targets:** 27-29 held, none evicted.
- **Screenshots:** right, in the cinematic, the fight, the gate, the jumping area and the yard
  after it.

**Made default.**

- The catalog's `"default": true` is new. `gowa.py` writes it into the header; `patches.cpp` uses
  it as the setting's default; the launcher shows it and its Defaults button sets it.
- Disable MLAA, Disable Motion Blur and Disable Shadows have it. Disable Mesh Trimming and Disable
  Depth of Field stay off.
- A value in `gowa.ini` still wins. So the user's `[patches]` lines were set to the new set, with
  a copy of the file kept beside it.
- Disable MLAA costs the bloom: in the title's path without MLAA, bloom and the motion blur are
  drawn and then overwritten ([FSR 3 note](2026-11-03-fsr3.md), section 3).
