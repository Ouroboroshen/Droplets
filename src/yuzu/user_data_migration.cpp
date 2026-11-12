// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QTranslator>
#include "common/fs/fs_paths.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_string_lookup.h"
#include "user_data_migration.h"
#include "yuzu/migration_dialog.h"

// Needs to be included at the end due to https://bugreports.qt.io/browse/QTBUG-73263
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDir>
#include <QGuiApplication>
#include <QProgressDialog>
#include <QRadioButton>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>

namespace {

namespace fs = std::filesystem;
using namespace Common::FS;

// An earlier Droplet whose data can be brought here
struct PreviousDroplet {
    QString label;
    QString where;
    MigrationSource source;
    fs::file_time_type used{};
};

// Whether this Droplet has looked for earlier ones already. The config isn't loaded yet, so the
// [UI] calloutFlags line is read from the file.
bool DropletImportChecked() {
    std::ifstream file{GetEdenPath(EdenPath::ConfigDir) / "qt-config.ini"};
    bool ui = false;
    for (std::string line; std::getline(file, line);) {
        if (line.ends_with('\r')) {
            line.pop_back();
        }
        if (line.starts_with('[')) {
            ui = line == "[UI]";
        } else if (ui && line.starts_with("calloutFlags=")) {
            return (std::strtoul(line.c_str() + 13, nullptr, 10) & DropletImportCallout) != 0;
        }
    }
    return false;
}

QString PathText(const fs::path& path) {
    return QDir::toNativeSeparators(QString::fromStdString(PathToUTF8String(path)));
}

// "Droplet-v0.1.2-windows-x64" shows as "Droplet v0.1.2"
QString ReleaseLabel(const fs::path& folder) {
    QString name = QString::fromStdString(PathToUTF8String(folder.filename()));
    name.remove(QRegularExpression(QStringLiteral("-(windows|linux)-x64.*$")));
    if (name.startsWith(QStringLiteral("Droplet-"))) {
        return QStringLiteral("Droplet ") + name.mid(8);
    }
    return QStringLiteral("Droplet (%1)").arg(name);
}

// Earlier Droplets on this computer, the most recently used first: release folders (the Droplet
// program beside a "user" folder that has been used) next to this one's folder or in the folders
// next to its parent, and the shared data folder when this Droplet keeps its own. Only whether
// those folders exist is looked at, nothing inside them is read.
std::vector<PreviousDroplet> FindPreviousDroplets() {
#ifdef _WIN32
    static constexpr const char* program = "droplet.exe";
#else
    static constexpr const char* program = "droplet";
#endif
    const fs::path own_user = GetEdenPath(EdenPath::EdenDir);
    const bool portable = own_user.filename() == PORTABLE_DIR;
    const fs::path install =
        portable ? own_user.parent_path()
                 : fs::path{ToU8String(QCoreApplication::applicationDirPath().toStdString())};

    std::vector<PreviousDroplet> found;
    const auto add = [&found](const QString& label, const fs::path& where,
                              const MigrationSource& source) {
        std::error_code ec;
        auto used = fs::last_write_time(source.user_dir / CONFIG_DIR / "qt-config.ini", ec);
        if (ec) {
            used = fs::last_write_time(source.user_dir / NAND_DIR, ec);
        }
        found.push_back({label, PathText(where), source, used});
    };
    const auto consider = [&](const fs::path& folder) {
        std::error_code ec;
        if (fs::equivalent(folder, install, ec) || !fs::is_regular_file(folder / program, ec) ||
            !fs::is_directory(folder / PORTABLE_DIR / NAND_DIR, ec)) {
            return;
        }
        const QString label = ReleaseLabel(folder);
        add(label, folder, {label, folder / PORTABLE_DIR, {}, {}});
    };
    // The folders in `folder`, without following links. A huge folder costs at most `budget`
    // entries in all.
    size_t budget = 4000;
    const auto subfolders = [&budget](const fs::path& folder, const auto& visit) {
        std::error_code ec;
        for (fs::directory_iterator it{folder, fs::directory_options::skip_permission_denied, ec};
             !ec && it != fs::directory_iterator{} && budget > 0; it.increment(ec), --budget) {
            std::error_code type_ec;
            if (it->is_directory(type_ec) && !it->is_symlink(type_ec)) {
                visit(it->path());
            }
        }
    };
    const fs::path parent = install.parent_path();
    subfolders(parent, consider);
    // Each download unpacked into a folder of its own (Downloads\Compressed\<random>\Droplet-...)
    const fs::path grandparent = parent.parent_path();
    if (grandparent != parent) {
        subfolders(grandparent, [&](const fs::path& folder) {
            if (folder != parent) {
                consider(folder);
                subfolders(folder, consider);
            }
        });
    }
    if (portable) {
#ifdef _WIN32
        const MigrationSource shared{
            QObject::tr("Eden or Droplet data"), GetAppDataRoamingDirectory() / EDEN_DIR, {}, {}};
#else
        const MigrationSource shared{QObject::tr("Eden or Droplet data"),
                                     GetDataDirectory("XDG_DATA_HOME") / EDEN_DIR,
                                     GetDataDirectory("XDG_CONFIG_HOME") / EDEN_DIR,
                                     GetDataDirectory("XDG_CACHE_HOME") / EDEN_DIR};
#endif
        std::error_code ec;
        if (fs::is_directory(shared.user_dir / NAND_DIR, ec)) {
            add(shared.name, shared.user_dir, shared);
        }
    }

    std::sort(found.begin(), found.end(),
              [](const PreviousDroplet& a, const PreviousDroplet& b) { return a.used > b.used; });
    if (found.size() > 4) {
        found.resize(4);
    }
    QStringList labels;
    for (PreviousDroplet& droplet : found) {
        const QString label = droplet.label;
        for (int copy = 2; labels.contains(droplet.label); ++copy) {
            droplet.label = QStringLiteral("%1 (%2)").arg(label).arg(copy);
        }
        labels.append(droplet.label);
    }
    return found;
}

// Droplet test hook (main_window.cpp): EDEN_NX2_QT_STEPS "migrate" steps press the button with
// that text in whichever dialog is open <ms> after the prompt opens (the prompt, or the message
// after it), and "migrategrab" steps save that dialog as an image.
void ArmMigrationTestSteps() {
    const char* steps = std::getenv("EDEN_NX2_QT_STEPS");
    if (steps == nullptr) {
        return;
    }
    for (const QString& step : QString::fromUtf8(steps).split(QLatin1Char(';'))) {
        const QStringList parts = step.split(QLatin1Char(':'));
        if (parts.size() < 3 || !parts[1].startsWith(QStringLiteral("migrate"))) {
            continue;
        }
        const bool grab = parts[1] == QStringLiteral("migrategrab");
        const QString text = parts.mid(2).join(QLatin1Char(':'));
        QTimer::singleShot(parts[0].toInt(), qApp, [text, grab] {
            QWidget* const dialog = QApplication::activeModalWidget();
            if (dialog == nullptr) {
                return;
            }
            if (grab) {
                dialog->grab().save(text);
                return;
            }
            for (QAbstractButton* button : dialog->findChildren<QAbstractButton*>()) {
                if (button->text().remove(QLatin1Char('&')) == text) {
                    button->click();
                    return;
                }
            }
        });
    }
}

} // namespace

