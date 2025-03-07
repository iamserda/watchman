/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/test/lib/FakeWatcher.h"
#include "watchman/fs/FileSystem.h"

namespace watchman {

FakeWatcher::FakeWatcher(FileSystem& fileSystem)
    : Watcher{"FakeWatcher", 0}, fileSystem_{fileSystem} {}

std::unique_ptr<DirHandle> FakeWatcher::startWatchDir(
    const std::shared_ptr<Root>& root,
    struct watchman_dir* dir,
    const char* path) {
  (void)root;
  (void)dir;
  return fileSystem_.openDir(path);
}

bool FakeWatcher::waitNotify(int timeoutms) {
  (void)timeoutms;
  throw std::logic_error{"waitNotify not implemented"};
}

Watcher::ConsumeNotifyRet FakeWatcher::consumeNotify(
    const std::shared_ptr<Root>& root,
    PendingChanges& coll) {
  (void)root;
  (void)coll;
  throw std::logic_error{"consumeNotify not implemented"};
}

} // namespace watchman
