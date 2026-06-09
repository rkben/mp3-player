#include "MainWindow.h"
#include "PlaylistModel.h"
#include "PlayerController.h"
#include "SettingsDialog.h"
#include "MusicLibrary.h"
#include "RemoteResolver.h"
#include "CoverLabel.h"
#include "StatusStack.h"
#include "Theme.h"
#include "AudioFormats.h"
#include "TimeFormat.h"
#include "TrackUri.h"
#include "Toast.h"
#include "Notifier.h"
#include "Importer.h"
#include "ImportDialog.h"
#include "PlaylistEditorDialog.h"
#include "YtDlpManager.h"
#include "TrackListModel.h"   // totalDurationMs()

#ifdef HAVE_DISCORD_RPC
#include "DiscordPresence.h"
#endif
#include "MediaSession.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableView>
#include <QTreeView>
#include <QTabWidget>
#include <QSplitter>
#include <QHeaderView>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QDir>
#include <QStandardPaths>
#include <QDirIterator>
#include <QFileInfo>
#include <QDesktopServices>
#include <QHash>
#include <QLineEdit>
#include <QComboBox>
#include <QTimer>
#include <QLabel>
#include <QSlider>
#include <QToolButton>
#include <QMenu>
#include <QListWidget>
#include <QPushButton>
#include <QInputDialog>
#include <QMessageBox>
#include <QDialog>
#include <QFormLayout>
#include <QPlainTextEdit>
#include <QFontDatabase>
#include <QDialogButtonBox>
#include <QStyle>
#include <QIcon>
#include <QSettings>
#include <QLocale>
#include <QThread>
#include <QApplication>
#include <QFrame>
#include <QResizeEvent>
#include <QEvent>
#include <QPixmap>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QRandomGenerator>

namespace {
// Custom item-data roles for the library tree's QStandardItemModel.
enum TreeRole {
    PathRole = Qt::UserRole + 1,   // absolute filesystem path (empty for none)
    IsDirRole,                     // bool: directory vs. audio file
    PopulatedRole,                 // bool: children have been lazily loaded
    KeyRole,                       // virtual-tree leaf: a track key (url string)
    IsGroupRole,                   // virtual-tree grouping node (artist/album/host)
};

// A leaf is an enqueueable track: a virtual-tree leaf (carries a key) or a
// filesystem file (has a path, isn't a directory). Everything else is a group.
bool isLeafNode(const QModelIndex &i)
{
    if (!i.data(KeyRole).toString().isEmpty())
        return true;
    return !i.data(PathRole).toString().isEmpty() && !i.data(IsDirRole).toBool();
}

constexpr int kSearchDebounceMs = 220;   // idle time after typing before searching
constexpr int kQueueCacheDebounceMs = 800;   // coalesce burst queue edits into one write

// Playlist list item: display text is the bare name; this holds the right-anchored
// "(count - duration)" meta string drawn by PlaylistItemDelegate.
constexpr int PlaylistMetaRole = Qt::UserRole + 1;

// Draws a playlist row as "<name> ……… (count - duration)": the name left-aligned
// (elided if long), the meta string anchored to the right edge.
class PlaylistItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString name = index.data(Qt::DisplayRole).toString();
        const QString meta = index.data(PlaylistMetaRole).toString();

        opt.text.clear();   // we draw the two text segments ourselves
        const QWidget *w = opt.widget;
        QStyle *style = w ? w->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, w);

        constexpr int kHMargin = 6;   // breathing room at the row's left/right edges
        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, w)
                                   .adjusted(kHMargin, 0, -kHMargin, 0);
        const bool sel = opt.state & QStyle::State_Selected;
        painter->save();
        painter->setPen(opt.palette.color(sel ? QPalette::HighlightedText
                                              : QPalette::Text));
        const int metaW = opt.fontMetrics.horizontalAdvance(meta);
        QRect metaRect = textRect;
        metaRect.setLeft(textRect.right() - metaW);
        painter->drawText(metaRect, Qt::AlignRight | Qt::AlignVCenter, meta);

        QRect nameRect = textRect;
        nameRect.setRight(metaRect.left() - 8);   // gap before the meta column
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          opt.fontMetrics.elidedText(name, Qt::ElideRight,
                                                     nameRect.width()));
        painter->restore();
    }
};
}

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
    , m_model(new PlaylistModel(this))
    , m_controller(new PlayerController(this))
{
    buildUi();

    connect(m_controller, &PlayerController::currentTrackChanged,
            this, &MainWindow::onCurrentTrackChanged);
    connect(m_controller, &PlayerController::queueChanged,
            this, &MainWindow::onQueueChanged);
    connect(m_controller, &PlayerController::playbackStateChanged,
            this, &MainWindow::onPlaybackStateChanged);
    // Status hint while a remote stream is being resolved (yt-dlp) and opened. Posted to
    // its own (higher-priority) slot, so it overlays any scan/import line and the lower
    // line reappears when it clears — see StatusStack.
    connect(m_controller, &PlayerController::remoteResolving, this, [this](bool active) {
        m_status->post(StatusStack::Fetch, active ? tr("Fetching remote track…") : QString());
    });
    // Background prefetch of the next remote track's stream URL (lower-priority slot,
    // so an active foreground fetch outranks it).
    connect(m_controller, &PlayerController::remotePrefetching, this, [this](bool active) {
        m_status->post(StatusStack::Prefetch,
                       active ? tr("Prefetching remote track…") : QString());
    });
    connect(m_controller, &PlayerController::positionChanged,
            this, &MainWindow::onPositionChanged);
    connect(m_controller, &PlayerController::durationChanged,
            this, &MainWindow::onDurationChanged);
    connect(m_controller, &PlayerController::trackError, this,
            [](const QString &name, const QString &msg) {
                ToastArea::post(tr("Skipped %1 — %2").arg(name, msg));
            });
    connect(m_controller, &PlayerController::metadataResolved, this,
            [this](const QUrl &url, const QString &title, const QString &artist,
                   const QString &album, int trackNo, qint64 durationMs) {
                // Update the matching view row (if present) and persist to the DB.
                const int row = playRowForKey(url);
                if (row >= 0)
                    m_model->updateTrack(row, title, artist, album, durationMs, trackNo);
                if (m_controller->currentTrack().url == url) {
                    const Track &cur = m_controller->currentTrack();
                    setWindowTitle(cur.displayText());
                    showTrackInfo(cur);   // refresh the left info panel
                }
                emit enrichTrack(url.toLocalFile(), title, artist, album,
                                 trackNo, durationMs);
            });

    // Keep the widgets and persisted settings in sync with the controller, no
    // matter what changed it — UI buttons, restored settings, or MPRIS/D-Bus.
    connect(m_controller, &PlayerController::shuffleChanged, this, [this](bool on) {
        QSignalBlocker block(m_shuffleBtn);
        m_shuffleBtn->setChecked(on);
        QSettings().setValue("playback/shuffle", on);
    });
    connect(m_controller, &PlayerController::volumeChanged, this, [this](float v) {
        const int pct = qRound(v * 100);
        QSignalBlocker block(m_volume);
        m_volume->setValue(pct);   // persisted on slider release, not per change
        updateVolumeIcon();        // block() suppressed valueChanged, so refresh here
    });
    connect(m_controller, &PlayerController::repeatModeChanged, this,
            [this](PlayerController::RepeatMode mode) {
                m_repeatState = static_cast<int>(mode);
                updateRepeatButton();
                QSettings().setValue("playback/repeat", m_repeatState);
            });

    m_session = MediaSession::create(m_controller, this, this);   // null if none

#ifdef HAVE_DISCORD_RPC
    // Cross-platform Discord Rich Presence, alongside (not instead of) the OS media
    // session. Parented to this window; idle when no app ID is configured. Held so
    // the Settings enable toggle can flip it live.
    m_discord = new DiscordPresence(m_controller, this);
#endif

    // Managed yt-dlp: on startup, if enabled, check GitHub for a newer release and
    // notify (no auto-download). The binary lives under AppData (removed on reset).
    m_ytdlp = new YtDlpManager(this);
    connect(m_ytdlp, &YtDlpManager::latestVersion, this,
            [](const QString &tag, bool updateAvailable) {
                if (updateAvailable && YtDlpManager::isManagedInstalled())
                    ToastArea::post(
                        tr("yt-dlp update available (%1) — update in Settings").arg(tag));
            });
    if (QSettings().value("ytdlp/useManaged", false).toBool())
        m_ytdlp->checkLatest();

    startLibraryThread();
    restoreSettings();

    // Persist UI state on any quit path (window close, MPRIS Quit, etc.), not
    // just closeEvent — saveGeometry() is valid even once the window is hidden.
    connect(qApp, &QApplication::aboutToQuit, this, &MainWindow::saveUiState);
    connect(qApp, &QApplication::aboutToQuit, this, &MainWindow::flushQueueCache);
}

void MainWindow::flushQueueCache()
{
    if (m_resetting)
        return;   // a reset is wiping AppData — don't recreate the queue cache
    // A debounced queue edit may still be pending when we quit; write it now so the
    // resume cache reflects the final queue rather than a stale snapshot.
    if (m_queueCacheTimer->isActive()) {
        m_queueCacheTimer->stop();
        m_store.saveQueueCache(queueStoredPaths());
    }
}

MainWindow::~MainWindow()
{
    if (m_libThread) {
        m_library->cancel();      // abort any in-flight scan so close is prompt
        m_libThread->quit();
        m_libThread->wait();
    }
}

