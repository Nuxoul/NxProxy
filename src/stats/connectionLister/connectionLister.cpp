#include <QThread>
#include <QDateTime>
#include <QUuid>
#include <core/gen/libcore.pb.h>
#include <include/api/RPC.h>
#include "include/ui/mainwindow_interface.h"
#include <include/stats/connections/connectionLister.hpp>
#include "include/stats/traffic/TrafficStatsManager.hpp"
#include "include/global/LocalNetwork.hpp"

#include <algorithm>



namespace Stats
{
    QMutex outboundDisplayMu;
    QMap<QString, QString> outboundDisplayNames;

    void SetOutboundDisplayNames(const QMap<QString, QString> &tagToDisplay)
    {
        QMutexLocker lk(&outboundDisplayMu);
        outboundDisplayNames = tagToDisplay;
    }

    QString OutboundDisplayName(const QString &tag)
    {
        QMutexLocker lk(&outboundDisplayMu);
        return outboundDisplayNames.value(tag, tag);
    }
    QString ConnectionOutboundDisplayName(const QString& outbound, const QStringList& chain)
    {
        QMutexLocker lock(&outboundDisplayMu);
        QString group, node;
        // sing-box records leaf-to-selector chain order. Select the actual group
        // traversed by this connection and the actual leaf, never a group's current pick.
        for (const auto& tag : chain) {
            const auto label = outboundDisplayNames.value(tag);
            if (tag.startsWith("selector-") && !label.isEmpty()) group = label;
            else if (node.isEmpty() && !label.isEmpty()) node = label;
        }
        const auto leaf = outboundDisplayNames.value(outbound, outbound);
        if (node.isEmpty()) node = leaf;
        // The independent legacy path already labels its member "group -> node".
        if (node.contains(QStringLiteral(" → "))) return node;
        if (!group.isEmpty() && group != node) return group + QStringLiteral(" → ") + node;
        return node;
    }

    // Guards out-of-band ForceUpdate() polls: a tiny byte delta over a tiny interval reads as a spike.
    static constexpr qint64 kSpeedSampleMinMs = 500;

    // Off-view polls still run: they keep the per-app traffic stats sampled.
    static constexpr unsigned long kActivePollMs = 1000;
    static constexpr unsigned long kRelaxedPollMs = 5000;

    ConnectionLister* connection_lister = new ConnectionLister();

    void ConnectionLister::ForceUpdate()
    {
        QMutexLocker wlk(&waitMu_);
        forced_ = true;
        waitCond_.wakeAll();
    }


    void ConnectionLister::Loop()
    {
        while (true)
        {
            if (stop) return;

            bool forced = false;
            {
                QMutexLocker wlk(&waitMu_);
                if (!forced_) waitCond_.wait(&waitMu_, inView_.load() ? kActivePollMs : kRelaxedPollMs);
                forced = forced_;
                forced_ = false;
            }

            if (stop) return;
            // A forced poll is a user action on the table, so it runs even while stats are off.
            if (!forced && (suspend || !Configs::dataManager->settingsRepo->enable_stats)) continue;

            mu.lock();
            update(forced || inView_.load());
            mu.unlock();
        }
    }

    void ConnectionLister::SetInView(bool inView)
    {
        const bool was = inView_.exchange(inView);
        if (inView && !was)
        {
            QMutexLocker wlk(&waitMu_);
            waitCond_.wakeAll();
        }
    }

    QString EndpointHost(const QString& endpoint)
    {
        if (endpoint.startsWith('['))
        {
            const auto close = endpoint.indexOf(']');
            return close > 0 ? endpoint.mid(1, close - 1) : endpoint.mid(1);
        }
        const auto colon = endpoint.lastIndexOf(':');
        return colon < 0 ? endpoint : endpoint.left(colon);
    }

    static ConnectionMetadata metaFromProto(const libcore::ConnectionMetaData& conn, const QString& localLabel)
    {
        ConnectionMetadata c;
        c.id = QString::fromStdString(conn.id.value());
        for (const auto& tag : conn.chain) c.chain.append(QString::fromStdString(tag));
        c.createdAtMs = conn.created_at.value();
        c.dest = QString::fromStdString(conn.dest.value());
        c.upload = conn.upload.value();
        c.download = conn.download.value();
        c.domain = QString::fromStdString(conn.domain.value());
        c.network = QString::fromStdString(conn.network.value());
        c.outbound = QString::fromStdString(conn.outbound.value());
        c.outboundDisplay = ConnectionOutboundDisplayName(c.outbound, c.chain);
        c.process = QString::fromStdString(conn.process.value());
        c.processPath = QString::fromStdString(conn.process_path.value());
        c.protocol = QString::fromStdString(conn.protocol.value());
        c.closedAtMs = conn.closed_at.value();
        c.source = QString::fromStdString(conn.source.value());
        // In tun mode our own traffic enters with the tun's (or this machine's LAN) address, so a loopback-only test would label it a LAN client.
        const QString host = EndpointHost(c.source);
        c.sourceDisplay = host.isEmpty() || LocalNetwork::IsOwnAddress(host) ? localLabel : host;
        return c;
    }

