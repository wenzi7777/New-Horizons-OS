// Checks VersionCompare against the shared vectors in
// spec/fixtures/versions.json, which the Desktop, Gateway and Hub tests read
// too -- so every implementation of the OTA version order agrees.
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "VersionCompare.h"

namespace {

int failures = 0;

int sign(int value) { return value > 0 ? 1 : (value < 0 ? -1 : 0); }

void expect(bool condition, const std::string& what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s path/to/versions.json\n", argv[0]);
    return 2;
  }
  std::ifstream file(argv[1]);
  if (!file) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  const std::string json = buffer.str();

  const std::regex compareRe(
      R"re(\{"a":\s*"([^"]*)",\s*"b":\s*"([^"]*)",\s*"order":\s*(-?\d+),\s*"core":\s*(-?\d+)\})re");
  int compares = 0;
  for (auto it = std::sregex_iterator(json.begin(), json.end(), compareRe);
       it != std::sregex_iterator(); ++it) {
    const std::string a = (*it)[1];
    const std::string b = (*it)[2];
    const int order = std::stoi((*it)[3]);
    const int core = std::stoi((*it)[4]);
    expect(sign(nhos::compareVersions(a.c_str(), b.c_str())) == order,
           "compareVersions(" + a + ", " + b + ") == " + std::to_string(order));
    expect(sign(nhos::compareVersionCores(a.c_str(), b.c_str())) == core,
           "compareVersionCores(" + a + ", " + b + ") == " + std::to_string(core));
    ++compares;
  }

  const std::regex preRe(R"re(\{"version":\s*"([^"]*)",\s*"prerelease":\s*(true|false)\})re");
  int prereleases = 0;
  for (auto it = std::sregex_iterator(json.begin(), json.end(), preRe);
       it != std::sregex_iterator(); ++it) {
    const std::string version = (*it)[1];
    const bool expected = (*it)[2] == "true";
    expect(nhos::isPrereleaseVersion(version.c_str()) == expected,
           "isPrereleaseVersion(" + version + ")");
    ++prereleases;
  }

  expect(compares >= 10, "fixture parsed (compare vectors found)");
  expect(prereleases >= 2, "fixture parsed (prerelease vectors found)");
  // Defensive inputs the fixture cannot express.
  expect(nhos::compareVersions(nullptr, "v0.0.0") == 0, "nullptr reads as v0.0.0");
  expect(nhos::compareVersionCores("v1.6.0", nullptr) > 0, "nullptr min_os is satisfied");

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_version_compare: %d compare + %d prerelease vectors OK\n", compares,
              prereleases);
  return 0;
}