UserDataMigrator::UserDataMigrator(QMainWindow* main_window) {
    // NOTE: Logging is not initialized yet, do not produce logs here.

    // Check migration if config directory does not exist
    // TODO: ProfileManager messes with us a bit here, and force-creates the
    // /nand/system/save/8000000000000010/su/avators/profiles.dat file. Find a way to reorder
    // operations and have it create after this guy runs.
    // Droplet's release folders come with a config, so a first start is also one with no saves
    // here yet that hasn't looked for earlier Droplets.
    droplet_checked =
        !fs::is_directory(GetEdenPath(EdenPath::ConfigDir)) ||
        (!DropletImportChecked() && !fs::is_directory(GetEdenPath(EdenPath::NANDDir) / "user"));
    if (droplet_checked) {
        ShowMigrationPrompt(main_window);
    }
}

bool UserDataMigrator::AfterConfigLoad() {
    if (!droplet_checked) {
        return false;
    }
    UISettings::values.callout_flags =
        UISettings::values.callout_flags.GetValue() | DropletImportCallout;
    if (!droplet_user_dir.empty()) {
        // The copied config keeps the folders it had, inside the earlier Droplet's user folder
        const auto folder = [](std::string path) {
            std::replace(path.begin(), path.end(), '\\', '/');
            while (path.ends_with('/')) {
                path.pop_back();
            }
            return path + '/';
        };
        const std::string old_dir = folder(PathToUTF8String(droplet_user_dir));
        const std::string new_dir = folder(GetEdenPathString(EdenPath::EdenDir));
        for (const EdenPath path :
             {EdenPath::NANDDir, EdenPath::SDMCDir, EdenPath::LoadDir, EdenPath::DumpDir,
              EdenPath::TASDir, EdenPath::SaveDir, EdenPath::ScreenshotsDir}) {
            const std::string current = folder(GetEdenPathString(path));
            if (current.starts_with(old_dir)) {
                std::string moved = new_dir + current.substr(old_dir.size());
                moved.pop_back();
                SetEdenPath(path, moved);
            }
        }
    }
    return true;
}

