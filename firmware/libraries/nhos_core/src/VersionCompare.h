#pragma once

// Version ordering for OTA and package compatibility. Plain C++ (no
// Arduino), so the host-native tests exercise exactly this code; the Hub
// keeps a byte-identical copy (checked by its tests_native).
//
// Versions look like `vMAJOR.MINOR.PATCH` with an optional semver
// prerelease, `v1.14.0-beta.2`. A prerelease sorts BELOW its release:
//
//   v1.14.0-beta.1 < v1.14.0-beta.2 < v1.14.0 < v1.14.1-beta.1
//
// That ordering is what lets the beta channel work at all. The old
// sscanf("v%d.%d.%d") comparison read v1.14.0-beta.1 as v1.14.0, so a
// device running the beta would have been told the real v1.14.0 was "not
// newer" and stayed on the beta forever.
//
// The leading 'v' is optional on either side. Prerelease identifiers are
// compared the semver way: dot-separated, numeric ones numerically, a
// numeric one below an alphanumeric one, and a shorter list below a longer
// one it is a prefix of (beta < beta.1). Build metadata (`+...`) is ignored.

namespace nhos {

// <0 if a is older than b, 0 if equal, >0 if a is newer.
int compareVersions(const char* a, const char* b);

// Like compareVersions, but ignoring any prerelease: v1.14.0-beta.1 and
// v1.14.0 compare equal. For "does this firmware have the features of
// release X" questions (an app's min_os), where a beta of X is the
// candidate for X and already carries them.
int compareVersionCores(const char* a, const char* b);

// True if `version` carries a prerelease suffix (v1.14.0-beta.1).
bool isPrereleaseVersion(const char* version);

}  // namespace nhos
