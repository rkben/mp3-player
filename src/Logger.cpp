#include "Logger.h"

#include <QDateTime>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>

namespace {
QtMessageHandler g_previous = nullptr;   // chained so console output survives

QString levelTag(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return QStringLiteral("DEBUG");
    case QtInfoMsg:     return QStringLiteral("INFO ");
    case QtWarningMsg:  return QStringLiteral("WARN ");
    case QtCriticalMsg: return QStringLiteral("ERROR");
    case QtFatalMsg:    return QStringLiteral("FATAL");
    }
    return QStringLiteral("?????");
}
}

Logger *Logger::instance()
{
    static Logger s_instance;
    return &s_instance;
}

void Logger::install()
{
    // Touch the singleton so it outlives the handler, then take over message
    // delivery (keeping the prior handler to chain into).
    instance();
    g_previous = qInstallMessageHandler(&Logger::handler);
}

void Logger::handler(QtMsgType type, const QMessageLogContext &ctx,
                     const QString &msg)
{
    const QString line = QStringLiteral("%1  %2  %3")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
             levelTag(type), msg);
    instance()->append(line);
    if (g_previous)
        g_previous(type, ctx, msg);   // keep stderr/console output
}

QString Logger::logFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                        + QStringLiteral("/logs");
    return dir + QStringLiteral("/pocketplayer.log");
}

void Logger::writeToFile(const QString &line)
{
    // Caller holds m_mutex. Open lazily on the first write (AppData is resolvable
    // only after QApplication exists); give up quietly if it can't be opened.
    if (!m_file.isOpen()) {
        // AppData is empty until QApplication is constructed; a line logged that
        // early just skips the file (and retries later) rather than burning the
        // one-shot attempt on an unresolvable path.
        if (QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).isEmpty())
            return;
        if (m_fileTried)
            return;
        m_fileTried = true;
        const QString path = logFilePath();
        QDir().mkpath(QFileInfo(path).absolutePath());
        m_file.setFileName(path);
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
            return;
        m_fileBytes = m_file.size();
    }

    const QByteArray bytes = (line + QLatin1Char('\n')).toUtf8();
    m_file.write(bytes);
    m_file.flush();   // survive a crash with the log intact
    m_fileBytes += bytes.size();

    if (m_fileBytes > kMaxFileBytes) {
        // One-backup rotation: close, replace .log.1 with the current file, reopen
        // truncated. Bounds disk use without a real log framework.
        m_file.close();
        const QString path = logFilePath();
        const QString backup = path + QStringLiteral(".1");
        QFile::remove(backup);
        QFile::rename(path, backup);
        if (m_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            m_fileBytes = 0;
    }
}

void Logger::append(const QString &line)
{
    {
        QMutexLocker lock(&m_mutex);
        m_lines.append(line);
        if (m_lines.size() > kMaxLines)
            m_lines.remove(0, m_lines.size() - kMaxLines);
        writeToFile(line);
    }
    emit lineAppended(line);   // queued to the GUI thread by the consumer
}

QString Logger::text() const
{
    QMutexLocker lock(&m_mutex);
    return m_lines.join(QLatin1Char('\n'));
}

void Logger::clear()
{
    QMutexLocker lock(&m_mutex);
    m_lines.clear();
    if (m_file.isOpen()) {
        m_file.resize(0);   // truncate the live file so viewer + file stay in sync
        m_fileBytes = 0;
    }
}
