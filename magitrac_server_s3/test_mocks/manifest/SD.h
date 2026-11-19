#pragma once
// Host stub of the Arduino SD library for test_manifest.cpp: files and folders
// come from a folder on the host (gMockSdRoot).  Reads, writes and folder walks
// are real; gMockSdHook (if set) runs at every open, byte read and folder-walk
// step, so a test can act at any point in the middle of a sync.
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define FILE_READ  "r"
#define FILE_WRITE "w"

extern std::string gMockSdRoot;
extern void (*gMockSdHook)();

class File {
public:
    FILE*       fp  = nullptr;
    DIR*        dp  = nullptr;
    std::string path;                 // host path
    std::string nm;                   // name within its folder

    explicit operator bool() const { return fp || dp; }
    bool isDirectory() const { return dp != nullptr; }
    const char* name() const { return nm.c_str(); }
    size_t size() {
        if (!fp) return 0;
        long c = ftell(fp); fseek(fp, 0, SEEK_END); long s = ftell(fp); fseek(fp, c, SEEK_SET);
        return (size_t)s;
    }
    int available() {
        if (!fp) return 0;
        int c = fgetc(fp); if (c == EOF) return 0;
        ungetc(c, fp); return 1;
    }
    int  read() { if (gMockSdHook) gMockSdHook(); return fp ? fgetc(fp) : -1; }
    void print(const char* s) { if (fp) fputs(s, fp); }
    void printf(const char* fmt, ...) {
        if (!fp) return;
        va_list a; va_start(a, fmt); vfprintf(fp, fmt, a); va_end(a);
    }
    File openNextFile() {
        File f;
        if (!dp) return f;
        if (gMockSdHook) gMockSdHook();
        for (struct dirent* e; (e = readdir(dp)); ) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            f.path = path + "/" + e->d_name;
            f.nm   = e->d_name;
            struct stat st;
            if (stat(f.path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) f.dp = opendir(f.path.c_str());
            else                                                       f.fp = fopen(f.path.c_str(), "rb");
            return f;
        }
        return f;
    }
    void close() {
        if (fp) { fclose(fp); fp = nullptr; }
        if (dp) { closedir(dp); dp = nullptr; }
    }
};

struct SDClass {
    File open(const char* p, const char* mode = FILE_READ) {
        if (gMockSdHook) gMockSdHook();
        File f;
        f.path = gMockSdRoot + p;
        const char* slash = strrchr(p, '/');
        f.nm = slash ? slash + 1 : p;
        struct stat st;
        if (mode[0] == 'r' && stat(f.path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            f.dp = opendir(f.path.c_str());
        else
            f.fp = fopen(f.path.c_str(), mode[0] == 'w' ? "wb" : "rb");
        return f;
    }
    bool exists(const char* p) { struct stat st; return stat((gMockSdRoot + p).c_str(), &st) == 0; }
    bool remove(const char* p) { return ::remove((gMockSdRoot + p).c_str()) == 0; }
};
extern SDClass SD;
