#pragma once
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>

typedef unsigned char byte;

uint32_t millis();
static inline void delay(uint32_t /*ms*/) {}

class HardwareSerial {
public:
    void    write(uint8_t) {}
    int     available()    { return 0; }
    uint8_t read()         { return 0; }
    void    println(const char* s = "") { ::printf("%s\n", s); }
    void    printf(const char* fmt, ...) {
        va_list a; va_start(a, fmt); ::vprintf(fmt, a); va_end(a);
    }
};

extern HardwareSerial Serial;