void MainWindow::startLibraryThread()
{
    m_libThread = new QThread(this);
    m_library = new MusicLibrary;        // no parent: moved to the worker thread
    m_library->moveToThread(m_libThread);

    // Worker is destroyed when its thread finishes.
    connect(m_libThread, &QThread::finished, m_library, &QObject::deleteLater);

    // UI -> worker (queued: runs on the worker thread, which owns the DB).
    connect(this, &MainWindow::requestScan, m_library, &MusicLibrary::scan);
    connect(this, &MainWindow::enrichTrack, m_library, &MusicLibrary::enrichMetadata);
    connect(this, &MainWindow::requestArt, m_library, &MusicLibrary::resolveArt);
    connect(m_library, &MusicLibrary::artResolved, this, &MainWindow::onArtResolved);
    connect(this, &MainWindow::requestSearch, m_library, &MusicLibrary::search);
    connect(m_library, &MusicLibrary::searchResults, this, &MainWindow::onSearchResults);

    // Worker -> UI (queued: marshalled back to the GUI thread).
    connect(m_library, &MusicLibrary::libraryLoaded, this, &MainWindow::onLibraryLoaded);
    connect(m_library, &MusicLibrary::tracksAppended, this, &MainWindow::onTracksAppended);
    connect(m_library, &MusicLibrary::scanProgress, this, &MainWindow::onScanProgress);
    connect(m_library, &MusicLibrary::scanStatus, this, &MainWindow::onScanStatus);
    connect(this, &MainWindow::importTracks, m_library, &MusicLibrary::importTracks);
    connect(this, &MainWindow::removeTracks, m_library, &MusicLibrary::removeTracks);

    m_libThread->start();

    // yt-dlp importer (GUI thread; async QProcess). Inserts remote tracks via the
    // library worker, then creates/appends a playlist and notifies.
    m_importer = new Importer(this);
    connect(m_importer, &Importer::status, this, &MainWindow::onScanStatus);  // reuse status bar
    connect(m_importer, &Importer::trackFailed, this,
            [](const QString &msg) { ToastArea::post(msg); });
    connect(m_importer, &Importer::failed, this, [](const QString &msg) {
        Notifier::notify(tr("Import failed"), msg);
    });
    // Two-phase resumable import (see Importer / MusicLibrary). The library worker owns
    // the resume table so a track-insert and its resume-row clear commit atomically;
    // the m3u8/notify fires once per job when its last entry is committed.
    connect(m_importer, &Importer::enumerated, m_library, &MusicLibrary::beginImportJob);
    connect(m_importer, &Importer::trackImported, m_library, &MusicLibrary::commitImportedTrack);
    connect(m_importer, &Importer::importEntriesFailed, m_library, &MusicLibrary::failImportEntries);
    connect(m_library, &MusicLibrary::resumeImportJob, m_importer, &Importer::resumeImportJob);
    connect(m_library, &MusicLibrary::importEntriesDropped, this,
            [](int n) { ToastArea::post(tr("%n track(s) couldn't be imported", nullptr, n)); });
    connect(m_library, &MusicLibrary::importJobFinished, this,
            [this](const QString &createName, const QString &appendName,
                   const QList<Track> &tracks) {
                if (tracks.isEmpty())
                    return;
                QStringList lines;
                lines.reserve(tracks.size());
                for (const Track &t : tracks)
                    lines << storedForm(t.url);
                if (!createName.isEmpty())
                    m_store.write(createName, lines);
                else if (!appendName.isEmpty())
                    m_store.append(appendName, lines);
                refreshPlaylists();

                Notifier::notify(tr("Import complete"),
                                 tr("Added %n track(s).", nullptr, int(tracks.size())));
            });

    // Replay any imports interrupted by a previous quit/crash, in the background.
    QMetaObject::invokeMethod(m_library, "loadPendingImports", Qt::QueuedConnection);
}

void MainWindow::buildUi()
{
    setWindowTitle(tr("Pocket Player"));
    resize(960, 600);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // No top title bar: the now-playing track goes in the window title, and the
    // settings button lives in the transport bar.

    // --- Left panel: two tabs (Library / Playlists). Library is the filesystem
    // tree of the configured folders, plus a "Remote" root for streamed tracks. ---
    m_treeModel = new QStandardItemModel(this);
    m_tree = new QTreeView;
    m_tree->setModel(m_treeModel);
    m_tree->setHeaderHidden(true);
    m_tree->setFrameShape(QFrame::NoFrame);   // no rounded frame border (any style)
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setAnimated(true);
    m_tree->setExpandsOnDoubleClick(false);   // single-click expands; dbl-click enqueues
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeView::clicked, this, &MainWindow::onTreeClicked);
    connect(m_tree, &QTreeView::doubleClicked, this, &MainWindow::onTreeActivated);
    connect(m_tree, &QTreeView::customContextMenuRequested,
            this, &MainWindow::onTreeContextMenu);
    connect(m_tree, &QTreeView::expanded, this, &MainWindow::onTreeExpanded);  // lazy dirs

    m_leftTabs = new QTabWidget;
    m_leftTabs->setDocumentMode(true);   // flat tab pane, no rounded frame

    // Library tab — an "Add Directory" button (opens Settings on the Library tab,
    // where folders are managed) over the filesystem tree. Mirrors the Playlists tab.
    auto *libTab = new QWidget;
    auto *libLayout = new QVBoxLayout(libTab);
    libLayout->setContentsMargins(0, 0, 0, 0);
    libLayout->setSpacing(4);
    auto *libButtons = new QHBoxLayout;
    libButtons->setContentsMargins(6, 6, 6, 0);
    auto *addDirBtn = new QPushButton(tr("Add Directory"));
    addDirBtn->setToolTip(tr("Add a music folder (opens Settings)"));
    connect(addDirBtn, &QPushButton::clicked, this,
            [this] { openSettings(/*startOnLibrary=*/true); });
    libButtons->addWidget(addDirBtn);
    libButtons->addStretch(1);
    libLayout->addLayout(libButtons);
    libLayout->addWidget(m_tree, 1);
    m_leftTabs->addTab(libTab, tr("Library"));

    // Playlists tab — Create/Import buttons over the saved m3u8 list; double-click
    // a row to load+play, right-click to manage.
    m_playlistList = new QListWidget;
    m_playlistList->setFrameShape(QFrame::NoFrame);
    m_playlistList->setItemDelegate(new PlaylistItemDelegate(m_playlistList));
    m_playlistList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_playlistList, &QListWidget::itemActivated, this,
            [this](QListWidgetItem *item) {
                loadPlaylist(item->data(Qt::UserRole).toString());
            });
    connect(m_playlistList, &QListWidget::customContextMenuRequested, this,
            &MainWindow::onPlaylistContextMenu);

    auto *plButtons = new QHBoxLayout;
    plButtons->setContentsMargins(6, 6, 6, 0);
    auto *createBtn = new QPushButton(tr("Create"));
    auto *importBtn = new QPushButton(tr("Import"));
    auto *editBtn = new QPushButton(tr("Edit"));
    importBtn->setToolTip(tr("Import a track or playlist from a URL (yt-dlp)"));
    editBtn->setToolTip(tr("Open the playlist editor"));
    connect(createBtn, &QPushButton::clicked, this, &MainWindow::createPlaylist);
    connect(importBtn, &QPushButton::clicked, this, &MainWindow::importFromUrl);
    connect(editBtn, &QPushButton::clicked, this, [this] {
        // Open on the tab's current selection, if any.
        QListWidgetItem *sel = m_playlistList->currentItem();
        openPlaylistEditor(sel ? sel->data(Qt::UserRole).toString() : QString());
    });
    plButtons->addWidget(createBtn);
    plButtons->addWidget(importBtn);
    plButtons->addWidget(editBtn);
    plButtons->addStretch(1);

    auto *playlistTab = new QWidget;
    auto *plLayout = new QVBoxLayout(playlistTab);
    plLayout->setContentsMargins(0, 0, 0, 0);
    plLayout->setSpacing(4);
    plLayout->addLayout(plButtons);
    plLayout->addWidget(m_playlistList, 1);
    m_leftTabs->addTab(playlistTab, tr("Playlists"));
    refreshPlaylists();

    // Left panel: folder tabs on top, now-playing track info (art + tags) below.
    m_leftPanel = new QSplitter(Qt::Vertical);
    m_leftPanel->addWidget(m_leftTabs);
    m_leftPanel->addWidget(buildTrackInfoPanel());
    m_leftPanel->setStretchFactor(0, 1);
    m_leftPanel->setStretchFactor(1, 0);
    m_leftPanel->setSizes({360, 300});
    m_leftPanel->setChildrenCollapsible(false);

    // --- Splitter joining the two panels ---
    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->addWidget(m_leftPanel);
    m_splitter->addWidget(buildQueuePanel());
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({280, 680});
    m_splitter->setChildrenCollapsible(false);
    root->addWidget(m_splitter, /*stretch=*/1);

    // --- Full-width transport bar ---
    root->addWidget(buildTransportBar());

    // --- Bottom status bar: only shown during background work (e.g. syncing) ---
    m_status = new StatusStack;
    m_status->setObjectName("status");
    // Nudge the text right so it clears macOS's rounded bottom-left window corner.
    // Set in code (not just the Dark QSS) so it also applies under the native style.
    m_status->setContentsMargins(8, 0, 0, 0);
    m_status->hide();
    root->addWidget(m_status);

    // In-app toast overlay: a transparent child spanning the window. Registers
    // itself so ToastArea::post() works from anywhere (incl. worker threads).
    new ToastArea(this);

    // Theming is applied application-wide (see Theme::apply, called at startup
    // and from the settings dialog) so it can be swapped at runtime. The default
    // is System, i.e. the native KDE/platform theme.
}

