#pragma once

#include <Arduino.h>

namespace nhos {

String jsonEscape(const String& value);

void jsonStringField(String& out, const char* key, const String& value, bool& first);
void jsonBoolField(String& out, const char* key, bool value, bool& first);
void jsonUnsignedField(String& out, const char* key, unsigned long value, bool& first);
void jsonSignedField(String& out, const char* key, long value, bool& first);
void jsonRawField(String& out, const char* key, const String& rawJson, bool& first);

bool jsonExtractString(const String& source, const char* key, String& out);
String jsonExtractString(const String& source, const char* key, const String& fallback = "");
bool jsonExtractObject(const String& source, const char* key, String& out);
bool jsonExtractArray(const String& source, const char* key, String& out);
bool jsonExtractBool(const String& source, const char* key, bool& out);
bool jsonExtractInt(const String& source, const char* key, long& out);
bool jsonExtractFloat(const String& source, const char* key, float& out);
size_t jsonExtractUInt8Array(const String& source, const char* key, uint8_t* out, size_t maxCount);
// Every element must parse as a number; returns the element count, or 0 when
// the key is missing, an element is not a number, or there are more than
// maxCount elements (a truncated vector would be silently wrong).
size_t jsonExtractFloatArray(const String& source, const char* key, float* out, size_t maxCount);
// Non-negative integers up to 2^64-1 (epoch milliseconds do not fit a long).
bool jsonExtractUInt64(const String& source, const char* key, uint64_t& out);

}  // namespace nhos
