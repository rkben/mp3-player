#include "ImportDialog.h"
#include "YtDlp.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QFontDatabase>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

ImportDialog::ImportDialog(const QStringList &playlists, bool savedCreate,
                           const QString &savedAppend, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Import from URL"));
    setModal(true);
    setMinimumWidth(460);
    resize(460, 340);   // a touch taller by default; still grows with the error box

    auto *root = new QVBoxLayout(this);

    // Shown only when no yt-dlp is available (managed off + not on PATH); the input
    // and Check are disabled in that case (set up at the end of the constructor).
    m_needYtDlp = new QLabel(tr("yt-dlp is required to import. Enable managed yt-dlp "
                                "in Settings, or install it on your PATH."));
    m_needYtDlp->setWordWrap(true);
    m_needYtDlp->hide();
    root->addWidget(m_needYtDlp);

    // URL + inline Check.
    auto *urlRow = new QHBoxLayout;
    m_url = new QLineEdit;
    m_url->setPlaceholderText(tr("Paste a track or playlist URL"));
    m_check = new QPushButton(tr("Check"));
    urlRow->addWidget(m_url, 1);
    urlRow->addWidget(m_check);
    root->addLayout(urlRow);

    m_info = new QLabel(tr("Check a URL to see what it contains."));
    m_info->setWordWrap(true);
    root->addWidget(m_info);

    // Full yt-dlp stderr on a failed check — monospace + wrapped + read-only, hidden
    // until there's something to show so the dialog stays compact in the happy path.
    m_errorView = new QPlainTextEdit;
    m_errorView->setReadOnly(true);
    m_errorView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_errorView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_errorView->setMinimumHeight(120);
    m_errorView->hide();
    root->addWidget(m_errorView);

    // Playlist options — disabled until a check finds multiple tracks.
    auto *plBox = new QGroupBox(tr("Playlist"));
    auto *plForm = new QFormLayout(plBox);
    m_create = new QCheckBox(tr("Create a new playlist from this URL"));
    m_create->setChecked(savedCreate);
    m_append = new QComboBox;
    m_append->setToolTip(tr("Add entries to an existing playlist"));
    m_append->addItem(tr("(none)"));
    m_append->addItems(playlists);
    if (!savedAppend.isEmpty()) {
        const int idx = m_append->findText(savedAppend);
        if (idx >= 0)
            m_append->setCurrentIndex(idx);
    }
    plForm->addRow(m_create);
    plForm->addRow(tr("Append to:"), m_append);
    root->addWidget(plBox);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Import"));
    root->addWidget(m_buttons);

    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // The Check probe runs through a lifecycle-safe runner: it's a child of the dialog,
    // so closing the dialog mid-check cancels it cleanly (no callback into freed members).
    m_dlp = new YtDlp(this);
    connect(m_dlp, &YtDlp::finished, this, &ImportDialog::onCheckDone);
    connect(m_dlp, &YtDlp::startFailed, this, [this](const QString &msg) {
        m_info->setText(msg);
        m_check->setEnabled(true);
    });

    connect(m_check, &QPushButton::clicked, this, &ImportDialog::runCheck);
    connect(m_url, &QLineEdit::returnPressed, this, &ImportDialog::runCheck);
    // Editing the URL invalidates a prior check: require a fresh one before Import so
    // the captured entries can't go stale against a changed URL.
    connect(m_url, &QLineEdit::textEdited, this, [this] {
        m_entries.clear();
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    });
    // Create-from-URL and append-to-existing are mutually exclusive.
    connect(m_create, &QCheckBox::toggled, this, [this](bool on) {
        m_append->setEnabled(!on && m_multiple);
    });

    setPlaylistOptionsEnabled(false);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);   // require a check first

    // Without yt-dlp there's nothing to import: warn up top and disable the input.
    if (YtDlp::path().isEmpty()) {
        m_needYtDlp->show();
        m_url->setEnabled(false);
        m_check->setEnabled(false);
        m_info->hide();
    }
}

void ImportDialog::setPlaylistOptionsEnabled(bool on)
{
    m_multiple = on;
    m_create->setEnabled(on);
    m_append->setEnabled(on && !m_create->isChecked());
}

void ImportDialog::runCheck()
{
    const QString url = m_url->text().trimmed();
    if (url.isEmpty())
        return;
    if (YtDlp::path().isEmpty()) {
        m_info->setText(tr("yt-dlp not found — set its path in Settings."));
        return;
    }
    if (m_dlp->busy())
        return;   // a check is already running

    m_check->setEnabled(false);
    m_info->setText(tr("Checking…"));
    m_entries.clear();
    m_playlistTitle.clear();
    // --flat-playlist --dump-json: the same shallow probe the importer enumerates with,
    // so we capture the per-entry URLs (and playlist title) here and hand them straight
    // to the import — no second flat-playlist pass.
    m_dlp->run({QStringLiteral("--flat-playlist"), QStringLiteral("--dump-json"),
                QStringLiteral("--no-warnings"), QStringLiteral("--ignore-errors"), url});
}

void ImportDialog::onCheckDone(int /*code*/, const QByteArray &out, const QByteArray &err)
{
    m_check->setEnabled(true);

    auto strOf = [](const QJsonObject &o, std::initializer_list<const char *> keys) {
        for (const char *k : keys) {
            const QString v = o.value(QLatin1String(k)).toString().trimmed();
            if (!v.isEmpty())
                return v;
        }
        return QString();
    };
    for (const QByteArray &line : out.split('\n')) {
        if (line.trimmed().isEmpty())
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject())
            continue;
        const QJsonObject o = doc.object();
        const QString u = strOf(o, {"url", "webpage_url", "original_url"});
        if (!u.isEmpty() && !m_entries.contains(u))
            m_entries << u;
        if (m_playlistTitle.isEmpty())
            m_playlistTitle = strOf(o, {"playlist_title", "playlist", "title"});
    }
    const int count = m_entries.size();

    if (count == 0) {
        // Short reason in the label; the full yt-dlp output in the error box. HearThis.at
        // and ReverbNation only expose individual tracks in yt-dlp, so an uploader/
        // playlist URL fails — give a clearer reason (see notes/new_remote_sources.md).
        const QString host = QUrl(m_url->text().trimmed()).host();
        const bool singleOnly =
            host.contains(QLatin1String("hearthis.at"), Qt::CaseInsensitive)
            || host.contains(QLatin1String("reverbnation.com"), Qt::CaseInsensitive);
        m_info->setText(singleOnly ? tr("This source only supports single tracks.")
                                   : tr("Couldn't read that URL."));
        const QString errText = QString::fromUtf8(err).trimmed();
        if (errText.isEmpty()) {
            m_errorView->hide();
        } else {
            m_errorView->setPlainText(errText);
            m_errorView->show();
        }
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        return;
    }
    m_errorView->clear();
    m_errorView->hide();
    if (count > 1) {
        m_info->setText(tr("Found %n track(s).", nullptr, count));
        setPlaylistOptionsEnabled(true);
    } else {
        m_info->setText(tr("Single track."));
        setPlaylistOptionsEnabled(false);
    }
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
}

QString ImportDialog::url() const { return m_url->text().trimmed(); }

bool ImportDialog::createPlaylist() const
{
    return m_multiple && m_create->isChecked();
}

QString ImportDialog::appendPlaylist() const
{
    if (!m_multiple || m_create->isChecked() || m_append->currentIndex() <= 0)
        return {};
    return m_append->currentText();
}
