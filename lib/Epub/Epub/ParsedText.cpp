#include "ParsedText.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return renderer.getSpaceWidth(fontId, style);
  }
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    return renderer.getTextAdvanceX(fontId, word.c_str(), style);
  }

  std::string sanitized = word;
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextAdvanceX(fontId, sanitized.c_str(), style);
}

// Splits a UTF-8 string into individual characters (code points).
std::vector<std::string> splitUtf8Chars(const std::string& str) {
  std::vector<std::string> chars;
  const char* p = str.c_str();
  while (*p) {
    int charLen = 1;
    const unsigned char c = static_cast<unsigned char>(*p);
    if ((c & 0xF8) == 0xF0) {
      charLen = 4;
    } else if ((c & 0xF0) == 0xE0) {
      charLen = 3;
    } else if ((c & 0xE0) == 0xC0) {
      charLen = 2;
    }
    chars.emplace_back(p, charLen);
    p += charLen;
  }
  return chars;
}

}  // namespace

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious) {
  if (word.empty()) return;

  words.push_back(std::move(word));
  EpdFontFamily::Style combinedStyle = fontStyle;
  if (underline) {
    combinedStyle = static_cast<EpdFontFamily::Style>(combinedStyle | EpdFontFamily::UNDERLINE);
  }
  wordStyles.push_back(combinedStyle);
  wordContinues.push_back(attachToPrevious);
}

// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine) {
  if (words.empty()) {
    return;
  }

  // Apply fixed transforms before any per-line layout work.
  applyParagraphIndent();

  const int pageWidth = viewportWidth;
  const int spaceWidth = renderer.getSpaceWidth(fontId);

  // Character wrap mode: greedy line filling with character-level splitting for CJK text
  if (characterWrap && blockStyle.alignment == CssTextAlign::Justify) {
    layoutCharacterWrap(renderer, fontId, viewportWidth, spaceWidth, processLine, includeLastLine);
    return;
  }

  auto wordWidths = calculateWordWidths(renderer, fontId);

  std::vector<size_t> lineBreakIndices;
  if (hyphenationEnabled) {
    // Use greedy layout that can split words mid-loop when a hyphenated prefix fits.
    lineBreakIndices = computeHyphenatedLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths, wordContinues);
  } else {
    lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths, wordContinues);
  }
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, spaceWidth, wordWidths, wordContinues, lineBreakIndices, processLine);
  }

  // Remove consumed words so size() reflects only remaining words
  if (lineCount > 0) {
    const size_t consumed = lineBreakIndices[lineCount - 1];
    words.erase(words.begin(), words.begin() + consumed);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
    wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
  }
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    wordWidths.push_back(measureWordWidth(renderer, fontId, words[i], wordStyles[i]));
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  const int spaceWidth, std::vector<uint16_t>& wordWidths,
                                                  std::vector<bool>& continuesVec) {
  if (words.empty()) {
    return {};
  }

  // Calculate first line indent (only for left/justified text without extra paragraph spacing)
  const int firstLineIndent =
      blockStyle.textIndent > 0 && !extraParagraphSpacing &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  // Ensure any word that would overflow even as the first entry on a line is split using fallback hyphenation.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;
    while (wordWidths[i] > effectiveWidth) {
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true)) {
        break;
      }
    }
  }

  const size_t totalWordCount = words.size();

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = MAX_COST;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Add space before word j, unless it's the first word on the line or a continuation
      const int gap = j > static_cast<size_t>(i) && !continuesVec[j] ? spaceWidth : 0;
      currlen += wordWidths[j] + gap;

      if (currlen > effectivePageWidth) {
        break;
      }

      // Cannot break after word j if the next word attaches to it (continuation group)
      if (j + 1 < totalWordCount && continuesVec[j + 1]) {
        continue;
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line
      } else {
        const int remainingSpace = effectivePageWidth - currlen;
        // Use long long for the square to prevent overflow
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      if (cost < dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}

// Character-wrap mode: greedy line filling with justified alignment (1.0x–1.5x spacing).
// If spacing would exceed 1.5x, splits words at character boundaries to fill the line.
// Adapted for upstream's std::vector storage and getTextAdvanceX()-based measurement.
void ParsedText::layoutCharacterWrap(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                     const int spaceWidth,
                                     const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                     const bool includeLastLine) {
  const int pageWidth = viewportWidth;
  const int minSpacing = spaceWidth;
  const int maxSpacing = spaceWidth + (spaceWidth / 2);  // 1.5x

  size_t wordIdx = 0;  // Current position in words vector

  while (wordIdx < words.size()) {
    std::vector<std::string> lineWordsVec;
    std::vector<int> lineWordWidths;
    std::vector<EpdFontFamily::Style> lineWordStylesVec;
    int totalWordWidth = 0;

    // Phase 1: Greedily collect words to fill the line with spacing in [1.0x, 1.5x]
    while (wordIdx < words.size()) {
      const std::string& word = words[wordIdx];
      const EpdFontFamily::Style wordStyle = wordStyles[wordIdx];
      const int wordWidth = measureWordWidth(renderer, fontId, word, wordStyle);

      const int newTotalWidth = totalWordWidth + wordWidth;
      const int newGapCount = static_cast<int>(lineWordsVec.size());
      const int newSpareSpace = pageWidth - newTotalWidth;
      const int newSpacing = (newGapCount > 0) ? (newSpareSpace / newGapCount) : maxSpacing + 1;

      if (lineWordsVec.empty()) {
        // First word on line — must add something
        if (wordWidth <= pageWidth) {
          lineWordsVec.push_back(word);
          lineWordWidths.push_back(wordWidth);
          lineWordStylesVec.push_back(wordStyle);
          totalWordWidth = wordWidth;
          wordIdx++;
        } else {
          // Word too long for a single line — split at character boundaries
          auto chars = splitUtf8Chars(word);
          std::string partial;
          size_t charsFit = 0;
          for (size_t i = 0; i < chars.size(); i++) {
            std::string test = partial + chars[i];
            const int testWidth = measureWordWidth(renderer, fontId, test, wordStyle);
            if (testWidth > pageWidth) break;
            partial = test;
            charsFit = i + 1;
          }
          if (charsFit == 0) {
            charsFit = 1;
            partial = chars[0];
          }
          const int partialWidth = measureWordWidth(renderer, fontId, partial, wordStyle);
          lineWordsVec.push_back(partial);
          lineWordWidths.push_back(partialWidth);
          lineWordStylesVec.push_back(wordStyle);
          totalWordWidth = partialWidth;

          if (charsFit < chars.size()) {
            std::string remainder;
            for (size_t i = charsFit; i < chars.size(); i++) remainder += chars[i];
            words[wordIdx] = remainder;
          } else {
            wordIdx++;
          }
        }
      } else if (newSpacing >= minSpacing) {
        // Adding this word keeps spacing >= minSpacing
        lineWordsVec.push_back(word);
        lineWordWidths.push_back(wordWidth);
        lineWordStylesVec.push_back(wordStyle);
        totalWordWidth = newTotalWidth;
        wordIdx++;

        if (newSpacing <= maxSpacing) {
          continue;  // Within ideal range, try to fit more
        }
      } else {
        // Adding whole word would make spacing < minSpacing — try partial characters
        const int currentGapCount = static_cast<int>(lineWordsVec.size());
        const int maxPartialWidth = pageWidth - totalWordWidth - currentGapCount * minSpacing;

        if (maxPartialWidth > 0) {
          auto chars = splitUtf8Chars(word);
          std::string partial;
          size_t charsFit = 0;
          for (size_t i = 0; i < chars.size(); i++) {
            std::string test = partial + chars[i];
            const int testWidth = measureWordWidth(renderer, fontId, test, wordStyle);
            if (testWidth > maxPartialWidth) break;
            partial = test;
            charsFit = i + 1;
          }

          if (charsFit > 0) {
            const int partialWidth = measureWordWidth(renderer, fontId, partial, wordStyle);
            lineWordsVec.push_back(partial);
            lineWordWidths.push_back(partialWidth);
            lineWordStylesVec.push_back(wordStyle);
            totalWordWidth += partialWidth;

            if (charsFit < chars.size()) {
              std::string remainder;
              for (size_t i = charsFit; i < chars.size(); i++) remainder += chars[i];
              words[wordIdx] = remainder;
            } else {
              wordIdx++;
            }
          }
        }
        break;  // Line is full
      }
    }

    // Phase 2: If spacing is still too large, fill with more characters from next word
    while (wordIdx < words.size() && !lineWordsVec.empty()) {
      const int gapCount = static_cast<int>(lineWordsVec.size());
      const int spareSpace = pageWidth - totalWordWidth;
      const int spacing = (gapCount > 0) ? (spareSpace / gapCount) : 0;

      if (spacing <= maxSpacing) break;  // Spacing is acceptable

      const std::string& nextWord = words[wordIdx];
      const EpdFontFamily::Style nextStyle = wordStyles[wordIdx];
      auto chars = splitUtf8Chars(nextWord);

      const int maxPartialWidth = pageWidth - totalWordWidth - gapCount * minSpacing;
      if (maxPartialWidth <= 0) break;

      std::string partial;
      int partialWidth = 0;
      size_t charsFit = 0;
      for (size_t i = 0; i < chars.size(); i++) {
        std::string test = partial + chars[i];
        const int testWidth = measureWordWidth(renderer, fontId, test, nextStyle);
        if (testWidth > maxPartialWidth) break;
        partial = test;
        partialWidth = testWidth;
        charsFit = i + 1;
      }

      if (charsFit == 0) break;

      lineWordsVec.push_back(partial);
      lineWordWidths.push_back(partialWidth);
      lineWordStylesVec.push_back(nextStyle);
      totalWordWidth += partialWidth;

      if (charsFit < chars.size()) {
        std::string remainder;
        for (size_t i = charsFit; i < chars.size(); i++) remainder += chars[i];
        words[wordIdx] = remainder;
      } else {
        wordIdx++;
      }
    }

    // Phase 3: Calculate final justified positions
    const bool isLastLine = wordIdx >= words.size();
    const int gapCount = static_cast<int>(lineWordsVec.size()) - 1;
    const int spareSpace = pageWidth - totalWordWidth;

    std::vector<std::string> lineWords;
    std::vector<uint16_t> lineXPos;
    std::vector<EpdFontFamily::Style> lineWordStyles;

    if (isLastLine || gapCount <= 0) {
      // Last line or single word: left-align with normal spacing
      int xpos = 0;
      for (size_t i = 0; i < lineWordsVec.size(); i++) {
        lineXPos.push_back(static_cast<uint16_t>(xpos));
        lineWords.push_back(std::move(lineWordsVec[i]));
        lineWordStyles.push_back(lineWordStylesVec[i]);
        xpos += lineWordWidths[i] + minSpacing;
      }
    } else {
      // Justified: distribute spare space evenly across gaps
      const int baseSpacing = spareSpace / gapCount;
      const int extraPixels = spareSpace % gapCount;

      int xpos = 0;
      for (size_t i = 0; i < lineWordsVec.size(); i++) {
        lineXPos.push_back(static_cast<uint16_t>(xpos));
        lineWords.push_back(std::move(lineWordsVec[i]));
        lineWordStyles.push_back(lineWordStylesVec[i]);

        if (i < lineWordsVec.size() - 1) {
          const int gap = baseSpacing + (static_cast<int>(i) < extraPixels ? 1 : 0);
          xpos += lineWordWidths[i] + gap;
        }
      }
    }

    if (!lineWords.empty() && (!isLastLine || includeLastLine)) {
      // Strip soft hyphens from output words
      for (auto& w : lineWords) {
        if (containsSoftHyphen(w)) {
          stripSoftHyphensInPlace(w);
        }
      }

      BlockStyle lineBlockStyle = blockStyle;
      lineBlockStyle.alignment = isLastLine ? CssTextAlign::Left : CssTextAlign::Justify;
      processLine(std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles),
                                              lineBlockStyle));
    }
  }

  // All words consumed — clear vectors
  words.clear();
  wordStyles.clear();
  wordContinues.clear();
}

