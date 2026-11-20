#pragma once
#include <string>
#define WIFI_STA      1
#define WL_CONNECTED  3
extern int  gMockWifiMode, gMockWifiStatus;
extern bool gMockDnsOk;
struct IPAddress {
    std::string s = "0.0.0.0";
    std::string toString() const { return s; }
};
struct MockWiFi {
    int getMode() { return gMockWifiMode; }
    int status()  { return gMockWifiStatus; }
    int hostByName(const char*, IPAddress& ip) { if (gMockDnsOk) ip.s = "160.79.104.10"; return gMockDnsOk; }
    IPAddress dnsIP() { IPAddress a; a.s = "192.168.178.1"; return a; }
};
static MockWiFi WiFi;
