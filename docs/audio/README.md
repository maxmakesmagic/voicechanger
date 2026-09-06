# Audio example provenance

The four WAV files in this directory use the same 17.84-second, mono voice
recording as their source. They are lossless 16-bit PCM at 44.1 kHz so codec
artifacts do not obscure the DSP comparison.

## Source recording

- Recording: [Mark Cousins introducing himself][source-page]
- Speaker: Mark Cousins
- Recorder/uploader: Vera de Kok, Wikimedia Commons user `1Veertje`
- Recorded: 1 February 2017
- License: [Creative Commons CC0 1.0 Universal][cc0]
- Upstream format: 48 kHz, mono, 16-bit FLAC
- Upstream SHA-256:
  `07d1ef5e49c7cf6c96d6428e4e7c449c998080c8cf5f43811f13378e55e075cf`

The copyright holder dedicated the recording to the public domain worldwide.
Attribution is retained here for provenance even though CC0 does not require
it.

The checked-in `source.wav` was resampled and given 250 ms of silence at each
end with FFmpeg 6.1.1:

```sh
curl -fL \
  -o /tmp/voicechanger-upstream.flac \
  'https://commons.wikimedia.org/wiki/Special:Redirect/file/Mark_Cousins_-_voice_en_20170201.flac'

ffmpeg -y -i /tmp/voicechanger-upstream.flac \
  -af 'adelay=250,apad=pad_dur=0.25' \
  -ar 44100 -ac 1 -c:a pcm_s16le \
  -map_metadata -1 -fflags +bitexact -flags:a +bitexact \
  docs/audio/source.wav
```

Its SHA-256 is
`e63743eb6440e3aa73f54b918e45924852722197626436376ec1e6182343d031`.

## Processed examples

`tools/render-audio-examples.sh` regenerates every processed file using the
production effect classes and their public `AudioStream::update()` methods. No
post-effect gain adjustment or loudness normalization is applied:

| File | Processing | Channels | SHA-256 |
|---|---|---:|---|
| `pitch-up-1.25.wav` | Pitch ratio 1.25 | 1 | `d727bd50b0e2a5677e0a26152dfbadb29e974ada9ab91bee18aad0116a58b516` |
| `pitch-down-0.80.wav` | Pitch ratio 0.80 | 1 | `cb13643738d6aa6a6d78bab7b9c149d30a43e079dbbed4477b7ccb182c755081` |
| `chorus.wav` | 18 ms delay, 8 ms depth, 0.8 Hz, 60% wet | 2 | `503f4346710ef1925ea4f46b1cde04e4c64842c78c5da50f164114c0d28c3640` |

The host renderer links the production DSP sources to the tested portable
CMSIS implementations in `tests/support/`. Cortex-M7 floating-point rounding
can differ from these renders by a few least-significant bits.

Build and regenerate them with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target audio_example_renderer --parallel
tools/render-audio-examples.sh build/audio_example_renderer
```

CI regenerates the examples and checks their format, duration, channels, and
PCM samples against the committed files. A substantive DSP change therefore
requires intentionally rerendering these artifacts.

[source-page]: https://commons.wikimedia.org/wiki/File:Mark_Cousins_-_voice_en_20170201.ogg
[cc0]: https://creativecommons.org/publicdomain/zero/1.0/
