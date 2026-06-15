# Visualizer / shader notes

The visualizer is a fullscreen-triangle fragment shader (`ShaderArt`, a
`QRhiWidget`). Baked `.qsb` presets live in `demo/*.frag` and are compiled in by
`qt_add_shaders` (see `CMakeLists.txt`); `ShaderArt::availableShaders()`
enumerates them at runtime for the Settings dropdown. Drop a new `demo/*.frag`
in and it ships automatically (except `live`/`plasma.original`, which are filtered
out).

## Shader uniform contract

Two uniform blocks are provided to every shader. Both are optional to declare —
a shader uses only what it needs.

```glsl
// binding=0: always present, identical across all shaders. Do not change layout.
layout(std140, binding = 0) uniform buf {
    float time;       // animation clock (seconds)
    float amplitude;  // overall audio loudness 0..1 (smoothed RMS)
    vec2  resolution; // viewport pixel size (x=width, y=height)
};

// binding=1: 64 log-spaced frequency bands in [0..1], newest frame.
// Packed as vec4[16] in std140 (NOT float[64] — std140 would pad each element
// to 16 bytes).
layout(std140, binding = 1) uniform spec {
    vec4 bands[16];
};
```

**Portability:** shaders are baked to multiple targets including **GLSL ES 1.00**
(the OpenGL backend), which forbids integer bitwise ops (`>>`, `&`), dynamic
uniform-array indexing, and implicit int/float casts. So don't unpack a band with
`bands[i>>2][i&3]` — index `bands[]` only with **loop induction variables**
(constant-index expressions) and cast explicitly. See `spectrum_bars.frag` for
the portable pattern (a `for (g..16){ for (c..4){ if (g*4+c==i) ... } }` lookup).

`spectrum_bars.frag` is the reference shader for the spectrum input.

### Data flow
`MediaEngine::onAudioBuffer` (worker thread) downmixes the decoded audio tap to
mono, feeds `SpectrumAnalyzer` (FFT → log bands, *instantaneous* magnitudes), and
emits `spectrumChanged(QList<float>)`. `PlayerController` relays it to the GUI
thread; `MainWindow` connects it to `ShaderArt::setSpectrum`, which stores it as a
*target*. `ShaderArt::render()` eases the displayed bands toward that target once
per painted frame (snap up, ease down) and uploads them into the `binding=1` UBO.

**Two things keep motion smooth and codec-independent:**

1. **Fixed emit cadence (`SpectrumAnalyzer::kHop`).** The analyzer keeps a sliding
   window and emits a new spectrum every `kHop` *samples*, not once per decode
   block. Decoders hand over wildly different block sizes — MP3 ~1152 samples,
   FLAC ~4096 — so emitting per block makes MP3 update ~4× more often and look
   jumpier/more aggressive than FLAC. A fixed sample hop makes both update at the
   same rate. `MediaEngine` gates the `amplitude` emit on the same tick so
   loudness and spectrum stay in lockstep.
2. **Render-paced smoothing.** `setAmplitude`/`setSpectrum` only set *targets*;
   `render()` eases the displayed values toward them each painted frame (snap up,
   ease down, `0.22`). This interpolates the sparse analyzer updates up to the
   display refresh, so bars glide instead of stepping. The audio tap (`QAudioBufferOutput`) is only attached
while the visualizer is visible (`setVisualizerActive`), so the FFT never runs on
the idle/album-art path.

## Equalizer — deliberately not implemented

An EQ was scoped and intentionally deferred. The audio tap feeding the
visualizer (`QAudioBufferOutput`) is **read-only** — it delivers a *copy* of the
already-decoded audio downstream of playback, so there is no way to alter what the
user hears from there. A real equalizer would require rebuilding the playback
path (decode → DSP/biquad filters → a custom `QAudioSink`), replacing the
`QMediaPlayer` + `QAudioOutput` pipeline in `MediaEngine`. That is a much larger,
separate change and is out of scope for the spectrum work.
