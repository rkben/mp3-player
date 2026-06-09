#pragma once

#include <QDialog>
#include <QStringList>

class QLineEdit;
class QPushButton;
class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QDialogButtonBox;
class YtDlp;

// Modal for the Playlists tab "Import" button: a URL field + inline "Check" that
// probes the URL with yt-dlp. If it resolves to multiple tracks the playlist
// options (Create from URL / Append to existing) become available. Returns the
// URL and the chosen playlist intent; Import itself runs in the background.
class ImportDialog : public QDialog
{
    Q_OBJECT
public:
    ImportDialog(const QStringList &playlists, bool savedCreate,
                 const QString &savedAppend, QWidget *parent = nullptr);

    QString url() const;
    bool createPlaylist() const;        // make a new playlist from the source
    QString appendPlaylist() const;     // append to this existing one ("" = none)
    // The per-entry page URLs found by the last successful Check, and the source's
    // playlist title — handed to the Importer so it doesn't re-enumerate.
    QStringList entries() const { return m_entries; }
    QString playlistTitle() const { return m_playlistTitle; }

private:
    void runCheck();
    void onCheckDone(int code, const QByteArray &out, const QByteArray &err);
    void setPlaylistOptionsEnabled(bool on);

    QLineEdit *m_url;
    QPushButton *m_check;
    QLabel *m_needYtDlp;   // shown + input disabled when no yt-dlp is available
    QLabel *m_info;
    QPlainTextEdit *m_errorView;   // full yt-dlp stderr on failure; hidden otherwise
    QCheckBox *m_create;
    QComboBox *m_append;
    QDialogButtonBox *m_buttons;
    YtDlp *m_dlp = nullptr;        // the Check probe (lifecycle-safe; cancels on close)
    QStringList m_entries;        // entry URLs from the last successful Check
    QString m_playlistTitle;      // source playlist title (best-effort)
    bool m_multiple = false;
};
