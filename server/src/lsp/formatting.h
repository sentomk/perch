#pragma once

#include "lsp/json.h"

#include <string>
#include <string_view>

namespace kinglet::lsp {

struct FormatDocumentResult {
  std::string formatted;
  std::string error;
  // False for documents whose grammar isn't the .kl expression language
  // (e.g. kinglet.nest manifests). format_document_text() leaves formatted
  // and error both empty in that case rather than running the .kl parser
  // against source it was never designed to read.
  bool formattable = true;
};

FormatDocumentResult format_document_text(const std::string &file_path, std::string_view source);
json::Value document_range(const std::string &text);
json::Value make_formatting_edits(const std::string &original, const std::string &formatted);

} // namespace kinglet::lsp
