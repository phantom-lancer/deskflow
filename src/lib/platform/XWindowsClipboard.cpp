/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "arch/Arch.h"
#include "base/Log.h"

#include "platform/XWindowsClipboard.h"

#include "base/Stopwatch.h"
#include "platform/XWindowsClipboardBMPConverter.h"
#include "platform/XWindowsClipboardHTMLConverter.h"
#include "platform/XWindowsClipboardTextConverter.h"
#include "platform/XWindowsClipboardUCS2Converter.h"
#include "platform/XWindowsClipboardUTF8Converter.h"
#include "platform/XWindowsUtil.h"

#include <X11/Xatom.h>
#include <algorithm>
#include <cstring>

#if HAVE_FORMAT
#include <format>
#endif

#include <vector>

//
// XWindowsClipboard
//

XWindowsClipboard::XWindowsClipboard(Display *display, Window window, ClipboardID id)
    : m_display(display),
      m_window(window),
      m_id(id)
{
  std::scoped_lock lock{m_mutex};
  // get some atoms
  m_atomTargets = XInternAtom(m_display, "TARGETS", False);
  m_atomUriList = XInternAtom(m_display, "text/uri-list", False);
  m_atomMultiple = XInternAtom(m_display, "MULTIPLE", False);
  m_atomTimestamp = XInternAtom(m_display, "TIMESTAMP", False);
  m_atomInteger = XInternAtom(m_display, "INTEGER", False);
  m_atomAtom = XInternAtom(m_display, "ATOM", False);
  m_atomAtomPair = XInternAtom(m_display, "ATOM_PAIR", False);
  m_atomData = XInternAtom(m_display, "CLIP_TEMPORARY", False);
  m_atomINCR = XInternAtom(m_display, "INCR", False);
  m_atomMotifClipLock = XInternAtom(m_display, "_MOTIF_CLIP_LOCK", False);
  m_atomMotifClipHeader = XInternAtom(m_display, "_MOTIF_CLIP_HEADER", False);
  m_atomMotifClipAccess = XInternAtom(m_display, "_MOTIF_CLIP_LOCK_ACCESS_VALID", False);
  m_atomGDKSelection = XInternAtom(m_display, "GDK_SELECTION", False);

  // set selection atom based on clipboard id
  if (id == kClipboardClipboard) {
    m_selection = XInternAtom(m_display, "CLIPBOARD", False);
  } else {
    m_selection = XA_PRIMARY;
  }

  // add converters, most desired first
  m_converters.push_back(new XWindowsClipboardHTMLConverter(m_display, "text/html"));
  m_converters.push_back(new XWindowsClipboardHTMLConverter(m_display, "application/x-moz-nativehtml"));
  m_converters.push_back(new XWindowsClipboardBMPConverter(m_display));
  m_converters.push_back(new XWindowsClipboardUTF8Converter(m_display, "text/plain;charset=UTF-8", true));
  m_converters.push_back(new XWindowsClipboardUTF8Converter(m_display, "text/plain;charset=utf-8", true));
  m_converters.push_back(new XWindowsClipboardUTF8Converter(m_display, "UTF8_STRING"));
  m_converters.push_back(new XWindowsClipboardUCS2Converter(m_display, "text/plain;charset=ISO-10646-UCS-2"));
  m_converters.push_back(new XWindowsClipboardUCS2Converter(m_display, "text/unicode"));
  m_converters.push_back(new XWindowsClipboardTextConverter(m_display, "text/plain"));
  m_converters.push_back(new XWindowsClipboardTextConverter(m_display, "STRING"));

  // we have no data
  clearCache();
}

XWindowsClipboard::~XWindowsClipboard()
{
  std::scoped_lock lock{m_mutex};
  clearReplies();
  clearConverters();
}

void XWindowsClipboard::lost(Time time)
{
  std::scoped_lock lock{m_mutex};
  LOG_DEBUG("lost clipboard %d ownership at %d", m_id, time);
  if (m_owner) {
    m_owner = false;
    m_timeLost = time;
    m_fileUris.clear();
    clearCache();
  }
}

