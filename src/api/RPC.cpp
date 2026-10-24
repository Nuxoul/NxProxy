#include "include/api/RPC.h"
#include <utility>
#include "include/global/Configs.hpp"
#include <QLocalSocket>
#include <QDataStream>
#include <QMap>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <exception>
#include <memory>
#include <deque>
#include <algorithm>

namespace API {
    // Existing LE framing is unchanged. Budgets mirror core/internal/rpc/dispatch.go.
    // No unbounded invokeMethod(frame) queue: one bounded priority deque owns writes.
    class Client::LocalSocketChannel {
        static constexpr qint64 kRequestMax = 96LL << 20;
        static constexpr qint64 kResponseMax = 128LL << 20;
        static constexpr qint64 kRequestBudget = 128LL << 20;
        static constexpr qint64 kResponseBudget = 192LL << 20;
        static constexpr qint64 kControlMax = 64LL << 10;
        static constexpr int kOrdinarySlots = 8;
        static constexpr int kControlSlots = 4;
        static constexpr int kIOTimeoutMs = 30000;
        static constexpr qint64 kSocketWindow = 1LL << 20;
        using Clock = std::chrono::steady_clock;
        struct PendingCall {
            std::mutex mu;
            std::condition_variable cv;
            bool done = false;
            quint8 status = 1;
            QByteArray data;
            QString method;
            bool control = false;
            bool internal = false;
            bool handshake = false;
            Clock::time_point deadline;
            qint64 responseReservation = 0;
        };
        struct Outbound {
            quint32 id;
            quint64 generation;
            bool control;
            QByteArray frame;
            qint64 offset = 0;
        };
        QThread *io_thread;
        QObject *io_anchor;
        QTimer *timer = nullptr;
        QLocalSocket *sock = nullptr; // IO-thread only
        QByteArray read_buf;
        qint64 expectedSize = 9;
        quint32 readingID = 0;
        quint8 readingStatus = 1;
        bool readingPayload = false;
        std::shared_ptr<PendingCall> readingCall;
        qint64 readingReservation = 0;
        Clock::time_point readStarted{};
        Clock::time_point writeStarted{};
        quint64 ioGeneration = 0;
        std::atomic<bool> connected_{false};
        std::atomic<quint64> generation_{0};
        std::atomic<quint32> next_id{1};
        std::atomic<qint64> responseBytes{0};
        std::atomic<qint64> controlResponseBytes{0};
        mutable std::mutex pending_mu;
        bool pumpScheduled = false;
        QMap<quint32, std::shared_ptr<PendingCall>> pending;
        int ordinaryPending = 0, controlPending = 0;
        qint64 ordinaryQueuedBytes = 0, controlQueuedBytes = 0;
        std::deque<Outbound> ordinaryWrites, controlWrites;
        struct Cancellation { quint32 id; QString method; quint64 generation; };
        std::deque<Cancellation> cancellations; // bounded by eight ordinary calls
        Outbound currentWrite{};
        libcore::RPCCapabilities capabilities;

