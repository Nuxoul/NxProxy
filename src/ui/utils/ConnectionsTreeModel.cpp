#include "include/ui/utils/ConnectionsTreeModel.h"
#include "include/global/Utils.hpp"

#include <QHash>

#include <algorithm>

using ConnectionsTree::ConnectionLeafItem;
using ConnectionsTree::ProcessGroupItem;
using ConnectionsTree::TreeItem;

namespace {
    using Groups = std::vector<std::unique_ptr<ProcessGroupItem>>;

    TreeItem *itemOf(const QModelIndex &index) {
        return static_cast<TreeItem *>(index.internalPointer());
    }

    template <typename Item, typename Key>
    void sortItems(std::vector<std::unique_ptr<Item>> &items, bool descending, Key key) {
        std::stable_sort(items.begin(), items.end(), [&](const std::unique_ptr<Item> &a, const std::unique_ptr<Item> &b) {
            return descending ? key(*b) < key(*a) : key(*a) < key(*b);
        });
    }

    template <typename GroupKey, typename LeafKey>
    void sortTree(Groups &groups, bool descending, GroupKey groupKey, LeafKey leafKey) {
        sortItems(groups, descending, groupKey);
        for (auto &group : groups) sortItems(group->children, descending, leafKey);
    }
}

ConnectionsTreeModel::ConnectionsTreeModel(QObject *parent)
    : QAbstractItemModel(parent) {}

QModelIndex ConnectionsTreeModel::indexOf(TreeItem *item, int column) const {
    return createIndex(item->row, column, item);
}

QModelIndex ConnectionsTreeModel::index(int row, int column, const QModelIndex &parent) const {
    if (row < 0 || column < 0 || column >= ColumnCount) return {};
    if (!parent.isValid()) {
        if (row >= static_cast<int>(m_groups.size())) return {};
        return indexOf(m_groups[row].get(), column);
    }
    auto *item = itemOf(parent);
    if (parent.column() != 0 || !item->isProcess()) return {};
    const auto &children = static_cast<ProcessGroupItem *>(item)->children;
    if (row >= static_cast<int>(children.size())) return {};
    return indexOf(children[row].get(), column);
}

QModelIndex ConnectionsTreeModel::parent(const QModelIndex &child) const {
    if (!child.isValid()) return {};
    auto *item = itemOf(child);
    if (item->isProcess()) return {};
    return indexOf(static_cast<ConnectionLeafItem *>(item)->parent, 0);
}

int ConnectionsTreeModel::rowCount(const QModelIndex &parent) const {
    if (!parent.isValid()) return static_cast<int>(m_groups.size());
    auto *item = itemOf(parent);
    if (parent.column() != 0 || !item->isProcess()) return 0;
    return static_cast<int>(static_cast<ProcessGroupItem *>(item)->children.size());
}

int ConnectionsTreeModel::columnCount(const QModelIndex &) const {
    return ColumnCount;
}

Qt::ItemFlags ConnectionsTreeModel::flags(const QModelIndex &index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    const Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    return itemOf(index)->isProcess() ? flags : flags | Qt::ItemNeverHasChildren;
}

QString ConnectionsTreeModel::displayProcessName(const QString &processName) {
    return processName.isEmpty() ? tr("System") : processName;
}

