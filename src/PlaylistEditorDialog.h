#pragma once

#include <QDialog>
#include <QList>

#include <functional>

#include "PlaylistModel.h"   // Track

class PlaylistStore;
class TrackListModel;
class QListView;
class QListWidget;
class QLineEdit;
class QCheckBox;
class QComboBox;
class QLabel;

// A dual-list ("shuttle") playlist editor. Left = the whole indexed library, with a
// Tracks/Albums/Artists view mode, a search box and an include-remote toggle. Right
// = the tracks of the selected playlist, drag-reorderable. Arrows move tracks
// between them; Save writes the right list back to the playlist (m3u8 order = list
// order). Reuses the host's library snapshot and path->Track resolver rather than
// re-querying the DB.
class PlaylistEditorDialog : public QDialog
{
    Q_OBJECT
public:
    using Resolver = std::function<QList<Track>(const QStringList &)>;

    PlaylistEditorDialog(const QList<Track> &library, PlaylistStore *store,
                         Resolver resolve, const QString &preselect,
                         QWidget *parent = nullptr);

signals:
    void playlistsChanged();   // a playlist was saved; host should refresh its list

private:
    void onModeChanged(int index);
    void applyLeftFilter();
    void onPlaylistSelected(int index);
    void onPlaylistActivated(int index);   // handles the "New playlist…" row
    void addSelected();        // left selection -> right list
    void removeSelected();     // drop right-list selection
    void deduplicate();        // drop later same-artist+title rows (keep first)
    void save();
    void reject() override;    // intercept Close to prompt on unsaved changes

    void loadPlaylistIntoRight(const QString &name);
    void rebuildPlaylistCombo(const QString &select);
    void appendTrack(const Track &t);            // right list, dedup by key
    QList<Track> rightTracks() const;
    void refreshLeftFooter();
    void refreshRightFooter();
    void markDirty(bool dirty);
    bool confirmDiscardIfDirty();                // true = ok to proceed

    PlaylistStore *m_store;
    Resolver m_resolve;
    TrackListModel *m_leftModel;

    QComboBox *m_modeCombo;
    QLineEdit *m_search;
    QCheckBox *m_includeRemote;
    QListView *m_leftView;
    QLabel *m_leftFooter;

    QComboBox *m_playlistCombo;
    QListWidget *m_rightList;
    QLabel *m_rightFooter;

    QString m_currentName;     // playlist being edited ("" = none chosen yet)
    bool m_dirty = false;
    bool m_comboGuard = false; // suppress combo signal during programmatic changes
};
