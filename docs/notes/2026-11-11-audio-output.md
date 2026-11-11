# Sound output: the effects, 7.1, a change of device (2026-11-11)

The questions: what WASAPI and the APOs are, whether the runtime works with them, and four changes asked for:
- A: follow a change of output device;
- B: 7.1 and 5.1 output, on this stereo laptop too;
- C: the stream's category;
- D: a way past the effects.

## WASAPI and the APOs

- **WASAPI** is Windows' audio API. In shared mode a stream goes to the audio engine, which mixes it with the other
  programs' streams, converts it to the device's format and runs the effects. In exclusive mode a program has the
  device alone, without the effects.
- **APOs** (Audio Processing Objects) are those effects: plug-ins of the audio engine, per stream (SFX), per mode (MFX)
  or per device (EFX). Examples are Loudness Equalization, virtual surround, Windows Sonic, Dolby Atmos for Headphones,
  the Realtek, Nahimic or Dolby suites, and Equalizer APO.
  - A program in shared mode gets them without doing anything.
  - It can only name its stream's category, which some suites use for a profile, or ask for raw, past them.

## Before

- **The backend** (`libs/audio/cellAudio.c`): WASAPI, shared, event-driven, 48 kHz float stereo, a 60 ms buffer, the
  engine converting to the device's format. So the APOs applied to it like any stream.
- **The title's sound** is one 8-channel port (`PortOpen(nChannel=8, nBlock=8)`). The mixer folded it to stereo.
- **A lost device** (`AUDCLNT_E_DEVICE_INVALIDATED`: unplugged, disabled, or a new default) left the mixing thread
  pacing on the clock with no device. The sound stopped until the next start.
- **cellAudioOut** said 5.1 at most to `GetSoundAvailability`, and stereo to `GetState` and `GetConfiguration`.

## The PS3's channel order

| channel | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| a PS3 8-channel port | L | R | C | LFE | surround L | surround R | rear L | rear R |
| Windows 7.1 (`KSAUDIO_SPEAKER_7POINT1_SURROUND`) | FL | FR | FC | LFE | back L | back R | side L | side R |

- The PS3's is the SDK's order, from the documentation [RPCS3 #11066](https://github.com/rpcs3/rpcs3/issues/11066)
  quotes (merged there in 2021). Windows' is the order of the speaker bits.
- So 7.1 swaps the last two pairs: the rears go to the back speakers, the surrounds to the sides.
- The old stereo fold called channels 4 and 5 the rears and 6 and 7 the sides. Its weights stay as they were, 0.5 and
  0.3; only the names in the code are fixed.

## What changed

**B, the layouts** (`libs/audio/audio_mix.h`, `PS3_AUDIO_CHANNELS`, the launcher's Speakers):

| a port of | to 7.1 | to 5.1 | to stereo |
|---|---|---|---|
| 8 channels | as it is, the last two pairs swapped | the rears into the surrounds at -3 dB | as before, to the bit |
| 2 channels | the front pair | the front pair | as it is |
| 1 channel | both front channels | both front channels | both |

- **Auto** takes the layout from the device's mix format: 8 channels or more 7.1, 6 or 7 5.1, else stereo.
- **A multichannel stream** is `WAVEFORMATEXTENSIBLE` with its speakers. 5.1 takes the device's own pair, back or side.
  A layout the device lacks is folded down by Windows' conversion, `AUTOCONVERTPCM`, which the stream already asked
  for. A multichannel stream the device refuses falls back to stereo.
- **What the title is told** follows the output:
  - `GetSoundAvailability` says 7.1 when the output is 7.1, else 5.1 as before;
  - `GetState` and `GetConfiguration` say the output's channels;
  - `Configure` logs what the title asks for.

**A, a change of device.**
- A notification client (`OnDefaultDeviceChanged`, the console role) or a lost device's errors
  (`AUDCLNT_E_DEVICE_INVALIDATED`, `AUDCLNT_E_SERVICE_NOT_RUNNING`) set a flag.
- The mixing thread then closes the stream and opens the default device's, between two blocks, and its pacing starts
  again. With no device it tries every second, which also covers a start with none.
- `PS3_AUDIO_REOPEN=0` keeps the old way. `PS3_AUDIO_REOPEN_AT=<s>` opens the device again at that second, as a change
  would, for tests.

**C, the category.** `IAudioClient2::SetClientProperties` with `AudioCategory_GameEffects`, the game's whole mix.
`GameMedia` is a game's background music, which Windows may lower or replace. `PS3_AUDIO_CATEGORY=0` sets none.

**D, raw.** `AUDCLNT_STREAMOPTIONS_RAW` when the device says it has a raw path
(`PKEY_AudioDevice_RawProcessingSupported`). Otherwise the log says the effects stay. `PS3_AUDIO_RAW=1`, or the
launcher's Bypass sound effects.

**The log** has a line each time a device is opened:

    [cellAudio] WASAPI opened: "Speakers (Realtek(R) Audio)", 2 channels (speakers 0x3); the stream 7.1, 48 kHz float,
    2880 frames; category game; raw, past the effects

- `AUDIO_RATE` adds `[audio-chan]`: the level of each channel of the title's 8-channel ports, and of the output.
- `AUDIO_WAV` writes the output's channels; the log gives how many.

## Checked

- **`test_audio_mix`** checks the 7.1 order, the 5.1 fold, and the front pair for stereo and mono ports.
  - Stereo is the same to the bit as before, over 4,096 random frames.
  - That holds built with AVX2, FMA and `-ffp-contract=on` too.
- **Two runs of the Megaera fight** on this laptop, Realtek speakers, stereo, with `AUDIO_RATE`:

  | run | Speakers, effects | the title asked for | the stream | the title's port, rms |
  |---|---|---|---|---|
  | A | 7.1, raw | 8 channels | 7.1, raw taken; opened again at 80 s | L 0.28, R 0.29, the other six 0 |
  | B | auto | 6 channels, as before | stereo | L 0.30, R 0.30, the other six 0 |

  - In both: 187.5 blocks a second, no gap, no underrun, the title's ring three blocks ahead or more.
  - A's reopen went through between two blocks, and the counters stayed the same after it.
  - The Realtek driver has a raw path, so raw was taken.
- **What the title fills is its own choice.** With its Sound Mode on Stereo, as the test saves have it, the title
  wrote two channels into its eight-channel port, whatever it was told. Its Audio menu has Stereo, Headphones, Dolby
  Pro Logic II, 5.1 Surround and 7.1 Surround.
- **A third run** was to set the Sound Mode to 7.1 Surround through the menus by `PAD_SCRIPT`. The presses, made blind,
  went to a load and then to the main menu, so it was not set. It is a setting for the player.

## Not checked

- **A real change of device:** headphones plugged in, or another default device. The reopen ran by
  `PS3_AUDIO_REOPEN_AT`.
- **The title's surround in the stream.** It needs its Sound Mode on 5.1 or 7.1 Surround. The mapping it goes through
  is `test_audio_mix`'s.
- **Real 5.1 or 7.1 speakers**, or Windows Sonic. This laptop is stereo, and Windows folded the 7.1 stream down.
