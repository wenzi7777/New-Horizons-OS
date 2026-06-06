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
bool jsonExtractBool(const String& source, const char* key, bool& out);
bool jsonExtractInt(const String& source, const char* key, long& out);
bool jsonExtractFloat(const String& source, const char* key, float& out);
size_t jsonExtractUInt8Array(const String& source, const char* key, uint8_t* out, size_t maxCount);

}  // namespace nhos
