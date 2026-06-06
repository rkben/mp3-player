#include "ImportDialog.h"
#include "RemoteResolver.h"   // ytDlpPath()
#include "ProcUtil.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QProcess>

ImportDialog::ImportDialog(const QStringList &playlists, bool savedCreate,
                           const QString &savedAppend, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Import from URL"));
    setModal(true);
    setMinimumWidth(460);

    auto *root = new QVBoxLayout(this);

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
    connect(m_check, &QPushButton::clicked, this, &ImportDialog::runCheck);
    connect(m_url, &QLineEdit::returnPressed, this, &ImportDialog::runCheck);
    // Create-from-URL and append-to-existing are mutually exclusive.
    connect(m_create, &QCheckBox::toggled, this, [this](bool on) {
        m_append->setEnabled(!on && m_multiple);
    });

    setPlaylistOptionsEnabled(false);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);   // require a check first
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
    const QString exe = RemoteResolver::ytDlpPath();
    if (exe.isEmpty()) {
        m_info->setText(tr("yt-dlp not found — set its path in Settings."));
        return;
    }
    if (m_proc)
        return;   // a check is already running

    m_check->setEnabled(false);
    m_info->setText(tr("Checking…"));

    m_proc = new QProcess(this);
    // Fast, shallow probe: list entry ids without extracting each track.
    suppressConsoleWindow(m_proc);
    m_proc->start(exe, {QStringLiteral("--flat-playlist"), QStringLiteral("--no-warnings"),
                        QStringLiteral("--print"), QStringLiteral("id"), url});
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        const int count = QString::fromUtf8(m_proc->readAllStandardOutput())
                              .split('\n', Qt::SkipEmptyParts).size();
        const QString err = QString::fromUtf8(m_proc->readAllStandardError()).trimmed();
        m_proc->deleteLater();
        m_proc = nullptr;
        m_check->setEnabled(true);

        if (code != 0 && count == 0) {
            m_info->setText(err.isEmpty() ? tr("Couldn't read that URL.")
                                          : err.section('\n', -1));
            m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
            return;
        }
        if (count > 1) {
            m_info->setText(tr("Found %n track(s).", nullptr, count));
            setPlaylistOptionsEnabled(true);
        } else {
            m_info->setText(tr("Single track."));
            setPlaylistOptionsEnabled(false);
        }
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
    });
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
