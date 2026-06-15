#pragma once

#include <QCloseEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <utility>

// Top-level window that hosts the visualizer widget when it's "popped out" of the
// now-playing panel. Double-click or F11 toggles fullscreen; Esc leaves fullscreen
// (or closes the window). It never owns the hosted widget — on close it invokes the
// supplied callback so the host can re-dock the widget back into its panel.
// Header-only with no signals/slots (callback instead of a signal), so no moc.
class VisualizerWindow : public QWidget
{
public:
    VisualizerWindow(QWidget *content, std::function<void()> onClose,
                     QWidget *parent = nullptr)
        : QWidget(parent, Qt::Window), m_onClose(std::move(onClose))
    {
        setWindowTitle(tr("Visualizer"));
        auto *l = new QVBoxLayout(this);
        l->setContentsMargins(0, 0, 0, 0);
        l->addWidget(content);
        resize(960, 600);
        // The content (a QRhiWidget) fills the window and grabs mouse/key events,
        // which don't bubble to the parent — filter them so double-click/F11/Esc
        // work no matter where focus sits.
        content->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *, QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::MouseButtonDblClick:
            toggleFullScreen();
            return true;
        case QEvent::KeyPress:
            return handleKey(static_cast<QKeyEvent *>(e));
        default:
            return false;
        }
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        if (!handleKey(e))
            QWidget::keyPressEvent(e);
    }

    void mouseDoubleClickEvent(QMouseEvent *) override { toggleFullScreen(); }

    void closeEvent(QCloseEvent *e) override
    {
        if (m_onClose)
            m_onClose();   // host re-parents the content back into its panel
        QWidget::closeEvent(e);
    }

private:
    void toggleFullScreen() { isFullScreen() ? showNormal() : showFullScreen(); }

    // Returns true if the key was handled (F11 fullscreen, Esc exit/close).
    bool handleKey(QKeyEvent *e)
    {
        if (e->key() == Qt::Key_F11) {
            toggleFullScreen();
            return true;
        }
        if (e->key() == Qt::Key_Escape) {
            if (isFullScreen())
                showNormal();
            else
                close();
            return true;
        }
        return false;
    }

    std::function<void()> m_onClose;
};
