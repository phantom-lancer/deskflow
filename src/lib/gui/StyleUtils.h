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
#include "common/Settings.h"

#include <QFile>

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

enum class ThemeMode
{
  System,
  Light,
  Dark
};

//! Theme chosen in the settings; System follows the operating system
inline ThemeMode themeMode()
{
  const auto value = Settings::value(Settings::Gui::Theme).toString();
  if (value == QStringLiteral("light"))
    return ThemeMode::Light;
  if (value == QStringLiteral("dark"))
    return ThemeMode::Dark;
  return ThemeMode::System;
}

inline bool systemIsDark()
{
  // Ask the platform first: this keeps working after a custom palette is applied.
  const auto scheme = QGuiApplication::styleHints()->colorScheme();
  if (scheme != Qt::ColorScheme::Unknown)
    return scheme == Qt::ColorScheme::Dark;

  // Fall back to the palette; cache it before applyDeskflowTheme() replaces it.
  static const bool dark = isDarkMode();
  return dark;
}

inline bool isDarkTheme()
{
  const auto mode = themeMode();
  if (mode == ThemeMode::Dark)
    return true;
  if (mode == ThemeMode::Light)
    return false;
  return systemIsDark();
}

inline void applyDeskflowTheme(QApplication &app)
{
#if defined(Q_OS_WIN)
  const bool dark = isDarkTheme();
  QPalette palette = app.palette();
  if (dark) {
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#20242b")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#e8ebf1")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#1c2026")));
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#242a33")));
    palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#2b313b")));
    palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#eef2f7")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#e8ebf1")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#303742")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#e8ebf1")));
    palette.setColor(QPalette::Light, QColor(QStringLiteral("#3a424e")));
    palette.setColor(QPalette::Midlight, QColor(QStringLiteral("#333a44")));
    palette.setColor(QPalette::Mid, QColor(QStringLiteral("#2a3039")));
    palette.setColor(QPalette::Dark, QColor(QStringLiteral("#161a1f")));
    palette.setColor(QPalette::Shadow, QColor(QStringLiteral("#101317")));
    palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#315f91")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::Link, QColor(QStringLiteral("#6b9edb")));
    palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#8a93a0")));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#7c8590")));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#7c8590")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#7c8590")));
  } else {
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#f5f6f8")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#20242b")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#f6f8fa")));
    palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#fffefb")));
    palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#20242b")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#20242b")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#20242b")));
    palette.setColor(QPalette::Light, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::Midlight, QColor(QStringLiteral("#eceff3")));
    palette.setColor(QPalette::Mid, QColor(QStringLiteral("#d7dbe1")));
    palette.setColor(QPalette::Dark, QColor(QStringLiteral("#b9bfc8")));
    palette.setColor(QPalette::Shadow, QColor(QStringLiteral("#9aa1aa")));
    palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#2878c8")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::Link, QColor(QStringLiteral("#2878c8")));
    palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#6b7280")));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#8c949f")));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#8c949f")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#8c949f")));
  }
  app.setPalette(palette);

  // Keep the native Windows widget style; the stylesheet only tunes surfaces,
  // spacing and the accent color, as Windows 11 Fluent does.
  QFile themeFile(
      dark ? QStringLiteral(":/themes/windows-fluent-dark.qss") : QStringLiteral(":/themes/windows-fluent.qss")
  );
  if (themeFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
    app.setStyleSheet(QString::fromUtf8(themeFile.readAll()));
  }
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
