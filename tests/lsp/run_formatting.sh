#!/usr/bin/env bash
# Note: deliberately no `set -e` here. Each check below is run through `run`,
# which records pass/fail and keeps going — one failing case must not hide
# the pass/fail status of every check that runs after it. `pipefail` is kept
# since none of the commands below rely on a failing left-hand side of a
# pipe.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

if [[ -x "$ROOT/out/Default/kinglet-lsp" ]]; then
  LSP="$ROOT/out/Default/kinglet-lsp"
elif [[ -x "$ROOT/out/Debug/kinglet-lsp" ]]; then
  LSP="$ROOT/out/Debug/kinglet-lsp"
else
  echo "kinglet-lsp not built; run: gn gen out/Default && ninja -C out/Default kinglet-lsp formatting_test" >&2
  exit 1
fi

if [[ ! -x "$ROOT/out/Default/formatting_test" && ! -x "$ROOT/out/Debug/formatting_test" ]]; then
  echo "formatting_test not built; run: ninja -C out/Default formatting_test" >&2
  exit 1
fi

if [[ -x "$ROOT/out/Default/formatting_test" ]]; then
  TEST_BIN="$ROOT/out/Default/formatting_test"
else
  TEST_BIN="$ROOT/out/Debug/formatting_test"
fi

FAILURES=()

# Run one check. Prints a pass/fail marker and keeps going regardless of the
# check's exit code — failures are collected in FAILURES and only turned
# into a nonzero script exit status at the very end, after every check has
# had a chance to run.
run() {
  local description="$1"
  shift
  if "$@"; then
    echo "  [pass] $description"
  else
    echo "  [FAIL] $description"
    FAILURES+=("$description")
  fi
}

echo "==> unit: formatting_test"
run "formatting_test" "$TEST_BIN" "$ROOT"

echo "==> integration: lsp formatting"
run "basic_spacing" python3 "$ROOT/tests/lsp/lsp_formatting_test.py" "$LSP" "$ROOT/tests/lsp/cases/basic_spacing"
run "project_config" python3 "$ROOT/tests/lsp/lsp_formatting_test.py" "$LSP" "$ROOT/tests/lsp/cases/project_config"
run "preserve_blank_and_cast" python3 "$ROOT/tests/lsp/lsp_formatting_test.py" "$LSP" "$ROOT/tests/lsp/cases/preserve_blank_and_cast"

echo "==> integration: lsp incremental didChange"
run "incremental_ascii" python3 "$ROOT/tests/lsp/lsp_incremental_test.py" "$LSP" "$ROOT/tests/lsp/cases/incremental_ascii"
run "incremental_utf16" python3 "$ROOT/tests/lsp/lsp_incremental_test.py" "$LSP" "$ROOT/tests/lsp/cases/incremental_utf16"

echo "==> integration: lsp nest walk-up"
run "nest_walkup" python3 "$ROOT/tests/lsp/lsp_nest_walkup_test.py" "$LSP" "$ROOT/tests/lsp/cases/nest_walkup"

echo "==> integration: lsp nest diagnostics"
run "nest_diag_dirty" python3 "$ROOT/tests/lsp/lsp_nest_diagnostic_test.py" "$LSP" "$ROOT/tests/lsp/cases/nest_diag_dirty"
run "nest_diag_clean" python3 "$ROOT/tests/lsp/lsp_nest_diagnostic_test.py" "$LSP" "$ROOT/tests/lsp/cases/nest_diag_clean"

echo "==> integration: lsp import completion"
run "completion_import_prefix" python3 "$ROOT/tests/lsp/lsp_completion_test.py" "$LSP" "$ROOT/tests/lsp/cases/completion_import_prefix"
run "completion_import_mid_prefix" python3 "$ROOT/tests/lsp/lsp_completion_test.py" "$LSP" "$ROOT/tests/lsp/cases/completion_import_mid_prefix"
run "completion_import_empty" python3 "$ROOT/tests/lsp/lsp_completion_test.py" "$LSP" "$ROOT/tests/lsp/cases/completion_import_empty"
run "completion_namespace_access" python3 "$ROOT/tests/lsp/lsp_completion_test.py" "$LSP" "$ROOT/tests/lsp/cases/completion_namespace_access"

echo "==> integration: lsp nest completion"
run "nest_complete_modules" python3 "$ROOT/tests/lsp/lsp_completion_test.py" "$LSP" "$ROOT/tests/lsp/cases/nest_complete_modules"
run "nest_complete_targets" python3 "$ROOT/tests/lsp/lsp_completion_test.py" "$LSP" "$ROOT/tests/lsp/cases/nest_complete_targets"
run "nest_complete_build_default" python3 "$ROOT/tests/lsp/lsp_completion_test.py" "$LSP" "$ROOT/tests/lsp/cases/nest_complete_build_default"

echo "==> integration: lsp completion stress"
run "completion_stress" python3 "$ROOT/tests/lsp/lsp_completion_stress_test.py" "$LSP"

echo "==> integration: lsp go-to-definition"
run "definition_cross_file" python3 "$ROOT/tests/lsp/lsp_definition_test.py" "$LSP" "$ROOT/tests/lsp/cases/definition_cross_file"

echo
if [[ ${#FAILURES[@]} -eq 0 ]]; then
  echo "All LSP formatting tests passed."
  exit 0
fi

echo "${#FAILURES[@]} check(s) failed:"
for f in "${FAILURES[@]}"; do
  echo "  - $f"
done
exit 1
