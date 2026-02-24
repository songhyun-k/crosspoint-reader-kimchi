#include "FontSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "FontManager.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* CACHE_DIR = "/.crosspoint/cache";

// Recursively delete a directory and its contents
void deleteDirectory(const char* path) {
  FsFile dir = Storage.open(path);
  if (!dir || !dir.isDir()) {
    if (dir) dir.close();
    return;
  }

  FsFile entry;
  while (entry.openNext(&dir, O_RDONLY)) {
    char entryName[64];
    entry.getName(entryName, sizeof(entryName));
    entry.close();

    std::string fullPath = std::string(path) + "/" + entryName;
    FsFile check = Storage.open(fullPath.c_str());
    if (check) {
      bool isDir = check.isDir();
      check.close();
      if (isDir) {
        deleteDirectory(fullPath.c_str());
      } else {
        Storage.remove(fullPath.c_str());
      }
    }
  }
  dir.close();
  Storage.rmdir(path);
}

// Invalidate rendering caches for EPUB and TXT readers.
// Keeps progress.bin (reading position) but removes layout caches.
void invalidateReaderCaches() {
  LOG_INF("FNT", "Invalidating reader rendering caches...");

  FsFile cacheDir = Storage.open(CACHE_DIR);
  if (!cacheDir || !cacheDir.isDir()) {
    if (cacheDir) cacheDir.close();
    LOG_DBG("FNT", "No cache directory found");
    return;
  }

  int deletedCount = 0;
  FsFile bookCache;
  while (bookCache.openNext(&cacheDir, O_RDONLY)) {
    char bookCacheName[64];
    bookCache.getName(bookCacheName, sizeof(bookCacheName));
    bookCache.close();

    std::string bookCachePath = std::string(CACHE_DIR) + "/" + bookCacheName;

    // For EPUB: delete sections/ folder (keeps progress.bin)
    std::string sectionsPath = bookCachePath + "/sections";
    FsFile sectionsDir = Storage.open(sectionsPath.c_str());
    if (sectionsDir && sectionsDir.isDir()) {
      sectionsDir.close();
      deleteDirectory(sectionsPath.c_str());
      LOG_DBG("FNT", "Deleted EPUB sections cache: %s", sectionsPath.c_str());
      deletedCount++;
    } else {
      if (sectionsDir) sectionsDir.close();
    }

    // For TXT: delete index.bin (keeps progress.bin)
    std::string indexPath = bookCachePath + "/index.bin";
    if (Storage.exists(indexPath.c_str())) {
      Storage.remove(indexPath.c_str());
      LOG_DBG("FNT", "Deleted TXT index cache: %s", indexPath.c_str());
      deletedCount++;
    }
  }
  cacheDir.close();

  LOG_INF("FNT", "Invalidated %d cache entries", deletedCount);
}
}  // namespace

void FontSelectionActivity::scanFontsInDirectory(const char* dirPath) {
  FsFile dir = Storage.open(dirPath);
  if (!dir) {
    LOG_DBG("FNT", "Font folder %s not found", dirPath);
    return;
  }

  if (!dir.isDir()) {
    LOG_DBG("FNT", "%s is not a directory", dirPath);
    dir.close();
    return;
  }

  FsFile file;
  while (file.openNext(&dir, O_RDONLY)) {
    if (!file.isDir()) {
      char filename[64];
      file.getName(filename, sizeof(filename));

      // Check for .epdfont extension and skip macOS hidden files (._*)
      const size_t len = strlen(filename);
      if (len > 8 && strcasecmp(filename + len - 8, ".epdfont") == 0 && strncmp(filename, "._", 2) != 0) {
        std::string fullPath = std::string(dirPath) + "/" + filename;
        fontFiles.push_back(fullPath);

        // Extract name without extension for display
        std::string displayName(filename, len - 8);
        fontNames.push_back(displayName);

        LOG_DBG("FNT", "Found font: %s", fullPath.c_str());
      }
    }
    file.close();
  }
  dir.close();
}

