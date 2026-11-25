/*
 * Hopflow -- mouse, keyboard and file sharing, built on Deskflow
 * SPDX-FileCopyrightText: (C) 2026 Hopflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/FileTransferLocations.h"

#include "common/Constants.h"
#include "deskflow/FileTransfer.h"

#include <QDir>
#include <QStandardPaths>

#include <chrono>
#include <optional>
#include <string>

#if defined(_WIN32)
#include <windows.h>

#include <KnownFolders.h>
#include <ShlObj.h>
#include <WtsApi32.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace deskflow::filetransfer {

namespace {

const auto kClipboardFolder = ".clipboard";
const auto kDragFolder = "deskflow-drag";
// A stale folder is one a crashed process never released.
constexpr auto kStaleDragAge = std::chrono::hours(1);

#if defined(_WIN32)
//! A known folder of the user at the console; only succeeds when running as SYSTEM
std::optional<fs::path> consoleUserFolder(REFKNOWNFOLDERID folderId)
{
  HANDLE token = nullptr;
  if (!WTSQueryUserToken(WTSGetActiveConsoleSessionId(), &token)) {
    return std::nullopt;
  }

  PWSTR folder = nullptr;
  const auto result = SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, token, &folder);
  CloseHandle(token);
  if (FAILED(result)) {
    CoTaskMemFree(folder);
    return std::nullopt;
  }

  fs::path path(folder);
  CoTaskMemFree(folder);
  return path;
}
#endif

fs::path downloadsFolder()
{
#if defined(_WIN32)
  if (auto path = consoleUserFolder(FOLDERID_Downloads)) {
    return *path;
  }
#endif

  auto location = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  if (location.isEmpty()) {
    location = QDir::homePath();
  }
  return pathFromUtf8(location.toStdString());
}

//! Private temporary root for files being dragged into a native drop target
fs::path dragRoot()
{
#if defined(_WIN32)
  // The core may run as SYSTEM; the GUI and the drop target run as the console
  // user, so staging must live in that user's profile rather than SYSTEM's temp.
  if (auto path = consoleUserFolder(FOLDERID_LocalAppData)) {
    return *path / kAppName / kDragFolder;
  }
#else
  // Target applications run as the same user and must be able to read the
  // staged files. Use the user cache rather than the system temp directory:
  // a Flatpak build of Deskflow only sees its own private /tmp. Inside Flatpak
  // XDG_CACHE_HOME points into the sandbox, which native applications cannot
  // read, so with --filesystem=home use the real ~/.cache instead.
  QString cache;
  if (qEnvironmentVariableIsSet("FLATPAK_ID")) {
    cache = QDir::homePath() + QStringLiteral("/.cache");
  } else {
    cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
  }
  if (cache.isEmpty()) {
    cache = QDir::tempPath();
  }
  return pathFromUtf8(cache.toStdString()) / kAppName /
         (std::string(kDragFolder) + "-" + std::to_string(getuid()));
#endif

  return pathFromUtf8(QDir::tempPath().toStdString()) / kDragFolder;
}

void secureRoot(const fs::path &root)
{
  std::error_code ec;
  fs::create_directories(root, ec);
  if (!ec) {
    fs::permissions(
        root, fs::perms::owner_all, fs::perm_options::replace, ec
    );
  }
}

} // namespace

fs::path receivedFilesFolder()
{
  return downloadsFolder() / kAppName;
}

fs::path dragTransferFolder(uint32_t id)
{
  const auto root = dragRoot();
  secureRoot(root);
  return root / std::to_string(id);
}

fs::path transferFolder(FileTransferPurpose purpose, uint32_t id)
{
  if (purpose == FileTransferPurpose::Clipboard) {
    return receivedFilesFolder() / kClipboardFolder / std::to_string(id);
  }
  return dragTransferFolder(id);
}

void releaseDragTransfer(uint32_t id)
{
  std::error_code ec;
  fs::remove_all(dragRoot() / std::to_string(id), ec);
}

void removeStaleDragTransfers()
{
  const auto root = dragRoot();
  std::error_code ec;
  const auto now = fs::file_time_type::clock::now();
  for (const auto &entry : fs::directory_iterator(root, ec)) {
    if (ec) {
      return;
    }

    std::error_code entryError;
    const auto modified = fs::last_write_time(entry.path(), entryError);
    if (!entryError && now - modified < kStaleDragAge) {
      continue;
    }
    fs::remove_all(entry.path(), entryError);
  }
}

void removeOldClipboardTransfers(uint32_t keepId)
{
  const auto folder = receivedFilesFolder() / kClipboardFolder;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(folder, ec)) {
    if (entry.path().filename() != std::to_string(keepId)) {
      fs::remove_all(entry.path(), ec);
    }
  }
}

} // namespace deskflow::filetransfer
