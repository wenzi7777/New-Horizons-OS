#include "EspNowResultPager.h"

#include <cstdio>
#include <cstring>

namespace nhos {

namespace {

bool isUtf8Continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

// The envelope up to and including the opening quote of "data".
int writeHead(char* out, size_t capacity, uint16_t rid, size_t index, size_t count) {
  return std::snprintf(out, capacity,
                       "{\"type\":\"result_chunk\",\"rid\":%u,\"chunk\":%u,\"chunks\":%u,\"data\":\"",
                       static_cast<unsigned>(rid), static_cast<unsigned>(index),
                       static_cast<unsigned>(count));
}

}  // namespace

size_t EspNowResultPager::escapedLen(unsigned char c) {
  switch (c) {
    case '"':
    case '\\':
    case '\b':
    case '\f':
    case '\n':
    case '\r':
    case '\t':
      return 2;
    default:
      return c < 0x20 ? 6 : 1;  // other control bytes become \u00XX
  }
}

size_t EspNowResultPager::paginate(const char* text, size_t len, size_t budget,
                                   uint32_t* starts, size_t maxPages) {
  if (text == nullptr || len == 0 || starts == nullptr || maxPages == 0 || budget < 8) {
    return 0;
  }
  size_t count = 0;
  size_t pos = 0;
  while (pos < len) {
    if (count >= maxPages) return 0;
    starts[count++] = static_cast<uint32_t>(pos);
    size_t used = 0;
    size_t end = pos;
    while (end < len) {
      const size_t cost = escapedLen(static_cast<unsigned char>(text[end]));
      if (used + cost > budget) break;
      used += cost;
      ++end;
    }
    // Back off so the page does not end inside a UTF-8 sequence: the next
    // page must not start on a continuation byte.
    while (end < len && end > pos && isUtf8Continuation(static_cast<unsigned char>(text[end]))) {
      --end;
    }
    if (end == pos) return 0;  // budget cannot hold even one character
    pos = end;
  }
  return count;
}

size_t EspNowResultPager::pageLength(const char* text, size_t start, size_t end,
                                     uint16_t rid, size_t index, size_t count) {
  char head[96];
  const int headLen = writeHead(head, sizeof(head), rid, index, count);
  if (headLen <= 0) return 0;
  size_t total = static_cast<size_t>(headLen) + 2;  // closing quote and brace
  for (size_t i = start; i < end; ++i) {
    total += escapedLen(static_cast<unsigned char>(text[i]));
  }
  return total;
}

size_t EspNowResultPager::writePage(const char* text, size_t start, size_t end,
                                    uint16_t rid, size_t index, size_t count,
                                    char* out, size_t capacity) {
  if (text == nullptr || out == nullptr || start > end) return 0;
  if (pageLength(text, start, end, rid, index, count) > capacity) return 0;

  static const char kHex[] = "0123456789abcdef";
  char head[96];
  const int headLen = writeHead(head, sizeof(head), rid, index, count);
  std::memcpy(out, head, static_cast<size_t>(headLen));
  size_t n = static_cast<size_t>(headLen);
  for (size_t i = start; i < end; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    switch (c) {
      case '"': out[n++] = '\\'; out[n++] = '"'; break;
      case '\\': out[n++] = '\\'; out[n++] = '\\'; break;
      case '\b': out[n++] = '\\'; out[n++] = 'b'; break;
      case '\f': out[n++] = '\\'; out[n++] = 'f'; break;
      case '\n': out[n++] = '\\'; out[n++] = 'n'; break;
      case '\r': out[n++] = '\\'; out[n++] = 'r'; break;
      case '\t': out[n++] = '\\'; out[n++] = 't'; break;
      default:
        if (c < 0x20) {
          out[n++] = '\\';
          out[n++] = 'u';
          out[n++] = '0';
          out[n++] = '0';
          out[n++] = kHex[c >> 4];
          out[n++] = kHex[c & 0x0F];
        } else {
          out[n++] = static_cast<char>(c);
        }
    }
  }
  out[n++] = '"';
  out[n++] = '}';
  return n;
}

}  // namespace nhos
