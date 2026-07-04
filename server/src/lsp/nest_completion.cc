#include "lsp/nest_completion.h"

#include "lsp/protocol.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace kinglet::lsp {

namespace {

constexpr int kCompletionKindModule = 9;
constexpr int kCompletionKindKeyword = 14;
constexpr int kCompletionKindProperty = 10;
constexpr int kCompletionKindEnum = 13;
constexpr int kCompletionKindSnippet = 15;
constexpr int kCompletionKindFile = 17;

// Mirror nest_analysis.cc's whitelists. Duplicated here on purpose: the two
// translation units have different concerns (diagnostics vs candidate
// generation) and we'd rather over-list a candidate than silently drop one.
const std::vector<std::string> kBuildKeys = {"default", "backend", "out", "cache"};
const std::vector<std::string> kFmtKeys = {"indent", "max_width", "newline", "trailing_comma",
                                           "extensions"};
const std::vector<std::string> kFmtExtensions = {"align-imports", "group-using",
                                                 "align-struct-fields"};
const std::vector<std::string> kBlockHeaders = {"modules", "target", "build", "fmt"};

struct ParsedManifest {
  // module name -> rel path
  std::map<std::string, std::string> modules;
};

// Tiny, forgiving re-parse of the live text. We don't reuse nest_analysis
// because we need this to be tolerant of incomplete buffers. Tracks braces
// char-by-char so inline `modules { foo = "a.kl" }` works.
ParsedManifest parse_modules(const std::string &text) {
  ParsedManifest out;
  std::istringstream in(text);
  std::string line;
  std::string block;
  int depth = 0;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    // strip comment outside strings
    if (const auto h = line.find('#'); h != std::string::npos) {
      bool in_str = false;
      for (std::size_t i = 0; i < h; ++i) {
        if (line[i] == '"')
          in_str = !in_str;
      }
      if (!in_str)
        line.resize(h);
    }
    if (line.empty())
      continue;

    // Locate potential block opener on this line (only when at depth 0).
    std::string opener;
    if (block.empty()) {
      auto l = line.find_first_not_of(" \t");
      const auto brace_pos = line.find('{');
      if (l != std::string::npos && brace_pos != std::string::npos && brace_pos > l) {
        std::string header = line.substr(l, brace_pos - l);
        auto rr = header.find_last_not_of(" \t");
        if (rr != std::string::npos)
          header.resize(rr + 1);
        // Block keyword is only the FIRST word — headers like
        // `target <name> {` carry a name after the keyword that must not
        // be folded into the block identifier.
        auto sp = header.find_first_of(" \t");
        if (sp != std::string::npos)
          header.resize(sp);
        opener = header;
      }
    }

    // Scan for key=value entries inside a modules block. We have to handle
    // both multi-line and inline forms.
    auto scan_entries = [&](std::size_t from, std::size_t to) {
      // Pull the substring, split on whitespace-tolerant `name = "path"`.
      const std::string s = line.substr(from, to - from);
      std::size_t i = 0;
      while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == ',' || s[i] == ';')) {
          ++i;
        }
        if (i >= s.size())
          break;
        std::size_t ks = i;
        while (i < s.size() &&
               (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '.')) {
          ++i;
        }
        if (i == ks) {
          ++i;
          continue;
        }
        std::string key = s.substr(ks, i - ks);
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
          ++i;
        if (i >= s.size() || s[i] != '=')
          continue;
        ++i;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
          ++i;
        if (i >= s.size() || s[i] != '"')
          continue;
        ++i;
        std::size_t vs = i;
        while (i < s.size() && s[i] != '"') {
          if (s[i] == '\\' && i + 1 < s.size())
            i += 2;
          else
            ++i;
        }
        if (i > s.size())
          break;
        std::string val = s.substr(vs, i - vs);
        if (i < s.size())
          ++i; // skip closing quote
        out.modules[key] = val;
      }
    };

    bool in_str = false;
    std::size_t entry_start = 0;
    bool collecting = (block == "modules" && depth > 0);
    if (collecting)
      entry_start = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
      const char c = line[i];
      if (c == '"') {
        in_str = !in_str;
        continue;
      }
      if (in_str)
        continue;
      if (c == '{') {
        if (depth == 0 && !opener.empty()) {
          // Flush any preceding `modules` collection (impossible here, depth
          // was 0) and switch block.
          block = opener;
          opener.clear();
          if (block == "modules") {
            entry_start = i + 1;
            collecting = true;
          }
        } else if (collecting) {
          // nested brace inside modules — shouldn't happen in well-formed
          // manifests, but tolerate.
        }
        ++depth;
      } else if (c == '}') {
        if (collecting) {
          scan_entries(entry_start, i);
          collecting = false;
        }
        if (depth > 0)
          --depth;
        if (depth == 0)
          block.clear();
      }
    }
    // End of line: if we're still collecting in modules block, flush from
    // entry_start to end-of-line so multi-line forms work.
    if (collecting) {
      scan_entries(entry_start, line.size());
    }
  }
  return out;
}

