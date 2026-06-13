#pragma once

#include <QByteArray>
#include <QRhiWidget>
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

  void setAmplitude(float amp) { m_amplitude = amp; }

  // Swap the fragment shader and rebuild the pipeline on the next frame. Takes
  // the serialized contents of a baked .qsb file. Safe to call from the GUI
  // thread at any time; empty/invalid bytes are ignored (last shader stays).
  void setFragmentShader(const QByteArray &serializedShader);

protected:
  void initialize(QRhiCommandBuffer *cb) override;
  void render(QRhiCommandBuffer *cb) override;

private:
  std::unique_ptr<QElapsedTimer> m_clock;
  QRhi *m_rhi = nullptr;
  std::unique_ptr<QRhiBuffer> m_ubuf;
  std::unique_ptr<QRhiBuffer> m_vbuf;
  bool m_vbufUploaded = false;
  std::unique_ptr<QRhiShaderResourceBindings> m_srb;
  std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
  float m_amplitude = 0.0f;

  QByteArray m_fragQsb;             // serialized fragment shader (baked or swapped-in)
  bool m_shaderNeedsReload = false; // pipeline rebuild pending
};