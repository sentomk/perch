#pragma once

#include "lsp/json.h"

#include <string>

namespace kinglet::lsp {

// Compute completion items for a kinglet.nest manifest. The result mirrors
// what handle_completion returns: an Array of CompletionItem objects (each
// already carrying textEdit + filterText so VS Code's client-side filter
// works correctly).
//
// `file_path` is the absolute path to the manifest (used to discover .kl
// files for modules{}-completion). `text` is the live document text. `line`
// and `character` are 0-based LSP positions.
json::Array complete_nest(const std::string &file_path, const std::string &text, int line,
                          int character);

} // namespace kinglet::lsp
