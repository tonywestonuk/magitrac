import Foundation
import Network
import MagiTrackCore
import MagiTrackWire

/// Generic UDP message listener.
///
/// Binds UDP `MAGI_PORT` (4242) and receives every datagram (broadcast +
/// unicast). On each, the first byte is the `MagiMsgType`; the listener
/// dispatches to handlers registered per-type, plus exposes a typed
/// `Beacon` stream for `MSG_SERVER_ANNOUNCE`.
///
/// Single shared instance per app (held by `AppState`) — multiple
/// listeners can't bind the same UDP port even with `allowLocalEndpointReuse`
/// because the kernel picks one socket per datagram.
public final class MagiDiscovery: @unchecked Sendable {

    public struct Beacon: Sendable, Equatable {
        public let host: String          // source IP (e.g. "192.168.1.42")
        public let tcpPort: UInt16
        public let name: String
        public let receivedAt: Date
    }

    private let port: NWEndpoint.Port
    private let queue = DispatchQueue(label: "magi-udp-listener", qos: .userInitiated)
    private var listener: NWListener?
    private var subscribers: [UUID: AsyncStream<Beacon>.Continuation] = [:]
    private var handlers: [UInt8: @Sendable (Data, String) -> Void] = [:]
    private(set) public var latest: Beacon?

    public init(port: UInt16 = MagiTrack.magiPort) {
        self.port = NWEndpoint.Port(rawValue: port)!
    }

    /// Start listening. Idempotent.
    public func start() {
        queue.async { [weak self] in
            guard let self else { return }
            self.listener?.cancel()
            do {
                let params = NWParameters.udp
                params.allowLocalEndpointReuse = true
                params.includePeerToPeer = true
                let l = try NWListener(using: params, on: self.port)
                l.newConnectionHandler = { [weak self] c in
                    self?.handle(c)
                }
                l.stateUpdateHandler = { state in
                    if case .failed(let err) = state {
                        print("[MagiDiscovery] listener failed: \(err)")
                    }
                }
                l.start(queue: self.queue)
                self.listener = l
                print("[MagiDiscovery] listening on UDP \(self.port)")
            } catch {
                print("[MagiDiscovery] start failed: \(error)")
            }
        }
    }

    public func stop() {
        queue.async { [weak self] in
            self?.listener?.cancel()
            self?.listener = nil
        }
    }

    /// Subscribe to server-announce beacons. Latest beacon is yielded
    /// immediately if already received.
    public func discoveryStream() -> AsyncStream<Beacon> {
        AsyncStream { continuation in
            let id = UUID()
            queue.async { [weak self] in
                guard let self else { continuation.finish(); return }
                self.subscribers[id] = continuation
                if let latest = self.latest { continuation.yield(latest) }
            }
            continuation.onTermination = { [weak self] _ in
                self?.queue.async { [weak self] in
                    self?.subscribers.removeValue(forKey: id)
                }
            }
        }
    }

    /// Register a handler for raw payloads of a given message type byte.
    /// The handler is called on the listener's serial queue. `source` is
    /// the datagram's source IP (informational; useful for unicast vs
    /// broadcast inspection).
    public func registerHandler(_ msgType: UInt8,
                                _ handler: @escaping @Sendable (Data, String) -> Void) {
        queue.async { [weak self] in
            self?.handlers[msgType] = handler
        }
    }

    public func unregisterHandler(_ msgType: UInt8) {
        queue.async { [weak self] in
            self?.handlers.removeValue(forKey: msgType)
        }
    }

    // ── Internals — queue-isolated ──────────────────────────────────────

    private func handle(_ c: NWConnection) {
        c.start(queue: queue)
        c.receiveMessage { [weak self] data, _, _, _ in
            guard let self else { return }
            defer { c.cancel() }
            guard let data, !data.isEmpty else { return }
            let src = Self.ipString(from: c.endpoint)
            let typeByte = data[data.startIndex]
            // Beacon path
            if typeByte == MagiMsgID.serverAnnounce.rawValue,
               let msg = MsgServerAnnounce(decoding: data) {
                let beacon = Beacon(host: src,
                                    tcpPort: msg.tcpPort,
                                    name: msg.name,
                                    receivedAt: Date())
                self.latest = beacon
                for cont in self.subscribers.values { cont.yield(beacon) }
            }
            // Generic handler path (also fires for ServerAnnounce if registered).
            if let h = self.handlers[typeByte] {
                h(data, src)
            }
        }
    }

    private static func ipString(from endpoint: NWEndpoint) -> String {
        switch endpoint {
        case .hostPort(let host, _):
            switch host {
            case .ipv4(let addr): return addr.debugDescription
            case .ipv6(let addr): return addr.debugDescription
            case .name(let n, _): return n
            @unknown default:     return "?"
            }
        case .service(let name, _, _, _): return name
        case .unix(let path):              return path
        case .url(let url):                return url.absoluteString
        case .opaque:                      return "?"
        @unknown default:                  return "?"
        }
    }
}
