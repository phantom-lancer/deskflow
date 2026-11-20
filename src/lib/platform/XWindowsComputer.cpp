/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/Settings.h" // must include first

#include "platform/XWindowsComputer.h"

#include "arch/Arch.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "base/Stopwatch.h"
#include "deskflow/App.h"
#include "deskflow/ClientApp.h"
#include "deskflow/Clipboard.h"
#include "deskflow/DisplayException.h"
#include "deskflow/DragProbe.h"
#include "deskflow/FileTransfer.h"
#include "deskflow/KeyMap.h"
#include "platform/XDGKeyUtil.h"
#include "platform/XWindowsClipboard.h"
#include "platform/XWindowsConfig.h"
#include "platform/XWindowsEventQueueBuffer.h"
#include "platform/XWindowsKeyState.h"
#include "platform/XWindowsScreenSaver.h"
#include "platform/XWindowsUtil.h"

#include <X11/X.h>
#include <X11/Xutil.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#define XK_MISCELLANY
#define XK_XKB_KEYS
#include <X11/keysymdef.h>
#if HAVE_X11_EXTENSIONS_DPMS_H
extern "C"
{
#include <X11/extensions/dpms.h>
}
#endif
#if HAVE_X11_EXTENSIONS_XTEST_H
#include <X11/extensions/XTest.h>
#else
#error The XTest extension is required to build deskflow
#endif
#if HAVE_X11_EXTENSIONS_XINERAMA_H
// Xinerama.h may lack extern "C" for inclusion by C++
extern "C"
{
#include <X11/extensions/Xinerama.h>
}
#endif
#if HAVE_X11_EXTENSIONS_XRANDR_H
#include <X11/extensions/Xrandr.h>
#endif
#if HAVE_XKB_EXTENSION
#include <X11/XKBlib.h>
#endif
#ifdef HAVE_XI2
#include <X11/extensions/XInput2.h>
#endif

static int xi_opcode;

//
// XWindowsComputer
//

// NOTE -- the X display is shared among several objects but is owned
// by the XWindowsComputer.  Xlib is not reentrant so we must ensure
// that no two objects can simultaneously call Xlib with the display.
// this is easy since we only make X11 calls from the main thread.
// we must also ensure that these objects do not use the display in
// their destructors or, if they do, we can tell them not to.  This
// is to handle unexpected disconnection of the X display, when any
// call on the display is invalid.  In that situation we discard the
// display and the X11 event queue buffer, ignore any calls that try
// to use the display, and wait to be destroyed.

XWindowsComputer *XWindowsComputer::s_computer = nullptr;

XWindowsComputer::XWindowsComputer(const char *displayName, bool isPrimary, IEventQueue *events)
    : PlatformComputer(events),
      m_isPrimary(isPrimary),
      m_isOnComputer(m_isPrimary),
      m_events(events)
{
  assert(s_computer == nullptr);
  s_computer = this;

  if (XInitThreads() == 0) {
    throw std::runtime_error("XInitThreads() returned zero");
  }

  // set the X I/O error handler so we catch the display disconnecting
  XSetIOErrorHandler(&XWindowsComputer::ioErrorHandler);

  try {
    m_display = openDisplay(displayName);
    m_root = DefaultRootWindow(m_display);
    saveShape();
    m_window = openWindow();
    m_screensaver = new XWindowsScreenSaver(m_display, m_window, getEventTarget(), events);
    m_keyState = new XWindowsKeyState(m_display, m_xkb, events, m_keyMap);
    LOG_DEBUG("computer shape: %d,%d %dx%d %s", m_x, m_y, m_w, m_h, m_xinerama ? "(xinerama)" : "");
    LOG_DEBUG("window is 0x%08x", m_window);
  } catch (...) {
    if (m_display != nullptr) {
      XCloseDisplay(m_display);
    }
    throw;
  }

  // primary/secondary computer only initialization
  if (m_isPrimary) {
#ifdef HAVE_XI2
    m_xi2detected = detectXI2();
    if (m_xi2detected) {
      selectXIRawMotion();
    } else
#endif
    {
      // start watching for events on other windows
      selectEvents(m_root);
    }

    // prepare to use input methods
    openIM();
  } else {
    // become impervious to server grabs
    XTestGrabControl(m_display, True);
  }

  // disable sleep if the flag is set
  if (Settings::value(Settings::Core::PreventSleep).toBool()) {
    m_powerManager.disableSleep();
  }

  // initialize the clipboards
  for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
    m_clipboard[id] = new XWindowsClipboard(m_display, m_window, id);
  }

  // install event handlers
  m_events->addHandler(EventTypes::System, m_events->getSystemTarget(), [this](const auto &e) {
    handleSystemEvent(e);
  });

  // install the platform event queue
  m_events->adoptBuffer(new XWindowsEventQueueBuffer(m_display, m_window, m_events));
}

