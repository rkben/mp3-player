#include "SettingsDialog.h"
#include "Logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QTabWidget>
#include <QScrollArea>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QTableWidget>
#include <QHeaderView>
#include <QGroupBox>
#include <QSettings>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextCursor>
#include <QFontDatabase>
#include <QDir>
#include <QMediaDevices>
#include <QAudioDevice>

SettingsDialog::SettingsDialog(QList<LibraryFolder> folders, bool autoSync,
                               bool restoreQueue, bool autoPlay,
                               QString ytDlpPath, QByteArray audioDeviceId,
                               Theme::Mode themeMode, QString themeFile,
                               QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Settings"));
    setModal(true);
    setMinimumWidth(420);
    resize(620, 520);

    auto *root = new QVBoxLayout(this);
    auto *tabs = new QTabWidget;

    // Each tab's content can outgrow the dialog (small screens, the Library folder
    // list, the Discord row, etc.), so wrap every tab in a vertically-scrolling area.
    // setWidgetResizable lets the content keep its natural width and only scroll once
    // it's taller than the viewport.
    auto scrollable = [](QWidget *content) -> QWidget * {
        auto *area = new QScrollArea;
        area->setWidgetResizable(true);
        area->setFrameShape(QFrame::NoFrame);
        area->setWidget(content);
        return area;
    };
    root->addWidget(tabs);

    // ---- General tab ----
    // Combo carries the Theme::Mode in item data; file row only enabled for Custom.
    m_themeCombo = new QComboBox;
    m_themeCombo->addItem(tr("System (native)"), int(Theme::Mode::System));
    m_themeCombo->addItem(tr("Dark"), int(Theme::Mode::Dark));
    m_themeCombo->addItem(tr("Custom stylesheet…"), int(Theme::Mode::Custom));
    m_themeCombo->setCurrentIndex(m_themeCombo->findData(int(themeMode)));

    m_themeFileEdit = new QLineEdit(themeFile);
    m_themeFileEdit->setPlaceholderText(tr("Path to a .qss stylesheet"));
    m_themeBrowse = new QPushButton(tr("Browse…"));
    m_themeBrowse->setMinimumHeight(34);

    m_restoreQueue = new QCheckBox(tr("Restore last queue"));
    m_restoreQueue->setChecked(restoreQueue);

    m_autoPlay = new QCheckBox(tr("Auto-play"));
    m_autoPlay->setChecked(autoPlay);
    m_autoPlay->setToolTip(tr("Start playback automatically when the app launches "
                              "(respects the shuffle setting)."));

    m_ytDlpEdit = new QLineEdit(ytDlpPath);
    m_ytDlpEdit->setPlaceholderText(tr("Path to yt-dlp"));
    m_ytDlpEdit->setToolTip(tr("Used to import and stream remote tracks. "
                               "Defaults to the yt-dlp found on $PATH."));

    // Output device. QMediaDevices is Qt's cross-platform audio-device seam, so
    // the same combo works on every backend. Each item carries the device id
    // (QAudioDevice::id()) in its data; the first item is the system default
    // (empty id) so the player follows OS device changes unless overridden.
    m_audioCombo = new QComboBox;
    m_audioCombo->addItem(tr("System default"), QByteArray());
    int audioIdx = 0;
    const auto outputs = QMediaDevices::audioOutputs();
    for (const QAudioDevice &dev : outputs) {
        m_audioCombo->addItem(dev.description(), dev.id());
        if (dev.id() == audioDeviceId)
            audioIdx = m_audioCombo->count() - 1;
    }
    m_audioCombo->setCurrentIndex(audioIdx);
    m_audioCombo->setToolTip(tr("Where to send audio. \"System default\" follows "
                                "the OS's current output device."));

    tabs->addTab(scrollable(buildGeneralTab()), tr("General"));

    connect(m_themeCombo, &QComboBox::currentIndexChanged, this,
            &SettingsDialog::onThemeRowChanged);
    connect(m_themeFileEdit, &QLineEdit::editingFinished, this,
            &SettingsDialog::onThemeRowChanged);
    connect(m_themeBrowse, &QPushButton::clicked, this, [this] {
        const QString start = m_themeFileEdit->text().isEmpty()
            ? QDir::homePath() : m_themeFileEdit->text();
        const QString f = QFileDialog::getOpenFileName(
            this, tr("Choose a Qt stylesheet"), start,
            tr("Qt stylesheets (*.qss *.css);;All files (*)"));
        if (!f.isEmpty()) {
            m_themeFileEdit->setText(f);
            onThemeRowChanged();
        }
    });
    onThemeRowChanged();   // set initial enabled state (no signal needed)

    // ---- Library tab ----
    QWidget *library = buildLibraryTab(autoSync);
    for (const LibraryFolder &f : folders)
        addFolderRow(f);   // m_folderTable exists once buildLibraryTab has run
    m_libraryPage = scrollable(library);
    tabs->addTab(m_libraryPage, tr("Library"));
    tabs->addTab(scrollable(buildLogTab()), tr("Log"));
    tabs->addTab(scrollable(buildAboutTab()), tr("About"));
    tabs->setCurrentIndex(0);   // open on General
    m_tabs = tabs;

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
#ifdef HAVE_DISCORD_RPC
    // Persist the Discord override on OK (this field doesn't round-trip via the host).
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        QSettings().setValue(QStringLiteral("discord/appId"),
                             m_discordAppId->text().trimmed());
    });
