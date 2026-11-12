// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <QMainWindow>
#include "common/common_types.h"
#include "yuzu/migration_worker.h"

// The [UI] calloutFlags bit for "this Droplet has looked for earlier ones" (CalloutFlag)
inline constexpr u32 DropletImportCallout = 0x4;

// TODO(crueter): Quick implementation
class UserDataMigrator {
public:
    UserDataMigrator(QMainWindow* main_window);

    // Once the config is loaded: records that this Droplet looked for earlier ones, and points the
    // folders a copied Droplet config still names in the old user folder at this one. True when
    // the config changed and should be saved now.
    bool AfterConfigLoad();

    bool migrated{false};
    Emulator selected_emu;

private:
    void ShowMigrationPrompt(QMainWindow* main_window);
    void ShowMigrationCancelledMessage(QMainWindow* main_window);
    void MigrateUserData(QMainWindow* main_window, const MigrationSource& source,
                         const bool clear_shader_cache,
                         const MigrationWorker::MigrationStrategy strategy, const bool droplet);

    bool droplet_checked{false};
    std::filesystem::path droplet_user_dir; // the earlier Droplet's, once its data is here
};
