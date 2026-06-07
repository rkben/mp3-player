#pragma once

#include <QObject>
#include <QStringList>
#include <QMutex>
#include <QFile>

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

    // Path to the on-disk log file (<AppData>/logs/pocketplayer.log). Lines are
    // tee'd here so logs survive a restart; the "Open log file" button uses this.
    static QString logFilePath();

signals:
    // One formatted line was appended (delivered to the GUI thread by the dialog's
    // queued connection, since this can fire from a worker thread).
    void lineAppended(const QString &line);

private:
    explicit Logger(QObject *parent = nullptr) : QObject(parent) {}
    void append(const QString &line);
    void writeToFile(const QString &line);   // tee to disk; rotates at the size cap
    static void handler(QtMsgType type, const QMessageLogContext &ctx,
                        const QString &msg);

    mutable QMutex m_mutex;
    QStringList m_lines;
    QFile m_file;                  // on-disk tee; opened lazily on first write
    bool m_fileTried = false;      // attempted to open (don't retry every line)
    qint64 m_fileBytes = 0;        // running size, to rotate without statting
    static constexpr int kMaxLines = 2000;          // ring cap; oldest lines drop
    static constexpr qint64 kMaxFileBytes = 1 << 20; // ~1 MB, then rotate to .1
};