void ParsedText::applyParagraphIndent() {
  if (!paragraphIndent || paragraphIndentApplied || words.empty()) {
    return;
  }
  paragraphIndentApplied = true;

  if (blockStyle.textIndentDefined) {
    // CSS text-indent is explicitly set (even if 0) - don't use fallback indent
    // The actual indent positioning is handled in extractLine()
  } else if (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left) {
    // No CSS text-indent defined - use ideographic space (U+3000)
    words.front().insert(0, "\xe3\x80\x80");
  }
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int pageWidth, const int spaceWidth,
                                                            std::vector<uint16_t>& wordWidths,
                                                            std::vector<bool>& continuesVec) {
  // Calculate first line indent (only for left/justified text without extra paragraph spacing)
  const int firstLineIndent =
      blockStyle.textIndent > 0 && !extraParagraphSpacing &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;
  bool isFirstLine = true;

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = isFirstLine ? pageWidth - firstLineIndent : pageWidth;

    // Consume as many words as possible for current line, splitting when prefixes fit
    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      const int spacing = isFirstWord || continuesVec[currentIndex] ? 0 : spaceWidth;
      const int candidateWidth = spacing + wordWidths[currentIndex];

      // Word fits on current line
      if (lineWidth + candidateWidth <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      // Word would overflow — try to split based on hyphenation points
      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;  // Only for first word on line

      if (availableWidth > 0 &&
          hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths, allowFallbackBreaks)) {
        // Prefix now fits; append it to this line and move to next line
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      // Could not split: force at least one word per line to avoid infinite loop
      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    // Don't break before a continuation word (e.g., orphaned "?" after "question").
    // Backtrack to the start of the continuation group so the whole group moves to the next line.
    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() && continuesVec[currentIndex]) {
      --currentIndex;
    }

    lineBreakIndices.push_back(currentIndex);
    isFirstLine = false;
  }

  return lineBreakIndices;
}

// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  const std::string& word = words[wordIndex];
  const auto style = wordStyles[wordIndex];

  // Collect candidate breakpoints (byte offsets and hyphen requirements).
  auto breakInfos = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const int prefixWidth = measureWordWidth(renderer, fontId, word.substr(0, offset), style, needsHyphen);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    // No hyphenation point produced a prefix that fits in the remaining space.
    return false;
  }

  // Split the word at the selected breakpoint and append a hyphen if required.
  std::string remainder = word.substr(chosenOffset);
  words[wordIndex].resize(chosenOffset);
  if (chosenNeedsHyphen) {
    words[wordIndex].push_back('-');
  }

  // Insert the remainder word (with matching style and continuation flag) directly after the prefix.
  words.insert(words.begin() + wordIndex + 1, remainder);
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, style);

  // Continuation flag handling after splitting a word into prefix + remainder.
  //
  // The prefix keeps the original word's continuation flag so that no-break-space groups
  // stay linked. The remainder always gets continues=false because it starts on the next
  // line and is not attached to the prefix.
  //
  // Example: "200&#xA0;Quadratkilometer" produces tokens:
  //   [0] "200"               continues=false
  //   [1] " "                 continues=true
  //   [2] "Quadratkilometer"  continues=true   <-- the word being split
  //
  // After splitting "Quadratkilometer" at "Quadrat-" / "kilometer":
  //   [0] "200"         continues=false
  //   [1] " "           continues=true
  //   [2] "Quadrat-"    continues=true   (KEPT — still attached to the no-break group)
  //   [3] "kilometer"   continues=false  (NEW — starts fresh on the next line)
  //
  // This lets the backtracking loop keep the entire prefix group ("200 Quadrat-") on one
  // line, while "kilometer" moves to the next line.
  // wordContinues[wordIndex] is intentionally left unchanged — the prefix keeps its original attachment.
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = measureWordWidth(renderer, fontId, remainder, style);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const int spaceWidth,
                             const std::vector<uint16_t>& wordWidths, const std::vector<bool>& continuesVec,
                             const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  // Calculate first line indent (only for left/justified text without extra paragraph spacing)
  const bool isFirstLine = breakIndex == 0;
  const int firstLineIndent =
      isFirstLine && blockStyle.textIndent > 0 && !extraParagraphSpacing &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  // Calculate total word width for this line and count actual word gaps
  // (continuation words attach to previous word with no gap)
  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    // Count gaps: each word after the first creates a gap, unless it's a continuation
    if (wordIdx > 0 && !continuesVec[lastBreakAt + wordIdx]) {
      actualGapCount++;
    }
  }

  // Calculate spacing (account for indent reducing effective page width on first line)
  const int effectivePageWidth = pageWidth - firstLineIndent;
  const int spareSpace = effectivePageWidth - lineWordWidthSum;

  int spacing = spaceWidth;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  // For justified text, calculate spacing based on actual gap count
  if (blockStyle.alignment == CssTextAlign::Justify && !isLastLine && actualGapCount >= 1) {
    spacing = spareSpace / static_cast<int>(actualGapCount);
  }

  // Calculate initial x position (first line starts at indent for left/justified text)
  auto xpos = static_cast<uint16_t>(firstLineIndent);
  if (blockStyle.alignment == CssTextAlign::Right) {
    xpos = spareSpace - static_cast<int>(actualGapCount) * spaceWidth;
  } else if (blockStyle.alignment == CssTextAlign::Center) {
    xpos = (spareSpace - static_cast<int>(actualGapCount) * spaceWidth) / 2;
  }

  // Pre-calculate X positions for words
  // Continuation words attach to the previous word with no space before them
  std::vector<uint16_t> lineXPos;
  lineXPos.reserve(lineWordCount);

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    const uint16_t currentWordWidth = wordWidths[lastBreakAt + wordIdx];

    lineXPos.push_back(xpos);

    // Add spacing after this word, unless the next word is a continuation
    const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];

    xpos += currentWordWidth + (nextIsContinuation ? 0 : spacing);
  }

  // Build line data by moving from the original vectors using index range
  std::vector<std::string> lineWords(std::make_move_iterator(words.begin() + lastBreakAt),
                                     std::make_move_iterator(words.begin() + lineBreak));
  std::vector<EpdFontFamily::Style> lineWordStyles(wordStyles.begin() + lastBreakAt, wordStyles.begin() + lineBreak);

  for (auto& word : lineWords) {
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
  }

  processLine(
      std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), blockStyle));
}
