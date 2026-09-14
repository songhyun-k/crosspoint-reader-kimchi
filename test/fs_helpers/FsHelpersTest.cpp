#include <FsHelpers.h>
#include <gtest/gtest.h>

TEST(FsHelpersTest, RemovesCurrentDirectoryComponents) {
  EXPECT_EQ(FsHelpers::normalisePath("./chapter.xhtml"), "chapter.xhtml");
  EXPECT_EQ(FsHelpers::normalisePath("OPS/./text/./chapter.xhtml"), "OPS/text/chapter.xhtml");
  EXPECT_EQ(FsHelpers::normalisePath("OPS/text/."), "OPS/text");
  EXPECT_EQ(FsHelpers::normalisePath("././."), "");
  EXPECT_EQ(FsHelpers::normalisePath("OPS/./text/../chapter.xhtml"), "OPS/chapter.xhtml");
  EXPECT_EQ(FsHelpers::normalisePath("./../chapter.xhtml"), "chapter.xhtml");
}

TEST(FsHelpersTest, PreservesOtherPathComponents) {
  EXPECT_EQ(FsHelpers::normalisePath("/OPS//text///chapter.xhtml/"), "OPS/text/chapter.xhtml");
  EXPECT_EQ(FsHelpers::normalisePath("../../OPS/chapter.xhtml"), "OPS/chapter.xhtml");
  EXPECT_EQ(FsHelpers::normalisePath(""), "");
  EXPECT_EQ(FsHelpers::normalisePath("./OPS/.hidden/../제1장...xhtml"), "OPS/제1장...xhtml");
  EXPECT_EQ(FsHelpers::normalisePath(".../.hidden.xhtml"), ".../.hidden.xhtml");
}
