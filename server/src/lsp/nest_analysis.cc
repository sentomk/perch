#include "lsp/nest_analysis.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace kinglet::lsp {

namespace {

constexpr int kSeverityWarning = 2;

const std::unordered_set<std::string> kKnownBlocks = {"modules", "target", "build", "fmt"};
const std::unordered_set<std::string> kBuildKeys = {"default", "backend", "out", "cache"};
const std::unordered_set<std::string> kFmtKeys = {"indent", "max_width", "newline",
                                                  "trailing_comma", "extensions"};
const std::unordered_set<std::string> kFmtExtensions = {"align-imports", "group-using",
                                                        "align-struct-fields"};

struct LineView {
  std::string text;
  int line;      // 1-based
  int start_col; // 1-based column of `text[0]` in the original source
};

// Split `source` into LineView entries — preserves trailing whitespace per
// line because we want column-accurate diagnostics.
std::vector<LineView> split_lines(const std::string &source) {
  std::vector<LineView> out;
  std::size_t i = 0;
  int line = 1;
  while (i <= source.size()) {
    const std::size_t start = i;
    while (i < source.size() && source[i] != '\n') {
      ++i;
    }
    out.push_back({source.substr(start, i - start), line, 1});
    if (i == source.size())
      break;
    ++i; // skip '\n'
    ++line;
  }
  return out;
}

void ltrim(std::string &s, int &col) {
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
    ++i;
    ++col;
  }
  s.erase(0, i);
}

void rtrim(std::string &s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.pop_back();
  }
}

// Strip a trailing `# ...` comment, respecting strings. Returns the comment-
// free prefix (still possibly with trailing whitespace from before the #).
std::string strip_comment(const std::string &line) {
  bool in_string = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '"') {
      // crude escape handling: \" stays in string
      if (i > 0 && line[i - 1] == '\\')
        continue;
      in_string = !in_string;
    } else if (c == '#' && !in_string) {
      return line.substr(0, i);
    }
  }
  return line;
}

struct Token {
  std::string text;
  int col; // 1-based
};

// Tokenize a single (non-comment, non-blank) line into a small sequence:
//   - quoted strings (without the quotes)
//   - bracket runs `[...]` kept as-is
//   - identifiers / numbers / `=` / `{` / `}`
// All other chars are dropped silently.
std::vector<Token> tokenize_line(const std::string &line, int start_col) {
  std::vector<Token> out;
  std::size_t i = 0;
  while (i < line.size()) {
    const char c = line[i];
    if (c == ' ' || c == '\t' || c == '\r') {
      ++i;
      continue;
    }
    if (c == '"') {
      const int col = start_col + static_cast<int>(i);
      std::string buf;
      buf.push_back('"');
      ++i;
      while (i < line.size() && line[i] != '"') {
        if (line[i] == '\\' && i + 1 < line.size()) {
          buf.push_back(line[i]);
          buf.push_back(line[i + 1]);
          i += 2;
          continue;
        }
        buf.push_back(line[i]);
        ++i;
      }
      if (i < line.size()) {
        buf.push_back('"');
        ++i;
      }
      out.push_back({buf, col});
      continue;
    }
    if (c == '[') {
      const int col = start_col + static_cast<int>(i);
      std::string buf;
      int depth = 0;
      while (i < line.size()) {
        buf.push_back(line[i]);
        if (line[i] == '[')
          ++depth;
        else if (line[i] == ']') {
          --depth;
          if (depth == 0) {
            ++i;
            break;
          }
        }
        ++i;
      }
      out.push_back({buf, col});
      continue;
    }
    if (c == '=' || c == '{' || c == '}') {
      const int col = start_col + static_cast<int>(i);
      out.push_back({std::string(1, c), col});
      ++i;
      continue;
    }
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-') {
      const int col = start_col + static_cast<int>(i);
      std::string buf;
      while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) ||
                                 line[i] == '_' || line[i] == '.' || line[i] == '-')) {
        buf.push_back(line[i]);
        ++i;
      }
      out.push_back({buf, col});
      continue;
    }
    // Unknown char — skip silently; the high-level structure check will
    // catch the missing pieces.
    ++i;
  }
  return out;
}

// Return the contents of a quoted token without its surrounding quotes.
std::string unquote(const std::string &t) {
  if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
    return t.substr(1, t.size() - 2);
  }
  return t;
}

bool is_integer_literal(const std::string &s) {
  if (s.empty())
    return false;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c)))
      return false;
  }
  return true;
}

