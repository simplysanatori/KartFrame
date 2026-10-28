// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

// F10 > Diagnostics > Export Logs: gathers what a bug report needs into one
// folder the player chose, ready to zip and attach.
namespace log_export {

inline constexpr std::string_view kExportFolderPrefix = "KartFrame-logs-";

struct Result {
    // The folder created for this export; empty when it could not be created.
    std::filesystem::path destination;
    size_t files_copied = 0;
    size_t files_failed = 0;
    // The first problem met, suitable for display.
    std::string error;

    bool Succeeded() const { return !destination.empty() && files_failed == 0 && error.empty(); }
};

// Creates "KartFrame-logs-YYYYMMDD-HHMMSS" (local time, with a numeric suffix
// if that name exists) inside `parent` and copies into it:
//   Logs/            the whole run-log tree, one folder per run, current run included
//   Config.toml      the player's configuration
//   export-info.txt  the export time followed by `note`
// A missing logs directory or config file is skipped, not an error, as long as
// something was exported.
//
// Files are copied through ordinary shared-read streams rather than a platform
// copy call, so the running session's console.log, which is still open for
// writing, is exported up to its current end.
Result ExportLogs(const std::filesystem::path& logs_directory, const std::filesystem::path& config_file,
                  const std::filesystem::path& parent, std::string_view note,
                  std::chrono::system_clock::time_point now);

} // namespace log_export
