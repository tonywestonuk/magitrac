// MagiLink.h — persistent TCP link, the magitrac reliable data path.
//
// Topology-agnostic: one side accepts, the other connects.  Either side
// can host the AP, join the AP, or join an external AP — MagiLink waits
// for *any* local IP (softAP or STA) before starting work.  Each side
// spawns one FreeRTOS task that owns the connection: accept (or
// connect), wait until the peer drops, loop back.
//
// Listens on MAGI_PORT.  Per-message dispatch is via registered
// callbacks (registerCallback); transactional flows acquire the
// mutex, send/read, then release.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <IPAddress.h>

class MagiLink {
public:
    enum class Role : uint8_t { Unconfigured, Accept, Connect };

    // Accept role: opens a WiFiServer on `port` and accepts the next
    // client.  Spawns the worker task immediately; the worker waits
    // until a local IP is available (softAP or STA) before listening.
    void beginAccept(uint16_t port);

    // Connect role: connects to `peer:port` and stays connected.
    // Spawns the worker task immediately; the worker waits for a local
    // STA IP before attempting connect.
    void beginConnect(uint16_t port, IPAddress peer);

    // True between "connection established" and "connection dropped".
    // Updated from the worker task; safe to read from any task.
    bool isConnected() const { return _connected; }

    // Monotonic counter, bumped every time a new connection is established —
    // including a preempt-swap to a fresh peer.  A session task can compare
    // this against the value it last handled to detect "this is a new
    // connection" robustly, even if it polls too slowly to observe the
    // isConnected() false→true edge during a preempt.
    uint32_t generation() const { return _generation; }

    // ── Transactional API ────────────────────────────────────────────────────
    //
    // Every message is a fixed-size struct beginning with {uint8_t id,
    // uint16_t length}.  The wire carries the struct bytes directly — no
    // extra framing prefix.  length includes the id+length bytes themselves
    // (i.e. == sizeof(MsgFoo)).
    //
    // Mutex model:
    //   - send() / read() / registered-callback dispatch all take a single
    //     shared mutex.  If the calling task already holds it, the
    //     take is skipped.  Mutex stays held until releaseMutex().
    //   - The worker task takes the mutex briefly each iteration to read
    //     incoming messages and run registered callbacks; releases and
    //     yields between iterations so transactional tasks can grab it.
    //   - A transactional task calls: acquireMutex (optional — first send
    //     or read takes it) → send → read → … → releaseMutex.
    //
    // read() blocks indefinitely.  If the socket dies it returns nullptr
    // (caller must releaseMutex and handle reconnect via isConnected()).

    using MessageCb = void (*)(const uint8_t* msg, size_t len, void* ctx);

    // Acquire / release the transaction mutex explicitly.  send() and read()
    // also take it if not already held — explicit acquire is mostly for
    // symmetry / readability at call sites.
    void acquireMutex();
    void releaseMutex();

    // Send one message.  Returns false if not connected or write failed.
    // Mutex stays held — caller must releaseMutex().
    bool send(const void* msg, size_t len);

    // Block until one message arrives.  Returns a pointer into an internal
    // buffer — caller may cast to a message struct after checking buf[0]
    // (the id).  Pointer is valid until the next send()/read()/releaseMutex().
    // Returns nullptr on disconnect.
    const uint8_t* read();

    // Register a permanent handler for messages of a single type, called
    // from the worker task when a matching message arrives.  Replaces any
    // existing registration for that type.  Returns false if the table is
    // full.
    bool registerCallback(uint8_t msgType, MessageCb cb, void* ctx = nullptr);

private:
    Role        _role     = Role::Unconfigured;
    uint16_t    _port     = 0;
    IPAddress   _peer;             // Connect role only

    // Opaque impl state so we don't drag WiFi.h into the public header.
    struct Impl;
    Impl*       _impl     = nullptr;

    // Set by the worker on connect/disconnect; polled by other tasks.
    volatile bool     _connected  = false;
    volatile uint32_t _generation = 0;   // ++ on each new established link

    // Registration table for per-id callbacks.  Worker task dispatches
    // matching incoming messages here.  Server registers ~28 entries
    // (full command set + the two streaming HEADER/BODY pairs); 48
    // leaves headroom for future additions without another overflow.
    struct RegisteredHandler { uint8_t msgType; MessageCb cb; void* ctx; };
    static const int MAX_REGISTERED = 48;
    RegisteredHandler _registered[MAX_REGISTERED];
    int               _registeredCount = 0;

    // Buffer that read() returns a pointer into.  Single-slot — safe
    // because the mutex serialises all access.
    static const size_t RECV_BUF_BYTES = 1029;
    uint8_t  _recvBuf[RECV_BUF_BYTES] = {};

    static void _trampoline(void* arg);
    void _runAcceptLoop();
    void _runConnectLoop();

    // Internal helpers (mutex-aware; caller must hold the mutex).
    bool _readFully(uint8_t* dst, size_t n);   // blocking read of exactly n bytes
    bool _readMessageRaw();                     // fills _recvBuf, NO dispatch
    void _dispatch(const uint8_t* msg, size_t len);  // call registered handler if any
};

// One global instance per binary — created in MagiLink.cpp.
extern MagiLink gMagiLink;
