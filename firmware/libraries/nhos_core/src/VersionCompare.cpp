#include "VersionCompare.h"

#include <cstddef>

namespace nhos {
namespace {

struct ParsedVersion {
  unsigned long core[3] = {0, 0, 0};
  // Points into the original string: the text after '-', up to '+' or the
  // end. Empty (preLength == 0) for a release.
  const char* pre = nullptr;
  size_t preLength = 0;
};

bool isDigit(char c) { return c >= '0' && c <= '9'; }

ParsedVersion parse(const char* text) {
  ParsedVersion out;
  if (text == nullptr) {
    return out;
  }
  const char* p = text;
  while (*p == ' ') {
    ++p;
  }
  if (*p == 'v' || *p == 'V') {
    ++p;
  }
  for (int part = 0; part < 3; ++part) {
    unsigned long value = 0;
    while (isDigit(*p)) {
      value = value * 10 + static_cast<unsigned long>(*p - '0');
      ++p;
    }
    out.core[part] = value;
    if (*p != '.') {
      break;
    }
    ++p;
  }
  // Anything between the core and '-' (a fourth component, stray text) is
  // skipped rather than misread as part of the prerelease.
  while (*p != '\0' && *p != '-' && *p != '+') {
    ++p;
  }
  if (*p == '-') {
    ++p;
    out.pre = p;
    while (*p != '\0' && *p != '+') {
      ++p;
    }
    out.preLength = static_cast<size_t>(p - out.pre);
  }
  return out;
}

int compareCore(const ParsedVersion& a, const ParsedVersion& b) {
  for (int i = 0; i < 3; ++i) {
    if (a.core[i] != b.core[i]) {
      return a.core[i] > b.core[i] ? 1 : -1;
    }
  }
  return 0;
}

// One dot-separated prerelease identifier: [start, start + length).
int compareIdentifier(const char* a, size_t aLen, const char* b, size_t bLen) {
  bool aNumeric = aLen > 0;
  for (size_t i = 0; i < aLen; ++i) {
    aNumeric = aNumeric && isDigit(a[i]);
  }
  bool bNumeric = bLen > 0;
  for (size_t i = 0; i < bLen; ++i) {
    bNumeric = bNumeric && isDigit(b[i]);
  }
  if (aNumeric && bNumeric) {
    // Leading zeros are not valid semver; tolerate them by skipping.
    while (aLen > 1 && *a == '0') {
      ++a;
      --aLen;
    }
    while (bLen > 1 && *b == '0') {
      ++b;
      --bLen;
    }
    if (aLen != bLen) {
      return aLen > bLen ? 1 : -1;
    }
    for (size_t i = 0; i < aLen; ++i) {
      if (a[i] != b[i]) {
        return a[i] > b[i] ? 1 : -1;
      }
    }
    return 0;
  }
  if (aNumeric != bNumeric) {
    return aNumeric ? -1 : 1;  // numeric identifiers sort first
  }
  const size_t n = aLen < bLen ? aLen : bLen;
  for (size_t i = 0; i < n; ++i) {
    if (a[i] != b[i]) {
      return static_cast<unsigned char>(a[i]) > static_cast<unsigned char>(b[i]) ? 1 : -1;
    }
  }
  if (aLen != bLen) {
    return aLen > bLen ? 1 : -1;
  }
  return 0;
}

int comparePrerelease(const ParsedVersion& a, const ParsedVersion& b) {
  if (a.preLength == 0 || b.preLength == 0) {
    if (a.preLength == b.preLength) {
      return 0;
    }
    return a.preLength == 0 ? 1 : -1;  // a release outranks its prereleases
  }
  size_t ai = 0;
  size_t bi = 0;
  while (ai < a.preLength && bi < b.preLength) {
    size_t aEnd = ai;
    while (aEnd < a.preLength && a.pre[aEnd] != '.') {
      ++aEnd;
    }
    size_t bEnd = bi;
    while (bEnd < b.preLength && b.pre[bEnd] != '.') {
      ++bEnd;
    }
    const int c = compareIdentifier(a.pre + ai, aEnd - ai, b.pre + bi, bEnd - bi);
    if (c != 0) {
      return c;
    }
    ai = aEnd + 1;
    bi = bEnd + 1;
  }
  const bool aDone = ai >= a.preLength;
  const bool bDone = bi >= b.preLength;
  if (aDone && bDone) {
    return 0;
  }
  return aDone ? -1 : 1;  // the shorter identifier list sorts first
}

}  // namespace

int compareVersions(const char* a, const char* b) {
  const ParsedVersion pa = parse(a);
  const ParsedVersion pb = parse(b);
  const int core = compareCore(pa, pb);
  if (core != 0) {
    return core;
  }
  return comparePrerelease(pa, pb);
}

int compareVersionCores(const char* a, const char* b) {
  return compareCore(parse(a), parse(b));
}

bool isPrereleaseVersion(const char* version) {
  return parse(version).preLength > 0;
}

}  // namespace nhos