void UserDataMigrator::ShowMigrationPrompt(QMainWindow* main_window) {
    using namespace QtCommon::StringLookup;

    MigrationDialog migration_prompt;
    migration_prompt.setWindowTitle(QObject::tr("Migration"));

    // mutually exclusive
    QButtonGroup* group = new QButtonGroup(&migration_prompt);

    // MACRO MADNESS

#define BUTTON(clazz, name, text, tooltip, checkState)                                             \
    clazz* name = new clazz(&migration_prompt);                                                    \
    name->setText(text);                                                                           \
    name->setToolTip(Lookup(tooltip));                                                             \
    name->setChecked(checkState);                                                                  \
    migration_prompt.addBox(name);

    BUTTON(QCheckBox, clear_shaders, QObject::tr("Clear Shader Cache"), MigrationTooltipClearShader,
           true)

    // The ids are MigrationStrategy's: Copy, Move, Link
    int id = 0;

#define RADIO(name, text, tooltip, checkState)                                                     \
    BUTTON(QRadioButton, name, text, tooltip, checkState)                                          \
    group->addButton(name, id++);

    RADIO(keep_old, QObject::tr("Keep Old Data"), MigrationTooltipKeepOld, true)
    RADIO(clear_old, QObject::tr("Clear Old Data"), MigrationTooltipClearOld, false)
    RADIO(link_old, QObject::tr("Link Old Directory"), MigrationTooltipLinkOld, false)

#undef RADIO
#undef BUTTON

    std::vector<Emulator> found{};

    for (const Emulator& emu : legacy_emus)
        if (fs::is_directory(emu.get_user_dir()))
            found.emplace_back(emu);

    const std::vector<PreviousDroplet> droplets = FindPreviousDroplets();

    if (found.empty() && droplets.empty()) {
        return;
    }

    // makes my life easier
    qRegisterMetaType<Emulator>();

    QString prompt_text = Lookup(MigrationPromptPrefix);

    // natural language processing is a nightmare
    for (const Emulator& emu : found) {
        prompt_text = prompt_text % QStringLiteral("\n ") % emu.name();

        QAbstractButton* button = migration_prompt.addButton(emu.name());

        // This is cursed, but it's actually the most efficient way by a mile
        button->setProperty("emulator", QVariant::fromValue(emu));
    }

    for (size_t i = 0; i < droplets.size(); ++i) {
        prompt_text = prompt_text % QStringLiteral("\n ") % droplets[i].label %
                      QStringLiteral(" (") % droplets[i].where % QStringLiteral(")");

        QAbstractButton* button = migration_prompt.addButton(droplets[i].label);
        button->setToolTip(droplets[i].where);
        button->setProperty("droplet", static_cast<int>(i));
    }

    prompt_text.append(QObject::tr("\n\n"));
    prompt_text = prompt_text % QStringLiteral("\n\n") % Lookup(MigrationPrompt);

    migration_prompt.setText(prompt_text);
    migration_prompt.addButton(QObject::tr("No"), true);

    ArmMigrationTestSteps();
    migration_prompt.exec();

    QAbstractButton* button = migration_prompt.clickedButton();

    if (button == nullptr || button->text() == QObject::tr("No")) {
        return ShowMigrationCancelledMessage(main_window);
    }

    MigrationWorker::MigrationStrategy strategy =
        static_cast<MigrationWorker::MigrationStrategy>(group->checkedId());

    if (const QVariant index = button->property("droplet"); index.isValid()) {
        const PreviousDroplet& droplet = droplets[static_cast<size_t>(index.toInt())];
        return MigrateUserData(main_window, droplet.source, clear_shaders->isChecked(), strategy,
                               true);
    }

    selected_emu = button->property("emulator").value<Emulator>();

    MigrateUserData(main_window,
                    {selected_emu.name(), selected_emu.get_user_dir(),
                     selected_emu.get_config_dir(), selected_emu.get_cache_dir()},
                    clear_shaders->isChecked(), strategy, false);
}

