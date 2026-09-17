#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>

#include "../../util/BookmarkFile.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderActivity.h"
#include "ReaderFontSizes.h"
#include "ReaderToolbarUi.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/settings/TextSettingsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

namespace {
// The X4 Pro and X4 Classic carry the X4's panel but sit outside isXteinkDevice()
// (that helper also gates power management). Overlay refresh choices are per-panel:
// this family runs the grayscale anti-aliasing pass, so chrome painted over a
// fresh page needs the HALF ghost-cleanup and closing re-renders the page.
bool xteinkClassPanel() { return gpio.isXteinkDevice() || BoardConfig::isX4Pro() || BoardConfig::isX4Classic(); }

constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

constexpr char READ_FOLDER[] = "/read";

bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

EpubReaderActivity::~EpubReaderActivity() {
  assert(!section && "Reader Section must be released by its render task");
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  }
}

bool EpubReaderActivity::onPrepareExit() { return activityManager.retireActivity(*this); }

void EpubReaderActivity::onRenderExit() {
  if (renderRetired) return;
  changeReaderContext();
  ImageBlock::setExtractor(nullptr, nullptr);
  discardOverlayPage();  // free the overlay's page snapshot if one is held

  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

  section.reset();
  renderRetired = true;
}

bool EpubReaderActivity::loadBookOnRender() {
  if (!pageTurns.available()) return false;
  auto loadedEpub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
  if (!loadedEpub) {
    LOG_ERR("ERS", "Failed to allocate EPUB object");
    return false;
  }

  const bool uncached = !Storage.exists((loadedEpub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
    disableFastInitialRefresh();
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }

  bool loaded;
  {
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    loaded = loadedEpub->load(true, SETTINGS.embeddedStyle == 0);
  }
  if (!loaded) {
    LOG_ERR("ERS", "Failed to load EPUB");
    return false;
  }
  epub = std::move(loadedEpub);

  ImageBlock::clearSessionRenderFailures();
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest);
  });

  epub->setupCacheDir();

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[10];
    int dataSize = f.read(data, sizeof(data));
    if (dataSize == 4 || dataSize == 6 || dataSize == 10) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    } else if (dataSize == 10) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      cachedVisibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                                (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
    }
  }

  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      cachedVisibleTextOffset.reset();
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  loadCachedBookmarks();
  return true;
}

void EpubReaderActivity::openReaderMenu() {
  pauseReader();
  const auto state = readSnapshot();
  if (usesToolbarMenu()) {
    RenderLock lock;
    // Reached from a child activity's result handler (footnotes, bookmarks,
    // go-to-percent... cancelled back to the menu), so the framebuffer holds
    // that screen, not the page: re-render the page and let renderBook() put
    // the toolbar on top. The in-reader fast path is openOverlay().
    overlay = Overlay::Toolbar;
    focusedTool = 0;
    panelHoldJumped = false;
    panelCursorShown = !mappedInput.hasTouch();
    if (!toolbarUi) toolbarUi = std::make_unique<ReaderToolbarUi>(renderer);
    toolbarUi->begin();
    discardOverlayPage();
    requestUpdate();
    return;
  }
  const int currentPage = state.hasSection ? state.page + 1 : 0;
  const int totalPages = state.hasSection ? state.pageCount : 0;
  const int bookProgressPercent = state.hasSection ? state.progressPercent : 0;
  startActivityForResult(std::make_unique<EpubReaderMenuActivity>(renderer, mappedInput, epub->getTitle(), currentPage,
                                                                  totalPages, bookProgressPercent, SETTINGS.orientation,
                                                                  state.footnoteCount != 0, state.hasBookmarks),
                         [this](const ActivityResult& result) {
                           const auto& menu = std::get<MenuResult>(result.data);
                           if (SETTINGS.orientation != menu.orientation) {
                             applyOrientation(menu.orientation);
                           }
                           toggleAutoPageTurn(menu.pageTurnOption);
                           if (!result.isCancelled) {
                             onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                           }
                         });
}

bool EpubReaderActivity::buildTickHeapGate() {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxBlock = ESP.getMaxAllocHeap();
  buildHeapPaused = freeHeap < BACKGROUND_BUILD_MIN_FREE_HEAP || maxBlock < BACKGROUND_BUILD_MIN_MAX_ALLOC;
  return !buildHeapPaused;
}

void EpubReaderActivity::showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (!buildPopupPending || !renderer.hasFrameBuffer()) return;
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  pagesUntilFullRefresh = 1;
  buildPopupPending = false;
}

void EpubReaderActivity::openDictionaryWordSelect() {
  if (SETTINGS.dictionaryName[0] != '\0') pauseReader();
  Command command{Control::DictionaryPage};
  runCommand(command);
  if (!command.page) {
    onResume();
    return;
  }
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  top += SETTINGS.screenMargin;
  left += SETTINGS.screenMargin;
  startActivityForResult(
      std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(command.page), left, top),
      [this](const ActivityResult&) { requestUpdate(); });
}

void EpubReaderActivity::openFootnotes(const bool followSingle) {
  pauseReader();
  Command command{Control::Footnotes};
  command.flag = followSingle;
  runCommand(command);
  if (!command.footnotes.empty()) {
    startActivityForResult(
        std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, std::move(command.footnotes)),
        [this, followSingle](ActivityResult&& result) {
          auto& note = std::get<FootnoteResult>(result.data);
          Command command{Control::RestoreFootnotes};
          command.footnotes = std::move(note.footnotes);
          runCommand(command);
          if (result.isCancelled) {
            if (!followSingle) openReaderMenu();
          } else {
            navigateToHref(note.href, true);
          }
          requestUpdate();
        });
  } else {
    requestUpdate();
    onResume();
  }
}

bool EpubReaderActivity::hasInputActivity() const {
  const auto physical = gpio.getPhysicalButtonFrame();
  return physical.down != 0 || physical.debouncePending || gpio.hasPendingButtonFrames() ||
         mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased() || gpio.wasTouchActivity();
}

void EpubReaderActivity::runBackgroundWork() {
  const bool allowSpeculativeWork = !inputActive.load();
  auto* fontCache = renderer.getFontCacheManager();
  if (fontCache && fontCache->isPrewarming()) {
    // ponytail: fixed work quantum; tune only with measured target SD latency.
    constexpr uint16_t PREWARM_UNITS_PER_TICK = 16;
    if (allowSpeculativeWork && pageTurns.getCounts().pending == 0 && pendingControl.load() == nullptr) {
      fontCache->prewarmSome(PREWARM_UNITS_PER_TICK);
    }
    return;
  }

  constexpr unsigned long IDLE_FINISH_DELAY_MS = 1000;
  const bool finishWhileIdle = lastRenderCompleteMs != 0 && millis() - lastRenderCompleteMs > IDLE_FINISH_DELAY_MS;

  if (section && (allowSpeculativeWork || section->currentPage >= static_cast<int>(section->pageCount)) &&
      !section->isBuilding() && section->isPartial() && buildViewportWidth > 0 && !partialRebuildStartFailed &&
      section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount)) {
    const ReaderRenderSpec buildSpec = SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight);
    if (!section->startBuild(buildSpec)) {
      partialRebuildStartFailed = true;
      LOG_ERR("ERS", "Failed to start deferred partial extension build");
    } else {
      LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
              section->pageCount);
    }
  }

  if (section && (allowSpeculativeWork || section->currentPage >= static_cast<int>(section->pageCount)) &&
      section->isBuilding() &&
      (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD ||
       finishWhileIdle) &&
      buildTickHeapGate()) {
    if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK, 1)) {
      LOG_ERR("ERS", "Background section build failed");
      cancelPageTurns(EpubPageTurns::Outcome::Failed, "section build");
      section.reset();
      requestUpdate();
    } else if (section->isBuildComplete() && applyDeferredReposition()) {
      requestUpdate();
    }
  }
  constexpr unsigned long IDLE_PREWARM_DEBOUNCE_MS = 400;
  if (allowSpeculativeWork && section && renderer.hasFrameBuffer() && lastRenderCompleteMs != 0 &&
      millis() - lastRenderCompleteMs > IDLE_PREWARM_DEBOUNCE_MS && ESP.getFreeHeap() > RENDER_MIN_FREE_HEAP &&
      ESP.getMaxAllocHeap() > BACKGROUND_BUILD_MIN_MAX_ALLOC &&
      (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
    const int nextPage = section->currentPage + 1;
    if (nextPage < static_cast<int>(section->pageCount)) {
      if (const auto p = section->loadPage(nextPage)) {
        if (auto* fcm = renderer.getFontCacheManager()) {
          const auto t0 = millis();
          auto scope = fcm->createPrewarmScope();
          p->render(renderer, SETTINGS.getReaderFontId(), 0, 0);
          scope.deferPrewarm();
          idlePrewarmSpine = currentSpineIndex;
          idlePrewarmPage = section->currentPage;
          LOG_DBG("ERS", "Idle font scan: page %d in %lums", nextPage, millis() - t0);
        }
      }
    }
  }
}

