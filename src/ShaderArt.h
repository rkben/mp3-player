#pragma once

#include <QFileSystemWatcher>
#include <QRhiWidget>
#include <memory>

class QElapsedTimer;
class QRhiBuffer;
class QRhiShaderResourceBindings;
class QRhiGraphicsPipeline;

class ShaderArt : public QRhiWidget {
  Q_OBJECT
public:
  explicit ShaderArt(QWidget *parent = nullptr);
  ~ShaderArt() override;

  void setAmplitude(float amp) { m_amplitude = amp; }

protected:
  void initialize(QRhiCommandBuffer *cb) override;
  void render(QRhiCommandBuffer *cb) override;

private slots:
  void onShaderFileChanged(const QString &path);

private:
  std::unique_ptr<QElapsedTimer> m_clock;
  QRhi *m_rhi = nullptr;
  std::unique_ptr<QRhiBuffer> m_ubuf;
  std::unique_ptr<QRhiBuffer> m_vbuf;
  bool m_vbufUploaded = false;
  std::unique_ptr<QRhiShaderResourceBindings> m_srb;
  std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
  float m_amplitude = 0.0f;

  QFileSystemWatcher *m_watcher = nullptr;
  bool m_shaderNeedsReload = false; // Add this reload flag
};