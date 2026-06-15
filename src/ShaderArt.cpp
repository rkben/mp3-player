#include "ShaderArt.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <rhi/qrhi.h>

namespace {
QByteArray readFile(const QString &path) {
  QFile f(path);
  if (f.open(QIODevice::ReadOnly))
    return f.readAll();
  qWarning("ShaderArt: failed to load %s", qPrintable(path));
  return {};
}
QShader loadShader(const QString &path) {
  return QShader::fromSerialized(readFile(path));
}
// Always-available baked fallback (compiled in by qt_add_shaders).
constexpr auto kDefaultFragQsb = ":/shaders/plasma.frag.qsb";
constexpr float kVerts[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
} // namespace

ShaderArt::ShaderArt(QWidget *parent)
    : QRhiWidget(parent), m_clock(std::make_unique<QElapsedTimer>()) {
  m_clock->start();
}

ShaderArt::~ShaderArt() = default;

void ShaderArt::setFragmentShader(const QByteArray &serializedShader) {
  if (!QShader::fromSerialized(serializedShader).isValid()) {
    qWarning() << "ShaderArt::setFragmentShader: ignoring invalid shader";
    return; // keep the current shader
  }
  m_fragQsb = serializedShader;
  m_shaderNeedsReload = true; // rebuild the pipeline on the next frame
  update();
}

void ShaderArt::setShaderByName(const QString &name) {
  setFragmentShader(
      readFile(QStringLiteral(":/shaders/") + name + QStringLiteral(".frag.qsb")));
}

QStringList ShaderArt::availableShaders() {
  // Derived from the baked resources so the list never drifts from CMake.
  static const QString kSuffix = QStringLiteral(".frag.qsb");
  QStringList names;
  const QStringList qsb = QDir(QStringLiteral(":/shaders"))
                              .entryList({QLatin1Char('*') + kSuffix},
                                         QDir::Files, QDir::Name);
  names.reserve(qsb.size());
  for (const QString &f : qsb)
    names << f.chopped(kSuffix.size());
  return names;
}

void ShaderArt::initialize(QRhiCommandBuffer *) {
  QRhi *r = rhi();

  if (m_rhi != r || m_shaderNeedsReload) {
    m_pipeline.reset();
    m_rhi = r;
    m_shaderNeedsReload = false;
  }

  if (!m_ubuf) {
    m_ubuf.reset(
        r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16));
    m_ubuf->create();
  }

  if (!m_specBuf) {
    // binding=1: 64 spectrum bands as std140 vec4[16] = 256 bytes. Additive to
    // the binding=0 uniforms; shaders that don't declare it simply ignore it.
    m_specBuf.reset(
        r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 256));
    m_specBuf->create();
  }

  if (!m_vbuf) {
    m_vbuf.reset(r->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                              sizeof(kVerts)));
    m_vbuf->create();
    m_vbufUploaded = false;
  }

  if (!m_srb) {
    m_srb.reset(r->newShaderResourceBindings());
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::FragmentStage, m_ubuf.get()),
        QRhiShaderResourceBinding::uniformBuffer(
            1, QRhiShaderResourceBinding::FragmentStage, m_specBuf.get()),
    });
    m_srb->create();
  }

  if (!m_pipeline) {
    // First use (or no shader swapped in yet): fall back to the baked default.
    if (m_fragQsb.isEmpty())
      m_fragQsb = readFile(QString::fromLatin1(kDefaultFragQsb));

    const QShader fragShader = QShader::fromSerialized(m_fragQsb);
    if (!fragShader.isValid()) {
      qWarning()
          << "[Loader] No valid fragment shader. Postponing pipeline creation.";
      return;
    }

    std::unique_ptr<QRhiGraphicsPipeline> newPipeline(r->newGraphicsPipeline());
    newPipeline->setShaderStages({
        {QRhiShaderStage::Vertex,
         loadShader(QStringLiteral(":/shaders/fullscreen.vert.qsb"))},
        {QRhiShaderStage::Fragment, fragShader},
    });
    QRhiVertexInputLayout layout;
    layout.setBindings({{2 * sizeof(float)}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
    newPipeline->setVertexInputLayout(layout);
    newPipeline->setShaderResourceBindings(m_srb.get());
    newPipeline->setRenderPassDescriptor(
        renderTarget()->renderPassDescriptor());

    if (newPipeline->create()) {
      m_pipeline = std::move(newPipeline);
      qDebug() << "[Loader] Graphics pipeline successfully created!";
    } else {
      qWarning() << "[Loader] Failed to create new graphics pipeline!";
    }
  }
}

void ShaderArt::render(QRhiCommandBuffer *cb) {
  // SAFE TRICK: Since initialize() is only called by the OS on startup/resize,
  // we manually invoke it here inside the active render loop if a reload is
  // pending.
  if (m_shaderNeedsReload) {
    initialize(cb);
  }

  // Safety Guard: If the pipeline is temporarily invalid, skip rendering this
  // frame
  if (!m_pipeline) {
    return;
  }

  QRhiResourceUpdateBatch *batch = rhi()->nextResourceUpdateBatch();
  if (!m_vbufUploaded) {
    batch->uploadStaticBuffer(m_vbuf.get(), kVerts);
    m_vbufUploaded = true;
  }

  // uniform layout (std140): { float time; float amplitude; vec2 resolution; }.
  // resolution is the viewport pixel size so shaders can correct aspect ratio
  // (resolution.x/resolution.y) or do pixel-space work, instead of assuming square.
  const QSize sz = renderTarget()->pixelSize();
  const float t = m_clock->elapsed() / 1000.0f;

  // Ease the displayed loudness toward its target each frame (snap up, ease
  // down) — same display-paced smoothing as the spectrum below.
  if (m_amplitudeTarget > m_amplitude)
    m_amplitude = m_amplitudeTarget;
  else
    m_amplitude += (m_amplitudeTarget - m_amplitude) * 0.22f;

  float ubufData[4] = {t, m_amplitude, float(sz.width()), float(sz.height())};
  batch->updateDynamicBuffer(m_ubuf.get(), 0, 16, ubufData);

  // Ease the displayed spectrum toward the latest target once per painted frame:
  // snap up on transients (punchy) but ease back down, decoupling the bars'
  // motion from the bursty audio-callback cadence. Spectrum at binding=1: 64
  // contiguous floats map 1:1 onto std140 vec4[16] (each vec4 holds 4
  // tightly-packed floats), so a flat upload is correct.
  for (size_t i = 0; i < m_spectrum.size(); ++i) {
    const float target = m_spectrumTarget[i];
    if (target > m_spectrum[i])
      m_spectrum[i] = target;                              // snap up
    else
      m_spectrum[i] += (target - m_spectrum[i]) * 0.22f;   // ease down
  }
  batch->updateDynamicBuffer(m_specBuf.get(), 0, 256, m_spectrum.data());

  cb->beginPass(renderTarget(), QColor(Qt::black), {1.0f, 0}, batch);
  cb->setGraphicsPipeline(m_pipeline.get());
  cb->setViewport({0, 0, float(sz.width()), float(sz.height())});
  cb->setShaderResources(m_srb.get());
  const QRhiCommandBuffer::VertexInput vbuf(m_vbuf.get(), 0);
  cb->setVertexInput(0, 1, &vbuf);
  cb->draw(3);
  cb->endPass();

  update();
}