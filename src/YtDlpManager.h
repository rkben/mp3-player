#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

// Downloads and manages a yt-dlp binary under AppData, so remote streaming/import
// works without the user installing yt-dlp themselves. Uses GitHub's "latest release"
// assets — the platform-specific PyInstaller standalones (no Python needed) — and
// verifies the download against the release's SHA-256 sums before installing.
//
// Path resolution (RemoteResolver::ytDlpPath) prefers the managed binary when the
// "use managed" toggle is on; the binary lives under AppDataLocation so a full app
// reset removes it for free. All network work is async (QNetworkAccessManager).
class YtDlpManager : public QObject
{
    Q_OBJECT
public:
    explicit YtDlpManager(QObject *parent = nullptr);

    // <AppData>/bin/yt-dlp (".exe" on Windows). Static so ytDlpPath() can consult it
    // without an instance.
    static QString managedPath();
    // The GitHub release asset for this platform/arch, or empty if unsupported.
    static QString assetName();
    static bool isManagedInstalled();
    // The installed managed version string, or empty if none (QSettings).
    static QString installedVersion();

    bool busy() const { return m_busy; }

public slots:
    // Query GitHub for the latest release tag. Emits latestVersion() or checkFailed().
    void checkLatest();
    // Download + verify + install the latest binary. Emits progress/installed/failed.
    void downloadLatest();
    // Delete the managed binary and forget its version (revert to $PATH). Leaves the
    // explicit ytdlp/path override untouched.
    void remove();

signals:
    void latestVersion(const QString &tag, bool updateAvailable);
    void checkFailed(const QString &message);
    void downloadProgress(qint64 received, qint64 total);
    void installed(const QString &version);
    void failed(const QString &message);
    void removed();

private:
    void startBinaryDownload(const QString &expectedSha256);
    void finishInstall(const QString &tempPath);
    void fail(const QString &message);

    QNetworkAccessManager *m_net;
    QNetworkReply *m_reply = nullptr;   // in-flight request (check or download)
    QFile *m_temp = nullptr;            // download sink
    QString m_tempPath;
    bool m_busy = false;
};