void XWindowsClipboard::addRequest(Window owner, Window requestor, Atom target, ::Time time, Atom property)
{
  std::scoped_lock lock{m_mutex};
  // must be for our window and we must have owned the selection
  // at the given time.
  bool success = false;
  if (owner == m_window) {
    LOG_VERBOSE(
        "request for clipboard %d, target %s by 0x%08x (property=%s)", m_selection,
        XWindowsUtil::atomToString(m_display, target).c_str(), requestor,
        XWindowsUtil::atomToString(m_display, property).c_str()
    );
    if (wasOwnedAtTime(time)) {
      if (target == m_atomMultiple && property != None) {
        // add a multiple request.  property may not be None
        // according to ICCCM.
        success = insertMultipleReply(requestor, time, property);
      } else {
        addSimpleRequest(requestor, target, time, property);

        // addSimpleRequest() will have already handled failure
        success = true;
      }
    } else {
      LOG_VERBOSE("clipboard not owned at time %d", time);
    }
  }

  if (!success) {
    // send failure
    LOG_VERBOSE("clipboard request was not added");
    insertReply(new Reply(requestor, target, time));
  }

  // send notifications that are pending
  pushReplies();
}

bool XWindowsClipboard::addSimpleRequest(Window requestor, Atom target, ::Time time, Atom property)
{
  // obsolete requestors may supply a None property.  in
  // that case we use the target as the property to store
  // the conversion.
  if (property == None) {
    property = target;
  }

  // handle targets
  std::string data;
  Atom type = None;
  int format = 0;
  if (target == m_atomTargets) {
    type = getTargetsData(data, &format);
  } else if (target == m_atomUriList && !m_fileUris.empty()) {
    data = m_fileUris;
    format = 8;
    type = m_atomUriList;
  } else if (target == m_atomTimestamp) {
    type = getTimestampData(data, &format);
  } else {
    const IXWindowsClipboardConverter *converter = getConverter(target);
    if (converter != nullptr) {
      const auto clipboardFormat = static_cast<int>(converter->getFormat());
      if (m_added[clipboardFormat]) {
        try {
          data = converter->fromIClipboard(m_data[clipboardFormat]);
          format = converter->getDataSize();
          type = converter->getAtom();
        } catch (...) {
          // ignore -- cannot convert
          LOG_WARN("error while converting clipboard data");
        }
      }
    }
  }

  if (type != None) {
    // success
    LOG_VERBOSE("clipboard request added");
    insertReply(new Reply(requestor, target, time, property, data, type, format));
    return true;
  } else {
    // failure
    LOG_VERBOSE("clipboard request not added");
    insertReply(new Reply(requestor, target, time));
    return false;
  }
}

bool XWindowsClipboard::processRequest(Window requestor, ::Time /*time*/, Atom property)
{
  std::scoped_lock lock{m_mutex};
  ReplyMap::iterator index = m_replies.find(requestor);
  if (index == m_replies.end()) {
    // unknown requestor window
    return false;
  }
  LOG_VERBOSE(
      "received property %s delete from 0x08%x", XWindowsUtil::atomToString(m_display, property).c_str(), requestor
  );

  // find the property in the known requests.  it should be the
  // first property but we'll check 'em all if we have to.
  ReplyList &replies = index->second;
  for (auto index2 = replies.begin(); index2 != replies.end(); ++index2) {
    const Reply *reply = *index2;
    if (reply->m_replied && reply->m_property == property) {
      // if reply is complete then remove it and start the
      // next one.
      pushReplies(index, replies, index2);
      return true;
    }
  }

  return false;
}

bool XWindowsClipboard::destroyRequest(Window requestor)
{
  ReplyMap::iterator index = m_replies.find(requestor);
  if (index == m_replies.end()) {
    // unknown requestor window
    return false;
  }

  // destroy all replies for this window
  clearReplies(index->second);
  m_replies.erase(index);

  // note -- we don't stop watching the window for events because
  // we're called in response to the window being destroyed.

  return true;
}

