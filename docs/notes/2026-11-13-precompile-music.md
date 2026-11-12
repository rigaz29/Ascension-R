# The precompile screen's music (2026-11-13)

- **What:** the precompile screen (the gate, `port/loading_screen.cpp`) plays "Embers", a theme written for this
  project, over the scene's sea, wind, brazier and thunder after its lightning. All of it is made by code when the
  screen opens (`port/precompile_music.cpp`): nothing recorded, no file shipped. The launcher plays none.
- **How it came about:** three 30 s sketches rendered to MP3 and listened to: a calm D Dorian, an A minor in 6/8, and
  "Embers", D minor with the Phrygian E flat. Embers was picked and grown into a loop with the weight of the series'
  music: war drums, a male choir, low brass, an ostinato on D and a second theme in D Phrygian dominant. Only that
  style is borrowed (big drums, choir, the modes): no melody or motif of the games' scores.
- **The theme:** 90 a minute, 32 bars, 85.3 s. Intro, bars 0-3: a drone on D, a heartbeat on the war drum, the choir
  on "oo" coming in, the lyre from bar 2. A, 4-11: theme A over the lyre and toms, the ostinato in eighths from bar 6.
  B, 12-19, the climax: theme B, the choir on "ah", brass stabs, war drums in 3+3+2, the ostinato in sixteenths, the
  lead doubled an octave down, a snare roll into bar 19's long chord. A', 20-27: theme A doubled, with a brass call
  each bar. Outro, 28-31: a last phrase of A, back to the drone. Its bars run from -31 to -17 dBFS (RMS), the loudest
  in B.
- **The voices:** an 8-bit console's: band-limited pulses (PolyBLEP), a 16-step triangle, noise. The lead is one voice
  that glides between notes that touch, with an accent on each and a vibrato after a while; the lyre a 12.5% pulse,
  plucked, with an echo; the choir three detuned pulses a note, left, middle and right, through three vowel formants
  that move from "oo" to "ah" by section; the brass two pulses through a low-pass that opens with the note; the drums
  a falling sine with a click of noise. Over it all a hall after Jezar's Freeverb (public domain), written again here.
  Each part's level is set against the lead's while each sounds, so the mix does not depend on a voice's waveform.
- **The loop:** rendered once and 8 s past its end; what sounds past the end (echoes, the hall, releases) is added to
  its start, where the loop before it leaves them. The ambience is a loop of the same length, crossfaded over 4 s at its
  seam. Both are high-passed at 30 Hz as loops: once through to settle the filter, then again from the start.
- **Thunder:** three rolls of 5 s, played 1.2 s after each of the scene's strokes of lightning (6 s in, then every
  9.5 s), on the screen's clock.
- **Playing it:** a thread of its own makes it, 2.4 to 2.5 s here (Ryzen 5 4600H, with the build running), about
  90 MB while it is made and 36 MB of 16-bit loops and rolls after. XAudio2 2.9, Windows 10's own, loaded at run time, plays it
  on three source voices: the theme, the sea, the thunder. Not before 2.5 s on the screen, so a start that only checks
  (about a second) ends in silence; in over 2.5 s; when the screen goes (`gowa_loading_screen_close`), out over a
  second, as the game's own sound opens.
- **Setting:** `[system] precompile_music` (`GOWA_PRECOMPILE_MUSIC`): off, low (the default), medium, high, the master
  voice at 0.3, 0.55 and 0.85. In the launcher: Precompile music, in the System tab.
- **Tried:** the gate on a test folder (`logs\music-gate`: a copy of the shader cache without its mark, the game's
  folder through a junction), started with `--no-launcher`:

  | Start | What the log said |
  |---|---|
  | Full build, no programs' list (108 s) | made in 2.5 s; playing (low); faded out after 107.4 s played, 11 rolls of thunder, as the game's window opened |
  | Programs' list kept, mark taken away (1 s) | the screen went before it played |
  | `precompile_music = off` | off |

  `--music-preview` against the sketch that was approved: within 1 dB bar by bar, peak -4.6 dBFS at full level.
- **Preview:** `Ascension-R.exe --music-preview <file.wav> [seconds]` writes what the screen plays from its start
  (silence for 2.5 s, then in), at full level.
