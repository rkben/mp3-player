#pragma once

#include <QDialog>

#include "Theme.h"
#include "LibraryFolder.h"

class QLineEdit;
class QCheckBox;
class QComboBox;
class QPushButton;
class QTableWidget;
class QPlainTextEdit;
class QTabWidget;
class QWidget;

// Tabbed modal settings dialog. General tab holds appearance (theme); Library
// tab holds the labelled music folders, a manual Sync button, and an Auto Sync
// toggle; Log tab shows the captured application log; About tab holds
// version/attribution. Designed so more tabs slot in.
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    SettingsDialog(QList<LibraryFolder> folders, bool autoSync, bool restoreQueue,
                   bool autoPlay, QString ytDlpPath, QByteArray audioDeviceId,
                   Theme::Mode themeMode, QString themeFile,
                   QWidget *parent = nullptr);

    QList<LibraryFolder> folders() const;   // labels default to the dir name
    bool autoSync() const;
    bool restoreQueue() const;
    bool autoPlay() const;
    QString ytDlpPath() const;
    QByteArray audioDeviceId() const;   // empty == follow the system default
    Theme::Mode themeMode() const;
    QString themeFile() const;

    // True if the user confirmed "Reset application" (clean slate). The host wipes
    // its data/config and restarts; other getters are ignored when this is set.
    bool resetRequested() const { return m_resetRequested; }

    // Open the dialog on the Library tab (e.g. from the Library "Add Directory"
    // button); default is the General tab.
    void selectLibraryTab();

signals:
    // Emitted when the user clicks "Sync now" (dialog stays open).
    void syncRequested();
    // Emitted whenever the theme selection changes, for a live preview.
    void themeChanged(Theme::Mode mode, const QString &file);
    // Emitted on output-device selection so the host can switch audio immediately.
    void audioDeviceChanged(const QByteArray &id);
#ifdef HAVE_DISCORD_RPC
    // Emitted on the Discord enable toggle, applied live by the host.
    void discordEnabledChanged(bool on);
#endif

private slots:
    void addFolder();
    void removeFolder();

private:
    void onThemeRowChanged();
    QWidget *buildGeneralTab();
    QWidget *buildLibraryTab(bool autoSync);
    QWidget *buildLogTab();
    QWidget *buildAboutTab();
    void addFolderRow(const LibraryFolder &f);

    QTableWidget *m_folderTable;
    QPushButton *m_removeBtn;
    QCheckBox *m_autoSync;
    QComboBox *m_themeCombo;
    QLineEdit *m_themeFileEdit;
    QPushButton *m_themeBrowse;
    QCheckBox *m_restoreQueue;
    QCheckBox *m_autoPlay;
    QLineEdit *m_ytDlpEdit;
    QComboBox *m_audioCombo;
    QPlainTextEdit *m_logView = nullptr;
    QTabWidget *m_tabs = nullptr;
    QWidget *m_libraryPage = nullptr;   // the Library tab's scroll page, for selection
    bool m_resetRequested = false;
#ifdef HAVE_DISCORD_RPC
    QCheckBox *m_discordEnabled = nullptr;
#endif
#ifdef HAVE_DISCORD_RPC
    QLineEdit *m_discordAppId = nullptr;   // self-contained: persists to QSettings
#endif
};
