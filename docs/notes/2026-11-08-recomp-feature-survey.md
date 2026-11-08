# What other static recompilers have that ps3recomp does not (2026-11-08)

`gh search repos` over "static recompilation", "static recompiler", "ps3recomp", "XenonRecomp", "rexglue" and the
like found 412 repositories. The READMEs of 66 of them were scanned for features and compared with this tree (the
port and the fork's `gowa-edge`). About ten were checked down to file names. None was built or run, so every claim
and number below is the project's own.

## Already here

Not candidates: instruction hooks (`ppu_lifter --insn-hooks`, `scripts/ppu_hooks.json`), switchable title patches
(`--insn-patches`, `scripts/title_patches.json`), the launcher's cheats, PGO of the lifted PPU and SPU code
(`PORT_PPU_PGO`, `PORT_SPU_PGO`), the shader and pipeline cache, vertex programs compiled ahead of play
(`--precompile-shaders`), FSR 3 and frame generation, and pad recording and replay.

## In order of work

1. **A profile of the code that was missed, read by the next lift.** Done, see
   [the note](2026-11-08-missed-code-profile.md). The executable records what fell to the
   interpreter or had no lifted code, and the next build compiles it. Until then this was done by hand with
   `SPU_INTERP_STATS`, `SPU_INTERP_BYSIG` and `SPU_DMA_RANGE`, and a PPU call to code that was not lifted only logs
   a line (`ppu_unlifted_stub`). The 20 EDGE pieces behind the brothel room's 3.5 frames a second were found that
   way ([the note](2026-11-08-brothel-illusion.md)).
   - [kirikae](https://github.com/Cor3Duo/kirikae) (`crates/rt/src/coverage.rs`, Apache-2.0): writes
     `<name>.profile` every 10 s; the next build compiles those addresses; a profile from another dump is refused.
   - [psxrecomp](https://github.com/RetroPortingToolKit/psxrecomp): overlays captured as they load, compiled
     offline and keyed by content.
   - [Xenon-Recomp](https://github.com/nimauria/Xenon-Recomp) (MIT): runtime observations fed to the next
     preparation. It has no playable title yet, so read it for the design only.
2. **Internal resolution above 720p.** Done, see [the note](2026-11-08-internal-resolution.md). Everything was drawn
   at 720p and FSR 3.1 scaled it, with the GPU at 25% in the brothel room. [ps3recomp issue #546](https://github.com/sp00nznet/ps3recomp/issues/546) has a design from the
   inFAMOUS 2 port: every colour and depth target scaled, viewports and scissors with them, fragment programs
   dividing WPOS back to guest pixels (one decompiler hook), guest readbacks point-sampled down to the guest's size,
   guest textures untouched. Also in [Ratchet](https://github.com/Re-TeRo17/ratchet-tod-recomp) (`RSX_SCALE`, 0.5x
   to 8x) and [Demon's Souls](https://github.com/timboat/demons-souls-recomp) (`DES_RES_SCALE`, up to 4).
3. **PPU registers as C locals.** [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (MIT) has `ctr_as_local`,
   `xer_as_local`, `reserved_as_local`, `cr_as_local`, `non_argument_as_local` and `non_volatile_as_local`, valid
   where the title keeps to the ABI, and reports 20 MB less code and several milliseconds a frame on Sonic Unleashed.
   `ppu_lifter` has `--lazy-flags` only, and `spu_lifter --loop-locals` covers single-block loops. Measure first: in
   the brothel room the SPUs are busy as well.

## Picture and controls

| Feature | Where | Here |
|---|---|---|
| In-game settings menu (Dear ImGui, F1): video, rebinding, cheats | Ratchet `src/ui/tod_settings_menu.cpp`, inFAMOUS 2, UnleashedRecomp | launcher and a statistics overlay |
| Texture dump and replacement by hash, with an overlay form; also button prompts | [DPRecomp](https://github.com/LittleBitUA/DPRecomp) `replacement.cpp`, Demon's Souls `LD_TEX_DUMP`, issue #546 (`PS3_MODS`) | none |
| FOV slider and Hor+ ultrawide with the HUD kept 16:9 | inFAMOUS 2 | none; `title_patches` fits |
| 60 fps at the right game speed | Demon's Souls, Ratchet, inFAMOUS 2 | frame generation only; needs title patches and CPU headroom |
| Mouse camera | Demon's Souls, inFAMOUS 2 | mouse buttons map to pad buttons, no look |
| Adaptive triggers; real SIXAXIS sensors | Ratchet; [Bakugan](https://github.com/Mauryln/Bakugan-Battle-Brawlers-PS3-recomp) | `cellPad` sends constant sensor values |
| Trophy notices | UnleashedRecomp achievements | `sceNpTrophyUnlockTrophy` prints to the log |

## Speed and stability

| Feature | Where | Here |
|---|---|---|
| Pipelines compiled while a level loads | UnleashedRecomp walks the render structures at asset load | compiled at first draw. Vertex programs ahead of play; 33,525 distinct fragment programs in the archives, so per level |
| SPU natives shared across titles, keyed by a hash of the code | Ratchet (skeletal animation, collision), gow-ascension-pc (EDGE) | per title (`scripts/spu_native_hooks.json`), though EDGE is Sony's SDK |
| P-cores only on Intel hybrid CPUs | Ratchet `TOD_PCORES`, which reports it ended freezes on 12th to 14th gen | none |
| Crash dump file | Ratchet 1.2.0, SpeedBreaker, DPRecomp | a stack in the log |
| Saves written whole or not at all | inFAMOUS 2 | `system.save_backups` |

[gow-ascension-pc](https://github.com/arnaldolima1/gow-ascension-pc) (MIT) is the same game on the same toolkit and
measures each change on its own (Ryzen 7 5700G, first level): PGO 6 ms a frame, the main thread above normal
priority 2.7 ms, end-of-frame jobs spread over 4 or 5 SPUs 2.1 ms, the RSX thread asleep on an empty queue about 8%.
It puts about 55% of the frame in EDGE, and names the job manager's interruptions inside each job as the largest
waste there.

## Tools

| Feature | Where | Here |
|---|---|---|
| TCP debug server: pause, memory, render targets, input | [nesrecomp](https://github.com/mstan/nesrecomp) `runner/src/debug_server.c`, psxrecomp `docs/TCP_COMMANDS.md` | `PS3_DEBUG`, commands through a file |
| Deterministic replay; a per-frame state hash | [bo2-mac](https://github.com/adrbn/bo2-mac) `runtime/win32/bo2_replay.cpp`, [ssbu-recomp](https://github.com/OpenSmash-Ultimate/ssbu-recomp) | the pad replay gets through the brothel door about half the time |
| Recorded calls replayed against generated code; fuzzing against a reference | [bloodborne-recomp](https://github.com/bloodborne-native/bloodborne-recomp) `tools/recomp/replay.c`, [snesrecomp](https://github.com/RetroPortingToolKit/snesrecomp) | none. A fuzz of the fragment decompiler would have caught the clamp and negate order |

## Distribution

| Feature | Where | Here |
|---|---|---|
| The game's code built on the player's PC from their own disc | [XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp) (bundled recompiler, Clang and LLD), Demon's Souls | built by hand |
| The lift as a DLL, so a runtime change does not relink it | [ps3recomp issue #603](https://github.com/sp00nznet/ps3recomp/issues/603) | linked into the executable |
| A mods folder over the game's files; code mods | Ratchet `TOD_MODS`; [N64Recomp](https://github.com/N64Recomp/N64Recomp), XboxXBERecomp's overridable functions | none; this title's files sit inside PSARC archives |
| Online | [psnr](https://github.com/sp00nznet/psnr) (rooms, leaderboards), gow-ascension-pc (two players matched on one PC) | offline answers |

## Licenses

Upstream takes MIT, BSD, zlib and Apache sources, never GPL.

- GPL, ideas only: UnleashedRecomp, Zelda64Recomp, bo2-mac, ssbu-recomp, bloodborne-recomp.
- GitHub detects no license, check before copying: Ratchet, Demon's Souls, inFAMOUS 2, nesrecomp, psxrecomp.
- MIT or Apache-2.0: XenonRecomp, N64Recomp, DPRecomp, XboxXBERecomp, Xenon-Recomp, gow-ascension-pc, psnr, kirikae.
