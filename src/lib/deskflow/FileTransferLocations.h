/*
 * Hopflow -- mouse, keyboard and file sharing, built on Deskflow
 * SPDX-FileCopyrightText: (C) 2026 Hopflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ProtocolTypes.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace deskflow::filetransfer {

//! Folder that copied files are saved to: the signed-in user's Downloads/Hopflow
/*!
On Windows the core may run as SYSTEM in the user's session, so this asks for
the Downloads folder of the user signed in at the console, not the process's.
*/
std::filesystem::path receivedFilesFolder();

//! Folder a transfer of \p purpose is received into
/*!
Dragged files are staged per transfer in a private cache folder so the
receiving computer can offer them to native drag-and-drop and delete them after
the drop. Copied files are staged per transfer in a .clipboard subfolder until
they are pasted.
*/
std::filesystem::path transferFolder(FileTransferPurpose purpose, uint32_t id);

//! Private cache folder for dragged files of transfer \p id
std::filesystem::path dragTransferFolder(uint32_t id);

//! Delete the temporary folder of drag transfer \p id
void releaseDragTransfer(uint32_t id);

//! Delete drag folders left behind by an earlier run
void removeStaleDragTransfers();

//! Delete staged clipboard transfers other than \p keepId
void removeOldClipboardTransfers(uint32_t keepId);

} // namespace deskflow::filetransfer
