#include "PlaylistEditorDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListView>
#include <QListWidget>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QToolButton>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QSet>
#include <QMessageBox>
#include <QAbstractItemView>

#include "PlaylistStore.h"
#include "TrackListModel.h"
#include "TimeFormat.h"
#include "TrackUri.h"

namespace {
constexpr int kNewPlaylistData = -1;   // combo userData marking the "New playlist…" row
}

PlaylistEditorDialog::PlaylistEditorDialog(const QList<Track> &library,
                                           PlaylistStore *store, Resolver resolve,
                                           const QString &preselect, QWidget *parent)
    : QDialog(parent), m_store(store), m_resolve(std::move(resolve))
{
    setWindowTitle(tr("Playlist Editor"));
    resize(820, 560);

    // ---- Left pane: library source (mode + search + include-remote, list, footer).
    m_leftModel = new TrackListModel(this);
    m_leftModel->setSource(library);

    m_modeCombo = new QComboBox;
    m_modeCombo->addItem(tr("Tracks"),  TrackListModel::Tracks);
    m_modeCombo->addItem(tr("Albums"),  TrackListModel::Albums);
    m_modeCombo->addItem(tr("Artists"), TrackListModel::Artists);

    m_search = new QLineEdit;
    m_search->setPlaceholderText(tr("Search library…"));
    m_search->setClearButtonEnabled(true);

    m_includeRemote = new QCheckBox(tr("Include remote"));

    m_leftView = new QListView;
    m_leftView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_leftView->setModel(m_leftModel);
    m_leftView->setUniformItemSizes(true);   // 45k rows: keep layout cheap

    m_leftFooter = new QLabel;

    auto *leftCol = new QVBoxLayout;
    auto *leftTop = new QHBoxLayout;
    leftTop->addWidget(m_modeCombo);
    leftTop->addWidget(m_search, 1);
    leftCol->addLayout(leftTop);
    leftCol->addWidget(m_includeRemote);
    leftCol->addWidget(m_leftView, 1);
    leftCol->addWidget(m_leftFooter);

    // ---- Middle: move arrows.
    auto *addBtn = new QToolButton;
    addBtn->setText(QStringLiteral("▶"));
    addBtn->setToolTip(tr("Add selected to playlist"));
    auto *removeBtn = new QToolButton;
    removeBtn->setText(QStringLiteral("◀"));
    removeBtn->setToolTip(tr("Remove selected from playlist"));

    auto *midCol = new QVBoxLayout;
    midCol->addStretch(1);
    midCol->addWidget(addBtn);
    midCol->addWidget(removeBtn);
    midCol->addStretch(1);

    // ---- Right pane: the playlist (dropdown, reorderable list, footer).
    m_playlistCombo = new QComboBox;

    m_rightList = new QListWidget;
    m_rightList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_rightList->setDragDropMode(QAbstractItemView::InternalMove);   // drag-to-reorder

    m_rightFooter = new QLabel;

    auto *dedupeBtn = new QPushButton(tr("Deduplicate"));
    dedupeBtn->setToolTip(tr("Remove repeated tracks (same artist and title)."));

    auto *rightCol = new QVBoxLayout;
    rightCol->addWidget(m_playlistCombo);
    rightCol->addWidget(m_rightList, 1);
    auto *rightBottom = new QHBoxLayout;
    rightBottom->addWidget(m_rightFooter, 1);
    rightBottom->addWidget(dedupeBtn);
    rightCol->addLayout(rightBottom);

    auto *lists = new QHBoxLayout;
    lists->addLayout(leftCol, 1);
    lists->addLayout(midCol);
    lists->addLayout(rightCol, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close);

    auto *root = new QVBoxLayout(this);
    root->addLayout(lists, 1);
    root->addWidget(buttons);

    // ---- Wiring.
    connect(m_modeCombo, &QComboBox::currentIndexChanged, this,
            &PlaylistEditorDialog::onModeChanged);
    connect(m_search, &QLineEdit::textChanged, this,
            [this] { applyLeftFilter(); });
    connect(m_includeRemote, &QCheckBox::toggled, this,
            [this] { applyLeftFilter(); });
    connect(m_leftView, &QListView::doubleClicked, this,
            [this] { addSelected(); });

    connect(addBtn, &QToolButton::clicked, this, &PlaylistEditorDialog::addSelected);
    connect(removeBtn, &QToolButton::clicked, this, &PlaylistEditorDialog::removeSelected);

    connect(m_playlistCombo, &QComboBox::currentIndexChanged, this,
            &PlaylistEditorDialog::onPlaylistSelected);
    connect(m_playlistCombo, &QComboBox::activated, this,
            &PlaylistEditorDialog::onPlaylistActivated);
    connect(dedupeBtn, &QPushButton::clicked, this, &PlaylistEditorDialog::deduplicate);

    // Any content change to the right list (add/remove/drag-reorder) dirties it and
    // refreshes the footer; programmatic loads are wrapped to suppress the dirty flag.
    auto onRightChanged = [this] {
        if (m_comboGuard)   // programmatic bulk load: footer refreshed once afterward
            return;
        markDirty(true);
        refreshRightFooter();
    };
    connect(m_rightList->model(), &QAbstractItemModel::rowsInserted, this, onRightChanged);
    connect(m_rightList->model(), &QAbstractItemModel::rowsRemoved, this, onRightChanged);

    connect(buttons, &QDialogButtonBox::accepted, this, &PlaylistEditorDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    applyLeftFilter();
    rebuildPlaylistCombo(preselect);

    // Load the initial selection if it's a real (existing) playlist; otherwise start
    // empty (e.g. no playlists yet — the combo shows only "New playlist…").
    const int cur = m_playlistCombo->currentIndex();
    if (cur >= 0 && m_playlistCombo->itemData(cur).toInt() != kNewPlaylistData)
        loadPlaylistIntoRight(m_playlistCombo->itemText(cur));
    else
        refreshRightFooter();
}

void PlaylistEditorDialog::onModeChanged(int)
{
    m_leftModel->setMode(
        static_cast<TrackListModel::Mode>(m_modeCombo->currentData().toInt()));
    refreshLeftFooter();
}

void PlaylistEditorDialog::applyLeftFilter()
{
    m_leftModel->setFilter(m_search->text(), m_includeRemote->isChecked());
    refreshLeftFooter();
}

void PlaylistEditorDialog::onPlaylistSelected(int index)
{
    if (m_comboGuard || index < 0)
        return;
    // The "New playlist…" row is handled by onPlaylistActivated() — which also fires
    // when it's the only entry (no index change, so currentIndexChanged wouldn't).
    if (m_playlistCombo->itemData(index).toInt() == kNewPlaylistData)
        return;

    // Unsaved edits on the current playlist must be resolved before switching away.
    if (!confirmDiscardIfDirty()) {
        rebuildPlaylistCombo(m_currentName);   // revert the combo selection
        return;
    }
    loadPlaylistIntoRight(m_playlistCombo->itemText(index));
}

void PlaylistEditorDialog::onPlaylistActivated(int index)
{
    // Only the "New playlist…" sentinel; real playlists are handled by selection.
    // Using activated() (not currentIndexChanged) means this also works when the
    // sentinel is the sole entry and already current — the no-playlists case.
    if (m_comboGuard || index < 0
        || m_playlistCombo->itemData(index).toInt() != kNewPlaylistData)
        return;

    if (!confirmDiscardIfDirty()) {
        rebuildPlaylistCombo(m_currentName);
        return;
    }

    const QString name =
        QInputDialog::getText(this, tr("New Playlist"), tr("Playlist name:")).trimmed();
    if (name.isEmpty()) {
        rebuildPlaylistCombo(m_currentName);
        return;
    }
    if (m_store->exists(name)) {
        loadPlaylistIntoRight(name);   // already exists — edit it rather than clobber
        rebuildPlaylistCombo(name);
        return;
    }
    // A brand-new, empty playlist: created on Save.
    m_currentName = name;
    m_comboGuard = true;
    m_rightList->clear();
    m_comboGuard = false;
    refreshRightFooter();
    markDirty(true);   // Save persists the (possibly empty) new playlist
    rebuildPlaylistCombo(name);
}

void PlaylistEditorDialog::loadPlaylistIntoRight(const QString &name)
{
    m_currentName = name;
    const QList<Track> tracks = m_resolve(m_store->readPaths(name));

    m_comboGuard = true;   // suppress dirty/footer churn during the bulk load
    m_rightList->clear();
    for (const Track &t : tracks)
        appendTrack(t);
    m_comboGuard = false;

    refreshRightFooter();
    markDirty(false);
}

void PlaylistEditorDialog::rebuildPlaylistCombo(const QString &select)
{
    // Repopulate the combo entries and set the selection — but never load (callers
    // own loading), so this is safe to call on the revert path without clobbering an
    // unsaved playlist's in-progress tracks.
    m_comboGuard = true;
    m_playlistCombo->clear();
    for (const QString &name : m_store->names())
        m_playlistCombo->addItem(name);
    // Keep an unsaved, in-progress playlist visible until Save persists it.
    if (!m_currentName.isEmpty() && m_playlistCombo->findText(m_currentName) < 0)
        m_playlistCombo->addItem(m_currentName);
    m_playlistCombo->addItem(tr("New playlist…"), kNewPlaylistData);

    const int idx = m_playlistCombo->findText(select);
    m_playlistCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    m_comboGuard = false;
}

void PlaylistEditorDialog::appendTrack(const Track &t)
{
    // Dedupe: a track already in the playlist isn't added again.
    const QString key = t.key();
    for (int i = 0; i < m_rightList->count(); ++i)
        if (m_rightList->item(i)->data(Qt::UserRole).value<Track>().key() == key)
            return;

    auto *item = new QListWidgetItem(t.displayText());
    item->setData(Qt::UserRole, QVariant::fromValue(t));
    m_rightList->addItem(item);
}

void PlaylistEditorDialog::addSelected()
{
    const QModelIndexList sel = m_leftView->selectionModel()->selectedIndexes();
    for (const QModelIndex &idx : sel)
        for (const Track &t : m_leftModel->tracksForRow(idx.row()))
            appendTrack(t);
}

void PlaylistEditorDialog::removeSelected()
{
    // Delete from the bottom up so row indices stay valid as we remove.
    const QList<QListWidgetItem *> sel = m_rightList->selectedItems();
    for (QListWidgetItem *item : sel)
        delete m_rightList->takeItem(m_rightList->row(item));
}

void PlaylistEditorDialog::deduplicate()
{
    // Best-effort: keep the first occurrence of each artist+title (trimmed,
    // case-insensitive) and drop later repeats. May merge variants that share a
    // title (live vs studio) — accepted for a one-click cleanup.
    QSet<QString> seen;
    for (int i = 0; i < m_rightList->count(); /* advance conditionally */) {
        const Track t = m_rightList->item(i)->data(Qt::UserRole).value<Track>();
        const QString key = (t.artist.trimmed() + QLatin1Char('')
                             + t.title.trimmed()).toLower();
        if (seen.contains(key)) {
            delete m_rightList->takeItem(i);   // don't advance; row i is now the next
        } else {
            seen.insert(key);
            ++i;
        }
    }
}

QList<Track> PlaylistEditorDialog::rightTracks() const
{
    QList<Track> out;
    out.reserve(m_rightList->count());
    for (int i = 0; i < m_rightList->count(); ++i)
        out.append(m_rightList->item(i)->data(Qt::UserRole).value<Track>());
    return out;
}

void PlaylistEditorDialog::save()
{
    if (m_currentName.isEmpty()) {
        QMessageBox::information(this, tr("Playlist Editor"),
                                 tr("Choose or create a playlist first."));
        return;
    }
    QStringList paths;
    const QList<Track> tracks = rightTracks();
    paths.reserve(tracks.size());
    for (const Track &t : tracks)
        paths << storedForm(t.url);

    if (!m_store->write(m_currentName, paths)) {
        QMessageBox::warning(this, tr("Playlist Editor"),
                             tr("Could not save “%1”.").arg(m_currentName));
        return;
    }
    markDirty(false);
    rebuildPlaylistCombo(m_currentName);   // a new playlist now appears in the list
    emit playlistsChanged();
}

void PlaylistEditorDialog::reject()
{
    if (!confirmDiscardIfDirty())
        return;
    QDialog::reject();
}

bool PlaylistEditorDialog::confirmDiscardIfDirty()
{
    if (!m_dirty)
        return true;
    const auto choice = QMessageBox::question(
        this, tr("Unsaved Changes"),
        tr("Save changes to “%1”?").arg(m_currentName),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (choice == QMessageBox::Cancel)
        return false;
    if (choice == QMessageBox::Save)
        save();
    return true;
}

void PlaylistEditorDialog::markDirty(bool dirty)
{
    m_dirty = dirty;
}

void PlaylistEditorDialog::refreshLeftFooter()
{
    const int rows = m_leftModel->rowCount();
    QString noun;
    switch (m_leftModel->mode()) {
    case TrackListModel::Albums:  noun = tr("albums");  break;
    case TrackListModel::Artists: noun = tr("artists"); break;
    case TrackListModel::Tracks:  default: noun = tr("tracks"); break;
    }
    m_leftFooter->setText(tr("%1 %2 · %3").arg(rows).arg(
        noun, formatDurationLong(m_leftModel->visibleDurationMs())));
}

void PlaylistEditorDialog::refreshRightFooter()
{
    const QList<Track> tracks = rightTracks();
    m_rightFooter->setText(tr("%1 tracks · %2").arg(tracks.size()).arg(
        formatDurationLong(totalDurationMs(tracks))));
}
