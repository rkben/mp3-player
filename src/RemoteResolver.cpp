#include "RemoteResolver.h"

#include <QDebug>

RemoteResolver::RemoteResolver(QObject *parent) : QObject(parent)
{
    connect(&m_dlp, &YtDlp::finished, this,
            [this](int /*code*/, const QByteArray &out, const QByteArray &err) {
                const QUrl page = m_pageUrl;
                // `-g` prints one URL per selected stream; take the first non-empty.
                for (const QByteArray &line : out.split('\n')) {
                    const QString s = QString::fromUtf8(line).trimmed();
                    if (!s.isEmpty()) {
                        qInfo().noquote()
                            << QStringLiteral("[resolve] ok %1").arg(page.toString());
                        emit resolved(page, QUrl(s));
                        return;
                    }
                }
                // No stream URL: map the common failures to a plain reason.
                const QString e = QString::fromUtf8(err).trimmed();
                QString msg;
                if (e.contains(QStringLiteral("DRM"), Qt::CaseInsensitive))
                    msg = tr("This track is DRM-protected and can't be streamed");
                else
                    msg = e.isEmpty() ? tr("yt-dlp failed to resolve the stream") : e;
                qWarning().noquote() << QStringLiteral("[resolve] failed %1 — %2")
                                            .arg(page.toString(), msg);
                emit failed(page, msg);
            });
    connect(&m_dlp, &YtDlp::startFailed, this,
            [this](const QString &msg) { emit failed(m_pageUrl, msg); });
}

void RemoteResolver::resolve(const QUrl &pageUrl)
{
    qInfo().noquote() << QStringLiteral("[resolve] %1").arg(pageUrl.toString());
    m_pageUrl = pageUrl;
    // Best audio-only stream (falls back to best combined if none), URL only. run()
    // supersedes any still-running resolve.
    m_dlp.run({QStringLiteral("-f"), QStringLiteral("bestaudio/best"),
               QStringLiteral("-g"), pageUrl.toString()});
}
