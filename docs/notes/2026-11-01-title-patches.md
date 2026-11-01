# Switchable title patches: Disable MLAA first

Written 2026-11-01. RPCS3 is measured with its "Disable MLAA" patch on (the user's patch
config), and the port ran the title's MLAA tasks, whose output it never shows
([comparison 4.6](2026-10-31-rpcs3-vs-port.md), plan 2.6). This note adds title patches as
switches, off unless switched on, so that a launcher can offer them later: Disable MLAA, and
with it Depth of Field and Motion Blur.

## 1. The sites

RPCS3's patch list for this build (`PPU-3a0b43e4...`, GOWA.SELF 1.12) was the map. Every site
below was read back from our `GOWA.elf` (scratchpad `patch_sites.py`, `flag_xrefs.py`).

| patch | site | in our ELF | the edit |
|---|---|---|---|
| Disable MLAA | data `0x83D007` | `01`: the low byte of the word at `0x83D004`, which is `00000001` | `00` |
| | `0x397754` | `bl 0x397408` in `func_003973D0` | `nop` |
| | `0x397784` | `bl 0x397408` in `func_003973D0` | `nop` |
| Disable Depth of Field | `0x43BCBC` | `lwz r5,0x20(r31)` in `func_0043BA48`, before `cmpwi r5,0` | `li r5,0` |
| Disable Motion Blur | `0x43F210` | `lwz r5,0x20(r31)` in `func_0043F0D0`, the same code | `li r5,0` |
| Disable Mesh Trimming | SPU, `0x741074` | `and $4,$5,$25` in the EDGE module `jobmod_e8f44fbc6dbf_e6D98`, LS `0xDAF4` | `il $4,0` |
| | SPU, `0x726A24` | the same in `jobmod_9b03908f93a9_e77C8`, LS `0xE524` | `il $4,0` |

- **The word at `0x83D004` is the title's own MLAA switch.** It is read in 18 places, from
  start-up code (`0x30454`) to every frame (`func_0041C344`, which arms the MLAA counter, reads
  it at `0x41C3F8`). At 0 the title takes its own paths without MLAA.
- `0x397408` builds a 64-bit entry and stores it at `0(r3)`; it reads the same word and leaves
  a constant out when it is 0. `func_003973D0` calls it three times with `r4` = 4, 5 and 6; the
  patch skips 4 and 6 and keeps 5. What the entries are is not worked out here.
- Mesh trimming is EDGE's SPU culling code. It is for drawing above 720p (RPCS3's note: "the
  patch keeps signed-area, back-face, and later culling stages enabled"), not for speed: it can
  only add geometry.

## 2. How a patch is switched

- `scripts/title_patches.json` is the list. A patch has an id (its settings key), a name, a C
  guard, and its edits: `ppu` (instruction words) and `data` (bytes), each with the original it
  must find.
- `gowa.py lift` checks every original against the ELF and stops on a mismatch: a patch for
  another build of the title must never land. It then:
  - lifts each instruction edit in both forms behind the guard: `ppu_lifter --insn-patches`,
    which emits `if (guard) { new } else { original }`. A call patched out keeps its link and
    drain in the original form (fork test `test_ppu_lifter_insn_patches.py`, case 4);
  - writes `recompiled/gowa_title_patches.h`: the guards and the data edits.
- `patches.cpp`, right after `ppu_load_elf` and before the title runs: for each patch switched
  on, it compares the data edits' bytes in guest memory, writes them, sets the guard, and logs
  `[patches] <name>: on (...)`. A byte that differs refuses the whole patch.
- Switched on by, in this order of precedence:
  - `--set patches.disable_mlaa=on` on the command line;
  - `GOWA_PATCH_DISABLE_MLAA=1` in the environment;
  - `[patches]` / `disable_mlaa = on` in `gowa.ini`. A launcher checkbox would write that line:
    one more row in `launcher.cpp`'s table, section `patches`, key the patch's id.
- Everything is off by default. The guards are read by the lifted code, so they only change
  before the guest starts.

## 3. Adding a patch

1. Add it to `scripts/title_patches.json`.
2. `gowa.py patches` lists the patches and checks them against the ELF.
3. `gowa.py lift --chunks 200`, then `build`. The lift now writes into a staging folder and
   replaces only the files that changed, so the build stays incremental: three of 203 files
   for this note's patches.

Instruction edits may not branch (the lifter refuses a branch, `sc` or CR op as a
replacement): a nop, an immediate, a compare. Mesh trimming's edits are SPU instructions inside
lifted job modules, so they need an SPU lifter counterpart of `--insn-patches`; the catalog
refuses `spu` edits until that exists.

## 4. Results

Each measured on one exe, the patch switched by the environment, so the builds' layout cannot
differ.

**Disable MLAA.** Level, ABBA, two runs each:
- 20.61 -> 21.63 flips/s (+5.0%), both runs above both (21.38 / 21.99 against 20.41 / 20.62).
  The presents, over the same windows: 20.55 -> 21.47 (+4.5%). With it on the port is at ~90% of
  RPCS3's 24.0, which runs with this patch on.
- The title still makes its five MLAA tasks at start-up, whatever the word says, but never
  signals them: the six `spu job` threads went from 76-79% of a core to 2.6-3.2%.
- The picture: the same scene, nothing missing (`logs/bench-nomlaa`, shots at load + 75 s).
- **A cost, for now.** Without MLAA the title binds its textures in combinations the shader cache
  has not seen. The backend builds a variant of a fragment program for each combination of its
  units' gamma decode and coordinate scale: two programs that differ only there differ in two
  lines (`_rsx_gamma_input(...)`, `* fp_tcscale[n].xy`).
  - Each run with MLAA off added 250-700 programs to the cache (9,265 -> 11,075 over four runs);
    a run with it on added none.
  - In a run with `PERF=1`, 258 programs compiled with MLAA off, none with it on. While one
    builds its draws are skipped (`async_shaders`): 79,853 in a run, about 0.7% of the draws,
    against 4 with MLAA on. In those windows the presents fall behind the flips (18.2 against
    20.4).
  - The fix is the backend's: the gamma masks and coordinate scales from the constant buffer
    instead of the program's source, so that a program is one shader whatever is bound. Every
    path gains from it: this is also where first-play hitches come from (plan 4.4).

**Depth of Field, Motion Blur.** One run each against the same exe with nothing on:

| | flips/s | draws a present, median / p90 |
|---|---|---|
| nothing on | 20.33 | 2441 / 3392 |
| Disable Depth of Field | 20.76 | 2394 / 3385 |
| Disable Motion Blur | 22.16 | 2378 / 3242 |

- Both apply (`[patches] ...: on`), draw less, and compile nothing new.
- Motion Blur: at load + 75 s both runs caught the same pose. With the patch Megaera is sharp,
  without it her wings are smeared; nothing else changed (`logs/bench-dofmb`).
- Depth of Field: no damage seen, but this scene's shots are dominated by motion blur, so its
  effect is not told apart here. One run each, so the frame rates are a first look only.

## 5. Left

- The backend's per-binding program variants (section 4).
- Mesh Trimming: an SPU counterpart of `--insn-patches` in `spu_lifter.py`, passed through
  `lift_jobmods.py` for the two EDGE modules. The modules' bytes in guest memory stay as they
  are, so the runtime's code checks still match.
- The launcher rows: a `patches` group, one checkbox per patch.
