# The parasites' fight: stutter, frame drops, audio

Written 2026-11-03. The user played profile 3 up to the first fight with the small green
parasites. There the game stuttered, the frame time rose, the frame rate dropped, and the audio
stuttered too.

## Reproduced

- **Setup:** `gowa.py bench --profile 3` on a copy of the saves continues profile 3's
  checkpoint, a wall top with braziers. Kratos walks right for 13 s (`PAD_STICK`) and attacks
  every 3 s (`PAD_SCRIPT`, Square). That is the fight (`logs/parasite/walk1`, shots at 35 s:
  hits and a combo counter).
- **With the shader cache the user's own session had built** (14,680 programs):
  - 15.4-17.1 flips/s;
  - no frame of 100 ms or more;
  - the audio ring (`AUDIO_RATE=1`): no gaps, no underruns, the game 3 or more blocks ahead
    (2 at times).
- **With an empty cache** (`logs/parasite/fresh`): `graphics.shader_cache` points at a new file,
  so everything the session meets is compiled as it is met, as on a first visit.
  - The shader build threads take 70% of a core.
  - 13.8-14.5 flips/s for the first minute after the load, against 15.4-16.0. Later 16.0-16.7.
  - The audio ring: one 5 s report has 2 gaps (4 blocks read before the game had written them),
    and in several the game is 0-2 blocks ahead.
- **So what the user met** is the first encounter's shader builds, on a CPU the game already
  fills.
  - The game's audio mixing (SPURS workload 7) fell behind, and its blocks played as silence.
  - A second visit takes the shaders from the cache.

## Found on the way: the overlay's sampler

- The user plays with the stats overlay on (`display.show_fps = overlay`).
- Every 0.5 s its sampler took a snapshot of every thread on the system
  (`CreateToolhelp32Snapshot`). At this checkpoint that cost ~16% of a core in the bench's
  thread table.
- Fork (`rsx_d3d12_backend.c`, `ovs_sampler`):
  - the snapshot is taken every 5 s, only to find the PPU, SPU and RSX threads;
  - their handles stay open, and each 0.5 s reads their times.
- The thread is now below the 2% the table lists.

## The shader build threads' priority

- They ran at normal priority, as the game's threads do. The pipeline cache's builder and the
  overlay's sampler already ran below normal; now these do too (fork, `sj_worker`).
- Measured with an empty cache again (`logs/parasite/fresh2`):
  - 13.5-15.2 flips/s in the first minute, against 13.8-16.1 before;
  - one audio report with 3 gaps (3 blocks).
- That is within the spread, and the gaps stay. The builds share the cores (SMT) whatever their
  priority.
- **Not changed:** the HLSL optimisation level (1). It is part of the cache's key, so a lower one
  would compile the user's 14,680 programs again.

## What is left

- **The frame rate of this area, 15-17 flips/s.** The six SPU workers run at ~86-95% and the
  main thread waits on them (performance plan 2.21). That is the machine.
- **Audio under full load.** Workload 7's worker already runs above normal
  (`SPURS_BOOST_WID=7`). Its blocks are late when it waits for a free worker or shares a core.
  The next section measures that wait and keeps SPU 5 for it ("Steady audio").
- **For a capture of a later stutter:** `build/GowARecomp-debug.bat` now also logs the audio ring
  (`AUDIO_RATE=1`) and the SPU workloads (`SPURS_WID_PROF=10`).

## The audio in heavy fights (later the same day)

The user then played with `GowARecomp-debug.bat` twice. First a New Game on profile 3, ~5
minutes (`logs/user-20261103-173520.log`). Then the parasites' checkpoint again, after it was
restored from its save backup (`logs/user-20261103-175318.log`).

- **The New Game's last ~50 s:** the heaviest scene seen yet, 3,500-4,200 draws a frame
  (the gate has ~2,400).
  - 13-20 flips/s.
  - The six SPU workers at ~94% each and the main thread at 92-94%. The GPU took 8-19 ms a
    frame.
  - Five frames of 100-192 ms: none of them textures, shaders or GPU waits.
  - In the last ~20 s, the audio workload's runs grew from 2.3 to 8.7 ms (85% of a core). The
    ring then had 6 gaps (16 blocks), and 20-30% of the blocks played silent.
- **The parasites' fight from its checkpoint:** 4 shaders still new (1.3 s of compiling),
  otherwise cached. The last report had 4 gaps, with the game 0-2 blocks ahead.

**Why.**
- The title lets its audio workload (7) run on SPU 5 only (`spurs_claim`), at priority 1.
- A job is not interrupted. A workload yields only between jobs, when the kernel poll finds a
  stronger one asked for.
- `SPURS_KICK_PROF` in a scripted fight (`logs/audio/kick`): a kick of workload 7 waited
  0.7-0.9 ms for SPU 5 on average, at most 8-16 ms.
- The title keeps ~3 blocks (16 ms) ahead, so a longer wait plays as a gap.

**Tried: SPU 5's worker alone on a core** (`SPURS_AUDIO_CORE`, not kept).
- The process's default CPU sets left one core out, and the worker's own set was one of its
  processors.
- In the scripted fight (`logs/audio/core-ab`), audio's runs went from 2.0-2.2 to 2.4-2.7 ms,
  2 gaps appeared where the base had none, and flips/s fell 16.22 -> 15.87.

**Kept: SPU 5 for the audio workload alone** (fork, `SPURS_RESERVE=<wid>:<spu>`; port,
`[system] steady_audio`).
- How: no other workload may claim the SPU.
- Scripted fight, ABBA, two runs each (`logs/audio/reserve`):

| | flips/s | a kick of workload 7 to a pick | the game 2 blocks ahead, reports' sum |
|---|---|---|---|
| shared | 15.82 (16.22 / 15.63) | 0.75-0.95 ms mean, 8.3-16.4 ms max | 24, 13 |
| reserved | 15.42 (15.43 / 15.33) | 0.1-0.5 ms mean, 1.6-6.8 ms max | 2, 2 |

- The scripted fight has no gaps either way; the user's fights did. The longest wait fell
  below the ~16 ms the title keeps ahead.
- The audio workload now runs ~900 times a second, ~0.65 ms each, against ~265 of ~2.2 ms. That
  is the same share of a core.
- The cost is ~2.5% of the flips, since the other workloads lose SPU 5.
- **On by default**, as the launcher's "Steady audio" (System). Off gives SPU 5 back.
  `GOWA_STEADY_AUDIO=0` does the same, and `SPURS_RESERVE` set by hand overrides it.
- `GowARecomp-debug.bat` now also logs `SPURS_KICK_PROF=10`.