Window XWindowsClipboard::getWindow() const
{
  return m_window;
}

Atom XWindowsClipboard::getSelection() const
{
  return m_selection;
}

bool XWindowsClipboard::empty()
{
  std::scoped_lock lock{m_mutex};
  assert(m_open);

  LOG_DEBUG("empty clipboard %d", m_id);

  // assert ownership of clipboard
  XSetSelectionOwner(m_display, m_selection, m_window, m_time);
  if (XGetSelectionOwner(m_display, m_selection) != m_window) {
    LOG_WARN("failed to grab clipboard %d", m_id);
    return false;
  }

  // clear all data.  since we own the data now, the cache is up
  // to date.
  m_fileUris.clear();
  clearCache();
  m_cached = true;

  // FIXME -- actually delete motif clipboard items?
  // FIXME -- do anything to motif clipboard properties?

  // save time
  m_timeOwned = m_time;
  m_timeLost = 0;

  // we're the owner now
  m_owner = true;
  LOG_DEBUG("grabbed clipboard %d", m_id);

  return true;
}

void XWindowsClipboard::add(Format format, const std::string &data)
{
  std::scoped_lock lock{m_mutex};
  assert(m_open);
  assert(m_owner);

  LOG_DEBUG("add %d bytes to clipboard %d format: %d", data.size(), m_id, format);

  const auto formatID = static_cast<int>(format);
  m_data[formatID] = data;
  m_added[formatID] = true;

  // FIXME -- set motif clipboard item?
}

bool XWindowsClipboard::open(Time time) const
{
  std::scoped_lock lock{m_mutex};
  if (m_open) {
    LOG_WARN("failed to open clipboard: already opened");
    return false;
  }

  LOG_DEBUG("open clipboard %d", m_id);

  // assume not motif
  m_motif = false;

  // lock clipboard
  if (m_id == kClipboardClipboard) {
    if (!motifLockClipboard()) {
      return false;
    }

    // check if motif owns the selection.  unlock motif clipboard
    // if it does not.
    m_motif = motifOwnsClipboard();
    LOG_VERBOSE("motif does %sown clipboard", m_motif ? "" : "not ");
    if (!m_motif) {
      motifUnlockClipboard();
    }
  }

  // now open
  m_open = true;
  m_time = time;

  // be sure to flush the cache later if it's dirty
  m_checkCache = true;

  return true;
}

void XWindowsClipboard::close() const
{
  std::scoped_lock lock{m_mutex};
  assert(m_open);

  LOG_DEBUG("close clipboard %d", m_id);

  // unlock clipboard
  if (m_motif) {
    motifUnlockClipboard();
  }

  m_motif = false;
  m_open = false;
}

IClipboard::Time XWindowsClipboard::getTime() const
{
  std::scoped_lock lock{m_mutex};
  checkCache();
  return m_timeOwned;
}

bool XWindowsClipboard::has(Format format) const
{
  std::scoped_lock lock{m_mutex};
  assert(m_open);

  fillCache();
  return m_added[static_cast<int>(format)];
}

std::string XWindowsClipboard::get(Format format) const
{
  std::scoped_lock lock{m_mutex};
  assert(m_open);

  fillCache();
  return m_data[static_cast<int>(format)];
}

std::string XWindowsClipboard::getFileUris(Time time) const
{
  if (!open(time)) {
    return {};
  }

  std::string uris;
  {
    std::scoped_lock lock{m_mutex};
    if (m_owner) {
      uris = m_fileUris;
    } else {
      Atom actualTarget = None;
      std::string data;
      if (icccmGetSelection(m_atomUriList, &actualTarget, &data) && actualTarget == m_atomUriList) {
        uris = std::move(data);
      }
    }
  }
  close();
  return uris;
}

bool XWindowsClipboard::setFileUris(Time time, const std::string &uris)
{
  if (!open(time)) {
    return false;
  }

  const bool success = empty();
  if (success) {
    std::scoped_lock lock{m_mutex};
    m_fileUris = uris;
  }
  close();
  return success;
}

