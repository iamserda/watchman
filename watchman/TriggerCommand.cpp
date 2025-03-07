/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/TriggerCommand.h"
#include <folly/String.h>
#include "watchman/Errors.h"
#include "watchman/PDU.h"
#include "watchman/QueryableView.h"
#include "watchman/Shutdown.h"
#include "watchman/UserDir.h"
#include "watchman/query/Query.h"
#include "watchman/query/eval.h"
#include "watchman/query/parse.h"
#include "watchman/root/Root.h"
#include "watchman/sockname.h"
#include "watchman/watchman_stream.h"

namespace watchman {

namespace {

void parse_redirection(
    json_ref trig,
    std::string& name,
    int* flags,
    const char* label) {
  *flags = 0;

  auto ele = trig.get_default(label);
  if (!ele) {
    // Specifying a redirection is optional
    return;
  }

  if (!ele.isString()) {
    throw CommandValidationError(label, " must be a string");
  }

  name = json_string_value(ele);
  if (name.empty() || name[0] != '>') {
    throw CommandValidationError(
        label, ": must be prefixed with either > or >>, got ", name);
  }

  *flags = O_CREAT | O_WRONLY;

  if (name[1] == '>') {
#ifdef _WIN32
    throw CommandValidationError(label, ": Windows does not support O_APPEND");
#else
    *flags |= O_APPEND;
    name.erase(0, 2);
#endif
  } else {
    *flags |= O_TRUNC;
    name.erase(0, 1);
  }
}

std::unique_ptr<watchman_stream> prepare_stdin(
    struct TriggerCommand* cmd,
    QueryResult* res) {
  char stdin_file_name[WATCHMAN_NAME_MAX];

  if (cmd->stdin_style == trigger_input_style::input_dev_null) {
    return w_stm_open("/dev/null", O_RDONLY | O_CLOEXEC);
  }

  // Adjust result to fit within the specified limit
  if (cmd->max_files_stdin > 0) {
    auto& fileList = res->resultsArray.array();
    auto n_files = std::min(size_t(cmd->max_files_stdin), fileList.size());
    fileList.resize(std::min(fileList.size(), n_files));
  }

  /* prepare the input stream for the child process */
  snprintf(
      stdin_file_name,
      sizeof(stdin_file_name),
      "%s/wmanXXXXXX",
      getTemporaryDirectory().c_str());
  auto stdin_file = w_mkstemp(stdin_file_name);
  if (!stdin_file) {
    logf(
        ERR,
        "unable to create a temporary file: {} {}\n",
        stdin_file_name,
        folly::errnoStr(errno));
    return NULL;
  }

  /* unlink the file, we don't need it in the filesystem;
   * we'll pass the fd on to the child as stdin */
  unlink(stdin_file_name); // FIXME: windows path translation

  switch (cmd->stdin_style) {
    case input_json: {
      w_jbuffer_t buffer;

      logf(DBG, "input_json: sending json object to stm\n");
      if (!buffer.jsonEncodeToStream(res->resultsArray, stdin_file.get(), 0)) {
        logf(
            ERR,
            "input_json: failed to write json data to stream: {}\n",
            folly::errnoStr(errno));
        return NULL;
      }
      break;
    }
    case input_name_list:
      for (auto& name : res->resultsArray.array()) {
        auto& nameStr = json_to_w_string(name);
        if (stdin_file->write(nameStr.data(), nameStr.size()) !=
                (int)nameStr.size() ||
            stdin_file->write("\n", 1) != 1) {
          logf(
              ERR,
              "write failure while producing trigger stdin: {}\n",
              folly::errnoStr(errno));
          return nullptr;
        }
      }
      break;
    case input_dev_null:
      // already handled above
      break;
  }

  stdin_file->rewind();
  return stdin_file;
}

void spawn_command(
    const std::shared_ptr<Root>& root,
    struct TriggerCommand* cmd,
    QueryResult* res,
    ClockSpec* since_spec) {
  bool file_overflow = false;

  size_t arg_max = ChildProcess::getArgMax();

  size_t argspace_remaining = arg_max;

  // Allow some misc working overhead
  argspace_remaining -= 32;

  // Record an overflow before we call prepare_stdin(), which mutates
  // and resizes the results to fit the specified limit.
  if (cmd->max_files_stdin > 0 &&
      res->resultsArray.array().size() > cmd->max_files_stdin) {
    file_overflow = true;
  }

  auto stdin_file = prepare_stdin(cmd, res);
  if (!stdin_file) {
    logf(
        ERR,
        "trigger {}:{} {}\n",
        root->root_path,
        cmd->triggername,
        folly::errnoStr(errno));
    return;
  }

  // Assumption: that only one thread will be executing on a given
  // cmd instance so that mutation of cmd->env is safe.
  // This is guaranteed in the current architecture.

  // It is way too much of a hassle to try to recreate the clock value if it's
  // not a relative clock spec, and it's only going to happen on the first run
  // anyway, so just skip doing that entirely.
  if (since_spec && since_spec->tag == w_cs_clock) {
    cmd->env.set("WATCHMAN_SINCE", since_spec->clock.position.toClockString());
  } else {
    cmd->env.unset("WATCHMAN_SINCE");
  }

  cmd->env.set(
      "WATCHMAN_CLOCK", res->clockAtStartOfQuery.position().toClockString());

  if (cmd->query->relative_root) {
    cmd->env.set("WATCHMAN_RELATIVE_ROOT", cmd->query->relative_root);
  } else {
    cmd->env.unset("WATCHMAN_RELATIVE_ROOT");
  }

  // Compute args
  auto args = json_deep_copy(cmd->command);

  if (cmd->append_files) {
    // Measure how much space the base args take up
    for (size_t i = 0; i < json_array_size(args); i++) {
      const char* ele = json_string_value(json_array_get(args, i));

      argspace_remaining -= strlen(ele) + 1 + sizeof(char*);
    }

    // Dry run with env to compute space
    size_t env_size;
    cmd->env.asEnviron(&env_size);
    argspace_remaining -= env_size;

    for (const auto& item : res->dedupedFileNames) {
      // also: NUL terminator and entry in argv
      uint32_t size = item.size() + 1 + sizeof(char*);

      if (argspace_remaining < size) {
        file_overflow = true;
        break;
      }
      argspace_remaining -= size;

      json_array_append_new(args, w_string_to_json(item));
    }
  }

  cmd->env.setBool("WATCHMAN_FILES_OVERFLOW", file_overflow);

  ChildProcess::Options opts;
  opts.environment() = cmd->env;
#ifndef _WIN32
  sigset_t mask;
  sigemptyset(&mask);
  opts.setSigMask(mask);
#endif
  opts.setFlags(POSIX_SPAWN_SETPGROUP);

  opts.dup2(stdin_file->getFileDescriptor(), STDIN_FILENO);

  if (!cmd->stdout_name.empty()) {
    opts.open(STDOUT_FILENO, cmd->stdout_name.c_str(), cmd->stdout_flags, 0666);
  } else {
    opts.dup2(FileDescriptor::stdOut(), STDOUT_FILENO);
  }

  if (!cmd->stderr_name.empty()) {
    opts.open(STDERR_FILENO, cmd->stderr_name.c_str(), cmd->stderr_flags, 0666);
  } else {
    opts.dup2(FileDescriptor::stdErr(), STDERR_FILENO);
  }

  // Figure out the appropriate cwd
  w_string working_dir(cmd->query->relative_root);
  if (!working_dir) {
    working_dir = root->root_path;
  }

  auto cwd = cmd->definition.get_default("chdir");
  if (cwd) {
    auto target = json_to_w_string(cwd);
    if (w_is_path_absolute_cstr_len(target.data(), target.size())) {
      working_dir = target;
    } else {
      working_dir = w_string::pathCat({working_dir, target});
    }
  }

  watchman::log(watchman::DBG, "using ", working_dir, " for working dir\n");
  opts.chdir(working_dir.c_str());

  try {
    if (cmd->current_proc) {
      cmd->current_proc->kill();
      cmd->current_proc->wait();
    }
    cmd->current_proc = std::make_unique<ChildProcess>(args, std::move(opts));
  } catch (const std::exception& exc) {
    watchman::log(
        watchman::ERR,
        "trigger ",
        root->root_path,
        ":",
        cmd->triggername,
        " failed: ",
        exc.what(),
        "\n");
  }

  // We have integration tests that check for this string
  watchman::log(
      cmd->current_proc ? watchman::DBG : watchman::ERR,
      "posix_spawnp: ",
      cmd->triggername,
      "\n");
}

} // namespace

TriggerCommand::TriggerCommand(
    SavedStateFactory savedStateFactory,
    const std::shared_ptr<Root>& root,
    const json_ref& trig)
    : definition(trig),
      append_files(false),
      stdin_style(input_dev_null),
      max_files_stdin(0),
      stdout_flags(0),
      stderr_flags(0),
      savedStateFactory_{savedStateFactory},
      ping_(w_event_make_sockets()) {
  auto queryDef = json_object();
  auto expr = definition.get_default("expression");
  if (expr) {
    queryDef.set("expression", json_ref(expr));
  }
  auto relative_root = definition.get_default("relative_root");
  if (relative_root) {
    json_object_set_nocheck(queryDef, "relative_root", relative_root);
  }

  query = w_query_parse(root, queryDef);
  if (!query) {
    return;
  }

  auto name = trig.get_default("name");
  if (!name || !name.isString()) {
    throw CommandValidationError("invalid or missing name");
  }
  triggername = json_to_w_string(name);

  command = definition.get_default("command");
  if (!command || !command.isArray() || !json_array_size(command)) {
    throw CommandValidationError("invalid command array");
  }

  append_files = trig.get_default("append_files", json_false()).asBool();
  if (append_files) {
    // This is unfortunately a bit of a hack.  When appending files to the
    // command line we need a list of just the file names.  We would normally
    // just set the field list to contain the name, but that may conflict with
    // the setting for the "stdin" property that is managed below; if they
    // didn't ask for the name, we can't just force it in. As a bit of an
    // "easy" workaround, we'll capture the list of names from the deduping
    // mechanism.
    query->dedup_results = true;
  }

  auto ele = definition.get_default("stdin");
  if (!ele) {
    stdin_style = input_dev_null;
  } else if (ele.isArray()) {
    stdin_style = input_json;
    parse_field_list(ele, &query->fieldList);
  } else if (ele.isString()) {
    const char* str = json_string_value(ele);
    if (!strcmp(str, "/dev/null")) {
      stdin_style = input_dev_null;
    } else if (!strcmp(str, "NAME_PER_LINE")) {
      stdin_style = input_name_list;
      parse_field_list(
          json_array({typed_string_to_json("name")}), &query->fieldList);
    } else {
      throw CommandValidationError("invalid stdin value ", str);
    }
  } else {
    throw CommandValidationError("invalid value for stdin");
  }

  // unlimited unless specified
  auto ival = trig.get_default("max_files_stdin", json_integer(0)).asInt();
  if (ival < 0) {
    throw CommandValidationError("max_files_stdin must be >= 0");
  }
  max_files_stdin = ival;

  parse_redirection(trig, stdout_name, &stdout_flags, "stdout");
  parse_redirection(trig, stderr_name, &stderr_flags, "stderr");

  // Set some standard vars
  env.set(
      {{"WATCHMAN_ROOT", root->root_path},
       {"WATCHMAN_SOCK", get_sock_name_legacy()},
       {"WATCHMAN_TRIGGER", triggername}});
}

TriggerCommand::~TriggerCommand() {
  if (triggerThread_.joinable() && !stopTrigger_) {
    // We could try to call stop() here, but that is paving over the problem,
    // especially if we happen to be the triggerThread_ for some reason.
    watchman::log(
        watchman::FATAL, "destroying trigger without stopping it first\n");
  }
}

void TriggerCommand::run(const std::shared_ptr<Root>& root) {
  std::vector<std::shared_ptr<const watchman::Publisher::Item>> pending;
  w_set_thread_name(
      "trigger ", triggername.view(), " ", root->root_path.view());

  try {
    watchman_event_poll pfd[1];
    pfd[0].evt = ping_.get();

    watchman::log(watchman::DBG, "waiting for settle\n");

    while (!w_is_stopping() && !stopTrigger_) {
      ignore_result(w_poll_events(pfd, 1, 86400));
      if (w_is_stopping() || stopTrigger_) {
        break;
      }
      while (ping_->testAndClear()) {
        pending.clear();
        subscriber_->getPending(pending);
        bool seenSettle = false;
        for (auto& item : pending) {
          if (item->payload.get_default("settled")) {
            seenSettle = true;
            break;
          }
        }

        if (seenSettle) {
          if (!maybeSpawn(root)) {
            continue;
          }
          waitNoIntr();
        }
      }
    }

    if (current_proc) {
      current_proc->kill();
      current_proc->wait();
    }
  } catch (const std::exception& exc) {
    watchman::log(
        watchman::ERR,
        "Uncaught exception in trigger thread: ",
        exc.what(),
        "\n");
  }

  watchman::log(watchman::DBG, "out of loop\n");
}

void TriggerCommand::stop() {
  stopTrigger_ = true;
  if (triggerThread_.joinable()) {
    ping_->notify();
    triggerThread_.join();
  }
}

void TriggerCommand::start(const std::shared_ptr<Root>& root) {
  subscriber_ =
      root->unilateralResponses->subscribe([this] { ping_->notify(); });
  triggerThread_ = std::thread([this, root] {
    try {
      run(root);
    } catch (const std::exception& e) {
      watchman::log(
          watchman::ERR, "exception in trigger thread: ", e.what(), "\n");
    }
  });
}

bool TriggerCommand::maybeSpawn(const std::shared_ptr<Root>& root) {
  bool didRun = false;

  // If it looks like we're in a repo undergoing a rebase or
  // other similar operation, we want to defer triggers until
  // things settle down
  if (root->view()->isVCSOperationInProgress()) {
    logf(DBG, "deferring triggers until VCS operations complete\n");
    return false;
  }

  auto since_spec = query->since_spec.get();

  if (since_spec && since_spec->tag == w_cs_clock) {
    logf(
        DBG,
        "running trigger \"{}\" rules! since {}\n",
        triggername,
        since_spec->clock.position.ticks);
  } else {
    logf(DBG, "running trigger \"{}\" rules!\n", triggername);
  }

  // Triggers never need to sync explicitly; we are only dispatched
  // at settle points which are by definition sync'd to the present time
  query->sync_timeout = std::chrono::milliseconds(0);
  watchman::log(watchman::DBG, "assessing trigger ", triggername, "\n");
  try {
    auto res =
        w_query_execute(query.get(), root, time_generator, savedStateFactory_);

    watchman::log(
        watchman::DBG,
        "trigger \"",
        triggername,
        "\" generated ",
        res.resultsArray.array().size(),
        " results\n");

    // create a new spec that will be used the next time
    auto saved_spec = std::move(query->since_spec);
    query->since_spec = std::make_unique<ClockSpec>(res.clockAtStartOfQuery);

    watchman::log(
        watchman::DBG,
        "updating trigger \"",
        triggername,
        "\" use ",
        res.clockAtStartOfQuery.position().ticks,
        " ticks next time\n");

    if (!res.resultsArray.array().empty()) {
      didRun = true;
      spawn_command(root, this, &res, saved_spec.get());
    }
    return didRun;
  } catch (const QueryExecError& e) {
    watchman::log(
        watchman::ERR,
        "error running trigger \"",
        triggername,
        "\" query: ",
        e.what(),
        "\n");
    return false;
  }
}

bool TriggerCommand::waitNoIntr() {
  if (!w_is_stopping() && !stopTrigger_) {
    if (current_proc && current_proc->terminated()) {
      current_proc.reset();
      return true;
    }
  }
  return false;
}

} // namespace watchman
