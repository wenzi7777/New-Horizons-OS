#pragma once

// Splits a command reply too big for one ESP-NOW control frame into pages.
//
// One control frame tops out at kEspNowMaxFrameBytes (32 fragments x 240 B,
// the ceiling of EspNowReassembler's uint32_t receivedMask_), but replies
// such as a calibration dump (9-33 KB) or the full status (~13 KB) are
// bigger. Rather than trimming each reply to fit, the reply is cached and
// handed out one page at a time: page 0 goes back as the command's own
// reply, and the Hub pulls the rest with {"command":"result_page",...}.
// A page carries a slice of the reply as a JSON string, the same
// "result_chunk" shape ControlServer::sendUdpResult() uses over UDP, so
// one reassembler on the Desktop Backend serves both transports:
//
//   {"type":"result_chunk","rid":R,"chunk":I,"chunks":N,"data":"<slice>"}
//
// `rid` names the cached reply, so a page from a reply that was rebuilt
// meanwhile is never spliced into an older one.
//
// Plain C++ with no Arduino dependency, so tests_native can exercise it.

#include <cstddef>
#include <cstdint>

#include "EspNowFrame.h"

namespace nhos {

// Escaped slice bytes per page: the frame budget minus the page envelope
// (`{"type":"result_chunk","rid":65535,"chunk":63,"chunks":64,"data":""}`
// is 70 B), with some slack.
constexpr size_t kEspNowResultPageDataBudget = kEspNowMaxFrameBytes - 96;

// More pages than this and the reply is refused as response_too_large.
// 64 pages is ~480 KB, far past anything the heap could hold anyway.
constexpr size_t kEspNowResultMaxPages = 64;

class EspNowResultPager {
 public:
  // Fills starts[0..count) with each page's first byte offset into
  // text[0..len) so that every page's JSON-escaped slice is at most
  // `budget` bytes, and returns the page count. A page never ends inside a
  // UTF-8 sequence, so every page decodes on its own. Returns 0 when len is
  // 0, budget is too small to make progress, or more than maxPages pages
  // would be needed.
  static size_t paginate(const char* text, size_t len, size_t budget,
                         uint32_t* starts, size_t maxPages);

  // Bytes `c` takes inside a JSON string.
  static size_t escapedLen(unsigned char c);

  // Writes page `index` of `count` (the slice text[start..end)) into out
  // and returns its length, or 0 if it does not fit in `capacity`. Not
  // NUL-terminated.
  static size_t writePage(const char* text, size_t start, size_t end,
                          uint16_t rid, size_t index, size_t count,
                          char* out, size_t capacity);

  // Exact length writePage() would produce, for sizing its buffer.
  static size_t pageLength(const char* text, size_t start, size_t end,
                           uint16_t rid, size_t index, size_t count);
};

}  // namespace nhos
