#include "JsonUtils.h"

#include <stdlib.h>
#include <string.h>

namespace nhos {
namespace {

int skipWhitespace(const String& source, int index) {
  while (index < source.length()) {
    const char c = source.charAt(index);
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
      break;
    }
    ++index;
  }
  return index;
}

bool appendDecodedEscape(String& out, const String& source, int& index) {
  if (index >= source.length()) {
    return false;
  }
  const char c = source.charAt(index);
  switch (c) {
    case '"':
    case '\\':
    case '/':
      out += c;
      return true;
    case 'b':
      out += '\b';
      return true;
    case 'f':
      out += '\f';
      return true;
    case 'n':
      out += '\n';
      return true;
    case 'r':
      out += '\r';
      return true;
    case 't':
      out += '\t';
      return true;
    case 'u':
      if (index + 4 >= source.length()) {
        return false;
      }
      index += 4;
      out += '?';
      return true;
    default:
      out += c;
      return true;
  }
}

bool parseJsonStringToken(const String& source, int start, String& out, int& nextIndex) {
  if (start < 0 || start >= source.length() || source.charAt(start) != '"') {
    return false;
  }
  out = "";
  out.reserve(source.length() - start);
  for (int i = start + 1; i < source.length(); ++i) {
    const char c = source.charAt(i);
    if (c == '\\') {
      ++i;
      if (!appendDecodedEscape(out, source, i)) {
        return false;
      }
      continue;
    }
    if (c == '"') {
      nextIndex = i + 1;
      return true;
    }
    out += c;
  }
  return false;
}

bool captureCompositeValue(const String& source, int start, char openChar, char closeChar, String& out) {
  if (start < 0 || start >= source.length() || source.charAt(start) != openChar) {
    return false;
  }
  int depth = 0;
  bool inString = false;
  bool escape = false;
  for (int i = start; i < source.length(); ++i) {
    const char c = source.charAt(i);
    if (inString) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }
    if (c == '"') {
      inString = true;
      continue;
    }
    if (c == openChar) {
      ++depth;
      continue;
    }
    if (c == closeChar) {
      --depth;
      if (depth == 0) {
        out = source.substring(start, i + 1);
        return true;
      }
    }
  }
  return false;
}

bool extractScalarText(const String& source, const char* key, String& out);

int findKeyColon(const String& source, const char* key) {
  const size_t keyLen = strlen(key);
  if (keyLen == 0) {
    return -1;
  }
  for (int i = 0; i < source.length(); ++i) {
    if (source.charAt(i) != '"') {
      continue;
    }
    String currentKey;
    int nextIndex = i;
    if (!parseJsonStringToken(source, i, currentKey, nextIndex)) {
      return -1;
    }
    if (currentKey == key) {
      const int colon = skipWhitespace(source, nextIndex);
      if (colon < source.length() && source.charAt(colon) == ':') {
        return colon;
      }
    }
    i = nextIndex - 1;
  }
  return -1;
}

bool extractScalarText(const String& source, const char* key, String& out) {
  const int colon = findKeyColon(source, key);
  if (colon < 0) {
    return false;
  }
  int start = skipWhitespace(source, colon + 1);
  if (start >= source.length()) {
    return false;
  }
  int end = start;
  while (end < source.length()) {
    const char c = source.charAt(end);
    if (c == ',' || c == '}' || c == ']') {
      break;
    }
    ++end;
  }
  out = source.substring(start, end);
  out.trim();
  return !out.isEmpty();
}

void jsonFieldPrefix(String& out, const char* key, bool& first) {
  if (!first) {
    out += ',';
  }
  first = false;
  out += '"';
  out += key;
  out += "\":";
}

}  // namespace

String jsonEscape(const String& value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value.charAt(i);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

void jsonStringField(String& out, const char* key, const String& value, bool& first) {
  jsonFieldPrefix(out, key, first);
  out += '"';
  out += jsonEscape(value);
  out += '"';
}

void jsonBoolField(String& out, const char* key, bool value, bool& first) {
  jsonFieldPrefix(out, key, first);
  out += value ? "true" : "false";
}

void jsonUnsignedField(String& out, const char* key, unsigned long value, bool& first) {
  jsonFieldPrefix(out, key, first);
  out += String(value);
}

void jsonSignedField(String& out, const char* key, long value, bool& first) {
  jsonFieldPrefix(out, key, first);
  out += String(value);
}

void jsonRawField(String& out, const char* key, const String& rawJson, bool& first) {
  jsonFieldPrefix(out, key, first);
  out += rawJson;
}

bool jsonExtractString(const String& source, const char* key, String& out) {
  const int colon = findKeyColon(source, key);
  if (colon < 0) {
    return false;
  }
  const int start = skipWhitespace(source, colon + 1);
  int nextIndex = start;
  return parseJsonStringToken(source, start, out, nextIndex);
}

String jsonExtractString(const String& source, const char* key, const String& fallback) {
  String out;
  if (!jsonExtractString(source, key, out)) {
    return fallback;
  }
  return out;
}

bool jsonExtractObject(const String& source, const char* key, String& out) {
  const int colon = findKeyColon(source, key);
  if (colon < 0) {
    return false;
  }
  const int start = skipWhitespace(source, colon + 1);
  return captureCompositeValue(source, start, '{', '}', out);
}

bool jsonExtractBool(const String& source, const char* key, bool& out) {
  String value;
  if (!extractScalarText(source, key, value)) {
    return false;
  }
  if (value == "true") {
    out = true;
    return true;
  }
  if (value == "false") {
    out = false;
    return true;
  }
  return false;
}

bool jsonExtractInt(const String& source, const char* key, long& out) {
  String value;
  if (!extractScalarText(source, key, value)) {
    return false;
  }
  char* end = nullptr;
  const long parsed = strtol(value.c_str(), &end, 10);
  if (!end || *end != '\0') {
    return false;
  }
  out = parsed;
  return true;
}

bool jsonExtractFloat(const String& source, const char* key, float& out) {
  String value;
  if (!extractScalarText(source, key, value)) {
    return false;
  }
  char* end = nullptr;
  const float parsed = strtof(value.c_str(), &end);
  if (!end || *end != '\0') {
    return false;
  }
  out = parsed;
  return true;
}

size_t jsonExtractUInt8Array(const String& source, const char* key, uint8_t* out, size_t maxCount) {
  const int colon = findKeyColon(source, key);
  if (colon < 0) {
    return 0;
  }
  int cursor = skipWhitespace(source, colon + 1);
  if (cursor >= source.length() || source.charAt(cursor) != '[') {
    return 0;
  }
  ++cursor;
  size_t count = 0;
  while (cursor < source.length()) {
    cursor = skipWhitespace(source, cursor);
    if (cursor >= source.length()) {
      return 0;
    }
    if (source.charAt(cursor) == ']') {
      return count;
    }
    int end = cursor;
    while (end < source.length()) {
      const char c = source.charAt(end);
      if (c == ',' || c == ']') {
        break;
      }
      ++end;
    }
    String token = source.substring(cursor, end);
    token.trim();
    if (token.length() && count < maxCount) {
      out[count++] = static_cast<uint8_t>(token.toInt());
    }
    cursor = skipWhitespace(source, end);
    if (cursor >= source.length()) {
      return count;
    }
    if (source.charAt(cursor) == ',') {
      ++cursor;
      continue;
    }
    if (source.charAt(cursor) == ']') {
      return count;
    }
    return count;
  }
  return count;
}

}  // namespace nhos