void XWindowsClipboard::clearConverters()
{
  for (auto index = m_converters.begin(); index != m_converters.end(); ++index) {
    delete *index;
  }
  m_converters.clear();
}

IXWindowsClipboardConverter *XWindowsClipboard::getConverter(Atom target, bool onlyIfNotAdded) const
{
  IXWindowsClipboardConverter *converter = nullptr;
  for (auto index = m_converters.begin(); index != m_converters.end(); ++index) {
    converter = *index;
    if (converter->getAtom() == target) {
      break;
    }
  }
  if (converter == nullptr) {
    LOG_VERBOSE("  no converter for target %s", XWindowsUtil::atomToString(m_display, target).c_str());
    return nullptr;
  }

  // optionally skip already handled targets
  if (const auto formatID = static_cast<int>(converter->getFormat()); onlyIfNotAdded && m_added[formatID]) {
    LOG_VERBOSE("  skipping handled format %d", formatID);
    return nullptr;
  }

  return converter;
}

void XWindowsClipboard::checkCache() const
{
  if (!m_checkCache) {
    return;
  }
  m_checkCache = false;

  // get the time the clipboard ownership was taken by the current
  // owner.
  if (m_motif) {
    m_timeOwned = motifGetTime();
  } else {
    m_timeOwned = icccmGetTime();
  }

  // if we can't get the time then use the time passed to us
  if (m_timeOwned == 0) {
    m_timeOwned = m_time;
  }

  // if the cache is dirty then flush it
  if (m_timeOwned != m_cacheTime) {
    clearCache();
  }
}

void XWindowsClipboard::clearCache() const
{
  const_cast<XWindowsClipboard *>(this)->doClearCache();
}

void XWindowsClipboard::doClearCache()
{
  m_checkCache = false;
  m_cached = false;
  for (int32_t index = 0; index < static_cast<int>(Format::TotalFormats); ++index) {
    m_data[index] = "";
    m_added[index] = false;
  }
}

void XWindowsClipboard::fillCache() const
{
  // get the selection data if not already cached
  checkCache();
  if (!m_cached) {
    const_cast<XWindowsClipboard *>(this)->doFillCache();
  }
}

void XWindowsClipboard::doFillCache()
{
  if (m_motif) {
    motifFillCache();
  } else {
    icccmFillCache();
  }
  m_checkCache = false;
  m_cached = true;
  m_cacheTime = m_timeOwned;
}

void XWindowsClipboard::icccmFillCache()
{
  LOG_DEBUG("icccm fill clipboard %d", m_id);

  // see if we can get the list of available formats from the selection.
  // if not then use a default list of formats.  note that some clipboard
  // owners are broken and report TARGETS as the type of the TARGETS data
  // instead of the correct type ATOM;  allow either.
  const Atom atomTargets = m_atomTargets;
  Atom target;
  std::string data;
  if (!icccmGetSelection(atomTargets, &target, &data) || (target != m_atomAtom && target != m_atomTargets)) {
    LOG_VERBOSE("selection doesn't support TARGETS");
    data = "";
    XWindowsUtil::appendAtomData(data, XA_STRING);
  }

  XWindowsUtil::convertAtomProperty(data);
  auto targets = static_cast<const Atom *>(static_cast<const void *>(data.data()));
  const uint32_t numTargets = data.size() / sizeof(Atom);
  LOG_DEBUG("  available targets: %s", XWindowsUtil::atomsToString(m_display, targets, numTargets).c_str());

  // try each converter in order (because they're in order of
  // preference).
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    const IXWindowsClipboardConverter *converter = *index;
    const auto formatID = static_cast<int>(converter->getFormat());

    // skip already handled targets
    if (m_added[formatID]) {
      continue;
    }

    // see if atom is in target list
    Atom target = None;
    // XXX -- just ask for the converter's target to see if it's
    // available rather than checking TARGETS.  i've seen clipboard
    // owners that don't report all the targets they support.
    target = converter->getAtom();
    /*
    for (uint32_t i = 0; i < numTargets; ++i) {
        if (converter->getAtom() == targets[i]) {
            target = targets[i];
            break;
        }
    }
    */
    if (target == None) {
      continue;
    }

    // get the data
    Atom actualTarget;
    std::string targetData;
    if (!icccmGetSelection(target, &actualTarget, &targetData)) {
      LOG_VERBOSE("  no data for target %s", XWindowsUtil::atomToString(m_display, target).c_str());
      continue;
    }

    // add to clipboard and note we've done it
    m_data[formatID] = converter->toIClipboard(targetData);
    m_added[formatID] = true;
    LOG(
        (CLOG_DEBUG "added format %d for target %s (%u %s)", formatID,
         XWindowsUtil::atomToString(m_display, target).c_str(), targetData.size(),
         targetData.size() == 1 ? "byte" : "bytes")
    );
  }
}

