#include <Epub/Page.h>
#include <FontCacheManager.h>
#include <Logging.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "EpubReaderActivity.h"
#include "EpubReaderUtils.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"

EpubReaderActivity::Snapshot EpubReaderActivity::readSnapshot() const {
  taskENTER_CRITICAL(&snapshotMux);
  const Snapshot value = snapshot;
  taskEXIT_CRITICAL(&snapshotMux);
  return value;
}

void EpubReaderActivity::publishSnapshot() {
  Snapshot value;
  value.context = readerContext;
  value.pageRevision = pageRevision;
  value.spineIndex = currentSpineIndex;
  value.page = section ? section->currentPage : nextPageNumber;
  value.pageCount = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  value.availablePageCount = section ? section->pageCount : 0;
  value.loaded = static_cast<bool>(epub);
  value.hasSection = static_cast<bool>(section);
  value.atEnd = atEndOnRender();
  value.footnoteCount = currentPageFootnotes.size();
  value.footnoteDepth = footnoteDepth;
  value.hasLinks = !currentPageLinks.empty();
  value.hasBookmarks = !cachedBookmarks.empty();
  value.bookmarkMessage = showBookmarkMessage;
  value.dictionaryMessage = showDictionaryMessage;
  value.orientation = appliedOrientation;
  value.automatic = automaticPageTurnActive;
  value.paused = readerPaused;
  if (epub && value.pageCount > 0) {
    value.progressPercent = std::clamp(
        static_cast<int>(epub->calculateProgress(currentSpineIndex, static_cast<float>(value.page) / value.pageCount) *
                             100.0f +
                         0.5f),
        0, 100);
  }
  value.wakeAt = nextWakeAt(millis());
  taskENTER_CRITICAL(&snapshotMux);
  snapshot = value;
  taskEXIT_CRITICAL(&snapshotMux);
}

std::optional<uint32_t> EpubReaderActivity::nextWakeAt(const uint32_t nowMs) const {
  if (renderRetired || !epub || (readerPaused && overlay == Overlay::None)) return std::nullopt;
  if (redrawRequested.load()) return nowMs;
  std::optional<uint32_t> delay;
  const auto due = [&](uint32_t wait) {
    if (!delay || wait < *delay) delay = wait;
  };
  const auto remaining = [nowMs](uint32_t start, uint32_t duration) {
    const uint32_t elapsed = nowMs - start;
    return elapsed >= duration ? 0 : duration - elapsed;
  };
  if (showBookmarkMessage) due(remaining(bookmarkMessageTime, ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS));
  if (showDictionaryMessage) due(remaining(dictionaryMessageTime, ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS));
  if (!readerPaused) {
    if (pageTurns.getCounts().pending != 0) {
      due(remaining(lastPageTurnTime, MIN_MANUAL_TURN_GAP_MS));
    } else {
      if (automaticPageTurnActive && !inputActive.load()) due(remaining(lastPageTurnTime, pageTurnDuration));
      const auto idle = idleWork(nowMs);
      if (idle.action != EpubReaderUtils::IdleAction::None) due(idle.delayMs);
    }
  }
  return delay ? std::make_optional(nowMs + *delay) : std::nullopt;
}

void EpubReaderActivity::runCommand(Command& command) {
  assert(pendingControl.load() == nullptr && "Reader control command already pending");
  pendingControl.store(&command, std::memory_order_release);
  do {
    if (!activityManager.requestUpdateAndWait()) {
      pendingControl.store(nullptr);
      LOG_ERR("ERS", "Reader control command has no render task");
      return;
    }
    // A page-dependent command can require several bounded preparation turns.
    // Its stack-owned payload stays alive until the worker consumes the slot.
  } while (pendingControl.load(std::memory_order_acquire) == &command);
  const auto state = readSnapshot();
  if (inputContext != state.context) {
    mappedInput.discardPendingInput();
    inputContext = state.context;
  }
}

void EpubReaderActivity::changeReaderContext() {
  ++readerContext;
  if (pageTarget == PageTarget::NextPage) pageTarget = PageTarget::Current;
  retirePagePreparation();
  idlePrewarmSpine = idlePrewarmPage = -1;
  currentPageVisibleOffset.reset();
  cancelPageTurns(EpubPageTurns::Outcome::ContextChanged, "reader context");
}

void EpubReaderActivity::pauseReader() {
  Command command{Control::Pause};
  runCommand(command);
}

void EpubReaderActivity::releaseSection() {
  if (section && pageTarget != PageTarget::Navigation) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }
  retireSection();
}