void EpubReaderActivity::loop() {
  const auto state = readSnapshot();
  if (state.context != inputContext) {
    mappedInput.discardPendingInput();
    inputContext = state.context;
    return;
  }
  inputActive.store(hasInputActivity());
  if (state.needsWork) Activity::requestUpdate();
  if (!state.loaded) {
    finish();
    return;
  }

  // Someone else turned the screen while this reader was stacked (the control
  // center's orientation tile). Reflow before the next render, or the page
  // would be drawn with a layout built for the previous frame size.
  if (state.orientation != SETTINGS.orientation) {
    applyOrientation(SETTINGS.orientation);
    requestUpdate();
    return;
  }

  const bool atEndOfBook = state.spineIndex > 0 && state.atEnd;
  clearEndOfBookOptionsIfNeeded();

  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);

  // The toolbar reader menu owns all input while shown, ahead of the automatic page turn
  // below: the More panel's rate popup switches automatic turning on and leaves the panel
  // open, so the timer must neither flip the page under it nor eat the panel's next
  // Confirm/Back release.
  if (overlay != Overlay::None) {
    if (usesToolbarMenu()) {
      handleOverlayInput();
      return;
    }
    // The style was switched off while an overlay was up (Settings reached via
    // the More panel); fall back to the clean page.
    closeOverlayToPage();
    return;
  }

  if (state.automatic && (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
                          mappedInput.wasReleased(MappedInputManager::Button::Back) ||
                          ReaderUtils::isTouchMenuGesture(renderer, mappedInput))) {
    toggleAutoPageTurn(0);
    requestUpdate();
    return;
  }

  // While the end-of-book suggestion menu is up it owns Confirm/Back/navigation, so it
  // gets this tick's input first and the long-press shortcuts below stay inert behind it
  // -- a hold there must not drop a bookmark onto the suggestion screen or paint the
  // dictionary word picker over it. Anything the menu does not handle (long-press Back to
  // the file browser, say) still falls through to the regular handlers.
  if (handleEndOfBookMenu()) {
    return;
  }
  const bool endOfBookMenuOpen = endOfBookMenuActive();

  const unsigned long confirmHoldMs = confirmLongPressThreshold();
  // wasLongPressed() suppresses the release that follows it, so leave it unpolled while
  // the end-of-book menu owns Confirm -- otherwise the menu never sees that release.
  const bool confirmLongPressed = !endOfBookMenuOpen && confirmHoldMs != 0 &&
                                  mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, confirmHoldMs);
  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (confirmLongPressed) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        addBookmark(true);
        requestUpdate();
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        if (launchKOReaderSync()) {
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        openDictionaryWordSelect();
        return;
      case CrossPointSettings::LP_MENU_READER_MENU:
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Home-key boards have no front Confirm button, so a Home-key hold runs the
  // same user-selected long-press action. The SDK emits this event once per
  // hold and suppresses the short Home tap for the same contact.
  if (mappedInput.wasHomeKeyHold() && !endOfBookMenuOpen) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        if (!state.bookmarkMessage) {
          addBookmark(true);
          requestUpdate();
        }
        return;
      case CrossPointSettings::LP_MENU_KOSYNC:
        launchKOReaderSync();
        return;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        if (!state.dictionaryMessage) {
          openDictionaryWordSelect();
        }
        return;
      case CrossPointSettings::LP_MENU_READER_MENU:
        if (usesToolbarMenu() && state.hasSection) {
          openOverlay(Overlay::Toolbar);
        } else {
          openReaderMenu();
        }
        return;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Link taps take priority over the reader-menu and page-turn zones.
  if (!atEndOfBook && state.hasLinks && SETTINGS.touchReaderControls && mappedInput.hasTouch()) {
    int touchX = 0;
    int touchY = 0;
    if (mappedInput.wasScreenTapped(touchX, touchY)) {
      Command command{Control::LinkAtPoint};
      command.argument = touchX;
      command.secondArgument = touchY;
      command.spineIndex = state.spineIndex;
      command.pageNumber = state.page;
      command.pageRevision = state.pageRevision;
      runCommand(command);
      if (command.succeeded) {
        requestUpdate();
        return;
      }
    }
  }

  const bool menuTriggered = confirmReleased || ReaderUtils::isTouchMenuGesture(renderer, mappedInput);
  if (menuTriggered) {
    // Toolbar style: the page is on screen and in the framebuffer, so paint the
    // toolbar over it (one refresh) instead of pushing a full-screen menu.
    if (usesToolbarMenu() && state.hasSection) {
      openOverlay(Overlay::Toolbar);
    } else {
      openReaderMenu();
    }
    return;
  }

  if (state.footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime(MappedInputManager::Button::Back) < ReaderUtils::GO_BACK_OR_HOME_MS) {
    restoreSavedPosition();
    return;
  }

  if (handleBackNavigation()) {
    return;
  }

  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (state.footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      openFootnotes(true);
    }
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt, buttonHeldMs] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (prevTriggered || nextTriggered) {
    if (handleEndOfBookPageTurn(prevTriggered, nextTriggered)) return;
    if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
        mappedInput.wasReleased(MappedInputManager::Button::Down))
      return;

    const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : buttonHeldMs;
    const bool longPress = !fromTilt && heldMs >= ReaderUtils::SKIP_HOLD_MS;
    if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
      const uint8_t newOrientation =
          nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                        : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
      applyOrientation(newOrientation);
      requestUpdate();
      return;
    }
    if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
      skipPages(prevTriggered ? -1 : 1);
    } else {
      pageTurn(!prevTriggered);
    }
  }
}

void EpubReaderActivity::jumpToPercentOnRender(int percent) {
  if (!epub) return;
  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) return;

  percent = clampPercent(percent);

  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) targetSize = bookSize - 1;

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) return;

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  pendingSpineProgress = std::clamp(pendingSpineProgress, 0.0f, 1.0f);

  {
    clearDeferredReposition();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::onReaderMenuConfirm(const EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spine = readSnapshot().spineIndex;
      Command command{Control::ReleaseSection};
      runCommand(command);
      startActivityForResult(std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, spine),
                             [this](const ActivityResult& result) {
                               if (result.isCancelled) {
                                 openReaderMenu();
                                 return;
                               }
                               const auto& chapter = std::get<ChapterResult>(result.data);
                               Command command{Control::Chapter};
                               command.argument = chapter.spineIndex;
                               command.text = chapter.anchor;
                               runCommand(command);
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES:
      openFootnotes();
      break;
    case EpubReaderMenuActivity::MenuAction::TEXT_SETTINGS: {
      Command command{Control::ReleaseSection};
      runCommand(command);
      startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                    TextSettingsActivity::Tab::Family),
                             [this](const ActivityResult&) {
                               applyReaderTextSettings();
                               openReaderMenu();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::NIGHT_MODE:
    case EpubReaderMenuActivity::MenuAction::FRONTLIGHT:
      break;  // Handled in-place by the menu.
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      const auto state = readSnapshot();
      const int initialPercent =
          state.availablePageCount > 0 && epub->getBookSize() > 0
              ? clampPercent(static_cast<int>(epub->calculateProgress(state.spineIndex, static_cast<float>(state.page) /
                                                                                            state.availablePageCount) *
                                                  100.0f +
                                              0.5f))
              : 0;
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (result.isCancelled)
              openReaderMenu();
            else
              jumpToPercent(std::get<PercentResult>(result.data).percent);
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY:
      openDictionaryWordSelect();
      break;
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      Command command{Control::QrText};
      runCommand(command);
      if (!command.text.empty()) {
        startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, std::move(command.text)),
                               [this](const ActivityResult&) { openReaderMenu(); });
      } else
        requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME:
      onGoHome();
      return;
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      Command command{Control::DeleteCache};
      runCommand(command);
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      Command command{Control::Screenshot};
      runCommand(command);
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC:
      launchKOReaderSync();
      break;
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS:
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              Command command{Control::ReloadBookmarks};
              runCommand(command);
              openReaderMenu();
            } else {
              Command command{Control::Progress};
              command.progress = std::get<ProgressChangeResult>(result.data);
              runCommand(command);
              requestUpdate();
            }
          });
      break;
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK:
      addBookmark();
      break;
  }
}

