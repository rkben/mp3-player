#pragma once

#include <QByteArray>
#include <QList>
#include <QRhiWidget>
#include <QStringList>
#include <algorithm>
#include <array>
#include <memory>

class QElapsedTimer;
class QRhiBuffer;
class QRhiShaderResourceBindings;
class QRhiGraphicsPipeline;

// Fullscreen-triangle fragment-shader visualizer (QRhiWidget). Renders a baked
// shader from :/shaders/ and animates on elapsed time + an audio amplitude fed
// in via setAmplitude(). The fragment shader can be swapped at runtime with
// setFragmentShader() — the demo's ShaderReloader uses this for live editing,
// the main app to pick a preset. Live-reload itself lives outside this widget so
// the shipping app carries no file-watching/compiling.
//
// The shader is passed as the serialized bytes of a .qsb (QShader::serialized()
// form) rather than a QShader, deliberately: QShader is semi-private RHI API, so
// keeping it out of this header lets the main app include ShaderArt.h without
// pulling in Qt's private RHI headers.
class ShaderArt : public QRhiWidget {
  Q_OBJECT
public:
  explicit ShaderArt(QWidget *parent = nullptr);
  ~ShaderArt() override;

  // Sets the *target* loudness; render() eases the displayed value toward it
  // each painted frame (snap up, ease down) — same display-paced smoothing as
  // the spectrum, so it's smooth regardless of the audio-callback cadence.
  void setAmplitude(float amp) { m_amplitudeTarget = amp; }

  // 64 log-spaced [0..1] frequency bands, exposed to shaders at binding=1 as
  // `vec4 bands[16]`. This sets the *target*; render() eases the displayed bands
  // toward it each painted frame, so visual smoothing is paced by the display
  // refresh rather than the (bursty, codec-dependent) audio-callback cadence.
  // Extra/short lists are clamped; absent bands target 0.
  void setSpectrum(const QList<float> &bands) {
    const int n = std::min<int>(bands.size(), int(m_spectrumTarget.size()));
    for (int i = 0; i < n; ++i)
      m_spectrumTarget[i] = bands[i];
    for (int i = n; i < int(m_spectrumTarget.size()); ++i)
      m_spectrumTarget[i] = 0.0f;
  }

  // Swap the fragment shader and rebuild the pipeline on the next frame. Takes
  // the serialized contents of a baked .qsb file. Safe to call from the GUI
  // thread at any time; empty/invalid bytes are ignored (last shader stays).
  void setFragmentShader(const QByteArray &serializedShader);

  // Load a baked preset by name: :/shaders/<name>.frag.qsb.
  void setShaderByName(const QString &name);

  // Preset names available to pick, derived from the baked :/shaders/*.frag.qsb
  // resources at runtime (sorted). Used to populate the settings dropdown.
  static QStringList availableShaders();

protected:
  void initialize(QRhiCommandBuffer *cb) override;
  void render(QRhiCommandBuffer *cb) override;

private:
  std::unique_ptr<QElapsedTimer> m_clock;
  QRhi *m_rhi = nullptr;
  std::unique_ptr<QRhiBuffer> m_ubuf;
  std::unique_ptr<QRhiBuffer> m_specBuf;   // binding=1: 64 spectrum bands (256B)
  std::unique_ptr<QRhiBuffer> m_vbuf;
  bool m_vbufUploaded = false;
  std::unique_ptr<QRhiShaderResourceBindings> m_srb;
  std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
  float m_amplitudeTarget = 0.0f;             // latest loudness from the engine
  float m_amplitude = 0.0f;                   // displayed loudness (eased each frame)
  std::array<float, 64> m_spectrumTarget{};   // latest bands from the analyzer
  std::array<float, 64> m_spectrum{};         // displayed bands (eased each frame)

  QByteArray m_fragQsb;             // serialized fragment shader (baked or swapped-in)
  bool m_shaderNeedsReload = false; // pipeline rebuild pending
};