XWindowsComputer::~XWindowsComputer()
{
  assert(s_computer != nullptr);
  assert(m_display != nullptr);

  m_events->adoptBuffer(nullptr);
  m_events->removeHandler(EventTypes::System, m_events->getSystemTarget());
  for (auto clipboard : m_clipboard) {
    delete clipboard;
  }
  delete m_keyState;
  delete m_screensaver;
  m_keyState = nullptr;
  m_screensaver = nullptr;
  if (m_display != nullptr) {
    // FIXME -- is it safe to clean up the IC and IM without a display?
    if (m_ic != nullptr) {
      XDestroyIC(m_ic);
    }
    if (m_im != nullptr) {
      XCloseIM(m_im);
    }
    XDestroyWindow(m_display, m_window);
    XCloseDisplay(m_display);
  }
  XSetIOErrorHandler(nullptr);

  s_computer = nullptr;
}

void XWindowsComputer::enable()
{
  if (!m_isPrimary) {
    // get the keyboard control state
    XKeyboardState keyControl;
    XGetKeyboardControl(m_display, &keyControl);
    m_autoRepeat = (keyControl.global_auto_repeat == AutoRepeatModeOn);
    m_keyState->setAutoRepeat(keyControl);

    // move hider window under the cursor center
    XMoveWindow(m_display, m_window, m_xCenter, m_yCenter);

    // raise and show the window
    // FIXME -- take focus?
    XMapRaised(m_display, m_window);

    // warp the mouse to the cursor center
    fakeMouseMove(m_xCenter, m_yCenter);
  }
}

void XWindowsComputer::disable()
{
  // release input context focus
  if (m_ic != nullptr) {
    XUnsetICFocus(m_ic);
  }

  // unmap the hider/grab window.  this also ungrabs the mouse and
  // keyboard if they're grabbed.
  XUnmapWindow(m_display, m_window);

  // restore auto-repeat state
  if (!m_isPrimary && m_autoRepeat) {
    // XAutoRepeatOn(m_display);
  }
}

void XWindowsComputer::enter()
{
  screensaver(false);

  // release input context focus
  if (m_ic != nullptr) {
    XUnsetICFocus(m_ic);
  }

  // set the input focus to what it had been when we took it
  if (m_lastFocus != None) {
    // the window may not exist anymore so ignore errors
    XWindowsUtil::ErrorLock lock(m_display);
    XSetInputFocus(m_display, m_lastFocus, m_lastFocusRevert, CurrentTime);
  }

#if HAVE_X11_EXTENSIONS_DPMS_H
  // Force the DPMS to turn computer back on since we don't
  // actually cause physical hardware input to trigger it
  int dummy;
  CARD16 powerlevel;
  if (BOOL enabled; DPMSQueryExtension(m_display, &dummy, &dummy) && DPMSCapable(m_display) &&
                    DPMSInfo(m_display, &powerlevel, &enabled)) {
    if (enabled && powerlevel != DPMSModeOn)
      DPMSForceLevel(m_display, DPMSModeOn);
  }
#endif

  // unmap the hider/grab window.  this also ungrabs the mouse and
  // keyboard if they're grabbed.
  XUnmapWindow(m_display, m_window);

  // maybe call this if entering for the screensaver
  // set keyboard focus to root window.  the screensaver should then
  // pick up key events for when the user enters a password to unlock.
  // XSetInputFocus(m_display, PointerRoot, PointerRoot, CurrentTime);

  if (!m_isPrimary) {
    // get the keyboard control state
    XKeyboardState keyControl;
    XGetKeyboardControl(m_display, &keyControl);
    m_autoRepeat = (keyControl.global_auto_repeat == AutoRepeatModeOn);
    m_keyState->setAutoRepeat(keyControl);

    // turn off auto-repeat.  we do this so fake key press events don't
    // cause the local server to generate their own auto-repeats of
    // those keys.
    // XAutoRepeatOff(m_display);
  }

  // now on computer
  m_isOnComputer = true;
}