QVariant ConnectionsTreeModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid()) return {};
    auto *item = itemOf(index);

    if (item->isProcess()) {
        const auto *group = static_cast<const ProcessGroupItem *>(item);
        switch (role) {
        case IsProcessRole:
            return true;
        case ProcessNameRole:
            return group->processName;
        case ConnIdsRole:
            return group->connectionIds();
        case Qt::DisplayRole:
            switch (index.column()) {
            case ColTarget:
                return QStringLiteral("%1 (%2)").arg(displayProcessName(group->processName), QString::number(group->totalConnections));
            case ColSource:
                return group->sameSource ? group->commonSource : QStringLiteral("-");
            case ColProtocol:
                return QStringLiteral("-");
            case ColOutbound:
                return group->sameOutbound ? group->commonOutbound : QStringLiteral("-");
            case ColTraffic:
                return ReadableSize(group->totalUpload) + "↑ " + ReadableSize(group->totalDownload) + "↓";
            case ColSpeed:
                return ReadableSize(group->totalUploadSpeed) + "/s↑ " + ReadableSize(group->totalDownloadSpeed) + "/s↓";
            default:
                return {};
            }
        case Qt::ToolTipRole:
            if (index.column() != ColTarget) return {};
            return tr("Process: %1\nActive connections: %2\nTotal traffic: %3↑ %4↓\nTotal speed: %5/s↑ %6/s↓")
                .arg(displayProcessName(group->processName), QString::number(group->totalConnections),
                     ReadableSize(group->totalUpload), ReadableSize(group->totalDownload),
                     ReadableSize(group->totalUploadSpeed), ReadableSize(group->totalDownloadSpeed));
        default:
            return {};
        }
    }

    const auto *leaf = static_cast<const ConnectionLeafItem *>(item);
    switch (role) {
    case IsProcessRole:
        return false;
    case ProcessNameRole:
        return leaf->parent->processName;
    case ConnIdsRole:
        return leaf->connectionIds;
    case Qt::DisplayRole:
        switch (index.column()) {
        case ColTarget:
            return leaf->count > 1 ? QStringLiteral("%1 (%2)").arg(leaf->destText, QString::number(leaf->count)) : leaf->destText;
        case ColSource:
            return leaf->meta.sourceDisplay;
        case ColProtocol:
            return leaf->protocolText;
        case ColOutbound:
            return leaf->meta.outboundDisplay;
        case ColTraffic:
            return ReadableSize(leaf->upload) + "↑ " + ReadableSize(leaf->download) + "↓";
        case ColSpeed:
            return ReadableSize(leaf->uploadSpeed) + "/s↑ " + ReadableSize(leaf->downloadSpeed) + "/s↓";
        default:
            return {};
        }
    case Qt::ToolTipRole: {
        if (index.column() != ColTarget) return {};
        const QString process = displayProcessName(leaf->parent->processName);
        if (leaf->count > 1) {
            return tr("Destination: %1\nConnections: %2\nProcess: %3\nProtocol: %4\nStrategy / node: %5\nCore outbound: %6\nTotal traffic: %7↑ %8↓\nTotal speed: %9/s↑ %10/s↓")
                .arg(leaf->destText, QString::number(leaf->count), process, leaf->protocolText, leaf->meta.outboundDisplay, leaf->meta.outbound,
                     ReadableSize(leaf->upload), ReadableSize(leaf->download),
                     ReadableSize(leaf->uploadSpeed), ReadableSize(leaf->downloadSpeed));
        }
        return tr("Destination: %1\nProcess: %2\nProtocol: %3\nStrategy / node: %4\nCore outbound: %5")
            .arg(leaf->destText, process, leaf->protocolText, leaf->meta.outboundDisplay, leaf->meta.outbound);
    }
    default:
        return {};
    }
}

QVariant ConnectionsTreeModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal) return {};

    if (role == Qt::DisplayRole) {
        switch (section) {
        case ColTarget: return tr("Process / Destination");
        case ColSource: return tr("Source");
        case ColProtocol: return tr("Protocol");
        case ColOutbound: return tr("Outbound");
        case ColTraffic: return tr("Traffic");
        case ColSpeed: return tr("Speed");
        default: return {};
        }
    }

    if (role == Qt::ToolTipRole) {
        switch (section) {
        case ColTarget: return tr("Click To Sort By Process");
        case ColSource: return tr("Click To Sort By Source");
        case ColProtocol: return tr("Click To Sort By Protocol");
        case ColOutbound: return tr("Click To Sort By Outbound");
        case ColTraffic: return tr("Click to sort by traffic; right-click to choose total/down/up");
        case ColSpeed: return tr("Click to sort by speed; right-click to choose total/down/up");
        default: return {};
        }
    }

    return {};
}

