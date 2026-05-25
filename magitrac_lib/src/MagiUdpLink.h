// MagiUdpLink — best-effort UDP datagram link.
//
// Companion to MagiCommsTcp for messages that are latency-sensitive but
// loss-tolerant — e.g. sequencer row position, preview playhead.  Using
// UDP avoids head-of-line blocking: a stale "row 23" sitting in TCP's
// retransmit queue can't delay a fresh "row 25" anymore.
//
// One-way by default (server → magitrac is the only direction wired up
// today).  Same port number as TCP — TCP and UDP ports are independent
// namespaces in the IP stack so they coexist without conflict.
//
// Listener (magitrac AP side): bind a UDP socket and poll for incoming
// datagrams from the main loop.  Each datagram is a complete message
// (UDP preserves boundaries — no length-prefix framing needed).
//
// Sender (magitrac_server STA side): point at the AP's IP and fire
// datagrams.  No ACK, no retry.  If the send buffer is full, the
// datagram is dropped on the floor.

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <WiFiUdp.h>
#include <IPAddress.h>

class MagiUdpLink {
public:
    using RecvCb = void (*)(const uint8_t* data, int len);

    // Bind a UDP socket to `port` on the local AP iface.  Call after WiFi
    // is up.  Returns true on success.  Sender role calls `beginSender`
    // instead.
    bool beginListener(uint16_t port);

    // Configure remote endpoint for outgoing datagrams.  Local port is
    // ephemeral (assigned by the kernel).  Call after WiFi STA is associated.
    void beginSender(const uint8_t dest_ip[4], uint16_t port);

    // Fire one datagram.  Best-effort — returns false if the kernel send
    // buffer was full or the link is down, but never blocks.
    bool send(const void* data, size_t len);

    void setOnReceive(RecvCb cb) { _cb = cb; }

    // Drain any pending incoming packets.  Call regularly from the main
    // loop (listener role only — no-op for sender).
    void poll();

private:
    WiFiUDP   _udp;
    IPAddress _destIp;
    uint16_t  _destPort   = 0;
    bool      _isListener = false;
    bool      _senderBound = false;
    RecvCb    _cb         = nullptr;
};
