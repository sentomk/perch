#include "lsp/json.h"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <sstream>

namespace kinglet::json {

namespace {

void skip_ws(std::string_view input, std::size_t &pos) {
  while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
    ++pos;
  }
}

// Append the UTF-8 encoding of a Unicode code point to `out`. Invalid
// (non-scalar) values are encoded as U+FFFD so we never produce broken UTF-8.
void append_utf8(std::string &out, std::uint32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    cp = 0xFFFD;
  }
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Read exactly four hex digits at `pos` and advance. Returns -1 on failure
// (e.g. malformed input).
int parse_hex4(std::string_view input, std::size_t &pos) {
  if (pos + 4 > input.size())
    return -1;
  int value = 0;
  for (int i = 0; i < 4; ++i) {
    const char c = input[pos + static_cast<std::size_t>(i)];
    int digit;
    if (c >= '0' && c <= '9')
      digit = c - '0';
    else if (c >= 'a' && c <= 'f')
      digit = 10 + (c - 'a');
    else if (c >= 'A' && c <= 'F')
      digit = 10 + (c - 'A');
    else
      return -1;
    value = (value << 4) | digit;
  }
  pos += 4;
  return value;
}

std::string parse_string(std::string_view input, std::size_t &pos) {
  if (pos >= input.size() || input[pos] != '"') {
    return "";
  }
  ++pos; // skip opening "
  std::string result;
  while (pos < input.size() && input[pos] != '"') {
    if (input[pos] == '\\' && pos + 1 < input.size()) {
      ++pos;
      switch (input[pos]) {
      case 'n':
        result += '\n';
        ++pos;
        break;
      case 't':
        result += '\t';
        ++pos;
        break;
      case 'r':
        result += '\r';
        ++pos;
        break;
      case 'b':
        result += '\b';
        ++pos;
        break;
      case 'f':
        result += '\f';
        ++pos;
        break;
      case '/':
        result += '/';
        ++pos;
        break;
      case '\\':
        result += '\\';
        ++pos;
        break;
      case '"':
        result += '"';
        ++pos;
        break;
      case 'u': {
        ++pos; // skip 'u'
        const int hi = parse_hex4(input, pos);
        if (hi < 0)
          break;
        std::uint32_t cp = static_cast<std::uint32_t>(hi);
        // High surrogate? Look for the matching low surrogate.
        if (cp >= 0xD800 && cp <= 0xDBFF && pos + 2 <= input.size() && input[pos] == '\\' &&
            input[pos + 1] == 'u') {
          std::size_t saved = pos;
          pos += 2;
          const int lo = parse_hex4(input, pos);
          if (lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<std::uint32_t>(lo) - 0xDC00);
          } else {
            // Not a valid low surrogate — rewind and emit the high surrogate
            // as a replacement char.
            pos = saved;
          }
        }
        append_utf8(result, cp);
        break;
      }
      default:
        result += input[pos];
        ++pos;
        break;
      }
    } else {
      result += input[pos];
      ++pos;
    }
  }
  if (pos < input.size())
    ++pos; // skip closing "
  return result;
}

Number parse_number(std::string_view input, std::size_t &pos) {
  const std::size_t start = pos;
  if (pos < input.size() && input[pos] == '-')
    ++pos;
  while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos])))
    ++pos;
  if (pos < input.size() && input[pos] == '.') {
    ++pos;
    while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos])))
      ++pos;
  }
  if (pos < input.size() && (input[pos] == 'e' || input[pos] == 'E')) {
    ++pos;
    if (pos < input.size() && (input[pos] == '+' || input[pos] == '-'))
      ++pos;
    while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos])))
      ++pos;
  }
  const auto num_str = input.substr(start, pos - start);
  char *end = nullptr;
  return std::strtod(num_str.data(), &end);
}

Value parse_value(std::string_view input, std::size_t &pos);

Object parse_object(std::string_view input, std::size_t &pos) {
  Object obj;
  if (pos >= input.size() || input[pos] != '{')
    return obj;
  ++pos;
  skip_ws(input, pos);
  if (pos < input.size() && input[pos] == '}') {
    ++pos;
    return obj;
  }
  while (pos < input.size()) {
    skip_ws(input, pos);
    std::string key = parse_string(input, pos);
    skip_ws(input, pos);
    if (pos < input.size() && input[pos] == ':')
      ++pos;
    skip_ws(input, pos);
    obj[key] = parse_value(input, pos);
    skip_ws(input, pos);
    if (pos < input.size() && input[pos] == ',') {
      ++pos;
      continue;
    }
    if (pos < input.size() && input[pos] == '}') {
      ++pos;
      break;
    }
  }
  return obj;
}

Array parse_array(std::string_view input, std::size_t &pos) {
  Array arr;
  if (pos >= input.size() || input[pos] != '[')
    return arr;
  ++pos;
  skip_ws(input, pos);
  if (pos < input.size() && input[pos] == ']') {
    ++pos;
    return arr;
  }
  while (pos < input.size()) {
    skip_ws(input, pos);
    arr.push_back(parse_value(input, pos));
    skip_ws(input, pos);
    if (pos < input.size() && input[pos] == ',') {
      ++pos;
      continue;
    }
    if (pos < input.size() && input[pos] == ']') {
      ++pos;
      break;
    }
  }
  return arr;
}

Value parse_value(std::string_view input, std::size_t &pos) {
  skip_ws(input, pos);
  if (pos >= input.size())
    return Value::null();
  switch (input[pos]) {
  case '{':
    return Value{parse_object(input, pos)};
  case '[':
    return Value{parse_array(input, pos)};
  case '"':
    return Value{parse_string(input, pos)};
  case 't':
    pos += 4;
    return Value{true};
  case 'f':
    pos += 5;
    return Value{false};
  case 'n':
    pos += 4;
    return Value::null();
  default:
    return Value{parse_number(input, pos)};
  }
}

std::string escape_json(const std::string &s) {
  std::string result;
  result.reserve(s.size());
  for (char c : s) {
    switch (c) {
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    default:
      result += c;
      break;
    }
  }
  return result;
}

} // namespace

Value parse(std::string_view input, std::size_t &pos) {
  return parse_value(input, pos);
}

std::string to_string(const Value &val) {
  if (val.is_string()) {
    return "\"" + escape_json(val.as_string()) + "\"";
  }
  if (val.is_bool()) {
    return val.as_bool() ? "true" : "false";
  }
  if (val.is_number()) {
    std::ostringstream oss;
    oss << val.as_number();
    return oss.str();
  }
  if (val.is_object()) {
    std::string result = "{";
    bool first = true;
    for (const auto &[k, v] : val.as_object()) {
      if (!first)
        result += ",";
      first = false;
      result += "\"" + escape_json(k) + "\":" + to_string(v);
    }
    result += "}";
    return result;
  }
  if (val.is_array()) {
    std::string result = "[";
    bool first = true;
    for (const auto &v : val.as_array()) {
      if (!first)
        result += ",";
      first = false;
      result += to_string(v);
    }
    result += "]";
    return result;
  }
  return "null";
}

} // namespace kinglet::json
