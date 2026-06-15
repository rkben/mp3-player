#include "SettingsDialog.h"
#include "Logger.h"
#ifdef HAVE_VISUALIZER
#include "ShaderArt.h"   // ShaderArt::availableShaders() for the visualizer dropdown
#endif

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
#include <QDesktopServices>
#include <QUrl>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextCursor>
#include <QFontDatabase>
#include <QDir>
#include <QMediaDevices>
#include <QAudioDevice>

#include "YtDlpManager.h"
#include "YtDlp.h"
#include "SubsonicClient.h"

#include <QListWidget>
#include <QSignalBlocker>

SettingsDialog::SettingsDialog(QList<LibraryFolder> folders, bool autoSync,
                               bool restoreQueue, bool autoPlay,
                               QString ytDlpPath, QByteArray audioDeviceId,
                               Theme::Mode themeMode, QString themeFile,
                               YtDlpManager *ytdlp, QWidget *parent)
    : QDialog(parent), m_ytdlp(ytdlp)
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
    m_themeCombo->setToolTip(tr("\"System\" follows your desktop's light/dark setting; "
                                "\"Dark\" forces the built-in dark theme; \"Custom\" "
                                "loads your own Qt stylesheet."));

    m_themeFileEdit = new QLineEdit(themeFile);
    m_themeFileEdit->setPlaceholderText(tr("Path to a .qss stylesheet"));
    m_themeFileEdit->setToolTip(tr("A Qt Style Sheet (.qss/.css) applied on top of the "
                                   "base theme. Used only when the theme is \"Custom\"."));
    m_themeBrowse = new QPushButton(tr("Browse…"));
    m_themeBrowse->setMinimumHeight(34);
    m_themeBrowse->setToolTip(tr("Choose a .qss/.css stylesheet file."));

    m_restoreQueue = new QCheckBox(tr("Restore last queue"));
    m_restoreQueue->setChecked(restoreQueue);
    m_restoreQueue->setToolTip(tr("Reload the play queue from your last session on "
                                  "startup, including the playing track and position."));

    m_autoPlay = new QCheckBox(tr("Auto-play"));
    m_autoPlay->setChecked(autoPlay);
    m_autoPlay->setToolTip(tr("Start playback automatically when the app launches "
                              "(respects the shuffle setting)."));

    m_savedYtOverride = ytDlpPath.trimmed();
    m_ytDlpEdit = new QLineEdit;   // text set by updateYtPathField() once managed state is known
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

    // ---- Subsonic tab ----
    {
        auto *page = new QWidget;
        auto *l = new QVBoxLayout(page);
        l->addWidget(buildSubsonicGroup());
        l->addStretch(1);
        tabs->addTab(scrollable(page), tr("Subsonic"));
    }

    tabs->addTab(scrollable(buildLogTab()), tr("Log"));
    tabs->addTab(scrollable(buildAboutTab()), tr("About"));
    tabs->setCurrentIndex(0);   // open on General
    m_tabs = tabs;

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // Persist the managed-yt-dlp toggle on OK (download/remove already act on disk).
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        QSettings s;
        s.setValue(QStringLiteral("ytdlp/useManaged"), m_ytUseManaged->isChecked());
        s.setValue(QStringLiteral("playback/preferHq"),
                   m_preferHqCombo->currentData().toInt());
        // Split into lines, dropping blanks/whitespace-only entries.
        const QStringList patterns = m_ignoreTitles->toPlainText().split(
            QLatin1Char('\n'), Qt::SkipEmptyParts);
        QStringList cleaned;
        for (const QString &p : patterns)
            if (!p.trimmed().isEmpty())
                cleaned.append(p.trimmed());
        s.setValue(QStringLiteral("playback/ignoreTitles"), cleaned);
#ifdef HAVE_VISUALIZER
        s.setValue(QStringLiteral("ui/visualizer"), m_visualizer->isChecked());
        s.setValue(QStringLiteral("ui/visualizerShader"),
                   m_visualizerShader->currentData().toString());
#endif
    });
    // Persist the Subsonic server list on OK.
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        ssCommitFields();
        SubsonicClient::saveServers(m_subsonicServers);
    });
