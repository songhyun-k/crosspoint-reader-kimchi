#pragma once

#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/PageLink.h>
#include <Epub/Section.h>
#include <FontCacheManager.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "BookmarkEntry.h"
#include "EpubPageTurns.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "ReaderActivity.h"
#include "ReaderToolbarUi.h"
#include "components/OptionPopup.h"

namespace EpubReaderUtils {
struct IdleWork;
}

class EpubReaderActivity final : public ReaderActivity {
  struct Snapshot {
    uint32_t context = 0;
    uint32_t pageRevision = 0;
    int spineIndex = 0;
    int page = 0;
    int pageCount = 0;
    int availablePageCount = 0;
    int progressPercent = 0;
    int footnoteCount = 0;
    int footnoteDepth = 0;
    uint8_t orientation = 0;
    bool loaded = false;
    bool hasSection = false;
    bool atEnd = false;
    bool hasLinks = false;
    bool hasBookmarks = false;
    bool bookmarkMessage = false;
    bool dictionaryMessage = false;
    bool automatic = false;
    bool paused = false;
    std::optional<uint32_t> wakeAt;
  };
  enum class Control {
    Open,
    Pause,
    Resume,
    Chapter,
    Toc,
    Percent,
    Href,
    LinkAtPoint,
    RestoreFootnote,
    Orientation,
    TextSettings,
    ReleaseSection,
    Progress,
    AutoTurn,
    Bookmark,
    ReloadBookmarks,
    DictionaryPage,
    Footnotes,
    RestoreFootnotes,
    QrText,
    Sync,
    DeleteCache,
    Screenshot,
    ReturnFromEnd
  };
  struct Command {
    Control type;
    int argument = 0;
    int secondArgument = 0;
    bool flag = false;
    bool succeeded = false;
    uint32_t pageRevision = 0;
    std::string text;
    ProgressChangeResult progress;
    int spineIndex = 0;
    int pageNumber = 0;
    int totalPages = 0;
    std::optional<uint16_t> paragraphIndex;
    SavedProgressPosition syncPosition{};
    std::unique_ptr<Page> page;
    std::vector<FootnoteEntry> footnotes;
  };
  // Only main publishes a command and keeps its owning values alive until acknowledgement.
  std::atomic<Command*> pendingControl{nullptr};
  std::atomic<bool> redrawRequested{false};
  std::atomic<bool> inputActive{false};
  mutable portMUX_TYPE snapshotMux = portMUX_INITIALIZER_UNLOCKED;
  Snapshot snapshot;
  uint32_t inputContext = 0;   // Main's last observed context.
  uint32_t readerContext = 0;  // Render-task context for accepted requests.
  uint32_t pageRevision = 0;   // Completed page render, distinct from location application.
  bool readerPaused = false;
#if defined(ARDUINO_ARCH_ESP32)
  static_assert(sizeof(Command) < 256);
#endif
  Snapshot readSnapshot() const;
  void publishSnapshot();
  void runCommand(Command& command);
  void executeCommand(Command& command);
  void pauseReader();
  void changeReaderContext();
  bool loadBookOnRender();
  void jumpToPercentOnRender(int percent);
  bool jumpToChapterOnRender(int spineIndex, const std::string& anchor);
  void navigateToHrefOnRender(const std::string& href, bool savePosition);
  void restoreSavedPositionOnRender();
  void applyOrientationOnRender(uint8_t orientation);
  void toggleAutoPageTurnOnRender(uint8_t option);
  void applyReaderTextSettingsOnRender();
  void addBookmarkOnRender();
  void applyProgressOnRender(const ProgressChangeResult& progress);
  void releaseSection();
  CrossPointPosition currentPositionOnRender() const;
  bool atEndOnRender() const;
  // Metadata is published after Open. Section, navigation and build state are
  // render-task-owned; main reads the value snapshot or sends a control command.
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  bool renderRetired = false;  // Render-task owned; blocks stale notifications after cleanup.
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  std::string pendingAnchor;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  std::optional<uint32_t> currentPageVisibleOffset;
  std::optional<uint32_t> pendingOffsetJump;
  static constexpr uint32_t MIN_MANUAL_TURN_GAP_MS = 200;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  EpubPageTurns pageTurns;
  enum class PageTarget : uint8_t { Current, Navigation, NextPage };
  enum class Preparation : uint8_t { Pending, Ready, Error };
  PageTarget pageTarget = PageTarget::Current;
  std::unique_ptr<Page> pendingRenderPage;
  std::optional<FontCacheManager::PrewarmScope> renderFontScope;
  std::optional<uint32_t> buildStartedAt;
  uint32_t prewarmStartedAt = 0;
  Preparation prepareSection(const ReaderRenderSpec& spec);
  void clearPendingNavigation();
  void retirePagePreparation();
  void retireSection();
  void processPageTurns();
  void cancelPageTurns(EpubPageTurns::Outcome outcome, const char* reason);
  bool pendingPercentJump = false;
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool currentPageBookmarked = false;
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  unsigned long lastRenderCompleteMs = 0;
  bool bookmarkRemoved = false;
  std::vector<BookmarkEntry> cachedBookmarks;
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  bool pendingReadFolderMove = false;