QWidget *MainWindow::buildQueuePanel()
{
    // Proxy gives the table clickable-header sorting over the source model.
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setSortLocaleAware(true);
    m_proxy->setDynamicSortFilter(true);

    m_table = new QTableView;
    m_table->setModel(m_proxy);
    m_table->setFrameShape(QFrame::NoFrame);   // match the flat library panel
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(true);
    m_table->setWordWrap(false);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);
    QHeaderView *hh = m_table->horizontalHeader();
    hh->setHighlightSections(false);
    // Every column user-resizable (Stretch/ResizeToContents would lock the width
    // so only one column dragged). The last section stretches to fill the panel.
    hh->setSectionResizeMode(QHeaderView::Interactive);
    hh->setStretchLastSection(true);
    hh->resizeSection(PlaylistModel::Title,    220);
    hh->resizeSection(PlaylistModel::Artist,   150);
    hh->resizeSection(PlaylistModel::Album,    180);
    hh->resizeSection(PlaylistModel::Year,     56);
    hh->resizeSection(PlaylistModel::TrackNo,  44);
    hh->resizeSection(PlaylistModel::Duration, 64);
    m_table->sortByColumn(-1, Qt::AscendingOrder);   // default: keep DB/result order
    // Only doubleClicked — `activated` also fires on a double-click (and on Enter),
    // so connecting both ran the handler twice per double-click.
    connect(m_table, &QTableView::doubleClicked, this, &MainWindow::onTrackActivated);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::customContextMenuRequested,
            this, &MainWindow::onQueueContextMenu);

    // Search row: scope selector + debounced text field.
    m_scope = new QComboBox;
    m_scope->addItem(tr("All"));      // index maps to MusicLibrary::SearchScope
    m_scope->addItem(tr("Title"));
    m_scope->addItem(tr("Artist"));
    m_scope->addItem(tr("Album"));
    m_scope->addItem(tr("Year"));
    m_scope->setToolTip(tr("Search in"));
    m_scope->setMaximumWidth(52);

    m_searchEdit = new QLineEdit;
    m_searchEdit->setPlaceholderText(tr("Search Library…"));
    m_searchEdit->setToolTip(tr("Search entire library"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setMaximumWidth(320);   // cap so it doesn't span the whole panel

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(kSearchDebounceMs);
    connect(m_searchTimer, &QTimer::timeout, this, [this] {
        m_searching = true;
        emit requestSearch(m_searchEdit->text(), m_scope->currentIndex());
    });
    // The resumable queue cache is rewritten in full on every queue mutation. A
    // burst of single-track enqueues (or a "Play library" of tens of thousands)
    // would otherwise be O(n^2) synchronous m3u8 writes on the GUI thread; debounce
    // so only the settled queue is written once typing/clicking pauses.
    m_queueCacheTimer = new QTimer(this);
    m_queueCacheTimer->setSingleShot(true);
    m_queueCacheTimer->setInterval(kQueueCacheDebounceMs);
    connect(m_queueCacheTimer, &QTimer::timeout, this,
            [this] { m_store.saveQueueCache(queueStoredPaths()); });

    connect(m_searchEdit, &QLineEdit::textChanged, this, &MainWindow::scheduleSearch);
    connect(m_scope, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!m_searchEdit->text().trimmed().isEmpty())
            scheduleSearch();
    });

    m_queueTitle = new QLabel;
    m_queueTitle->setObjectName("queueTitle");

    // Queue-actions dropdown (Clear / Save / Save As / Append), rebuilt on show.
    m_queueMenu = new QMenu(this);
    connect(m_queueMenu, &QMenu::aboutToShow, this, &MainWindow::buildQueueMenu);
    m_queueMenuBtn = new QToolButton;
    m_queueMenuBtn->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    m_queueMenuBtn->setAutoRaise(true);
    m_queueMenuBtn->setToolTip(tr("Queue actions"));
    m_queueMenuBtn->setPopupMode(QToolButton::InstantPopup);
    m_queueMenuBtn->setMenu(m_queueMenu);

    updateQueueTitle();

    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(8, 6, 8, 4);
    searchRow->setSpacing(6);
    searchRow->addWidget(m_queueTitle);
    searchRow->addWidget(m_queueMenuBtn);
    searchRow->addStretch(1);   // push the search bits to the right
    searchRow->addWidget(m_scope);
    searchRow->addWidget(m_searchEdit);

    auto *tab = new QWidget;
    auto *v = new QVBoxLayout(tab);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addLayout(searchRow);
    v->addWidget(m_table, 1);
    return tab;
}

void MainWindow::scheduleSearch()
{
    if (m_searchEdit->text().trimmed().isEmpty()) {
        // Cleared: cancel any pending query and restore the full library.
        m_searchTimer->stop();
        if (m_searching) {
            m_searching = false;
            m_model->setTracks(m_controller->queue());   // back to the queue view
            if (m_controller->hasTrack())   // reveal the playing row on return
                highlightPlaying(m_controller->currentTrack().url, /*scroll=*/true);
        }
        return;
    }
    m_searchTimer->start();   // debounce: fire once typing pauses
}

QWidget *MainWindow::buildTrackInfoPanel()
{
    auto *panel = new QWidget;
    panel->setObjectName("infoPanel");
    auto *v = new QVBoxLayout(panel);
    v->setContentsMargins(14, 14, 14, 14);
    v->setSpacing(6);

    m_coverArt = new CoverLabel;
    m_coverArt->setObjectName("coverArt");
    m_coverArt->setAlignment(Qt::AlignCenter);
    m_coverArt->setMinimumHeight(80);
    m_coverArt->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_infoTitle = new QLabel;
    m_infoTitle->setObjectName("infoTitle");
    m_infoTitle->setAlignment(Qt::AlignCenter);
    m_infoTitle->setWordWrap(true);

    m_infoArtist = new QLabel;
    m_infoArtist->setObjectName("infoArtist");
    m_infoArtist->setAlignment(Qt::AlignCenter);
    m_infoArtist->setWordWrap(true);

    m_infoAlbum = new QLabel;
    m_infoAlbum->setObjectName("infoAlbum");
    m_infoAlbum->setAlignment(Qt::AlignCenter);
    m_infoAlbum->setWordWrap(true);

    v->addWidget(m_coverArt, /*stretch=*/1);
    v->addWidget(m_infoTitle);
    v->addWidget(m_infoArtist);
    v->addWidget(m_infoAlbum);
    return panel;
}

void MainWindow::showTrackInfo(const Track &t)
{
    if (t.url.isEmpty()) {
        m_infoTitle->clear();
        m_infoArtist->clear();
        m_infoAlbum->clear();
        loadCover(QString());
        return;
    }
    m_infoTitle->setText(t.title);
    m_infoArtist->setText(t.artist);
    QString album = t.album;
    if (t.year > 0)
        album += album.isEmpty() ? QString::number(t.year)
                                 : QStringLiteral(" · %1").arg(t.year);
    m_infoAlbum->setText(album);
    loadCover(t.artUrl);
}

void MainWindow::loadCover(const QString &artUrl)
{
    if (artUrl.isEmpty())
        m_coverArt->setCover(QPixmap());   // -> placeholder
    else
        m_coverArt->setCover(QPixmap(QUrl(artUrl).toLocalFile()));
}

