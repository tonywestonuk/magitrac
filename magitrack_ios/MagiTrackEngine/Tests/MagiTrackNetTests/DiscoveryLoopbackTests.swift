import XCTest
import Network
@testable import MagiTrackNet
@testable import MagiTrackWire
@testable import MagiTrackCore

/// Loopback test for `MagiDiscovery` — send a synthetic broadcast on
/// localhost UDP and verify the listener parses it correctly.
final class DiscoveryLoopbackTests: XCTestCase {

    func testReceivesBeacon() async throws {
        // Pick a random port (Apple recommends not hardcoding test ports)
        let port: UInt16 = UInt16.random(in: 50_000...60_000)
        let discovery = MagiDiscovery(port: port)
        discovery.start()
        // Give NWListener a tick to bind
        try await Task.sleep(nanoseconds: 200_000_000)

        // Synthesise an announce and send to 127.0.0.1:port
        let announce = MsgServerAnnounce(tcpPort: 4242, name: "loopback-srv")
        let payload = announce.encode()

        let conn = NWConnection(
            host: NWEndpoint.Host("127.0.0.1"),
            port: NWEndpoint.Port(rawValue: port)!,
            using: .udp)
        conn.start(queue: .global())
        // Wait for connection to be ready
        try await Task.sleep(nanoseconds: 100_000_000)
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            conn.send(content: payload, completion: .contentProcessed { error in
                if let error { cont.resume(throwing: error) } else { cont.resume() }
            })
        }

        // Subscribe and wait for the beacon
        var iterator = discovery.discoveryStream().makeAsyncIterator()
        let waitTask = Task<MagiDiscovery.Beacon?, Never> { await iterator.next() }
        // Bound the wait
        let beacon = try await withThrowingTaskGroup(of: MagiDiscovery.Beacon?.self) { group in
            group.addTask { await waitTask.value }
            group.addTask {
                try await Task.sleep(nanoseconds: 2_000_000_000)
                return nil
            }
            let first = try await group.next()
            group.cancelAll()
            return first ?? nil
        }
        let received = try XCTUnwrap(beacon, "Discovery listener didn't receive the beacon within 2 s")
        XCTAssertEqual(received.tcpPort, 4242)
        XCTAssertEqual(received.name, "loopback-srv")
        XCTAssertTrue(received.host.hasPrefix("127.0.0.1"),
                      "Expected source IP 127.0.0.1, got \(received.host)")

        conn.cancel()
        discovery.stop()
    }
}
