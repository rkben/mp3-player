#pragma once

#include <QObject>
#include <QStringList>
#include <QMutex>

// Process-wide in-memory log. Installs a Qt message handler (via install()) that
// captures qDebug/qInfo/qWarning/qCritical from every thread into a bounded ring
// of formatted lines, while still chaining to the previous handler so console
// output is unchanged. The Settings "Log" tab reads text() and live-appends on the
// lineAppended() signal. Thread-safe: the handler fires from the GUI thread and the
// MusicLibrary / MediaEngine worker threads.
class Logger : public QObject
{
    Q_OBJECT
public:
    static Logger *instance();
    // Install the Qt message handler. Call once, early in main().
    static void install();

    // Whole log as one newline-joined block (for first paint of the Log tab).
    QString text() const;
    void clear();

signals:
    // One formatted line was appended (delivered to the GUI thread by the dialog's
    // queued connection, since this can fire from a worker thread).
    void lineAppended(const QString &line);

private:
    explicit Logger(QObject *parent = nullptr) : QObject(parent) {}
    void append(const QString &line);
    static void handler(QtMsgType type, const QMessageLogContext &ctx,
                        const QString &msg);

    mutable QMutex m_mutex;
    QStringList m_lines;
    static constexpr int kMaxLines = 2000;   // ring cap; oldest lines drop
};
