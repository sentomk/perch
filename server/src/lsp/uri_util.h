#pragma once

#include <cctype>
#include <string>

namespace kinglet::lsp {

// Convert an LSP file URI to a local filesystem path.
inline std::string uri_to_path(std::string uri) {
  const std::string prefix = "file://";
  if (uri.rfind(prefix, 0) == 0) {
    uri = uri.substr(prefix.size());
  }
  // file:///C:/... on Windows → C:/...
  if (uri.size() >= 3 && uri[0] == '/' && std::isalpha(static_cast<unsigned char>(uri[1])) &&
      uri[2] == ':') {
    uri = uri.substr(1);
  }
  return uri;
}

// Convert a local filesystem path to an LSP file URI. Inverse of
// uri_to_path. Used when a Location must point at a file other than the
// one currently open (e.g. a symbol resolved through an import).
inline std::string path_to_uri(const std::string &path) {
  const std::string prefix = "file://";
  // Windows drive-letter paths (C:/... or C:\...) need a leading slash
  // after the scheme: file:///C:/...
  if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') {
    return prefix + "/" + path;
  }
  return prefix + path;
}

} // namespace kinglet::lsp