#ifdef HAVE_DISCORD_RPC
    // Persist the Discord override on OK (this field doesn't round-trip via the host).
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        QSettings s;
        s.setValue(QStringLiteral("discord/appId"), m_discordAppId->text().trimmed());
        s.setValue(QStringLiteral("discord/enabled"), m_discordEnabled->isChecked());
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

void SettingsDialog::refreshYtStatus(const QString &override)
{
    if (!m_ytStatus)
        return;
    const bool installed = YtDlpManager::isManagedInstalled();
    const QString ver = YtDlpManager::installedVersion();
    const bool unsupported = YtDlpManager::assetName().isEmpty();

    // The "Managed:" row describes the managed binary's state. The label already says
    // "Managed", so the value is just that state — no redundant "Managed: Installed:"
    // / "Managed: Latest:" prefixes. Update info is appended by the latestVersion handler.
    if (!override.isEmpty())
        m_ytStatus->setText(override);
    else if (unsupported)
        m_ytStatus->setText(tr("Not available for this platform"));
    else if (installed)
        m_ytStatus->setText(ver.isEmpty() ? tr("Installed") : tr("Installed (%1)").arg(ver));
    else
        m_ytStatus->setText(tr("Not installed"));

    m_ytDownload->setText(installed ? tr("Update") : tr("Download"));
    const bool busy = m_ytdlp && m_ytdlp->busy();
    m_ytDownload->setEnabled(!unsupported && m_ytdlp && !busy);
    m_ytRemove->setEnabled(installed && !busy);
}

