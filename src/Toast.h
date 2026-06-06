#pragma once

#include <QWidget>

class QVBoxLayout;

// Lightweight in-app toast overlay. One instance is created over the main window;
// anything in the app can post a transient message via the static post() — it's
// marshalled to the GUI thread, so worker threads (scan, import) can call it too.
// Toasts stack at the bottom-centre and fade out after a few seconds.
class ToastArea : public QWidget
{
    Q_OBJECT
public:
    explicit ToastArea(QWidget *parent);
    ~ToastArea() override;

    // Post a toast from anywhere (thread-safe). No-op if no ToastArea exists.
    static void post(const QString &text);

private slots:
    void addToast(const QString &text);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void reposition();

    static ToastArea *s_instance;
    QVBoxLayout *m_layout;
};
