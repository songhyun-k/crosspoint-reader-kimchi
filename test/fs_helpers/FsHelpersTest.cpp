#include <FsHelpers.h>
#include <gtest/gtest.h>

TEST(FsHelpersTest, RemovesCurrentDirectoryComponents) {
  EXPECT_EQ(FsHelpers::normalisePath("./chapter.xhtml"), "chapter.xhtml");
  EXPECT_EQ(FsHelpers::normalisePath("OPS/./text/./chapter.xhtml"), "OPS/text/chapter.xhtml");
  EXPECT_EQ(FsHelpers::normalisePath("OPS/text/."), "OPS/text");
  EXPECT_EQ(FsHelpers::normalisePath("././."), "");
}

TEST(FsHelpersTest, CombinesDotAndParentComponents) {
  EXPECT_EQ(FsHelpers::normalisePath("OPS/./text/../chapter.xhtml"), "OPS/chapter.xhtml");
  EXPECT_EQ(FsHelpers::normalisePath("OPS/text/./.././../cover.jpg"), "cover.jpg");
  EXPECT_EQ(FsHelpers::normalisePath("./../chapter.xhtml"), "chapter.xhtml");
}

TEST(FsHelpersTest, PreservesArchiveRootAndSeparatorSemantics) {
  EXPECT_EQ(FsHelpers::normalisePath("/OPS//text///chapter.xhtml/"), "OPS/text/chapter.xhtml");
  EXPECT_EQ(FsHelpers::normalisePath("../../OPS/chapter.xhtml"), "OPS/chapter.xhtml");
  EXPECT_EQ(FsHelpers::normalisePath("/.././"), "");
  EXPECT_EQ(FsHelpers::normalisePath("/"), "");
  EXPECT_EQ(FsHelpers::normalisePath(""), "");
}

TEST(FsHelpersTest, LeavesLiteralDotsAndUnicodeInNames) {
  EXPECT_EQ(FsHelpers::normalisePath("./OPS/.hidden/../제1장...xhtml"), "OPS/제1장...xhtml");
  EXPECT_EQ(FsHelpers::normalisePath(".../.hidden.xhtml"), ".../.hidden.xhtml");
}

TEST(FsHelpersTest, NormalizationIsIdempotent) {
  const std::string path = FsHelpers::normalisePath("./OPS/./text/.././제1장.xhtml");
  EXPECT_EQ(FsHelpers::normalisePath(path), path);
}

TEST(FsHelpersTest, KeepsEscapedHashInFilenameSeparateFromAnchor) {
  // EPUB callers split the fragment before decoding and normalising the path.
  const std::string href = "OPS/./chapter%23one.xhtml#section%2F.%2Fone";
  const size_t hash = href.find('#');
  EXPECT_EQ(FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(href.substr(0, hash))), "OPS/chapter#one.xhtml");
  EXPECT_EQ(FsHelpers::decodeUriEscapes(href.substr(hash + 1)), "section/./one");
}