unsigned long EpubReaderActivity::confirmLongPressThreshold() const {
  switch (SETTINGS.longPressMenuFunction) {
    case CrossPointSettings::LP_MENU_BOOKMARK:
    case CrossPointSettings::LP_MENU_DICTIONARY:
      return ReaderUtils::BOOKMARK_HOLD_MS;
    case CrossPointSettings::LP_MENU_KOSYNC:
      return KOREADER_STORE.hasCredentials() ? ReaderUtils::GO_HOME_MS : 0;
    case CrossPointSettings::LP_MENU_READER_MENU:
    case CrossPointSettings::LP_MENU_DISABLED:
    default:
      return 0;
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;
  pauseReader();
  const std::string savedPath = bookPath;
  Command command{Control::Sync};
  runCommand(command);
  if (!command.succeeded) {
    onResume();
    return true;
  }
  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedPath, command.spineIndex, command.pageNumber, command.totalPages,
      std::move(command.syncPosition), std::move(command.text), command.paragraphIndex));
  return true;
}

void EpubReaderActivity::applyInitialOrientation() { ReaderActivity::applyInitialOrientation(); }

void EpubReaderActivity::applyOrientationOnRender(const uint8_t orientation) {
  // Also runs when SETTINGS already holds the new value but this layout was
  // built for the old one — that is what an external change looks like here.
  if (SETTINGS.orientation == orientation && appliedOrientation == orientation) {
    return;
  }

  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }

  if (SETTINGS.orientation != orientation) {
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();
  }
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  appliedOrientation = orientation;
  section.reset();
}