        static bool isControl(const QString &name) {
            return name == "Stop" || name == "StopTest" || name == "StopDiagnostics" ||
                   name == "CancelRequest" || name == "CancelVPNChallenge" || name == "QueryCapabilities";
        }
        bool cancelSupportedLocked(const QString &method) const {
            return capabilities.cancel_request.value_or(false) &&
                std::find(capabilities.cancelable_methods.begin(), capabilities.cancelable_methods.end(),
                          method.toStdString()) != capabilities.cancelable_methods.end();
        }
        void erasePendingLocked(quint32 id) {
            auto it = pending.find(id);
            if (it == pending.end()) return;
            if (it.value()->control) --controlPending; else --ordinaryPending;
            pending.erase(it);
        }
        bool reserveResponse(qint64 bytes, bool control) {
            auto &counter = control ? controlResponseBytes : responseBytes;
            const qint64 budget = control ? kControlSlots * kControlMax : kResponseBudget;
            auto used = counter.load();
            do {
                if (bytes > budget - used) return false;
            } while (!counter.compare_exchange_weak(used, used + bytes));
            return true;
        }
        void clearReader() {
            if (readingReservation) (readingCall && readingCall->control ? controlResponseBytes : responseBytes).fetch_sub(readingReservation);
            readingReservation = 0;
            readingCall.reset();
            read_buf.clear();
            expectedSize = 9;
            readingPayload = false;
            readStarted = {};
        }
        void wakeAllWithError() {
            connected_.store(false, std::memory_order_release);
            std::lock_guard<std::mutex> lock(pending_mu);
            capabilities = {};
            for (auto &call : pending) {
                std::lock_guard<std::mutex> cg(call->mu);
                call->done = true;
                call->status = 1;
                call->cv.notify_one();
            }
            pending.clear();
            ordinaryPending = controlPending = 0;
            ordinaryWrites.clear(); controlWrites.clear(); cancellations.clear();
            ordinaryQueuedBytes = controlQueuedBytes = 0;
        }
        void failConnection() {
            wakeAllWithError();
            clearReader();
            currentWrite = {};
            writeStarted = {};
            if (sock) sock->abort();
        }
        void finishResponse() {
            // Publish before dropping pending_mu: a timeout that loses to the
            // reader observes a completed call, never an unowned reservation.
            std::lock_guard<std::mutex> pendingLock(pending_mu);
            auto call = readingCall;
            if (call && pending.value(readingID) == call) erasePendingLocked(readingID);
            else call.reset();
            if (call) {
                if (call->handshake) {
                    libcore::RPCCapabilities negotiated;
                    if (readingStatus == 0) {
                        try { negotiated = spb::pb::deserialize<libcore::RPCCapabilities>(read_buf); }
                        catch (...) { negotiated = {}; }
                    }
                    capabilities = std::move(negotiated);
                } else if (!call->internal) {
                    std::lock_guard<std::mutex> lock(call->mu);
                    call->status = readingStatus;
                    call->data = std::move(read_buf);
                    call->responseReservation = readingReservation;
                    readingReservation = 0;
                    call->done = true;
                    call->cv.notify_one();
                }
            }
            clearReader();
        }
        void onReadyRead() {
            // Read only the validated current frame. QLocalSocket's own buffer is
            // bounded too; no readAll or speculative allocation from dataLen.
            while (sock && sock->bytesAvailable() > 0) {
                if (readStarted == Clock::time_point{}) readStarted = Clock::now();
                const auto have = readingPayload && !readingCall ? qint64(0) : qint64(read_buf.size());
                const qint64 need = expectedSize - have;
                const qint64 amount = qMin(qMin(need, sock->bytesAvailable()), qint64(64 << 10));
                if (amount <= 0) { failConnection(); return; }
                auto chunk = sock->read(amount);
                if (chunk.isEmpty()) return;
                if (readingPayload && !readingCall) expectedSize -= chunk.size();
                else read_buf += chunk;
                if ((!readingCall && readingPayload && expectedSize == 0) || read_buf.size() == expectedSize) {
                    if (readingPayload) { finishResponse(); continue; }
                    quint32 dataLen;
                    QDataStream ds(read_buf);
                    ds.setByteOrder(QDataStream::LittleEndian);
                    ds >> readingID >> readingStatus >> dataLen;
                    if (dataLen > kResponseMax || readingStatus > 1) { failConnection(); return; }
                    {
                        std::lock_guard<std::mutex> lock(pending_mu);
                        readingCall = pending.value(readingID);
                    }
                    if (readingCall && readingCall->control && dataLen > kControlMax) { failConnection(); return; }
                    if (readingCall && !reserveResponse(dataLen, readingCall->control)) { failConnection(); return; }
                    readingReservation = readingCall ? dataLen : 0;
                    read_buf.clear();
                    expectedSize = dataLen;
                    readingPayload = true;
                    if (dataLen == 0) finishResponse();
                }
            }
        }
        void pumpWrites() {
            if (!sock || !connected_.load() || ioGeneration != generation_.load()) return;
            while (sock->bytesToWrite() < kSocketWindow) {
                if (currentWrite.frame.isEmpty()) {
                    std::lock_guard<std::mutex> lock(pending_mu);
                    auto &queue = controlWrites.empty() ? ordinaryWrites : controlWrites;
                    if (queue.empty()) { if (!sock->bytesToWrite()) writeStarted = {}; return; }
                    currentWrite = std::move(queue.front()); queue.pop_front();
                    if (currentWrite.generation != ioGeneration || !pending.contains(currentWrite.id)) {
                        auto &bytes = currentWrite.control ? controlQueuedBytes : ordinaryQueuedBytes;
                        bytes = qMax(qint64(0), bytes - currentWrite.frame.size());
                        currentWrite = {}; continue;
                    }
                    writeStarted = Clock::now();
                }
                const auto remaining = currentWrite.frame.size() - currentWrite.offset;
                const qint64 amount = qMin(qMin(qint64(remaining), kSocketWindow - sock->bytesToWrite()), qint64(64 << 10));
                const qint64 n = sock->write(currentWrite.frame.constData() + currentWrite.offset, amount);
                if (n <= 0) { failConnection(); return; }
                currentWrite.offset += n; // preserve remainder on a short write
                if (currentWrite.offset == currentWrite.frame.size()) {
                    std::lock_guard<std::mutex> lock(pending_mu);
                    auto &bytes = currentWrite.control ? controlQueuedBytes : ordinaryQueuedBytes;
                    bytes = qMax(qint64(0), bytes - currentWrite.frame.size());
                    currentWrite = {};
                    // Do not reset the stall deadline until the socket drains.
                }
            }
        }
        int enqueue(const QString &method, const std::string &req, std::shared_ptr<PendingCall> call,
                    quint32 &id, quint64 &generation, quint64 expectedGeneration = 0) {
            auto name = method.toUtf8();
            const bool control = isControl(method);
            if (name.isEmpty() || name.size() > 128 || req.size() > size_t(kRequestMax) ||
                (control && req.size() > size_t(kControlMax))) return CallLimit;
            const qint64 frameSize = 10 + name.size() + qint64(req.size());
            std::lock_guard<std::mutex> lock(pending_mu);
            if (!connected_.load() || (expectedGeneration && generation_.load() != expectedGeneration)) return CallNotConnected;
            auto &count = control ? controlPending : ordinaryPending;
            auto &bytes = control ? controlQueuedBytes : ordinaryQueuedBytes;
            const auto budget = control ? kControlSlots * (kControlMax + 138) : kRequestBudget;
            if (count >= (control ? kControlSlots : kOrdinarySlots) ||
                (control ? controlWrites.size() : ordinaryWrites.size()) >= size_t(control ? kControlSlots : kOrdinarySlots) ||
                frameSize > budget - bytes) return CallBusy;
            // ID zero is unused; never reuse an ID within a connection, even if
            // its response is late. Wrap requires reconnect rather than collision.
            id = next_id.fetch_add(1);
            if (id == 0) { connected_.store(false); return CallLimit; }
            generation = generation_.load();
            call->method = method; call->control = control;
            QByteArray frame;
            frame.reserve(frameSize);
            QDataStream ds(&frame, QIODevice::WriteOnly);
            ds.setByteOrder(QDataStream::LittleEndian);
            ds << id << quint16(name.size());
            ds.writeRawData(name.constData(), int(name.size()));
            ds << quint32(req.size());
            ds.writeRawData(req.data(), int(req.size()));
            pending[id] = call; ++count; bytes += frame.size();
            (control ? controlWrites : ordinaryWrites).push_back({id, generation, control, std::move(frame)});
            return CallOK;
        }
        void fire(const QString &method, const std::string &req, bool handshake = false) {
            auto call = std::make_shared<PendingCall>();
            call->internal = true; call->handshake = handshake;
            call->deadline = Clock::now() + std::chrono::milliseconds(kIOTimeoutMs);
            quint32 id; quint64 generation;
            if (enqueue(method, req, call, id, generation) == CallOK) pumpWrites();
        }
        void schedulePump() {
            std::lock_guard<std::mutex> lock(pending_mu);
            if (pumpScheduled) return;
            pumpScheduled = true;
            QMetaObject::invokeMethod(io_anchor, [this]() {
                { std::lock_guard<std::mutex> lock(pending_mu); pumpScheduled = false; }
                pumpCancellations();
                pumpWrites();
            }, Qt::QueuedConnection);
        }
        void sendCancel(quint32 id, const QString &method, quint64 generation) {
            {
                std::lock_guard<std::mutex> lock(pending_mu);
                if (generation_.load() != generation || !cancelSupportedLocked(method)) return;
                const auto found = std::find_if(cancellations.begin(), cancellations.end(),
                    [id, generation](const auto &c) { return c.id == id && c.generation == generation; });
                if (found == cancellations.end() && cancellations.size() < size_t(kOrdinarySlots))
                    cancellations.push_back({id, method, generation});
            }
            // tick drains the bounded cancellation backlog as control slots free.
            schedulePump();
        }
        void pumpCancellations() {
            for (int i = 0; i < kControlSlots; ++i) {
                Cancellation cancellation;
                {
                    std::lock_guard<std::mutex> lock(pending_mu);
                    if (cancellations.empty()) return;
                    cancellation = cancellations.front();
                }
                libcore::CancelRequestReq req;
                req.req_id = cancellation.id;
                auto call = std::make_shared<PendingCall>();
                call->internal = true;
                call->deadline = Clock::now() + std::chrono::milliseconds(kIOTimeoutMs);
                quint32 id; quint64 generation;
                const auto status = enqueue("CancelRequest", spb::pb::serialize<std::string>(req), call,
                                            id, generation, cancellation.generation);
                if (status == CallBusy) return;
                std::lock_guard<std::mutex> lock(pending_mu);
                if (!cancellations.empty() && cancellations.front().id == cancellation.id &&
                    cancellations.front().generation == cancellation.generation) cancellations.pop_front();
            }
        }
        void tick() {
            const auto now = Clock::now();
            const auto limit = std::chrono::milliseconds(kIOTimeoutMs);
            if ((readStarted != Clock::time_point{} && now - readStarted >= limit) ||
                (writeStarted != Clock::time_point{} && now - writeStarted >= limit)) {
                failConnection(); return;
            }
            {
                std::lock_guard<std::mutex> lock(pending_mu);
                QList<quint32> expired;
                for (auto it = pending.cbegin(); it != pending.cend(); ++it)
                    if (it.value()->internal && it.value()->deadline <= now) expired.append(it.key());
                for (auto id : expired) erasePendingLocked(id);
            }
            pumpCancellations();
            pumpWrites();
        }
    public:
        static constexpr int CallOK = 0;
        static constexpr int CallNotConnected = -1919;
        static constexpr int CallBusy = -1920;
        static constexpr int CallLimit = -1921;
        LocalSocketChannel() {
            io_thread = new QThread;
            io_anchor = new QObject;
            io_anchor->moveToThread(io_thread);
            io_thread->start();
            QMetaObject::invokeMethod(io_anchor, [this]() {
                timer = new QTimer(io_anchor);
                QObject::connect(timer, &QTimer::timeout, io_anchor, [this]() { tick(); });
                timer->start(250);
            }, Qt::QueuedConnection);
        }
        ~LocalSocketChannel() {
            wakeAllWithError();
            QMetaObject::invokeMethod(io_anchor, [this]() {
                clearReader();
                if (sock) { sock->disconnect(io_anchor); sock->abort(); delete sock; sock = nullptr; }
                timer->stop();
                io_thread->quit();
            }, Qt::QueuedConnection);
            io_thread->wait();
            delete io_anchor;
            delete io_thread;
        }
        void Reconnect(QLocalSocket *newSock) {
            wakeAllWithError();
            const auto generation = generation_.fetch_add(1) + 1;
            newSock->setParent(nullptr);
            newSock->moveToThread(io_thread);
            QMetaObject::invokeMethod(io_anchor, [this, newSock, generation]() {
                if (generation != generation_.load()) { newSock->deleteLater(); return; }
                if (sock) { sock->disconnect(io_anchor); sock->abort(); sock->deleteLater(); }
                sock = newSock; ioGeneration = generation;
                clearReader(); currentWrite = {}; writeStarted = {};
                sock->setReadBufferSize(kSocketWindow);
                QObject::connect(sock, &QLocalSocket::readyRead, io_anchor, [this]() { onReadyRead(); });
                QObject::connect(sock, &QLocalSocket::bytesWritten, io_anchor, [this](qint64) {
                    writeStarted = sock->bytesToWrite() ? Clock::now() : Clock::time_point{};
                    pumpWrites();
                });
                QObject::connect(sock, &QLocalSocket::disconnected, io_anchor, [this]() { failConnection(); });
                QObject::connect(sock, &QLocalSocket::errorOccurred, io_anchor,
                                 [this](QLocalSocket::LocalSocketError) { failConnection(); });
                connected_.store(sock->state() == QLocalSocket::ConnectedState);
                if (connected_.load()) fire("QueryCapabilities", {}, true);
                if (sock->bytesAvailable()) onReadyRead();
                pumpWrites();
            }, Qt::BlockingQueuedConnection);
        }
        libcore::RPCCapabilities Capabilities() const {
            std::lock_guard<std::mutex> lock(pending_mu);
            return capabilities;
        }
        void CancelPendingCalls() {
            QList<std::pair<quint32, QString>> calls;
            quint64 generation;
            {
                std::lock_guard<std::mutex> lock(pending_mu);
                generation = generation_.load();
                for (auto it = pending.cbegin(); it != pending.cend(); ++it)
                    if (cancelSupportedLocked(it.value()->method)) calls.append({it.key(), it.value()->method});
            }
            for (const auto &[id, method] : calls) sendCancel(id, method, generation);
        }
        int Call(const QString &methodName, const std::string &req, std::vector<uint8_t> &rsp, int timeout_ms = 0) {
            rsp.clear();
            auto call = std::make_shared<PendingCall>();
            quint32 id; quint64 generation;
            const int ms = timeout_ms > 0 ? timeout_ms : kIOTimeoutMs;
            call->deadline = Clock::now() + std::chrono::milliseconds(ms);
            const auto status = enqueue(methodName, req, call, id, generation);
            if (status != CallOK) {
                if (status == CallBusy) { const std::string busy = "rpc busy: client request budget"; rsp.assign(busy.begin(), busy.end()); }
                return status;
            }
            schedulePump();
            std::unique_lock<std::mutex> lock(call->mu);
            const bool ok = call->cv.wait_until(lock, call->deadline, [&call]() { return call->done; });
            lock.unlock(); // pending_mu always precedes call->mu
            if (!ok) {
                {
                    std::lock_guard<std::mutex> plock(pending_mu);
                    if (pending.value(id) == call) erasePendingLocked(id);
                }
                sendCancel(id, methodName, generation); // capability-gated, connection-local
            }
            std::lock_guard<std::mutex> guard(call->mu);
            rsp.assign(call->data.begin(), call->data.end());
            (call->control ? controlResponseBytes : responseBytes).fetch_sub(call->responseReservation);
            call->responseReservation = 0;
            if (!ok || call->status != 0) {
                if (!call->data.isEmpty()) MW_show_log("[Core error] " + QString::fromUtf8(call->data));
                return 1;
            }
            return CallOK;
        }
    };
    namespace {
        // spb throws on malformed input; uncaught on a worker QThread that would terminate the process.
        template <typename T>
        bool tryDeserialize(const std::vector<uint8_t> &resp, T &out) {
            try {
                out = spb::pb::deserialize<T>(resp);
                return true;
            } catch (const std::exception &e) {
                MW_show_log(QString("[RPC] dropped malformed response: ") + e.what());
                return false;
            } catch (...) {
                MW_show_log("[RPC] dropped malformed response");
                return false;
            }
        }
    }