bool XWindowsClipboard::icccmGetSelection(Atom target, Atom *actualTarget, std::string *data) const
{
  assert(actualTarget != nullptr);
  assert(data != nullptr);

  // request data conversion
  if (CICCCMGetClipboard getter(m_window, m_time, m_atomData);
      !getter.readClipboard(m_display, m_selection, target, actualTarget, data)) {
    LOG_VERBOSE("can't get data for selection target %s", XWindowsUtil::atomToString(m_display, target).c_str());
    LOGC(getter.error(), (CLOG_WARN "icccm violation by clipboard owner"));
    return false;
  } else if (*actualTarget == None) {
    LOG_VERBOSE("selection conversion failed for target %s", XWindowsUtil::atomToString(m_display, target).c_str());
    return false;
  }
  return true;
}

IClipboard::Time XWindowsClipboard::icccmGetTime() const
{
  Atom actualTarget;
  std::string data;
  if (icccmGetSelection(m_atomTimestamp, &actualTarget, &data) && actualTarget == m_atomInteger) {
    Time time = *static_cast<const Time *>(static_cast<const void *>(data.data()));
    LOG_VERBOSE("got ICCCM time %d", time);
    return time;
  } else {
    // no timestamp
    LOG_VERBOSE("can't get ICCCM time");
    return 0;
  }
}

bool XWindowsClipboard::motifLockClipboard() const
{
  // fail if anybody owns the lock (even us, so this is non-recursive)
  Window lockOwner = XGetSelectionOwner(m_display, m_atomMotifClipLock);
  if (lockOwner != None) {
    LOG_VERBOSE("motif lock owner 0x%08x", lockOwner);
    return false;
  }

  // try to grab the lock
  // FIXME -- is this right?  there's a race condition here --
  // A grabs successfully, B grabs successfully, A thinks it
  // still has the grab until it gets a SelectionClear.
  Time time = XWindowsUtil::getCurrentTime(m_display, m_window);
  XSetSelectionOwner(m_display, m_atomMotifClipLock, m_window, time);
  lockOwner = XGetSelectionOwner(m_display, m_atomMotifClipLock);
  if (lockOwner != m_window) {
    LOG_VERBOSE("motif lock owner 0x%08x", lockOwner);
    return false;
  }

  LOG_VERBOSE("locked motif clipboard");
  return true;
}

void XWindowsClipboard::motifUnlockClipboard() const
{
  LOG_VERBOSE("unlocked motif clipboard");

  // fail if we don't own the lock
  if (Window lockOwner = XGetSelectionOwner(m_display, m_atomMotifClipLock); lockOwner != m_window) {
    return;
  }

  // release lock
  Time time = XWindowsUtil::getCurrentTime(m_display, m_window);
  XSetSelectionOwner(m_display, m_atomMotifClipLock, None, time);
}

