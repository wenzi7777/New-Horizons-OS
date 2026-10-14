// UpdateChannel.h: which manifest each channel resolves to, and that
// anything unrecognised falls back to stable.
#include <cstdio>
#include <cstring>

#include "UpdateChannel.h"

namespace {
int failures = 0;
void expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}
}  // namespace

int main() {
  expect(nhos::isValidUpdateChannel("stable"), "stable is valid");
  expect(nhos::isValidUpdateChannel("beta"), "beta is valid");
  expect(!nhos::isValidUpdateChannel("Beta"), "channels are case-sensitive");
  expect(!nhos::isValidUpdateChannel(""), "empty is not a channel");
  expect(!nhos::isValidUpdateChannel(nullptr), "nullptr is not a channel");
  expect(std::strcmp(nhos::updateChannelManifestSuffix("stable"), "-latest.json") == 0,
         "stable reads -latest.json");
  expect(std::strcmp(nhos::updateChannelManifestSuffix("beta"), "-beta.json") == 0,
         "beta reads -beta.json");
  expect(std::strcmp(nhos::updateChannelManifestSuffix("nightly"), "-latest.json") == 0,
         "an unknown channel never lands on prereleases");
  expect(std::strcmp(nhos::updateChannelManifestSuffix(nullptr), "-latest.json") == 0,
         "nullptr falls back to stable");
  if (failures != 0) {
    return 1;
  }
  std::printf("test_update_channel: OK\n");
  return 0;
}