    Client::~Client() = default;

    Client::Client() {
        this->channel = std::make_unique<LocalSocketChannel>();
    }

    void Client::Reconnect(QLocalSocket *socket) {
        channel->Reconnect(socket);
    }

    libcore::RPCCapabilities Client::Capabilities() const { return channel->Capabilities(); }
    void Client::CancelPendingCalls() { channel->CancelPendingCalls(); }

#define NOT_OK      \
    *rpcOK = false; \
    MW_show_log(QString("IPC call failed (code %1)\n").arg(status));

    QString Client::Start(bool *rpcOK, const libcore::LoadConfigReq &request,
                          bool* tunAdapterRetryable, int timeoutMs, TunStartup::Diagnostic* diagnostic) {
        if (tunAdapterRetryable) *tunAdapterRetryable = false;
        if (diagnostic) *diagnostic = {};
        libcore::ErrorResp reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("Start", spb::pb::serialize<std::string>(request), resp, timeoutMs);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            if (tunAdapterRetryable) *tunAdapterRetryable = reply.tun_adapter_retryable.value_or(false);
            if (diagnostic && reply.tun_startup_diagnostic.has_value()) {
                const auto& info = *reply.tun_startup_diagnostic;
                const auto createCode = info.create_code.value_or(0);
                const auto openCode = info.open_code.value_or(0);
                const int reason = static_cast<int>(info.reason.value_or(libcore::TunStartupReason::TUN_DIAGNOSTIC_UNAVAILABLE));
                if (TunStartup::RecognizedCode(createCode) && TunStartup::RecognizedCode(openCode) && reason >= 0 && reason <= 4) {
                    *diagnostic = {true, info.tun_stage.value_or(false), info.configure_stage.value_or(false), createCode, openCode, reason};
                    diagnostic->version = info.diagnostic_version.value_or(0);
                    diagnostic->traversalTruncated = info.traversal_truncated.value_or(false) || info.causes.size() > TunStartup::MaxCauses;
                    for (const auto& cause : info.causes) {
                        if (diagnostic->causes.size() >= TunStartup::MaxCauses) break;
                        const int step = static_cast<int>(cause.step.value_or(libcore::TunStartupStep::TUN_STEP_UNKNOWN));
                        if (step < 0 || step > 8) { diagnostic->traversalTruncated = true; continue; }
                        diagnostic->causes.append({step, cause.win32_code});
                    }
                    const auto safe = TunStartup::SanitizeDetail(QString::fromStdString(info.error_detail.value_or("")));
                    diagnostic->errorDetail = safe.text;
                    diagnostic->detailTruncated = safe.truncated || info.detail_truncated.value_or(false);
                    diagnostic->detailRedacted = safe.redacted || info.detail_redacted.value_or(false);
                    diagnostic->coreBuild = TunStartup::SafeBuild(QString::fromStdString(info.core_build.value_or("")));
                }
            }
            const auto originalError = QString::fromStdString(reply.error.value());
            if (diagnostic && !originalError.isEmpty() && diagnostic->errorDetail.isEmpty()
                && ((diagnostic->available && diagnostic->tunStage && diagnostic->configureStage && diagnostic->reason != 3)
                    || (!reply.tun_startup_diagnostic.has_value() && originalError.contains("start inbound/tun[") && originalError.contains("configure tun interface")))) {
                const auto safe = TunStartup::SanitizeDetail(originalError);
                diagnostic->errorDetail = safe.text;
                diagnostic->detailTruncated = safe.truncated;
                diagnostic->detailRedacted = safe.redacted;
                if (diagnostic->coreBuild.isEmpty()) diagnostic->coreBuild = "unavailable";
            }
            return originalError;
        } else {
            NOT_OK
            return "";
        }
    }

    QString Client::Stop(bool *rpcOK) {
        libcore::EmptyReq request;
        libcore::ErrorResp reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("Stop", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return QString::fromStdString(reply.error.value());
        } else {
            NOT_OK
            return "";
        }
    }

    libcore::QueryStatsResp Client::QueryStats() {
        libcore::EmptyReq request;
        libcore::QueryStatsResp reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("QueryStats", spb::pb::serialize<std::string>(request), resp, 500);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            return reply;
        }
        return {};
    }

    libcore::TestResp Client::Test(bool *rpcOK, const libcore::TestReq &request, QString *coreError, int timeoutMs) {
        libcore::TestResp reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("Test", spb::pb::serialize<std::string>(request), resp, timeoutMs);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return reply;
        } else {
            if (coreError && !resp.empty())
                *coreError = QString::fromUtf8(reinterpret_cast<const char *>(resp.data()), static_cast<int>(resp.size()));
            NOT_OK
            return {};
        }
    }

    void Client::StopTests(bool *rpcOK) {
        const libcore::EmptyReq request;
        std::vector<uint8_t> resp;
        auto status = channel->Call("StopTest", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK) {
            *rpcOK = true;
        } else {
            NOT_OK
        }
    }

    libcore::QueryURLTestResponse Client::QueryURLTest(bool *rpcOK)
    {
        libcore::EmptyReq request;
        libcore::QueryURLTestResponse reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("QueryURLTest", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return reply;
        } else {
            NOT_OK
            return {};
        }
    }

    libcore::IPTestResp Client::IPTest(bool *rpcOK, const libcore::IPTestRequest &request, QString *coreError, int timeoutMs) {
        libcore::IPTestResp reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("IPTest", spb::pb::serialize<std::string>(request), resp, timeoutMs);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return reply;
        } else {
            if (coreError && !resp.empty())
                *coreError = QString::fromUtf8(reinterpret_cast<const char *>(resp.data()), static_cast<int>(resp.size()));
            NOT_OK
            return {};
        }
    }

    libcore::QueryIPTestResponse Client::QueryIPTest(bool *rpcOK) {
        libcore::EmptyReq request;
        libcore::QueryIPTestResponse reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("QueryIPTest", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return reply;
        } else {
            NOT_OK
            return {};
        }
    }

    libcore::GetDefaultInterfaceResponse Client::GetDefaultInterface(bool *rpcOK) const {
        libcore::EmptyReq request;
        libcore::GetDefaultInterfaceResponse reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("GetDefaultInterface", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return reply;
        } else {
            NOT_OK
            return {};
        }
    }

    libcore::QueryAutoSelectorsResponse Client::QueryAutoSelectors(bool *rpcOK) const {
        libcore::EmptyReq request;
        libcore::QueryAutoSelectorsResponse reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("QueryAutoSelectors", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return reply;
        } else {
            NOT_OK
            return {};
        }
    }

    QString Client::AutoSelectorAction(bool *rpcOK, const QString &tag, const QString &action,
                                       const QString &member) const {
        libcore::AutoSelectorActionRequest request;
        request.tag = tag.toStdString();
        request.action = action.toStdString();
        request.member = member.toStdString();
        libcore::ErrorResp reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("AutoSelectorAction", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return QString::fromStdString(reply.error.value());
        } else {
            NOT_OK
            return "IPC error";
        }
    }

    QString Client::SelectOutbound(bool *rpcOK, const QString &groupTag, const QString &outboundTag) const {
        libcore::SelectOutboundRequest request;
        request.group_tag = groupTag.toStdString();
        request.outbound_tag = outboundTag.toStdString();
        libcore::ErrorResp reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("SelectOutbound", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return QString::fromStdString(reply.error.value());
        } else {
            NOT_OK
            return "IPC error";
        }
    }

    libcore::VPNStatusResponse Client::QueryVPNStatus(bool *rpcOK, const QStringList &endpointTags,
                                                      int timeoutMs) const {
        libcore::VPNStatusRequest request;
        for (const auto &tag : endpointTags) request.endpoint_tags.push_back(tag.toStdString());
        request.timeout_ms = timeoutMs;
        libcore::VPNStatusResponse reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("QueryVPNStatus", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return reply;
        } else {
            NOT_OK
            return {};
        }
    }

    QString Client::SubmitVPNChallenge(bool *rpcOK, const QString &endpointTag, const QString &challengeId,
                                       const QString &username, const QString &password, const QString &secret,
                                       const QMap<QString, QString> &formValues) const {
        libcore::SubmitVPNChallengeRequest request;
        request.endpoint_tag = endpointTag.toStdString();
        request.challenge_id = challengeId.toStdString();
        request.username = username.toStdString();
        request.password = password.toStdString();
        request.secret = secret.toStdString();
        for (auto it = formValues.constBegin(); it != formValues.constEnd(); ++it)
            request.form_values[it.key().toStdString()] = it.value().toStdString();
        libcore::ErrorResp reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("SubmitVPNChallenge", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return QString::fromStdString(reply.error.value());
        } else {
            NOT_OK
            return "IPC error";
        }
    }

    QString Client::CancelVPNChallenge(bool *rpcOK, const QString &endpointTag, const QString &challengeId) const {
        libcore::SubmitVPNChallengeRequest request;
        request.endpoint_tag = endpointTag.toStdString();
        request.challenge_id = challengeId.toStdString();
        libcore::ErrorResp reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("CancelVPNChallenge", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return QString::fromStdString(reply.error.value());
        } else {
            NOT_OK
            return "IPC error";
        }
    }

    QString Client::SetSystemDNS(bool *rpcOK, const bool clear) const {
        libcore::SetSystemDNSRequest request{clear};
        std::vector<uint8_t> resp;
        auto status = channel->Call("SetSystemDNS", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK) {
            *rpcOK = true;
            return "";
        } else {
            NOT_OK
            return "IPC error";
        }
    }

    libcore::QueryConnectionsResp Client::QueryConnections(bool* rpcOK) const
    {
        libcore::EmptyReq request;
        libcore::QueryConnectionsResp reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("QueryConnections", spb::pb::serialize<std::string>(request), resp);

        if (rpcOK) *rpcOK = false;
        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            if (rpcOK) *rpcOK = true;
            return reply;
        }
        if (status != LocalSocketChannel::CallOK) MW_show_log("Failed to query connections: IPC error");
        return {};
    }

    libcore::QueryConnectionsDeltaResponse Client::QueryConnectionsDelta(bool* rpcOK, const libcore::QueryConnectionsDeltaRequest& request) const {
        *rpcOK = false;
        libcore::QueryConnectionsDeltaResponse reply;
        std::vector<uint8_t> resp;
        const auto status = channel->Call("QueryConnectionsDelta", spb::pb::serialize<std::string>(request), resp);
        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) *rpcOK = true;
        return reply;
    }

    QString Client::CloseConnections(bool *rpcOK, const QStringList &ids, int *closedCount) const
    {
        if (closedCount != nullptr) *closedCount = 0;
        libcore::CloseConnectionsRequest request;
        for (const auto &id : ids) request.ids.push_back(id.toStdString());
        libcore::CloseConnectionsResponse reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("CloseConnections", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            if (closedCount != nullptr) *closedCount = reply.closed.value();
            return QString::fromStdString(reply.error.value());
        } else {
            NOT_OK
            return "IPC error";
        }
    }

    QString Client::UpdateRuleSets(bool *rpcOK, int *updatedCount) const
    {
        if (updatedCount != nullptr) *updatedCount = 0;
        libcore::EmptyReq request;
        libcore::UpdateRuleSetsResponse reply;
        std::vector<uint8_t> resp;
        // Must outlast the core's 60 s deadline in rulesets.go.
        const int timeoutMs = 75000;
        auto status = channel->Call("UpdateRuleSets", spb::pb::serialize<std::string>(request), resp, timeoutMs);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            if (updatedCount != nullptr) *updatedCount = reply.updated.value();
            return QString::fromStdString(reply.error.value());
        } else {
            NOT_OK
            return "IPC error";
        }
    }

    QString Client::CheckConfig(bool* rpcOK, const QString& config, bool isXray) const
    {
        libcore::LoadConfigReq request;
        if (isXray)
        {
            request.need_xray = true;
            request.xray_config = config.toStdString();
        } else
        {
            request.core_config = config.toStdString();
        }
        libcore::ErrorResp reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("CheckConfig", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply))
        {
            *rpcOK = true;
            return QString::fromStdString(reply.error.value());
        } else
        {
            NOT_OK
            return "IPC error";
        }
    }

    bool Client::IsPrivileged(bool* rpcOK) const
    {
        libcore::EmptyReq request;
        libcore::IsPrivilegedResponse reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("IsPrivileged", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply))
        {
            *rpcOK = true;
            return reply.has_privilege.value();
        } else
        {
            NOT_OK
            return false;
        }
    }

    libcore::SpeedTestResponse Client::SpeedTest(bool *rpcOK, const libcore::SpeedTestRequest &request, QString *coreError)
    {
        libcore::SpeedTestResponse reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("SpeedTest", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return reply;
        } else {
            if (coreError && !resp.empty())
                *coreError = QString::fromUtf8(reinterpret_cast<const char *>(resp.data()), static_cast<int>(resp.size()));
            NOT_OK
            return {};
        }
    }

    libcore::QuerySpeedTestResponse Client::QueryCurrentSpeedTests(bool *rpcOK)
    {
        const libcore::EmptyReq request;
        libcore::QuerySpeedTestResponse reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("QuerySpeedTest", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return reply;
        } else {
            NOT_OK
            return {};
        }
    }

    libcore::QueryCountryTestResponse Client::QueryCountryTestResults(bool* rpcOK)
    {
        const libcore::EmptyReq request;
        libcore::QueryCountryTestResponse reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("QueryCountryTest", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return reply;
        } else {
            NOT_OK
            return {};
        }
    }

    libcore::GenWgKeyPairResponse Client::GenWgKeyPair(bool *rpcOK)
    {
        const libcore::EmptyReq request;
        libcore::GenWgKeyPairResponse reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("GenWgKeyPair", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return reply;
        } else {
            NOT_OK
            return {};
        }
    }

    libcore::WarpRegisterResponse Client::WarpRegister(bool *rpcOK, const QString &tunnelType, const QString &proxy,
                                                       const QStringList &apiHosts)
    {
        libcore::WarpRegisterRequest request;
        request.tunnel_type = tunnelType.toStdString();
        request.proxy = proxy.toStdString();
        for (const auto &host : apiHosts) request.api_hosts.push_back(host.toStdString());
        libcore::WarpRegisterResponse reply;
        std::vector<uint8_t> resp;
        // Must outlast the core's per-host budget in server_warp.go.
        const int timeoutMs = qMax(60000, 30000 + 25000 * static_cast<int>(apiHosts.size()));
        auto status = channel->Call("WarpRegister", spb::pb::serialize<std::string>(request), resp, timeoutMs);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return reply;
        } else {
            NOT_OK
            return {};
        }
    }

    QString Client::InstallDashboard(bool *rpcOK, const QString &archivePath, const QString &targetDir) const
    {
        libcore::InstallDashboardRequest request;
        request.archive_path = archivePath.toStdString();
        request.target_dir = targetDir.toStdString();
        libcore::ErrorResp reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("InstallDashboard", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return QString::fromStdString(reply.error.value());
        } else {
            NOT_OK
            return "IPC error";
        }
    }

    libcore::DiagnosticsResponse Client::CaptureDiagnostics(bool *rpcOK, const libcore::DiagnosticsRequest &request, int timeoutMs)
    {
        libcore::DiagnosticsResponse reply;
        std::vector<uint8_t> resp;
        auto status = channel->Call("CaptureDiagnostics", spb::pb::serialize<std::string>(request), resp, timeoutMs);

        if (status == LocalSocketChannel::CallOK && tryDeserialize(resp, reply)) {
            *rpcOK = true;
            return reply;
        } else {
            NOT_OK
            return {};
        }
    }

    void Client::StopDiagnostics(bool *rpcOK)
    {
        const libcore::EmptyReq request;
        std::vector<uint8_t> resp;
        auto status = channel->Call("StopDiagnostics", spb::pb::serialize<std::string>(request), resp);

        if (status == LocalSocketChannel::CallOK) {
            *rpcOK = true;
        } else {
            NOT_OK
        }
    }

} // namespace API