bool XWindowsComputer::canLeave()
{
  // raise and show the window, required to grab mouse and keyboard
  XMapRaised(m_display, m_window);

  // see if grabbing the mouse and keyboard, if primary, is possible
  return !(m_isPrimary && !grabMouseAndKeyboard());
}

void XWindowsComputer::leave()
{
  if (!m_isPrimary) {
    // restore the previous keyboard auto-repeat state.  if the user
    // changed the auto-repeat configuration while on the client then
    // that state is lost.  that's because we can't get notified by
    // the X server when the auto-repeat configuration is changed so
    // we can't track the desired configuration.
    if (m_autoRepeat) {
      // XAutoRepeatOn(m_display);
    }

    // move hider window under the cursor center
    XMoveWindow(m_display, m_window, m_xCenter, m_yCenter);
  }

  // raise and show the window
  XMapRaised(m_display, m_window);

  // grab the mouse and keyboard, if primary and possible
  if (m_isPrimary && !grabMouseAndKeyboard()) {
    XUnmapWindow(m_display, m_window);
  }

  // save current focus
  XGetInputFocus(m_display, &m_lastFocus, &m_lastFocusRevert);

  // take focus
  if (m_isPrimary || !m_weakFocus) {
    XSetInputFocus(m_display, m_window, RevertToPointerRoot, CurrentTime);
  }

  // now warp the mouse.  we warp after showing the window so we're
  // guaranteed to get the mouse leave event and to prevent the
  // keyboard focus from changing under point-to-focus policies.
  if (m_isPrimary) {
    warpCursor(m_xCenter, m_yCenter);
  } else {
    // WARN: not using fakeMouseMove() intentionally
    // forcibly ignore any xinerama quirks, so that `xrandr --panning ... --tracking ...` works
    XTestFakeMotionEvent(m_display, DefaultScreen(m_display), m_xCenter, m_yCenter, CurrentTime);
    XFlush(m_display);
  }

  // set input context focus to our window
  if (m_ic != nullptr) {
    XmbResetIC(m_ic);
    XSetICFocus(m_ic);
    m_filtered.clear();
  }

  // now off computer
  m_isOnComputer = false;
}

bool XWindowsComputer::setClipboard(ClipboardID id, const IClipboard *clipboard)
{
  // fail if we don't have the requested clipboard
  if (m_clipboard[id] == nullptr) {
    return false;
  }

  // get the actual time.  ICCCM does not allow CurrentTime.
  Time timestamp = XWindowsUtil::getCurrentTime(m_display, m_clipboard[id]->getWindow());

  if (clipboard != nullptr) {
    // save clipboard data
    return Clipboard::copy(m_clipboard[id], clipboard, timestamp);
  } else {
    // assert clipboard ownership
    if (!m_clipboard[id]->open(timestamp)) {
      return false;
    }
    m_clipboard[id]->empty();
    m_clipboard[id]->close();
    return true;
  }
}

std::vector<std::string> XWindowsComputer::getClipboardFiles() const
{
  if (m_clipboard[kClipboardClipboard] == nullptr) {
    return {};
  }

  const Time timestamp = XWindowsUtil::getCurrentTime(m_display, m_clipboard[kClipboardClipboard]->getWindow());
  const std::string uris = m_clipboard[kClipboardClipboard]->getFileUris(timestamp);
  return deskflow::filetransfer::fileUriListToPaths(uris);
}

