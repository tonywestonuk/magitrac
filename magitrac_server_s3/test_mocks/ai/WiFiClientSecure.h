#pragma once
// The fake network: what the client writes goes to gMockSent; reads come from
// gMockReply, handed out in small pieces.
#include <string>
#include <string.h>
#include <stdint.h>
extern std::string gMockSent, gMockReply;
extern bool        gMockConnectOk;
extern std::string gMockCa;
class WiFiClientSecure {
public:
    void setCACert(const char* ca)        { gMockCa = ca; }
    void setHandshakeTimeout(unsigned long) {}
    int  connect(const char*, uint16_t, int32_t) { _pos = 0; _open = gMockConnectOk; return _open; }
    size_t write(const uint8_t* p, size_t n) { gMockSent.append((const char*)p, n); return n; }
    int  available() { return _open ? (int)(gMockReply.size() - _pos > 7 ? 7 : gMockReply.size() - _pos) : 0; }
    int  read(uint8_t* b, size_t n) {
        size_t k = gMockReply.size() - _pos; if (k > n) k = n;
        memcpy(b, gMockReply.data() + _pos, k); _pos += k; return (int)k;
    }
    uint8_t connected() { return _open && _pos < gMockReply.size(); }
    void stop() { _open = false; }
    int  lastError(char* b, size_t n) { if (n) b[0] = 0; return gMockConnectOk ? 0 : -0x7780; }
private:
    size_t _pos = 0;
    bool   _open = false;
};
