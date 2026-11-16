#pragma once
// Host stub of the Arduino SD library: files come from a folder on the host
// (gMockSdRoot), and every read / seek / open is counted.
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string>
extern std::string gMockSdRoot;
extern long gMockSdOpens, gMockSdReadBytes, gMockSdMaxRead, gMockSdOpenNow;
class File {
public:
    FILE* fp = nullptr;
    explicit operator bool() const { return fp != nullptr; }
    size_t read(uint8_t* dst, size_t n) {
        if (!fp) return 0;
        size_t g = fread(dst, 1, n, fp);
        gMockSdReadBytes += (long)g; if ((long)n > gMockSdMaxRead) gMockSdMaxRead = (long)n;
        return g;
    }
    size_t readBytes(char* dst, size_t n) { return read((uint8_t*)dst, n); }
    bool   seek(uint32_t pos) { return fp && fseek(fp, pos, SEEK_SET) == 0; }
    size_t size() { if (!fp) return 0; long c = ftell(fp); fseek(fp, 0, SEEK_END); long s = ftell(fp); fseek(fp, c, SEEK_SET); return (size_t)s; }
    void   close() { if (fp) { fclose(fp); fp = nullptr; gMockSdOpenNow--; } }
};
struct SDClass {
    File open(const char* path) {
        File f; f.fp = fopen((gMockSdRoot + path).c_str(), "rb");
        if (f.fp) { gMockSdOpens++; gMockSdOpenNow++; }
        return f;
    }
};
extern SDClass SD;