#endif
    root->addWidget(buttons);
}

QList<LibraryFolder> SettingsDialog::folders() const
{
    QList<LibraryFolder> out;
    for (int r = 0; r < m_folderTable->rowCount(); ++r) {
        const QString path = m_folderTable->item(r, 1)->text();
        if (path.isEmpty())
            continue;
        QString label = m_folderTable->item(r, 0)->text().trimmed();
        if (label.isEmpty())   // labels are required; default to the dir name
            label = QDir(path).dirName().isEmpty() ? path : QDir(path).dirName();
        out.append({label, path});
    }
    return out;
}

bool SettingsDialog::autoSync() const
{
    return m_autoSync->isChecked();
}

void SettingsDialog::selectLibraryTab()
{
    if (m_tabs && m_libraryPage)
        m_tabs->setCurrentWidget(m_libraryPage);
}

bool SettingsDialog::restoreQueue() const
{
    return m_restoreQueue->isChecked();
}

bool SettingsDialog::autoPlay() const
{
    return m_autoPlay->isChecked();
}

QString SettingsDialog::ytDlpPath() const
{
    return m_ytDlpEdit->text().trimmed();
}

QByteArray SettingsDialog::audioDeviceId() const
{
    return m_audioCombo->currentData().toByteArray();
}

Theme::Mode SettingsDialog::themeMode() const
{
    return Theme::Mode(m_themeCombo->currentData().toInt());
}

QString SettingsDialog::themeFile() const
{
    return m_themeFileEdit->text();
}

QWidget *SettingsDialog::buildGeneralTab()
{
    auto *general = new QWidget;
    auto *layout = new QVBoxLayout(general);
    layout->setSpacing(12);

    // ---- Startup ----
    auto *startupBox = new QGroupBox(tr("Startup"));
    auto *startupForm = new QFormLayout(startupBox);
    startupForm->setSpacing(8);
    startupForm->addRow(m_restoreQueue);
    startupForm->addRow(m_autoPlay);
    layout->addWidget(startupBox);

    // ---- Theme ----
    auto *themeBox = new QGroupBox(tr("Theme"));
    auto *themeForm = new QFormLayout(themeBox);
    themeForm->setSpacing(8);
    themeForm->addRow(tr("Theme:"), m_themeCombo);

    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_themeFileEdit, 1);
    fileRow->addWidget(m_themeBrowse);
    themeForm->addRow(tr("Stylesheet:"), fileRow);
    layout->addWidget(themeBox);

    // ---- Audio ----
    auto *audioBox = new QGroupBox(tr("Audio"));
    auto *audioForm = new QFormLayout(audioBox);
    audioForm->setSpacing(8);
    audioForm->addRow(tr("Output device:"), m_audioCombo);
    layout->addWidget(audioBox);

    // ---- Import ----
    auto *importBox = new QGroupBox(tr("Import"));
    auto *importForm = new QFormLayout(importBox);
    importForm->setSpacing(8);
    importForm->addRow(tr("yt-dlp path:"), m_ytDlpEdit);
    // Curated list of sources known to map cleanly (anything yt-dlp supports still
    // imports generically — this is guidance, not a gate). Extend as sources are
    // tuned in Importer::parseLine. A full-width hint under the path: a word-wrap
    // label in a form's field column mis-sizes when the dialog is wide, so span both
    // columns and style it like the other small hints rather than a disabled field.
    auto *sources = new QLabel(tr("Supported sources: %1").arg(QStringLiteral(
        "YouTube, SoundCloud, Bandcamp, Mixcloud, HearThis.at, ReverbNation")));
    sources->setWordWrap(true);
    QFont hintFont = sources->font();
    hintFont.setPointSize(qMax(1, hintFont.pointSize() - 1));
    sources->setFont(hintFont);
    importForm->addRow(sources);   // single-widget row spans the full groupbox width
    layout->addWidget(importBox);

