#pragma once

#include "lsp/analysis.h"

#include <string>

namespace kinglet::lsp {

// Analyze a kinglet.nest manifest at `file_path` with raw `text` contents.
// Returns an AnalysisResult populated with diagnostics. Unlike analyze() for
// .kl files, this does not produce symbols or imported namespaces — the
// manifest is configuration, not code.
//
// Reported diagnostics include:
//   * unknown top-level block (typo of "modules"/"target"/"build"/"fmt")
//   * unknown key inside build {} / fmt {} / target {} blocks
//   * fmt.extensions value not in the known whitelist
//   * fmt.indent / fmt.max_width not a number
//   * fmt.trailing_comma not a boolean literal
//   * modules { foo = "rel/path" } where the file does not exist on disk
//   * target { kind = "bogus" } where bogus is not a known kind
//   * build.default referencing an undeclared target
//   * duplicate keys inside the same block
AnalysisResult analyze_nest(const std::string &file_path, const std::string &text);

} // namespace kinglet::lsp
