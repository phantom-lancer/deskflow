/*
 * Hopflow -- mouse, keyboard and file sharing, built on Deskflow
 * SPDX-FileCopyrightText: (C) 2026 Hopflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>
#include <QStringList>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <utility>

//! Starts a real operating-system drag for files received from another computer
/*!
The files are already staged locally by the core. This widget hands their local
URLs to Qt's native drag implementation, which speaks XDND on X11 and OLE
CF_HDROP on Windows, so the application under the pointer receives a normal
drop instead of a copy in the downloads folder.
*/
class NativeDragSource : public QObject
{
  Q_OBJECT

public:
  explicit NativeDragSource(QObject *parent = nullptr);
  ~NativeDragSource() override;

  //! Called after the drag ended and the staged files may be deleted
  void setFinishedCallback(std::function<void(uint32_t)> callback);

  //! Start dragging \p paths, or ignore the request when one is already active
  void start(uint32_t id, const QStringList &paths);

  bool isActive() const
  {
    return m_active;
  }

private:
  void finish(uint32_t id);

  QWidget m_window;
  std::function<void(uint32_t)> m_finished;
  bool m_active = false;
};
