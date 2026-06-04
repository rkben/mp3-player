#pragma once

#include <QString>
#include <QList>
#include <QDir>
#include <QSettings>

// A user-configured library location: a filesystem path plus a required,
// human-readable label shown as the top-level node in the library tree.
struct LibraryFolder {
    QString label;
    QString path;
};

// Load the configured folders from QSettings (key "library/folders"). Migrates
// the legacy single-folder key "library/folder" on first read.
inline QList<LibraryFolder> loadLibraryFolders()
{
    QSettings s;
    QList<LibraryFolder> out;
    const int n = s.beginReadArray("library/folders");
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        LibraryFolder f;
        f.label = s.value("label").toString();
        f.path = s.value("path").toString();
        if (!f.path.isEmpty())
            out.append(f);
    }
    s.endArray();

    if (out.isEmpty()) {   // migrate a legacy single folder, if present
        const QString legacy = s.value("library/folder").toString();
        if (!legacy.isEmpty()) {
            LibraryFolder f;
            f.path = legacy;
            f.label = QDir(legacy).dirName();
            if (f.label.isEmpty())
                f.label = legacy;
            out.append(f);
        }
    }
    return out;
}

inline void saveLibraryFolders(const QList<LibraryFolder> &folders)
{
    QSettings s;
    s.beginWriteArray("library/folders");
    for (int i = 0; i < folders.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue("label", folders[i].label);
        s.setValue("path", folders[i].path);
    }
    s.endArray();
    s.remove("library/folder");   // drop the obsolete legacy key
}

inline QStringList libraryFolderPaths(const QList<LibraryFolder> &folders)
{
    QStringList paths;
    paths.reserve(folders.size());
    for (const LibraryFolder &f : folders)
        paths.append(f.path);
    return paths;
}

inline QStringList folderLabels(const QList<LibraryFolder> &folders)
{
    QStringList labels;
    labels.reserve(folders.size());
    for (const LibraryFolder &f : folders)
        labels.append(f.label);
    return labels;
}