void SettingsDialog::updateYtPathField()
{
    // Always show the *effective* yt-dlp location, not just an override. When managed is
    // enabled the managed binary is what runs, so show its path and disable the field
    // (the override doesn't apply); otherwise show the override if set, else the
    // auto-detected $PATH location so the user can see what's being used.
    const bool managed = m_ytUseManaged && m_ytUseManaged->isChecked();
    if (managed) {
        m_ytDlpEdit->setText(YtDlpManager::managedPath());
        m_ytDlpEdit->setEnabled(false);
    } else {
        m_ytDlpEdit->setEnabled(true);
        m_ytDlpEdit->setText(m_savedYtOverride.isEmpty() ? YtDlp::systemPath()
                                                         : m_savedYtOverride);
    }
    if (m_ytReset)   // the override controls are inert while managed is authoritative
        m_ytReset->setEnabled(!managed);
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
    // When managed is on the field just shows the managed path (informational), so don't
    // persist it as an override. Otherwise persist the text only if it's a real override
    // — not the auto-detected $PATH location the field is pre-filled with.
    if (m_ytUseManaged && m_ytUseManaged->isChecked())
        return QString();
    const QString t = m_ytDlpEdit->text().trimmed();
    return (t == YtDlp::systemPath()) ? QString() : t;
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

    // ---- Playback ----
    auto *audioBox = new QGroupBox(tr("Playback"));
    auto *audioForm = new QFormLayout(audioBox);
    audioForm->setSpacing(8);
    audioForm->addRow(tr("Output device:"), m_audioCombo);
    // Apply the device live so the user hears the change immediately; the host
    // reverts on Cancel (mirrors the theme live-preview).
    connect(m_audioCombo, &QComboBox::currentIndexChanged, this, [this] {
        emit audioDeviceChanged(m_audioCombo->currentData().toByteArray());
    });

    // Prefer-HQ dedup (No / Naive / Yes), self-contained via QSettings.
    m_preferHqCombo = new QComboBox;
    m_preferHqCombo->addItem(tr("No"), 0);
    m_preferHqCombo->addItem(tr("Naive (lossless over lossy)"), 1);
    m_preferHqCombo->addItem(tr("Yes (also higher bitrate)"), 2);
    m_preferHqCombo->setCurrentIndex(
        QSettings().value(QStringLiteral("playback/preferHq"), 0).toInt());
    m_preferHqCombo->setToolTip(tr("When duplicates of the same song are queued, keep "
                                   "only the best copy. \"Naive\" prefers lossless over "
                                   "lossy; \"Yes\" also prefers the higher bitrate. "
                                   "Applies to local tracks."));
    audioForm->addRow(tr("Prefer higher quality:"), m_preferHqCombo);
    auto *hqHint = new QLabel(tr("When the same song exists in several files, play the "
                                 "better one — lossless over lossy, then higher bitrate."));
    hqHint->setWordWrap(true);
    {
        QFont f = hqHint->font();
        f.setPointSize(qMax(1, f.pointSize() - 1));
        hqHint->setFont(f);
    }
    audioForm->addRow(hqHint);

    // Ignore-by-title (one regex per line). Self-contained via QSettings; matched
    // case-insensitively against a local track's title when a set is enqueued.
    // Local-only by design — remote tracks (Subsonic/yt-dlp) are never filtered.
    m_ignoreTitles = new QPlainTextEdit;
    m_ignoreTitles->setPlainText(
        QSettings().value(QStringLiteral("playback/ignoreTitles"))
            .toStringList().join(QLatin1Char('\n')));
    m_ignoreTitles->setPlaceholderText(
        tr("One pattern per line, e.g.\n^intro$\n\\[skit\\]\nhidden track"));
    m_ignoreTitles->setMaximumHeight(96);
    m_ignoreTitles->setToolTip(tr("One regular expression per line. A local track "
                                  "whose title matches any of them is skipped when "
                                  "building the play queue."));
    audioForm->addRow(tr("Ignore titles:"), m_ignoreTitles);
    auto *ignoreHint = new QLabel(tr("Keep local tracks out of the play queue when their "
                                     "title matches one of these regular expressions "
                                     "(one per line). Applies to your local library "
                                     "only — streamed sources such as Subsonic and "
                                     "yt-dlp are never filtered. Matched "
                                     "case-insensitively; invalid patterns are ignored."));
    ignoreHint->setWordWrap(true);
    {
        QFont f = ignoreHint->font();
        f.setPointSize(qMax(1, f.pointSize() - 1));
        ignoreHint->setFont(f);
    }
    audioForm->addRow(ignoreHint);
    layout->addWidget(audioBox);

#ifdef HAVE_VISUALIZER
    // ---- Visualizer ----
    // Self-contained via QSettings (like Prefer-HQ): the checkbox swaps album art
    // for a QRhiWidget shader; the combo picks which baked shader (names enumerated
    // from the baked resources). Applied live by the host, reverted on Cancel, saved
    // on OK.
    auto *visBox = new QGroupBox(tr("Visualizer"));
    auto *visForm = new QFormLayout(visBox);
    visForm->setSpacing(8);

    m_visualizer = new QCheckBox(tr("Show audio visualizer instead of album art"));
    m_visualizer->setChecked(
        QSettings().value(QStringLiteral("ui/visualizer"), false).toBool());
    m_visualizer->setToolTip(tr("Replace the album-art panel with a live shader that "
                                "reacts to the audio. Off keeps the cover image and "
                                "leaves the audio analysis disabled."));
    visForm->addRow(m_visualizer);

    m_visualizerShader = new QComboBox;
    for (const QString &name : ShaderArt::availableShaders())
        m_visualizerShader->addItem(name, name);
    const QString curShader =
        QSettings().value(QStringLiteral("ui/visualizerShader")).toString();
    if (const int idx = m_visualizerShader->findData(curShader); idx >= 0)
        m_visualizerShader->setCurrentIndex(idx);
    m_visualizerShader->setEnabled(m_visualizer->isChecked());
    m_visualizerShader->setToolTip(tr("Which visualizer shader to display. "
                                      "Only used while the visualizer is shown."));
    visForm->addRow(tr("Shader:"), m_visualizerShader);

    auto emitVisualizer = [this] {
        emit visualizerChanged(m_visualizer->isChecked(),
                               m_visualizerShader->currentData().toString());
    };
    connect(m_visualizer, &QCheckBox::toggled, this,
            [this, emitVisualizer](bool on) {
                m_visualizerShader->setEnabled(on);
                emitVisualizer();
            });
    connect(m_visualizerShader, &QComboBox::currentIndexChanged, this,
            [emitVisualizer] { emitVisualizer(); });
    layout->addWidget(visBox);
#endif // HAVE_VISUALIZER

    // ---- Import ----
    auto *importBox = new QGroupBox(tr("Import"));
    auto *importForm = new QFormLayout(importBox);
    importForm->setSpacing(8);

    // --- Managed yt-dlp (download + auto-update from GitHub releases) ---
    m_ytUseManaged = new QCheckBox(tr("Use managed yt-dlp"));
    m_ytUseManaged->setChecked(
        QSettings().value(QStringLiteral("ytdlp/useManaged"), false).toBool());
    m_ytUseManaged->setToolTip(tr("Let the app download and update its own private copy "
                                  "of yt-dlp, instead of the one on your system PATH. "
                                  "The managed copy takes precedence when enabled."));
    importForm->addRow(m_ytUseManaged);
    auto *ytManagedHint = new QLabel(tr("Download and keep yt-dlp updated "
        "automatically instead of using the copy on your system PATH."));
    ytManagedHint->setWordWrap(true);
    {
        QFont f = ytManagedHint->font();
        f.setPointSize(qMax(1, f.pointSize() - 1));
        ytManagedHint->setFont(f);
    }
    importForm->addRow(ytManagedHint);

    m_ytStatus = new QLabel;
    importForm->addRow(tr("Managed:"), m_ytStatus);

    auto *ytBtns = new QHBoxLayout;
    m_ytDownload = new QPushButton;   // label set by refreshYtStatus (Download/Update)
    m_ytDownload->setToolTip(tr("Fetch the latest yt-dlp release from GitHub into the "
                                "app's private copy."));
    m_ytRemove = new QPushButton(tr("Remove"));
    m_ytRemove->setToolTip(tr("Delete the managed binary and use the system PATH copy."));
    ytBtns->addWidget(m_ytDownload);
    ytBtns->addWidget(m_ytRemove);
    ytBtns->addStretch(1);
    importForm->addRow(ytBtns);

    if (m_ytdlp) {
        connect(m_ytUseManaged, &QCheckBox::toggled, this, [this] {
            refreshYtStatus();
            updateYtPathField();   // managed on -> show managed path + disable the field
        });
        connect(m_ytDownload, &QPushButton::clicked, this, [this] {
            m_ytDownload->setEnabled(false);
            m_ytRemove->setEnabled(false);
            m_ytdlp->downloadLatest();
        });
        connect(m_ytRemove, &QPushButton::clicked, this, [this] {
            m_ytdlp->remove();
            refreshYtStatus();
        });
        connect(m_ytdlp, &YtDlpManager::downloadProgress, this,
                [this](qint64 rec, qint64 total) {
                    refreshYtStatus(total > 0
                        ? tr("Downloading… %1%").arg(rec * 100 / total)
                        : tr("Downloading…"));
                });
        connect(m_ytdlp, &YtDlpManager::installed, this, [this] { refreshYtStatus(); });
        connect(m_ytdlp, &YtDlpManager::removed, this, [this] { refreshYtStatus(); });
        connect(m_ytdlp, &YtDlpManager::failed, this,
                [this](const QString &m) { refreshYtStatus(tr("Error: %1").arg(m)); });
        // A failed version check clears busy but emits no version — re-enable the
        // buttons (without it, Download stays greyed until the dialog is reopened).
        connect(m_ytdlp, &YtDlpManager::checkFailed, this,
                [this](const QString &) { refreshYtStatus(); });
        connect(m_ytdlp, &YtDlpManager::latestVersion, this,
                [this](const QString &tag, bool upd) {
                    if (!YtDlpManager::isManagedInstalled()) {
                        refreshYtStatus();   // not installed: latest tag is just noise
                        return;
                    }
                    const QString ver = YtDlpManager::installedVersion();
                    const QString base = ver.isEmpty() ? tr("Installed")
                                                       : tr("Installed (%1)").arg(ver);
                    refreshYtStatus(upd ? tr("%1 — update available: %2").arg(base, tag)
                                        : tr("%1 — up to date").arg(base));
                });
        m_ytdlp->checkLatest();   // refresh availability when the dialog opens
    } else {
        m_ytUseManaged->setEnabled(false);
        m_ytDownload->setEnabled(false);
        m_ytRemove->setEnabled(false);
    }
    refreshYtStatus();

    // yt-dlp path + Reset. Reset only clears this explicit override (falls back to the
    // managed binary if enabled, else $PATH) — it never deletes the managed binary.
    auto *ytRow = new QHBoxLayout;
    m_ytReset = new QPushButton(tr("Reset"));
    m_ytReset->setToolTip(tr("Clear this override (use managed yt-dlp or $PATH)."));
    connect(m_ytReset, &QPushButton::clicked, this, [this] {
        m_savedYtOverride.clear();
        updateYtPathField();   // back to the auto-detected $PATH location
    });
    ytRow->addWidget(m_ytDlpEdit, 1);
    ytRow->addWidget(m_ytReset);
    importForm->addRow(tr("yt-dlp path:"), ytRow);
    updateYtPathField();   // initial display (effective path; disabled if managed)
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
    auto *bestEffort = new QLabel(tr("Best effort: everything yt-dlp supports."));
    bestEffort->setWordWrap(true);
    bestEffort->setFont(hintFont);
    importForm->addRow(bestEffort);
    layout->addWidget(importBox);

#ifdef HAVE_DISCORD_RPC
    // ---- Discord (only present in a Discord-enabled build) ----
    // Self-contained: loads from / saves to QSettings("discord/appId") directly
    // rather than threading an #ifdef'd value through the constructor/getters. Blank
    // = use the application ID baked in at build (shown as the placeholder).
    auto *discordBox = new QGroupBox(tr("Discord"));
    auto *discordForm = new QFormLayout(discordBox);
    discordForm->setSpacing(8);

    // Enable toggle — applied live by the host (connect/disconnect), saved on OK.
    m_discordEnabled = new QCheckBox(tr("Show now-playing as a Discord status"));
    m_discordEnabled->setChecked(
        QSettings().value(QStringLiteral("discord/enabled"), false).toBool());
    m_discordEnabled->setToolTip(tr("Display the current track as your Discord Rich "
                                    "Presence while the app is running and Discord is "
                                    "open."));
    connect(m_discordEnabled, &QCheckBox::toggled, this,
            [this](bool on) { emit discordEnabledChanged(on); });
    discordForm->addRow(m_discordEnabled);

    m_discordAppId = new QLineEdit(
        QSettings().value(QStringLiteral("discord/appId")).toString());
    const QString baked = QString::fromLatin1(DISCORD_APP_ID).trimmed();
    m_discordAppId->setPlaceholderText(
        baked.isEmpty() ? tr("Discord application ID") : baked);
    m_discordAppId->setToolTip(tr("Override the built-in Discord application ID. "
                                  "Blank uses the default. Applies on next launch."));
    // App ID + Reset (clears the override so the baked-in default is used).
    auto *idRow = new QHBoxLayout;
    auto *idReset = new QPushButton(tr("Reset"));
    idReset->setToolTip(tr("Clear the override and use the built-in application ID."));
    connect(idReset, &QPushButton::clicked, this, [this] { m_discordAppId->clear(); });
    idRow->addWidget(m_discordAppId, 1);
    idRow->addWidget(idReset);
    discordForm->addRow(tr("Application ID:"), idRow);
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
    addBtn->setToolTip(tr("Add a folder to scan for music. It and its subfolders are "
                          "indexed into your library."));
    connect(addBtn, &QPushButton::clicked, this, &SettingsDialog::addFolder);
    m_removeBtn = new QPushButton(tr("Remove"));
    m_removeBtn->setMinimumHeight(34);
    m_removeBtn->setToolTip(tr("Stop scanning the selected folder. Its tracks are "
                               "dropped from the library on the next sync."));
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
    auto *openBtn = new QPushButton(tr("Open log file"));
    openBtn->setMinimumHeight(34);
    openBtn->setToolTip(Logger::logFilePath());
    connect(openBtn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(Logger::logFilePath()));
    });
    auto *copyBtn = new QPushButton(tr("Copy"));
    copyBtn->setMinimumHeight(34);
    connect(copyBtn, &QPushButton::clicked, this,
            [this] { m_logView->selectAll(); m_logView->copy();
                     m_logView->moveCursor(QTextCursor::End); });
    auto *clearBtn = new QPushButton(tr("Clear"));
    clearBtn->setMinimumHeight(34);
    connect(clearBtn, &QPushButton::clicked, this,
            [this] { Logger::instance()->clear(); m_logView->clear(); });
    btnRow->addWidget(openBtn);
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
        "commons.wikimedia.org</a><br>"
        "<b>qt-toast</b> — toast sizing approach, by Niklas Henning, MIT · "
        "<a href=\"https://github.com/niklashenning/qt-toast\">github.com</a>"));

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

