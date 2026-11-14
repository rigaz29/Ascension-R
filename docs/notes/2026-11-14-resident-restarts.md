# The resident part's restarts: no wait for the GPU, room for the indexes (2026-11-14)

A review asked what the kept vertexes' resident part (`rsx_kept.h`) costs when it starts over, since one session of
issue #9 printed `[vtx-kept] resident part full: started over` 47 times, and whether that was the texture cache's
problem again.

## What the reports say

- The counts per session: #9 47 (2026-11-13 17:06, 0.1.0-alpha.9, ~29 min), 25, 22, 18; #7 229, 197, 106; #13 3.
- In the 47, 21 came between TEM450 and TEM500, one every 23,000-50,000 `draw_arrays`: at the session's average rate,
  every 11-17 s for a few minutes. Each generation took 128-192 MB of new vertexes in that time: the temple keeps
  bringing new EDGE results, not the same ones again.
- At every restart the indexes held 51-63 of their 64 MB and the vertexes 128-192 of their 256 MB: the indexes ran out
  first. They are u32 there, from the lists' u16, and their part was a sixteenth of the vertexes'.

## What a restart cost

A temporary probe in `render_frame` and `kept_resident`, on the replay of #9's session 170610 from that report's save
(`temple_repro.ps1`, render scale 1, no anti-aliasing, a window, no VSync), 13 min, 5 restarts:

- the wait for the GPU: 8.7-16.5 ms;
- the frame of the restart and the next 30: the longest 46-59 ms, against a median of 18-32 before;
- the copies back in those frames: 33-61 MB in 5-10 ms; a whole generation's, ~224 MB in ~42 ms.

So 20-30 ms a restart, under 0.1% of play at 47 a session: one frame that stutters now and then, at most every ~15 s
where the temple is heaviest. Not the texture cache's kind of problem: #9's texture fix was about what a texture
showed (uploads torn while the game wrote them), and a cache that uploads again every frame (the inFAMOUS 2 port's
4x texture cache answers that) costs every frame. This costs a few times a minute, and only a resident part far too
small for a frame's draws makes it every frame (`VTX_RESIDENT_MB=8`: 3,527 restarts,
[the temple's note](2026-11-12-temple-textures.md)).

## The changes (ps3recomp, `rsx_d3d12_backend.c`)

1. **No wait.** When a block finds no room, the frame's end notes the value the fence gets next (it comes after this
   frame's batches, whoever signals it: `move_to_next_frame`, or a wait before it) and the resident part retires:
   `kept_resident` turns every block down, so the draws go through the ring as a block with no room always did, and
   no batch reads the resident part any more. Once the fence reaches the value, the part starts over, at a frame's
   end, with nothing waited for. The log line says how many frames went through the ring: one, every time here.
2. **Room for the indexes.** An eighth of the vertex part in indexes, 128 MB for 256 MB (+64 MB of system memory):
   the vertexes run out first.

## Measured

One run each, the temporary probe and two switches in one build (the old wait, the old sixteenth) for the A/B:

| | Before | After |
|---|---|---|
| #9's session 150705 from report A's save, 9 min (TEM400, TEM450, TEM500): restarts | 6 | 3 |
| its generations' copies | ~217 MB | ~357 MB |
| the events' frames +0 to +3, extra over the median before: mean | 8.3 ms | 5.9 ms |
| their worst frame over the median: median of the events | 9.2 ms | 6.0 ms |
| flips/s, the whole run | 53.14 | 52.95 |
| #9's session 170610 from report 6's save, 13 min (TEM749 to TEM775): restarts | 5 | 3 |
| its generations' copies | ~224 MB | ~353 MB |

The frame in which the part fills (+0) stays ~9 ms over the median either way: it is the one that brings the new
geometry. The frame through the ring (+1) was no slower than the median on average; one of the three of the second
replay took 44.8 ms against 20.9. With 3 to 6 events a run the cost per event is within the noise; the restarts
halved, and the wait is gone.

`VTX_RESIDENT_MB=32` (a part barely above a frame's ~26 MB of copies), 150 s: 5 restarts, one frame through the ring
each, no video memory errors, the picture as usual.

## Not done

- The EDGE memo's arena changes (`spu_edge_memo.c`: at most every 5 s while its 128 MB arena fills) make identical
  results again as new blocks, which the resident part then copies again. A guess for how tight the temple's
  restarts were, not checked.
- The resident part lives in an upload heap: system memory the GPU reads over PCIe every frame. Not measured against a
  copy in video memory.
