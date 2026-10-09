// Host-native unit tests for EspNowResultPager. Build/run with
// tests_native/run.sh.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../firmware/newhorizons_os/EspNowResultPager.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

using nhos::EspNowResultPager;

struct Page {
  std::string json;
  std::string slice;  // the unescaped "data" value
};

// Reverses the escaping writePage() applies, for round-trip checks.
std::string unescape(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '\\') {
      out += in[i];
      continue;
    }
    const char c = in[++i];
    switch (c) {
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': out += static_cast<char>(std::stoi(in.substr(i + 1, 4), nullptr, 16)); i += 4; break;
      default: out += c;
    }
  }
  return out;
}

std::vector<Page> pages(const std::string& text, uint16_t rid = 1) {
  std::vector<uint32_t> starts(nhos::kEspNowResultMaxPages);
  const size_t count = EspNowResultPager::paginate(text.data(), text.size(),
                                                   nhos::kEspNowResultPageDataBudget,
                                                   starts.data(), starts.size());
  std::vector<Page> out;
  for (size_t i = 0; i < count; ++i) {
    const size_t start = starts[i];
    const size_t end = i + 1 < count ? starts[i + 1] : text.size();
    const size_t len = EspNowResultPager::pageLength(text.data(), start, end, rid, i, count);
    std::string buf(len, '\0');
    CHECK(EspNowResultPager::writePage(text.data(), start, end, rid, i, count, &buf[0], len) == len);
    const std::string marker = "\"data\":\"";
    const size_t dataAt = buf.find(marker) + marker.size();
    out.push_back({buf, unescape(buf.substr(dataAt, buf.size() - dataAt - 2))});
  }
  return out;
}

std::string join(const std::vector<Page>& ps) {
  std::string all;
  for (const Page& p : ps) all += p.slice;
  return all;
}

void testShortTextIsOnePage() {
  const std::string text = "{\"ok\":true,\"cmd\":\"status\"}";
  const auto ps = pages(text, 7);
  CHECK(ps.size() == 1);
  CHECK(ps[0].json ==
        "{\"type\":\"result_chunk\",\"rid\":7,\"chunk\":0,\"chunks\":1,"
        "\"data\":\"{\\\"ok\\\":true,\\\"cmd\\\":\\\"status\\\"}\"}");
  CHECK(join(ps) == text);
}

void testWorstCaseEscapingStaysInsideAFrame() {
  // Every byte doubles when escaped.
  const std::string text(30000, '"');
  const auto ps = pages(text);
  CHECK(ps.size() >= 8);
  for (const Page& p : ps) CHECK(p.json.size() <= nhos::kEspNowMaxFrameBytes);
  CHECK(join(ps) == text);
}

void testCalibrationDumpSizedReply() {
  // A V2.3.D level dump with saved and draft layers: 225 cells x 2 at ~74 B.
  std::string text = "{\"ok\":true,\"cmd\":\"calibration_dump_level\",\"data\":{\"saved\":{\"cells\":[";
  for (int layer = 0; layer < 2; ++layer) {
    for (int i = 0; i < 225; ++i) {
      char cell[96];
      std::snprintf(cell, sizeof(cell),
                    "{\"sensor_index\":%d,\"row\":%d,\"col\":%d,\"calibrated\":true,\"value\":-1234.567},",
                    i, i % 15, i / 15);
      text += cell;
    }
  }
  text += "{}]}}}";
  CHECK(text.size() > 30000);
  const auto ps = pages(text);
  CHECK(ps.size() >= 5 && ps.size() <= 7);
  for (const Page& p : ps) CHECK(p.json.size() <= nhos::kEspNowMaxFrameBytes);
  CHECK(join(ps) == text);
}

void testPagesNeverSplitUtf8() {
  // "校" is 3 bytes; shift it across every possible boundary offset.
  for (size_t pad = 0; pad < 3; ++pad) {
    std::string text(pad, 'a');
    while (text.size() < 20000) text += "\xE6\xA0\xA1";
    std::vector<uint32_t> starts(nhos::kEspNowResultMaxPages);
    const size_t count = EspNowResultPager::paginate(text.data(), text.size(),
                                                     nhos::kEspNowResultPageDataBudget,
                                                     starts.data(), starts.size());
    CHECK(count >= 3);
    for (size_t i = 0; i < count; ++i) {
      CHECK((static_cast<unsigned char>(text[starts[i]]) & 0xC0) != 0x80);
    }
    CHECK(join(pages(text)) == text);
  }
}

void testControlBytesAreEscaped() {
  std::string text = "a\nb\tc";
  text += '\x01';
  const auto ps = pages(text);
  CHECK(ps.size() == 1);
  CHECK(ps[0].json.find("a\\nb\\tc\\u0001") != std::string::npos);
  CHECK(join(ps) == text);
}

void testTooManyPagesIsRefused() {
  const std::string text(nhos::kEspNowResultPageDataBudget * (nhos::kEspNowResultMaxPages + 1), 'x');
  std::vector<uint32_t> starts(nhos::kEspNowResultMaxPages);
  CHECK(EspNowResultPager::paginate(text.data(), text.size(), nhos::kEspNowResultPageDataBudget,
                                    starts.data(), starts.size()) == 0);
}

void testWritePageRefusesSmallBuffer() {
  const std::string text = "hello";
  char buf[16];
  CHECK(EspNowResultPager::writePage(text.data(), 0, text.size(), 1, 0, 1, buf, sizeof(buf)) == 0);
}

}  // namespace

int main() {
  testShortTextIsOnePage();
  testWorstCaseEscapingStaysInsideAFrame();
  testCalibrationDumpSizedReply();
  testPagesNeverSplitUtf8();
  testControlBytesAreEscaped();
  testTooManyPagesIsRefused();
  testWritePageRefusesSmallBuffer();
  if (g_failures) {
    std::fprintf(stderr, "test_esp_now_result_pager: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_esp_now_result_pager: OK\n");
  return 0;
}