QWidget *MainWindow::buildTransportBar()
{
    auto *bar = new QFrame;
    bar->setObjectName("transportBar");

    // Two rows: centred transport buttons over a full-width seek bar, with the
    // volume + settings cluster anchored to the right.
    //
    // macOS-native knob: under QMacStyle (System mode) these autoRaise buttons
    // still render with a rounded bezel (see notes/ native screenshots) — autoRaise
    // does not fully flatten QToolButtons on macOS. For borderless media controls
    // there, give the bar a System-mode-only QSS in Theme::apply, e.g.
    //   QFrame#transportBar QToolButton { border:0; background:transparent; }
    // The built-in Dark theme already flattens them (Theme buttonRadius token).
    auto makeBtn = [this](QStyle::StandardPixmap pm) {
        auto *b = new QToolButton;
        b->setIcon(style()->standardIcon(pm));
        b->setIconSize(QSize(22, 22));
        b->setMinimumSize(44, 44);
        b->setAutoRaise(true);
        return b;
    };

    m_shuffleBtn = new QToolButton;
    m_shuffleBtn->setIconSize(QSize(22, 22));
    m_shuffleBtn->setMinimumSize(44, 44);
    m_shuffleBtn->setAutoRaise(true);
    m_shuffleBtn->setCheckable(true);
    m_shuffleBtn->setToolTip(tr("Shuffle"));
    connect(m_shuffleBtn, &QToolButton::toggled, m_controller, &PlayerController::setShuffle);

    auto *prevBtn = makeBtn(QStyle::SP_MediaSkipBackward);
    connect(prevBtn, &QToolButton::clicked, m_controller, &PlayerController::previous);

    m_playPause = makeBtn(QStyle::SP_MediaPlay);
    m_playPause->setIconSize(QSize(28, 28));
    m_playPause->setMinimumSize(52, 52);
    connect(m_playPause, &QToolButton::clicked, this, [this] {
        if (m_controller->hasTrack())
            m_controller->togglePlayPause();
        else
            m_controller->play();   // resume/cold-start the queue (or ready library)
    });

    auto *nextBtn = makeBtn(QStyle::SP_MediaSkipForward);
    connect(nextBtn, &QToolButton::clicked, m_controller, &PlayerController::next);

    m_repeatBtn = new QToolButton;
    m_repeatBtn->setIconSize(QSize(22, 22));
    m_repeatBtn->setMinimumSize(44, 44);
    m_repeatBtn->setAutoRaise(true);
    m_repeatBtn->setCheckable(true);
    connect(m_repeatBtn, &QToolButton::clicked, this, &MainWindow::cycleRepeat);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(4);
    buttons->addWidget(m_shuffleBtn);
    buttons->addWidget(prevBtn);
    buttons->addWidget(m_playPause);
    buttons->addWidget(nextBtn);
    buttons->addWidget(m_repeatBtn);

    // Volume: a click-to-mute button whose icon tracks the level + the slider.
    m_volBtn = new QToolButton;
    m_volBtn->setIconSize(QSize(18, 18));
    m_volBtn->setMinimumSize(36, 36);
    m_volBtn->setAutoRaise(true);
    connect(m_volBtn, &QToolButton::clicked, this, [this] {
        if (m_volume->value() > 0) {
            m_preMuteVolume = m_volume->value();
            m_volume->setValue(0);                 // mute
        } else {
            m_volume->setValue(m_preMuteVolume > 0 ? m_preMuteVolume : 80);
        }
    });

    m_volume = new QSlider(Qt::Horizontal);
    m_volume->setRange(0, 100);
    m_volume->setValue(80);
    m_volume->setFixedWidth(110);
    connect(m_volume, &QSlider::valueChanged, this, [this](int v) {
        m_controller->setVolume(v / 100.0f);
        if (v > 0)
            m_preMuteVolume = v;   // so un-mute restores the last *audible* level
        updateVolumeIcon();
    });
    // Persist on release (or keyboard step), not on every drag tick.
    connect(m_volume, &QSlider::sliderReleased, this,
            [this] { QSettings().setValue("playback/volume", m_volume->value()); });

    m_settingsBtn = new QToolButton;
    m_settingsBtn->setIconSize(QSize(20, 20));
    m_settingsBtn->setMinimumSize(40, 40);
    m_settingsBtn->setAutoRaise(true);
    m_settingsBtn->setToolTip(tr("Settings"));
    connect(m_settingsBtn, &QToolButton::clicked, this, &MainWindow::openSettings);

    // Right cluster, pushed to the far right of its zone.
    auto *rightCluster = new QHBoxLayout;
    rightCluster->setSpacing(6);
    rightCluster->addStretch();
    rightCluster->addWidget(m_volBtn);
    rightCluster->addWidget(m_volume);
    rightCluster->addSpacing(8);
    rightCluster->addWidget(m_settingsBtn);

    // Controls row: equal-weight left/right zones keep the buttons window-centred.
    auto *controlsRow = new QHBoxLayout;
    controlsRow->setSpacing(0);
    controlsRow->addStretch(1);            // left zone (empty)
    controlsRow->addLayout(buttons, 0);    // centred transport buttons
    controlsRow->addLayout(rightCluster, 1);

    m_elapsed = new QLabel("0:00");
    m_total = new QLabel("0:00");
    m_seek = new QSlider(Qt::Horizontal);
    m_seek->setRange(0, 0);
    connect(m_seek, &QSlider::sliderPressed, this, [this] { m_userSeeking = true; });
    connect(m_seek, &QSlider::sliderReleased, this, [this] {
        m_userSeeking = false;
        m_controller->setPosition(m_seek->value());
    });
    connect(m_seek, &QSlider::sliderMoved, this, [this](int v) {
        m_elapsed->setText(formatTime(v));
    });
    auto *seekRow = new QHBoxLayout;
    seekRow->setSpacing(8);
    seekRow->addWidget(m_elapsed);
    seekRow->addWidget(m_seek, 1);
    seekRow->addWidget(m_total);

    auto *barV = new QVBoxLayout(bar);
    barV->setContentsMargins(14, 6, 14, 8);
    barV->setSpacing(2);
    barV->addLayout(controlsRow);
    barV->addLayout(seekRow);

    refreshThemedIcons();   // initial tint for shuffle/repeat/settings + volume icon
    return bar;
}

void MainWindow::rebuildTree()
{
    m_treeModel->clear();
    appendRemoteNode();   // "Remote" virtual root on top, when remote tracks exist
    const QIcon dirIcon = style()->standardIcon(QStyle::SP_DirIcon);

    for (const LibraryFolder &f : m_folders) {
        auto *node = new QStandardItem(dirIcon, f.label);
        node->setData(f.path, PathRole);
        node->setData(true, IsDirRole);
        node->setData(false, PopulatedRole);
        node->setToolTip(f.path);
        // A placeholder child gives the node an expand arrow; it's replaced by
        // the real contents the first time the node is expanded.
        if (QFileInfo(f.path).isDir())
            node->appendRow(new QStandardItem);
        m_treeModel->appendRow(node);
    }
}

void MainWindow::populateNode(QStandardItem *item)
{
    // Only filesystem nodes are lazily populated; virtual nodes (Remote subtree)
    // are built eagerly and carry no path, so leave them untouched.
    if (!item || item->data(PopulatedRole).toBool()
        || item->data(PathRole).toString().isEmpty())
        return;
    item->setData(true, PopulatedRole);
    item->removeRows(0, item->rowCount());   // drop the placeholder

    const QString path = item->data(PathRole).toString();
    const QIcon dirIcon = style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon fileIcon = style()->standardIcon(QStyle::SP_FileIcon);
    QDir dir(path);

    const auto subdirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                           QDir::Name | QDir::LocaleAware);
    for (const QFileInfo &sub : subdirs) {
        auto *child = new QStandardItem(dirIcon, sub.fileName());
        child->setData(sub.absoluteFilePath(), PathRole);
        child->setData(true, IsDirRole);
        child->setData(false, PopulatedRole);
        // Only show an expand arrow if the subdir actually has children (a nested
        // folder or an audio file). One extra dir read per subdir, on expansion.
        QDir sd(sub.absoluteFilePath());
        const bool hasChildren =
            !sd.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()
            || !sd.entryInfoList(audioGlobs(), QDir::Files).isEmpty();
        if (hasChildren)
            child->appendRow(new QStandardItem);   // placeholder; replaced lazily
        item->appendRow(child);
    }

    const auto files = dir.entryInfoList(audioGlobs(), QDir::Files,
                                         QDir::Name | QDir::LocaleAware);
    for (const QFileInfo &file : files) {
        auto *child = new QStandardItem(fileIcon, file.fileName());
        child->setData(file.absoluteFilePath(), PathRole);
        child->setData(false, IsDirRole);
        item->appendRow(child);
    }
}

void MainWindow::onTreeExpanded(const QModelIndex &index)
{
    populateNode(m_treeModel->itemFromIndex(index));
}

QStringList MainWindow::keysForIndex(const QModelIndex &index) const
{
    if (!index.isValid())
        return {};
    // Filesystem node (file or folder): resolve via its path — walks the folder.
    const QString path = index.data(PathRole).toString();
    if (!path.isEmpty())
        return audioPathsForPath(path);
    // Virtual leaf: its own key, normalised to the stored form (plain path for
    // local tracks, URL for remote) for clean playlist files.
    const QString key = index.data(KeyRole).toString();
    if (!key.isEmpty())
        return {storedForm(QUrl(key))};
    // Virtual group (artist/album/host/Remote): gather all descendant leaves.
    QStringList out;
    const QAbstractItemModel *m = index.model();
    const int rows = m->rowCount(index);
    for (int r = 0; r < rows; ++r)
        out += keysForIndex(m->index(r, 0, index));
    return out;
}

void MainWindow::appendRemoteNode()
{
    // "Remote" root: host → playlist title → track over the remote subset of the
    // library. Small (a handful of imports), so eager. The playlist title is the
    // track's album field (importer folds yt-dlp playlist_title into it).
    QList<const Track *> remotes;
    for (const Track &t : m_fullLibrary)
        if (t.isRemote())
            remotes.append(&t);
    if (remotes.isEmpty())
        return;

    const QIcon groupIcon = style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon fileIcon = style()->standardIcon(QStyle::SP_FileIcon);
    auto makeGroup = [&groupIcon](const QString &label) {
        auto *g = new QStandardItem(groupIcon, label);
        g->setData(true, IsGroupRole);
        g->setData(true, PopulatedRole);
        return g;
    };

    auto *root = makeGroup(tr("Remote"));
    // host -> playlist group items, created on first use.
    QHash<QString, QStandardItem *> hostItems, playlistItems;
    for (const Track *t : remotes) {
        const QString host = t->url.host().isEmpty() ? tr("Unknown") : t->url.host();
        const QString playlist = t->album.isEmpty() ? tr("Unknown") : t->album;

        QStandardItem *&hostItem = hostItems[host];
        if (!hostItem) { hostItem = makeGroup(host); root->appendRow(hostItem); }
        const QString plKey = host + '\x1f' + playlist;
        QStandardItem *&plItem = playlistItems[plKey];
        if (!plItem) { plItem = makeGroup(playlist); hostItem->appendRow(plItem); }

        auto *leaf = new QStandardItem(fileIcon, t->title);
        leaf->setData(t->key(), KeyRole);
        leaf->setToolTip(t->url.toString());
        plItem->appendRow(leaf);
    }
    m_treeModel->insertRow(0, root);   // always the top-most Library node
}

void MainWindow::refreshRemoteNode()
{
    // Replace just the Remote root, leaving the filesystem folder nodes (and their
    // expansion state) untouched.
    for (int r = 0; r < m_treeModel->rowCount(); ++r) {
        QStandardItem *it = m_treeModel->item(r);
        if (it && it->data(IsGroupRole).toBool()
            && it->data(PathRole).toString().isEmpty()) {
            m_treeModel->removeRow(r);
            break;
        }
    }
    appendRemoteNode();
}

int MainWindow::playRowForKey(const QUrl &url) const
{
    // O(1) via the model's key index — this is hit several times per track
    // change (highlight, art, metadata), so a linear scan over a 45k queue hurt.
    return m_model->rowForKey(url.toString(QUrl::FullyEncoded));
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // Hysteresis: enter compact below kNarrow, leave it only above kWide, so the
    // layout doesn't thrash when dragging the edge near the threshold.
    constexpr int kNarrow = 600;
    constexpr int kWide = 720;
    const int w = width();
    bool compact = m_compact;
    if (!m_compact && w < kNarrow)
        compact = true;
    else if (m_compact && w > kWide)
        compact = false;
    if (compact != m_compact)
        applyCompact(compact);
}

