/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/QueryableView.h"
#include "watchman/Errors.h"

namespace watchman {

QueryableView::~QueryableView() = default;

/** Perform a time-based (since) query and emit results to the supplied
 * query context */
void QueryableView::timeGenerator(const Query*, QueryContext*) const {
  throw QueryExecError("timeGenerator not implemented");
}

/** Walks files that match the supplied set of paths */
void QueryableView::pathGenerator(const Query*, QueryContext*) const {
  throw QueryExecError("pathGenerator not implemented");
}

void QueryableView::globGenerator(const Query*, QueryContext*) const {
  throw QueryExecError("globGenerator not implemented");
}

void QueryableView::allFilesGenerator(const Query*, QueryContext*) const {
  throw QueryExecError("allFilesGenerator not implemented");
}

uint32_t QueryableView::getLastAgeOutTickValue() const {
  return 0;
}

std::chrono::system_clock::time_point QueryableView::getLastAgeOutTimeStamp()
    const {
  return std::chrono::system_clock::time_point{};
}

void QueryableView::ageOut(PerfSample&, std::chrono::seconds) {}

bool QueryableView::isVCSOperationInProgress() const {
  static const std::vector<w_string> lockFiles{".hg/wlock", ".git/index.lock"};
  return doAnyOfTheseFilesExist(lockFiles);
}

} // namespace watchman
