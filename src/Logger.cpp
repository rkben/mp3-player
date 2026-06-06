#include "Logger.h"

#include <QDateTime>

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

void Logger::append(const QString &line)
{
    {
        QMutexLocker lock(&m_mutex);
        m_lines.append(line);
        if (m_lines.size() > kMaxLines)
            m_lines.remove(0, m_lines.size() - kMaxLines);
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
}