bool XWindowsClipboard::motifOwnsClipboard() const
{
  // get the current selection owner
  // FIXME -- this can't be right.  even if the window is destroyed
  // Motif will still have a valid clipboard.  how can we tell if
  // some other client owns CLIPBOARD?
  Window owner = XGetSelectionOwner(m_display, m_selection);
  if (owner == None) {
    return false;
  }

  // get the Motif clipboard header property from the root window
  Atom target;
  int32_t format;
  std::string data;
  if (Window root = RootWindow(m_display, DefaultScreen(m_display));
      !XWindowsUtil::getWindowProperty(m_display, root, m_atomMotifClipHeader, &data, &target, &format, False)) {
    return false;
  }

  // check the owner window against the current clipboard owner
  if (data.size() >= sizeof(MotifClipHeader)) {
    MotifClipHeader header;
    std::memcpy(&header, data.data(), sizeof(header));
    if ((header.m_id == MotifClip::Header) && (static_cast<Window>(header.m_selectionOwner) == owner)) {
      return true;
    }
  }

  return false;
}

void XWindowsClipboard::motifFillCache()
{
  LOG_DEBUG("motif fill clipboard %d", m_id);

  // get the Motif clipboard header property from the root window
  Atom target;
  int32_t format;
  std::string data;
  Window root = RootWindow(m_display, DefaultScreen(m_display));
  if (!XWindowsUtil::getWindowProperty(m_display, root, m_atomMotifClipHeader, &data, &target, &format, False)) {
    return;
  }

  MotifClipHeader header;
  if (data.size() < sizeof(header)) { // check that the header is okay
    return;
  }
  std::memcpy(&header, data.data(), sizeof(header));
  if (header.m_id != MotifClip::Header || header.m_numItems < 1) {
    return;
  }

  // get the Motif item property from the root window
  static const int buffer_size = 18 + 20;
  char name[buffer_size];
#if HAVE_FORMAT
  std::format_to_n(name, buffer_size, "_MOTIF_CLIP_ITEM_{}", header.m_item);
#else
  snprintf(name, buffer_size, "_MOTIF_CLIP_ITEM_%d", header.m_item);
#endif
  Atom atomItem = XInternAtom(m_display, name, False);
  data = "";
  if (!XWindowsUtil::getWindowProperty(m_display, root, atomItem, &data, &target, &format, False)) {
    return;
  }

  MotifClipItem item;
  if (data.size() < sizeof(item)) { // check that the item is okay
    return;
  }
  std::memcpy(&item, data.data(), sizeof(item));
  if (item.m_id != MotifClip::Item || item.m_numFormats - item.m_numDeletedFormats < 1) {
    return;
  }

  // format list is after static item structure elements
  const int32_t numFormats = item.m_numFormats - item.m_numDeletedFormats;
  auto formats = static_cast<const int32_t *>(static_cast<const void *>(item.m_size + data.data()));

  // get the available formats
  using MotifFormatMap = std::map<Atom, std::string>;
  MotifFormatMap motifFormats;
  for (int32_t i = 0; i < numFormats; ++i) {
    // get Motif format property from the root window
#if HAVE_FORMAT
    std::format_to_n(name, buffer_size, "_MOTIF_CLIP_ITEM_{}", formats[i]);
#else
    snprintf(name, buffer_size, "_MOTIF_CLIP_ITEM_%d", formats[i]);
#endif
    Atom atomFormat = XInternAtom(m_display, name, False);
    std::string data;
    if (!XWindowsUtil::getWindowProperty(m_display, root, atomFormat, &data, &target, &format, False)) {
      continue;
    }

    // check that the format is okay
    MotifClipFormat motifFormat;
    if (data.size() < sizeof(motifFormat)) {
      continue;
    }
    std::memcpy(&motifFormat, data.data(), sizeof(motifFormat));
    if (motifFormat.m_id != MotifClip::Format || motifFormat.m_length < 0 || motifFormat.m_type == None ||
        motifFormat.m_deleted != 0) {
      continue;
    }

    // save it
    motifFormats.try_emplace(motifFormat.m_type, data);
  }

  // try each converter in order (because they're in order of
  // preference).
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    const IXWindowsClipboardConverter *converter = *index;
    const auto formatID = static_cast<int>(converter->getFormat());

    // skip already handled targets
    if (m_added[formatID]) {
      continue;
    }

    // see if atom is in target list
    MotifFormatMap::const_iterator index2 = motifFormats.find(converter->getAtom());
    if (index2 == motifFormats.end()) {
      continue;
    }

    // get format
    MotifClipFormat motifFormat;
    std::memcpy(&motifFormat, index2->second.data(), sizeof(motifFormat));
    const Atom target = motifFormat.m_type;

    // get the data (finally)
    Atom actualTarget;
    std::string targetData;
    if (!motifGetSelection(&motifFormat, &actualTarget, &targetData)) {
      LOG_VERBOSE("  no data for target %s", XWindowsUtil::atomToString(m_display, target).c_str());
      continue;
    }

    // add to clipboard and note we've done it
    m_data[formatID] = converter->toIClipboard(targetData);
    m_added[formatID] = true;
    LOG_DEBUG("added format %d for target %s", format, XWindowsUtil::atomToString(m_display, target).c_str());
  }
}

