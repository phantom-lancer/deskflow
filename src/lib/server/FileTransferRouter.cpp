/*
 * Hopflow -- mouse, keyboard and file sharing, built on Deskflow
 * SPDX-FileCopyrightText: (C) 2026 Hopflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/FileTransferRouter.h"

#include "base/Log.h"
#include "deskflow/Computer.h"
#include "deskflow/FileTransfer.h"
#include "deskflow/FileTransferLocations.h"
#include "deskflow/FileTransferNotify.h"
#include "server/BaseClientProxy.h"

using namespace deskflow::filetransfer;

FileTransferRouter::FileTransferRouter(BaseClientProxy *primary, deskflow::Computer *computer)
    : m_primary(primary),
      m_computer(computer),
      m_local(
          FileTransferManager::Callbacks{
              .destination = transferFolder,
              .received = [this](
                              uint32_t id, FileTransferPurpose purpose, const std::vector<std::string> &paths
                          ) { localReceived(purpose, id, paths); },
              .failed = notifyFailed
          }
      )
{
  removeStaleDragTransfers();
}

FileTransferRouter::~FileTransferRouter() = default;

void FileTransferRouter::setEnabled(bool enabled)
{
  if (m_enabled && !enabled) {
    LOG_INFO("file transfer is disabled");
    m_clipboard = {};
    m_drag = {};
  }
  m_enabled = enabled;
}

void FileTransferRouter::setMaxTransferBytes(uint64_t bytes)
{
  m_maxTransferBytes = bytes;
  m_local.setMaxTransferBytes(bytes);
}

bool FileTransferRouter::canReceive(const BaseClientProxy *computer) const
{
  return computer == m_primary || computer->isHopflow();
}

bool FileTransferRouter::canPasteFiles(const BaseClientProxy *computer) const
{
  // Windows, macOS and Linux X11 publish local file lists through their native clipboard APIs.
  const auto platform = computer->getPlatform();
  return canReceive(computer) &&
         (platform == PeerPlatform::MacOS || platform == PeerPlatform::Windows || platform == PeerPlatform::Linux);
}

void FileTransferRouter::onClipboardFiles(BaseClientProxy *from, const std::vector<std::string> &paths)
{
  if (paths.empty()) {
    // whoever copied last decides what the clipboard holds
    if (m_clipboard.owner != nullptr) {
      LOG_DEBUG("clipboard no longer holds files");
    }
    m_clipboard = {};
    return;
  }

  LOG_DEBUG("\"%s\" copied %zu files", from->getName().c_str(), paths.size());
  m_clipboard = Offer{from, paths, {}};

  // clients report files as the cursor leaves them, so it may already be elsewhere
  if (m_active != nullptr) {
    onEnter(m_active);
  }
}

bool FileTransferRouter::isOwnPaste(const std::vector<std::string> &files) const
{
  // our own paste of received files, not a new copy
  return !files.empty() && samePaths(files, m_localClipboardReceived);
}

void FileTransferRouter::offerPrimaryClipboard(const std::vector<std::string> &files)
{
  m_localClipboardReceived.clear();
  onClipboardFiles(m_primary, files);
}

void FileTransferRouter::onPrimaryClipboardChanged()
{
  const auto files = m_computer->getClipboardFiles();
  if (!isOwnPaste(files)) {
    offerPrimaryClipboard(files);
  }
}

void FileTransferRouter::refreshPrimaryClipboard(const std::vector<std::string> &files)
{
  const auto alreadyOffered = m_clipboard.owner == m_primary && samePaths(files, m_clipboard.paths);
  if (!isOwnPaste(files) && !alreadyOffered) {
    offerPrimaryClipboard(files);
  }
}

void FileTransferRouter::onClipboardTakenByOther(BaseClientProxy *by)
{
  if (!by->isHopflow()) {
    m_clipboard = {};
  }
}

void FileTransferRouter::onDragFiles(BaseClientProxy *from, const std::vector<std::string> &paths)
{
  if (paths.empty()) {
    if (m_drag.owner == from) {
      m_drag = {};
      m_dropSeen = false;
      cancelDropRoutes(from);
    }
    return;
  }

  if (m_drag.owner != from) {
    LOG_INFO("\"%s\" is dragging %zu files", from->getName().c_str(), paths.size());
  }
  m_drag = Offer{from, paths, {}};
  m_dropSeen = false;

  // clients report their drag as the cursor leaves them, so it may already be
  // on another computer that must start receiving before the user drops
  if (m_active != nullptr && m_active != from) {
    onEnter(m_active);
  }
}

bool FileTransferRouter::isDragging(const BaseClientProxy *computer) const
{
  return m_enabled && m_drag.owner == computer;
}

void FileTransferRouter::onEnter(BaseClientProxy *computer)
{
  m_active = computer;

  // A drag only belongs to the computer under the pointer. Cancel transfers
  // to computers the cursor left so they never start a stale native drag.
  if (m_drag.owner != nullptr) {
    cancelDropRoutesExcept(computer);
  }

  auto &offer = m_clipboard;
  if (m_enabled && offer.owner != nullptr && offer.owner != computer && canPasteFiles(computer) &&
      !offer.deliveredTo.contains(computer)) {
    offer.deliveredTo.insert(computer);
    startTransfer(offer.owner, computer, FileTransferPurpose::Clipboard, offer.paths);
  }

  auto &drag = m_drag;
  if (m_enabled && drag.owner != nullptr && drag.owner != computer && canReceive(computer) &&
      !drag.deliveredTo.contains(computer)) {
    // Start receiving while the button is still down, so the destination can
    // show its own native drag cursor and accept the drop in the application.
    drag.deliveredTo.insert(computer);
    startTransfer(drag.owner, computer, FileTransferPurpose::Drop, drag.paths);
  }
}

void FileTransferRouter::onDrop(BaseClientProxy *computer)
{
  if (m_drag.owner == nullptr) {
    return;
  }

  const auto drag = std::exchange(m_drag, {});
  m_dropSeen = true;
  if (!m_enabled || drag.owner == computer) {
    return;
  }

  if (!canReceive(computer)) {
    LOG_WARN("files dropped on \"%s\", which does not run Hopflow", computer->getName().c_str());
    return;
  }

  // The transfer normally starts as the cursor enters, so the drop only has to
  // finish it. This is the fallback for an offer that arrived after the enter.
  if (!drag.deliveredTo.contains(computer)) {
    startTransfer(drag.owner, computer, FileTransferPurpose::Drop, drag.paths);
  }
}

void FileTransferRouter::startTransfer(
    BaseClientProxy *source, BaseClientProxy *destination, FileTransferPurpose purpose,
    const std::vector<std::string> &paths
)
{
  const auto id = m_nextId++;
  LOG_INFO(
      "file transfer %u: %zu items from \"%s\" to \"%s\"", id, paths.size(), source->getName().c_str(),
      destination->getName().c_str()
  );

  m_routes.insert_or_assign(id, Route{source, destination, purpose});
  if (source == m_primary) {
    m_local.send(id, purpose, paths, transportTo(destination));
  } else {
    source->fileRequest(id, purpose, joinPaths(paths));
  }
}

void FileTransferRouter::cancelRoute(uint32_t id, const Route &route)
{
  // Stop the sender and release the receiver, whichever side is this computer.
  if (route.source == m_primary) {
    m_local.onEnd(id, FileTransferStatus::Cancelled);
  } else {
    route.source->fileEnd(id, FileTransferStatus::Cancelled);
  }

  if (route.destination == m_primary) {
    m_local.onEnd(id, FileTransferStatus::Cancelled);
  } else {
    route.destination->fileEnd(id, FileTransferStatus::Cancelled);
  }
}

void FileTransferRouter::cancelDropRoutes(BaseClientProxy *source)
{
  for (auto it = m_routes.begin(); it != m_routes.end();) {
    if (it->second.purpose != FileTransferPurpose::Drop || it->second.source != source) {
      ++it;
      continue;
    }

    const auto [id, route] = *it;
    it = m_routes.erase(it);
    LOG_INFO("file transfer %u: cancelled, the drag ended", id);
    cancelRoute(id, route);
  }
}

void FileTransferRouter::cancelDropRoutesExcept(BaseClientProxy *destination)
{
  for (auto it = m_routes.begin(); it != m_routes.end();) {
    if (it->second.purpose != FileTransferPurpose::Drop || it->second.destination == destination) {
      ++it;
      continue;
    }

    const auto [id, route] = *it;
    it = m_routes.erase(it);
    LOG_INFO("file transfer %u: cancelled, the cursor left the drag target", id);
    // Forget the delivery so dragging back onto this computer starts it again.
    m_drag.deliveredTo.erase(route.destination);
    cancelRoute(id, route);
  }
}

Transport FileTransferRouter::transportTo(BaseClientProxy *client)
{
  return Transport{
      .start = [client](
                   uint32_t id, FileTransferPurpose purpose, const std::string &manifest
               ) { client->fileStart(id, purpose, manifest); },
      .chunk = [client](uint32_t id, const std::string &data) { client->fileChunk(id, data); },
      .ack = [client](uint32_t id, uint32_t chunks) { client->fileAck(id, chunks); },
      .end =
          [this, client](uint32_t id, FileTransferStatus status) {
            client->fileEnd(id, status);
            endRoute(id);
          }
  };
}

void FileTransferRouter::endRoute(uint32_t id)
{
  m_routes.erase(id);
}

void FileTransferRouter::localReceived(FileTransferPurpose purpose, uint32_t id, const std::vector<std::string> &paths)
{
  if (purpose == FileTransferPurpose::Clipboard) {
    m_localClipboardReceived = paths;
    if (!m_computer->setClipboardFiles(paths)) {
      LOG_WARN("this computer cannot put files on the clipboard");
    }
    removeOldClipboardTransfers(id);
  } else if (purpose == FileTransferPurpose::Drop) {
    // Start a native drag while the button is still down instead of revealing
    // a permanent copy in Downloads. A transfer that finishes after the
    // release is discarded.
    if (!m_dropSeen) {
      notifyDragReady(id, paths);
    } else {
      releaseDragTransfer(id);
    }
    return;
  }
  notifyReceived(purpose, paths);
}

void FileTransferRouter::onClientRemoved(BaseClientProxy *client)
{
  if (m_active == client) {
    m_active = nullptr;
  }
  if (m_clipboard.owner == client) {
    m_clipboard = {};
  }
  m_clipboard.deliveredTo.erase(client);
  if (m_drag.owner == client) {
    m_drag = {};
  }

  for (auto it = m_routes.begin(); it != m_routes.end();) {
    const auto [id, route] = *it;
    if (route.source != client && route.destination != client) {
      ++it;
      continue;
    }

    it = m_routes.erase(it);
    LOG_INFO("file transfer %u: cancelled, \"%s\" disconnected", id, client->getName().c_str());
    auto *other = route.source == client ? route.destination : route.source;
    if (other == m_primary) {
      m_local.onEnd(id, FileTransferStatus::Cancelled);
    } else {
      other->fileEnd(id, FileTransferStatus::Cancelled);
    }
  }
}

void FileTransferRouter::onOffer(BaseClientProxy *from, FileTransferPurpose purpose, const std::string &paths)
{
  if (purpose == FileTransferPurpose::Clipboard) {
    onClipboardFiles(from, splitPaths(paths));
  } else {
    onDragFiles(from, splitPaths(paths));
  }
}

void FileTransferRouter::onStart(
    BaseClientProxy *from, uint32_t id, FileTransferPurpose purpose, const std::string &manifest
)
{
  auto it = m_routes.find(id);
  if (it == m_routes.end() || it->second.source != from) {
    LOG_WARN("file transfer %u: unexpected start from \"%s\"", id, from->getName().c_str());
    from->fileEnd(id, FileTransferStatus::Cancelled);
    return;
  }

  const auto route = it->second;
  std::string error;
  const auto parsed = parseManifest(manifest, error);
  if (!m_enabled || !parsed || totalSize(*parsed) > m_maxTransferBytes) {
    LOG_WARN("file transfer %u: refused, %s", id, !m_enabled ? "disabled" : parsed ? "too large" : error.c_str());
    m_routes.erase(it);
    from->fileEnd(id, FileTransferStatus::Cancelled);
    if (parsed && m_enabled) {
      notifyFailed("the files are larger than the limit");
    }
    return;
  }

  if (route.destination == m_primary) {
    m_local.onStart(id, purpose, manifest, transportTo(from));
  } else {
    route.destination->fileStart(id, purpose, manifest);
  }
}

void FileTransferRouter::onChunk(BaseClientProxy *from, uint32_t id, const std::string &data)
{
  auto it = m_routes.find(id);
  if (it == m_routes.end() || it->second.source != from) {
    return;
  }

  if (it->second.destination == m_primary) {
    m_local.onChunk(id, data);
  } else {
    it->second.destination->fileChunk(id, data);
  }
}

void FileTransferRouter::onAck(BaseClientProxy *from, uint32_t id, uint32_t chunks)
{
  auto it = m_routes.find(id);
  if (it == m_routes.end() || it->second.destination != from) {
    return;
  }

  if (it->second.source == m_primary) {
    m_local.onAck(id, chunks);
  } else {
    it->second.source->fileAck(id, chunks);
  }
}

void FileTransferRouter::onEnd(BaseClientProxy *from, uint32_t id, FileTransferStatus status)
{
  auto it = m_routes.find(id);
  if (it == m_routes.end() || (it->second.source != from && it->second.destination != from)) {
    return;
  }

  const auto route = it->second;
  m_routes.erase(it);
  auto *other = route.source == from ? route.destination : route.source;
  if (other == m_primary) {
    m_local.onEnd(id, status);
  } else {
    other->fileEnd(id, status);
  }
}
