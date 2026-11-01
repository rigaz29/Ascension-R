# Switchable title patches, and one shader per fragment program

Written 2026-11-01. RPCS3 is measured with its "Disable MLAA" patch on (the user's patch
config), and the port ran the title's MLAA tasks, whose output it never shows
([comparison 4.6](2026-10-31-rpcs3-vs-port.md), plan 2.6). This note adds title patches as
switches, off unless switched on, with a launcher group for them: Disable MLAA, Depth of
Field, Motion Blur and Mesh Trimming. Switching MLAA off showed the D3D12 backend compiling a
new shader for each binding of a program, and reading programs the title had already written
over; section 5 fixes both.

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
- Mesh trimming is EDGE's SPU geometry code. It is for drawing above 720p (RPCS3's note: "the
  patch keeps signed-area, back-face, and later culling stages enabled"), not for speed: it can
  only add geometry.
- `jobmod_9b03908f93a9_e77C8` is a second build of the EDGE module that the level does not run.
  Its lift ends at `0xB90C`: no entry it has found reaches the function (`0xE35C`, the same code
  as the first module's `0xD92C`, 253 of 256 words equal). So its edit is optional (section 2).

## 2. How a patch is switched

- `scripts/title_patches.json` is the list. A patch has an id (its settings key), a name, a C
  guard, and its edits, each with the original it must find:
  - `ppu`: instruction words of the title;
  - `spu`: instruction words of a lifted job module, at a local-store address;
  - `data`: bytes of the title.
- `gowa.py lift` checks every original against the ELF and stops on a mismatch: a patch for
  another build of the title must never land. It then:
  - lifts each PPU edit in both forms behind the guard: `ppu_lifter --insn-patches`, which emits
    `if (guard) { new } else { original }`. A call patched out keeps its link and drain in the
    original form (fork test `test_ppu_lifter_insn_patches.py`, case 4);
  - lifts each SPU edit the same way in its module: `lift_jobmods.py --insn-patches` hands each
    module its own, `spu_lifter.py --insn-patches` lifts them (fork test
    `test_spu_lifter_insn_patches.py`: the shape, the refusals, and a run with the guard at 0 and
    1). The modules' bytes in guest memory stay as they are, so the runtime's code checks still
    match; code an SPU interprets there would run unpatched. An edit marked `optional` may lie
    outside the lifted code: the lift reports it (`optional insn patch at 0x...: in no lifted
    function, not applied`) instead of stopping;
  - writes `recompiled/gowa_title_patches.h`: the guards, the data edits, and how many edits each
    patch has. A lift of the job modules alone (`--jobmods-only`) rewrites it too.
- `patches.cpp`, right after `ppu_load_elf` and before the title runs: for each patch switched
  on, it compares the data edits' bytes in guest memory, writes them, sets the guard, and logs
  `[patches] <name>: on (...)`. A byte that differs refuses the whole patch.
- Switched on by, in this order of precedence:
  - `--set patches.disable_mlaa=on` on the command line;
  - `GOWA_PATCH_DISABLE_MLAA=1` in the environment;
  - `[patches]` / `disable_mlaa = on` in `gowa.ini`, which the launcher writes.
- **The launcher's Game patches group** has a checkbox for every patch the build has, in the
  catalog's order (`patches.cpp` lists them for it): a patch added to the catalog shows up after
  the next lift. Save writes `[patches] <id> = on/off`; Defaults turns them all off. An on/off box
  now reads `1`, `true` and `yes` as on too, as the settings do, so a value written by hand is not
  shown off and then saved so.
- Everything is off by default. The guards are read by the lifted code, so they only change
  before the guest starts.

## 3. Adding a patch

1. Add it to `scripts/title_patches.json`.
2. `gowa.py patches` lists the patches and checks them against the ELF (SPU edits too, once a
   lift has placed their modules).
3. `gowa.py lift --chunks 200` (PPU edits), or `lift --jobmods-only` (SPU and data edits only),
   then `build`. The lift writes into a staging folder and replaces only the files that changed,
   so the build stays incremental: three of 203 files for this note's PPU patches.

An instruction edit may not change the control flow the function was lifted with: no branch,
`sc` or CR op on the PPU; no branch, hint, call, channel op, stop or halt on the SPU. A nop, an
immediate, a compare.

## 4. The patches' results

Each measured on one exe, the patch switched by the environment, so the builds' layout cannot
differ.

**Disable MLAA.** Level, ABBA, two runs each (the backend before section 5):
- 20.61 -> 21.63 flips/s (+5.0%), both runs above both (21.38 / 21.99 against 20.41 / 20.62).
  The presents, over the same windows: 20.55 -> 21.47 (+4.5%). With it on the port is at ~90% of
  RPCS3's 24.0, which runs with this patch on.
- The title still makes its five MLAA tasks at start-up, whatever the word says, but never
  signals them: the six `spu job` threads went from 76-79% of a core to 2.6-3.2%.
- The picture: the same scene, nothing missing (`logs/bench-nomlaa`, shots at load + 75 s).
- It made the backend compile new shaders every run, and skip draws while they built: section 5.

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

**Disable Mesh Trimming.** One run against the same exe with nothing on (after section 5):
19.88 against 20.54 flips/s, 2474 against 2413 draws a present (median): more geometry, as
expected; no damage in the shots (`logs/bench-mesh`). It applies in the module the level runs;
the second module's edit is reported as not applied (section 1).

## 5. The backend: one shader per fragment program

With MLAA off, every run compiled hundreds of fragment shaders the cache had never seen (it grew
9,265 -> 11,075 programs over four runs; with MLAA on, none) and skipped ~80,000 draws while
they built, ~0.7% of the draws (`async_shaders`). Two causes, found one after the other.

**A program compiled once for each binding.** The buffered program (its inline constants in b1,
the default) had compiled in which lanes of each unit's fetch decode from sRGB, and which units
are cubes (a cube's coordinate was left unscaled). Both come from what is bound, so one program
became a shader per combination of its bindings: two of them differed in two lines,
`_rsx_gamma_input(...)` and `* fp_tcscale[n].xy`.
- Fork: `rsx_fp_decompile_buffered_color_dyn_ex` wraps every TEX/TXP/TXB/TXL fetch without
  EXP_TEX in `_rsx_gamma_dyn(sample, unit)`, which decodes the lanes `_rsx_gamma_mask(unit)`
  names, behind a uniform branch. The backend defines that as `fp_tcscale[u].z` and writes each
  unit's lanes there for every draw, beside its coordinate scale; every unit's coordinate takes
  its scale (a cube's is 1). The buffered key leaves gamma and cubes out, as it already left the
  scales. TXD/TEXBEM/TXPBEM fetches are not decoded this way (compiled in, a program with one on
  a gamma unit was refused; never seen in this title's logs). `test_fp_decompiler.c`: 12 checks.
- With MLAA on, after one run to fill the cache: 1624 -> 305 pipelines a run, nothing compiled.
- With MLAA off, still 327-398 new shaders a run, every one a different program.

**Programs read after the title had written over them.** The draws of a batch are recorded as
the walk reaches them, but their pipelines are looked up at the batch's end. By then the title
may have reused the program's memory, once the walk has passed the label that frees it.
- A diagnostic build (`FP_RACE`, not kept) compared each program's hash at record and at lookup:
  with MLAA on 0 of 11.1M lookups differed, with it off 22,738 of 11.4M (0.2%, in bursts of
  ~2,000). Built from those bytes a pipeline was a mix of two programs: a wrong draw, and a
  shader never seen before (some even decoded inline constants as an opcode 0x3F, with their
  values in the source).
- Fork: each draw now keeps its program as it was when recorded (`fp_snapshot`): copied once a
  batch per program address, again only if the bytes changed, into the half of a 2 x 4 MB arena
  that b1's slots use, and the lookup builds from it. `FP_SNAPSHOT=0` reads at the lookup, as
  before.

| (`PERF=1`) | shaders compiled | pipelines a run | draws skipped |
|---|---|---|---|
| MLAA off, before | 258-700 | 2082 | ~80,000 |
| MLAA off, after, its 2nd run | 0 | 372 | 5 |
| MLAA on, before | 0 | 1624 | 4 |
| MLAA on, after | 0 | 305 | 3 |

- A new exe's first run still builds its pipelines and the driver's code (1st runs: 39,547 and
  100,956 draws skipped); its second has none of that.
- The shots: the same colours as before (sRGB decode right), nothing missing.

**Speed: the new backend against the old** (`logs/bench-backend`), level, ABBA, two runs each:
- MLAA on: 20.03 -> 20.20 flips/s, within the spread.
- MLAA off: 21.94 -> 22.33 flips/s, both new runs above both old ones; the lowest window 18.9-19.0
  -> 21.6-21.8: no more compile dips.
- With MLAA off the frame clock thread is the next limit: at 77-79% of a core (70-72% with MLAA
  on) it presents ~0.8 a second fewer frames than the title flips. The presents' median is the
  old build's (21.2 / 21.7 against 21.8 / 21.1), their lowest window higher (18.4-18.6 against
  17.1-17.6), and every draw is drawn.

## 6. Left

- Mesh Trimming in `jobmod_9b03908f93a9_e77C8`: an entry that reaches its function, if the
  title ever runs it (`gowa.py jobmods-entries` from an `SPU_INTERP_ENTRIES` run would show it).
- Depth of Field: a scene where it shows, to see it go.