// Find which block the cursor is in. Walk lines from top to (line-1)
// tracking the open/close brace stack of known block headers. Returns:
//   - block name ("modules" / "targets" / "build" / "fmt") if inside
//   - "" if at top level
// Lines after the cursor are not considered. We track braces char-by-char so
// single-line forms like `modules { foo = "x.kl" }` correctly close on the
// same line.
std::string block_at(const std::string &text, int cursor_line) {
  std::istringstream in(text);
  std::string line;
  std::string block; // currently-open block name; empty at top level
  int depth = 0;     // brace depth inside the currently-open block
  int lineno = 0;
  while (std::getline(in, line) && lineno < cursor_line) {
    ++lineno;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    // strip comment outside strings
    if (const auto h = line.find('#'); h != std::string::npos) {
      bool in_str = false;
      for (std::size_t i = 0; i < h; ++i) {
        if (line[i] == '"')
          in_str = !in_str;
      }
      if (!in_str)
        line.resize(h);
    }
    // Pull out the block name if this line opens one. We do this before the
    // char loop so we can attribute braces to that block.
    std::string opener;
    if (block.empty()) {
      auto l = line.find_first_not_of(" \t");
      const auto brace = line.find('{');
      if (l != std::string::npos && brace != std::string::npos && brace > l) {
        std::string header = line.substr(l, brace - l);
        auto rr = header.find_last_not_of(" \t");
        if (rr != std::string::npos)
          header.resize(rr + 1);
        // Block keyword is only the FIRST word — `target <name> {` must
        // resolve to block "target", not "target <name>".
        auto sp = header.find_first_of(" \t");
        if (sp != std::string::npos)
          header.resize(sp);
        opener = header;
      }
    }
    bool in_str = false;
    for (char c : line) {
      if (c == '"') {
        in_str = !in_str;
        continue;
      }
      if (in_str)
        continue;
      if (c == '{') {
        if (depth == 0 && !opener.empty()) {
          block = opener;
          opener.clear();
        }
        ++depth;
      } else if (c == '}') {
        if (depth > 0)
          --depth;
        if (depth == 0)
          block.clear();
      }
    }
  }
  return block;
}

// Convenience: pull the substring of `line_text` up to the cursor column.
std::string before_cursor(const std::string &line_text, int character) {
  if (character <= 0)
    return "";
  const std::size_t n = static_cast<std::size_t>(character);
  return line_text.substr(0, std::min(n, line_text.size()));
}

// Word the user has typed so far (last [A-Za-z0-9_-] run before cursor).
std::string trailing_word(const std::string &prefix) {
  std::size_t i = prefix.size();
  while (i > 0) {
    const char c = prefix[i - 1];
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') {
      --i;
      continue;
    }
    break;
  }
  return prefix.substr(i);
}

// Cursor sits inside an open string literal opened on the same line. Returns
// the partial text inside (after the opening quote) on success, or
// std::nullopt-equivalent (`bool out_open = false`) when not in a string.
bool in_open_string(const std::string &prefix, std::string &partial) {
  bool in_str = false;
  std::size_t start = 0;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (prefix[i] == '"') {
      if (in_str) {
        in_str = false;
      } else {
        in_str = true;
        start = i + 1;
      }
    }
  }
  if (in_str) {
    partial = prefix.substr(start);
    return true;
  }
  return false;
}

std::string get_line(const std::string &text, int line) {
  std::istringstream in(text);
  std::string l;
  int idx = 0;
  while (std::getline(in, l)) {
    if (idx == line) {
      if (!l.empty() && l.back() == '\r')
        l.pop_back();
      return l;
    }
    ++idx;
  }
  return "";
}

json::Value mk_item(const std::string &label, int kind, const std::string &detail, int line,
                    int start_char, int end_char, const std::string &insert_text = "") {
  // Use snippet_item_with_edit when caller passes a snippet body (contains
  // $0 / $1), and completion_item_with_edit otherwise. Both set filterText.
  if (insert_text.empty()) {
    return protocol::completion_item_with_edit(label, kind, detail, line, start_char, end_char);
  }
  return protocol::snippet_item_with_edit(label, kind, detail, insert_text, line, start_char,
                                          end_char);
}

