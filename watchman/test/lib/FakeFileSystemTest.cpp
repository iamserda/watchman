/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/test/lib/FakeFileSystem.h"
#include <folly/portability/GTest.h>

namespace {

using namespace ::testing;
using namespace watchman;

TEST(FakeFileSystemTest, get_root) {
  FakeFileSystem fs;
  auto fi = fs.getFileInformation("/");
  EXPECT_TRUE(fi.isDir());
}

TEST(FakeFileSystemTest, defineContents_populates_files_and_directories) {
  FakeFileSystem fs;
  fs.defineContents({
      "/fake/root/empty/",
      "/fake/root/.watchmanconfig",
      "/fake/root/dir/file.txt",
  });

  EXPECT_EQ(DType::Dir, fs.getFileInformation("/fake/root/empty").dtype());
  EXPECT_EQ(
      DType::Regular,
      fs.getFileInformation("/fake/root/.watchmanconfig").dtype());
  EXPECT_EQ(
      DType::Regular, fs.getFileInformation("/fake/root/dir/file.txt").dtype());
}

TEST(FakeFileSystemTest, openDir_enumerates_entries_without_stat) {
  FakeFileSystem fs;
  fs.defineContents({
      "/fake/a",
      "/fake/b",
      "/fake/c/",
  });

  auto handle = fs.openDir("/fake");
  auto* entry = handle->readDir();
  EXPECT_NE(nullptr, entry);

  EXPECT_FALSE(entry->has_stat);
  EXPECT_STREQ("a", entry->d_name);

  entry = handle->readDir();
  EXPECT_NE(nullptr, entry);
  EXPECT_FALSE(entry->has_stat);
  EXPECT_STREQ("b", entry->d_name);

  entry = handle->readDir();
  EXPECT_NE(nullptr, entry);
  EXPECT_FALSE(entry->has_stat);
  EXPECT_STREQ("c", entry->d_name);

  EXPECT_EQ(nullptr, handle->readDir());
}

TEST(FakeFileSystemTest, openDir_enumerates_entries_with_stat) {
  FakeFileSystem::Flags flags;
  flags.includeReadDirStat = true;
  FakeFileSystem fs{flags};
  fs.defineContents({
      "/fake/a",
      "/fake/b",
      "/fake/c/",
  });

  auto handle = fs.openDir("/fake");
  auto* entry = handle->readDir();
  EXPECT_NE(nullptr, entry);

  EXPECT_TRUE(entry->has_stat);
  EXPECT_STREQ("a", entry->d_name);
  EXPECT_FALSE(entry->stat.isDir());

  entry = handle->readDir();
  EXPECT_NE(nullptr, entry);
  EXPECT_TRUE(entry->has_stat);
  EXPECT_STREQ("b", entry->d_name);
  EXPECT_FALSE(entry->stat.isDir());

  entry = handle->readDir();
  EXPECT_NE(nullptr, entry);
  EXPECT_TRUE(entry->has_stat);
  EXPECT_STREQ("c", entry->d_name);
  EXPECT_TRUE(entry->stat.isDir());

  EXPECT_EQ(nullptr, handle->readDir());
}

} // namespace
