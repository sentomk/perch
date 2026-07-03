#pragma once

#include "lsp/analysis.h"
#include "frontend/parser/completion_context.h"
#include "lsp/json.h"

#include <string>
#include <vector>

namespace kinglet::lsp {

class CompletionResolver {
public:
  CompletionResolver(const AnalysisResult &analysis, const std::string &prefix, int line,
                     int character, const std::string &uri);

  json::Array resolve(const CompletionInfo &info);

private:
  json::Array resolve_top_level();
  json::Array resolve_statement();
  json::Array resolve_expression();
  json::Array resolve_type_expr(const std::vector<std::string> &type_params);
  json::Array resolve_param_type(const std::vector<std::string> &type_params);
  json::Array resolve_field_access(const std::string &receiver_type);
  std::string walk_access_chain(const std::string &chain);
  std::string member_type(const std::string &type_name, const std::string &member, bool is_call);
  json::Array resolve_namespace_access(const std::string &ns_name);
  json::Array resolve_import_path();
  json::Array resolve_import_symbol(const std::string &import_path);
  json::Array resolve_using_namespace();
  json::Array resolve_struct_literal(const std::string &struct_name);
  json::Array resolve_enum_variant(const std::string &subject_name);

  void add_scope_symbols(json::Array &items);
  void add_io_members(json::Array &items);
  void add_type_keywords(json::Array &items);
  void add_type_keywords(json::Array &items, bool include_void_auto);
  void add_cast_keywords(json::Array &items);
  void add_type_params(json::Array &items, const std::vector<std::string> &type_params);
  void add_statement_keywords(json::Array &items);
  void add_decl_keywords(json::Array &items);
  void add_namespace_completions(json::Array &items);

  bool matches_prefix(const std::string &name) const;

  const AnalysisResult &analysis_;
  std::string prefix_;
  int line_;
  int character_;
  std::string uri_;
};

} // namespace kinglet::lsp