bool XWindowsClipboard::motifGetSelection(const MotifClipFormat *format, Atom *actualTarget, std::string *data) const
{
  // if the current clipboard owner and the owner indicated by the
  // motif clip header are the same then transfer via a property on
  // the root window, otherwise transfer as a normal ICCCM client.
  if (!motifOwnsClipboard()) {
    return icccmGetSelection(format->m_type, actualTarget, data);
  }

  // use motif way
  // FIXME -- this isn't right.  it'll only work if the data is
  // already stored on the root window and only if it fits in a
  // property.  motif has some scheme for transferring part by
  // part that i don't know.
  static const int buffer_size = 18 + 20;
  char name[buffer_size];
#if HAVE_FORMAT
  std::format_to_n(name, buffer_size, "_MOTIF_CLIP_ITEM_{}", format->m_data);
#else
  snprintf(name, buffer_size, "_MOTIF_CLIP_ITEM_%d", format->m_data);
#endif
  Atom target = XInternAtom(m_display, name, False);
  Window root = RootWindow(m_display, DefaultScreen(m_display));
  return XWindowsUtil::getWindowProperty(m_display, root, target, data, actualTarget, nullptr, False);
}

IClipboard::Time XWindowsClipboard::motifGetTime() const
{
  return icccmGetTime();
}

bool XWindowsClipboard::insertMultipleReply(Window requestor, ::Time time, Atom property)
{
  // get the requested targets
  Atom target;
  int32_t format;
  std::string data;
  if (!XWindowsUtil::getWindowProperty(m_display, requestor, property, &data, &target, &format, False)) {
    // can't get the requested targets
    return false;
  }

  // fail if the requested targets isn't of the correct form
  if (format != 32 || target != m_atomAtomPair) {
    return false;
  }

  // data is a list of atom pairs:  target, property
  XWindowsUtil::convertAtomProperty(data);
  auto targets = static_cast<const Atom *>(static_cast<const void *>(data.data()));
  const uint32_t numTargets = data.size() / sizeof(Atom);

  // add replies for each target
  bool changed = false;
  for (uint32_t i = 0; i < numTargets; i += 2) {
    const Atom target = targets[i + 0];
    const Atom property = targets[i + 1];
    if (!addSimpleRequest(requestor, target, time, property)) {
      // note that we can't perform the requested conversion
      XWindowsUtil::replaceAtomData(data, i, None);
      changed = true;
    }
  }

  // update the targets property if we changed it
  if (changed) {
    XWindowsUtil::setWindowProperty(m_display, requestor, property, data.data(), data.size(), target, format);
  }

  // add reply for MULTIPLE request
  insertReply(new Reply(requestor, m_atomMultiple, time, property, std::string(), None, 32));

  return true;
}

