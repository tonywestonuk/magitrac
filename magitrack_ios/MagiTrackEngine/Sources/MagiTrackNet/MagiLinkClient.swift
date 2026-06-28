import Foundation
import Network
import MagiTrackCore
import MagiTrackWire

/// TCP MagiLink client. Drop-in replacement for the C++ `MagiLink::beginConnect`
/// path on the LilyGo. The CoreS3 server is unchanged — same `[u8 id][u16 length]
/// LE][payload]` frame format, same per-msg-type callback dispatch.
///
/// Threading: a single serial `DispatchQueue` mediates all socket work, mirroring
/// the FreeRTOS task + mutex model in the C++ client. Public methods are async
/// and hop onto the queue under the hood. Handlers fire on the same queue.
public final class MagiLinkClient: @unchecked Sendable {

    public enum State: Sendable, Equatable {
        case idle
        case connecting
        case connected
        case waiting     // NWConnection thinks the path is unviable; will retry
        case disconnected
        case failed(reason: String)
    }

    public enum ClientError: Error, Equatable {
        case notConnected
        case sendFailed(String)
        case connectionCancelled
    }

    private let host: NWEndpoint.Host
    private let port: NWEndpoint.Port
    private let queue: DispatchQueue
    private var connection: NWConnection?
    private var parser = MagiFrameParser()
    private var state: State = .idle {
        didSet { stateContinuation?.yield(state) }
    }
    private var handlers: [UInt8: @Sendable (Data) -> Void] = [:]
    private var stateContinuation: AsyncStream<State>.Continuation?
    private var pendingConnectContinuation: CheckedContinuation<Void, Error>?
    private var reconnectOnFailure: Bool = false
    private var reconnectDelay: TimeInterval = 1.0

    public init(host: String, port: UInt16 = MagiTrackWire.port) {
        self.host = NWEndpoint.Host(host)
        self.port = NWEndpoint.Port(rawValue: port)!
        self.queue = DispatchQueue(label: "magilink", qos: .userInitiated)
        installSessionAutoAck()
    }

    /// Auto-handle the MagiLink session handshake: server sends MSG_CONNECT
    /// the moment its TCP accept fires; until it sees MSG_CONNECT_ACK back
    /// it treats the session as "not established" and ignores everything
    /// else. Without this the client looks connected but no command gets
    /// processed. Registering at construction time so the handler exists
    /// before the first byte arrives.
    private func installSessionAutoAck() {
        register(.connect) { [weak self] _ in
            guard let self else { return }
            Task { try? await self.send(MsgConnectAck()) }
        }
    }


    // ── Public API ──────────────────────────────────────────────────────

    /// Stream of state transitions. Multiple consumers each get their own
    /// stream (last-write semantics — late subscribers see only future
    /// updates).
    public func stateStream() -> AsyncStream<State> {
        AsyncStream { continuation in
            queue.async {
                self.stateContinuation = continuation
                continuation.yield(self.state)
            }
        }
    }

