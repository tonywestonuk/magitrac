#include "MagiUdpLink.h"
#include <Arduino.h>

// UDP-routed messages are tiny (3-byte SeqPos, 2-byte PreviewRow, 3-byte
// MidiNoteIn); 256 B is plenty of headroom without putting a big buffer
// on the stack.
static const size_t MAX_UDP_PACKET = 256;

bool MagiUdpLink::beginListener(uint16_t port) {
    _isListener = true;
    bool ok = _udp.begin(port);
    Serial.printf("[UDP] listener on port %u: %s\n", (unsigned)port,
                  ok ? "OK" : "FAIL");
    return ok;
}

void MagiUdpLink::beginSender(const uint8_t dest_ip[4], uint16_t port) {
    _isListener = false;
    _destIp = IPAddress(dest_ip[0], dest_ip[1], dest_ip[2], dest_ip[3]);
    _destPort = port;
    // Don't bind a local socket here — STA may not be associated yet.
    // Deferred to first send().
    Serial.printf("[UDP] sender → %u.%u.%u.%u:%u (lazy bind)\n",
                  dest_ip[0], dest_ip[1], dest_ip[2], dest_ip[3],
                  (unsigned)port);
}

bool MagiUdpLink::send(const void* data, size_t len) {
    if (_destPort == 0)            return false;
    if (len == 0 || len > MAX_UDP_PACKET) return false;
    if (!_senderBound) {
        _senderBound = _udp.begin(0);   // ephemeral local port; fails if no iface
        if (!_senderBound) return false;
    }
    _udp.beginPacket(_destIp, _destPort);
    size_t w = _udp.write((const uint8_t*)data, len);
    bool ok = _udp.endPacket() && (w == len);
    return ok;
}

void MagiUdpLink::poll() {
    if (!_isListener) return;
    int pktSize = _udp.parsePacket();
    if (pktSize <= 0) return;
    uint8_t buf[MAX_UDP_PACKET];
    int toRead = pktSize > (int)sizeof(buf) ? (int)sizeof(buf) : pktSize;
    int n = _udp.read(buf, toRead);
    // Drop oversized packets entirely — we never expect them and partial
    // delivery would mis-frame the message dispatch.
    if (n != pktSize) return;
    if (_cb) _cb(buf, n);
}
