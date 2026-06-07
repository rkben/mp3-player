#include "YtDlpManager.h"
#include "ProcUtil.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QSettings>
#include <QCryptographicHash>
#include <QProcess>
#include <QSysInfo>

namespace {
constexpr char kApiLatest[] =
    "https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest";
constexpr char kDownloadBase[] =
    "https://github.com/yt-dlp/yt-dlp/releases/latest/download/";
constexpr char kVersionKey[] = "ytdlp/managedVersion";

// GitHub's API rejects requests without a User-Agent; set one on every request.
QNetworkRequest ghRequest(const QString &url)
{
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("PocketPlayer"));
    return req;
}
}

QString YtDlpManager::managedPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                        + QStringLiteral("/bin");
#ifdef Q_OS_WIN
    return dir + QStringLiteral("/yt-dlp.exe");
#else
    return dir + QStringLiteral("/yt-dlp");
#endif
}

QString YtDlpManager::assetName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("yt-dlp.exe");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("yt-dlp_macos");
#elif defined(Q_OS_LINUX)
    const QString arch = QSysInfo::currentCpuArchitecture();
    if (arch == QLatin1String("arm64") || arch == QLatin1String("aarch64"))
        return QStringLiteral("yt-dlp_linux_aarch64");
    if (arch == QLatin1String("x86_64"))
        return QStringLiteral("yt-dlp_linux");
    return {};   // unsupported arch
#else
    return {};
#endif
}

bool YtDlpManager::isManagedInstalled()
{
    return QFileInfo::exists(managedPath());
}

QString YtDlpManager::installedVersion()
{
    return QSettings().value(QLatin1String(kVersionKey)).toString();
}

YtDlpManager::YtDlpManager(QObject *parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this))
{
}

void YtDlpManager::checkLatest()
{
    if (m_busy)
        return;
    m_busy = true;
    QNetworkReply *reply = m_net->get(ghRequest(QLatin1String(kApiLatest)));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        m_busy = false;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning().noquote() << QStringLiteral("[ytdlp] check failed: %1")
                                        .arg(reply->errorString());
            emit checkFailed(reply->errorString());
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        const QString tag = o.value(QStringLiteral("tag_name")).toString();
        if (tag.isEmpty()) {
            emit checkFailed(tr("Could not read the latest release."));
            return;
        }
        const bool updateAvailable = tag != installedVersion();
        qInfo().noquote() << QStringLiteral("[ytdlp] latest %1 (installed %2)")
                                 .arg(tag, installedVersion().isEmpty()
                                               ? QStringLiteral("none")
                                               : installedVersion());
        emit latestVersion(tag, updateAvailable);
    });
}

void YtDlpManager::downloadLatest()
{
    if (m_busy)
        return;
    const QString asset = assetName();
    if (asset.isEmpty()) {
        fail(tr("No yt-dlp build is available for this platform."));
        return;
    }
    m_busy = true;
    qInfo("[ytdlp] downloading %s", qPrintable(asset));

    // Phase 1: fetch the checksum manifest, find our asset's expected SHA-256.
    QNetworkReply *sums = m_net->get(ghRequest(QLatin1String(kDownloadBase)
                                               + QStringLiteral("SHA2-256SUMS")));
    connect(sums, &QNetworkReply::finished, this, [this, sums, asset] {
        sums->deleteLater();
        if (sums->error() != QNetworkReply::NoError) {
            fail(tr("Couldn't fetch checksums: %1").arg(sums->errorString()));
            return;
        }
        QString expected;
        const QString text = QString::fromUtf8(sums->readAll());
        for (const QString &line : text.split('\n', Qt::SkipEmptyParts)) {
            // "<sha256>  <filename>"
            const QStringList cols = line.simplified().split(' ');
            if (cols.size() >= 2 && cols.last() == asset) {
                expected = cols.first();
                break;
            }
        }
        if (expected.isEmpty()) {
            fail(tr("No checksum found for %1.").arg(asset));
            return;
        }
        startBinaryDownload(expected);
    });
}

void YtDlpManager::startBinaryDownload(const QString &expectedSha256)
{
    const QString asset = assetName();
    m_tempPath = managedPath() + QStringLiteral(".download");
    QDir().mkpath(QFileInfo(m_tempPath).absolutePath());

    m_temp = new QFile(m_tempPath, this);
    if (!m_temp->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("Couldn't open a temporary file for the download."));
        return;
    }

    // Phase 2: stream the binary to the temp file, then verify before installing.
    m_reply = m_net->get(ghRequest(QLatin1String(kDownloadBase) + asset));
    connect(m_reply, &QNetworkReply::downloadProgress, this,
            &YtDlpManager::downloadProgress);
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (m_temp)
            m_temp->write(m_reply->readAll());
    });
    connect(m_reply, &QNetworkReply::finished, this, [this, expectedSha256] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->deleteLater();
        if (m_temp) {
            m_temp->write(reply->readAll());
            m_temp->close();
        }
        if (reply->error() != QNetworkReply::NoError) {
            fail(tr("Download failed: %1").arg(reply->errorString()));
            return;
        }

        QFile f(m_tempPath);
        QByteArray actual;
        if (f.open(QIODevice::ReadOnly)) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (hash.addData(&f))
                actual = hash.result().toHex();
        }
        if (actual.compare(expectedSha256.toLatin1(), Qt::CaseInsensitive) != 0) {
            fail(tr("Checksum mismatch — the download was discarded."));
            return;
        }
        finishInstall(m_tempPath);
    });
}

void YtDlpManager::finishInstall(const QString &tempPath)
{
    const QString dest = managedPath();
    QFile::remove(dest);
    if (!QFile::rename(tempPath, dest)) {
        fail(tr("Couldn't install the downloaded binary."));
        return;
    }
#ifndef Q_OS_WIN
    QFile::setPermissions(dest, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                    | QFileDevice::ExeOwner | QFileDevice::ReadGroup
                                    | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                    | QFileDevice::ExeOther);
#endif

    // Confirm it actually runs and capture its version.
    QProcess probe;
    suppressConsoleWindow(&probe);
    probe.start(dest, {QStringLiteral("--version")});
    QString version;
    if (probe.waitForFinished(5000) && probe.exitStatus() == QProcess::NormalExit
        && probe.exitCode() == 0)
        version = QString::fromUtf8(probe.readAllStandardOutput()).trimmed();
    if (version.isEmpty()) {
        QFile::remove(dest);
        fail(tr("The downloaded yt-dlp could not be run."));
        return;
    }

    QSettings().setValue(QLatin1String(kVersionKey), version);
    m_busy = false;
    if (m_temp) { m_temp->deleteLater(); m_temp = nullptr; }
    qInfo().noquote() << QStringLiteral("[ytdlp] installed %1").arg(version);
    emit installed(version);
}

void YtDlpManager::remove()
{
    QFile::remove(managedPath());
    QSettings().remove(QLatin1String(kVersionKey));
    qInfo("[ytdlp] managed binary removed; reverting to $PATH");
    emit removed();
}

void YtDlpManager::fail(const QString &message)
{
    if (m_reply) { m_reply->abort(); m_reply->deleteLater(); m_reply = nullptr; }
    if (m_temp) {
        m_temp->close();
        m_temp->deleteLater();
        m_temp = nullptr;
    }
    if (!m_tempPath.isEmpty())
        QFile::remove(m_tempPath);
    m_busy = false;
    qWarning().noquote() << QStringLiteral("[ytdlp] %1").arg(message);
    emit failed(message);
}