void XWindowsClipboard::insertReply(Reply *reply)
{
  assert(reply != nullptr);

  // note -- we must respond to requests in order if requestor,target,time
  // are the same, otherwise we can use whatever order we like with one
  // exception:  each reply in a MULTIPLE reply must be handled in order
  // as well.  those replies will almost certainly not share targets so
  // we can't simply use requestor,target,time as map index.
  //
  // instead we'll use just the requestor.  that's more restrictive than
  // necessary but we're guaranteed to do things in the right order.
  // note that we could also include the time in the map index and still
  // ensure the right order.  but since that'll just make it harder to
  // find the right reply when handling property notify events we stick
  // to just the requestor.

  const bool newWindow = !m_replies.contains(reply->m_requestor);
  m_replies[reply->m_requestor].push_back(reply);

  // adjust requestor's event mask if we haven't done so already.  we
  // want events in case the window is destroyed or any of its
  // properties change.
  if (newWindow) {
    // note errors while we adjust event masks
    bool error = false;
    {
      XWindowsUtil::ErrorLock lock(m_display, &error);

      // get and save the current event mask
      XWindowAttributes attr;
      XGetWindowAttributes(m_display, reply->m_requestor, &attr);
      m_eventMasks[reply->m_requestor] = attr.your_event_mask;

      // add the events we want
      XSelectInput(m_display, reply->m_requestor, attr.your_event_mask | StructureNotifyMask | PropertyChangeMask);
    }

    // if we failed then the window has already been destroyed
    if (error) {
      m_replies.erase(reply->m_requestor);
      delete reply;
    }
  }
}

void XWindowsClipboard::pushReplies()
{
  // send the first reply for each window if that reply hasn't
  // been sent yet.
  for (auto index = m_replies.begin(); index != m_replies.end();) {
    assert(!index->second.empty());
    auto listit = index->second.begin();
    while (listit != index->second.end()) {
      if (!(*listit)->m_replied)
        break;
      ++listit;
    }
    if (listit != index->second.end() && !(*listit)->m_replied) {
      pushReplies(index, index->second, listit);
    } else {
      ++index;
    }
  }
}

void XWindowsClipboard::pushReplies(ReplyMap::iterator &mapIndex, ReplyList &replies, ReplyList::iterator index)
{
  Reply *reply = *index;
  while (sendReply(reply)) {
    // reply is complete.  discard it and send the next reply,
    // if any.
    index = replies.erase(index);
    delete reply;
    if (index == replies.end()) {
      break;
    }
    reply = *index;
  }

  // if there are no more replies in the list then remove the list
  // and stop watching the requestor for events.
  if (replies.empty()) {
    XWindowsUtil::ErrorLock lock(m_display);
    Window requestor = mapIndex->first;
    XSelectInput(m_display, requestor, m_eventMasks[requestor]);
    m_replies.erase(mapIndex++);
    m_eventMasks.erase(requestor);
  } else {
    ++mapIndex;
  }
}

bool XWindowsClipboard::sendReply(Reply *reply)
{
  assert(reply != nullptr);

  // bail out immediately if reply is done
  if (reply->m_done) {
    LOG((
        CLOG_VERBOSE "clipboard: finished reply to 0x%08x,%d,%d", reply->m_requestor, reply->m_target, reply->m_property
    ));
    return true;
  }

  // start in failed state if property is None
  bool failed = (reply->m_property == None);
  if (!failed) {
    LOG(
        (CLOG_VERBOSE "clipboard: setting property on 0x%08x,%d,%d", reply->m_requestor, reply->m_target,
         reply->m_property)
    );

    // send using INCR if already sending incrementally or if reply
    // is too large, otherwise just send it.
    const uint32_t maxRequestSize = 3 * XMaxRequestSize(m_display);
    const bool useINCR = (reply->m_data.size() > maxRequestSize);

    // send INCR reply if incremental and we haven't replied yet
    if (useINCR && !reply->m_replied) {
      uint32_t size = reply->m_data.size();
      if (!XWindowsUtil::setWindowProperty(
              m_display, reply->m_requestor, reply->m_property, &size, 4, m_atomINCR, 32
          )) {
        failed = true;
      }
    }

    // send more INCR reply or entire non-incremental reply
    else {
      // how much more data should we send?
      uint32_t size = reply->m_data.size() - reply->m_ptr;
      if (size > maxRequestSize)