// --- Subsonic servers (multi-server list + edit fields + Test) ------------------

static QString ssLabel(const SubsonicServer &sv)
{
    if (!sv.name.isEmpty())
        return sv.name;
    const QString host = QUrl(sv.url).host();
    return host.isEmpty() ? SettingsDialog::tr("New server") : host;
}

// A usable server base URL: an explicit http/https scheme and a host. We don't guess a
// scheme — the user enters the full URL (e.g. http://host:4533).
static bool ssUrlValid(const QString &raw)
{
    const QUrl u(raw.trimmed());
    return (u.scheme() == QLatin1String("http") || u.scheme() == QLatin1String("https"))
        && !u.host().isEmpty();
}

QWidget *SettingsDialog::buildSubsonicGroup()
{
    m_subsonicServers = SubsonicClient::servers();

    auto *box = new QGroupBox(tr("Subsonic servers"));
    auto *outer = new QVBoxLayout(box);
    outer->setSpacing(8);

    auto *hint = new QLabel(tr("Stream from Subsonic / OpenSubsonic servers (Navidrome, "
                               "Airsonic, …); their libraries sync into yours."));
    hint->setWordWrap(true);
    { QFont f = hint->font(); f.setPointSize(qMax(1, f.pointSize() - 1)); hint->setFont(f); }
    outer->addWidget(hint);

    m_ssList = new QListWidget;
    m_ssList->setMaximumHeight(110);
    outer->addWidget(m_ssList);

    auto *listBtns = new QHBoxLayout;
    auto *addBtn = new QPushButton(tr("Add"));
    addBtn->setToolTip(tr("Add a new Subsonic/OpenSubsonic server entry to configure "
                          "below."));
    m_ssRemoveBtn = new QPushButton(tr("Remove"));
    m_ssRemoveBtn->setToolTip(tr("Delete the selected server and remove its synced "
                                 "tracks from your library."));
    listBtns->addWidget(addBtn);
    listBtns->addWidget(m_ssRemoveBtn);
    listBtns->addStretch(1);
    outer->addLayout(listBtns);

    auto *form = new QFormLayout;
    m_ssName = new QLineEdit;  m_ssName->setPlaceholderText(tr("My server"));
    m_ssUrl  = new QLineEdit;  m_ssUrl->setPlaceholderText(tr("https://music.example.com"));
    m_ssUser = new QLineEdit;
    m_ssPass = new QLineEdit;  m_ssPass->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Name:"), m_ssName);
    form->addRow(tr("URL:"), m_ssUrl);
    form->addRow(tr("Username:"), m_ssUser);
    form->addRow(tr("Password:"), m_ssPass);
    outer->addLayout(form);

    auto *testRow = new QHBoxLayout;
    m_ssTestBtn = new QPushButton(tr("Test"));
    m_ssTestBtn->setToolTip(tr("Check that the URL and credentials can reach the "
                               "server, without syncing anything."));
    auto *syncBtn = new QPushButton(tr("Sync now"));
    syncBtn->setToolTip(tr("Save and sync this server's library into yours (runs in the "
                           "background; you can close Settings)."));
    m_ssStatus = new QLabel;
    m_ssStatus->setWordWrap(true);
    testRow->addWidget(m_ssTestBtn);
    testRow->addWidget(syncBtn);
    testRow->addWidget(m_ssStatus, 1);
    outer->addLayout(testRow);

    connect(m_ssList, &QListWidget::currentRowChanged, this, [this] { ssLoadSelected(); });
    for (QLineEdit *e : {m_ssName, m_ssUrl, m_ssUser, m_ssPass})
        connect(e, &QLineEdit::textEdited, this, [this] { ssCommitFields(); });
    connect(addBtn, &QPushButton::clicked, this, [this] { ssAddServer(); });
    connect(m_ssRemoveBtn, &QPushButton::clicked, this, [this] { ssRemoveServer(); });
    connect(m_ssTestBtn, &QPushButton::clicked, this, [this] { ssTestSelected(); });
    connect(syncBtn, &QPushButton::clicked, this, [this] { ssSyncNow(); });

    for (const SubsonicServer &sv : m_subsonicServers)
        m_ssList->addItem(ssLabel(sv));
    if (!m_subsonicServers.isEmpty())
        m_ssList->setCurrentRow(0);
    else
        ssLoadSelected();   // disables the fields when there's nothing selected
    return box;
}

