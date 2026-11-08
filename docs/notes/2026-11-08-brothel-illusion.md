# Tisiphone's brothel illusion at 3.5 frames a second (2026-11-08)

The player continued Profile 3 (the Hecatonchires, after the prison), walked into the building on the bridge
that Tisiphone projects as a brothel, and the game fell to 3.5 frames a second. The overlay read PPU 100%, GPU 8%,
RSX 0%, and the picture was cream and navy.

## Getting there again

- Each session from the launcher now records the pad (`session-<date>-<time>.pad` beside its log), with the
  files the title opens and the threads it starts. The player played to the room once, and `gowa.py bench
  --replay` plays that recording on a copy of the saves it started from.
- The first replay walked off the route: the playback held its clock for 4.7 s at a texture stream's mark with
  the stick still pushed. It now waits at a mark only where the recording had nothing pressed (a load, a menu
  coming up).
- The second replay reached the room, and the frame rate fell as it did in play. Flips/s in 10 s windows from
  the load: 27.2 and 27.6 on the bridge, then 6.7, 3.7 and 3.2 in the room.

## Where the time went

- `GOWA_THREADS`: in the room the SPURS policy-module threads used 450% of a core (413% on the bridge) and the
  two RSX threads 5% each (40% on the bridge). PPU 100% is the main thread's usual figure; the player sees it on
  the menu too.
- `PS3_SAMPLE` on `spurs pm` and `ppu main`: in the room the SPU workers ran the interpreter (`spu_step` 12% of
  their samples, `spu_lifted_lookup` 7%, `spu_interp_run_until_body` 4%), mostly for the EDGE workloads 14-16
  (44%). On the bridge they waited 91% of the time.
- `SPU_INTERP_STATS` and `SPU_INTERP_BYSIG`: 10 to 20 M interpreted steps a second in the room, against 0.1 M on
  the bridge, at LS `0x11400-0x12000` and `0x1D400-0x1E000`. These are EDGE code pieces streamed from the
  executable that `scripts/spu_pic_pieces.json` did not list; the one at EA `0x757CF0` alone took 11.2 M steps
  a second.
- `SPU_DMA_RANGE=755000-767000` with `SPU_DMA_RANGE_UNIQ=1`: the room's jobs fetched 46 distinct pieces, and
  20 of them were not listed. Each one's entry is where its interpreted steps began.

## The fix

The 20 pieces are in `scripts/spu_pic_pieces.json` (50 now), lifted by `gowa.py lift --jobmods-only` with no
unsupported instruction.

- Five of them have the size and entry of a listed piece and differ from it in 1 to 118 bytes:
  - `0x75C320`, beside `0x75BD10`;
  - `0x760240`, beside `0x75FE80`;
  - `0x763BC0` and `0x763E00`, beside `0x763980`;
  - `0x765F40`, beside `0x766080`.
- They are other pieces, not copies.

| The player's route, replayed | Before | After |
|---|---|---|
| The bridge, flips/s | 27.2-27.6 | 27.1-28.0 |
| The room, flips/s | 6.7, 3.7, 3.2 | 26.9, 25.2 |
| Interpreted SPU steps in the room | 10-20 M/s | 0.1-0.2 M/s |

## The colours

The room stayed cream and navy at 25 to 30 frames a second. The player's RPCS3 screenshots show it dark purple,
with a violet haze, warm lamps and normal skin.

Not the cause, each a replay of the route:

- the fog pass (`fp=0x01BB8202`: it decodes the scene target `0x00E10000`, adds the fog and tone maps into
  `0x00A8C000`). Read from both processes in the room with ReadProcessMemory (RPCS3's guest addresses for these
  heaps are the port's minus `0x40000000`), its fog colour turns (0.522, 0.646, 1.000) about 49 s after the load in
  both, and the SPU job's 64x32x16 fog volume holds the same values. Only the exposure differs, 4.0 here against
  5.66, the title adapting to a brighter scene;
- the blend equations (the room uses ADD, MIN and REV_SUBTRACT, none of the signed ones), and logic ops (never on);
- Disable Shadows, Disable MLAA and Disable Motion Blur off;
- EDGE: every lighting native off, the output stage's native off, the job results cache off (`GOWA_EDGE_MEMO=0`),
  the GPU lighting pass off (`GOWA_EDGE_GPU=0`); the General6 natives off (`GOWA_G6_NATIVE=0`);
- FSR 3, frame generation and antialiasing off; depth EQUAL drawn as LESS_EQUAL; culling off with the faces
  flipped; every occlusion query answered visible (`RSX_ZPASS=0`);
- the dialog stream's failed open (`BLISSBODY_W2_209.XVAG`): RPCS3's log has the same probe failing.

Where it was:

- A RenderDoc capture of one room frame (`RENDERDOC_DLL`): 67% of the scene target's pixels hold one value,
  (1.0, 0.863, 0.314, 0.659) in its RGB-over-largest-channel encoding, a colour of (5.6, 4.0, 0.45).
- RenderDoc's pixel debugger on a wall pixel of its material draw: the lit term is 1e-6, the room is as dark as
  RPCS3's, and the colour is 0.25 times an inline constant, (22.4, 16.0, 1.8).
- The wall's program, found by its constants in RPCS3's memory, has the same 36 constants as the port's copy. Its
  microcode, decoded by hand:

  ```
  ADD     R1.w, -H5.w, 0.5         first source with precision 4 (SRC1 [21:19])
  ADD     R1.z, -H5.w, 1           the same
  DIV_sat R3.w, R1.w, R1.z
  ...                              smoothstep(R3.w), squared, into R0.w
  MAD     H0.xyz, R0.w, (22.4, 16.0, 1.8), H0
  ```

- Precision 4 clamps the first source to [0, 1]. The decompiler clamped it after its negate, `saturate(-h5.w)`,
  which is 0 for every positive H5.w, so the gold sheen weighed smoothstep(0.5) squared, 0.25, on every surface.
  RPCS3's `GetSRC` clamps after the abs and before the negate: `-saturate(h5.w)` leaves a negative numerator, and
  the sheen weighs 0 where H5.w is 1.

## The colour fix

`rsx_fp_decompiler.c` clamps the first source after its abs and before its negate, and
`tests/test_fp_decompiler.c` checks the order (105 passed). The [-1, 1] clamp of precision 3 reads the same
either way.

| The player's route, replayed | Before | After |
|---|---|---|
| The room | cream and navy | dark purple, violet haze, warm lamps, normal skin |
| Flips/s, the windows 40-50 s and 50-60 s from the load | 27.4, 24.1 | 26.2, 22.0 |

One run each, the second after the shader cache had every new program. In the last window the frame waits on
the CPU (PPU and SPU 100%, GPU 25%). Every program with a negated, clamped first source compiles again once: the
bench's cache journal grew 6.3 MB over the first two runs, with "Compiling shaders" up on the way in.
