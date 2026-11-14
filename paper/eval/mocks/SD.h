// Host mock of the Arduino SD `File` — just enough for SongMigration.h.
#pragma once
#include <cstdio>
#include <cstdint>
#include <cstddef>
class File {
public:
    FILE* fp = nullptr;
    int    read(uint8_t* dst, size_t n)        { return fp ? (int)fread(dst, 1, n, fp) : 0; }
    size_t write(const uint8_t* src, size_t n) { return fp ? fwrite(src, 1, n, fp) : 0; }
    explicit operator bool() const { return fp != nullptr; }
};