void SettingsDialog::ssLoadSelected()
{
    const int row = m_ssList->currentRow();
    const bool ok = row >= 0 && row < m_subsonicServers.size();
    for (QLineEdit *e : {m_ssName, m_ssUrl, m_ssUser, m_ssPass})
        e->setEnabled(ok);
    m_ssRemoveBtn->setEnabled(ok);
    m_ssTestBtn->setEnabled(ok);
    m_ssStatus->clear();
    if (!ok) {
        for (QLineEdit *e : {m_ssName, m_ssUrl, m_ssUser, m_ssPass})
            e->clear();
        return;
    }
    const SubsonicServer &sv = m_subsonicServers.at(row);
    m_ssName->setText(sv.name);     // setText doesn't emit textEdited, so no commit loop
    m_ssUrl->setText(sv.url);
    m_ssUser->setText(sv.user);
    m_ssPass->setText(sv.password);
}

void SettingsDialog::ssCommitFields()
{
    const int row = m_ssList->currentRow();
    if (row < 0 || row >= m_subsonicServers.size())
        return;
    SubsonicServer &sv = m_subsonicServers[row];
    sv.name = m_ssName->text().trimmed();
    sv.url = m_ssUrl->text().trimmed();
    sv.user = m_ssUser->text().trimmed();
    sv.password = m_ssPass->text();
    if (auto *it = m_ssList->item(row))
        it->setText(ssLabel(sv));
}

