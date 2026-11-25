/*
 * Hopflow -- mouse, keyboard and file sharing, built on Deskflow
 * SPDX-FileCopyrightText: (C) 2026 Hopflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "NativeDragSource.h"

#include <QApplication>
#include <QCursor>
#include <QDrag>
#include <QFileInfo>
#include <QIcon>
#include <QMimeData>
#include <QPoint>
#include <QPixmap>
#include <QStyle>
#include <QTimer>
#include <QUrl>

namespace {

// How long a target application may still read the staged files after the drag.
constexpr int kCleanupDelayMs = 60000;

// A mapped but almost invisible window; Qt needs a real source window for XDND.
constexpr int kWindowSize = 4;

} // namespace

NativeDragSource::NativeDragSource(QObject *parent)
    : QObject(parent),
      m_window(
          nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus |
                       Qt::NoDropShadowWindowHint
      )
{
  m_window.setAttribute(Qt::WA_ShowWithoutActivating);
  m_window.setAttribute(Qt::WA_TranslucentBackground);
  m_window.setWindowOpacity(0.01);
  m_window.resize(kWindowSize, kWindowSize);
}

NativeDragSource::~NativeDragSource() = default;

void NativeDragSource::setFinishedCallback(std::function<void(uint32_t)> callback)
{
  m_finished = std::move(callback);
}

void NativeDragSource::start(uint32_t id, const QStringList &paths)
{
  if (m_active) {
    // one native drag at a time; drop the stale staging of the other transfer
    finish(id);
    return;
  }

  if (paths.isEmpty()) {
    finish(id);
    return;
  }

  m_active = true;

  // Let the IPC handler return before the drag takes over the GUI event loop.
  QTimer::singleShot(0, this, [this, id, paths] {
    QList<QUrl> urls;
    for (const auto &path : paths) {
      if (!QFileInfo(path).exists()) {
        continue;
      }
      urls.append(QUrl::fromLocalFile(path));
    }

    if (urls.isEmpty()) {
      m_active = false;
      finish(id);
      return;
    }

    auto *mime = new QMimeData;
    mime->setUrls(urls);

    const auto position = QCursor::pos();
    m_window.move(position.x() - kWindowSize / 2, position.y() - kWindowSize / 2);
    m_window.show();
    m_window.raise();

    QDrag drag(&m_window);
    drag.setMimeData(mime);
    const auto dragIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
    drag.setPixmap(dragIcon.pixmap(48, 48));
    drag.setHotSpot(QPoint(24, 24));
    drag.exec(Qt::CopyAction, Qt::CopyAction);

    m_window.hide();
    m_active = false;

    // The target may still be reading the files. Delete them after a grace
    // period instead of immediately, and let the core do the removal.
    QTimer::singleShot(kCleanupDelayMs, this, [this, id] { finish(id); });
  });
}

void NativeDragSource::finish(uint32_t id)
{
  if (m_finished) {
    m_finished(id);
  }
}