bool ConnectionsTreeModel::updateValuesIfTopologyMatches(const QList<Stats::ConnectionMetadata>& connections,
                                                         Stats::ConnectionSort sort, bool ascending) {
    if (m_groups.empty() || m_lastSort != sort || m_lastAscending != ascending ||
        connections.size() != m_valueRows.size()) return false;
    // Numeric sorting may reorder rows. Fixed topology/order is the only fast path.
    if (sort == Stats::ByTraffic || sort == Stats::ByDownload || sort == Stats::ByUpload ||
        sort == Stats::BySpeed || sort == Stats::ByDownloadSpeed || sort == Stats::ByUploadSpeed) return false;
    for (qsizetype i = 0; i < connections.size(); ++i) {
        const auto& a = connections[i]; const auto& b = m_valueRows[i];
        if (a.id != b.id || a.process != b.process || a.dest != b.dest || a.domain != b.domain ||
            a.network != b.network || a.protocol != b.protocol || a.outbound != b.outbound ||
            a.chain != b.chain || a.sourceDisplay != b.sourceDisplay) return false;
    }
    for (auto& group : m_groups) {
        group->totalUpload = group->totalDownload = group->totalUploadSpeed = group->totalDownloadSpeed = 0;
        group->sameOutbound = true;
        for (auto& leaf : group->children) {
            leaf->count = 0;
            leaf->upload = leaf->download = leaf->uploadSpeed = leaf->downloadSpeed = 0;
        }
    }
    for (qsizetype i = 0; i < connections.size(); ++i) {
        const auto& c = connections[i]; auto* leaf = m_valueLeaves[i]; auto* group = leaf->parent;
        if (leaf->count++ == 0) leaf->meta = c;
        leaf->upload += c.upload; leaf->download += c.download;
        leaf->uploadSpeed += c.uploadSpeed; leaf->downloadSpeed += c.downloadSpeed;
        group->totalUpload += c.upload; group->totalDownload += c.download;
        group->totalUploadSpeed += c.uploadSpeed; group->totalDownloadSpeed += c.downloadSpeed;
    }
    for (auto& group : m_groups) {
        if (!group->children.empty()) group->commonOutbound = group->children.front()->meta.outboundDisplay;
        for (const auto& leaf : group->children)
            group->sameOutbound &= group->commonOutbound == leaf->meta.outboundDisplay;
        const auto parent = indexOf(group.get(), 0);
        if (!group->children.empty()) emit dataChanged(index(0, 0, parent), index(static_cast<int>(group->children.size()) - 1, ColumnCount - 1, parent));
        emit dataChanged(parent, indexOf(group.get(), ColumnCount - 1));
    }
    m_valueRows = connections;
    return true;
}