void EpubReaderActivity::toggleAutoPageTurnOnRender(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

bool EpubReaderActivity::pageTurn(const bool forward) {
  const bool accepted =
      pageTurns.accept(forward ? EpubPageTurns::Action::NextPage : EpubPageTurns::Action::PreviousPage,
                       gpio.getButtonFrame().capturedAtMs, millis(), inputContext);
  if (accepted) Activity::requestUpdate();
  return accepted;
}

bool EpubReaderActivity::skipPages(const int amount) {
  const bool accepted =
      pageTurns.accept(amount > 0 ? EpubPageTurns::Action::NextChapter : EpubPageTurns::Action::PreviousChapter,
                       gpio.getButtonFrame().capturedAtMs, millis(), inputContext);
  if (accepted) Activity::requestUpdate();
  return accepted;
}

void EpubReaderActivity::cancelPageTurns(const EpubPageTurns::Outcome outcome, const char* reason) {
  prepareNextPage = false;
  const auto pending = pageTurns.getCounts().pending;
  pageTurns.cancelPending(outcome, millis());
  if (pending) LOG_INF("ERS", "Page turns terminated: %lu, reason=%s", static_cast<unsigned long>(pending), reason);
}

void EpubReaderActivity::onSuspend() { pauseReader(); }

void EpubReaderActivity::onExit() {
  const auto& counts = pageTurns.getCounts();
  LOG_INF("ERS", "Turns accepted=%lu rejected=%lu applied=%lu cancelled=%lu failed=%lu pending=%lu high=%lu wait=%lums",
          static_cast<unsigned long>(counts.accepted), static_cast<unsigned long>(counts.rejected),
          static_cast<unsigned long>(counts.applied), static_cast<unsigned long>(counts.cancelled),
          static_cast<unsigned long>(counts.failed), static_cast<unsigned long>(counts.pending),
          static_cast<unsigned long>(counts.highWater), static_cast<unsigned long>(counts.maxWaitMs));
  ReaderActivity::onExit();
}

void EpubReaderActivity::processPageTurns() {
  constexpr unsigned long MIN_MANUAL_TURN_GAP_MS = 200;
  pageTurns.cancelStale(readerContext, millis());
  if (pageTurns.getCounts().pending == 0) return;

  if (!epub) {
    cancelPageTurns(EpubPageTurns::Outcome::Failed, "book unavailable");
    return;
  }
  if (atEndOnRender()) {
    cancelPageTurns(EpubPageTurns::Outcome::ContextChanged, "end of book");
    return;
  }
  if (millis() - lastPageTurnTime < MIN_MANUAL_TURN_GAP_MS) return;
  if (!section || prepareNextPage) {
    redrawRequested.store(true);
    return;
  }

  const auto step =
      pageTurns.applyTo(*section, currentSpineIndex, epub->getSpineItemsCount(), pendingPageJump, millis());
  if (step == EpubPageTurns::Step::NeedsNextPage) {
    prepareNextPage = true;
    redrawRequested.store(true);
    return;
  }
  if (step == EpubPageTurns::Step::Empty || step == EpubPageTurns::Step::Boundary) return;
  currentPageVisibleOffset.reset();
  clearDeferredReposition();
  if (step == EpubPageTurns::Step::SectionChanged) {
    nextPageNumber = 0;
    section.reset();
  }
  lastPageTurnTime = millis();
  if (atEndOnRender()) {
    changeReaderContext();
  }
  redrawRequested.store(true);
}

bool EpubReaderActivity::isAtEndOfBook() const { return readSnapshot().atEnd; }
bool EpubReaderActivity::atEndOnRender() const { return epub && currentSpineIndex >= epub->getSpineItemsCount(); }

void EpubReaderActivity::onReturnFromEndOfBook() {
  Command command{Control::ReturnFromEnd};
  runCommand(command);
}

bool EpubReaderActivity::skipLoopDelay() { return false; }

void EpubReaderActivity::renderBook() {
  currentPageLinks.clear();
  if (!epub) return;
  const bool preparingPageTurn = prepareNextPage;
  prepareNextPage = false;

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  const auto showBuildError = [this]() {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
    cancelPageTurns(EpubPageTurns::Outcome::Failed, "page preparation");
  };

  if (currentSpineIndex < 0) currentSpineIndex = 0;
  if (currentSpineIndex > epub->getSpineItemsCount()) currentSpineIndex = epub->getSpineItemsCount();

  if (currentSpineIndex == epub->getSpineItemsCount()) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = makeUniqueNoThrow<Section>(epub, currentSpineIndex, renderer);
    if (!section) {
      LOG_ERR("ERS", "OOM: reader Section");
      showBuildError();
      return;
    }
    partialRebuildStartFailed = false;
    idlePrewarmSpine = idlePrewarmPage = -1;  // rebuilt pages at the same position need a new scan

    const bool cacheLoaded = section->loadSectionFile(renderSpec);
    if (cacheLoaded) {
      cachedChapterTotalPageCount = 0;
      cachedVisibleTextOffset.reset();
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    const bool explicitOffsetJump = pendingOffsetJump.has_value();
    const std::optional<uint32_t> offsetJump =
        explicitOffsetJump ? pendingOffsetJump
        : (pendingPageJump.has_value() || !pendingAnchor.empty() || currentSpineIndex != cachedSpineIndex)
            ? std::nullopt
            : cachedVisibleTextOffset;
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        pagesUntilFullRefresh = 1;
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        GfxRenderer::FrameBufferLoan loan(renderer);
        if (!section->createSectionFile(renderSpec, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          section.reset();
          loan.end();
          showBuildError();
          return;
        }
        loan.end();
      } else {
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        if (section->isPartial() &&
            (anchorJump ? section->getPageForAnchor(pendingAnchor).has_value()
                        : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            pagesUntilFullRefresh = 1;
          }
          buildPopupPending = !showPopup;
          const unsigned long buildStartMs = millis();
          bool started;
          {
            GfxRenderer::FrameBufferLoan loan(renderer);
            started = section->startBuild(renderSpec, [this] { showBuildPopup(renderer, pagesUntilFullRefresh); });
          }
          if (!started) {
            LOG_ERR("ERS", "Failed to start section build");
            section.reset();
            buildPopupPending = false;
            showBuildError();
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump               ? !section->findAnchor(pendingAnchor)
                  : offsetJump.has_value() ? !section->buildReachedVisibleTextOffset(*offsetJump)
                                           : static_cast<int>(section->pageCount) <= target)) {
            if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
              showBuildPopup(renderer, pagesUntilFullRefresh);
            }
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              section.reset();
              buildPopupPending = false;
              showBuildError();
              return;
            }
          }
          buildPopupPending = false;
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) section->currentPage = 0;
    }

    if (offsetJump.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*offsetJump)) {
        section->currentPage = *offsetPage;
        clearDeferredReposition();
      }
    }
    if (explicitOffsetJump) {
      clearDeferredReposition();
    }
    pendingOffsetJump.reset();

    if (!pendingAnchor.empty()) {
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) newPage = section->pageCount - 1;
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  const int requiredPage = section->currentPage + (preparingPageTurn ? 1 : 0);
  if (section->isPartial() && requiredPage >= static_cast<int>(section->pageCount)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
  }
  while (section->isPartial() && requiredPage >= static_cast<int>(section->pageCount)) {
    if (!section->isBuilding() && !section->startBuild(renderSpec)) {
      LOG_ERR("ERS", "Failed to start partial extension build");
      section.reset();
      showBuildError();
      return;
    }
    while (!section->isBuildComplete() && requiredPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }
  if (section->isBuilding()) {
    while (!section->isBuildComplete() && requiredPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }

  if (preparingPageTurn) return;

  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  applyDeferredReposition();

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  {
    auto p = section->loadPage(section->currentPage);
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;
        cancelPageTurns(EpubPageTurns::Outcome::Failed, "page preparation");
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;

    currentPageVisibleOffset = p->visibleTextOffset;
    currentPageFootnotes = std::move(p->footnotes);
    currentPageLinks = std::move(p->links);
    currentPageLinkMarginLeft = orientedMarginLeft;
    currentPageLinkMarginTop = orientedMarginTop;

    // The overlay and non-tiled grayscale renderer share the renderer's single
    // stored-BW slot. Release the old page snapshot before renderContents()
    // needs that slot, then snapshot the newly rendered page below.
    discardOverlayPage();

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
    lastRenderCompleteMs = millis();
    ++pageRevision;
  }

  if (currentSpineIndex != lastSavedSpineIndex || section->currentPage != lastSavedPage ||
      section->pageCount != lastSavedPageCount) {
    if (saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages())) {
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = section->currentPage;
      lastSavedPageCount = section->estimatedTotalPages();
    }
  }

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
  }

  // Toolbar menu: overlay the toolbar / panel on top of the freshly rendered page.
  publishSnapshot();
  if (overlay != Overlay::None && usesToolbarMenu()) {
    // The page just re-rendered under the overlay: refresh the snapshot that
    // backs panel->toolbar restores (any previous copy is stale).
    overlayPageStored = renderer.storeBwBuffer();
    renderOverlay();
    // An open option picker rides on top of the freshly drawn panel.
    if (overlayPopup.isActive()) overlayPopup.render(renderer);
    // FAST, same as openOverlay: HALF's inverting pass flashes the sheet
    // (white, in night mode) on every repaint under an open panel. Any AA
    // residue a FAST differential leaves under the chrome has not shown in
    // practice; restore a HALF cleanup here if text ever visibly ghosts
    // through the sheet (see #2190 for the mechanism).
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

void EpubReaderActivity::onEndOfBookRendered() {
  automaticPageTurnActive = false;
  if (pendingSyncSaveError) {
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  }
}

bool EpubReaderActivity::applyDeferredReposition() {
  if ((!cachedVisibleTextOffset.has_value() && cachedChapterTotalPageCount == 0) || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  if (currentSpineIndex == cachedSpineIndex) {
    int newPage = section->currentPage;
    bool mappedOffset = false;
    if (cachedVisibleTextOffset.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*cachedVisibleTextOffset)) {
        newPage = *offsetPage;
        mappedOffset = true;
      }
    }
    if (!mappedOffset && cachedChapterTotalPageCount > 0 && section->pageCount != cachedChapterTotalPageCount) {
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
      newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    }
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      currentPageVisibleOffset.reset();
      changed = true;
    }
  }
  clearDeferredReposition();
  return changed;
}

void EpubReaderActivity::clearDeferredReposition() {
  cachedChapterTotalPageCount = 0;
  cachedVisibleTextOffset.reset();
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  std::optional<uint32_t> offset;
  if (section && spineIndex == currentSpineIndex && currentPage >= 0 && currentPage < section->pageCount) {
    offset = (currentPage == section->currentPage && currentPageVisibleOffset.has_value())
                 ? currentPageVisibleOffset
                 : section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
  }
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount, offset);
}

