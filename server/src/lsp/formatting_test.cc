#include "lsp/formatting.h"
#include "lsp/json.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

void check(bool cond, const char *expr, int line) {
  if (!cond) {
    std::cerr << "FAIL at line " << line << ": " << expr << '\n';
    ++failures;
  }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

std::string read_file(const std::filesystem::path &path) {
  std::ifstream in(path);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::filesystem::path cases_root(int argc, char **argv) {
  const std::filesystem::path root =
      (argc > 1) ? std::filesystem::path(argv[1]) : std::filesystem::current_path();
  return root / "tests" / "lsp" / "cases";
}

void test_document_range() {
  const kinglet::json::Value range = kinglet::lsp::document_range("using io;\nint main() {\n}\n");
  CHECK(range.is_object());
  const auto &obj = range.as_object();
  const auto &end = obj.at("end").as_object();
  CHECK(end.at("line").as_number() == 3);
  CHECK(end.at("character").as_number() == 0);
}

void test_format_basic_spacing(const std::filesystem::path &cases) {
  const std::filesystem::path case_dir = cases / "basic_spacing";
  const std::string input = read_file(case_dir / "input.kl");
  const std::string expected = read_file(case_dir / "expected.kl");

  const kinglet::lsp::FormatDocumentResult result =
      kinglet::lsp::format_document_text((case_dir / "input.kl").string(), input);
  CHECK(result.error.empty());
  CHECK(result.formatted == expected);
}

void test_format_parse_error(const std::filesystem::path &cases) {
  const std::filesystem::path case_dir = cases / "parse_error";
  const std::string input = read_file(case_dir / "input.kl");

  const kinglet::lsp::FormatDocumentResult result =
      kinglet::lsp::format_document_text((case_dir / "input.kl").string(), input);
  CHECK(!result.error.empty());
}

void test_format_project_config(const std::filesystem::path &cases) {
  const std::filesystem::path case_dir = cases / "project_config";
  const std::string input = read_file(case_dir / "input.kl");
  const std::string expected = read_file(case_dir / "expected.kl");

  const kinglet::lsp::FormatDocumentResult result =
      kinglet::lsp::format_document_text((case_dir / "input.kl").string(), input);
  CHECK(result.error.empty());
  CHECK(result.formatted == expected);
}

void test_format_nest_manifest_is_noop() {
  // kinglet.nest uses a completely different grammar than .kl source.
  // Regression for: formatting a .nest file used to run it through the .kl
  // expression parser and surface a misleading "Expected ';' after
  // expression" parse error instead of leaving the document untouched.
  const std::string nest_source = "project \"kinglet-app\" version \"0.1.0\"\n\n"
                                  "target app {\n}\n\n"
                                  "build {\n  default = \"app\"\n}\n";
  const kinglet::lsp::FormatDocumentResult result =
      kinglet::lsp::format_document_text("/tmp/kinglet.nest", nest_source);
  CHECK(!result.formattable);
  CHECK(result.error.empty());
  CHECK(result.formatted.empty());
}

void test_make_formatting_edits() {
  const std::string original = "int x=1;\n";
  const std::string formatted = "int x = 1;\n";
  const kinglet::json::Value edits = kinglet::lsp::make_formatting_edits(original, formatted);
  CHECK(edits.is_array());
  CHECK(edits.as_array().size() == 1);
  const auto &edit = edits.as_array().front().as_object();
  CHECK(edit.at("newText").as_string() == formatted);
}

} // namespace

int main(int argc, char **argv) {
  const std::filesystem::path cases = cases_root(argc, argv);
  test_document_range();
  test_format_basic_spacing(cases);
  test_format_parse_error(cases);
  test_format_project_config(cases);
  test_format_nest_manifest_is_noop();
  test_make_formatting_edits();

  if (failures == 0) {
    std::cout << "All formatting tests passed.\n";
    return 0;
  }
  std::cerr << failures << " formatting test(s) failed.\n";
  return 1;
}