#ifdef HAVE_DISCORD_RPC
    // ---- Discord (only present in a Discord-enabled build) ----
    // Self-contained: loads from / saves to QSettings("discord/appId") directly
    // rather than threading an #ifdef'd value through the constructor/getters. Blank
    // = use the application ID baked in at build (shown as the placeholder).
    auto *discordBox = new QGroupBox(tr("Discord"));
    auto *discordForm = new QFormLayout(discordBox);
    discordForm->setSpacing(8);
    m_discordAppId = new QLineEdit(
        QSettings().value(QStringLiteral("discord/appId")).toString());
    const QString baked = QString::fromLatin1(DISCORD_APP_ID).trimmed();
    m_discordAppId->setPlaceholderText(
        baked.isEmpty() ? tr("Discord application ID") : baked);
    m_discordAppId->setToolTip(tr("Override the built-in Discord application ID. "
                                  "Blank uses the default. Applies on next launch."));
    discordForm->addRow(tr("Application ID:"), m_discordAppId);
    layout->addWidget(discordBox);
#endif

    // ---- Reset (destructive; at the very bottom) ----
    auto *resetBox = new QGroupBox(tr("Reset"));
    auto *resetLayout = new QVBoxLayout(resetBox);
    auto *resetHint = new QLabel(
        tr("Erase the library database, playlists, cached art, and all settings — "
           "a clean slate. This cannot be undone."));
    resetHint->setWordWrap(true);
    auto *resetBtn = new QPushButton(tr("Reset application…"));
    connect(resetBtn, &QPushButton::clicked, this, [this] {
        const auto choice = QMessageBox::warning(
            this, tr("Reset application"),
            tr("Permanently erase the library database, playlists, cached cover art, "
               "and all settings, then quit?\n\nThis cannot be undone."),
            QMessageBox::Reset | QMessageBox::Cancel, QMessageBox::Cancel);
        if (choice == QMessageBox::Reset) {
            m_resetRequested = true;
            accept();   // host sees resetRequested() and performs the wipe
        }
    });
    resetLayout->addWidget(resetHint);
    resetLayout->addWidget(resetBtn, 0, Qt::AlignLeft);
    layout->addWidget(resetBox);

    layout->addStretch();
    return general;
}

QWidget *SettingsDialog::buildLibraryTab(bool autoSync)
{
    auto *library = new QWidget;
    auto *libLayout = new QVBoxLayout(library);

    // Multiple labelled folders. Top-level label shows in the library tree.
    libLayout->addWidget(new QLabel(tr("Library folders:")));

    m_folderTable = new QTableWidget(0, 2);
    m_folderTable->setHorizontalHeaderLabels({tr("Label"), tr("Path")});
    m_folderTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_folderTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_folderTable->verticalHeader()->hide();
    m_folderTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_folderTable->setSelectionMode(QAbstractItemView::SingleSelection);
    libLayout->addWidget(m_folderTable, 1);

    auto *folderBtns = new QHBoxLayout;
    auto *addBtn = new QPushButton(tr("Add folder…"));
    addBtn->setMinimumHeight(34);
    connect(addBtn, &QPushButton::clicked, this, &SettingsDialog::addFolder);
    m_removeBtn = new QPushButton(tr("Remove"));
    m_removeBtn->setMinimumHeight(34);
    connect(m_removeBtn, &QPushButton::clicked, this, &SettingsDialog::removeFolder);
    folderBtns->addWidget(addBtn);
    folderBtns->addWidget(m_removeBtn);
    folderBtns->addStretch();
    libLayout->addLayout(folderBtns);

    // Auto-sync isn't implemented yet, so the control is hidden rather than shown
    // as a non-functional checkbox. Kept as a hidden member (no layout row) so the
    // persisted "library/autoSync" value round-trips untouched until the feature
    // lands and a real row is restored here.
    m_autoSync = new QCheckBox(this);
    m_autoSync->setChecked(autoSync);
    m_autoSync->hide();

    // Manual sync: a quick mtime reconcile of all configured folders.
    auto *syncRow = new QHBoxLayout;
    auto *syncBtn = new QPushButton(tr("Sync now"));
    syncBtn->setMinimumHeight(34);
    syncBtn->setToolTip(tr("Scan the library folders for new, changed, "
                           "and removed files."));
    connect(syncBtn, &QPushButton::clicked, this, &SettingsDialog::syncRequested);
    auto *syncHint = new QLabel(tr("Checks for new/changed/removed files."));
    QFont hintFont = syncHint->font();
    hintFont.setPointSize(qMax(1, hintFont.pointSize() - 1));
    syncHint->setFont(hintFont);
    syncRow->addWidget(syncBtn);
    syncRow->addWidget(syncHint, 1);
    libLayout->addLayout(syncRow);

    return library;
}

