/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFileInfoList>
#include <QFontDatabase>
#include <QIcon>
#include <QPalette>
#include <QStyleHints>
#include <QStyleFactory>

#include "common/Constants.h"

namespace deskflow::gui {

/**
 * @brief Detects dark mode in a universal manner (all Qt versions).
 * Until better platform support is added, it's more reliable to use the old way (compare text and window lightness),
 * because the newer versions in Qt 6.5+ are not always correct and some return `UnknownScheme`.
 * https://www.qt.io/blog/dark-mode-on-windows-11-with-qt-6.5
 */
inline bool isDarkMode()
{
  const QPalette palette = QApplication::instance() != nullptr ? QApplication::palette() : QPalette();
  const auto text = palette.color(QPalette::WindowText);
  const auto window = palette.color(QPalette::Window);
  return text.lightness() > window.lightness();
}
/**
 * @brief get a string for the iconMode
 * @returns "dark" or "light"
 */
inline QString iconMode()
{
  return isDarkMode() ? QStringLiteral("dark") : QStringLiteral("light");
}

inline void applyDeskflowTheme(QApplication &app)
{
#if defined(Q_OS_WIN)
  // Keep the native Windows widget style; only tune spacing, surfaces and accent color.
  QPalette palette = app.palette();
  palette.setColor(QPalette::Window, QColor(QStringLiteral("#f5f6f7")));
  palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#1f2328")));
  palette.setColor(QPalette::Base, QColor(QStringLiteral("#ffffff")));
  palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#f1f3f5")));
  palette.setColor(QPalette::Text, QColor(QStringLiteral("#1f2328")));
  palette.setColor(QPalette::Button, QColor(QStringLiteral("#f4f5f6")));
  palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#1f2328")));
  palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#0067c0")));
  palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
  palette.setColor(QPalette::Link, QColor(QStringLiteral("#0067c0")));
  app.setPalette(palette);
  app.setStyleSheet(QStringLiteral(R"(
    QPushButton {
      min-height: 28px; padding: 5px 14px; border: 1px solid #d2d7df;
      border-radius: 6px; background: #f7f8fa; color: #1f2328;
    }
    QPushButton:hover { background: #eceff3; border-color: #b7c0cb; }
    QPushButton:pressed { background: #e2e7ed; }
    QPushButton:default { background: #0067c0; border-color: #0067c0; color: white; }
    QPushButton:default:hover { background: #0875d1; }
    QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
      min-height: 26px; padding: 2px 8px; border: 1px solid #c5cbd3;
      border-radius: 6px; background: white; color: #1f2328;
    }
    QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus {
      border: 1px solid #0067c0;
    }
    QTabBar::tab { padding: 8px 15px; border: none; border-bottom: 2px solid transparent; }
    QTabBar::tab:selected { border-bottom-color: #0067c0; }
    QGroupBox { margin-top: 12px; padding: 10px; border: 1px solid #d9dde3; border-radius: 8px; }
    QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }
  )"));
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
  // Fusion with a restrained dark palette is consistent across XFCE and KDE,
  // and does not add any runtime process, animation or rendering dependency.
  app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
  QPalette palette;
  palette.setColor(QPalette::Window, QColor(QStringLiteral("#191c21")));
  palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#e6e9ee")));
  palette.setColor(QPalette::Base, QColor(QStringLiteral("#12151a")));
  palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#22262d")));
  palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#252a32")));
  palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#f1f3f6")));
  palette.setColor(QPalette::Text, QColor(QStringLiteral("#e6e9ee")));
  palette.setColor(QPalette::Button, QColor(QStringLiteral("#292e36")));
  palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#e6e9ee")));
  palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#ffffff")));
  palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#397fdb")));
  palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
  palette.setColor(QPalette::Link, QColor(QStringLiteral("#78aaff")));
  palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#8a929f")));
  app.setPalette(palette);
  app.setStyleSheet(QStringLiteral(R"(
    QWidget { color: #e6e9ee; }
    QMainWindow, QDialog, QMenu { background: #191c21; }
    QGroupBox {
      margin-top: 12px; padding: 10px; border: 1px solid #343b46;
      border-radius: 8px; background: #20242b;
    }
    QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }
    QPushButton {
      min-height: 28px; padding: 5px 14px; border: 1px solid #414955;
      border-radius: 6px; background: #2b3039; color: #e6e9ee;
    }
    QPushButton:hover { background: #363e49; border-color: #566273; }
    QPushButton:pressed { background: #242a32; }
    QPushButton:default { background: #397fdb; border-color: #397fdb; color: white; }
    QPushButton:default:hover { background: #4b90ec; }
    QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
      min-height: 26px; padding: 2px 8px; border: 1px solid #3a424e;
      border-radius: 6px; background: #15191f; color: #e6e9ee;
      selection-background-color: #397fdb;
    }
    QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus {
      border: 1px solid #397fdb;
    }
    QTabWidget::pane { border: 1px solid #343b46; border-radius: 6px; top: -1px; }
    QTabBar::tab { padding: 8px 15px; background: #20242b; border: 1px solid #343b46; }
    QTabBar::tab:selected { background: #2d3540; border-bottom: 2px solid #5b9bff; }
    QListWidget, QTreeWidget, QTableView, QPlainTextEdit, QTextEdit {
      background: #15191f; alternate-background-color: #20242b;
      border: 1px solid #343b46; border-radius: 6px;
    }
    QHeaderView::section { background: #252a32; color: #dce1e8; padding: 5px; border: none; }
    QMenu::item:selected { background: #303b4a; }
    QToolTip { background: #252a32; color: #f1f3f6; border: 1px solid #454d59; }
  )"));
#endif
}

inline void updateIconTheme()
{
  // Sets the fallback icon path and fallback theme
  const auto themeName = QStringLiteral("%1-%2").arg(kAppId, iconMode());
  if (QIcon::themeName().isEmpty() || QIcon::themeName().startsWith(kAppId))
    QIcon::setThemeName(themeName);
  else
    QIcon::setFallbackThemeName(themeName);
  QIcon::setFallbackSearchPaths({QStringLiteral(":/icons/%1").arg(themeName)});
}
} // namespace deskflow::gui

inline QFont fixedFont()
{
#if defined(Q_OS_WIN)
  QFont f({"Hack", "Liberation Mono", "Monospace", "Andale Mono"});
  f.setStyleHint(QFont::Monospace);
#else
  QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
#endif

#if defined(Q_OS_MACOS)
  f.setPointSize(12);
#endif
  return f;
}