bool XWindowsComputer::setClipboardFiles(const std::vector<std::string> &paths)
{
  if (m_clipboard[kClipboardClipboard] == nullptr) {
    return false;
  }

  const std::string uris = deskflow::filetransfer::pathsToFileUriList(paths);
  const Time timestamp = XWindowsUtil::getCurrentTime(m_display, m_clipboard[kClipboardClipboard]->getWindow());
  return m_clipboard[kClipboardClipboard]->setFileUris(timestamp, uris);
}

std::vector<std::string> XWindowsComputer::getDraggedFiles() const
{
  if (!m_isOnComputer) {
    return {};
  }

  // Both callers reach this method only while a mouse button is held:
  // Server::isLockedToComputer() on the primary and ServerProxyHopflow::onMouseMoved() on clients.
  // Do not issue another synchronous XQueryPointer round-trip here.
  int32_t probeX = m_xCursor;
  int32_t probeY = m_yCursor;
  if (isPrimary()) {
    // On the server the physical cursor reaches the edge directly.
    const bool atEdge = probeX <= m_x || probeY <= m_y || probeX >= (m_x + m_w - 1) ||
                        probeY >= (m_y + m_h - 1);
    if (!atEdge) {
      deskflow::dragprobe::stop();
      return {};
    }
  } else {
    // A client cursor is warped back to the center and motion is sent as deltas.
    // The Qt drag probe must therefore sit under the real, re-centered pointer.
    getCursorCenter(probeX, probeY);
  }

  deskflow::dragprobe::start(probeX, probeY);
  auto paths = deskflow::dragprobe::files();
  if (!paths.empty()) {
    m_fileDragActive = true;
  }
  return paths;
}

void XWindowsComputer::cancelDrag()
{
  if (!m_fileDragActive) {
    deskflow::dragprobe::stop();
    return;
  }

  const KeyCode escape = XKeysymToKeycode(m_display, XK_Escape);
  if (escape != 0) {
    XTestFakeKeyEvent(m_display, escape, True, CurrentTime);
    XTestFakeKeyEvent(m_display, escape, False, CurrentTime);
    XFlush(m_display);
  }

  // On a secondary computer this is a synthetic button held by Deskflow,
  // not the user's physical button on the primary computer.
  if (!m_isPrimary) {
    fakeMouseButton(kButtonLeft, false);
  }

  m_fileDragActive = false;
  deskflow::dragprobe::stop();
}

void XWindowsComputer::checkClipboards()
{
  // do nothing, we're always up to date
}

void XWindowsComputer::openScreensaver(bool notify)
{
  m_screensaverNotify = notify;
  if (!m_screensaverNotify) {
    m_screensaver->disable();
  }
}

void XWindowsComputer::closeScreensaver()
{
  if (!m_screensaverNotify) {
    m_screensaver->enable();
  }
}

void XWindowsComputer::screensaver(bool activate)
{
  if (activate) {
    m_screensaver->activate();
  } else {
    m_screensaver->deactivate();
  }
}

void XWindowsComputer::resetOptions()
{
  m_xtestIsXineramaUnaware = true;
  m_weakFocus = false;
}

void XWindowsComputer::setOptions(const OptionsList &options)
{
  if (options.size() % 2 != 0) {
    LOG_ERR("options are the incorrect size, can not process them");
    return;
  }
  for (uint32_t i = 0, n = options.size(); i < n; i += 2) {
    if (options[i] == kOptionXTestXineramaUnaware) {
      m_xtestIsXineramaUnaware = (options[i + 1] != 0);
      LOG_VERBOSE("library, XTest is Xinerama unaware %s", m_xtestIsXineramaUnaware ? "true" : "false");
    } else if (options[i] == kOptionComputerX11WeakFocus) {
      m_weakFocus = (options[i + 1] != 0);
      LOG_VERBOSE("preserve focus: %s", m_weakFocus ? "true" : "false");
    }
  }
}

void XWindowsComputer::setSequenceNumber(uint32_t seqNum)
{
  m_sequenceNumber = seqNum;
}

bool XWindowsComputer::isPrimary() const
{
  return m_isPrimary;
}