void push_diag(AnalysisResult &out, int line, int col, int length, std::string message,
               int severity) {
  Diagnostic d;
  d.line = line;
  d.col = col;
  d.length = std::max(1, length);
  d.message = std::move(message);
  d.severity = severity;
  out.diagnostics.push_back(std::move(d));
}

} // namespace

AnalysisResult analyze_nest(const std::string &file_path, const std::string &text) {
  AnalysisResult out;

  std::filesystem::path manifest_path(file_path);
  std::error_code ec;
  std::filesystem::path manifest_abs = std::filesystem::absolute(manifest_path, ec);
  if (ec)
    manifest_abs = manifest_path;
  const std::filesystem::path project_root =
      manifest_abs.has_parent_path() ? manifest_abs.parent_path() : std::filesystem::path(".");

  const auto lines = split_lines(text);

  // First pass: collect modules { } entries and target <name> { } blocks so we
  // can cross-reference build.default in the second pass.
  std::unordered_map<std::string, int> modules;              // name -> declared line
  std::unordered_map<std::string, std::string> module_paths; // name -> rel path
  std::unordered_set<std::string> targets;                   // declared target names
  std::set<std::string> reported_dup_modules;

  // We track block context across lines via a simple state machine.
  // current_block:
  //   ""        - top level
  //   "modules" | "target" | "build" | "fmt"
  //   "?<name>" - unknown block (still skip its body for diag purposes)
  std::string block;
  int block_open_line = 0;
  std::string current_target_name; // name from "target <name> {"

  // Per-block duplicate-key tracking.
  std::unordered_set<std::string> block_keys;

  // When the parser enters a multi-line array value (e.g. `sources = [\n`
  // ... `]`), subsequent lines inside the array are just quoted strings
  // (optionally comma-terminated), not `key = value` entries. We track
  // this so the block-body parser below doesn't flag them as malformed.
  bool in_block_array = false;

  for (const auto &lv : lines) {
    std::string raw = strip_comment(lv.text);
    int col = lv.start_col;
    ltrim(raw, col);
    rtrim(raw);
    if (raw.empty())
      continue;

    // Block-close marker (alone on a line) ends the block.
    if (raw == "}") {
      block.clear();
      current_target_name.clear();
      block_keys.clear();
      in_block_array = false;
      continue;
    }

    auto tokens = tokenize_line(raw, col);
    if (tokens.empty()) {
      // A `]` (or `],`) on its own line is not tokenized ("]" is not an
      // operator token in the nest grammar), so the token array comes
      // back empty. If we're inside a multi-line array, this empty line
      // is the closing bracket — exit array mode.
      if (in_block_array && raw.find(']') != std::string::npos) {
        in_block_array = false;
      }
      continue;
    }

    // Block headers like `modules {` or `fmt {`.
    if (block.empty()) {
      if (tokens.size() >= 2 && tokens.back().text == "{") {
        const std::string &header = tokens.front().text;
        // `project "name" version "ver"` — single line, ignore for diags.
        if (header == "project") {
          continue;
        }
        if (!kKnownBlocks.count(header)) {
          push_diag(out, lv.line, tokens.front().col, static_cast<int>(header.size()),
                    "unknown block '" + header + "'; expected modules, target, build, or fmt",
                    kSeverityWarning);
        }
        block = kKnownBlocks.count(header) ? header : "?" + header;
        block_open_line = lv.line;
        block_keys.clear();
        // For "target <name> {" blocks, record the target name.
        if (header == "target" && tokens.size() >= 3 && tokens[2].text == "{") {
          current_target_name = tokens[1].text;
          targets.insert(current_target_name);
        }
        continue;
      }
      // Top-level `project` line.
      if (tokens.front().text == "project") {
        continue;
      }
      // Anything else at top level is suspicious.
      push_diag(out, lv.line, tokens.front().col, static_cast<int>(tokens.front().text.size()),
                "unexpected top-level token '" + tokens.front().text + "'", kSeverityWarning);
      continue;
    }

    // Inside a block. Skip unknown-block bodies — we already warned at the
    // header.
    if (!block.empty() && block.front() == '?') {
      if (tokens.size() == 1 && tokens.front().text == "}") {
        block.clear();
        block_keys.clear();
      }
      continue;
    }

    // Inside a block body.
    // If we're currently inside a multi-line array value (e.g. the lines
    // after `sources = [`), skip key=value validation — the tokens are just
    // quoted strings and commas, not structured entries.
    if (in_block_array) {
      // Look for the closing bracket (possibly with a trailing comma).
      if (raw.find(']') != std::string::npos) {
        in_block_array = false;
      }
      continue;
    }

    // Block body: lines look like `key = value [value ...]`.
    if (tokens.size() < 3 || tokens[1].text != "=") {
      // Try to detect a multi-line array continuation — a lone quoted
      // string or comma-terminated value that belongs to the array opened
      // by the previous line's `key = [`.
      bool looks_like_array_entry = false;
      for (const auto &t : tokens) {
        if (t.text.size() >= 2 && t.text.front() == '"') {
          looks_like_array_entry = true;
          break;
        }
        if (t.text == "," || t.text == "]" || t.text == "],") {
          looks_like_array_entry = true;
          break;
        }
      }
      if (looks_like_array_entry) {
        if (raw.find(']') != std::string::npos) {
          // Closing bracket on this line — exit array mode.
        }
        continue;
      }
      push_diag(out, lv.line, tokens.front().col, static_cast<int>(tokens.front().text.size()),
                "expected `key = value` inside " + block + " { ... }", kSeverityWarning);
      continue;
    }
    const Token &key = tokens[0];
    const Token &eq = tokens[1];
    (void)eq;

    // Detect `key = [` (start of a multi-line array value).
    if (tokens.size() >= 3) {
      const std::string &rhs = tokens[2].text;
      if (rhs.size() >= 2 && rhs.front() == '[' && rhs.back() == ']') {
        // Single-line array ('key = [...]') — no special tracking needed.
      } else if (rhs.front() == '[' && rhs.back() != ']') {
        // Multi-line array: the value list spans across lines.
        // Subsequent lines are just quoted strings / commas, not key=value.
        in_block_array = true;
      }
    }

    if (!block_keys.insert(key.text).second) {
      push_diag(out, lv.line, key.col, static_cast<int>(key.text.size()),
                "duplicate key '" + key.text + "' in " + block + " block", kSeverityWarning);
      // Keep the latter declaration for downstream checks.
    }

    if (block == "modules") {
      // Module/target name validity — must look like a dotted identifier.
      bool legal = !key.text.empty() &&
                   (std::isalpha(static_cast<unsigned char>(key.text[0])) || key.text[0] == '_');
      for (std::size_t i = 1; legal && i < key.text.size(); ++i) {
        const char c = key.text[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.')) {
          legal = false;
        }
      }
      if (!legal) {
        push_diag(out, lv.line, key.col, static_cast<int>(key.text.size()),
                  "invalid identifier '" + key.text + "'", kSeverityWarning);
      }
    }

    if (block == "modules") {
      // Right-hand side should be a single quoted path.
      if (tokens.size() != 3 || tokens[2].text.empty() || tokens[2].text.front() != '"') {
        push_diag(out, lv.line, tokens[2].col, static_cast<int>(tokens[2].text.size()),
                  "modules entries take a single quoted path, e.g. \"lib/foo.kl\"",
                  kSeverityWarning);
      } else {
        const std::string rel = unquote(tokens[2].text);
        const std::filesystem::path candidate = project_root / rel;
        if (!std::filesystem::exists(candidate, ec)) {
          push_diag(out, lv.line, tokens[2].col, static_cast<int>(tokens[2].text.size()),
                    "module file '" + rel + "' does not exist under the project root",
                    kSeverityWarning);
        }
        modules[key.text] = lv.line;
        module_paths[key.text] = rel;
      }
      continue;
    }

    if (block == "build") {
      if (!kBuildKeys.count(key.text)) {
        push_diag(out, lv.line, key.col, static_cast<int>(key.text.size()),
                  "unknown build key '" + key.text + "'; expected default, backend, out, or cache",
                  kSeverityWarning);
        continue;
      }
      // Defer build.default cross-check to a second pass — modules table
      // may be declared after the build block.
      continue;
    }

    if (block == "fmt") {
      if (!kFmtKeys.count(key.text)) {
        push_diag(out, lv.line, key.col, static_cast<int>(key.text.size()),
                  "unknown fmt key '" + key.text +
                      "'; expected indent, max_width, newline, trailing_comma, or extensions",
                  kSeverityWarning);
        continue;
      }
      const std::string &v = tokens[2].text;
      if (key.text == "indent" || key.text == "max_width") {
        const std::string raw_v = v.front() == '"' ? unquote(v) : v;
        if (!is_integer_literal(raw_v)) {
          push_diag(out, lv.line, tokens[2].col, static_cast<int>(v.size()),
                    "fmt." + key.text + " expects an integer literal", kSeverityWarning);
        }
      } else if (key.text == "trailing_comma") {
        const std::string raw_v = v.front() == '"' ? unquote(v) : v;
        if (raw_v != "true" && raw_v != "false") {
          push_diag(out, lv.line, tokens[2].col, static_cast<int>(v.size()),
                    "fmt.trailing_comma expects true or false", kSeverityWarning);
        }
      } else if (key.text == "extensions") {
        // Accept either `"name"` or `[name, name]`. Iterate the value tokens.
        std::vector<std::pair<std::string, int>> items;
        if (v.front() == '"') {
          items.emplace_back(unquote(v), tokens[2].col);
        } else if (v.front() == '[' && v.back() == ']') {
          std::string body = v.substr(1, v.size() - 2);
          int cursor = tokens[2].col + 1;
          std::string item;
          int item_col = cursor;
          auto flush = [&]() {
            if (item.empty())
              return;
            std::string trimmed;
            for (char c : item) {
              if (c != ' ' && c != '\t')
                trimmed.push_back(c);
            }
            if (!trimmed.empty()) {
              if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"') {
                trimmed = trimmed.substr(1, trimmed.size() - 2);
              }
              items.emplace_back(trimmed, item_col);
            }
            item.clear();
          };
          for (char c : body) {
            if (c == ',') {
              flush();
              item_col = cursor + 1;
            } else {
              if (item.empty())
                item_col = cursor;
              item.push_back(c);
            }
            ++cursor;
          }
          flush();
        } else {
          push_diag(out, lv.line, tokens[2].col, static_cast<int>(v.size()),
                    "fmt.extensions expects a quoted name or a [list]", kSeverityWarning);
        }
        for (const auto &[name, name_col] : items) {
          if (!kFmtExtensions.count(name)) {
            push_diag(out, lv.line, name_col, static_cast<int>(name.size()) + 2,
                      "unknown fmt extension '" + name +
                          "'; known extensions are align-imports, group-using, align-struct-fields",
                      kSeverityWarning);
          }
        }
      }
      continue;
    }

    if (block == "target") {
      // target <name> { kind = "binary" / "library" / "test" / "object"
      //                sources = [...]
      //                deps = [...] }
      const std::string &k = key.text;
      if (k == "kind") {
        const std::string v =
            tokens[2].text.front() == '"' ? unquote(tokens[2].text) : tokens[2].text;
        if (v != "binary" && v != "library" && v != "test" && v != "object") {
          push_diag(out, lv.line, tokens[2].col, static_cast<int>(tokens[2].text.size()),
                    "unknown target kind '" + v + "'; expected binary, library, test, or object",
                    kSeverityWarning);
        }
      } else if (k == "sources" || k == "deps") {
        // sources / deps: accept any value (string list in brackets or bare).
      } else {
        push_diag(out, lv.line, key.col, static_cast<int>(key.text.size()),
                  "unknown target key '" + k + "'; expected kind, sources, or deps",
                  kSeverityWarning);
      }
      continue;
    }
  }
  if (!block.empty() && block.front() != '?') {
    push_diag(out, block_open_line, 1, 1, "unterminated " + block + " { ... } block",
              kSeverityWarning);
  }

  // Second pass: now that the modules table is known, cross-check references.
  block.clear();
  for (const auto &lv : lines) {
    std::string raw = strip_comment(lv.text);
    int col = lv.start_col;
    ltrim(raw, col);
    rtrim(raw);
    if (raw.empty())
      continue;
    auto tokens = tokenize_line(raw, col);
    if (tokens.empty())
      continue;
    if (raw == "}") {
      block.clear();
      continue;
    }
    if (block.empty()) {
      if (tokens.size() >= 2 && tokens.back().text == "{") {
        const std::string header = tokens.front().text;
        if (kKnownBlocks.count(header))
          block = header;
        else
          block = "?" + header;
      }
      continue;
    }
    if (!block.empty() && block.front() == '?') {
      if (tokens.size() == 1 && tokens.front().text == "}")
        block.clear();
      continue;
    }
    if (tokens.size() < 3 || tokens[1].text != "=")
      continue;

    if (block == "build" && tokens[0].text == "default") {
      if (tokens[2].text.front() == '"') {
        const std::string name = unquote(tokens[2].text);
        // build.default references a target, not a module.
        if (!name.empty() && !targets.count(name)) {
          push_diag(out, lv.line, tokens[2].col, static_cast<int>(tokens[2].text.size()),
                    "build.default '" + name + "' is not declared in a target { ... } block",
                    kSeverityWarning);
        }
      }
    }
  }

  return out;
}

} // namespace kinglet::lsp
