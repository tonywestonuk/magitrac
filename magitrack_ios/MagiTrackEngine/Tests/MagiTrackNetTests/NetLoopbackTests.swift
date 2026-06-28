import XCTest
import Network
@testable import MagiTrackNet
@testable import MagiTrackWire
@testable import MagiTrackCore

/// Loopback integration tests. Spin up a real `NWListener` on a random
/// localhost port to act as a mock server, then drive `MagiLinkClient`
/// against it. This is the closest we can get to the live CoreS3 server
/// without one on the bench.
final class NetLoopbackTests: XCTestCase {

    /// Mock server. Accepts one connection, parses incoming frames, runs a
    /// per-frame handler (set by the test). Sends are wrapped in `send()`.
    actor MockServer {
        private let queue = DispatchQueue(label: "mock-magilink-server")
        private var listener: NWListener?
        private var connection: NWConnection?
        private var parser = MagiFrameParser()
        private var onFrame: (@Sendable (MagiFrameParser.Frame, MockServer) async -> Void)?

        nonisolated let portContinuation = AsyncStream<UInt16>.makeStream()

        func start(onFrame: @escaping @Sendable (MagiFrameParser.Frame, MockServer) async -> Void) throws {
            self.onFrame = onFrame
            let l = try NWListener(using: .tcp, on: .any)
            l.stateUpdateHandler = { [weak self] state in
                guard let self else { return }
                if case .ready = state, let port = l.port {
                    self.portContinuation.continuation.yield(port.rawValue)
                }
            }
            l.newConnectionHandler = { [weak self] c in
                guard let self else { return }
                Task { await self.accept(c) }
            }
            l.start(queue: queue)
            self.listener = l
        }

        private func accept(_ c: NWConnection) {
            connection = c
            c.stateUpdateHandler = { _ in }
            c.start(queue: queue)
            receiveLoop(on: c)
        }

        private func receiveLoop(on c: NWConnection) {
            c.receive(minimumIncompleteLength: 1, maximumLength: 16 * 1024) { [weak self] data, _, isComplete, error in
                guard let self else { return }
                if let data, !data.isEmpty {
                    Task { await self.handleBytes(data) }
                }
                if !(isComplete || error != nil) {
                    Task { await self.receiveLoop(on: c) }
                }
            }
        }

        private func handleBytes(_ data: Data) async {
            let frames: [MagiFrameParser.Frame]
            do { frames = try parser.feed(data) } catch { return }
            for f in frames {
                await onFrame?(f, self)
            }
        }

        func send(_ frame: Data) {
            connection?.send(content: frame, completion: .contentProcessed { _ in })
        }

        func stop() {
            connection?.cancel()
            listener?.cancel()
        }
    }

    func testConnectAndExchangeMessages() async throws {
        let server = MockServer()
        let received = expectation(description: "client receives MSG_CONNECT_ACK")
        let sawPlay = expectation(description: "server sees MSG_PLAY")

        try await server.start { frame, srv in
            if frame.id == MagiMsgID.connect.rawValue {
                // Reply with ACK
                await srv.send(MsgConnectAck().encode())
            } else if frame.id == MagiMsgID.play.rawValue {
                sawPlay.fulfill()
            }
        }

        // Wait for the listener's port
        var port: UInt16 = 0
        for await p in server.portContinuation.stream {
            port = p
            break
        }
        XCTAssertGreaterThan(port, 0)

        let client = MagiLinkClient(host: "127.0.0.1", port: port)
        client.register(MsgConnectAck.self) { _ in
            received.fulfill()
        }
        try await client.connect(autoReconnect: false)

        try await client.send(MsgConnect())
        await fulfillment(of: [received], timeout: 2.0)

        try await client.send(MsgPlay())
        await fulfillment(of: [sawPlay], timeout: 2.0)

        await client.disconnect()
        await server.stop()
    }

    func testServerStreamReassembly() async throws {
        // Server sends a SONG_PUSH_HEADER + 3 BODY messages + END_OF_DATA.
        // Client must reassemble despite arbitrary TCP boundaries.
        let server = MockServer()
        try await server.start { frame, srv in
            if frame.id == MagiMsgID.connect.rawValue {
                await srv.send(MsgConnectAck().encode())
                // Stream a 2500-byte synthetic "song"
                let chunkA = Data((0..<1024).map { UInt8($0 % 256) })
                let chunkB = Data((1024..<2048).map { UInt8($0 % 256) })
                let chunkC = Data((2048..<2500).map { UInt8($0 % 256) })
                await srv.send(MsgSongPushHeader(totalSize: 2500).encode())
                await srv.send(MsgSongPushBody(data: chunkA).encode())
                await srv.send(MsgSongPushBody(data: chunkB).encode())
                await srv.send(MsgSongPushBody(data: chunkC).encode())
                await srv.send(MsgEndOfData().encode())
            }
        }

        var port: UInt16 = 0
        for await p in server.portContinuation.stream { port = p; break }

        let client = MagiLinkClient(host: "127.0.0.1", port: port)

        let headerArrived = expectation(description: "song push header")
        let endArrived    = expectation(description: "end of data")
        let bytesCollected = ReceivedBytes()

        client.register(MsgSongPushHeader.self) { hdr in
            Task { await bytesCollected.setExpected(Int(hdr.totalSize)) }
            headerArrived.fulfill()
        }
        client.register(MsgSongPushBody.self) { body in
            Task { await bytesCollected.append(body.data) }
        }
        client.register(MsgEndOfData.self) { _ in
            endArrived.fulfill()
        }
        try await client.connect(autoReconnect: false)
        try await client.send(MsgConnect())

        await fulfillment(of: [headerArrived, endArrived], timeout: 3.0)
        let total = await bytesCollected.total
        let expected = await bytesCollected.expected
        XCTAssertEqual(total, expected)
        XCTAssertEqual(total, 2500)

        await client.disconnect()
        await server.stop()
    }
}

/// Actor for thread-safe collection of received body bytes in the streaming test.
actor ReceivedBytes {
    var total: Int = 0
    var expected: Int = 0
    func setExpected(_ n: Int) { expected = n }
    func append(_ d: Data) { total += d.count }
}
