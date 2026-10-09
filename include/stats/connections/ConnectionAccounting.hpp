#pragma once
#include <core/gen/libcore.pb.h>
#include <QHash>
#include <QSet>
#include <QPair>
#include <QString>
#include <algorithm>
namespace Stats {
class ConnectionAccounting {
public:
    template<class Credit>
    void consume(const libcore::QueryConnectionsResp& snapshot, Credit credit) {
        QHash<QString, QPair<qint64,qint64>> nextBytes;
        QSet<QString> nextClosed;
        auto apply = [&](const libcore::ConnectionMetaData& record) {
            const auto id = QString::fromStdString(record.id.value_or(""));
            const auto baseline = lastBytes_.value(id);
            const auto up = std::max<qint64>(0, record.upload.value_or(0) - baseline.first);
            const auto down = std::max<qint64>(0, record.download.value_or(0) - baseline.second);
            if (up || down) credit(record, up, down);
        };
        for (const auto& record : snapshot.active) {
            const auto id = QString::fromStdString(record.id.value_or(""));
            if (id.isEmpty() || nextBytes.contains(id)) continue;
            apply(record);
            nextBytes.insert(id,{record.upload.value_or(0),record.download.value_or(0)});
        }
        for (const auto& record : snapshot.closed) {
            const auto id = QString::fromStdString(record.id.value_or(""));
            // A tracker seen active in this same sample is not finally accounted.
            if (id.isEmpty() || nextBytes.contains(id) || nextClosed.contains(id)) continue;
            nextClosed.insert(id);
            if (!accountedClosed_.contains(id)) apply(record);
        }
        lastBytes_ = std::move(nextBytes);
        accountedClosed_ = std::move(nextClosed);
    }
private:
    QHash<QString,QPair<qint64,qint64>> lastBytes_;
    QSet<QString> accountedClosed_;
};
}