void MainWindow::applyCompact(bool compact)
{
    m_compact = compact;
    // Narrow: hide the whole left panel (folders + track info) and collapse the
    // table to a Title-led view.
    m_leftPanel->setVisible(!compact);
    for (int col : {PlaylistModel::Artist, PlaylistModel::Album, PlaylistModel::Year,
                    PlaylistModel::TrackNo, PlaylistModel::Duration})
        m_table->setColumnHidden(col, compact);
}

void MainWindow::openSettings(bool startOnLibrary)
{
    QSettings s;
    const bool curAutoSync = s.value("library/autoSync", false).toBool();
    const bool curRestoreQueue = s.value("ui/restoreQueue", true).toBool();
    const bool curAutoPlay = s.value("ui/autoPlay", false).toBool();
    // The explicit override only (empty = use managed/$PATH); showing the *resolved*
    // path here would let OK pin it and defeat the managed toggle.
    const QString curYtDlp = s.value("ytdlp/path").toString();
    const QByteArray curAudioDev = s.value("audio/outputDevice").toByteArray();
    const Theme::Mode curTheme =
        Theme::modeFromString(s.value("ui/theme", "system").toString());
    const QString curThemeFile = s.value("ui/themeFile").toString();

    SettingsDialog dlg(m_folders, curAutoSync, curRestoreQueue, curAutoPlay,
                       curYtDlp, curAudioDev, curTheme, curThemeFile, m_ytdlp, this);
    if (startOnLibrary)
        dlg.selectLibraryTab();   // "Add Directory" lands the user on the folders

    // "Sync now" reconciles the configured folders (mtime diff) without closing
    // the dialog; the scan runs on the worker and updates the status.
    connect(&dlg, &SettingsDialog::syncRequested, this, [this] {
        if (!m_folders.isEmpty())
            emit requestScan(libraryFolderPaths(m_folders));
    });

    // Live theme preview while the dialog is open.
    connect(&dlg, &SettingsDialog::themeChanged, this,
            [](Theme::Mode mode, const QString &file) { Theme::apply(mode, file); });

    // Live output-device switch (reverted below on Cancel).
    connect(&dlg, &SettingsDialog::audioDeviceChanged, this,
            [this](const QByteArray &id) { m_controller->setAudioDevice(id); });

#ifdef HAVE_DISCORD_RPC
    const bool curDiscordEnabled = s.value("discord/enabled", true).toBool();
    connect(&dlg, &SettingsDialog::discordEnabledChanged, this,
            [this](bool on) { if (m_discord) m_discord->setEnabled(on); });
#endif

    if (dlg.exec() != QDialog::Accepted) {
        Theme::apply(curTheme, curThemeFile);          // revert any preview
        m_controller->setAudioDevice(curAudioDev);     // revert live device switch
#ifdef HAVE_DISCORD_RPC
        if (m_discord) m_discord->setEnabled(curDiscordEnabled);
#endif
        return;
    }

    if (dlg.resetRequested()) {
        resetApplication();   // wipes everything and quits — don't persist below
        return;
    }

    s.setValue("library/autoSync", dlg.autoSync());    // placeholder for now
    s.setValue("ui/restoreQueue", dlg.restoreQueue());
    m_resumeQueue = dlg.restoreQueue();
    s.setValue("ui/autoPlay", dlg.autoPlay());
    m_autoPlay = dlg.autoPlay();
    s.setValue("ytdlp/path", dlg.ytDlpPath());
    s.setValue("audio/outputDevice", dlg.audioDeviceId());
    m_controller->setAudioDevice(dlg.audioDeviceId());
    // Prefer-HQ is persisted self-contained by the dialog; push the new value through.
    m_controller->setPreferHq(QSettings().value("playback/preferHq", 0).toInt());
    s.setValue("ui/theme", Theme::modeToString(dlg.themeMode()));
    s.setValue("ui/themeFile", dlg.themeFile());
    Theme::apply(dlg.themeMode(), dlg.themeFile());

    const QList<LibraryFolder> chosen = dlg.folders();
    if (libraryFolderPaths(chosen) != libraryFolderPaths(m_folders)
        || folderLabels(chosen) != folderLabels(m_folders)) {
        const bool pathsChanged =
            libraryFolderPaths(chosen) != libraryFolderPaths(m_folders);
        m_folders = chosen;
        saveLibraryFolders(m_folders);
        rebuildTree();                       // labels and/or paths changed
        if (pathsChanged)
            emit requestScan(libraryFolderPaths(m_folders));
    }
}

void MainWindow::resetApplication()
{
    // Clean slate: erase every bit of persisted state, then quit so the next launch
    // starts fresh. Confirmation already happened in the dialog.
    qInfo("[reset] erasing library, playlists, art and settings; quitting");
    m_resetting = true;   // suppress the save-on-quit handlers (see saveUiState /
                          // flushQueueCache) so they don't recreate what we delete

    // Close the DB first: on Windows the file can't be removed while the SQLite
    // connection holds it open. Stopping the worker thread runs ~MusicLibrary, which
    // closes and removes the connection.
    if (m_libThread) {
        m_library->cancel();
        m_libThread->quit();
        m_libThread->wait();
        m_libThread = nullptr;   // already torn down; skip the repeat in ~MainWindow
    }

    // All data (library.db, art/, playlists/, queue.m3u8) lives under AppData; wipe
    // the whole tree, then clear the QSettings store.
    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!dataDir.isEmpty())
        QDir(dataDir).removeRecursively();
    QSettings s;
    s.clear();
    s.sync();

    qApp->quit();
}

void MainWindow::onLibraryLoaded(const QList<Track> &tracks)
{
    m_fullLibrary = tracks;        // browse source for the tree/search/enqueue
    m_libIndexByKey.clear();       // rebuild the key->index lookup for tracksForKeys
    m_libIndexByKey.reserve(m_fullLibrary.size());
    for (int i = 0; i < m_fullLibrary.size(); ++i)
        m_libIndexByKey.insert(m_fullLibrary.at(i).key(), i);
    refreshPlaylists();            // now that durations can be resolved
    refreshRemoteNode();           // Remote root in the Library tree

    // Resume the cached queue once the library is available to resolve metadata.
    // Loaded silently (no autoplay) under the guard so it isn't flagged dirty.
    if (!m_pendingResume.isEmpty()) {
        const QList<Track> resumed = tracksForKeys(m_pendingResume);
        m_pendingResume.clear();
        if (!resumed.isEmpty()) {
            m_loadingPlaylist = true;
            m_controller->enqueue(resumed);
            m_loadingPlaylist = false;
            updateQueueTitle();
        }
    }

    seedReadyQueue();              // cold Play falls back to the whole library

    // Auto-play on launch: start the restored queue (or the library as a
    // fallback), honouring shuffle so the first track is random when it's on.
    if (m_autoPlayPending && !m_controller->hasTrack()) {
        m_autoPlayPending = false;
        startAutoPlay();
    }

    if (m_controller->hasTrack())
        return;   // a track is playing; don't clobber the info panel
    if (tracks.isEmpty() && m_folders.isEmpty())
        m_infoTitle->setText(tr("Open settings to add a music folder"));
    else if (tracks.isEmpty())
        m_infoTitle->setText(tr("No audio found in this folder"));
    else
        m_infoTitle->setText(tr("Nothing playing"));
}

void MainWindow::onTracksAppended(const QList<Track> &tracks)
{
    const int first = m_fullLibrary.size();
    m_fullLibrary.append(tracks);   // grow the browse source; queue view untouched
    for (int i = first; i < m_fullLibrary.size(); ++i)   // keep the index in sync
        m_libIndexByKey.insert(m_fullLibrary.at(i).key(), i);
}

void MainWindow::onSearchResults(const QString &query, const QList<Track> &tracks)
{
    // Drop stale/late results: only apply if they match the current query.
    if (!m_searching || query != m_searchEdit->text())
        return;
    m_model->setTracks(tracks);   // browse mode: double-click a result to enqueue
}

void MainWindow::onScanProgress(int done, int total, const QString &sourceLabel,
                                const QString &fileName)
{
    if (total <= 0)
        return;
    const QString src = sourceLabel.isEmpty() ? tr("library") : sourceLabel;
    m_status->post(StatusStack::Scan, tr("Importing from %1 (%L2/%L3): %4")
                                          .arg(src).arg(done).arg(total).arg(fileName));
}

void MainWindow::onScanStatus(const QString &message)
{
    m_status->post(StatusStack::Scan, message);
    // Scripted-benchmark hook: quit cleanly after a scan so logs flush.
    // (Timing print itself is PP_SCAN_BENCH and does not quit — that lets
    // you run normally and watch parse timing while tuning the real library.)
    if (message.isEmpty() && qEnvironmentVariableIsSet("PP_SCAN_QUIT"))
        qApp->quit();
}