std::string XWindowsComputer::getSecureInputApp() const
{
  // ignore on Linux
  return "";
}

void *XWindowsComputer::getEventTarget() const
{
  return const_cast<XWindowsComputer *>(this);
}

bool XWindowsComputer::getClipboard(ClipboardID id, IClipboard *clipboard) const
{
  assert(clipboard != nullptr);

  // fail if we don't have the requested clipboard
  if (m_clipboard[id] == nullptr) {
    return false;
  }

  // get the actual time.  ICCCM does not allow CurrentTime.
  Time timestamp = XWindowsUtil::getCurrentTime(m_display, m_clipboard[id]->getWindow());

  // copy the clipboard
  return Clipboard::copy(clipboard, m_clipboard[id], timestamp);
}

void XWindowsComputer::getShape(int32_t &x, int32_t &y, int32_t &w, int32_t &h) const
{
  x = m_x;
  y = m_y;
  w = m_w;
  h = m_h;
}

void XWindowsComputer::getCursorPos(int32_t &x, int32_t &y) const
{
  Window root;
  Window window;
  int mx;
  int my;
  int xWindow;
  int yWindow;
  unsigned int mask;
  if (XQueryPointer(m_display, m_root, &root, &window, &mx, &my, &xWindow, &yWindow, &mask)) {
    x = mx;
    y = my;
  } else {
    x = m_xCenter;
    y = m_yCenter;
  }
}

void XWindowsComputer::reconfigure(uint32_t activeSides)
{
  const static auto sidesText = sidesMaskToString(activeSides);
  LOG_DEBUG("active sides: %s (0x%02x)", sidesText.c_str(), activeSides);
  m_activeSides = activeSides;
}

uint32_t XWindowsComputer::activeSides()
{
  return m_activeSides;
}

void XWindowsComputer::warpCursor(int32_t x, int32_t y)
{
  // warp mouse
  warpCursorNoFlush(x, y);

  // remove all input events before and including warp
  XEvent event;
  while (XCheckMaskEvent(
      m_display,
      PointerMotionMask | ButtonPressMask | ButtonReleaseMask | KeyPressMask | KeyReleaseMask | KeymapStateMask, &event
  )) {
    // do nothing
  }

  // save position as last position
  m_xCursor = x;
  m_yCursor = y;
}

