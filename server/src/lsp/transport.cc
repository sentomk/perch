#include "lsp/transport.h"

#include <iostream>
#include <sstream>
#include <string>

namespace kinglet::lsp {

std::string Transport::read_message() {
  long length = -1;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      if (length > 0)
        break;
      continue;
    }
    if (line.rfind("Content-Length:", 0) == 0) {
      try {
        length = std::stol(line.substr(15));
      } catch (...) {
        return "";
      }
      continue;
    }
  }
  if (length <= 0)
    return "";
  std::string content(static_cast<std::size_t>(length), '\0');
  std::cin.read(content.data(), length);
  return content;
}

void Transport::write_message(const json::Value &msg) {
  std::string body = json::to_string(msg);
  std::ostringstream header;
  header << "Content-Length: " << body.size() << "\r\n\r\n";
  std::cout << header.str() << body << std::flush;
}

} // namespace kinglet::lsp
