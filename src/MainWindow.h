#pragma once

#include <QWidget>

#include <functional>

#include "PlaylistModel.h"   // Track (for QList<Track> signal arg)
#include "LibraryFolder.h"
#include "PlaylistStore.h"

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
class QListWidget;
class QMenu;
class QStackedWidget;
class CoverLabel;
class ShaderArt;
class StatusStack;
class SubsonicClient;
class QNetworkAccessManager;
class PlayerController;
class MusicLibrary;
class MediaSession;
class Importer;

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
#ifdef HAVE_VISUALIZER
    bool eventFilter(QObject *watched, QEvent *event) override;   // reposition the pop-out overlay
#endif

signals:
    void requestScan(const QStringList &folders);
    void requestArt(const QString &path);
    void requestSearch(const QString &text, int scope);
    void enrichTrack(const QString &path, const QString &title, const QString &artist,
                     const QString &album, int trackNo, qint64 durationMs);
    void importTracks(const QList<Track> &tracks);   // -> library worker
    void removeTracks(const QStringList &uris);      // -> library worker (Remote tree)
    void cancelImports(bool removeTracks);           // -> library worker (clear resume)
    // -> library worker: upsert a Subsonic sync batch (finalPrune drops stale rows)
    void syncSubsonicBatch(const QString &serverId, const QList<Track> &tracks,
                           qint64 epoch, bool finalPrune);

private slots:
    void openSettings(bool startOnLibrary = false);
    void resetApplication();   // wipe DB + data + settings, then quit (clean slate)
    void onTrackActivated(const QModelIndex &index);
    void onTreeActivated(const QModelIndex &index);   // double-click: enqueue
    void onTreeClicked(const QModelIndex &index);      // single-click: expand dirs
    void onTreeContextMenu(const QPoint &pos);
    void onQueueContextMenu(const QPoint &pos);
    void onPlaylistContextMenu(const QPoint &pos);
    void openPlaylistEditor(const QString &preselect = QString());
    void createPlaylist();     // Playlists tab: make a new empty playlist
    void importFromUrl();      // Playlists tab: yt-dlp import modal
    void cancelActiveImport(); // status-row ✕: stop import, ask Keep/Remove
    void startSubsonicSync(const QString &serverId);   // background sync (from Settings)
    void onCurrentTrackChanged(const Track &track);
    void onQueueChanged(const QList<Track> &queue);
    void onPlaybackStateChanged(bool playing);
    void onPositionChanged(qint64 ms);
    void onDurationChanged(qint64 ms);
    void onLibraryLoaded(const QList<Track> &tracks);
    void onTracksAppended(const QList<Track> &tracks);
    void onArtResolved(const QString &path, const QString &artUrl);
    void onSearchResults(const QString &query, const QList<Track> &tracks);
    void onScanProgress(int done, int total, const QString &sourceLabel,
                        const QString &fileName);
    void onScanStatus(const QString &message);

private:
    void buildUi();
    QWidget *buildTransportBar();
    QWidget *buildQueuePanel();       // right: "Queue" label + search + queue table
    QWidget *buildTrackInfoPanel();   // bottom-left: album art + track metadata
#ifdef HAVE_VISUALIZER
    void applyVisualizer(bool on, const QString &shader);   // switch art/visualizer + pick shader
    void applyVisualizerSettings();   // read the saved on/off + shader, then applyVisualizer()
    void toggleVisualizerPopOut();    // open/close a mirror visualizer window
    void updateVisualizerCapture();   // enable audio capture if the panel or pop-out needs it
