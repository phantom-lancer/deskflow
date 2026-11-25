/*
 * Hopflow -- mouse, keyboard and file sharing, built on Deskflow
 * SPDX-FileCopyrightText: (C) 2026 Hopflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/Log.h"

#include <QObject>

class FileTransferRouterTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void clipboardDeliveredOnceOnEnter();
  void clipboardNotSentBackToOwner();
  void clipboardNotSentToDeskflowClient();
  void clipboardSentToLinux();
  void newClipboardWithoutFilesClearsOffer();
  void deskflowClientGrabClearsOffer();
  void lateOfferDeliveredToActiveComputer();
  void refreshOffersFilesCopiedWithoutGrab();
  void refreshWithSameFilesDoesNotResend();
  void relaysTransferBetweenClients();
  void ignoresMessagesFromWrongEnd();
  void refusesUnknownTransfer();
  void refusesTooLarge();
  void dropStartsTransfer();
  void dropOnSameComputerDoesNothing();
  void dragStartsTransferOnEnter();
  void dragRepeatedEnterDoesNotRestart();
  void dragLeavingTargetCancelsTransfer();
  void dragWithdrawnCancelsTransfer();
  void disconnectCancelsTransfer();
  void disabledDoesNothing();

private:
  Log m_log;
};