QWidget *SettingsDialog::buildLogTab()
{
    auto *tab = new QWidget;
    auto *layout = new QVBoxLayout(tab);

    m_logView = new QPlainTextEdit;
    m_logView->setReadOnly(true);
    m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_logView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_logView->setPlainText(Logger::instance()->text());
    layout->addWidget(m_logView, 1);

    // Live-append new lines. AutoConnection makes this queued when the message
    // arrives from a worker thread, so the widget is only touched on the GUI thread.
    connect(Logger::instance(), &Logger::lineAppended, this,
            [this](const QString &line) {
                const bool atBottom =
                    m_logView->verticalScrollBar()->value()
                        == m_logView->verticalScrollBar()->maximum();
                m_logView->appendPlainText(line);
                if (atBottom)   // only auto-follow if the user hadn't scrolled up
                    m_logView->verticalScrollBar()->setValue(
                        m_logView->verticalScrollBar()->maximum());
            });

    auto *btnRow = new QHBoxLayout;
    auto *copyBtn = new QPushButton(tr("Copy"));
    copyBtn->setMinimumHeight(34);
    connect(copyBtn, &QPushButton::clicked, this,
            [this] { m_logView->selectAll(); m_logView->copy();
                     m_logView->moveCursor(QTextCursor::End); });
    auto *clearBtn = new QPushButton(tr("Clear"));
    clearBtn->setMinimumHeight(34);
    connect(clearBtn, &QPushButton::clicked, this,
            [this] { Logger::instance()->clear(); m_logView->clear(); });
    btnRow->addStretch();
    btnRow->addWidget(copyBtn);
    btnRow->addWidget(clearBtn);
    layout->addLayout(btnRow);

    // Start scrolled to the newest line.
    m_logView->moveCursor(QTextCursor::End);
    return tab;
}

QWidget *SettingsDialog::buildAboutTab()
{
    auto *about = new QWidget;
    auto *layout = new QVBoxLayout(about);
    layout->setSpacing(10);

    auto *title = new QLabel(tr("Pocket Player"));
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.3);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    layout->addWidget(new QLabel(tr("A lightweight, power-conscious music player.")));

    auto *license = new QLabel(
        tr("Released into the public domain (or under the BSD Zero Clause "
           "License) — final license to be decided."));
    license->setWordWrap(true);
    layout->addWidget(license);

    auto makeHeading = [&](const QString &text) {
        auto *h = new QLabel(text);
        QFont f = h->font();
        f.setBold(true);
        h->setFont(f);
        layout->addSpacing(6);
        layout->addWidget(h);
    };

    auto richLabel = [&](const QString &html) {
        auto *l = new QLabel(html);
        l->setTextFormat(Qt::RichText);
        l->setWordWrap(true);
        l->setOpenExternalLinks(true);
        l->setTextInteractionFlags(Qt::TextBrowserInteraction);
        layout->addWidget(l);
    };

    makeHeading(tr("Built with"));
    richLabel(tr(
        "<b>Qt 6</b> — LGPLv3 · "
        "<a href=\"https://www.qt.io/\">qt.io</a><br>"
        "<b>TagLib</b> — LGPLv2.1 / MPL 1.1 · "
        "<a href=\"https://taglib.org/\">taglib.org</a><br>"
        "<b>FFmpeg</b> — LGPLv2.1+ (Qt Multimedia backend) · "
        "<a href=\"https://ffmpeg.org/\">ffmpeg.org</a>"));

    makeHeading(tr("Attribution"));
    richLabel(tr(
        "<b>album_icon.svg</b> — by Pymouss, Own work, Public Domain · "
        "<a href=\"https://commons.wikimedia.org/w/index.php?curid=5793388\">"
        "commons.wikimedia.org</a>"));

    layout->addStretch();
    return about;
}

void SettingsDialog::onThemeRowChanged()
{
    const bool custom = themeMode() == Theme::Mode::Custom;
    m_themeFileEdit->setEnabled(custom);
    m_themeBrowse->setEnabled(custom);
    emit themeChanged(themeMode(), m_themeFileEdit->text());   // live preview
}

void SettingsDialog::addFolderRow(const LibraryFolder &f)
{
    const int r = m_folderTable->rowCount();
    m_folderTable->insertRow(r);

    auto *label = new QTableWidgetItem(f.label);   // editable: the required label
    auto *path = new QTableWidgetItem(f.path);
    path->setFlags(path->flags() & ~Qt::ItemIsEditable);
    path->setToolTip(f.path);
    m_folderTable->setItem(r, 0, label);
    m_folderTable->setItem(r, 1, path);
}

void SettingsDialog::addFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Choose a music folder"), QDir::homePath());
    if (dir.isEmpty())
        return;
    // Default the (required) label to the directory name; the user can edit it.
    QString label = QDir(dir).dirName();
    if (label.isEmpty())
        label = dir;
    addFolderRow({label, dir});
    m_folderTable->editItem(m_folderTable->item(m_folderTable->rowCount() - 1, 0));
}

void SettingsDialog::removeFolder()
{
    const int r = m_folderTable->currentRow();
    if (r >= 0)
        m_folderTable->removeRow(r);
}
