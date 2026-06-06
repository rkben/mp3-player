#include "ShaderArt.h"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QLibraryInfo>
#include <QProcess>
#include <rhi/qrhi.h>

namespace {
QShader loadShader(const QString &path) {
  QFile f(path);
  if (f.open(QIODevice::ReadOnly))
    return QShader::fromSerialized(f.readAll());
  qWarning("ShaderArt: failed to load %s", qPrintable(path));
  return {};
}
constexpr float kVerts[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
} // namespace

ShaderArt::ShaderArt(QWidget *parent)
    : QRhiWidget(parent), m_clock(std::make_unique<QElapsedTimer>()) {
  m_clock->start();

#ifdef SHADER_DIR
  m_watcher = new QFileSystemWatcher(this);
  QString fragPath = QStringLiteral(SHADER_DIR) + "/plasma.frag";

  if (QFile::exists(fragPath)) {
    m_watcher->addPath(fragPath);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this,
            &ShaderArt::onShaderFileChanged);
    qDebug() << "Live Shader Watcher active for:" << fragPath;
  } else {
    qWarning() << "Could not find physical shader to watch at:" << fragPath;
  }
#endif
}

ShaderArt::~ShaderArt() = default;

void ShaderArt::onShaderFileChanged(const QString &path) {
  qDebug() << "\n[Watcher] Shader modified. Recompiling:" << path;

  QString qsbPath = QLibraryInfo::path(QLibraryInfo::BinariesPath) + "/qsb";
#ifdef Q_OS_WIN
  qsbPath += ".exe";
#endif

  QString outputPath = QStringLiteral(SHADER_DIR) + "/plasma.frag.qsb";

  QStringList arguments;
  arguments << "--qt6" << "-o" << outputPath << path;

  qDebug() << "[Compiler] Executing:" << qsbPath << arguments.join(" ");

  QProcess *process = new QProcess(this);
  connect(process, &QProcess::finished, this,
          [this, process, outputPath](int exitCode) {
            if (exitCode == 0) {
              qDebug() << "[Compiler] Success! New .qsb size is:"
                       << QFileInfo(outputPath).size() << "bytes";

#ifdef SHADER_DIR
              m_watcher->addPath(QStringLiteral(SHADER_DIR) + "/plasma.frag");
#endif

              m_shaderNeedsReload = true;
              update();
            } else {
              qWarning() << "[Compiler] GLSL Compile Error (Exit Code"
                         << exitCode << "):";
              qWarning().noquote() << process->readAllStandardError();
            }
            process->deleteLater();
          });

  process->start(qsbPath, arguments);
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
    });
    m_srb->create();
  }

  if (!m_pipeline) {
    QShader fragShader;
#ifdef SHADER_DIR
    QString diskPath = QStringLiteral(SHADER_DIR) + "/plasma.frag.qsb";
    if (QFile::exists(diskPath)) {
      QFileInfo info(diskPath);
      qDebug() << "[Loader] SUCCESS: Loading shader from DISK:" << diskPath;
      qDebug() << "[Loader] File modified at:" << info.lastModified().toString()
               << "Size:" << info.size() << "bytes";
      fragShader = loadShader(diskPath);
    } else {
      qWarning() << "[Loader] WARNING: Disk shader NOT found at:" << diskPath
                 << "\n         Falling back to static compiled resource!";
      fragShader = loadShader(QStringLiteral(":/shaders/plasma.frag.qsb"));
    }
#else
    qWarning()
        << "[Loader] SHADER_DIR not defined. Loading from static resources.";
    fragShader = loadShader(QStringLiteral(":/shaders/plasma.frag.qsb"));
#endif

    if (!fragShader.isValid()) {
      qWarning()
          << "[Loader] Loaded shader is invalid. Postponing pipeline creation.";
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

  const float t = m_clock->elapsed() / 1000.0f;
  float ubufData[4] = {t, m_amplitude, 0.0f, 0.0f};
  batch->updateDynamicBuffer(m_ubuf.get(), 0, 16, ubufData);

  cb->beginPass(renderTarget(), QColor(Qt::black), {1.0f, 0}, batch);
  cb->setGraphicsPipeline(m_pipeline.get());
  const QSize sz = renderTarget()->pixelSize();
  cb->setViewport({0, 0, float(sz.width()), float(sz.height())});
  cb->setShaderResources(m_srb.get());
  const QRhiCommandBuffer::VertexInput vbuf(m_vbuf.get(), 0);
  cb->setVertexInput(0, 1, &vbuf);
  cb->draw(3);
  cb->endPass();

  update();
}