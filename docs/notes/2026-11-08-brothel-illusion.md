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

The room stays cream and navy at 25 to 30 frames a second.

- A replay with `GOWA_EDGE_GPU=0`, bilinear scaling, no frame generation and no antialiasing gives the same
  picture, so none of those paths is the cause.
- Everything bright comes out cream and everything dark navy, the way a two-tone grade does it.
- The shader that samples a volume texture (`fp=0x1BB8202`) is first used on the menus, before the level.
- Whether the PS3 shows the scene this way is not known yet: no reference was found.