void SettingsDialog::ssAddServer()
{
    SubsonicServer sv;
    sv.id = SubsonicClient::newId();
    sv.name = tr("New server");
    m_subsonicServers.append(sv);
    m_ssList->addItem(ssLabel(sv));
    m_ssList->setCurrentRow(m_ssList->count() - 1);
    m_ssName->setFocus();
    m_ssName->selectAll();
}

void SettingsDialog::ssRemoveServer()
{
    const int row = m_ssList->currentRow();
    if (row < 0 || row >= m_subsonicServers.size())
        return;
    m_subsonicServers.removeAt(row);
    delete m_ssList->takeItem(row);   // currentRowChanged -> ssLoadSelected
}

void SettingsDialog::ssSyncNow()
{
    ssCommitFields();
    const int row = m_ssList->currentRow();
    if (row < 0 || row >= m_subsonicServers.size())
        return;
    if (!m_subsonicServers.at(row).valid()) {
        m_ssStatus->setText(tr("Fill in URL, username and password first."));
        return;
    }
    if (!ssUrlValid(m_subsonicServers.at(row).url)) {
        m_ssStatus->setText(tr("Enter a full URL including http:// or https:// "
                               "(e.g. http://host:4533)."));
        return;
    }
    // Persist now so the host can look the server up by id and sync it in the background.
    SubsonicClient::saveServers(m_subsonicServers);
    m_ssStatus->setText(tr("Syncing in the background…"));
    emit subsonicSyncRequested(m_subsonicServers.at(row).id);
}

void SettingsDialog::ssTestSelected()
{
    ssCommitFields();
    const int row = m_ssList->currentRow();
    if (row < 0 || row >= m_subsonicServers.size())
        return;
    if (!m_subsonicServers.at(row).valid()) {
        m_ssStatus->setText(tr("Fill in URL, username and password first."));
        return;
    }
    if (!ssUrlValid(m_subsonicServers.at(row).url)) {
        m_ssStatus->setText(tr("Enter a full URL including http:// or https:// "
                               "(e.g. http://host:4533)."));
        return;
    }
    m_ssStatus->setText(tr("Testing…"));
    m_ssTestBtn->setEnabled(false);
    auto *client = new SubsonicClient(m_subsonicServers.at(row), this);
    connect(client, &SubsonicClient::pinged, this, [this, client](bool ok, const QString &msg) {
        m_ssStatus->setText(ok ? tr("✓ %1").arg(msg) : tr("✗ %1").arg(msg));
        m_ssTestBtn->setEnabled(true);
        client->deleteLater();
    });
    client->ping();
}