  // Toolbar reader menu (SETTINGS.readerMenuStyle == READER_MENU_TOOLBAR): drawn
  // over the page instead of pushing the full-screen list menu. Select opens the
  // Toolbar; its tools open the Contents/Text/More bottom-sheet panels.
  enum class Overlay { None, Toolbar, Contents, Text, More };
  Overlay overlay = Overlay::None;
  int focusedTool = 0;  // toolbar tool focus: 0=Contents, 1=Text, 2=More
  int panelIndex = 0;   // selected row within the active panel
  // Panel list navigation: a tap steps one row, a hold jumps PANEL_HOLD_STEP rows in one go
  // (a contents list runs to hundreds of chapters). One jump per hold, not a repeat -- every
  // step repaints the panel, so repeating is bounded by the e-ink refresh anyway and reads as
  // sluggish. True once a hold has jumped, so the release that ends it is swallowed.
  static constexpr unsigned long PANEL_HOLD_MS = 1500;
  static constexpr int PANEL_HOLD_STEP = 10;
  bool panelHoldJumped = false;
  // Whether the panel draws its cursor row. Button boards always do; touch
  // boards only once a button has moved it, so a tapped row is not left inverted.
  bool panelCursorShown = false;
  // FreeInkUI chrome + tap targets for the overlay; created when it opens,
  // released when it closes.
  std::unique_ptr<ReaderToolbarUi> toolbarUi;
  // Modal option picker over the panel (same component the Settings screens
  // use), for enum rows: font size / line spacing / alignment / orientation /
  // auto page turn. Toggle rows stay one-tap toggles, as in Settings.
  OptionPopup overlayPopup;
  // True while a clean-page snapshot (renderer.storeBwBuffer) backs the open
  // overlay, letting panel->toolbar steps restore the page without a full
  // re-render. Discarded on close / whenever the page under the overlay changes.
  bool overlayPageStored = false;
  int autoTurnOption = 0;  // current auto page-turn rate index (More panel)
  std::vector<EpubReaderMenuActivity::MenuItem> moreItems;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  std::vector<PageLink> currentPageLinks;
  int currentPageLinkMarginLeft = 0;
  int currentPageLinkMarginTop = 0;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  bool partialRebuildStartFailed = false;

  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  EpubReaderUtils::IdleWork idleWork(uint32_t nowMs) const;
  std::optional<uint32_t> nextWakeAt(uint32_t nowMs) const;
  bool hasInputActivity() const;
  void runBackgroundWork();
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  bool buildPopupPending = false;
  void showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh);
  bool applyDeferredReposition();
  void clearDeferredReposition();
  void rememberCurrentContentOffset();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void openReaderMenu();
  // Toolbar reader menu (see Overlay above).
  bool usesToolbarMenu() const;
  void openOverlay(Overlay target);
  void closeOverlayToPage();
  void discardOverlayPage();
  void handleOverlayInput();
  void renderOverlay();
  std::string currentChapterTitle() const;
  // Text panel rows (font, size, line spacing, alignment, focus reading).
  std::string textRowName(int row) const;
  std::string textRowValue(int row) const;
  void showTextRowPopup(int row);
  // Persist + re-paginate + re-render under the open panel (live preview).
  void applyTextSettingLive();
  void paintOverlayPopup();
  // Persist the reader text settings, (re)load the selected SD font, and
  // re-paginate the current chapter so changes apply without re-opening the book.
  void applyReaderTextSettings();
  // More panel rows.
  void buildMoreActions();
  std::string moreRowName(int row) const;
  std::string moreRowValue(int row) const;
  void activateMoreRow(int row);
  void openDictionaryWordSelect();
  void openFootnotes(bool followSingle = false);
  bool launchKOReaderSync();
  unsigned long confirmLongPressThreshold() const;
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void loadCachedBookmarks();
  void addBookmark(bool showMessage = false);
  void updateBookmarkFlag();

  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void applyOrientation(uint8_t orientation);
  void applyInitialOrientation() override;
  // The orientation the current layout was built for. The control center's
  // orientation tile can move SETTINGS.orientation while this reader sits on
  // the activity stack, and Pop restores it without onEnter(), so the drift has
  // to be noticed here rather than assumed away.
  uint8_t appliedOrientation = 0;

  bool loadBook() override;
  std::string getBookTitle() const override { return epub ? epub->getTitle() : ""; }
  std::string getBookAuthor() const override { return epub ? epub->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return epub ? epub->getThumbBmpPath() : ""; }
  void renderBook() override;
  void onEndOfBookRendered() override;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                              bool allowFastInitialRefresh);
  ~EpubReaderActivity() override;

  void loop() override;
  void onSuspend() override;
  void onResume() override;
  void onExit() override;
  bool onPrepareExit() override;
  void onRenderExit() override;
  void render(RenderLock&& lock) override;
  void requestUpdate(bool immediate = false) override;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  bool skipLoopDelay() override;

  ScreenshotInfo getScreenshotInfo() const override;
};
