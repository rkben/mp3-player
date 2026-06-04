#pragma once

#include <QDialog>

#include "Theme.h"
#include "LibraryFolder.h"

class QLineEdit;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QPushButton;
class QTableWidget;
class QWidget;

// Tabbed modal settings dialog. General tab holds appearance (theme); Library
// tab holds the music folder, scan thread cap, a manual Sync button, and an Auto
// Sync toggle. Designed so more tabs/options slot in later.
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    SettingsDialog(QList<LibraryFolder> folders, bool autoSync,
                   Theme::Mode themeMode, QString themeFile,
                   QWidget *parent = nullptr);

    QList<LibraryFolder> folders() const;   // labels default to the dir name
    bool autoSync() const;
    Theme::Mode themeMode() const;
    QString themeFile() const;

signals:
    // Emitted when the user clicks "Sync now" (dialog stays open).
    void syncRequested();
    // Emitted whenever the theme selection changes, for a live preview.
    void themeChanged(Theme::Mode mode, const QString &file);

private slots:
    void addFolder();
    void removeFolder();

private:
    void onThemeRowChanged();
    QWidget *buildGeneralTab();
    QWidget *buildAboutTab();
    void addFolderRow(const LibraryFolder &f);

    QTableWidget *m_folderTable;
    QPushButton *m_removeBtn;
    QCheckBox *m_autoSync;
    QComboBox *m_themeCombo;
    QLineEdit *m_themeFileEdit;
    QPushButton *m_themeBrowse;
};