void EpubReaderActivity::rememberCurrentContentOffset() {
  cachedVisibleTextOffset.reset();
  if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    cachedVisibleTextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(section->currentPage));
  }
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  const int fontId = SETTINGS.getReaderFontId();

  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  auto* fcm = renderer.getFontCacheManager();
  if (fcm->isPrewarming() && idlePrewarmSpine == currentSpineIndex && idlePrewarmPage + 1 == section->currentPage) {
    // This speculative target is now requested; finish its remaining glyphs.
    while (!fcm->prewarmSome(UINT16_MAX)) {
    }
  }
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  // Scan the status bar too: a CJK book/chapter title redirected to the SD
  // fallback font joins the page's single batch prewarm instead of triggering
  // its own SD pass after the scope ends.
  renderStatusBar();
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  const bool cleanImageBasePending = manualRefreshPending || pagesUntilFullRefresh <= 1;
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  const bool tiledGrayscale = needsAnyGrayscale && renderer.supportsStripGrayscale();
  // Paper Mono only (no other panel combines): defer the B/W base activation so
  // the gray planes join it in a single waveform. Displaying the base
  // separately makes the gray pass re-drive the whole text body — a visible
  // flash on every AA page.
  const bool combinedGrayscaleBase = tiledGrayscale && !pageHasImages && renderer.combinesGrayscaleBase();
  const bool overlapRefresh = tiledGrayscale && renderer.supportsAsyncRefresh() && !pageHasImages;
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  if (pageHasImagesNeedingDecode) {
    page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    renderer.clearScreen();
  }

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  const auto tBwRender = millis();

  if (pageHasImages) {
    // Image pages use one base refresh before the grayscale pass. FAST leaves
    // the panel receptive to the gray waveform; pending cleanup still honors
    // the scheduled/manual HALF refresh.
    renderer.displayBuffer(cleanImageBasePending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    pagesUntilFullRefresh = 1;
  } else if (combinedGrayscaleBase) {
    // Stash the base without activating; displayGrayBuffer() below commits
    // base + grays as one waveform.
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh);
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, overlapRefresh);
  }
  const auto tDisplay = millis();

  if (tiledGrayscale) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();
    const size_t planeBytes = static_cast<size_t>(gwBytes) * gh;

    auto renderPlaneToBuffer = [&](const bool lsbPlane, uint8_t* buf) {
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      renderer.beginStripTarget(buf, 0, gh);
      renderer.clearScreen(0x00);
      renderGrayscalePass();
      renderer.endStripTarget();
    };

    constexpr size_t PLANE_BUF_HEADROOM = 60000;
    constexpr size_t PLANE_BUF_MAX_ALLOC_RESERVE = 16 * 1024;
    const auto planeBufFits = [planeBytes] {
      return ESP.getFreeHeap() >= planeBytes + PLANE_BUF_HEADROOM &&
             ESP.getMaxAllocHeap() >= planeBytes + PLANE_BUF_MAX_ALLOC_RESERVE;
    };
    auto lsbPlaneBuf = (overlapRefresh && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
    auto msbPlaneBuf = (lsbPlaneBuf && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;

    if (lsbPlaneBuf) {
      renderPlaneToBuffer(true, lsbPlaneBuf.get());
      if (msbPlaneBuf) renderPlaneToBuffer(false, msbPlaneBuf.get());
      const auto tGrayRender = millis();

      renderer.waitRefreshComplete();
      const auto tWait = millis();

      renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, gh);
      if (msbPlaneBuf) {
        renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf.get(), 0, gh);
      } else {
        renderPlaneToBuffer(false, lsbPlaneBuf.get());
        renderer.writeGrayscalePlaneStrip(false, lsbPlaneBuf.get(), 0, gh);
      }
      const auto tGrayWrite = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tEnd = millis();

      LOG_DBG("ERS",
              "Page render (tiled async): prewarm=%lums bw_render=%lums display=%lums gray_render=%lums "
              "wait=%lums gray_write=%lums gray_display=%lums cleanup=%lums total=%lums (planes buffered: %d)",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayRender - tDisplay, tWait - tGrayRender,
              tGrayWrite - tWait, tGrayDisplay - tGrayWrite, tEnd - tGrayDisplay, tEnd - t0, msbPlaneBuf ? 2 : 1);
    } else {
      auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
      renderer.waitRefreshComplete();
      if (!scratch) {
        LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
        if (overlapRefresh || combinedGrayscaleBase) {
          // The BW refresh ran the shadow-free async path, so controller RAM's
          // differential baseline was never rebuilt. Even with AA skipped it must
          // be re-synced from the intact BW framebuffer, or the next differential
          // update diffs against stale contents. On the combined-base path the
          // base activation is still deferred; this cleanup commits it so the
          // page reaches the panel even without its grays.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }
      } else {
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
        }
        const auto tGrayLsb = millis();

        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
        }
        const auto tGrayMsb = millis();

        renderer.setRenderMode(GfxRenderer::BW);
        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();

        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
                "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
                tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
      }
    }
  } else {
    if (needsAnyGrayscale) {
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        return;
      }
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderGrayscalePass();
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderGrayscalePass();
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  const int currentPage = section ? section->currentPage + 1 : 1;
  const float pageCount = section ? section->estimatedTotalPages() : 1;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub ? (epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100) : 0;

  std::string title;
  int textYOffset = 0;
  const auto sb = SETTINGS.statusBarSpec();

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    if (epub) {
      const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
      if (tocIndex != -1) {
        const auto tocItem = epub->getTocItem(tocIndex);
        title = tocItem.title;
      }
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub ? epub->getTitle() : "";
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    section ? section->isBuilding() : false);
}

// ---------------------------------------------------------------------------
// Toolbar reader menu
// ---------------------------------------------------------------------------

namespace {
constexpr StrId kTextRowNames[] = {StrId::STR_FONT, StrId::STR_FONT_SIZE, StrId::STR_LINE_SPACING,
                                   StrId::STR_PARA_ALIGNMENT, StrId::STR_FOCUS_READING};
constexpr StrId kSpacingIds[] = {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE, StrId::STR_EXTRA_WIDE};
constexpr StrId kAlignIds[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                               StrId::STR_BOOK_S_STYLE};
constexpr int kTextRowCount = static_cast<int>(std::size(kTextRowNames));
static_assert(std::size(kSpacingIds) == CrossPointSettings::LINE_COMPRESSION_COUNT, "line spacing labels");
static_assert(std::size(kAlignIds) == CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, "alignment labels");
}  // namespace

bool EpubReaderActivity::usesToolbarMenu() const {
  // Touch-first chrome: button boards always get the classic list menu, even
  // if a settings file (e.g. an SD card moved from a touch board) says Toolbar.
  return mappedInput.hasTouch() && SETTINGS.readerMenuStyle == CrossPointSettings::READER_MENU_TOOLBAR;
}

std::string EpubReaderActivity::currentChapterTitle() const {
  if (!epub) return "";
  const int tocIndex = epub->getTocIndexForSpineIndex(readSnapshot().spineIndex);
  if (tocIndex != -1) {
    return epub->getTocItem(tocIndex).title;
  }
  return tr(STR_UNNAMED);
}

std::string EpubReaderActivity::textRowName(int row) const {
  return row >= 0 && row < kTextRowCount ? I18N.get(kTextRowNames[row]) : "";
}

std::string EpubReaderActivity::textRowValue(int row) const {
  static constexpr StrId kFamily[] = {StrId::STR_NOTO_SERIF, StrId::STR_NOTO_SANS, StrId::STR_KIMCHI_BATANG};
  switch (row) {
    case 0:
      if (SETTINGS.sdFontFamilyName[0] != '\0') return SETTINGS.sdFontFamilyName;
      return I18N.get(kFamily[builtinReaderFamilyIndex(SETTINGS.fontFamily)]);
    case 1:
      return std::to_string(SETTINGS.fontPointSize) + " pt";
    case 2:
      return I18N.get(kSpacingIds[SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT]);
    case 3:
      return I18N.get(kAlignIds[SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT]);
    case 4:
      return SETTINGS.focusReadingEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    default:
      return "";
  }
}

// Live apply: persist, re-paginate, and let renderBook() redraw the page with
// the open panel back on top -- the book itself is the preview.
void EpubReaderActivity::applyTextSettingLive() {
  applyReaderTextSettings();
  discardOverlayPage();  // the stored page is laid out with the old settings
  requestUpdate();
}

// Settings-style option pickers for the Text panel's enum rows. Every
// selection applies immediately to the page under the sheet.
void EpubReaderActivity::showTextRowPopup(const int row) {
  switch (row) {
    case 1: {
      // The point sizes the active family actually ships.
      const auto sizes = readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.sdFontFamilyName, SETTINGS.fontFamily);
      if (sizes.empty()) return;
      std::vector<std::string> labels;
      labels.reserve(sizes.size());
      for (const uint8_t size : sizes) labels.push_back(std::to_string(size) + " pt");
      const uint8_t cur = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
      int curIdx = 0;
      for (size_t i = 0; i < sizes.size(); ++i) {
        if (sizes[i] == cur) curIdx = static_cast<int>(i);
      }
      overlayPopup.show(StrId::STR_FONT_SIZE, labels, curIdx, [this, sizes](int idx) {
        if (idx < 0 || idx >= static_cast<int>(sizes.size())) return;
        {
          RenderLock lock;
          SETTINGS.fontPointSize = sizes[idx];
        }
        applyTextSettingLive();
      });
      break;
    }
    case 2:
      overlayPopup.show(StrId::STR_LINE_SPACING, kSpacingIds, static_cast<int>(std::size(kSpacingIds)),
                        SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT, [this](int idx) {
                          {
                            RenderLock lock;
                            SETTINGS.lineSpacing = static_cast<uint8_t>(idx);
                          }
                          applyTextSettingLive();
                        });
      break;
    case 3:
      overlayPopup.show(StrId::STR_PARA_ALIGNMENT, kAlignIds, static_cast<int>(std::size(kAlignIds)),
                        SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, [this](int idx) {
                          {
                            RenderLock lock;
                            SETTINGS.paragraphAlignment = static_cast<uint8_t>(idx);
                          }
                          applyTextSettingLive();
                        });
      break;
    default:
      return;
  }
  paintOverlayPopup();
}