void MainWindow::restoreSettings()
{
    QSettings s;

    const int vol = s.value("playback/volume", 80).toInt();
    m_volume->setValue(vol);                       // emits valueChanged -> sets volume

    // Push the saved output device to the engine; empty (the default) means follow
    // the OS. The engine caches it and applies once its QAudioOutput is up.
    m_controller->setAudioDevice(s.value("audio/outputDevice").toByteArray());
    m_controller->setPreferHq(s.value("playback/preferHq", 0).toInt());

    const bool shuffle = s.value("playback/shuffle", false).toBool();
    m_shuffleBtn->setChecked(shuffle);             // emits toggled -> controller

    m_repeatState = s.value("playback/repeat", 0).toInt();
    m_controller->setRepeatMode(static_cast<PlayerController::RepeatMode>(m_repeatState));
    updateRepeatButton();

    // Resumable queue: load the cached paths now, restore the header identity, and
    // resolve to Tracks on the first libraryLoaded (the library isn't loaded yet).
    m_resumeQueue = s.value("ui/restoreQueue", true).toBool();
    if (m_resumeQueue) {
        m_pendingResume = m_store.readQueueCache();
        m_currentPlaylist = s.value("ui/currentPlaylist").toString();
        m_queueDirty = s.value("ui/queueDirty", false).toBool();
    }
    m_autoPlay = s.value("ui/autoPlay", false).toBool();
    m_autoPlayPending = m_autoPlay;   // consumed once the library is available
    updateQueueTitle();

    m_folders = loadLibraryFolders();
    rebuildTree();
    // Always kick the worker: it emits the cached library instantly, then
    // reconciles against disk. No folders just yields an empty library.
    emit requestScan(libraryFolderPaths(m_folders));

    restoreUiState();   // window geometry, splitter sizes, column widths
}

void MainWindow::saveUiState() const
{
    if (m_resetting)
        return;   // a reset just cleared QSettings — don't write keys back
    QSettings s;
    s.setValue("ui/geometry", saveGeometry());
    s.setValue("ui/splitter", m_splitter->saveState());
    s.setValue("ui/leftPanel", m_leftPanel->saveState());
    s.setValue("ui/tableHeader", m_table->horizontalHeader()->saveState());
}

void MainWindow::restoreUiState()
{
    QSettings s;
    // Each is optional: fall back to the layout defaults set in buildUi() when a
    // key is absent (first run, or settings cleared).
    if (s.contains("ui/geometry"))
        restoreGeometry(s.value("ui/geometry").toByteArray());
    if (s.contains("ui/splitter"))
        m_splitter->restoreState(s.value("ui/splitter").toByteArray());
    if (s.contains("ui/leftPanel"))
        m_leftPanel->restoreState(s.value("ui/leftPanel").toByteArray());
    if (s.contains("ui/tableHeader")) {
        QHeaderView *hh = m_table->horizontalHeader();
        hh->restoreState(s.value("ui/tableHeader").toByteArray());
        // restoreState() also restores per-section resize *modes*, which would
        // bring back an old non-resizable (Stretch) config. Re-assert Interactive
        // so every column stays user-draggable; the restored widths are kept.
        hh->setSectionResizeMode(QHeaderView::Interactive);
        hh->setStretchLastSection(true);
    }
}

void MainWindow::cycleRepeat()
{
    // None -> All -> One -> None. The repeatModeChanged handler updates the
    // button, persists, and refreshes m_repeatState.
    const int next = (m_repeatState + 1) % 3;
    m_controller->setRepeatMode(static_cast<PlayerController::RepeatMode>(next));
}

void MainWindow::updateRepeatButton()
{
    switch (m_repeatState) {
    case 1:
        m_repeatBtn->setIcon(themedIcon(":/icons/repeat.svg"));
        m_repeatBtn->setToolTip(tr("Repeat: all"));
        m_repeatBtn->setChecked(true);
        break;
    case 2:
        m_repeatBtn->setIcon(themedIcon(":/icons/repeat-one.svg"));
        m_repeatBtn->setToolTip(tr("Repeat: one"));
        m_repeatBtn->setChecked(true);
        break;
    default:
        m_repeatBtn->setIcon(themedIcon(":/icons/repeat.svg"));
        m_repeatBtn->setToolTip(tr("Repeat: off"));
        m_repeatBtn->setChecked(false);
        break;
    }
}

QIcon MainWindow::themedIcon(const QString &resource) const
{
    // Our custom SVGs are monochrome black; recolour them to the current text
    // colour so they read on whatever theme/stylesheet is active.
    const QColor c = palette().color(QPalette::WindowText);
    QIcon src(resource);
    QPixmap pm = src.pixmap(64);
    if (pm.isNull())
        return src;
    QPixmap out(pm.size());
    out.setDevicePixelRatio(pm.devicePixelRatio());
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.drawPixmap(0, 0, pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(out.rect(), c);
    p.end();
    return QIcon(out);
}

void MainWindow::updateVolumeIcon()
{
    const int v = m_volume->value();
    QString name;
    if (v == 0)        name = QStringLiteral("audio-volume-muted");
    else if (v < 34)   name = QStringLiteral("audio-volume-low");
    else if (v < 67)   name = QStringLiteral("audio-volume-medium");
    else               name = QStringLiteral("audio-volume-high");

    QIcon icon = QIcon::fromTheme(name);   // KDE/native icons follow the colour scheme
    if (icon.isNull())                     // fallback for non-icon-theme platforms
        icon = style()->standardIcon(v == 0 ? QStyle::SP_MediaVolumeMuted
                                            : QStyle::SP_MediaVolume);
    m_volBtn->setIcon(icon);
    m_volBtn->setToolTip(v == 0 ? tr("Unmute") : tr("Mute"));
}

void MainWindow::refreshThemedIcons()
{
    if (!m_shuffleBtn)
        return;   // called before the transport bar is built
    m_shuffleBtn->setIcon(themedIcon(":/icons/shuffle.svg"));
    m_settingsBtn->setIcon(themedIcon(":/icons/settings.svg"));
    updateRepeatButton();   // re-tints the repeat icon
    updateVolumeIcon();
}

void MainWindow::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        refreshThemedIcons();
}

void MainWindow::seedReadyQueue()
{
    // So a cold media-key / MPRIS / transport Play (before anything is queued) has
    // something to start: fall back to the whole library. setReadyQueue is silent
    // (no queueChanged), so the Queue view stays empty until something actually
    // plays or is enqueued. No-op if a track is already playing.
    if (!m_controller->hasTrack())
        m_controller->setReadyQueue(m_fullLibrary);
}

void MainWindow::startAutoPlay()
{
    // Prefer the restored queue; fall back to the whole library if nothing was
    // resumed. With shuffle on, lead with a random track rather than index 0.
    const bool haveQueue = m_controller->queueSize() > 0;
    const QList<Track> &tracks = haveQueue ? m_controller->queue() : m_fullLibrary;
    if (tracks.isEmpty())
        return;
    const int start = m_controller->shuffle()
                          ? QRandomGenerator::global()->bounded(tracks.size())
                          : 0;
    if (haveQueue)
        m_controller->jumpTo(start);            // play the existing queue in place
    else
        m_controller->playQueue(m_fullLibrary, start);   // adopt the library
}

void MainWindow::highlightPlaying(const QUrl &url, bool scroll)
{
    const int row = playRowForKey(url);
    if (row < 0)
        return;   // not in the current view (e.g. filtered out)
    const QModelIndex proxyIdx = m_proxy->mapFromSource(m_model->index(row, 0));
    if (!proxyIdx.isValid())
        return;
    m_table->setCurrentIndex(proxyIdx);   // selection only; does not scroll
    if (scroll)
        m_table->scrollTo(proxyIdx, QAbstractItemView::EnsureVisible);
}

void MainWindow::onTrackActivated(const QModelIndex &index)
{
    if (!index.isValid())
        return;
    const int srcRow = m_proxy->mapToSource(index).row();
    if (m_searching) {
        // Browse mode: search results are the library, not the queue. Play the
        // track in place if it's already queued; otherwise append it and play.
        const Track &t = m_model->at(srcRow);
        const QList<Track> &queue = m_controller->queue();
        int existing = -1;
        for (int i = 0; i < queue.size(); ++i)
            if (queue.at(i).url == t.url) { existing = i; break; }
        if (existing < 0) {
            m_controller->enqueue({t});
            existing = m_controller->queueSize() - 1;
        }
        m_controller->jumpTo(existing);
    } else {
        // Queue mode: the source row is the queue index — play from there.
        m_controller->jumpTo(srcRow);
    }
}

void MainWindow::onTreeClicked(const QModelIndex &index)
{
    // Single click on a group node toggles its expansion (no need to hit the arrow).
    auto *tree = qobject_cast<QTreeView *>(sender());
    if (tree && index.isValid() && !isLeafNode(index))
        tree->setExpanded(index, !tree->isExpanded(index));
}

void MainWindow::onTreeActivated(const QModelIndex &index)
{
    // Double-click a leaf to enqueue it. Groups only expand (single-click); their
    // "Add to queue" / "Play now" live in the right-click menu.
    if (!index.isValid() || !isLeafNode(index))
        return;
    const QList<Track> add = tracksForKeys(keysForIndex(index));
    if (add.isEmpty())
        return;
    // Nothing queued yet: start playing it rather than silently filling the queue.
    if (m_controller->queueSize() == 0)
        playNow(add);
    else
        m_controller->enqueue(add);
}