uint32_t XWindowsComputer::registerHotKey(KeyID key, KeyModifierMask mask)
{
  // only allow certain modifiers
  if ((mask & ~(KeyModifierShift | KeyModifierControl | KeyModifierAlt | KeyModifierSuper)) != 0) {
    LOG_DEBUG("could not map hotkey id=%04x mask=%04x", key, mask);
    return 0;
  }

  // fail if no keys
  if (key == kKeyNone && mask == 0) {
    return 0;
  }

  // convert to X
  unsigned int modifiers;
  if (!m_keyState->mapModifiersToX(mask, modifiers)) {
    // can't map all modifiers
    LOG_DEBUG("could not map hotkey id=%04x mask=%04x", key, mask);
    return 0;
  }
  XWindowsKeyState::KeycodeList keycodes;
  m_keyState->mapKeyToKeycodes(key, keycodes);
  if (key != kKeyNone && keycodes.empty()) {
    // can't map key
    LOG_DEBUG("could not map hotkey id=%04x mask=%04x", key, mask);
    return 0;
  }

  // choose hotkey id
  uint32_t id;
  if (!m_oldHotKeyIDs.empty()) {
    id = m_oldHotKeyIDs.back();
    m_oldHotKeyIDs.pop_back();
  } else {
    id = m_hotKeys.size() + 1;
  }
  HotKeyList &hotKeys = m_hotKeys[id];

  // all modifier hotkey must be treated specially.  for each modifier
  // we need to grab the modifier key in combination with all the other
  // requested modifiers.
  bool err = false;
  {
    XWindowsUtil::ErrorLock lock(m_display, &err);
    if (key == kKeyNone) {
      static const KeyModifierMask s_hotKeyModifiers[] = {
          KeyModifierShift, KeyModifierControl, KeyModifierAlt, KeyModifierMeta, KeyModifierSuper
      };

      XModifierKeymap *modKeymap = XGetModifierMapping(m_display);
      for (size_t j = 0; j < std::size(s_hotKeyModifiers) && !err; ++j) {
        // skip modifier if not in mask
        if ((mask & s_hotKeyModifiers[j]) == 0) {
          continue;
        }

        // skip with error if we can't map remaining modifiers
        unsigned int modifiers2;
        if (KeyModifierMask mask2 = (mask & ~s_hotKeyModifiers[j]); !m_keyState->mapModifiersToX(mask2, modifiers2)) {
          err = true;
          continue;
        }

        // compute modifier index for modifier.  there should be
        // exactly one X modifier missing
        int index;
        switch (modifiers ^ modifiers2) {
        case ShiftMask:
          index = ShiftMapIndex;
          break;

        case LockMask:
          index = LockMapIndex;
          break;

        case ControlMask:
          index = ControlMapIndex;
          break;

        case Mod1Mask:
          index = Mod1MapIndex;
          break;

        case Mod2Mask:
          index = Mod2MapIndex;
          break;

        case Mod3Mask:
          index = Mod3MapIndex;
          break;

        case Mod4Mask:
          index = Mod4MapIndex;
          break;

        case Mod5Mask:
          index = Mod5MapIndex;
          break;

        default:
          err = true;
          continue;
        }

        // grab each key for the modifier
        const KeyCode *modifiermap = modKeymap->modifiermap + index * modKeymap->max_keypermod;
        for (int k = 0; k < modKeymap->max_keypermod && !err; ++k) {
          KeyCode code = modifiermap[k];
          if (modifiermap[k] != 0) {
            XGrabKey(m_display, code, modifiers2, m_root, False, GrabModeAsync, GrabModeAsync);
            if (!err) {
              hotKeys.push_back(std::make_pair(code, modifiers2));
              m_hotKeyToIDMap[HotKeyItem(code, modifiers2)] = id;
            }
          }
        }
      }
      XFreeModifiermap(modKeymap);
    }

    // a non-modifier key must be insensitive to CapsLock, NumLock and
    // ScrollLock, so we have to grab the key with every combination of
    // those.
    else {
      // collect available toggle modifiers
      unsigned int modifier;
      unsigned int toggleModifiers[3];
      size_t numToggleModifiers = 0;
      if (m_keyState->mapModifiersToX(KeyModifierCapsLock, modifier)) {
        toggleModifiers[numToggleModifiers++] = modifier;
      }
      if (m_keyState->mapModifiersToX(KeyModifierNumLock, modifier)) {
        toggleModifiers[numToggleModifiers++] = modifier;
      }
      if (m_keyState->mapModifiersToX(KeyModifierScrollLock, modifier)) {
        toggleModifiers[numToggleModifiers++] = modifier;
      }

      for (auto j = keycodes.begin(); j != keycodes.end() && !err; ++j) {
        for (size_t i = 0; i < (1u << numToggleModifiers); ++i) {
          // add toggle modifiers for index i
          unsigned int tmpModifiers = modifiers;
          if ((i & 1) != 0) {
            tmpModifiers |= toggleModifiers[0];
          }
          if ((i & 2) != 0) {
            tmpModifiers |= toggleModifiers[1];
          }
          if ((i & 4) != 0) {
            tmpModifiers |= toggleModifiers[2];
          }

          // add grab
          XGrabKey(m_display, *j, tmpModifiers, m_root, False, GrabModeAsync, GrabModeAsync);
          if (!err) {
            hotKeys.push_back(std::make_pair(*j, tmpModifiers));
            m_hotKeyToIDMap[HotKeyItem(*j, tmpModifiers)] = id;
          }
        }
      }
    }
  }

  if (err) {
    // if any failed then unregister any we did get
    for (const auto &[keyCode, keyMask] : hotKeys) {
      XUngrabKey(m_display, keyCode, keyMask, m_root);
      m_hotKeyToIDMap.erase(HotKeyItem(keyCode, keyMask));
    }

    m_oldHotKeyIDs.push_back(id);
    m_hotKeys.erase(id);
    LOG(
        (CLOG_WARN "failed to register hotkey %s (id=%04x mask=%04x)", deskflow::KeyMap::formatKey(key, mask).c_str(),
         key, mask)
    );
    return 0;
  }

  LOG(
      (CLOG_DEBUG "registered hotkey %s (id=%04x mask=%04x) as id=%d", deskflow::KeyMap::formatKey(key, mask).c_str(),
       key, mask, id)
  );
  return id;
}

