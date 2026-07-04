#include "lsp/analysis.h"

#include "frontend/checker/type_checker.h"
#include "frontend/lexer/scanner.h"
#include "frontend/module/module_loader.h"

#include <filesystem>

namespace kinglet::lsp {

namespace {

int stmt_start_line(const ast::Stmt *stmt) {
  return stmt != nullptr ? stmt->location.line : 999999;
}

class SymbolCollector {
public:
  void collect(const ast::Program &program) {
    for (const auto &decl : program.declarations) {
      visit_decl(*decl);
    }
  }

  SymbolTable take() { return std::move(table_); }

private:
  void visit_decl(const ast::Decl &decl) {
    if (const auto *func = dynamic_cast<const ast::FunctionDecl *>(&decl)) {
      Symbol sym;
      sym.name = func->name;
      sym.kind = SymbolKind::Function;
      sym.type_name = func->return_type.to_string();
      sym.location = func->location;
      sym.return_type = func->return_type.to_string();
      sym.params = func->params;
      sym.scope_start_line = 0;
      sym.scope_end_line = 999999;
      table_.symbols.push_back(std::move(sym));

      const int body_line = stmt_start_line(func->body.get());
      for (const auto &param : func->params) {
        Symbol psym;
        psym.name = param.name;
        psym.kind = SymbolKind::Parameter;
        psym.type_name = param.type.to_string();
        psym.location = func->location;
        psym.scope_start_line = body_line;
        psym.scope_end_line = 999999;
        table_.symbols.push_back(std::move(psym));
      }

      if (func->body) {
        visit_stmt(*func->body, body_line);
      }
    } else if (const auto *struct_decl = dynamic_cast<const ast::StructDecl *>(&decl)) {
      Symbol sym;
      sym.name = struct_decl->name;
      sym.kind = SymbolKind::Struct;
      sym.type_name = "struct";
      sym.location = struct_decl->location;
      sym.scope_start_line = 0;
      sym.scope_end_line = 999999;
      for (const auto &field : struct_decl->fields) {
        sym.fields.push_back(FieldSymbol{field.name, field.type.to_string()});
      }
      table_.symbols.push_back(std::move(sym));
    } else if (const auto *enum_decl = dynamic_cast<const ast::EnumDecl *>(&decl)) {
      Symbol sym;
      sym.name = enum_decl->name;
      sym.kind = SymbolKind::Enum;
      sym.type_name = "enum";
      sym.location = enum_decl->location;
      sym.scope_start_line = 0;
      sym.scope_end_line = 999999;
      sym.variants.clear();
      for (const auto &v : enum_decl->variants) {
        sym.variants.push_back(v.name);
        sym.variant_param_counts.push_back(static_cast<int>(v.param_types.size()));
      }
      table_.symbols.push_back(std::move(sym));
    } else if (const auto *concept_decl = dynamic_cast<const ast::ConceptDecl *>(&decl)) {
      Symbol sym;
      sym.name = concept_decl->name;
      sym.kind = SymbolKind::Concept;
      sym.type_name = "concept";
      sym.location = concept_decl->location;
      sym.scope_start_line = 0;
      sym.scope_end_line = 999999;
      for (const auto &method : concept_decl->methods) {
        sym.concept_methods.push_back(FieldSymbol{method.name, method.return_type.to_string()});
      }
      table_.symbols.push_back(std::move(sym));
    } else if (const auto *top = dynamic_cast<const ast::TopLevelStmtDecl *>(&decl)) {
      visit_stmt(*top->stmt, 0);
    }
  }

  void visit_stmt(const ast::Stmt &stmt, int scope_start) {
    if (const auto *block = dynamic_cast<const ast::BlockStmt *>(&stmt)) {
      const int block_line = block->location.line;
      for (const auto &s : block->statements) {
        if (!s)
          continue;
        visit_stmt(*s, block_line);
      }
    } else if (const auto *var = dynamic_cast<const ast::VarDeclStmt *>(&stmt)) {
      Symbol sym;
      sym.name = var->name;
      sym.kind = SymbolKind::Variable;
      sym.type_name = var->type.to_string();
      sym.location = var->location;
      sym.scope_start_line = var->location.line;
      sym.scope_end_line = 999999;
      table_.symbols.push_back(std::move(sym));
    } else if (const auto *if_s = dynamic_cast<const ast::IfStmt *>(&stmt)) {
      if (if_s->then_branch)
        visit_stmt(*if_s->then_branch, scope_start);
      if (if_s->else_branch)
        visit_stmt(*if_s->else_branch, scope_start);
    } else if (const auto *while_s = dynamic_cast<const ast::WhileStmt *>(&stmt)) {
      if (while_s->body)
        visit_stmt(*while_s->body, scope_start);
    } else if (const auto *for_s = dynamic_cast<const ast::ForStmt *>(&stmt)) {
      if (for_s->init)
        visit_stmt(*for_s->init, scope_start);
      if (for_s->body)
        visit_stmt(*for_s->body, scope_start);
    }
  }