void EpubReaderActivity::executeCommand(Command& command) {
  command.succeeded = true;
  switch (command.type) {
    case Control::Open:
      changeReaderContext();
      appliedOrientation = SETTINGS.orientation;
      command.succeeded = loadBookOnRender();
      break;
    case Control::Pause:
      if (!readerPaused)
        changeReaderContext();
      else
        retirePagePreparation();
      // Child font settings can unload the font referenced by paragraph work.
      if (section && section->isBuilding()) {
        section->suspendBuild();
        if (!section->isPartial() && !section->isBuildComplete()) retireSection();
      }
      readerPaused = true;
      break;
    case Control::Resume:
      readerPaused = false;
      lastPageTurnTime = millis();
      break;
    case Control::Chapter:
      command.succeeded = jumpToChapterOnRender(command.argument, command.text);
      break;
    case Control::Toc: {
      const auto item = epub->getTocItem(command.argument);
      command.succeeded = jumpToChapterOnRender(item.spineIndex, item.anchor);
      break;
    }
    case Control::Percent:
      changeReaderContext();
      jumpToPercentOnRender(command.argument);
      break;
    case Control::Href:
      changeReaderContext();
      navigateToHrefOnRender(command.text, command.flag);
      break;
    case Control::LinkAtPoint: {
      if (!section || command.pageRevision != pageRevision || command.spineIndex != currentSpineIndex ||
          command.pageNumber != section->currentPage)
        break;
      const auto* link = EpubReaderUtils::linkAtPoint(currentPageLinks, command.argument, command.secondArgument,
                                                      currentPageLinkMarginLeft, currentPageLinkMarginTop);
      command.succeeded = link != nullptr;
      if (link) {
        changeReaderContext();
        navigateToHrefOnRender(link->href, true);
      }
      break;
    }
    case Control::RestoreFootnote:
      changeReaderContext();
      restoreSavedPositionOnRender();
      break;
    case Control::Orientation:
      changeReaderContext();
      applyOrientationOnRender(command.argument);
      break;
    case Control::TextSettings:
      changeReaderContext();
      applyReaderTextSettingsOnRender();
      break;
    case Control::ReleaseSection:
      releaseSection();
      break;
    case Control::Progress:
      changeReaderContext();
      loadCachedBookmarks();
      applyProgressOnRender(command.progress);
      break;
    case Control::AutoTurn:
      if (command.argument == 0) changeReaderContext();
      toggleAutoPageTurnOnRender(command.argument);
      break;
    case Control::Bookmark:
      addBookmarkOnRender();
      if (command.flag) {
        showBookmarkMessage = true;
        bookmarkMessageTime = millis();
      }
      break;
    case Control::ReloadBookmarks:
      loadCachedBookmarks();
      break;
    case Control::DictionaryPage:
      if (SETTINGS.dictionaryName[0] == '\0') {
        showDictionaryMessage = true;
        dictionaryMessageTime = millis();
        requestUpdate();
      } else if (section) {
        command.page = section->loadPage(section->currentPage);
      }
      break;
    case Control::Footnotes:
      if (command.flag && currentPageFootnotes.size() == 1) {
        changeReaderContext();
        navigateToHrefOnRender(currentPageFootnotes.front().href, true);
      } else {
        command.footnotes = std::move(currentPageFootnotes);
      }
      break;
    case Control::RestoreFootnotes:
      currentPageFootnotes = std::move(command.footnotes);
      break;
    case Control::QrText:
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        command.text = section->getTextFromSectionFile();
      }
      break;
    case Control::Sync: {
      const auto position = currentPositionOnRender();
      command.spineIndex = position.spineIndex;
      command.pageNumber = position.pageNumber;
      command.totalPages = position.totalPages;
      command.paragraphIndex = position.hasParagraphIndex ? std::make_optional(position.paragraphIndex) : std::nullopt;
      command.syncPosition = ProgressMapper::toSavedProgress(epub, position);
      const int toc = epub->getTocIndexForSpineIndex(currentSpineIndex);
      command.text = toc >= 0 ? epub->getTocItem(toc).title : "";
      if (!saveProgress(currentSpineIndex, command.pageNumber, command.totalPages)) {
        pendingSyncSaveError = true;
        command.succeeded = false;
        requestUpdate();
        break;
      }
      ImageBlock::setExtractor(nullptr, nullptr);
      retireSection();
      epub.reset();
      break;
    }
    case Control::DeleteCache:
      if (epub && section) {
        const int page = section->currentPage;
        const int count = section->pageCount;
        retireSection();
        epub->clearCache();
        epub->setupCacheDir();
        if (!saveProgress(currentSpineIndex, page, count)) LOG_ERR("ERS", "Failed to save progress before cache clear");
      }
      break;
    case Control::Screenshot:
      pendingScreenshot = true;
      break;
    case Control::ReturnFromEnd:
      clearPendingNavigation();
      changeReaderContext();
      if (epub && epub->getSpineItemsCount() > 0) {
        currentSpineIndex = epub->getSpineItemsCount() - 1;
        nextPageNumber = 0;
        pendingPageJump = UINT16_MAX;
      }
      break;
  }
}

