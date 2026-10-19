#pragma once
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMutex>
#include <QScopeGuard>
#include <QUuid>
#include <atomic>
#include <algorithm>
#include "include/sys/TunStartupDiagnostic.hpp"

namespace TunStartup {
inline constexpr qint64 MaxRecordBytes = 2048;
inline constexpr qint64 MaxFileBytes = 256 * 1024;
enum class Outcome { Failed, Succeeded, Retry, Cancelled, Exhausted, Unknown, ResetRequested, ResetCompleted, ResetSkipped, StaleUnwound };
enum class WriteResult { Written, Disabled, Unavailable };
struct Record {
    int attempt = 0;
    int profileID = -1;
    quint64 generation = 0;
    quint64 launch = 0;
    qint64 elapsedMs = 0;
    bool rpcOK = false;
    bool retryable = false;
    Diagnostic diagnostic;
    Outcome outcome = Outcome::Failed;
};
inline const char* OutcomeName(Outcome outcome) {
    switch (outcome) {
    case Outcome::Succeeded: return "success";
    case Outcome::Retry: return "retry";
    case Outcome::Cancelled: return "cancelled";
    case Outcome::Exhausted: return "exhausted";
    case Outcome::Unknown: return "unknown";
    case Outcome::ResetRequested: return "reset_requested";
    case Outcome::ResetCompleted: return "reset_completed";
    case Outcome::ResetSkipped: return "reset_skipped";
    case Outcome::StaleUnwound: return "stale_unwound";
    default: return "failed";
    }
}
// No free-form payload API. In particular configs, node labels, error text and
// arbitrary paths cannot enter an encoded record. This writer never calls Logging.
class Journal {
    QString directory;
    const QString startID = QUuid::createUuid().toString(QUuid::WithoutBraces);
    inline static const QString sessionID = QUuid::createUuid().toString(QUuid::WithoutBraces);
    inline static QMutex mutex;
    inline static std::atomic<bool> warned = false;
public:
    explicit Journal(QString logDirectory) : directory(std::move(logDirectory)) {}
    QString Path() const { return QDir(directory).absoluteFilePath("tun-startup.jsonl"); }
    static bool ShouldWarnOnce() { return !warned.exchange(true); }
    WriteResult Append(const Record& record, bool enabled) const {
        if (!enabled) return WriteResult::Disabled;
        if (directory.isEmpty() || !mutex.tryLock()) return WriteResult::Unavailable;
        const auto unlock = qScopeGuard([] { mutex.unlock(); });
        if (!QDir().mkpath(directory)) return WriteResult::Unavailable;
        QLockFile lock(Path() + ".lock");
        if (!lock.tryLock(0)) return WriteResult::Unavailable;
        const auto& info = record.diagnostic;
        QJsonObject object{
            {"v", 1}, {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"session", sessionID}, {"start", startID}, {"pid", QString::number(QCoreApplication::applicationPid())},
            {"attempt", std::clamp(record.attempt, 0, 3)}, {"profile_id", record.profileID},
            {"generation", QString::number(record.generation)}, {"launch", QString::number(record.launch)},
            {"elapsed_ms", std::clamp<qint64>(record.elapsedMs, 0, 86400000)},
            {"rpc_ok", record.rpcOK}, {"retryable", record.retryable}, {"outcome", OutcomeName(record.outcome)},
            {"analysis_available", info.available}, {"tun_stage", info.tunStage}, {"configure_stage", info.configureStage},
            {"create_code", static_cast<int>(RecognizedCode(info.createCode) ? info.createCode : 0)},
            {"open_code", static_cast<int>(RecognizedCode(info.openCode) ? info.openCode : 0)},
            {"reason", std::clamp(info.reason, 0, 3)},
        };
        // Version is app-owned build metadata, never remote/error/config text.
        const auto version = QCoreApplication::applicationVersion();
        if (version.size() <= 64) object["app_build"] = version;
        auto line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
        if (line.size() > MaxRecordBytes) return WriteResult::Unavailable;
        QFile file(Path());
        bool partialTail = false;
        if (file.exists()) {
            if (!file.open(QIODevice::ReadOnly)) return WriteResult::Unavailable;
            if (file.size() > 0) {
                if (!file.seek(file.size() - 1)) return WriteResult::Unavailable;
                partialTail = file.read(1) != "\n";
            }
            file.close();
        }
        if (QFileInfo(Path()).size() + line.size() + (partialTail ? 1 : 0) > MaxFileBytes) {
            if (QFile::exists(Path() + ".1") && !QFile::remove(Path() + ".1")) return WriteResult::Unavailable;
            if (!QFile::rename(Path(), Path() + ".1")) return WriteResult::Unavailable;
            partialTail = false;
        }
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) return WriteResult::Unavailable;
        if (partialTail && file.write("\n") != 1) return WriteResult::Unavailable;
        if (file.write(line) != line.size() || !file.flush()) return WriteResult::Unavailable;
        return WriteResult::Written;
    }
};
}
