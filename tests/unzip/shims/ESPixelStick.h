#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

class String : public std::string
{
public:
    using std::string::string;
    using std::string::operator=;
    String() = default;
    String(const std::string& value) : std::string(value) {}
    String(const char* value) : std::string(value ? value : "") {}
    template<class T, typename std::enable_if<std::is_arithmetic<T>::value, int>::type = 0>
    String(T value, unsigned = 10) : std::string(std::to_string(value)) {}
    bool isEmpty() const { return empty(); }
    bool equals(const String& value) const { return *this == value; }
    bool equalsIgnoreCase(String value) const { String copy(*this); copy.toLowerCase(); value.toLowerCase(); return copy == value; }
    bool startsWith(const String& prefix) const { return compare(0, prefix.size(), prefix) == 0; }
    bool endsWith(const String& suffix) const { return size() >= suffix.size() && compare(size() - suffix.size(), suffix.size(), suffix) == 0; }
    int indexOf(const String& value, unsigned offset = 0) const { auto pos = find(value, offset); return pos == npos ? -1 : int(pos); }
    int indexOf(char value, unsigned offset = 0) const { auto pos = find(value, offset); return pos == npos ? -1 : int(pos); }
    int lastIndexOf(char value) const { auto pos = rfind(value); return pos == npos ? -1 : int(pos); }
    int lastIndexOf(const String& value) const { auto pos = rfind(value); return pos == npos ? -1 : int(pos); }
    String substring(unsigned start) const { return substr(std::min<size_t>(start, size())); }
    String substring(unsigned start, unsigned end) const { return substr(std::min<size_t>(start, size()), end - start); }
    void remove(unsigned start, unsigned count = unsigned(-1)) { erase(start, count); }
    void replace(const String& before, const String& after) {
        size_t offset = 0;
        while (!before.empty() && (offset = find(before, offset)) != npos) {
            std::string::replace(offset, before.size(), after);
            offset += after.size();
        }
    }
    void toLowerCase() { std::transform(begin(), end(), begin(), [](unsigned char c) { return char(std::tolower(c)); }); }
    size_t write(uint8_t value) { push_back(char(value)); return 1; }
    size_t write(const uint8_t* data, size_t count) { append(reinterpret_cast<const char*>(data), count); return count; }
};

#include <ArduinoJson.h>
using namespace ArduinoJson;
using std::min;
using std::max;
using byte = uint8_t;
#define F(value) value
#define DEC 10
#define HEX 16
#define DEBUG_START
#define DEBUG_END
#define DEBUG_V(...)
inline const String emptyString;
inline const String CN_stars = "***";
inline uint32_t testClock = 0;
inline bool testRebooting = false;
inline uint32_t millis() { return testClock; }
inline void delay(uint32_t duration) { testClock += duration; }
inline void FeedWDT() {}
inline bool RebootInProgress() { return testRebooting; }
inline void RequestReboot(const String&, uint32_t, bool = false) { testRebooting = true; }
inline void logcon(const String&) {}
inline void JsonWrite(JsonObject& object, const char* name, const String& value) { object[name] = static_cast<const std::string&>(value); }
template<class T> void JsonWrite(JsonObject& object, const char* name, T value) { object[name] = value; }
struct TestSerial {
    template<class T> void println(const T&) {}
    void println() {}
    void flush() {}
};
inline TestSerial LOG_PORT;
struct TestEsp { uint32_t getMaxAllocHeap() const { return 128 * 1024; } };
inline TestEsp ESP;
