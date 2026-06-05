#include "MainWindow.h"
#include "PlaylistModel.h"
#include "PlayerController.h"
#include "SettingsDialog.h"
#include "MusicLibrary.h"
#include "CoverLabel.h"
#include "Theme.h"
#include "AudioFormats.h"
#include "TimeFormat.h"
#ifdef HAVE_MPRIS
#include "MprisController.h"
#endif

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
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QLineEdit>
#include <QComboBox>
#include <QTimer>
#include <QLabel>
#include <QSlider>
#include <QToolButton>
#include <QMenu>
#include <QDialog>
#include <QFormLayout>
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

namespace {
// Custom item-data roles for the library tree's QStandardItemModel.
enum TreeRole {
    PathRole = Qt::UserRole + 1,   // absolute filesystem path (empty for none)
    IsDirRole,                     // bool: directory vs. audio file
    PopulatedRole,                 // bool: children have been lazily loaded
};

constexpr int kStatusTimeoutMs = 4000;   // how long a transient status line lingers
constexpr int kSearchDebounceMs = 220;   // idle time after typing before searching
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
    connect(m_controller, &PlayerController::positionChanged,
            this, &MainWindow::onPositionChanged);
    connect(m_controller, &PlayerController::durationChanged,
            this, &MainWindow::onDurationChanged);
    connect(m_controller, &PlayerController::trackError, this,
            [this](const QString &name, const QString &msg) {
                const QString text = tr("Skipped %1 — %2").arg(name, msg);
                m_status->setText(text);
                m_status->show();
                // Transient: clear it after a few seconds (unless superseded).
                QTimer::singleShot(kStatusTimeoutMs, this, [this, text] {
                    if (m_status->text() == text)
                        m_status->hide();
                });
            });
    connect(m_controller, &PlayerController::metadataResolved, this,
            [this](const QUrl &url, const QString &title, const QString &artist,
                   const QString &album, int trackNo, qint64 durationMs) {
                // Update the matching view row (if present) and persist to the DB.
                const int row = playRowForPath(url.toLocalFile());
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

#ifdef HAVE_MPRIS
    m_mpris = new MprisController(m_controller, this, this);
#endif

    startLibraryThread();
    restoreSettings();
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

    m_libThread->start();
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

    // --- Left panel: a "Library" tree whose top level is each configured
    // folder's label, with its directory contents lazily loaded underneath. ---
    m_treeModel = new QStandardItemModel(this);

    m_tree = new QTreeView;
    m_tree->setModel(m_treeModel);
    m_tree->setHeaderHidden(true);
    m_tree->setFrameShape(QFrame::NoFrame);   // no rounded frame border (any style)
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setAnimated(true);
    m_tree->setExpandsOnDoubleClick(false);   // single-click toggles; dbl-click enqueues
    // A single click on a folder toggles its expansion, but a double-click (two
    // clicks) would otherwise toggle *and* enqueue. Defer the toggle by the
    // double-click interval so onTreeActivated can cancel it for a genuine
    // double-click — giving clean single=expand, double=enqueue behaviour.
    m_treeClickTimer = new QTimer(this);
    m_treeClickTimer->setSingleShot(true);
    m_treeClickTimer->setInterval(QApplication::doubleClickInterval());
    connect(m_treeClickTimer, &QTimer::timeout, this, [this] {
        if (m_pendingToggle.isValid())
            m_tree->setExpanded(m_pendingToggle, !m_tree->isExpanded(m_pendingToggle));
        m_pendingToggle = QModelIndex();
    });
    connect(m_tree, &QTreeView::clicked, this, &MainWindow::onTreeClicked);
    connect(m_tree, &QTreeView::doubleClicked, this, &MainWindow::onTreeActivated);
    connect(m_tree, &QTreeView::expanded, this, &MainWindow::onTreeExpanded);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeView::customContextMenuRequested,
            this, &MainWindow::onTreeContextMenu);

    m_leftTabs = new QTabWidget;
    m_leftTabs->setDocumentMode(true);   // flat tab pane, no rounded frame
    m_leftTabs->addTab(m_tree, tr("Library"));

    // Playlists tab — placeholder for now (saved playlists land here later).
    auto *playlists = new QWidget;
    auto *plLayout = new QVBoxLayout(playlists);
    auto *plHint = new QLabel(tr("No playlists yet."));
    plHint->setAlignment(Qt::AlignCenter);
    plLayout->addStretch();
    plLayout->addWidget(plHint);
    plLayout->addStretch();
    m_leftTabs->addTab(playlists, tr("Playlists"));

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
    m_status = new QLabel;
    m_status->setObjectName("status");
    m_status->hide();
    root->addWidget(m_status);

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
    hh->setSectionResizeMode(PlaylistModel::Title,    QHeaderView::Stretch);
    hh->setSectionResizeMode(PlaylistModel::Artist,   QHeaderView::Interactive);
    hh->setSectionResizeMode(PlaylistModel::Album,    QHeaderView::Stretch);
    hh->setSectionResizeMode(PlaylistModel::Year,     QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(PlaylistModel::TrackNo,  QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(PlaylistModel::Duration, QHeaderView::ResizeToContents);
    m_table->sortByColumn(-1, Qt::AscendingOrder);   // default: keep DB/result order
    connect(m_table, &QTableView::activated, this, &MainWindow::onTrackActivated);
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
    m_searchEdit->setPlaceholderText(tr("Search…"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setMaximumWidth(320);   // cap so it doesn't span the whole panel

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(kSearchDebounceMs);
    connect(m_searchTimer, &QTimer::timeout, this, [this] {
        m_searching = true;
        emit requestSearch(m_searchEdit->text(), m_scope->currentIndex());
    });
    connect(m_searchEdit, &QLineEdit::textChanged, this, &MainWindow::scheduleSearch);
    connect(m_scope, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!m_searchEdit->text().trimmed().isEmpty())
            scheduleSearch();
    });

    m_queueTitle = new QLabel;
    m_queueTitle->setObjectName("queueTitle");
    updateQueueTitle();

    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(8, 6, 8, 4);
    searchRow->setSpacing(6);
    searchRow->addWidget(m_queueTitle);
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
    if (!item || item->data(PopulatedRole).toBool())
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

int MainWindow::playRowForPath(const QString &path) const
{
    // O(1) via the model's path index — this is hit several times per track
    // change (highlight, art, metadata), so a linear scan over a 45k queue hurt.
    return m_model->rowForPath(path);
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

void MainWindow::openSettings()
{
    QSettings s;
    const bool curAutoSync = s.value("library/autoSync", false).toBool();
    const Theme::Mode curTheme =
        Theme::modeFromString(s.value("ui/theme", "system").toString());
    const QString curThemeFile = s.value("ui/themeFile").toString();

    SettingsDialog dlg(m_folders, curAutoSync, curTheme, curThemeFile, this);

    // "Sync now" reconciles the configured folders (mtime diff) without closing
    // the dialog; the scan runs on the worker and updates the status.
    connect(&dlg, &SettingsDialog::syncRequested, this, [this] {
        if (!m_folders.isEmpty())
            emit requestScan(libraryFolderPaths(m_folders));
    });

    // Live theme preview while the dialog is open.
    connect(&dlg, &SettingsDialog::themeChanged, this,
            [](Theme::Mode mode, const QString &file) { Theme::apply(mode, file); });

    if (dlg.exec() != QDialog::Accepted) {
        Theme::apply(curTheme, curThemeFile);   // revert any preview
        return;
    }

    s.setValue("library/autoSync", dlg.autoSync());    // placeholder for now
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

void MainWindow::onLibraryLoaded(const QList<Track> &tracks)
{
    m_fullLibrary = tracks;        // browse source for the tree/search/enqueue
    seedReadyQueue();              // cold Play falls back to the whole library

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
    m_fullLibrary.append(tracks);   // grow the browse source; queue view untouched
}

void MainWindow::onSearchResults(const QString &query, const QList<Track> &tracks)
{
    // Drop stale/late results: only apply if they match the current query.
    if (!m_searching || query != m_searchEdit->text())
        return;
    m_model->setTracks(tracks);   // browse mode: double-click a result to enqueue
}

void MainWindow::onScanProgress(int done, int total)
{
    if (total <= 0)
        return;
    m_status->setText(tr("Scanning… %L1 / %L2").arg(done).arg(total));
}

void MainWindow::onScanStatus(const QString &message)
{
    if (message.isEmpty()) {
        m_status->hide();
        // Scripted-benchmark hook: quit cleanly after a scan so logs flush.
        // (Timing print itself is PP_SCAN_BENCH and does not quit — that lets
        // you run normally and watch parse timing while tuning the real library.)
        if (qEnvironmentVariableIsSet("PP_SCAN_QUIT"))
            qApp->quit();
    } else {
        m_status->setText(message);
        m_status->show();
    }
}

void MainWindow::restoreSettings()
{
    QSettings s;

    const int vol = s.value("playback/volume", 80).toInt();
    m_volume->setValue(vol);                       // emits valueChanged -> sets volume

    const bool shuffle = s.value("playback/shuffle", false).toBool();
    m_shuffleBtn->setChecked(shuffle);             // emits toggled -> controller

    m_repeatState = s.value("playback/repeat", 0).toInt();
    m_controller->setRepeatMode(static_cast<PlayerController::RepeatMode>(m_repeatState));
    updateRepeatButton();

    m_folders = loadLibraryFolders();
    rebuildTree();
    // Always kick the worker: it emits the cached library instantly, then
    // reconciles against disk. No folders just yields an empty library.
    emit requestScan(libraryFolderPaths(m_folders));
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

void MainWindow::highlightPlaying(const QUrl &url, bool scroll)
{
    const int row = playRowForPath(url.toLocalFile());
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
        // Browse mode: double-clicking a search result enqueues it.
        m_controller->enqueue({m_model->at(srcRow)});
    } else {
        // Queue mode: the source row is the queue index — play from there.
        m_controller->jumpTo(srcRow);
    }
}

void MainWindow::onTreeClicked(const QModelIndex &index)
{
    // Single click on a folder toggles its expansion (no need to hit the arrow),
    // but defer it so a double-click (enqueue) can cancel the toggle.
    if (index.isValid() && index.data(IsDirRole).toBool()) {
        m_pendingToggle = index;
        m_treeClickTimer->start();
    }
}

void MainWindow::onTreeActivated(const QModelIndex &index)
{
    // Double-click a file or folder: append its track(s) to the play queue.
    // Cancel the pending single-click expand so the folder doesn't also toggle.
    m_treeClickTimer->stop();
    m_pendingToggle = QModelIndex();
    if (!index.isValid())
        return;
    const QList<Track> add = tracksForPath(index.data(PathRole).toString());
    if (!add.isEmpty())
        m_controller->enqueue(add);
}

void MainWindow::onTreeContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_tree->indexAt(pos);
    if (!index.isValid())
        return;
    const QString path = index.data(PathRole).toString();

    QMenu menu(this);
    if (index.data(IsDirRole).toBool()) {
        // Directories and library-root nodes: queue or play their whole contents.
        connect(menu.addAction(tr("Play now")), &QAction::triggered, this, [this, path] {
            const QList<Track> tracks = tracksForPath(path);
            if (!tracks.isEmpty())
                m_controller->playQueue(tracks, 0);
        });
        connect(menu.addAction(tr("Add to queue")), &QAction::triggered, this, [this, path] {
            const QList<Track> tracks = tracksForPath(path);
            if (!tracks.isEmpty())
                m_controller->enqueue(tracks);
        });
    } else {
        // Files: just show metadata.
        connect(menu.addAction(tr("Show info")), &QAction::triggered, this, [this, path] {
            const QList<Track> tracks = tracksForPath(path);
            if (!tracks.isEmpty())
                showTrackDetails(tracks.first());
        });
    }
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
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
    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

void MainWindow::showTrackDetails(const Track &track)
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Track info"));
    auto *form = new QFormLayout(&dlg);

    auto addRow = [&](const QString &label, const QString &value) {
        if (value.isEmpty())
            return;
        auto *field = new QLabel(value);
        field->setTextInteractionFlags(Qt::TextSelectableByMouse);
        field->setWordWrap(true);
        form->addRow(label, field);
    };

    addRow(tr("Title:"), track.title);
    addRow(tr("Artist:"), track.artist);
    addRow(tr("Album:"), track.album);
    if (track.year > 0)
        addRow(tr("Year:"), QString::number(track.year));
    if (track.trackNo > 0)
        addRow(tr("Track:"), QString::number(track.trackNo));
    if (track.durationMs > 0)
        addRow(tr("Duration:"), formatTime(track.durationMs));
    addRow(tr("File:"), track.url.toLocalFile());
    if (!track.artUrl.isEmpty())
        addRow(tr("Artwork:"), QUrl(track.artUrl).toLocalFile());

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    form->addRow(buttons);

    dlg.adjustSize();   // size to contents, then widen/heighten for breathing room
    dlg.resize(dlg.width() * 2, dlg.height() + 80);
    dlg.exec();
}

QList<Track> MainWindow::tracksForPath(const QString &path) const
{
    if (path.isEmpty())
        return {};

    auto minimalTrack = [](const QString &p) {
        Track t;   // not in the library — minimal entry from the path
        t.url = QUrl::fromLocalFile(p);
        t.title = QFileInfo(p).fileName();
        return t;
    };

    QFileInfo info(path);
    if (!info.isDir()) {
        // Single file: one linear lookup, no full-library hash to build.
        for (const Track &t : m_fullLibrary)
            if (t.url.toLocalFile() == path)
                return {t};
        return {minimalTrack(path)};
    }

    // Folder: gather audio files beneath it, in name order.
    QStringList files;
    QDirIterator it(path, audioGlobs(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files.append(it.next());
    files.sort(Qt::CaseInsensitive);

    // One hash, reused across the (potentially many) files in the folder.
    QHash<QString, const Track *> byPath;
    byPath.reserve(m_fullLibrary.size());
    for (const Track &t : m_fullLibrary)
        byPath.insert(t.url.toLocalFile(), &t);

    QList<Track> out;
    out.reserve(files.size());
    for (const QString &p : files) {
        if (const Track *t = byPath.value(p, nullptr))
            out.append(*t);
        else
            out.append(minimalTrack(p));
    }
    return out;
}

void MainWindow::updateQueueTitle()
{
    // QLocale groups digits per the user's locale (e.g. "1,000"). Constructing
    // one is cheap but not free, and this is called on every queue change.
    static const QLocale locale;
    m_queueTitle->setText(tr("Queue (%1)")
                              .arg(locale.toString(m_controller->queueSize())));
}

void MainWindow::onQueueChanged(const QList<Track> &queue)
{
    updateQueueTitle();
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
    // Lazily resolve cover art for the now-playing track (worker thread).
    if (changed)
        emit requestArt(track.url.toLocalFile());
}

void MainWindow::onArtResolved(const QString &path, const QString &artUrl)
{
    // Update the view row (if visible) and the playing queue track / MPRIS.
    const int row = playRowForPath(path);
    if (row >= 0)
        m_model->setArtUrl(row, artUrl);
    m_controller->setCurrentArt(QUrl::fromLocalFile(path), artUrl);
    if (m_controller->currentTrack().url.toLocalFile() == path)
        loadCover(artUrl);   // refresh the info-panel cover
#ifdef HAVE_MPRIS
    if (m_mpris && m_controller->currentTrack().url.toLocalFile() == path)
        m_mpris->refreshMetadata();
#endif
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