void XWindowsComputer::unregisterHotKey(uint32_t id)
{
  // look up hotkey
  HotKeyMap::iterator i = m_hotKeys.find(id);
  if (i == m_hotKeys.end()) {
    return;
  }

  // unregister with OS
  bool err = false;
  {
    XWindowsUtil::ErrorLock lock(m_display, &err);
    const HotKeyList &hotKeys = i->second;
    for (const auto &[keyCode, keyMask] : hotKeys) {
      XUngrabKey(m_display, keyCode, keyMask, m_root);
      m_hotKeyToIDMap.erase(HotKeyItem(keyCode, keyMask));
    }
  }
  if (err) {
    LOG_WARN("failed to unregister hotkey id=%d", id);
  } else {
    LOG_DEBUG("unregistered hotkey id=%d", id);
  }

  // discard hot key from map and record old id for reuse
  m_hotKeys.erase(i);
  m_oldHotKeyIDs.push_back(id);
}

void XWindowsComputer::fakeInputBegin()
{
  // FIXME -- not implemented
}

void XWindowsComputer::fakeInputEnd()
{
  // FIXME -- not implemented
}

int32_t XWindowsComputer::getJumpZoneSize() const
{
  return 1;
}

bool XWindowsComputer::isAnyMouseButtonDown(uint32_t &) const
{
  // query the pointer to get the button state
  Window root;
  Window window;
  int xRoot;
  int yRoot;
  int xWindow;
  int yWindow;
  if (unsigned int state;
      XQueryPointer(m_display, m_root, &root, &window, &xRoot, &yRoot, &xWindow, &yWindow, &state)) {
    return ((state & (Button1Mask | Button2Mask | Button3Mask | Button4Mask | Button5Mask)) != 0);
  }

  return false;
}

void XWindowsComputer::getCursorCenter(int32_t &x, int32_t &y) const
{
  x = m_xCenter;
  y = m_yCenter;
}

void XWindowsComputer::fakeMouseButton(ButtonID button, bool press)
{
  const unsigned int xButton = mapButtonToX(button);
  if (xButton > 0 && xButton < 11) {
    XTestFakeButtonEvent(m_display, xButton, press ? True : False, CurrentTime);
    XFlush(m_display);
  }
}

void XWindowsComputer::fakeMouseMove(int32_t x, int32_t y)
{
  if (m_xinerama && m_xtestIsXineramaUnaware) {
    XWarpPointer(m_display, None, m_root, 0, 0, 0, 0, x, y);
  } else {
    XTestFakeMotionEvent(m_display, DefaultScreen(m_display), x, y, CurrentTime);
  }
  XFlush(m_display);
}

void XWindowsComputer::fakeMouseRelativeMove(int32_t dx, int32_t dy) const
{
  // FIXME -- ignore xinerama for now
  XTestFakeRelativeMotionEvent(m_display, dx, dy, CurrentTime);
  XFlush(m_display);
}