#endif
    QList<Track> tracksForKeys(const QStringList &keys) const;    // resolve via library
    QStringList audioPathsForPath(const QString &path) const;    // file or folder -> paths
    void playNow(const QList<Track> &tracks);   // replace queue, drop any active playlist
    void showTrackDetails(const Track &track);   // modal with full metadata + path
    // Reveal a track in the OS: local -> open its containing folder; remote -> open
    // the page URL in the default browser.
    void openTrackLocation(const Track &track);
    void scheduleSearch();   // debounce, or restore library when cleared
    void showTrackInfo(const Track &track);   // populate the info panel
    void updateQueueTitle();                  // "<playlist>[ [modified]] (N)" header
    QStringList queueStoredPaths() const;     // playlist-file form of the current queue
    void flushQueueCache();                   // write any pending debounced queue cache
    void buildQueueMenu();                    // (re)populate the queue-actions dropdown
    // "Add to playlist" submenu; paths resolved lazily on click (cheap popup).
    void addToPlaylistMenu(QMenu *menu, std::function<QStringList()> paths);
    void refreshPlaylists();                  // repopulate the Playlists tab list
    void loadPlaylist(const QString &name);   // resolve + play, set current playlist
    void setCurrentPlaylist(const QString &name, bool dirty);   // update header + persist
    void loadCover(const QString &artUrl);    // set the cover image (or placeholder)
    void resolveSubsonicCover(const QString &token);   // fetch+cache a deferred cover
    void startLibraryThread();
    void restoreSettings();
    void saveUiState() const;      // window geometry, splitter sizes, column widths
    void restoreUiState();         // … applied over the built-in defaults if present
    void cycleRepeat();
    void updateRepeatButton();
    void updateVolumeIcon();                      // pick level/mute icon for the volume
    void refreshThemedIcons();                    // re-tint custom SVG icons to the theme
    QIcon themedIcon(const QString &resource) const;   // SVG tinted to the text colour
    void rebuildTree();                       // Files tab: Remote node + folder roots
    void populateNode(QStandardItem *item);   // lazily fill a dir node's children
    void appendRemoteNode();                  // Library tree: "Remote" host/playlist tree
    void appendSubsonicNodes();               // Library tree: a "Subsonic" source per server
    void refreshRemoteNode();                 // replace just the Remote root
    // Stored keys (paths or URLs) a tree node resolves to: a filesystem node walks
    // its folder; a virtual leaf is its own key; a virtual group gathers descendants.
    QStringList keysForIndex(const QModelIndex &index) const;
    void onTreeExpanded(const QModelIndex &index);
    int playRowForKey(const QUrl &url) const;   // queue-view row for a track; -1 if absent
    // Select the playing row if visible. Scrolls to reveal it only when `scroll`
    // is set — kept false for art/metadata refreshes so it doesn't fight the user.
    void highlightPlaying(const QUrl &url, bool scroll);
    void seedReadyQueue();   // give the player a ready queue when nothing's playing
    void startAutoPlay();    // begin playback on launch, honouring the shuffle state
    void applyCompact(bool compact);   // narrow/mobile vs wide/desktop layout
    // Fold player-resolved metadata into the view row + DB. Shared by play-time
    // resolution and the MetadataProbe; updateNowPlaying refreshes the labels
    // only when the resolved track is the one currently playing.
    void applyResolvedMetadata(const QUrl &url, const QString &title,
                               const QString &artist, const QString &album,
                               int trackNo, qint64 durationMs,
                               bool updateNowPlaying);

    PlaylistModel *m_model;
    PlayerController *m_controller;
    MusicLibrary *m_library = nullptr;
    QThread *m_libThread = nullptr;
    class MetadataProbe *m_probe = nullptr;      // player-backed tag fallback
    QThread *m_probeThread = nullptr;
    Importer *m_importer = nullptr;
    SubsonicClient *m_subsonicSync = nullptr;   // active background Subsonic sync (or null)
    QNetworkAccessManager *m_coverNet = nullptr;   // lazy Subsonic cover-art downloads
    QString m_currentCoverToken;   // guards a late cover download against a track switch
    MediaSession *m_session = nullptr;   // OS media-session bridge (null if none)
    class YtDlpManager *m_ytdlp = nullptr;   // managed yt-dlp binary (download/update)
#ifdef HAVE_DISCORD_RPC
    class DiscordPresence *m_discord = nullptr;   // Discord Rich Presence (live toggle)
#endif

    QSplitter *m_splitter;
    QSplitter *m_leftPanel;          // vertical: folder tabs over track info
    QTabWidget *m_leftTabs;
    QLabel *m_queueTitle;            // "<playlist>[ [modified]] (N)" header
    QToolButton *m_queueMenuBtn = nullptr;   // queue-actions dropdown (Clear/Save/…)
    QMenu *m_queueMenu = nullptr;
    QListWidget *m_playlistList = nullptr;   // Playlists tab
    CoverLabel *m_coverArt;
#ifdef HAVE_VISUALIZER
    QStackedWidget *m_coverStack = nullptr;  // album art <-> audio visualizer
    ShaderArt *m_visualizer = nullptr;       // in-panel QRhiWidget (created eagerly)
    ShaderArt *m_popupVisualizer = nullptr;  // independent mirror in the pop-out window
    QToolButton *m_visPopOut = nullptr;      // overlay button: pop out
    QWidget *m_visWindow = nullptr;          // detached visualizer window (when open)
    QString m_currentShader;                 // applied to both in-panel + pop-out
    bool m_visualizerOn = false;             // visualizer shown in the panel
#endif
    QLabel *m_infoTitle;
    QLabel *m_infoArtist;
    QLabel *m_infoAlbum;
    QTableView *m_table;
    QSortFilterProxyModel *m_proxy;
    QTreeView *m_tree;                // Library tab (filesystem + Remote)
    QStandardItemModel *m_treeModel;
    QLineEdit *m_searchEdit;
    QComboBox *m_scope;
    QTimer *m_searchTimer;
    QTimer *m_queueCacheTimer;   // debounces the resumable queue-cache write
    StatusStack *m_status;   // layered status line (see StatusStack); post per slot
    QToolButton *m_cancelImportBtn = nullptr;   // ✕ on the status row, shown while importing
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
    QHash<QString, int> m_libIndexByKey;   // Track::key() -> m_fullLibrary index
    bool m_searching = false;

    PlaylistStore m_store;        // m3u8 playlists + resumable queue cache
    QString m_currentPlaylist;    // empty = unsaved "Queue"; else the loaded playlist
    bool m_queueDirty = false;    // queue diverged from the saved playlist
    bool m_loadingPlaylist = false;   // guard: deliberate load shouldn't flag dirty
    bool m_resetting = false;     // suppress save-on-quit while wiping for a reset
    bool m_resumeQueue = true;    // ui/restoreQueue: repopulate the queue on launch
    bool m_autoPlay = false;      // ui/autoPlay: start playback on launch
    bool m_autoPlayPending = false;   // one-shot: consumed on the first libraryLoaded
    QStringList m_pendingResume;  // cached-queue lines, resolved on first libraryLoaded
};
