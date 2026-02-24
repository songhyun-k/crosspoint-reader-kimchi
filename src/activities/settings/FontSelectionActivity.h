#pragma once
#include <functional>
#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"

/**
 * Activity for selecting a custom font from SD card.
 * Lists .epdfont files from /.crosspoint/fonts and /fonts directories.
 */
class FontSelectionActivity final : public ActivityWithSubactivity {
 public:
  explicit FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const std::function<void()>& onBack)
      : ActivityWithSubactivity("FontSelection", renderer, mappedInput), onBack(onBack) {}

  void onEnter() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  int selectedIndex = 0;
  std::vector<std::string> fontFiles;  // Full paths (empty string = default font)
  std::vector<std::string> fontNames;  // Display names
  const std::function<void()> onBack;

  void loadFontList();
  void handleSelection();
  void scanFontsInDirectory(const char* dirPath);

  static constexpr const char* FONTS_DIR = "/.crosspoint/fonts";
  static constexpr const char* ROOT_FONTS_DIR = "/fonts";
};