void offer(json::Array &items, const std::string &label, int kind, const std::string &detail,
           const std::string &partial, int line, int start_char, int end_char,
           const std::string &insert_text = "") {
  if (!partial.empty()) {
    // crude prefix filter — VS Code will fuzzy match too, but trimming up
    // front keeps the popup focused.
    if (label.size() < partial.size() || label.compare(0, partial.size(), partial) != 0) {
      return;
    }
  }
  items.push_back(mk_item(label, kind, detail, line, start_char, end_char, insert_text));
}

} // namespace

json::Array complete_nest(const std::string &file_path, const std::string &text, int line,
                          int character) {
  json::Array items;
  const std::string cur_line = get_line(text, line);
  const std::string prefix = before_cursor(cur_line, character);
  const std::string word = trailing_word(prefix);
  const int word_start = character - static_cast<int>(word.size());

  std::string in_str;
  const bool string_ctx = in_open_string(prefix, in_str);
  // For string-context replacements, the textEdit range covers from the
  // opening quote's interior to the cursor.
  const int str_start = character - static_cast<int>(in_str.size());

  const std::string block = block_at(text, line);
  const ParsedManifest parsed = parse_modules(text);

  // ---------------------------------------------------------------- top level
  if (block.empty()) {
    // If the user has typed nothing structural on this line yet, offer block
    // headers and the project line as a snippet.
    offer(items, "modules", kCompletionKindKeyword, "modules { name = \"path.kl\" }", word, line,
          word_start, character, "modules {\n  $0\n}");
    offer(items, "target", kCompletionKindKeyword, "target <name> { kind sources deps }", word,
          line, word_start, character,
          "target ${1:name} {\n  kind = \"${2:binary}\"\n  sources = [\"${3:src/}\"]$0\n}");
    offer(items, "build", kCompletionKindKeyword, "build { default backend out cache }", word, line,
          word_start, character, "build {\n  $0\n}");
    offer(items, "fmt", kCompletionKindKeyword,
          "fmt { indent max_width newline trailing_comma extensions }", word, line, word_start,
          character, "fmt {\n  $0\n}");
    offer(items, "project", kCompletionKindSnippet, "project header (name + semver)", word, line,
          word_start, character, "project \"${1:name}\" version \"${2:0.1.0}\"$0");
    return items;
  }

  // ---------------------------------------------------------------- modules
  if (block == "modules") {
    if (string_ctx) {
      // RHS path slot — offer .kl files under the project root.
      std::filesystem::path manifest(file_path);
      std::error_code ec;
      std::filesystem::path manifest_abs = std::filesystem::absolute(manifest, ec);
      if (ec)
        manifest_abs = manifest;
      const std::filesystem::path root =
          manifest_abs.has_parent_path() ? manifest_abs.parent_path() : std::filesystem::path(".");
      // Track paths we've already declared so we don't re-suggest them.
      std::set<std::string> taken;
      for (const auto &[name, p] : parsed.modules)
        taken.insert(p);
      for (auto it = std::filesystem::recursive_directory_iterator(
               root, std::filesystem::directory_options::skip_permission_denied, ec);
           !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_directory()) {
          const std::string name = it->path().filename().string();
          if (name.size() >= 1 && name.front() == '.')
            it.disable_recursion_pending();
          continue;
        }
        if (it->path().extension() != ".kl")
          continue;
        std::string rel = std::filesystem::relative(it->path(), root, ec).generic_string();
        if (ec)
          continue;
        if (taken.count(rel))
          continue;
        offer(items, rel, kCompletionKindFile, "file under project root", in_str, line, str_start,
              character);
      }
      return items;
    }
    // Key slot — nothing intrinsic to offer; the user picks a name freely.
    // Still give them a snippet so they get the `=` and quoted RHS for free.
    if (word.empty() || std::isalpha(static_cast<unsigned char>(word.front())) ||
        word.front() == '_') {
      items.push_back(protocol::snippet_item_with_edit(
          "module-entry", kCompletionKindSnippet, "module name = \"path.kl\"",
          "${1:name} = \"${2:path.kl}\"$0", line, word_start, character));
    }
    return items;
  }

  // ---------------------------------------------------------------- target
  if (block == "target") {
    const auto eq = prefix.find('=');
    if (eq != std::string::npos) {
      // RHS. Behavior depends on the key on the LHS.
      std::string lhs = prefix.substr(0, eq);
      auto lr = lhs.find_last_not_of(" \t");
      if (lr != std::string::npos)
        lhs = lhs.substr(0, lr + 1);
      auto ll = lhs.find_first_not_of(" \t");
      if (ll != std::string::npos)
        lhs = lhs.substr(ll);

      if (lhs == "kind") {
        offer(items, "binary", kCompletionKindEnum, "executable target", word, line, word_start,
              character);
        offer(items, "library", kCompletionKindEnum, "library target", word, line, word_start,
              character);
        offer(items, "test", kCompletionKindEnum, "test target", word, line, word_start, character);
        offer(items, "object", kCompletionKindEnum, "object file target", word, line, word_start,
              character);
      }
      // sources / deps — free-form, nothing to offer.
      return items;
    }
    // LHS — offer known keys.
    offer(items, "kind", kCompletionKindProperty, "target kind (binary, library, test, object)",
          word, line, word_start, character);
    offer(items, "sources", kCompletionKindProperty, "source files/dirs for this target", word,
          line, word_start, character);
    offer(items, "deps", kCompletionKindProperty, "dependencies of this target", word, line,
          word_start, character);
    return items;
  }

  // ---------------------------------------------------------------- build
  if (block == "build") {
    const auto eq = prefix.find('=');
    if (eq != std::string::npos) {
      // RHS. Behavior depends on the key on the LHS.
      std::string lhs = prefix.substr(0, eq);
      auto lr = lhs.find_last_not_of(" \t");
      if (lr != std::string::npos)
        lhs = lhs.substr(0, lr + 1);
      auto ll = lhs.find_first_not_of(" \t");
      if (ll != std::string::npos)
        lhs = lhs.substr(ll);

      if (lhs == "default") {
        if (string_ctx) {
          for (const auto &[name, _] : parsed.modules) {
            offer(items, name, kCompletionKindModule, "module entry point", in_str, line, str_start,
                  character);
          }
        } else {
          // Help the user open the quotes.
          items.push_back(protocol::snippet_item_with_edit(
              "\"module\"", kCompletionKindSnippet, "quoted module name", "\"${1:module}\"$0", line,
              word_start, character));
        }
        return items;
      }
      if (lhs == "backend") {
        offer(items, "native", kCompletionKindEnum, "native backend", word, line, word_start,
              character);
        offer(items, "bytecode", kCompletionKindEnum, "bytecode backend", word, line, word_start,
              character);
        return items;
      }
      // out / cache — free-form path. Nothing useful to offer.
      return items;
    }
    // LHS — offer the known keys.
    for (const auto &k : kBuildKeys) {
      offer(items, k, kCompletionKindProperty, "build setting", word, line, word_start, character);
    }
    return items;
  }

  // ---------------------------------------------------------------- fmt
  if (block == "fmt") {
    const auto eq = prefix.find('=');
    if (eq != std::string::npos) {
      std::string lhs = prefix.substr(0, eq);
      auto lr = lhs.find_last_not_of(" \t");
      if (lr != std::string::npos)
        lhs = lhs.substr(0, lr + 1);
      auto ll = lhs.find_first_not_of(" \t");
      if (ll != std::string::npos)
        lhs = lhs.substr(ll);

      if (lhs == "trailing_comma") {
        offer(items, "true", kCompletionKindEnum, "trailing comma on", word, line, word_start,
              character);
        offer(items, "false", kCompletionKindEnum, "trailing comma off", word, line, word_start,
              character);
        return items;
      }
      if (lhs == "extensions") {
        // Either inside a string literal or about to type one.
        for (const auto &e : kFmtExtensions) {
          if (string_ctx) {
            offer(items, e, kCompletionKindEnum, "fmt extension", in_str, line, str_start,
                  character);
          } else {
            items.push_back(protocol::snippet_item_with_edit("\"" + e + "\"", kCompletionKindEnum,
                                                             "fmt extension", "\"" + e + "\"", line,
                                                             word_start, character));
          }
        }
        return items;
      }
      // indent / max_width / newline — free-form integer or string.
      return items;
    }
    for (const auto &k : kFmtKeys) {
      offer(items, k, kCompletionKindProperty, "fmt setting", word, line, word_start, character);
    }
    return items;
  }

  // Unknown block — leave silent rather than guessing.
  return items;
}

} // namespace kinglet::lsp
