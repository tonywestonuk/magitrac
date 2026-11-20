#pragma once
#include <string>
#include <string.h>
#define FILE_READ "r"
extern std::string gMockKeyFile;
extern bool        gMockKeyFileExists;
class File {
public:
    File(bool ok = false) : _ok(ok) {}
    operator bool() const { return _ok; }
    int available() { return _ok && _pos < gMockKeyFile.size(); }
    size_t readBytesUntil(char t, char* buf, size_t n) {
        size_t k = 0;
        while (_pos < gMockKeyFile.size() && k < n) {
            char c = gMockKeyFile[_pos++];
            if (c == t) break;
            buf[k++] = c;
        }
        return k;
    }
    void close() {}
private:
    bool   _ok;
    size_t _pos = 0;
};
struct MockSD { File open(const char*, const char*) { return File(gMockKeyFileExists); } };
static MockSD SD;
