# What Qt Multimedia does in remora_mirror

A source reading of the current tree, written to answer one question: what the mirror client uses
Qt Multimedia for. It does not choose a Qt minimum, and nothing here was run against a real audio
device or a Qt Multimedia backend.

## Build declarations

- **Configure.** `CMakeLists.txt` asks for `find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets
  Test WebEngineWidgets Network DBus OpenGLWidgets Multimedia)`. `Multimedia` is in the REQUIRED
  list, so a configure without the Qt 6 Multimedia package fails before any target is built. The
  declared minimum is 6.4; which Qt versions actually build and pass is not established here.
- **Linkage.** Only one target names the module: `remora_mirror` links `Qt6::Multimedia`
  (`PUBLIC`, beside `Qt6::Widgets`, `Qt6::Network`, `Qt6::OpenGLWidgets` and `PkgConfig::FFMPEG`).
  The `remora` executable links `remora_mirror`, so the module reaches the shipped binary that way.
- **Tests.** No test target links `remora_mirror` or `Qt6::Multimedia`. `src/mirror/AudioPlayer.cpp`
  is compiled when the application builds, but no registered ctest suite executes it.

## Where the module is used

Searching `src/`, `tests/` and `agent/` for Qt Multimedia headers and classes finds them in one
place: `src/mirror/AudioPlayer.h` (`QAudioSink`) and `src/mirror/AudioPlayer.cpp` (`QAudioFormat`,
`QMediaDevices`, `QAudioDevice`). Its one owner is `MirrorSession` (`src/mirror/MirrorSession.h`,
member `audioPlayer_`). Audio is opt-in: `SessionOptions::audio` defaults to `false`.

## Decoding is FFmpeg, not Qt

Audio arrives as `MediaPacket`s on the agent's audio connection. The codec comes from the stream's
start (`kCodecOpus`, `kCodecAac`, `kCodecFlac` or `kCodecRawAudio`, `src/core/MirrorProto.h`).

`AudioPlayer::submit`:

- **Raw** (`kCodecRawAudio`, s16le 48 kHz stereo per `MirrorProto.h`): the payload is passed to
  `play()` as is. This branch is taken before the config check and involves no FFmpeg call.
- **Opus / AAC / FLAC:** a config packet's payload is stored as extradata. On the first media
  packet `openCodec()` calls `avcodec_find_decoder`, `avcodec_alloc_context3` and `avcodec_open2`.
  Each packet then goes through `av_new_packet` and `avcodec_send_packet`, and every frame from
  `avcodec_receive_frame` is converted by libswresample (`swr_alloc_set_opts2`, `swr_init` once,
  then `swr_convert`) to s16 interleaved stereo at 48 kHz before it reaches `play()`.

Qt Multimedia never sees compressed audio: it only receives the PCM bytes `flush()` writes.

## What Qt Multimedia is asked to do

`AudioPlayer::init`:

1. Builds a `QAudioFormat`: 48000 Hz, 2 channels, `QAudioFormat::Int16`,
   `QAudioFormat::ChannelConfigStereo`.
2. Takes `QMediaDevices::defaultAudioOutput()`; it fails with "no audio output device" if the
   device `isNull()`, and with "output device rejects 48 kHz s16 stereo" if `isFormatSupported`
   returns false.
3. Creates a `QAudioSink` for that device and format and calls `setBufferSize` with
   `REMORA_AUDIO_BUFFER_MS` worth of bytes (accepted range 20 to 1000, otherwise 60).
4. Calls `start()` with no argument and keeps the returned `QIODevice *` as `io_`. A null return
   deletes the sink and fails with "audio sink failed to start".

`AudioPlayer::play` appends the bytes to `pending_` and calls `flush()`. `AudioPlayer::flush`,
when `io_` is set:

- If `pending_` exceeds `kMaxBacklogMs` (80 ms) of audio, removes the excess, rounded down to a
  multiple of 4 bytes, from the front, and adds it to `droppedBytes()`.
- Computes the offer as `pending_`'s size rounded down to a multiple of 4 bytes (one s16 stereo
  frame) and, if it is non-zero, calls `io_->write(pending_.constData(), offer)`.
- The sink may accept fewer bytes than offered, including a count that is not a multiple of 4.
  When `write` returns a positive count, exactly that many bytes are removed from the front of
  `pending_` and added to `playedBytes()`; the unaccepted suffix stays held. A zero or negative
  return removes nothing. The offer length is frame-aligned; what the sink accepts need not be.
- Starts the 5 ms `drain_` timer while anything is held and stops it when `pending_` is empty; the
  timer calls `flush()` again.

`AudioPlayer::stop` calls `QAudioSink::stop()`, deletes the sink, clears `io_` and `pending_`,
and frees the FFmpeg decoder, frame, packet and resampler. `MirrorSession::stop` logs the played
and dropped byte counts when either is non-zero, then calls it; the destructor calls it too.

## When audio stops

Two different flags are involved, and some failures set neither.

- **Connection.** In `MirrorSession::agentAudioThenDone`, if the audio connection cannot be
  opened, a warning is logged and the session starts without it. `audio_` stays null, so
  `onAudioData` reads nothing; `audioDead_` is not set.
- **Session flag `audioDead_`** is set by `MirrorSession::drainAudioStream` when
  `AudioPlayer::init` returns false on the stream's start, and on the demuxer's `Unavailable`,
  `Failed` or `Error` events (which also tell the recorder audio is unavailable, when recording).
  Once set, `onAudioData` stops reading the audio socket.
- **Player flag `failed_`** is set inside `AudioPlayer::submit` when `openCodec()` fails or the
  resampler cannot be initialised. The session is not told: it keeps demuxing, still hands packets
  to the recorder, and `submit` returns at once from then on.
- **Neither.** A failed `av_new_packet` or `avcodec_send_packet` drops that packet only, and a
  `swr_convert` result of zero or less plays nothing for that frame.

None of these branches ends the session (none calls `fail()` or `finishStart(false, …)`); comments
in `agentAudioThenDone`, `drainAudioStream` and `submit` say video is meant to survive audio failing.

## Not established by this reading

Which Qt Multimedia backend or plugin is selected at run time, whether `setBufferSize` is honoured,
whether audio plays correctly, how the sink handles a write that ends mid-frame, and whether the
audio path depends on a particular backend. The oldest Qt version that CI builds is not recorded
here either. All of these need a run on real hardware or a CI log, not source.