    namespace
    {
        // The core iterates a map, so orderings must be total or equal keys reshuffle every poll.
        template <typename Key>
        void sortLargestFirst(QList<ConnectionMetadata>& list, bool asc, Key key)
        {
            std::sort(list.begin(), list.end(), [&](const ConnectionMetadata& a, const ConnectionMetadata& b)
            {
                const auto& ka = key(a);
                const auto& kb = key(b);
                if (ka == kb) return asc ? a.id > b.id : a.id < b.id;
                return asc ? ka < kb : ka > kb;
            });
        }

        // Text columns read the other way round: unflipped means A→Z, not Z→A.
        template <typename Key>
        void sortSmallestFirst(QList<ConnectionMetadata>& list, bool asc, Key key)
        {
            std::sort(list.begin(), list.end(), [&](const ConnectionMetadata& a, const ConnectionMetadata& b)
            {
                const auto& ka = key(a);
                const auto& kb = key(b);
                if (ka == kb) return asc ? a.id > b.id : a.id < b.id;
                return asc ? ka > kb : ka < kb;
            });
        }
    }

    bool SortIsDescending(const ConnectionSort sort, const bool ascending)
    {
        switch (sort)
        {
        case Default:
            return false;
        case ByProcess:
        case ByOutbound:
        case ByProtocol:
        case BySource:
            return ascending;
        default:
            return !ascending;
        }
    }

