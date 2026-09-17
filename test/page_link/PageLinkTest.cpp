#include <Epub/Epub/PageLink.h>
#include <gtest/gtest.h>

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