void XWindowsComputer::fakeMouseWheel(ScrollDelta delta) const
{
  if (delta.y == 0 && delta.x == 0) {
    return;
  }
  delta = applyScrollModifier(delta);
  bool isYScroll = delta.y != 0;
  int32_t axisDelta = delta.y == 0 ? delta.x : delta.y;

  ButtonID btn = kButtonNone;
  if (isYScroll) {
    btn = axisDelta >= 0 ? kX11ScrollWheelUp : kX11ScrollWheelDown;
  } else {
    btn = axisDelta >= 0 ? kX11ScrollWheelLeft : kX11ScrollWheelRight;
  }
  const unsigned int button = mapButtonToX(btn);

  // If the XServer does not support the Y scroll send PageUp/PageDown keystrokes instead.
  if (const KeyCode keycode = XKeysymToKeycode(m_display, delta.y >= 0 ? XK_Page_Up : XK_Page_Down);
      button == 0 && isYScroll && keycode != 0) {
    XTestFakeKeyEvent(m_display, keycode, True, CurrentTime);
    XTestFakeKeyEvent(m_display, keycode, False, CurrentTime);
    return;
  }

  // now use absolute value of delta
  if (axisDelta < 0) {
    axisDelta = -axisDelta;
  }

  // send as many clicks as necessary
  while (axisDelta > 0) {
    XTestFakeButtonEvent(m_display, button, True, CurrentTime);
    XTestFakeButtonEvent(m_display, button, False, CurrentTime);
    axisDelta -= s_scrollDelta;
  }
  XFlush(m_display);
}

Display *XWindowsComputer::openDisplay(const char *displayName)
{
  // get the DISPLAY
  if (displayName == nullptr) {
    displayName = std::getenv("DISPLAY");
    if (displayName == nullptr) {
      displayName = ":0.0";
    }
  }

  // open the display
  LOG_VERBOSE("calling XOpenDisplay(\"%s\")", displayName);
  Display *display = XOpenDisplay(displayName);
  if (display == nullptr) {
    throw X11DisplayUnavailableException();
  }

  // verify the availability of the XTest extension
  if (!m_isPrimary) {
    int majorOpcode;
    int firstEvent;
    int firstError;
    if (!XQueryExtension(display, XTestExtensionName, &majorOpcode, &firstEvent, &firstError)) {
      LOG_ERR("the XTest extension is not available");
      XCloseDisplay(display);
      throw DisplayOpenFailureException();
    }
  }

#if HAVE_XKB_EXTENSION
  {
    m_xkb = false;
    int major = XkbMajorVersion;
    int minor = XkbMinorVersion;
    if (XkbLibraryVersion(&major, &minor)) {
      int opcode;
      int firstError;
      if (XkbQueryExtension(display, &opcode, &m_xkbEventBase, &firstError, &major, &minor)) {
        m_xkb = true;
        XkbSelectEvents(display, XkbUseCoreKbd, XkbMapNotifyMask, XkbMapNotifyMask);
        XkbSelectEventDetails(display, XkbUseCoreKbd, XkbStateNotifyMask, XkbGroupStateMask, XkbGroupStateMask);
      }
    }
  }
#endif

#if HAVE_X11_EXTENSIONS_XRANDR_H
  // query for XRandR extension
  int dummyError;
  m_xrandr = XRRQueryExtension(display, &m_xrandrEventBase, &dummyError);
  if (m_xrandr) {
    // enable XRRScreenChangeNotifyEvent
    XRRSelectInput(display, DefaultRootWindow(display), RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask);
  }
#endif

  return display;
}

void XWindowsComputer::saveShape()
{
  // get shape of default computer
  setShape(WidthOfScreen(DefaultScreenOfDisplay(m_display)), HeightOfScreen(DefaultScreenOfDisplay(m_display)));
}

void XWindowsComputer::setShape(int32_t width, int32_t height)
{
  // set shape
  m_x = 0;
  m_y = 0;

  m_w = width;
  m_h = height;

  // get center of computer
  m_xCenter = m_x + (m_w >> 1);
  m_yCenter = m_y + (m_h >> 1);

  // check if xinerama is enabled and there is more than one screen.
  // get center of first Xinerama screen.  Xinerama appears to have
  // a bug when XWarpPointer() is used in combination with
  // XGrabPointer().  in that case, the warp is successful but the
  // next pointer motion warps the pointer again, apparently to
  // constrain it to some unknown region, possibly the region from
  // 0,0 to Wm,Hm where Wm (Hm) is the minimum width (height) over
  // all physical screens.  this warp only seems to happen if the
  // pointer wasn't in that region before the XWarpPointer().  the
  // second (unexpected) warp causes deskflow to think the pointer
  // has been moved when it hasn't.  to work around the problem,
  // we warp the pointer to the center of the first physical
