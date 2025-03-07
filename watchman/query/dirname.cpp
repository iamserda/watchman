/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/Errors.h"
#include "watchman/query/Query.h"
#include "watchman/query/QueryExpr.h"
#include "watchman/query/TermRegistry.h"
#include "watchman/query/intcompare.h"

#include <memory>

using namespace watchman;

static inline bool is_dir_sep(int c) {
  return c == '/' || c == '\\';
}

class DirNameExpr : public QueryExpr {
  w_string dirname;
  struct w_query_int_compare depth;
  using StartsWith = bool (*)(w_string_t* str, w_string_t* prefix);
  StartsWith startswith;

 public:
  explicit DirNameExpr(
      w_string dirname,
      struct w_query_int_compare depth,
      StartsWith startswith)
      : dirname(dirname), depth(depth), startswith(startswith) {}

  EvaluateResult evaluate(QueryContextBase* ctx, FileResult*) override {
    auto& str = ctx->getWholeName();

    if (str.size() <= dirname.size()) {
      // Either it doesn't prefix match, or file name is == dirname.
      // That means that the best case is that the wholename matches.
      // we only want to match if dirname(wholename) matches, so it
      // is not possible for us to match unless the length of wholename
      // is greater than the dirname operand
      return false;
    }

    // Want to make sure that wholename is a child of dirname, so
    // check for a dir separator.  Special case for dirname == '' (the root),
    // which won't have a slash in position 0.
    if (dirname.size() > 0 && !is_dir_sep(str.data()[dirname.size()])) {
      // may have a common prefix with, but is not a child of dirname
      return false;
    }

    if (!startswith(str, dirname)) {
      return false;
    }

    // Now compute the depth of file from dirname.  We do this by
    // counting dir separators, not including the one we saw above.
    json_int_t actual_depth = 0;
    for (size_t i = dirname.size() + 1; i < str.size(); i++) {
      if (is_dir_sep(str.data()[i])) {
        actual_depth++;
      }
    }

    return eval_int_compare(actual_depth, &depth);
  }

  // ["dirname", "foo"] -> ["dirname", "foo", ["depth", "ge", 0]]
  static std::unique_ptr<QueryExpr>
  parse(Query*, const json_ref& term, CaseSensitivity case_sensitive) {
    const char* which = case_sensitive == CaseSensitivity::CaseInSensitive
        ? "idirname"
        : "dirname";
    struct w_query_int_compare depth_comp;

    if (!term.isArray()) {
      throw QueryParseError("Expected array for '", which, "' term");
    }

    if (json_array_size(term) < 2) {
      throw QueryParseError(
          "Invalid number of arguments for '", which, "' term");
    }

    if (json_array_size(term) > 3) {
      throw QueryParseError(
          "Invalid number of arguments for '", which, "' term");
    }

    const auto& name = term.at(1);
    if (!name.isString()) {
      throw QueryParseError("Argument 2 to '", which, "' must be a string");
    }

    if (json_array_size(term) == 3) {
      const auto& depth = term.at(2);
      if (!depth.isArray()) {
        throw QueryParseError(
            "Invalid number of arguments for '", which, "' term");
      }

      parse_int_compare(depth, &depth_comp);

      if (strcmp("depth", json_string_value(json_array_get(depth, 0)))) {
        throw QueryParseError(
            "Third parameter to '",
            which,
            "' should be a relational depth term");
      }
    } else {
      depth_comp.operand = 0;
      depth_comp.op = W_QUERY_ICMP_GE;
    }

    return std::make_unique<DirNameExpr>(
        json_to_w_string(name),
        depth_comp,
        case_sensitive == CaseSensitivity::CaseInSensitive
            ? w_string_startswith_caseless
            : w_string_startswith);
  }
  static std::unique_ptr<QueryExpr> parseDirName(
      Query* query,
      const json_ref& term) {
    return parse(query, term, query->case_sensitive);
  }
  static std::unique_ptr<QueryExpr> parseIDirName(
      Query* query,
      const json_ref& term) {
    return parse(query, term, CaseSensitivity::CaseInSensitive);
  }
};

W_TERM_PARSER(dirname, DirNameExpr::parseDirName);
W_TERM_PARSER(idirname, DirNameExpr::parseIDirName);

/* vim:ts=2:sw=2:et:
 */
