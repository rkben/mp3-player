#pragma once

#include <QObject>
#include <QStringList>
#include <QByteArray>

#include <memory>

class QProcess;

// One async yt-dlp invocation, buffered and lifecycle-safe. Wraps a QProcess: resolves
// the yt-dlp executable, suppresses the console window (Windows), collects stdout/stderr,
// and emits finished() exactly once via a one-shot guard so a kill/crash can't double- or
// stale-fire. cancel() (also run from the destructor) mutes that guard and kills the
// process, so an owner can be destroyed mid-run without a callback reaching its freed
// members — fixing the classic "QProcess outlives its dialog" crash by construction.
//
// Buffered: the whole of stdout/stderr is delivered in finished(). Fine for the short
// probe/resolve calls (flat-playlist, -g); a long streaming run (the importer's hydrate)
// keeps its own QProcess.
class YtDlp : public QObject
{
    Q_OBJECT
public:
    explicit YtDlp(QObject *parent = nullptr);
    ~YtDlp() override;

    // The configured (or auto-discovered) yt-dlp path; empty if none is found.
    // Precedence: explicit override → managed binary (if enabled) → systemPath().
    static QString path();
    // Just the $PATH / common-install-dir lookup, ignoring the override and managed
    // settings — the "where yt-dlp is by default" baseline for the Settings field.
    static QString systemPath();

    bool busy() const { return m_proc != nullptr; }

    // Run `yt-dlp <args>`. Any in-flight run on this instance is superseded (cancelled).
    // If `stdinData` is non-empty it's written to stdin once the process starts, then the
    // write channel is closed (e.g. `--batch-file -`). Emits finished() or startFailed().
    void run(const QStringList &args, const QByteArray &stdinData = {});

    // Mute the one-shot guard, kill any running process, and clean up. No signal fires.
    void cancel();

signals:
    void finished(int exitCode, const QByteArray &out, const QByteArray &err);
    void startFailed(const QString &error);   // couldn't launch yt-dlp

private:
    QProcess *m_proc = nullptr;     // the in-flight run, or null
    std::shared_ptr<bool> m_done;   // its one-shot guard, so we can mute it
};
