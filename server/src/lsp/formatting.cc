#include "lsp/formatting.h"

#include "lsp/protocol.h"
#include "frontend/module/project_config.h"
#include "driver/preen/config.h"
#include "driver/preen/preen.h"

#include <filesystem>

namespace kinglet::lsp {

namespace {

kinglet::preen::FmtConfig resolve_fmt_config(const std::string &file_path) {
  kinglet::preen::FmtConfig config = kinglet::preen::FmtConfig::defaults();
  std::filesystem::path path(file_path);
  std::error_code ec;
  std::filesystem::path abs_path = std::filesystem::absolute(path, ec);
  if (ec)
    abs_path = path;
  const std::string dir =
      abs_path.has_parent_path() ? abs_path.parent_path().string() : std::string(".");
  if (const auto project = kinglet::find_project_config(dir)) {
    config = kinglet::preen::fmt_config_from_project(*project);
  }
  return config;
}

} // namespace

FormatDocumentResult format_document_text(const std::string &file_path, std::string_view source) {
  FormatDocumentResult out;
  // format_string()/Parser only understand .kl expression-language syntax.
  // kinglet.nest manifests (and any other non-.kl document a client might
  // ask to format) use a different, hand-rolled grammar entirely — running
  // them through the .kl parser produces a misleading "Expected ';' after
  // expression"-style error instead of a clean no-op. The CLI's `kinglet
  // fmt` already restricts itself to .kl files (cmd_fmt.cc's
  // collect_kl_files); mirror that restriction here.
  if (std::filesystem::path(file_path).extension() != ".kl") {
    out.formattable = false;
    return out;
  }
  const auto config = resolve_fmt_config(file_path);
  const kinglet::preen::FormatResult result = kinglet::preen::format_string(source, config);
  if (!result.error.empty()) {
    out.error = result.error;
    return out;
  }
  out.formatted = result.text;
  return out;
}

json::Value document_range(const std::string &text) {
  int end_line = 0;
  int end_character = 0;
  for (const char ch : text) {
    if (ch == '\n') {
      ++end_line;
      end_character = 0;
    } else {
      ++end_character;
    }
  }
  return protocol::range(0, 0, end_line, end_character);
}

json::Value make_formatting_edits(const std::string &original, const std::string &formatted) {
  json::Object edit;
  edit["range"] = document_range(original);
  edit["newText"] = json::Value::string(formatted);
  json::Array edits;
  edits.push_back(json::Value(edit));
  return json::Value(edits);
}

} // namespace kinglet::lsp