void FontSelectionActivity::loadFontList() {
  fontFiles.clear();
  fontNames.clear();

  // First entry is always the default font (empty path)
  fontFiles.emplace_back("");
  fontNames.emplace_back(tr(STR_DEFAULT_FONT));

  Storage.mkdir("/.crosspoint");
  Storage.mkdir(FONTS_DIR);

  scanFontsInDirectory(FONTS_DIR);
  scanFontsInDirectory(ROOT_FONTS_DIR);

  LOG_INF("FNT", "Total fonts found: %zu (including default)", fontFiles.size());

  // Find currently selected font index
  selectedIndex = 0;
  if (SETTINGS.hasCustomFont()) {
    for (size_t i = 1; i < fontFiles.size(); i++) {
      if (fontFiles[i] == SETTINGS.customFontPath) {
        selectedIndex = static_cast<int>(i);
        break;
      }
    }
  }
}

void FontSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  loadFontList();
  requestUpdate();
}

void FontSelectionActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int itemCount = static_cast<int>(fontNames.size());
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + itemCount - 1) % itemCount;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % itemCount;
    requestUpdate();
  }
}

void FontSelectionActivity::handleSelection() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  // Capture old font ID before changing settings (needed for proper cleanup)
  const int oldFontId = SETTINGS.hasCustomFont() ? SETTINGS.getCustomFontId() : 0;

  // Show loading screen
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 - 10, tr(STR_APPLYING_FONT));
  renderer.displayBuffer();

  // Update custom font path in settings
  if (selectedIndex == 0) {
    SETTINGS.customFontPath[0] = '\0';
  } else {
    strncpy(SETTINGS.customFontPath, fontFiles[selectedIndex].c_str(), sizeof(SETTINGS.customFontPath) - 1);
    SETTINGS.customFontPath[sizeof(SETTINGS.customFontPath) - 1] = '\0';
  }

  SETTINGS.saveToFile();
  LOG_INF("FNT", "Font selected: %s", selectedIndex == 0 ? "default" : SETTINGS.customFontPath);

  // Reload custom font dynamically (no reboot needed)
  reloadCustomReaderFont(oldFontId);

  // Invalidate EPUB/TXT caches since font changed
  invalidateReaderCaches();

  xSemaphoreGive(renderingMutex);

  onBack();
}

void FontSelectionActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_FONT_SELECT), true);

  // Calculate visible items (with scrolling if needed)
  constexpr int lineHeight = 30;
  constexpr int startY = 60;
  const int maxVisibleItems = (pageHeight - startY - 50) / lineHeight;
  const int itemCount = static_cast<int>(fontNames.size());

  // Calculate scroll offset to keep selected item visible
  int scrollOffset = 0;
  if (itemCount > maxVisibleItems) {
    if (selectedIndex >= maxVisibleItems) {
      scrollOffset = selectedIndex - maxVisibleItems + 1;
    }
  }

  // Determine currently active font (for checkmark)
  int currentSelectedIndex = 0;
  if (SETTINGS.hasCustomFont()) {
    for (size_t i = 1; i < fontFiles.size(); i++) {
      if (fontFiles[i] == SETTINGS.customFontPath) {
        currentSelectedIndex = static_cast<int>(i);
        break;
      }
    }
  }

  // Draw font list
  for (int i = 0; i < maxVisibleItems && (i + scrollOffset) < itemCount; i++) {
    const int itemIndex = i + scrollOffset;
    const int itemY = startY + i * lineHeight;
    const bool isHighlighted = (itemIndex == selectedIndex);
    const bool isCurrentFont = (itemIndex == currentSelectedIndex);

    if (isHighlighted) {
      renderer.fillRect(0, itemY - 2, pageWidth - 1, lineHeight);
    }

    // Draw asterisk marker for currently active font
    if (isCurrentFont) {
      renderer.drawText(UI_10_FONT_ID, 10, itemY, "*", !isHighlighted);
    }

    renderer.drawText(UI_10_FONT_ID, 35, itemY, fontNames[itemIndex].c_str(), !isHighlighted);
  }

  // Draw scroll indicators
  if (scrollOffset > 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, startY - 15, "...", true);
  }
  if (scrollOffset + maxVisibleItems < itemCount) {
    renderer.drawCenteredText(UI_10_FONT_ID, startY + maxVisibleItems * lineHeight, "...", true);
  }

  // Draw button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