void EpubReaderActivity::discardOverlayPage() {
  if (!overlayPageStored) return;
  renderer.discardStoredBwBuffer();
  overlayPageStored = false;
}

void EpubReaderActivity::openOverlay(Overlay target) {
  pauseReader();
  RenderLock lock;
  const auto state = readSnapshot();
  mappedInput.discardPendingInput();
  const Overlay previous = overlay;
  overlay = target;
  if (!toolbarUi) toolbarUi = std::make_unique<ReaderToolbarUi>(renderer);
  if (previous == Overlay::None) toolbarUi->begin();
  // Buttons show a cursor from the start; touch boards only once a button moves it.
  panelCursorShown = !mappedInput.hasTouch();
  switch (target) {
    case Overlay::Toolbar:
      focusedTool = 0;
      break;
    case Overlay::Contents:
      panelIndex = std::max(0, epub->getTocIndexForSpineIndex(state.spineIndex));
      // Fresh viewport opening on the current chapter, cursor shown or not.
      toolbarUi->nav().reset(panelIndex);
      toolbarUi->nav().top = panelIndex;
      break;
    case Overlay::Text:
      panelIndex = 0;
      toolbarUi->nav().reset();
      break;
    case Overlay::More:
      panelIndex = 0;
      buildMoreActions();
      toolbarUi->nav().reset();
      break;
    default:
      break;
  }
  panelHoldJumped = false;

  // The page is already on screen and still in the framebuffer, so paint the
  // chrome straight onto it and push one refresh. requestUpdate() would
  // re-render the whole page first: slow, and visibly wrong, since that repaint
  // lands before the overlay does.
  //
  // Refresh mode: FAST for every overlay paint, first open included. The AA
  // pass only grays glyph edges, and residue a FAST differential leaves under
  // the sheet has not shown in practice; it also self-heals on the
  // Xteink-class panels, whose close path re-renders the page. If text or
  // images ever visibly ghost through the chrome, restore a HALF cleanup on
  // the first open (see #2190 for the mechanism).
  if (state.hasSection) {
    // Serialize against the render task: renderBook may be mid-page (status
    // bar included) in the shared framebuffer, and painting the chrome from
    // the loop task at the same time interleaves the two frames.
    if (previous == Overlay::None) {
      // Snapshot the clean page so stepping back from a panel to the toolbar
      // (and closing, where supported) can restore it without a re-render.
      overlayPageStored = renderer.storeBwBuffer();
    } else if (overlayPageStored) {
      // Overlay -> overlay: wipe the previous chrome (toolbar header, sheet,
      // progress row) back to the clean page so none of it shows around or
      // through the new sheet; re-store for the next transition. No baseline
      // resync: the glass still shows the old chrome, and the differential
      // must keep diffing against it to erase it.
      renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
      overlayPageStored = renderer.storeBwBuffer();
    }
    renderOverlay();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    requestUpdate();  // no page yet: renderBook() draws the overlay once it is
  }
}

// Close the overlay back to the reading page. Boards without the Xteink
// grayscale-AA pass restore the page snapshot and push one FAST refresh -- no
// re-render, no flash; Xteink boards re-render to restore the AA planes.
void EpubReaderActivity::closeOverlayToPage() {
  {
    RenderLock lock;
    overlay = Overlay::None;
    overlayPopup.dismiss();
    toolbarUi.reset();
    if (!xteinkClassPanel() && overlayPageStored) {
      renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
      overlayPageStored = false;
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      discardOverlayPage();
      requestUpdate();
    }
  }
  onResume();
}