bool EpubReaderActivity::jumpToChapterOnRender(const int spineIndex, const std::string& anchor) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    LOG_ERR("ERS", "Invalid chapter destination: %d", spineIndex);
    return false;
  }
  clearPendingNavigation();
  changeReaderContext();
  clearDeferredReposition();
  currentSpineIndex = spineIndex;
  pendingAnchor = anchor;
  nextPageNumber = 0;
  retireSection();
  return true;
}

void EpubReaderActivity::onResume() {
  const auto state = readSnapshot();
  if (state.orientation != SETTINGS.orientation) applyOrientation(SETTINGS.orientation);
  if (overlay == Overlay::None && state.paused) {
    Command command{Control::Resume};
    runCommand(command);
    Activity::requestUpdate();
  }
}

void EpubReaderActivity::requestUpdate(const bool immediate) {
  redrawRequested.store(true);
  Activity::requestUpdate(immediate);
}

void EpubReaderActivity::render(RenderLock&& lock) {
  if (renderRetired) return;
  if (auto* command = pendingControl.load(std::memory_order_acquire)) {
    const bool needsPage = command->type == Control::Bookmark || command->type == Control::DictionaryPage ||
                           command->type == Control::Footnotes || command->type == Control::QrText ||
                           command->type == Control::Sync;
    if (needsPage && !atEndOnRender() &&
        (redrawRequested.load() || !section || pageTarget == PageTarget::Navigation || pendingRenderPage ||
         (section && section->currentPage >= section->pageCount && (section->isBuilding() || section->isPartial())))) {
      // These actions refer to the requested position, not an uninitialised
      // page zero in a newly created Section. Pause/navigation still preempt it.
      redrawRequested.store(false);
      renderBook();
      if (redrawRequested.load()) {
        publishSnapshot();
        lock.unlock();
        vTaskDelay(1);
        return;
      }
      if (!section) {
        command->succeeded = false;
        pendingControl.store(nullptr, std::memory_order_release);
        LOG_ERR("ERS", "Reader command failed while preparing its page");
        publishSnapshot();
        return;
      }
    }
    pendingControl.store(nullptr, std::memory_order_release);
    executeCommand(*command);
    publishSnapshot();
    // Main finishes its UI transition after the command acknowledgement.
    if (redrawRequested.load()) Activity::requestUpdate();
    return;
  }
  if (epub && (!readerPaused || overlay != Overlay::None)) {
    if (showBookmarkMessage && millis() - bookmarkMessageTime >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
      showBookmarkMessage = false;
      redrawRequested.store(true);
    }
    if (showDictionaryMessage && millis() - dictionaryMessageTime >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
      showDictionaryMessage = false;
      redrawRequested.store(true);
    }
    if (!readerPaused) {
      if (automaticPageTurnActive && pageTurns.getCounts().pending == 0 && !inputActive.load() &&
          millis() - lastPageTurnTime >= pageTurnDuration) {
        pageTurns.accept(EpubPageTurns::Action::NextPage, millis(), millis(), readerContext);
      }
      processPageTurns();
    }
    if (redrawRequested.exchange(false)) {
      publishSnapshot();
      ReaderActivity::render(std::move(lock));
    } else if (!readerPaused && pageTurns.getCounts().pending == 0) {
      runBackgroundWork();
    }
  }
  publishSnapshot();
  const auto wakeAt = readSnapshot().wakeAt;
  if (wakeAt && static_cast<int32_t>(static_cast<uint32_t>(millis()) - *wakeAt) >= 0) {
    // Real continuation work must let main, input and the idle watchdog run.
    lock.unlock();
    vTaskDelay(1);
    Activity::requestUpdate(true);
  }
}

bool EpubReaderActivity::loadBook() {
  Command command{Control::Open};
  runCommand(command);
  return command.succeeded;
}

void EpubReaderActivity::jumpToPercent(const int percent) {
  Command command{Control::Percent};
  command.argument = percent;
  runCommand(command);
  requestUpdate();
}

void EpubReaderActivity::navigateToHref(const std::string& href, const bool savePosition) {
  Command command{Control::Href};
  command.text = href;
  command.flag = savePosition;
  runCommand(command);
  requestUpdate();
}

void EpubReaderActivity::restoreSavedPosition() {
  Command command{Control::RestoreFootnote};
  runCommand(command);
  requestUpdate();
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  Command command{Control::Orientation};
  command.argument = orientation;
  runCommand(command);
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t option) {
  Command command{Control::AutoTurn};
  command.argument = option;
  runCommand(command);
}

void EpubReaderActivity::applyReaderTextSettings() {
  Command command{Control::TextSettings};
  runCommand(command);
}

void EpubReaderActivity::addBookmark(const bool showMessage) {
  Command command{Control::Bookmark};
  command.flag = showMessage;
  runCommand(command);
}
