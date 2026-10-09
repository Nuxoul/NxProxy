#pragma once
#include <core/gen/libcore.pb.h>
#include <QHash>
#include <QString>

namespace Stats {
// The poll worker is the single owner. Absolute counters reconstruct the legacy
// non-draining snapshot; UI coalescing is deliberately outside this boundary.
class ConnectionSnapshot {
public:
    libcore::QueryConnectionsDeltaRequest request(const QString& client) const {
        libcore::QueryConnectionsDeltaRequest req;
        req.client_id = client.toStdString(); req.epoch = epoch_; req.cursor = cursor_;
        return req;
    }
    void reset() { records_.clear(); epoch_ = cursor_ = 0; }
    bool apply(const libcore::QueryConnectionsDeltaResponse& delta) {
        const bool full = delta.full.value_or(false);
        const auto epoch = delta.epoch.value_or(0), cursor = delta.cursor.value_or(0);
        if (!full && (epoch != epoch_ || cursor != cursor_ + 1)) return false;
        if (!cursor) return false;
        // Validate before mutation: a bad response must not leave a partial cache.
        if (!full) {
            for (const auto& value : delta.changed) {
                const auto id = QString::fromStdString(value.id.value_or(""));
                if (!records_.contains(id)) return false;
            }
        }
        if (full) records_.clear();
        for (const auto& id : delta.removed) records_.remove(QString::fromStdString(id));
        for (const auto& value : delta.added) {
            const auto id = QString::fromStdString(value.id.value_or(""));
            if (!id.isEmpty()) records_.insert(id, value);
        }
        for (const auto& value : delta.changed) {
            auto it = records_.find(QString::fromStdString(value.id.value_or("")));
            if (it == records_.end()) { reset(); return false; }
            it->upload = value.upload; it->download = value.download; it->closed_at = value.closed_at;
        }
        if (records_.size() > 10000) { reset(); return false; }
        epoch_ = epoch; cursor_ = cursor;
        return true;
    }
    libcore::QueryConnectionsResp snapshot() const {
        libcore::QueryConnectionsResp result;
        for (const auto& record : records_) {
            if (record.closed_at.value_or(0) == 0) result.active.push_back(record);
            else result.closed.push_back(record);
        }
        return result;
    }
private:
    QHash<QString, libcore::ConnectionMetaData> records_;
    uint64_t epoch_ = 0, cursor_ = 0;
};
}
