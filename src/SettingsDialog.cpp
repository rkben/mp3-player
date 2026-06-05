#include "SettingsDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QTabWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QTableWidget>
#include <QHeaderView>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QDir>

SettingsDialog::SettingsDialog(QList<LibraryFolder> folders, bool autoSync,
                               Theme::Mode themeMode, QString themeFile,
                               QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Settings"));
    setModal(true);
    setMinimumWidth(420);

    auto *root = new QVBoxLayout(this);
    auto *tabs = new QTabWidget;
    root->addWidget(tabs);

    // ---- General tab ----
    // Combo carries the Theme::Mode in item data; file row only enabled for Custom.
    m_themeCombo = new QComboBox;
    m_themeCombo->addItem(tr("System (native)"), int(Theme::Mode::System));
    m_themeCombo->addItem(tr("Dark"), int(Theme::Mode::Dark));
    m_themeCombo->addItem(tr("Custom stylesheet…"), int(Theme::Mode::Custom));
    m_themeCombo->setCurrentIndex(m_themeCombo->findData(int(themeMode)));

    m_themeFileEdit = new QLineEdit(themeFile);
    m_themeFileEdit->setPlaceholderText(tr("Path to a .qss stylesheet"));
    m_themeBrowse = new QPushButton(tr("Browse…"));
    m_themeBrowse->setMinimumHeight(34);

    tabs->addTab(buildGeneralTab(), tr("General"));

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
    tabs->addTab(library, tr("Library"));
    tabs->addTab(buildAboutTab(), tr("About"));
    tabs->setCurrentWidget(library);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
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
    auto *form = new QFormLayout(general);
    form->setSpacing(8);

    form->addRow(tr("Theme:"), m_themeCombo);

    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_themeFileEdit, 1);
    fileRow->addWidget(m_themeBrowse);
    form->addRow(tr("Stylesheet:"), fileRow);

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
    connect(addBtn, &QPushButton::clicked, this, &SettingsDialog::addFolder);
    m_removeBtn = new QPushButton(tr("Remove"));
    m_removeBtn->setMinimumHeight(34);
    connect(m_removeBtn, &QPushButton::clicked, this, &SettingsDialog::removeFolder);
    folderBtns->addWidget(addBtn);
    folderBtns->addWidget(m_removeBtn);
    folderBtns->addStretch();
    libLayout->addLayout(folderBtns);

    auto *form = new QFormLayout;
    form->setSpacing(8);

    m_autoSync = new QCheckBox(tr("Automatically sync the library"));
    m_autoSync->setChecked(autoSync);
    m_autoSync->setToolTip(tr("Watch the library folders and sync changes "
                              "automatically. (Not implemented yet.)"));
    form->addRow(tr("Auto sync:"), m_autoSync);

    libLayout->addLayout(form);

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

    auto *attrTitle = new QLabel(tr("Attribution"));
    QFont attrFont = attrTitle->font();
    attrFont.setBold(true);
    attrTitle->setFont(attrFont);
    layout->addSpacing(6);
    layout->addWidget(attrTitle);

    auto *attr = new QLabel(
        tr("<b>album_icon.svg</b><br>"
           "By Pymouss — Own work, Public Domain, "
           "<a href=\"https://commons.wikimedia.org/w/index.php?curid=5793388\">"
           "commons.wikimedia.org</a>"));
    attr->setTextFormat(Qt::RichText);
    attr->setWordWrap(true);
    attr->setOpenExternalLinks(true);
    attr->setTextInteractionFlags(Qt::TextBrowserInteraction);
    layout->addWidget(attr);

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