void MainWindow::onTreeContextMenu(const QPoint &pos)
{
    auto *tree = qobject_cast<QTreeView *>(sender());
    if (!tree)
        return;
    const QModelIndex index = tree->indexAt(pos);
    if (!index.isValid())
        return;
    // Resolve keys lazily on click — gathering a big group's descendants (or
    // walking a folder) must not stall the menu popup. Persistent so it stays
    // valid for the menu's lifetime.
    const QPersistentModelIndex pidx(index);
    auto keys = [this, pidx] { return pidx.isValid() ? keysForIndex(pidx) : QStringList{}; };

    QMenu menu(this);
    if (isLeafNode(index)) {
        // A single leaf — resolve it now (cheap) so the menu can label and reveal it.
        const QList<Track> leaf = tracksForKeys(keys());
        if (!leaf.isEmpty()) {
            const Track track = leaf.first();
            connect(menu.addAction(tr("Show info")), &QAction::triggered, this,
                    [this, track] { showTrackDetails(track); });
            connect(menu.addAction(track.isRemote() ? tr("Open in browser")
                                                    : tr("Open directory")),
                    &QAction::triggered, this,
                    [this, track] { openTrackLocation(track); });
            // Remote tracks can be removed from the library (local rows are owned by
            // the folder scan, so they're left to the source settings instead).
            if (track.isRemote()) {
                menu.addSeparator();
                connect(menu.addAction(tr("Remove")), &QAction::triggered, this,
                        [this, keys] {
                            const QStringList uris = keys();
                            if (!uris.isEmpty())
                                emit removeTracks(uris);
                        });
            }
        }
    } else {
        // Group nodes (folders, library-roots, artist/album/host): play or queue all.
        connect(menu.addAction(tr("Play now")), &QAction::triggered, this, [this, keys] {
            playNow(tracksForKeys(keys()));
        });
        connect(menu.addAction(tr("Add to queue")), &QAction::triggered, this, [this, keys] {
            const QList<Track> tracks = tracksForKeys(keys());
            if (!tracks.isEmpty())
                m_controller->enqueue(tracks);
        });

        // Reveal the group. Local folder nodes carry their directory in PathRole and
        // open directly; remote group nodes (Remote/host/playlist) open the first
        // contained track's page URL in the browser.
        const QString dir = index.data(PathRole).toString();
        if (!dir.isEmpty()) {
            connect(menu.addAction(tr("Open directory")), &QAction::triggered, this,
                    [dir] { QDesktopServices::openUrl(QUrl::fromLocalFile(dir)); });
        } else if (index.data(IsGroupRole).toBool()) {
            connect(menu.addAction(tr("Open in browser")), &QAction::triggered, this,
                    [this, keys] {
                        const QList<Track> tracks = tracksForKeys(keys());
                        if (!tracks.isEmpty())
                            openTrackLocation(tracks.first());
                    });
            // A remote group (Remote / host / artist / playlist): remove all its
            // tracks. Confirm first since a group can span many tracks.
            menu.addSeparator();
            connect(menu.addAction(tr("Remove")), &QAction::triggered, this, [this, keys] {
                const QStringList uris = keys();
                if (uris.isEmpty())
                    return;
                if (QMessageBox::question(this, tr("Remove"),
                        tr("Remove %n remote track(s) from the library?", nullptr,
                           int(uris.size()))) == QMessageBox::Yes)
                    emit removeTracks(uris);
            });
        }
    }
    addToPlaylistMenu(&menu, keys);
    menu.exec(tree->viewport()->mapToGlobal(pos));
}

void MainWindow::onQueueContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_table->indexAt(pos);
    if (!index.isValid())
        return;
    const int srcRow = m_proxy->mapToSource(index).row();
    if (srcRow < 0 || srcRow >= m_model->count())
        return;
    const Track track = m_model->at(srcRow);

    QMenu menu(this);
    connect(menu.addAction(tr("Show info")), &QAction::triggered, this,
            [this, track] { showTrackDetails(track); });
    connect(menu.addAction(track.isRemote() ? tr("Open in browser")
                                            : tr("Open directory")),
            &QAction::triggered, this, [this, track] { openTrackLocation(track); });
    addToPlaylistMenu(&menu, [track] {
        return QStringList{storedForm(track.url)};
    });
    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

void MainWindow::openTrackLocation(const Track &track)
{
    if (track.isRemote()) {
        QDesktopServices::openUrl(track.url);   // page URL in the default browser
        return;
    }
    // Local: hand the file manager the containing folder. (Reveal-and-select is
    // platform-specific; opening the directory is the portable common denominator.)
    const QString dir = QFileInfo(track.url.toLocalFile()).absolutePath();
    if (!dir.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::showTrackDetails(const Track &track)
{
    // Collect the present fields, then render them as one aligned monospace block
    // — robust to long paths/URLs (scrolls instead of clipping) and selectable.
    QList<QPair<QString, QString>> rows;
    auto add = [&](const QString &label, const QString &value) {
        if (!value.isEmpty())
            rows.append({label, value});
    };
    add(tr("Title"), track.title);
    add(tr("Artist"), track.artist);
    add(tr("Album"), track.album);
    if (track.year > 0)    add(tr("Year"), QString::number(track.year));
    if (track.trackNo > 0) add(tr("Track"), QString::number(track.trackNo));
    if (track.durationMs > 0) add(tr("Duration"), formatTime(track.durationMs));
    add(track.isRemote() ? tr("URL") : tr("File"), storedForm(track.url));
    if (!track.artUrl.isEmpty())
        add(tr("Artwork"), QUrl(track.artUrl).toLocalFile());

    int width = 0;
    for (const auto &r : rows)
        width = qMax(width, r.first.size());
    QString text;
    for (const auto &r : rows)
        text += QStringLiteral("%1  %2\n").arg(r.first + ':', -(width + 1)).arg(r.second);

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Track info"));
    auto *v = new QVBoxLayout(&dlg);

    auto *view = new QPlainTextEdit(text.trimmed());
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);   // long paths scroll, never clip
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    v->addWidget(view);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(buttons);

    dlg.resize(560, 320);
    dlg.exec();
}

QStringList MainWindow::audioPathsForPath(const QString &path) const
{
    // Local audio file paths for a tree node: the file itself, or every audio
    // file beneath a folder (recursive), name-sorted. No Track/metadata
    // resolution — cheap enough to defer to an action click (e.g. Add to
    // playlist) rather than block the context-menu popup on a large source.
    if (path.isEmpty())
        return {};
    if (!QFileInfo(path).isDir())
        return {path};

    QStringList files;
    QDirIterator it(path, audioGlobs(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files.append(it.next());
    files.sort(Qt::CaseInsensitive);
    return files;
}

QList<Track> MainWindow::tracksForKeys(const QStringList &keys) const
{
    // Resolve each stored line (local path or URL) to its library Track (full
    // metadata), falling back to a minimal entry for tracks not in the library.
    // Uses the cached key->index map (rebuilt on libraryLoaded) so a 45k-row hash
    // isn't rebuilt on every playlist load / tree enqueue.
    QList<Track> out;
    out.reserve(keys.size());
    for (const QString &line : keys) {
        if (line.isEmpty())
            continue;
        const QUrl url = urlFromStored(line);
        const int idx = m_libIndexByKey.value(url.toString(QUrl::FullyEncoded), -1);
        if (idx >= 0) {
            out.append(m_fullLibrary.at(idx));
        } else {
            Track minimal;
            minimal.url = url;
            minimal.title = url.isLocalFile() ? QFileInfo(url.toLocalFile()).fileName()
                                              : url.toString();
            out.append(minimal);
        }
    }
    return out;
}

void MainWindow::updateQueueTitle()
{
    // Header is the current playlist's identity: its name (or "Queue" when no
    // playlist is loaded), a [modified] marker when the queue has diverged, and
    // the count. QLocale groups digits per the user's locale (e.g. "1,000").
    static const QLocale locale;
    const QString base = m_currentPlaylist.isEmpty() ? tr("Queue") : m_currentPlaylist;
    const QString modified = m_queueDirty ? tr(" [modified]") : QString();
    const qint64 total = totalDurationMs(m_controller->queue());
    m_queueTitle->setText(QStringLiteral("%1%2 (%3 - %4)").arg(
        base, modified, locale.toString(m_controller->queueSize()),
        formatDurationLong(total)));
}

QStringList MainWindow::queueStoredPaths() const
{
    QStringList paths;
    const QList<Track> &q = m_controller->queue();
    paths.reserve(q.size());
    for (const Track &t : q)
        paths << storedForm(t.url);   // local path for local tracks, URL for remote
    return paths;
}

void MainWindow::setCurrentPlaylist(const QString &name, bool dirty)
{
    m_currentPlaylist = name;
    m_queueDirty = dirty;
    QSettings s;
    s.setValue("ui/currentPlaylist", name);
    s.setValue("ui/queueDirty", dirty);
    updateQueueTitle();
}

void MainWindow::refreshPlaylists()
{
    if (!m_playlistList)
        return;
    // Durations live in the library; resolve each playlist entry through the
    // existing key->index map (kept current on libraryLoaded) rather than build a
    // second full-library hash every refresh. Entries not in the library still
    // count toward the track total but contribute 0 duration.
    m_playlistList->clear();
    for (const QString &name : m_store.names()) {
        const QStringList paths = m_store.readPaths(name);
        qint64 total = 0;
        for (const QString &p : paths) {
            const int idx = m_libIndexByKey.value(
                urlFromStored(p).toString(QUrl::FullyEncoded), -1);
            if (idx >= 0)
                total += m_fullLibrary.at(idx).durationMs;
        }
        auto *item = new QListWidgetItem(name);   // delegate draws name + meta
        item->setData(Qt::UserRole, name);   // the bare name, for load/rename/delete
        item->setData(PlaylistMetaRole,
                      tr("(%1 - %2)").arg(paths.size()).arg(formatDurationLong(total)));
        m_playlistList->addItem(item);
    }
}

void MainWindow::loadPlaylist(const QString &name)
{
    const QList<Track> tracks = tracksForKeys(m_store.readPaths(name));
    if (tracks.isEmpty())
        return;
    m_loadingPlaylist = true;
    m_controller->playQueue(tracks, 0);   // replace the queue and start playing
    m_loadingPlaylist = false;
    setCurrentPlaylist(name, /*dirty=*/false);
}

void MainWindow::playNow(const QList<Track> &tracks)
{
    if (tracks.isEmpty())
        return;
    // "Play now" starts a fresh, unsaved queue — it must not edit whatever
    // playlist happens to be loaded. With shuffle on, the first track is random
    // (otherwise we'd always lead with the same song).
    const int start = m_controller->shuffle()
                          ? QRandomGenerator::global()->bounded(tracks.size())
                          : 0;
    m_loadingPlaylist = true;     // suppress the dirty flag from playQueue's signal
    m_controller->playQueue(tracks, start);
    m_loadingPlaylist = false;
    setCurrentPlaylist(QString(), /*dirty=*/false);   // close any active playlist
}

void MainWindow::buildQueueMenu()
{
    m_queueMenu->clear();
    const bool hasQueue = m_controller->queueSize() > 0;
    const QStringList playlists = m_store.names();

    QAction *clear = m_queueMenu->addAction(tr("Clear"));
    clear->setEnabled(hasQueue);
    connect(clear, &QAction::triggered, this, [this] {
        m_controller->clearQueue();
        setCurrentPlaylist(QString(), /*dirty=*/false);   // close any active playlist
    });

    QAction *save = m_queueMenu->addAction(tr("Save"));
    save->setEnabled(hasQueue && m_queueDirty && !m_currentPlaylist.isEmpty());
    connect(save, &QAction::triggered, this, [this] {
        if (m_store.write(m_currentPlaylist, queueStoredPaths())) {
            setCurrentPlaylist(m_currentPlaylist, /*dirty=*/false);
            refreshPlaylists();
        }
    });

    QAction *saveAs = m_queueMenu->addAction(tr("Save As…"));
    saveAs->setEnabled(hasQueue);
    connect(saveAs, &QAction::triggered, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("Save playlist"), tr("Playlist name:"),
            QLineEdit::Normal, m_currentPlaylist, &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        if (m_store.exists(name)
            && QMessageBox::question(this, tr("Overwrite playlist?"),
                   tr("Playlist \"%1\" already exists. Overwrite it?").arg(name))
                   != QMessageBox::Yes)
            return;
        if (m_store.write(name, queueStoredPaths())) {
            setCurrentPlaylist(name, /*dirty=*/false);
            refreshPlaylists();
        }
    });

    QMenu *append = m_queueMenu->addMenu(tr("Append to"));
    append->setEnabled(hasQueue && !playlists.isEmpty());
    for (const QString &name : playlists)
        connect(append->addAction(name), &QAction::triggered, this,
                [this, name] { m_store.append(name, queueStoredPaths()); });
}

void MainWindow::addToPlaylistMenu(QMenu *menu, std::function<QStringList()> paths)
{
    // `paths` is resolved lazily, only when an action fires — building the menu
    // for a large library source must stay instant (no recursive disk walk here).
    QMenu *sub = menu->addMenu(tr("Add to playlist"));
    connect(sub->addAction(tr("New playlist…")), &QAction::triggered, this,
            [this, paths] {
                const QStringList p = paths();
                if (p.isEmpty())
                    return;
                bool ok = false;
                const QString name = QInputDialog::getText(
                    this, tr("New playlist"), tr("Playlist name:"),
                    QLineEdit::Normal, QString(), &ok).trimmed();
                if (!ok || name.isEmpty())
                    return;
                m_store.exists(name) ? m_store.append(name, p)
                                     : m_store.write(name, p);
                refreshPlaylists();
            });
    const QStringList names = m_store.names();
    if (!names.isEmpty())
        sub->addSeparator();
    for (const QString &name : names)
        connect(sub->addAction(name), &QAction::triggered, this,
                [this, name, paths] {
                    const QStringList p = paths();
                    if (!p.isEmpty())
                        m_store.append(name, p);
                });
}

void MainWindow::onPlaylistContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_playlistList->itemAt(pos);
    if (!item)
        return;
    const QString name = item->data(Qt::UserRole).toString();

    QMenu menu(this);
    connect(menu.addAction(tr("Play")), &QAction::triggered, this,
            [this, name] { loadPlaylist(name); });
    connect(menu.addAction(tr("Edit…")), &QAction::triggered, this,
            [this, name] { openPlaylistEditor(name); });
    connect(menu.addAction(tr("Rename…")), &QAction::triggered, this, [this, name] {
        bool ok = false;
        const QString to = QInputDialog::getText(
            this, tr("Rename playlist"), tr("New name:"),
            QLineEdit::Normal, name, &ok).trimmed();
        if (ok && !to.isEmpty() && to != name) {
            m_store.rename(name, to);
            if (m_currentPlaylist == name)
                setCurrentPlaylist(to, m_queueDirty);
            refreshPlaylists();
        }
    });
    connect(menu.addAction(tr("Delete")), &QAction::triggered, this, [this, name] {
        if (QMessageBox::question(this, tr("Delete playlist"),
                tr("Delete playlist \"%1\"?").arg(name)) == QMessageBox::Yes) {
            m_store.remove(name);
            if (m_currentPlaylist == name)
                setCurrentPlaylist(QString(), m_queueDirty);
            refreshPlaylists();
        }
    });
    menu.exec(m_playlistList->viewport()->mapToGlobal(pos));
}

void MainWindow::openPlaylistEditor(const QString &preselect)
{
    // The editor works in full-metadata Tracks; hand it the library snapshot and the
    // existing path->Track resolver so it doesn't re-query the DB. On save it writes
    // the m3u8 directly via the store, then signals us to refresh the tab list.
    PlaylistEditorDialog dlg(
        m_fullLibrary, &m_store,
        [this](const QStringList &keys) { return tracksForKeys(keys); },
        preselect, this);
    connect(&dlg, &PlaylistEditorDialog::playlistsChanged, this, [this] {
        refreshPlaylists();
    });
    dlg.exec();
}

void MainWindow::createPlaylist()
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("New playlist"), tr("Playlist name:"),
        QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    if (m_store.exists(name)) {
        QMessageBox::information(this, tr("Playlist exists"),
            tr("A playlist named \"%1\" already exists.").arg(name));
        return;
    }
    m_store.write(name, {});   // empty playlist; populate via "Add to playlist"
    refreshPlaylists();
}