    void ConnectionLister::update(const bool pushToUi)
    {
        if (snapshotInvalidated_.exchange(false)) {
            connectionSnapshot_.reset(); deltaClientID_.clear(); metadataCache_.clear();
        }
        libcore::QueryConnectionsResp resp;
        bool rpcOK = false;
        if (API::defaultClient->Capabilities().connection_delta.value_or(false)) {
            if (deltaClientID_.isEmpty()) deltaClientID_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
            auto delta = API::defaultClient->QueryConnectionsDelta(&rpcOK, connectionSnapshot_.request(deltaClientID_));
            if (rpcOK && connectionSnapshot_.apply(delta)) resp = connectionSnapshot_.snapshot();
            else {
                connectionSnapshot_.reset();
                resp = API::defaultClient->QueryConnections(&rpcOK);
            }
        } else {
            connectionSnapshot_.reset();
            resp = API::defaultClient->QueryConnections(&rpcOK);
        }
        // Failed transport is not an empty snapshot; preserve the accounting baseline.
        if (!rpcOK) return;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const QString localLabel = QObject::tr("Local");

        QList<ConnectionMetadata> rows;
        rows.reserve(static_cast<qsizetype>(resp.active.size()));
        QHash<QString, SpeedSample> newSamples;
        newSamples.reserve(static_cast<qsizetype>(resp.active.size()));
        QHash<QString, ConnectionMetadata> newMetadata;
        newMetadata.reserve(static_cast<qsizetype>(resp.active.size()));

        for (const auto& conn : resp.active)
        {
            const auto id = QString::fromStdString(conn.id.value());
            auto cached = metadataCache_.constFind(id);
            auto c = cached == metadataCache_.cend() ? metaFromProto(conn, localLabel) : *cached;
            c.upload = conn.upload.value(); c.download = conn.download.value();
            c.closedAtMs = conn.closed_at.value();
            c.outboundDisplay = ConnectionOutboundDisplayName(c.outbound, c.chain);
            newMetadata.insert(id, c);

            SpeedSample s;
            if (const auto it = speedSamples_.constFind(c.id); it != speedSamples_.constEnd())
            {
                const qint64 dt = nowMs - it->sampledAtMs;
                if (dt >= kSpeedSampleMinMs)
                {
                    qint64 dUp = c.upload - it->upload;
                    qint64 dDown = c.download - it->download;
                    if (dUp < 0) dUp = 0; // counters only grow; guard against any reset
                    if (dDown < 0) dDown = 0;
                    s.upload = c.upload;
                    s.download = c.download;
                    s.sampledAtMs = nowMs;
                    s.upSpeed = dUp * 1000 / dt;
                    s.downSpeed = dDown * 1000 / dt;
                }
                else
                {
                    s = *it; // window too short: keep last rate and baseline
                }
            }
            else
            {
                s.upload = c.upload; // first sighting: seed baseline, no rate yet
                s.download = c.download;
                s.sampledAtMs = nowMs;
            }
            newSamples.insert(c.id, s);
            c.uploadSpeed = s.upSpeed;
            c.downloadSpeed = s.downSpeed;

            rows.append(std::move(c));
        }
        speedSamples_ = newSamples; // drop ids for connections that have closed
        metadataCache_ = std::move(newMetadata); // bounded by the current active set only

        // Credits the live set plus the recently-closed ring (deduped by id), so a connection that opened and closed between polls still counts.
        if (!Configs::dataManager->settingsRepo->disable_traffic_stats)
        {
            accounting_.consume(resp, [&](const libcore::ConnectionMetaData& cm, qint64 up, qint64 down) {
                QString name = QString::fromStdString(cm.process.value());
                if (name.isEmpty()) name = "Unknown";
                trafficStatsManager->AddAppDelta(name, QString::fromStdString(cm.process_path.value()), up, down);
            });
        }

        if (!pushToUi) return;

        switch (sort)
        {
        // Oldest first, matching how rows used to accumulate. `asc` is ignored: this means "unsorted".
        case Default:
            sortSmallestFirst(rows, false, [](const ConnectionMetadata& c) { return c.createdAtMs; });
            break;
        case ByDownload:
            sortLargestFirst(rows, asc, [](const ConnectionMetadata& c) { return c.download; });
            break;
        case ByUpload:
            sortLargestFirst(rows, asc, [](const ConnectionMetadata& c) { return c.upload; });
            break;
        case ByTraffic:
            sortLargestFirst(rows, asc, [](const ConnectionMetadata& c) { return c.upload + c.download; });
            break;
        case ByDownloadSpeed:
            sortLargestFirst(rows, asc, [](const ConnectionMetadata& c) { return c.downloadSpeed; });
            break;
        case ByUploadSpeed:
            sortLargestFirst(rows, asc, [](const ConnectionMetadata& c) { return c.uploadSpeed; });
            break;
        case BySpeed:
            sortLargestFirst(rows, asc, [](const ConnectionMetadata& c) { return c.uploadSpeed + c.downloadSpeed; });
            break;
        case ByProcess:
            sortSmallestFirst(rows, asc, [](const ConnectionMetadata& c) -> const QString& { return c.process; });
            break;
        case ByOutbound:
            sortSmallestFirst(rows, asc, [](const ConnectionMetadata& c) -> const QString& { return c.outboundDisplay; });
            break;
        case ByProtocol:
            sortSmallestFirst(rows, asc, [](const ConnectionMetadata& c) -> const QString& { return c.protocol; });
            break;
        case BySource:
            sortSmallestFirst(rows, asc, [](const ConnectionMetadata& c) -> const QString& { return c.sourceDisplay; });
            break;
        }

        publishLatest(std::move(rows));
    }

    void ConnectionLister::publishLatest(QList<ConnectionMetadata> rows)
    {
        {
            QMutexLocker lock(&uiSnapshotMu_);
            latestRows_ = std::move(rows);
            if (uiUpdateQueued_) return;
            uiUpdateQueued_ = true;
        }
        runOnUiThread([this] {
            QList<ConnectionMetadata> current;
            {
                QMutexLocker lock(&uiSnapshotMu_);
                current.swap(latestRows_);
                uiUpdateQueued_ = false;
            }
            if (Configs::dataManager->settingsRepo->prepare_exit) return;
            if (auto* m = GetMainWindow()) m->UpdateConnectionList(current);
        });
    }

    void ConnectionLister::stopLoop()
    {
        stop = true;
        QMutexLocker wlk(&waitMu_);
        waitCond_.wakeAll();
    }

    void ConnectionLister::setSort(const ConnectionSort newSort)
    {
        if (sort == newSort) asc = !asc;
        else
        {
            sort = newSort;
            asc = false;
        }
    }

    void ConnectionLister::restoreSort(const ConnectionSort newSort, const bool ascending)
    {
        sort = newSort;
        asc = ascending;
    }

}
