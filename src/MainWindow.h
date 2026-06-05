#pragma once

#include <QWidget>

#include "PlaylistModel.h"   // Track (for QList<Track> signal arg)
#include "LibraryFolder.h"

class QLabel;
class QSlider;
class QToolButton;
class QThread;
class QTableView;
class QTreeView;
class QTabWidget;
class QSplitter;
class QSortFilterProxyModel;
class QStandardItemModel;
class QStandardItem;
class QLineEdit;
class QComboBox;
class QTimer;
class CoverLabel;
class PlayerController;
class MusicLibrary;
class MprisController;

// Two-panel desktop layout: a horizontal splitter with a tabbed left panel
// (directory tree) and a tabbed right/primary panel (Tracks table), above a
// full-width transport bar (now-playing, seek, transport buttons, volume).
class MainWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;   // re-tint icons on theme change

signals:
    void requestScan(const QStringList &folders);
    void requestArt(const QString &path);
    void requestSearch(const QString &text, int scope);
    void enrichTrack(const QString &path, const QString &title, const QString &artist,
                     const QString &album, int trackNo, qint64 durationMs);

private slots:
    void openSettings();
    void onTrackActivated(const QModelIndex &index);
    void onTreeActivated(const QModelIndex &index);   // double-click: enqueue
    void onTreeClicked(const QModelIndex &index);      // single-click: expand dirs
    void onTreeContextMenu(const QPoint &pos);
    void onQueueContextMenu(const QPoint &pos);
    void onCurrentTrackChanged(const Track &track);
    void onQueueChanged(const QList<Track> &queue);
    void onPlaybackStateChanged(bool playing);
    void onPositionChanged(qint64 ms);
    void onDurationChanged(qint64 ms);
    void onLibraryLoaded(const QList<Track> &tracks);
    void onTracksAppended(const QList<Track> &tracks);
    void onArtResolved(const QString &path, const QString &artUrl);
    void onSearchResults(const QString &query, const QList<Track> &tracks);
    void onScanProgress(int done, int total);
    void onScanStatus(const QString &message);

private:
    void buildUi();
    QWidget *buildTransportBar();
    QWidget *buildQueuePanel();       // right: "Queue" label + search + queue table
    QWidget *buildTrackInfoPanel();   // bottom-left: album art + track metadata
    QList<Track> tracksForPath(const QString &path) const;   // file or folder -> tracks
    void showTrackDetails(const Track &track);   // modal with full metadata + path
    void scheduleSearch();   // debounce, or restore library when cleared
    void showTrackInfo(const Track &track);   // populate the info panel
    void updateQueueTitle();                  // "Queue (N)" from the queue size
    void loadCover(const QString &artUrl);    // set the cover image (or placeholder)
    void startLibraryThread();
    void restoreSettings();
    void saveUiState() const;      // window geometry, splitter sizes, column widths
    void restoreUiState();         // … applied over the built-in defaults if present
    void cycleRepeat();
    void updateRepeatButton();
    void updateVolumeIcon();                      // pick level/mute icon for the volume
    void refreshThemedIcons();                    // re-tint custom SVG icons to the theme
    QIcon themedIcon(const QString &resource) const;   // SVG tinted to the text colour
    void rebuildTree();                       // top-level label nodes from m_folders
    void populateNode(QStandardItem *item);   // lazily fill a dir node's children
    void onTreeExpanded(const QModelIndex &index);
    int playRowForPath(const QString &path) const;   // -1 if not in queue view
    // Select the playing row if visible. Scrolls to reveal it only when `scroll`
    // is set — kept false for art/metadata refreshes so it doesn't fight the user.
    void highlightPlaying(const QUrl &url, bool scroll);
    void seedReadyQueue();   // give the player a ready queue when nothing's playing
    void applyCompact(bool compact);   // narrow/mobile vs wide/desktop layout

    PlaylistModel *m_model;
    PlayerController *m_controller;
    MusicLibrary *m_library = nullptr;
    QThread *m_libThread = nullptr;
#ifdef HAVE_MPRIS
    MprisController *m_mpris = nullptr;
#endif

    QSplitter *m_splitter;
    QSplitter *m_leftPanel;          // vertical: folder tabs over track info
    QTabWidget *m_leftTabs;
    QLabel *m_queueTitle;            // "Queue (N)" header
    CoverLabel *m_coverArt;
    QLabel *m_infoTitle;
    QLabel *m_infoArtist;
    QLabel *m_infoAlbum;
    QTableView *m_table;
    QSortFilterProxyModel *m_proxy;
    QTreeView *m_tree;
    QStandardItemModel *m_treeModel;
    QLineEdit *m_searchEdit;
    QComboBox *m_scope;
    QTimer *m_searchTimer;
    QLabel *m_status;
    QSlider *m_seek;
    QLabel *m_elapsed;
    QLabel *m_total;
    QToolButton *m_playPause;
    QToolButton *m_shuffleBtn = nullptr;   // null until the transport bar is built
    QToolButton *m_repeatBtn;
    QToolButton *m_volBtn = nullptr;
    QToolButton *m_settingsBtn = nullptr;
    QSlider *m_volume;

    bool m_userSeeking = false;
    int m_preMuteVolume = 80;   // volume to restore when un-muting
    qint64 m_lastElapsedSec = -1;   // last second shown in the elapsed label
    QUrl m_nowPlayingUrl;    // track currently revealed in the table (scroll guard)
    QList<LibraryFolder> m_folders;
    int m_repeatState = 0;   // 0 None, 1 All, 2 One
    bool m_compact = false;  // true = narrow/mobile single-panel layout
    QList<Track> m_fullLibrary;   // cached so search results can be cleared back
    bool m_searching = false;
};