  SymbolTable table_;
};

} // namespace

std::vector<const Symbol *> SymbolTable::visible_at(int line) const {
  std::vector<const Symbol *> result;
  std::set<std::string> seen;
  for (auto it = symbols.rbegin(); it != symbols.rend(); ++it) {
    if (it->scope_start_line <= line && line <= it->scope_end_line) {
      if (seen.insert(it->name).second) {
        result.push_back(&*it);
      }
    }
  }
  return result;
}

const Symbol *SymbolTable::find_definition(const std::string &name, int line) const {
  const Symbol *best = nullptr;
  for (const auto &sym : symbols) {
    if (sym.name == name && sym.scope_start_line <= line) {
      if (!best || sym.location.line > best->location.line) {
        best = &sym;
      }
    }
  }
  return best;
}

AnalysisResult analyze(const std::string &source, const std::string &file_path) {
  AnalysisResult result;

  Scanner scanner(source);
  auto tokens = scanner.scan_tokens();

  bool has_lexer_error = false;
  for (const auto &token : tokens) {
    if (token.type == TokenType::ERROR) {
      result.diagnostics.push_back({token.line, token.column, static_cast<int>(token.lexeme.size()),
                                    std::string(token.lexeme), 1});
      has_lexer_error = true;
    }
  }

  if (has_lexer_error)
    return result;

  Parser parser(tokens);
  auto parse_result = parser.parse();

  for (const auto &err : parse_result.errors) {
    result.diagnostics.push_back({err.line, err.column, 1, err.message, 1});
  }

  // Collect using and import declarations even with parse errors (for completion)
  if (parse_result.program) {
    for (const auto &decl : parse_result.program->declarations) {
      if (const auto *u = dynamic_cast<const ast::UsingDecl *>(decl.get())) {
        result.used_namespaces.insert(u->namespace_name);
        if (u->is_namespace) {
          result.opened_namespaces.insert(u->namespace_name);
        }
      }
      if (const auto *imp = dynamic_cast<const ast::ImportDecl *>(decl.get())) {
        std::string ns =
            imp->alias.empty() ? std::filesystem::path(imp->path).stem().string() : imp->alias;
        result.imported_namespaces.insert(ns);
      }
      if (const auto *imp = dynamic_cast<const ast::LogicalImportDecl *>(decl.get())) {
        // `import math;` — the module id IS the namespace once resolved
        // through kinglet.nest.
        result.imported_namespaces.insert(imp->module_id);
      }
    }
  }

  // Set up ModuleLoader for import resolution (before parse error check so
  // imported_symbols are available even when the file has parse errors)
  std::unique_ptr<ModuleLoader> module_loader;
  if (!file_path.empty()) {
    std::filesystem::path p(file_path);
    // Use an absolute base directory so the kinglet.nest walk-up can climb
    // past the open file's parent (relative paths stop at "." and never reach
    // the project root when the LSP client opens files by absolute URI).
    std::error_code ec;
    std::filesystem::path abs_p = std::filesystem::absolute(p, ec);
    if (ec)
      abs_p = p;
    std::string base_dir = abs_p.has_parent_path() ? abs_p.parent_path().string() : ".";
    module_loader = std::make_unique<ModuleLoader>(base_dir);
    module_loader->discover_project_root(base_dir);
    module_loader->register_source_file(file_path);
  }

  // Collect symbols from imported modules (always, even with parse errors).
  // Kinglet has two import surface forms today and they're distinct AST nodes:
  //   * `import "rel/path.kl";`     -> ast::ImportDecl, may have alias /
  //                                     selected_symbols (the legacy form).
  //   * `import math;`              -> ast::LogicalImportDecl, looked up in
  //                                     kinglet.nest (the manifest form).
  // Both end up populating result.imported_symbols[ns] so namespace access
  // (`math::|`) and qualified hover work regardless of import flavor.
  if (module_loader && parse_result.program) {
    // Populate result.imported_symbols[ns] from a freshly-loaded module.
    // Filters honor selected_symbols when supplied (legacy form only).
    auto ingest = [&](const ParsedModule *mod, const ast::Decl &decl_for_loc, const std::string &ns,
                      const std::vector<std::string> &selected_symbols,
                      const std::string &error_msg = {}) {
      if (!mod) {
        result.diagnostics.push_back(
            {decl_for_loc.location.line, decl_for_loc.location.column, 1, error_msg, 1});
        return;
      }
      const auto &module_ref = *mod;
      auto &syms = result.imported_symbols[ns];
      // Use the real declaration's own location + the module's resolved
      // file path — NOT decl_for_loc (the import statement) — so
      // go-to-definition/hover land on the actual `pub` declaration in the
      // defining file instead of bouncing back to the import line.
      for (const auto *fn : module_ref.public_functions) {
        if (!selected_symbols.empty()) {
          bool found = false;
          for (const auto &s : selected_symbols) {
            if (s == fn->name) {
              found = true;
              break;
            }
          }
          if (!found)
            continue;
        }
        Symbol sym;
        sym.name = fn->name;
        sym.kind = SymbolKind::Function;
        sym.return_type = fn->return_type.to_string();
        sym.params = fn->params;
        sym.location = fn->location;
        sym.file_path = module_ref.resolved_path;
        syms.push_back(std::move(sym));
      }
      for (const auto *sd : module_ref.public_structs) {
        if (!selected_symbols.empty())
          continue;
        Symbol sym;
        sym.name = sd->name;
        sym.kind = SymbolKind::Struct;
        sym.type_name = "struct";
        sym.location = sd->location;
        sym.file_path = module_ref.resolved_path;
        for (const auto &field : sd->fields) {
          sym.fields.push_back(FieldSymbol{field.name, field.type.to_string()});
        }
        syms.push_back(std::move(sym));
      }
      for (const auto *ed : module_ref.public_enums) {
        if (!selected_symbols.empty())
          continue;
        Symbol sym;
        sym.name = ed->name;
        sym.kind = SymbolKind::Enum;
        sym.type_name = "enum";
        sym.location = ed->location;
        sym.file_path = module_ref.resolved_path;
        for (const auto &v : ed->variants) {
          sym.variants.push_back(v.name);
          sym.variant_param_counts.push_back(static_cast<int>(v.param_types.size()));
        }
        syms.push_back(std::move(sym));
      }
    };

    for (const auto &decl : parse_result.program->declarations) {
      if (const auto *imp = dynamic_cast<const ast::ImportDecl *>(decl.get())) {
        auto load_result = module_loader->load(imp->path);
        const std::string ns =
            imp->alias.empty()
                ? (load_result.module ? load_result.module->namespace_name
                                      : std::filesystem::path(imp->path).stem().string())
                : imp->alias;
        ingest(load_result.module, *imp, ns, imp->selected_symbols, load_result.error);
        continue;
      }
      if (const auto *imp = dynamic_cast<const ast::LogicalImportDecl *>(decl.get())) {
        auto resolve_result = module_loader->resolve_logical(imp->module_id);
        if (!resolve_result.error.empty()) {
          result.diagnostics.push_back(
              {imp->location.line, imp->location.column, 1, resolve_result.error, 1});
        }
        for (const auto *mod : resolve_result.modules) {
          ingest(mod, *imp, mod->namespace_name, {});
        }
        continue;
      }
    }
  }

  auto register_imported_symbols = [&result] {
    // Register imported symbols with qualified names for hover / definition
    for (const auto &[ns, syms] : result.imported_symbols) {
      for (auto sym : syms) {
        sym.name = ns + "::" + sym.name;
        sym.scope_start_line = 0;
        sym.scope_end_line = 999999;
        result.symbols.symbols.push_back(std::move(sym));
      }
    }
  };

  auto register_selective_bare_names = [&result](const ast::Program &program) {
    for (const auto &decl : program.declarations) {
      const auto *imp = dynamic_cast<const ast::ImportDecl *>(decl.get());
      if (!imp || imp->selected_symbols.empty())
        continue;
      std::string ns =
          imp->alias.empty() ? std::filesystem::path(imp->path).stem().string() : imp->alias;
      auto it = result.imported_symbols.find(ns);
      if (it == result.imported_symbols.end())
        continue;
      for (auto sym : it->second) {
        sym.scope_start_line = 0;
        sym.scope_end_line = 999999;
        result.symbols.symbols.push_back(std::move(sym));
      }
    }
  };

  if (!parse_result.errors.empty() || !parse_result.program) {
    if (parse_result.program) {
      SymbolCollector collector;
      collector.collect(*parse_result.program);
      result.symbols = collector.take();
      register_imported_symbols();
      register_selective_bare_names(*parse_result.program);
    }
    return result;
  }

  TypeChecker checker;
  if (module_loader)
    checker.set_module_loader(module_loader.get());
  auto type_result = checker.check(*parse_result.program);
  for (const auto &err : type_result.errors) {
    result.diagnostics.push_back({err.location.line, err.location.column, err.location.length,
                                  err.message, static_cast<int>(err.severity)});
  }

  SymbolCollector collector;
  collector.collect(*parse_result.program);
  result.symbols = collector.take();
  register_imported_symbols();
  register_selective_bare_names(*parse_result.program);

  result.program = std::move(parse_result.program);
  result.valid = true;

  return result;
}

} // namespace kinglet::lsp