void EpubReaderActivity::renderOverlay() {
  const auto state = readSnapshot();
  if (!epub || !state.hasSection || !toolbarUi) return;

  ReaderToolbarUi::Model model;
  // The toolbar's tool pill is the button-navigation cursor: tap-first (same
  // convention as the panel lists), it only shows once a button has moved it.
  // Panels override below: there the pill marks the open panel on every board.
  model.activeTool = (overlay == Overlay::Toolbar && !panelCursorShown) ? -1 : focusedTool;
  // Strings the model points at live here until render() returns.
  std::string chapterTitle, pageInfo;

  if (overlay == Overlay::Toolbar) {
    chapterTitle = currentChapterTitle();
    const int pageCount = state.pageCount;
    const float chapterProgress =
        pageCount > 0 ? static_cast<float>(state.page + 1) / static_cast<float>(pageCount) : 0.0f;
    const float bookProgress = epub->calculateProgress(state.spineIndex, chapterProgress);
    pageInfo = std::to_string(state.page + 1) + "/" + std::to_string(pageCount) + "   " +
               std::to_string(clampPercent(static_cast<int>(bookProgress * 100.0f + 0.5f))) + "%";
    model.chapterTitle = chapterTitle.c_str();
    model.pageInfo = pageInfo.c_str();
    model.progressPermille = static_cast<int>(bookProgress * 1000.0f + 0.5f);
    toolbarUi->setModel(model);
    toolbarUi->render();
    return;
  }

  // Panels (Contents / Text / More): a bottom sheet over the page + button hints.
  model.panel = true;
  if (!mappedInput.hasTouch()) {
    model.bottomReserve = UITheme::getInstance().getMetrics().buttonHintsHeight;
    model.denseRows = true;
  }
  // Tap-first: the cursor is only drawn once a button has moved it, so a
  // tapped row does not stay inverted after its action.
  model.selectedIndex = panelCursorShown ? panelIndex : -1;
  if (overlay == Overlay::Contents) {
    model.panelTitle = tr(STR_TOOL_CONTENTS);
    model.itemCount = epub->getTocItemsCount();
    model.rowText = [this](int i) {
      const auto item = epub->getTocItem(i);
      const int depth = item.level > 1 ? (item.level - 1) * 2 : 0;
      return std::string(depth, ' ') + item.title;
    };
  } else if (overlay == Overlay::Text) {
    model.panelTitle = tr(STR_TOOL_TEXT);
    model.itemCount = kTextRowCount;
    model.rowText = [this](int i) { return textRowName(i); };
    model.rowValue = [this](int i) { return textRowValue(i); };
  } else {
    model.panelTitle = tr(STR_TOOL_MORE);
    model.itemCount = static_cast<int>(moreItems.size());
    model.rowText = [this](int i) { return moreRowName(i); };
    model.rowValue = [this](int i) { return moreRowValue(i); };
  }
  toolbarUi->setModel(model);
  toolbarUi->render();

  if (!mappedInput.hasTouch()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

void EpubReaderActivity::handleOverlayInput() {
  if (!toolbarUi) return;
  // Finish any outstanding preview before main changes the overlay model.
  if (hasInputActivity()) pauseReader();

  // A modal option picker over the panel owns all input while open.
  if (overlayPopup.isActive()) {
    overlayPopup.handleInput(mappedInput, [this] {
      if (overlayPopup.isActive()) {
        paintOverlayPopup();  // highlight moved
        return;
      }
      // Dismissed or selected: erase the dialog -- clean page back, then the
      // panel over it (the dialog can overhang the sheet onto the page).
      RenderLock lock;
      if (overlayPageStored) {
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
        renderOverlay();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      } else {
        requestUpdate();
      }
    });
    return;
  }
  const auto fastRedraw = [this] {
    RenderLock lock;  // the render task shares the framebuffer
    renderOverlay();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  };

  // Jump to another spine item (chapter scrub). The overlay stays up and is
  // re-drawn over the new page by renderBook().
  const auto gotoSpine = [this](int target) {
    const int spineCount = epub->getSpineItemsCount();
    target = std::clamp(target, 0, spineCount - 1);
    if (target != readSnapshot().spineIndex) {
      Command command{Control::Chapter};
      command.argument = target;
      runCommand(command);
    }
    requestUpdate();
  };
  const auto toolOverlay = [](int tool) {
    return tool == 0 ? Overlay::Contents : (tool == 1 ? Overlay::Text : Overlay::More);
  };

  // Touch first: FreeInkUI routes the frame against the tap targets the last
  // render registered and hands back the action it mapped to.
  const auto routed = toolbarUi->route(mappedInput);

  // --- Toolbar ---
  if (overlay == Overlay::Toolbar) {
    switch (routed.event) {
      case ReaderToolbarUi::Event::Dismiss:
        closeOverlayToPage();
        return;
      case ReaderToolbarUi::Event::Tool:
        focusedTool = routed.value;
        openOverlay(toolOverlay(focusedTool));
        return;
      case ReaderToolbarUi::Event::PrevChapter:
        gotoSpine(readSnapshot().spineIndex - 1);
        return;
      case ReaderToolbarUi::Event::NextChapter:
        gotoSpine(readSnapshot().spineIndex + 1);
        return;
      case ReaderToolbarUi::Event::Scrub:
        gotoSpine(static_cast<int>((static_cast<float>(routed.permille) / 1000.0f) *
                                       static_cast<float>(epub->getSpineItemsCount() - 1) +
                                   0.5f));
        return;
      default:
        break;
    }
    if (routed.routed) return;  // a touch frame the chrome consumed (or dead space)

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeOverlayToPage();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      focusedTool = (focusedTool + 2) % 3;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      focusedTool = (focusedTool + 1) % 3;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openOverlay(toolOverlay(focusedTool));
      return;
    }
    const bool prev = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool next = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (prev || next) {
      gotoSpine(readSnapshot().spineIndex + (next ? 1 : -1));
    }
    return;
  }

  // --- Panels (Contents / Text / More) ---
  const int count = overlay == Overlay::Contents ? epub->getTocItemsCount()
                    : overlay == Overlay::Text   ? kTextRowCount
                                                 : static_cast<int>(moreItems.size());
  const int pageRows = std::max(1, toolbarUi->visibleRows());

  // Activate the highlighted row: change a value / jump to a chapter / run an
  // action. Shared by the Confirm button and a row tap.
  const auto activateRow = [this, count, &fastRedraw] {
    if (panelIndex < 0 || panelIndex >= count) return;
    if (overlay == Overlay::Text) {
      if (panelIndex == 0) {
        // Full font picker (built-in + SD fonts, live preview) -- the same
        // screen Settings uses; a popup cannot scroll a long font list.
        overlay = Overlay::None;
        overlayPopup.dismiss();
        discardOverlayPage();
        Command command{Control::ReleaseSection};
        runCommand(command);
        startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                      TextSettingsActivity::Tab::Family),
                               [this](const ActivityResult&) {
                                 applyReaderTextSettings();
                                 overlay = Overlay::Text;  // back to the Text panel
                                 panelIndex = 0;
                                 if (toolbarUi) toolbarUi->begin();  // the picker drew its own FUI screen
                                 requestUpdate();                    // re-render page + Text panel
                               });
      } else if (panelIndex == 4) {
        // Focus Reading is a genuine on/off: a tap toggles and applies live.
        {
          RenderLock lock;
          SETTINGS.focusReadingEnabled = SETTINGS.focusReadingEnabled ? 0 : 1;
        }
        applyTextSettingLive();
      } else {
        // Enum rows open the Settings-style option picker.
        showTextRowPopup(panelIndex);
      }
    } else if (overlay == Overlay::Contents) {
      Command command{Control::Toc};
      command.argument = panelIndex;
      runCommand(command);
      overlay = Overlay::None;
      discardOverlayPage();
      requestUpdate();
    } else if (overlay == Overlay::More) {
      activateMoreRow(panelIndex);
    }
  };

  // Steps up to the toolbar -- the Back button and a tap on the page above
  // the sheet.
  const auto dismissPanel = [this, &fastRedraw] {
    overlay = Overlay::Toolbar;
    // Restore the snapshotted page under the toolbar instead of re-rendering
    // it (2+ refreshes -> one FAST). Re-store right away so another panel
    // round-trip can restore again.
    if (overlayPageStored) {
      {
        RenderLock lock;  // the render task shares the framebuffer
        // No baseline resync: the glass shows the panel, and erasing it needs
        // the differential to keep diffing against the last pushed frame.
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
      }
      fastRedraw();  // takes its own RenderLock
      return;
    }
    requestUpdate();
  };

  // Pages the list by one screen of rows through the nav (measured page size,
  // no-op at the ends). A shown cursor rides along so the buttons continue
  // from what is visible; on touch boards only the viewport moves.
  const auto pageList = [this, count, pageRows, &fastRedraw](int direction) {
    if (count <= 0) return;
    const bool moved = toolbarUi->nav().scrollBy(direction * pageRows, count);
    if (panelCursorShown) {
      panelIndex = std::clamp(panelIndex + direction * pageRows, 0, count - 1);
      fastRedraw();
      return;
    }
    if (moved) fastRedraw();
  };

  switch (routed.event) {
    case ReaderToolbarUi::Event::Dismiss:
      dismissPanel();
      return;
    case ReaderToolbarUi::Event::Tool: {
      // Sheet-bottom tool switcher: hop straight to another panel.
      const Overlay target = toolOverlay(routed.value);
      if (target != overlay) {
        focusedTool = routed.value;
        openOverlay(target);
      }
      return;
    }
    case ReaderToolbarUi::Event::Row:
      // A tap on the right-edge strip pages the sheet instead (upper half =
      // previous page, lower half = next): swipes are unreliable on etched
      // glass, and a long contents list needs a fast way through.
      if (routed.x >= renderer.getScreenWidth() - 44) {
        pageList(routed.y >= renderer.getScreenHeight() - (renderer.getScreenHeight() * 62) / 200 ? 1 : -1);
        return;
      }
      panelIndex = routed.value;
      panelCursorShown = false;
      activateRow();
      return;
    default:
      break;
  }
  // Swipe up/down pages the list. Checked before the routed-frame return:
  // FUI routes every touch frame over the sheet, so a swipe's frames count as
  // routed (without dispatching -- too much travel for a tap) and the gesture
  // would otherwise never be seen.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    pageList(swipe == MappedInputManager::SwipeDir::Up ? 1 : -1);
    return;
  }
  if (routed.routed) return;  // consumed by the chrome (title band, dead space)

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    dismissPanel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateRow();
    return;
  }

  // Up/Down (side) and Left/Right (front) move the cursor: a tap steps one
  // row, holding past PANEL_HOLD_MS jumps PANEL_HOLD_STEP rows in one go, which
  // is how you cross a hundreds-of-chapters contents list without a press per
  // row. The jump fires once on the hold and swallows the release that ends it,
  // so it never doubles up with the tap step.
  if (count > 0) {
    const bool up = mappedInput.isPressed(MappedInputManager::Button::Up) ||
                    mappedInput.isPressed(MappedInputManager::Button::Left);
    const bool down = mappedInput.isPressed(MappedInputManager::Button::Down) ||
                      mappedInput.isPressed(MappedInputManager::Button::Right);
    if (!panelHoldJumped && (up || down) && mappedInput.getHeldTime() >= PANEL_HOLD_MS) {
      const int step = down ? PANEL_HOLD_STEP : -PANEL_HOLD_STEP;
      panelIndex = std::clamp(panelIndex + step, 0, count - 1);
      panelHoldJumped = true;
      panelCursorShown = true;
      fastRedraw();
      return;
    }

    const bool releasedUp = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool releasedDown = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (releasedUp || releasedDown) {
      if (!panelHoldJumped) {
        panelIndex = releasedUp ? ButtonNavigator::previousIndex(panelIndex, count)
                                : ButtonNavigator::nextIndex(panelIndex, count);
        panelCursorShown = true;
        fastRedraw();
      }
      panelHoldJumped = false;
    }
  }
}