void UserDataMigrator::ShowMigrationCancelledMessage(QMainWindow* main_window) {
    QMessageBox::information(main_window, QObject::tr("Migration"),
                             QObject::tr("You can manually re-trigger this prompt by deleting the "
                                         "new config directory:\n%1")
                                 .arg(QString::fromStdString(Common::FS::GetEdenPathString(
                                     Common::FS::EdenPath::ConfigDir))),
                             QMessageBox::Ok);
}

void UserDataMigrator::MigrateUserData(QMainWindow* main_window, const MigrationSource& source,
                                       const bool clear_shader_cache,
                                       const MigrationWorker::MigrationStrategy strategy,
                                       const bool droplet) {
    // Create a dialog to let the user know it's migrating
    QProgressDialog* progress = new QProgressDialog(main_window);
    progress->setWindowTitle(QObject::tr("Migrating"));
    progress->setLabelText(QObject::tr("Migrating, this may take a while..."));
    progress->setRange(0, 0);
    progress->setCancelButton(nullptr);
    progress->setWindowModality(Qt::WindowModality::ApplicationModal);

    QThread* thread = new QThread(main_window);
    MigrationWorker* worker = new MigrationWorker(source, clear_shader_cache, strategy);
    worker->moveToThread(thread);

    thread->connect(thread, &QThread::started, worker, &MigrationWorker::process);

    thread->connect(worker, &MigrationWorker::finished, progress,
                    [=, this](const QString& success_text, const std::string& path) {
                        progress->close();
                        QMessageBox::information(main_window, QObject::tr("Migration"),
                                                 success_text, QMessageBox::Ok);

                        if (droplet) {
                            droplet_user_dir = source.user_dir;
                        } else {
                            migrated = true;
                        }
                        thread->quit();
                    });
    thread->connect(worker, &MigrationWorker::error, progress, [=](const QString& message) {
        progress->close();
        QMessageBox::warning(main_window, QObject::tr("Migration"), message, QMessageBox::Ok);
        thread->quit();
    });

    thread->connect(worker, &MigrationWorker::finished, worker, &QObject::deleteLater);
    thread->connect(worker, &MigrationWorker::error, worker, &QObject::deleteLater);
    thread->connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    thread->start();
    progress->exec();
}
