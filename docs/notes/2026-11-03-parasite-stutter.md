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
  Not fixed here.
- **For a capture of a later stutter:** `build/GowARecomp-debug.bat` now also logs the audio ring
  (`AUDIO_RATE=1`) and the SPU workloads (`SPURS_WID_PROF=10`).