// First paint of the option picker over the panel (and highlight repaints).
// The dialog draws over the current framebuffer without clearing; erasing it
// on dismissal is the popup gate's restore in handleOverlayInput().
void EpubReaderActivity::paintOverlayPopup() {
  RenderLock lock;
  overlayPopup.render(renderer);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void EpubReaderActivity::applyReaderTextSettingsOnRender() {
  SETTINGS.saveToFile();
  releaseSection();
  sdFontSystem.ensureLoaded(renderer);
}

// The More panel carries everything the classic list menu offers except the
// two entries that have their own tool (chapters -> Contents, text -> Text).
void EpubReaderActivity::buildMoreActions() {
  using MA = EpubReaderMenuActivity::MenuAction;
  const auto state = readSnapshot();
  EpubReaderMenuActivity::buildMenuItems(moreItems, state.footnoteCount != 0, state.hasBookmarks);
  moreItems.erase(std::remove_if(moreItems.begin(), moreItems.end(),
                                 [](const auto& item) {
                                   return item.action == MA::SELECT_CHAPTER || item.action == MA::TEXT_SETTINGS;
                                 }),
                  moreItems.end());
}

std::string EpubReaderActivity::moreRowName(int row) const {
  return row >= 0 && row < static_cast<int>(moreItems.size()) ? I18N.get(moreItems[row].labelId) : "";
}

std::string EpubReaderActivity::moreRowValue(int row) const {
  using MA = EpubReaderMenuActivity::MenuAction;
  static constexpr StrId kOrient[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED,
                                      StrId::STR_LANDSCAPE_CCW};
  static_assert(std::size(kOrient) == CrossPointSettings::ORIENTATION_COUNT, "orientation labels");
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return "";
  switch (moreItems[row].action) {
    case MA::ROTATE_SCREEN:
      return I18N.get(kOrient[SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT]);
    case MA::AUTO_PAGE_TURN:
      return (autoTurnOption == 0 || autoTurnOption >= static_cast<int>(std::size(PAGE_TURN_RATES)))
                 ? std::string(tr(STR_STATE_OFF))
                 : std::to_string(PAGE_TURN_RATES[autoTurnOption]);
    case MA::NIGHT_MODE:
      return SETTINGS.screenInverted ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case MA::FRONTLIGHT:
      return Frontlight.isOn() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    default:
      return "";
  }
}

void EpubReaderActivity::activateMoreRow(int row) {
  using MA = EpubReaderMenuActivity::MenuAction;
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return;
  const auto action = moreItems[row].action;
  // In-place toggles keep the panel open and re-render the page beneath it.
  switch (action) {
    case MA::ROTATE_SCREEN: {
      static constexpr StrId kOrientIds[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                             StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
      static_assert(std::size(kOrientIds) == CrossPointSettings::ORIENTATION_COUNT, "orientation options");
      overlayPopup.show(StrId::STR_ORIENTATION, kOrientIds, static_cast<int>(std::size(kOrientIds)),
                        SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT, [this](int idx) {
                          if (idx == SETTINGS.orientation) return;
                          applyOrientation(static_cast<uint8_t>(idx));
                          // The stored page is laid out for the old orientation.
                          discardOverlayPage();
                          requestUpdate();
                        });
      paintOverlayPopup();
      return;
    }
    case MA::AUTO_PAGE_TURN: {
      std::vector<std::string> labels;
      labels.reserve(std::size(PAGE_TURN_RATES));
      labels.emplace_back(tr(STR_STATE_OFF));
      for (size_t i = 1; i < std::size(PAGE_TURN_RATES); ++i) labels.push_back(std::to_string(PAGE_TURN_RATES[i]));
      overlayPopup.show(StrId::STR_AUTO_TURN_PAGES_PER_MIN, labels, autoTurnOption, [this](int idx) {
        autoTurnOption = idx;
        toggleAutoPageTurn(static_cast<uint8_t>(idx));
      });
      paintOverlayPopup();
      return;
    }
    case MA::NIGHT_MODE:
      SETTINGS.screenInverted = SETTINGS.screenInverted == 0 ? 1 : 0;
      SETTINGS.saveToFile();
      discardOverlayPage();
      requestUpdate();
      return;
    case MA::FRONTLIGHT: {
      const bool lightOn = !Frontlight.isOn();
      Frontlight.setOn(lightOn);
      SETTINGS.frontlightOn = lightOn ? 1 : 0;
      SETTINGS.saveToFile();
      {
        RenderLock lock;  // the render task shares the framebuffer
        renderOverlay();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      }
      return;
    }
    default:
      break;
  }
  // Leaf actions open their own screen / perform the action; close the overlay first.
  overlay = Overlay::None;
  discardOverlayPage();
  if (action == MA::TOGGLE_BOOKMARK) {
    // No child activity here to trigger the re-render the list menu relies on:
    // show the same confirmation popup the long-press path does.
    addBookmark(true);
    onResume();
    requestUpdate();
    return;
  }
  onReaderMenuConfirm(action);
  if (!activityManager.hasPendingTransition()) onResume();
  // Actions that neither open a screen nor leave the reader (a sync with no
  // credentials, say) would otherwise leave the closed panel on screen.
  if (action != MA::GO_HOME && action != MA::DELETE_CACHE) requestUpdate();
}

void EpubReaderActivity::navigateToHrefOnRender(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';
  int targetSpineIndex = sameFile ? currentSpineIndex : epub->resolveHrefToSpineIndex(hrefStr);

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;
    return;
  }

  {
    clearDeferredReposition();
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPositionOnRender() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    clearDeferredReposition();
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  BookmarkFile::load(epub->getPath(), cachedBookmarks);
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmarkOnRender() {
  if (!section || !epub) return;
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, currentPositionOnRender());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    const std::optional<uint32_t> offset =
        currentPageVisibleOffset.has_value() ? currentPageVisibleOffset
        : (currentPage >= 0 && currentPage < section->pageCount)
            ? section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))
            : std::nullopt;
    if (offset.has_value()) {
      entry.visibleTextOffset = *offset;
      entry.hasVisibleTextOffset = true;
    }
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  if (!BookmarkFile::save(epub->getPath(), cachedBookmarks)) {
    LOG_ERR("ERS", "Failed to save bookmarks");
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  const auto state = readSnapshot();
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
  info.spineIndex = state.spineIndex;
  if (state.hasSection) {
    info.currentPage = state.page + 1;
    info.totalPages = state.pageCount;
    info.progressPercent = state.progressPercent;
  }
  return info;
}

CrossPointPosition EpubReaderActivity::currentPositionOnRender() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    if (const auto offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))) {
      localPos.visibleTextOffset = *offset;
      localPos.hasVisibleTextOffset = true;
    }
  }
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}

void EpubReaderActivity::applyProgressOnRender(const ProgressChangeResult& sync) {
  if (sync.hasVisibleTextOffset && sync.spineIndex >= 0 && sync.spineIndex < epub->getSpineItemsCount()) {
    clearDeferredReposition();
    if (section && currentSpineIndex == sync.spineIndex) {
      const auto page = section->getPageForVisibleTextOffset(sync.visibleTextOffset);
      section->currentPage = page.value_or(std::max(0, sync.page));
    } else {
      currentSpineIndex = sync.spineIndex;
      pendingOffsetJump = sync.visibleTextOffset;
      nextPageNumber = std::max(0, sync.page);
      section.reset();
    }
    requestUpdate();
    return;
  }

  int targetSpineIndex = sync.spineIndex;
  int targetPage = sync.page;
  const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
  const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 && currentSpineIndex == sync.spineIndex &&
                                              sync.page >= 0 && sync.page < sync.totalPages &&
                                              activeTotalPages == sync.totalPages;

  if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
    const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
    CrossPointPosition fallback =
        ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
    targetSpineIndex = fallback.spineIndex;
    targetPage = fallback.pageNumber;
  }

  clearDeferredReposition();

  if (currentSpineIndex != targetSpineIndex) {
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = targetPage;
    section.reset();
  } else if (section && section->currentPage != targetPage) {
    const int clampedTargetPage = std::max(0, targetPage);
    section->currentPage = clampedTargetPage;
  } else if (!section) {
    nextPageNumber = targetPage;
  }
  requestUpdate();
}