void MainWindow::importFromUrl()
{
    QSettings s;
    ImportDialog dlg(m_store.names(),
                     s.value("import/createPlaylist", false).toBool(),
                     s.value("import/appendTo").toString(), this);
    if (dlg.exec() != QDialog::Accepted || dlg.url().isEmpty())
        return;

    s.setValue("import/createPlaylist", dlg.createPlaylist());
    s.setValue("import/appendTo", dlg.appendPlaylist());
    // Imports queue (FIFO): if one is already running, this starts when it finishes.
    // The dialog's Check already enumerated the entries — hand them over so the importer
    // skips a second flat-playlist pass.
    const bool wasBusy = m_importer->busy();
    m_importer->start(dlg.url(), dlg.entries(), dlg.createPlaylist(), dlg.appendPlaylist(),
                      dlg.playlistTitle());
    if (wasBusy)
        ToastArea::post(tr("Import queued — it'll start when the current one finishes."));
}

void MainWindow::onQueueChanged(const QList<Track> &queue)
{
    // A user-driven queue change diverges it from the saved playlist; a deliberate
    // load (Playlists tab / resume) sets m_loadingPlaylist so it stays clean.
    if (!m_loadingPlaylist) {
        m_queueDirty = true;
        QSettings().setValue("ui/queueDirty", m_queueDirty);
    }
    updateQueueTitle();
    m_queueCacheTimer->start();   // debounced; flushed on quit (see flushQueueCache)

    if (m_searching)
        return;   // search browse mode owns the table; restored when search clears
    m_model->setTracks(queue);
    if (m_controller->hasTrack())
        highlightPlaying(m_controller->currentTrack().url, /*scroll=*/false);
}

void MainWindow::onCurrentTrackChanged(const Track &track)
{
    if (track.url.isEmpty()) {
        setWindowTitle(tr("Pocket Player"));
        showTrackInfo(track);
        m_infoTitle->setText(tr("Nothing playing"));
        return;
    }
    const QString display = track.displayText();
    setWindowTitle(display);   // now-playing lives in the window title
    showTrackInfo(track);      // album art + tags in the left info panel
    // Scroll the table to reveal the row only when the track genuinely changes —
    // not on art/metadata refreshes, which re-emit currentTrackChanged.
    const bool changed = (track.url != m_nowPlayingUrl);
    m_nowPlayingUrl = track.url;
    if (changed)
        m_lastElapsedSec = -1;   // force the elapsed label to refresh for the new track
    highlightPlaying(track.url, /*scroll=*/changed);
    // Lazily resolve cover art for the now-playing track (worker thread). Remote
    // tracks already carry a cached cover from import, so only locals need this.
    if (changed && !track.isRemote())
        emit requestArt(track.url.toLocalFile());
}

void MainWindow::onArtResolved(const QString &path, const QString &artUrl)
{
    // Update the view row (if visible) and the playing queue track / MPRIS.
    // `path` is a local file path (art is local-only); map it to the model key.
    const int row = playRowForKey(QUrl::fromLocalFile(path));
    if (row >= 0)
        m_model->setArtUrl(row, artUrl);
    m_controller->setCurrentArt(QUrl::fromLocalFile(path), artUrl);
    if (m_controller->currentTrack().url.toLocalFile() == path)
        loadCover(artUrl);   // refresh the info-panel cover
    if (m_session && m_controller->currentTrack().url.toLocalFile() == path)
        m_session->refreshMetadata();
}

void MainWindow::onPlaybackStateChanged(bool playing)
{
    m_playPause->setIcon(style()->standardIcon(
        playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
}

void MainWindow::onPositionChanged(qint64 ms)
{
    if (!m_userSeeking)
        m_seek->setValue(static_cast<int>(ms));
    // positionChanged fires several times a second; the elapsed label only
    // changes once a second, so skip the string alloc + repaint in between.
    const qint64 sec = ms / 1000;
    if (sec != m_lastElapsedSec) {
        m_lastElapsedSec = sec;
        m_elapsed->setText(formatTime(ms));
    }
}

void MainWindow::onDurationChanged(qint64 ms)
{
    m_seek->setRange(0, static_cast<int>(ms));
    m_total->setText(formatTime(ms));
}
