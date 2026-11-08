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

## The colours (open)

The room stays cream and navy at 25 to 30 frames a second. The player's RPCS3 screenshots of the same room show it
dark purple, with a violet haze, warm lamps and normal skin.

What the frame is made of (`RSX_CAP_SHOT` captures in the room):

- The scene target `0x00E10000` holds the lit scene in an RGBM-like encoding: RGB divided by its largest channel,
  the scale in alpha. In the room it is golden at the back, and the characters have normal skin. It is never
  cleared; the room's material draws are all opaque, depth EQUAL after a depth pass.
- One full-screen pass (`fp=0x01BB8202`) decodes it, adds the fog and tone maps into `0x00A8C000`. Its fog comes
  from a 64x32x16 FP16 volume an SPU job writes every frame (PUTs from LS `0x0C840`/`0x0CC40`, pc `0x834C`), and
  from a 256x1 ramp (an identity ramp in alpha, read through remap `0xAA1B`).
- The pass's constants on the bridge and in the room differ in one value, 3.1999 against 3.2000. The fog colour
  (`c0`) is (0.823, 0.882, 1.000) in both, the bridge's sky, and the volume holds the same values in both. The room
  keeps the outdoor atmosphere: a light blue fog over a golden interior comes out cream.

Not the cause, each a replay of the route:

- the blend equations (the room uses ADD, MIN and REV_SUBTRACT, none of the signed ones), and logic ops (never on);
- Disable Shadows, Disable MLAA and Disable Motion Blur off;
- EDGE: every lighting native off, the output stage's native off, the job results cache off (`GOWA_EDGE_MEMO=0`),
  the GPU lighting pass off (`GOWA_EDGE_GPU=0`); the General6 natives off (`GOWA_G6_NATIVE=0`);
- FSR 3, frame generation and antialiasing off; depth EQUAL drawn as LESS_EQUAL; culling off with the faces
  flipped; every occlusion query answered visible (`RSX_ZPASS=0`);
- the dialog stream's failed open (`BLISSBODY_W2_209.XVAG`): RPCS3's log has the same probe failing.

Next: where the title takes the fog pass's colour from (a write watch on its `c0`), and why the illusion's
atmosphere is not applied when Kratos goes in.
