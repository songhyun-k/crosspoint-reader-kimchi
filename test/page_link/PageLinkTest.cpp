#include <Epub/Epub/PageLink.h>
#include <gtest/gtest.h>

#include <array>
#include <new>

#include "activities/reader/EpubReaderUtils.h"

TEST(PageLinkTest, HitAreaIncludesFingerSlopAndMinimumWidth) {
  PageLink link;
  link.x = 100;
  link.y = 50;
  link.width = 8;
  link.height = 20;

  EXPECT_TRUE(link.contains(90, 44, 6, 28));
  EXPECT_TRUE(link.contains(117, 75, 6, 28));
  EXPECT_FALSE(link.contains(89, 50, 6, 28));
  EXPECT_FALSE(link.contains(118, 50, 6, 28));
  EXPECT_FALSE(link.contains(100, 76, 6, 28));

  const std::vector<PageLink> links{link, link};
  EXPECT_EQ(EpubReaderUtils::linkAtPoint(links, 95, 51, 5, 7), &links.front());
  EXPECT_EQ(EpubReaderUtils::linkAtPoint(links, 123, 57, 5, 7), nullptr);
  EXPECT_EQ(EpubReaderUtils::linkAtPoint({}, 95, 51, 5, 7), nullptr);
}

TEST(PageLinkTest, InitializesTheWholeSerializedHref) {
  alignas(PageLink) std::array<unsigned char, sizeof(PageLink)> storage;
  storage.fill(0x55);
  const auto* link = new (storage.data()) PageLink;
  EXPECT_TRUE(std::all_of(std::begin(link->href), std::end(link->href), [](char byte) { return byte == 0; }));
  link->~PageLink();
}