void ConnectionsTreeModel::setConnections(const QList<Stats::ConnectionMetadata> &connections,
                                          Stats::ConnectionSort sort, bool ascending) {
    if (updateValuesIfTopologyMatches(connections, sort, ascending)) return;
    m_lastSort = sort; m_lastAscending = ascending;
    Groups groups;
    QHash<QString, ProcessGroupItem *> groupByName;
    QHash<QString, ConnectionLeafItem *> leafByKey;
    QList<ConnectionLeafItem*> valueLeaves;
    valueLeaves.reserve(connections.size());

    for (const auto &c : connections) {
        const QString process = c.process.trimmed();
        ProcessGroupItem *&group = groupByName[process];
        if (group == nullptr) {
            group = groups.emplace_back(std::make_unique<ProcessGroupItem>()).get();
            group->processName = process;
            group->commonOutbound = c.outboundDisplay;
            group->commonSource = c.sourceDisplay;
        }
        group->sameOutbound = group->sameOutbound && group->commonOutbound == c.outboundDisplay;
        group->sameSource = group->sameSource && group->commonSource == c.sourceDisplay;
        group->totalConnections++;
        group->totalUpload += c.upload;
        group->totalDownload += c.download;
        group->totalUploadSpeed += c.uploadSpeed;
        group->totalDownloadSpeed += c.downloadSpeed;

        const QString dest = DisplayDest(c.dest, c.domain);
        const QString protocol = c.protocol.isEmpty() ? c.network : c.network + " (" + c.protocol + ")";
        const QString key = dest + "\t" + protocol + "\t" + c.outbound + "\t" + c.chain.join(QChar(0x1f)) + "\t" + c.sourceDisplay;
        ConnectionLeafItem *&leaf = leafByKey[process + "\n" + key];
        if (leaf == nullptr) {
            leaf = group->children.emplace_back(std::make_unique<ConnectionLeafItem>()).get();
            leaf->parent = group;
            leaf->meta = c;
            leaf->key = key;
            leaf->destText = dest;
            leaf->protocolText = protocol;
        }
        valueLeaves.append(leaf);
        leaf->count++;
        if (!c.id.isEmpty()) leaf->connectionIds.append(c.id);
        leaf->upload += c.upload;
        leaf->download += c.download;
        leaf->uploadSpeed += c.uploadSpeed;
        leaf->downloadSpeed += c.downloadSpeed;
    }

    // Rows arrive in the lister's order, which already implements Default, Source, Protocol and Outbound.
    const bool descending = Stats::SortIsDescending(sort, ascending);
    switch (sort) {
    case Stats::ByTraffic:
        sortTree(groups, descending, [](const ProcessGroupItem &g) { return g.totalUpload + g.totalDownload; },
                 [](const ConnectionLeafItem &l) { return l.upload + l.download; });
        break;
    case Stats::ByDownload:
        sortTree(groups, descending, [](const ProcessGroupItem &g) { return g.totalDownload; },
                 [](const ConnectionLeafItem &l) { return l.download; });
        break;
    case Stats::ByUpload:
        sortTree(groups, descending, [](const ProcessGroupItem &g) { return g.totalUpload; },
                 [](const ConnectionLeafItem &l) { return l.upload; });
        break;
    case Stats::BySpeed:
        sortTree(groups, descending, [](const ProcessGroupItem &g) { return g.totalUploadSpeed + g.totalDownloadSpeed; },
                 [](const ConnectionLeafItem &l) { return l.uploadSpeed + l.downloadSpeed; });
        break;
    case Stats::ByDownloadSpeed:
        sortTree(groups, descending, [](const ProcessGroupItem &g) { return g.totalDownloadSpeed; },
                 [](const ConnectionLeafItem &l) { return l.downloadSpeed; });
        break;
    case Stats::ByUploadSpeed:
        sortTree(groups, descending, [](const ProcessGroupItem &g) { return g.totalUploadSpeed; },
                 [](const ConnectionLeafItem &l) { return l.uploadSpeed; });
        break;
    case Stats::ByProcess:
        sortTree(groups, descending, [](const ProcessGroupItem &g) -> const QString & { return g.processName; },
                 [](const ConnectionLeafItem &l) -> const QString & { return l.destText; });
        break;
    default:
        break;
    }

    for (size_t g = 0; g < groups.size(); ++g) {
        groups[g]->row = static_cast<int>(g);
        auto &children = groups[g]->children;
        for (size_t l = 0; l < children.size(); ++l) children[l]->row = static_cast<int>(l);
    }

    // Moving persistent indexes by key instead of resetting keeps selection, expansion and a half-finished click.
    emit layoutAboutToBeChanged();
    const QModelIndexList from = persistentIndexList();
    QModelIndexList to;
    to.reserve(from.size());
    for (const QModelIndex &index : from) {
        const auto *item = itemOf(index);
        TreeItem *moved = nullptr;
        if (item->isProcess()) {
            moved = groupByName.value(static_cast<const ProcessGroupItem *>(item)->processName);
        } else {
            const auto *leaf = static_cast<const ConnectionLeafItem *>(item);
            moved = leafByKey.value(leaf->parent->processName + "\n" + leaf->key);
        }
        to.append(moved != nullptr ? indexOf(moved, index.column()) : QModelIndex());
    }
    m_groups.swap(groups);
    m_valueRows = connections;
    m_valueLeaves = std::move(valueLeaves);
    changePersistentIndexList(from, to);
    emit layoutChanged();
}

bool ConnectionsTreeModel::isProcessIndex(const QModelIndex &index) const {
    return index.isValid() && itemOf(index)->isProcess();
}

QString ConnectionsTreeModel::processNameAt(const QModelIndex &index) const {
    if (!index.isValid()) return {};
    auto *item = itemOf(index);
    if (item->isProcess()) return static_cast<ProcessGroupItem *>(item)->processName;
    return static_cast<ConnectionLeafItem *>(item)->parent->processName;
}

const Stats::ConnectionMetadata *ConnectionsTreeModel::metaAt(const QModelIndex &index) const {
    if (!index.isValid() || itemOf(index)->isProcess()) return nullptr;
    return &static_cast<ConnectionLeafItem *>(itemOf(index))->meta;
}

QStringList ConnectionsTreeModel::connectionIdsAt(const QModelIndex &index) const {
    if (!index.isValid()) return {};
    auto *item = itemOf(index);
    if (item->isProcess()) return static_cast<ProcessGroupItem *>(item)->connectionIds();
    return static_cast<ConnectionLeafItem *>(item)->connectionIds;
}

const ProcessGroupItem *ConnectionsTreeModel::groupAt(int row) const {
    if (row < 0 || row >= static_cast<int>(m_groups.size())) return nullptr;
    return m_groups[row].get();
}
