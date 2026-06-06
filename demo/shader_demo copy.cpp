// Widgets-native shader demo: a QRhiWidget (Qt 6.7+) running a fullscreen fragment
// shader, animated by a uniform. This is the lean path for "shader shenanigans"
// behind the album art — no QML/Qt Quick, backend-agnostic (Vulkan/Metal/D3D/GL)
// via Qt's RHI. Shaders are pre-compiled to .qsb at build time (qt_add_shaders).
//
// Build with: cmake -B build -DBUILD_SHADER_DEMO=ON && cmake --build build

#include <QApplication>
#include <QRhiWidget>
#include <QElapsedTimer>
#include <QFile>

#include <rhi/qrhi.h>

#include <memory>

namespace {
QShader loadShader(const QString &path)
{
    QFile f(path);
    if (f.open(QIODevice::ReadOnly))
        return QShader::fromSerialized(f.readAll());
    qWarning("Failed to load shader %s", qPrintable(path));
    return {};
}
}

class PlasmaWidget : public QRhiWidget
{
public:
    explicit PlasmaWidget(QWidget *parent = nullptr) : QRhiWidget(parent)
    {
        m_clock.start();
    }

protected:
    // Called whenever the QRhi or the render target is (re)created.
    void initialize(QRhiCommandBuffer *) override
    {
        QRhi *r = rhi();
        if (m_rhi != r) {   // device lost / changed — rebuild everything
            m_pipeline.reset();
            m_rhi = r;
        }
        if (m_pipeline)
            return;

        m_ubuf.reset(r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                  sizeof(float)));
        m_ubuf->create();

        // Fullscreen triangle vertices (clip space); uploaded on the first render.
        m_vbuf.reset(r->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                  sizeof(kVerts)));
        m_vbuf->create();
        m_vbufUploaded = false;

        m_srb.reset(r->newShaderResourceBindings());
        m_srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::FragmentStage, m_ubuf.get()),
        });
        m_srb->create();

        m_pipeline.reset(r->newGraphicsPipeline());
        m_pipeline->setShaderStages({
            { QRhiShaderStage::Vertex,   loadShader(QStringLiteral(":/shaders/fullscreen.vert.qsb")) },
            { QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/shaders/plasma.frag.qsb")) },
        });
        QRhiVertexInputLayout layout;
        layout.setBindings({ { 2 * sizeof(float) } });
        layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float2, 0 } });
        m_pipeline->setVertexInputLayout(layout);
        m_pipeline->setShaderResourceBindings(m_srb.get());
        m_pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        m_pipeline->create();
    }

    void render(QRhiCommandBuffer *cb) override
    {
        QRhiResourceUpdateBatch *batch = rhi()->nextResourceUpdateBatch();
        if (!m_vbufUploaded) {
            batch->uploadStaticBuffer(m_vbuf.get(), kVerts);
            m_vbufUploaded = true;
        }
        const float t = m_clock.elapsed() / 1000.0f;
        batch->updateDynamicBuffer(m_ubuf.get(), 0, sizeof(float), &t);

        cb->beginPass(renderTarget(), QColor(Qt::black), { 1.0f, 0 }, batch);
        cb->setGraphicsPipeline(m_pipeline.get());
        const QSize sz = renderTarget()->pixelSize();
        cb->setViewport({ 0, 0, float(sz.width()), float(sz.height()) });
        cb->setShaderResources(m_srb.get());
        const QRhiCommandBuffer::VertexInput vbuf(m_vbuf.get(), 0);
        cb->setVertexInput(0, 1, &vbuf);
        cb->draw(3);   // fullscreen triangle
        cb->endPass();

        update();   // keep animating
    }

private:
    // Oversized triangle covering the viewport: (-1,-1) (3,-1) (-1,3).
    static constexpr float kVerts[6] = { -1.0f, -1.0f,  3.0f, -1.0f,  -1.0f, 3.0f };

    QRhi *m_rhi = nullptr;
    bool m_vbufUploaded = false;
    std::unique_ptr<QRhiBuffer> m_vbuf;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
    QElapsedTimer m_clock;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    PlasmaWidget w;
    w.resize(480, 480);
    w.setWindowTitle(QStringLiteral("QRhiWidget shader demo"));
    w.show();
    return app.exec();
}
