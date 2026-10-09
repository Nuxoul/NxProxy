// Standalone real-QLocalSocket regression (no GUI main or running core).
// Compile this TU plus the normally generated libcore.pb.cc / myproto library.
#include "src/api/RPC.cpp"
#include <QCoreApplication>
#include <QLocalServer>
#include <QUuid>
#include <thread>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char *what) { if (!ok) throw std::runtime_error(what); }
struct Peer {
    QString name = "throne-rpc-test-" + QUuid::createUuid().toString(QUuid::Id128);
    std::atomic<bool> stop{false}, legacy{false}, oversized{false}, hold{false};
    std::atomic<int> tests{0}, cancels{0};
    std::promise<void> listening;
    std::thread worker;
    Peer() : worker([this]() { run(); }) { listening.get_future().wait(); }
    ~Peer() { stop = true; worker.join(); }
    void run() {
        QLocalServer server;
        if (!server.listen(name)) std::terminate();
        listening.set_value();
        while (!stop) {
            if (!server.waitForNewConnection(100)) continue;
            std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
            QByteArray input;
            QList<quint32> held;
            auto respond = [&](quint32 id, quint8 status, const std::string &payload) {
                QByteArray frame;
                QDataStream ds(&frame, QIODevice::WriteOnly);
                ds.setByteOrder(QDataStream::LittleEndian);
                ds << id << status << quint32(payload.size());
                ds.writeRawData(payload.data(), int(payload.size()));
                // Deliberately fragment header and payload on the real socket.
                for (int pos = 0; pos < frame.size(); pos += 3) {
                    socket->write(frame.mid(pos, 3));
                    socket->waitForBytesWritten(1000);
                }
            };
            while (!stop && socket->state() == QLocalSocket::ConnectedState) {
                if (!socket->bytesAvailable()) socket->waitForReadyRead(100);
                input += socket->readAll();
                while (input.size() >= 6) {
                    quint32 id, length; quint16 nameLen;
                    QDataStream head(input); head.setByteOrder(QDataStream::LittleEndian);
                    head >> id >> nameLen;
                    if (input.size() < 10 + nameLen) break;
                    const auto method = input.mid(6, nameLen);
                    QDataStream size(input.mid(6 + nameLen, 4)); size.setByteOrder(QDataStream::LittleEndian); size >> length;
                    if (input.size() < 10 + nameLen + length) break;
                    const auto payload = input.mid(10 + nameLen, length);
                    input.remove(0, 10 + nameLen + length);
                    if (method == "QueryCapabilities") {
                        if (legacy) respond(id, 1, "unknown method: QueryCapabilities");
                        else {
                            libcore::RPCCapabilities cap;
                            cap.cancel_request = true; cap.protocol_version = 1;
                            cap.cancelable_methods = {"Test", "CaptureDiagnostics"};
                            respond(id, 0, spb::pb::serialize<std::string>(cap));
                        }
                    } else if (method == "Test") {
                        ++tests;
                        if (oversized) {
                            QByteArray header; QDataStream ds(&header, QIODevice::WriteOnly);
                            ds.setByteOrder(QDataStream::LittleEndian); ds << id << quint8(0) << quint32((128 << 20) + 1);
                            socket->write(header); socket->waitForBytesWritten(1000);
                        } else if (hold) held.append(id);
                        else respond(id, 0, spb::pb::serialize<std::string>(libcore::TestResp{}));
                    } else if (method == "CancelRequest") {
                        ++cancels;
                        auto cancel = spb::pb::deserialize<libcore::CancelRequestReq>(payload);
                        libcore::CancelRequestResp reply; reply.accepted = true;
                        respond(id, 0, spb::pb::serialize<std::string>(reply));
                        // Late success must not satisfy a different pending call.
                        if (held.removeOne(cancel.req_id.value())) respond(cancel.req_id.value(), 0, {});
                    } else if (method == "StopTest") {
                        respond(id, 0, {});
                        for (auto pending : held) respond(pending, 1, "test aborted");
                        held.clear();
                    } else respond(id, 0, {});
                }
            }
        }
        server.close();
    }
    void connect(API::Client &client) {
        auto *socket = new QLocalSocket;
        socket->connectToServer(name);
        require(socket->waitForConnected(2000), "local connect failed");
        client.Reconnect(socket);
        if (!legacy) {
            for (int i = 0; i < 200 && !client.Capabilities().cancel_request.value_or(false); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            require(client.Capabilities().cancel_request.value_or(false), "capability handshake missing");
        } else std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
};
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    MW_show_log = [](QString) {};
    try {
        Peer peer;
        API::Client client;
        peer.connect(client);
        bool ok = false;
        client.Test(&ok, {}, nullptr, 2000);
        require(ok, "fragmented response failed");
        peer.hold = true;
        std::vector<std::future<bool>> calls;
        const auto started = peer.tests.load();
        for (int i = 0; i < 8; ++i) calls.push_back(std::async(std::launch::async, [&]() {
            bool success = false; client.Test(&success, {}, nullptr, 3000); return success;
        }));
        for (int i = 0; i < 200 && peer.tests < started + 8; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        require(peer.tests == started + 8, "ordinary requests did not arrive");
        QString error;
        client.Test(&ok, {}, &error, 100);
        require(!ok && error.startsWith("rpc busy"), "client busy not deterministic");
        client.StopTests(&ok);
        require(ok, "reserved control slot starved");
        for (auto &call : calls) require(!call.get(), "stopped heavy request succeeded");
        const auto cancels = peer.cancels.load();
        client.Test(&ok, {}, nullptr, 40);
        require(!ok, "held call did not time out");
        for (int i = 0; i < 200 && peer.cancels == cancels; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        require(peer.cancels > cancels, "timeout cancellation missing");
        client.StopTests(&ok); require(ok, "late response corrupted next call");
        // All eight simultaneous timeouts must eventually use four bounded
        // reserved slots, not silently lose half the cancellations.
        calls.clear();
        const auto batchCancels = peer.cancels.load();
        for (int i = 0; i < 8; ++i) calls.push_back(std::async(std::launch::async, [&]() {
            bool success = false; client.Test(&success, {}, nullptr, 200); return success;
        }));
        for (auto &call : calls) require(!call.get(), "held batch unexpectedly succeeded");
        for (int i = 0; i < 400 && peer.cancels < batchCancels + 8; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        require(peer.cancels == batchCancels + 8, "bounded cancellation backlog lost requests");
        client.StopTests(&ok); require(ok, "batch late responses corrupted stream");
        peer.oversized = true; peer.hold = false;
        client.Test(&ok, {}, nullptr, 2000);
        require(!ok, "oversized response was accepted");
        peer.oversized = false;
        peer.connect(client);
        client.Test(&ok, {}, nullptr, 2000); require(ok, "reconnect failed");
        peer.legacy = true;
        peer.connect(client);
        require(!client.Capabilities().cancel_request.value_or(false), "legacy retained stale capabilities");
        peer.hold = true;
        const auto legacyCancels = peer.cancels.load();
        client.Test(&ok, {}, nullptr, 40);
        client.StopTests(&ok); require(ok, "legacy stop fallback failed");
        require(peer.cancels == legacyCancels, "CancelRequest sent to old core");
        std::cout << "PASS: fragmented/oversize/busy/control/timeout/cancel/late/reconnect/legacy QLocalSocket\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
