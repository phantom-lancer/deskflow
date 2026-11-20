/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ActionTests.h"

#include "common/Action.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

namespace {

Action makeMediaAction()
{
  Action action;
  KeySequence sequence;
  sequence.appendKey(Qt::Key_MediaPlay, 0);
  action.setKeySequence(sequence);
  action.setType(static_cast<int>(Action::Type::keystroke));
  return action;
}

} // namespace

void ActionTests::activeComputerRoute()
{
  auto action = makeMediaAction();
  action.setUseActiveComputer(true);
  QCOMPARE(action.text(), QStringLiteral("keystroke(AudioPlay)"));
}

void ActionTests::allComputersRoute()
{
  const auto action = makeMediaAction();
  QCOMPARE(action.text(), QStringLiteral("keystroke(AudioPlay,*)"));
}

void ActionTests::selectedComputerRoute()
{
  auto action = makeMediaAction();
  action.setHaveComputers(true);
  action.addComputer(QStringLiteral("diffulinux"));
  QCOMPARE(action.text(), QStringLiteral("keystroke(AudioPlay,diffulinux)"));
}

void ActionTests::activeRoutePersists()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());

  auto action = makeMediaAction();
  action.setUseActiveComputer(true);

  QSettings settings(directory.filePath(QStringLiteral("action.ini")), QSettings::IniFormat);
  settings.beginGroup(QStringLiteral("action"));
  action.saveSettings(settings);
  settings.endGroup();
  settings.sync();
  QCOMPARE(settings.status(), QSettings::NoError);

  Action loaded;
  settings.beginGroup(QStringLiteral("action"));
  loaded.loadSettings(settings);
  settings.endGroup();

  QVERIFY(loaded.useActiveComputer());
  QCOMPARE(loaded.text(), QStringLiteral("keystroke(AudioPlay)"));
}

QTEST_MAIN(ActionTests)