    /// Establish a connection. Returns once the connection enters `.ready`
    /// (mapped to `.connected`) or throws if it fails. Idempotent — if
    /// already connected, returns immediately.
    public func connect(autoReconnect: Bool = true) async throws {
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            queue.async {
                self.reconnectOnFailure = autoReconnect
                if case .connected = self.state {
                    cont.resume()
                    return
                }
                self.startConnection(initialContinuation: cont)
            }
        }
    }

    /// Cancel the connection and stop any reconnect loop.
    public func disconnect() async {
        await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
            queue.async {
                self.reconnectOnFailure = false
                self.connection?.cancel()
                self.connection = nil
                self.state = .disconnected
                self.parser.reset()
                cont.resume()
            }
        }
    }

    /// Send a typed MagiMessage. Awaits completion of the underlying socket
    /// write. Throws if not connected or the socket reports an error.
    public func send<M: MagiMessage>(_ msg: M) async throws {
        try await sendRaw(msg.encode())
    }

    /// Send a pre-encoded frame. Caller is responsible for framing.
    public func sendRaw(_ frame: Data) async throws {
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            queue.async {
                guard case .connected = self.state, let c = self.connection else {
                    cont.resume(throwing: ClientError.notConnected)
                    return
                }
                c.send(content: frame, completion: .contentProcessed { error in
                    if let error {
                        cont.resume(throwing: ClientError.sendFailed("\(error)"))
                    } else {
                        cont.resume()
                    }
                })
            }
        }
    }

    /// Register a handler for a specific message ID. Replaces any prior
    /// handler for the same ID. Handler is invoked on the client's serial
    /// queue with the raw payload (post-header).
    public func register(_ id: MagiMsgID, _ handler: @escaping @Sendable (Data) -> Void) {
        queue.async { self.handlers[id.rawValue] = handler }
    }

    /// Typed convenience: register a handler that decodes the payload into
    /// `M` and dispatches. Decode errors are silently dropped (matching the
    /// C++ client's "ignore unknown / malformed" behaviour).
    public func register<M: MagiMessage>(_ type: M.Type, _ handler: @escaping @Sendable (M) -> Void) {
        register(M.id) { data in
            guard let msg = try? M(payload: data) else { return }
            handler(msg)
        }
    }

    public func unregister(_ id: MagiMsgID) {
        queue.async { self.handlers.removeValue(forKey: id.rawValue) }
    }

    // ── Internals — queue-isolated ──────────────────────────────────────

    private func startConnection(initialContinuation: CheckedContinuation<Void, Error>?) {
        // Force WiFi only — the server is on a private subnet, cellular fallback
        // would route to the wrong interface.
        let tcpOptions = NWProtocolTCP.Options()
        tcpOptions.connectionTimeout = 5
        tcpOptions.enableKeepalive = true
        tcpOptions.keepaliveIdle = 4
        tcpOptions.keepaliveInterval = 2
        tcpOptions.keepaliveCount = 3
        let params = NWParameters(tls: nil, tcp: tcpOptions)
        // Note: on iOS we set requiredInterfaceType = .wifi to avoid cellular
        // routing to the server's private subnet. Skipped during loopback tests.
        if !host.isLoopback { params.requiredInterfaceType = .wifi }

        parser.reset()
        let c = NWConnection(host: host, port: port, using: params)
        connection = c
        state = .connecting
        pendingConnectContinuation = initialContinuation

        c.stateUpdateHandler = { [weak self] s in
            guard let self else { return }
            // We're already on `queue` because that's the queue we started the connection on.
            self.handleStateChange(s)
        }

        c.start(queue: queue)
        startReceiveLoop(on: c)
    }

    private func handleStateChange(_ s: NWConnection.State) {
        switch s {
        case .ready:
            state = .connected
            if let cont = pendingConnectContinuation {
                pendingConnectContinuation = nil
                cont.resume()
            }
        case .waiting:
            state = .waiting
        case .failed(let err):
            state = .failed(reason: "\(err)")
            if let cont = pendingConnectContinuation {
                pendingConnectContinuation = nil
                cont.resume(throwing: err)
            }
            handleDrop()
        case .cancelled:
            if state != .disconnected { state = .disconnected }
            if let cont = pendingConnectContinuation {
                pendingConnectContinuation = nil
                cont.resume(throwing: ClientError.connectionCancelled)
            }
        default:
            break
        }
    }

    private func handleDrop() {
        guard reconnectOnFailure else { return }
        let delay = reconnectDelay
        queue.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self else { return }
            guard self.reconnectOnFailure else { return }
            self.startConnection(initialContinuation: nil)
        }
    }

    private func startReceiveLoop(on c: NWConnection) {
        c.receive(minimumIncompleteLength: 1, maximumLength: 16 * 1024) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            // We are on `self.queue` because that's the queue NWConnection uses.
            if let data, !data.isEmpty {
                self.ingest(data)
            }
            if isComplete || error != nil {
                self.handleDrop()
                return
            }
            // Reschedule
            self.startReceiveLoop(on: c)
        }
    }

    private func ingest(_ data: Data) {
        let frames: [MagiFrameParser.Frame]
        do {
            frames = try parser.feed(data)
        } catch {
            // Malformed frame — drop the connection so we re-sync.
            connection?.cancel()
            return
        }
        for f in frames {
            handlers[f.id]?(f.payload)
        }
    }
}

private extension NWEndpoint.Host {
    var isLoopback: Bool {
        switch self {
        case .ipv4(let v): return v == .loopback
        case .ipv6(let v): return v == .loopback
        case .name(let name, _): return name == "localhost" || name == "127.0.0.1"
        @unknown default: return false
        }
    }
}
