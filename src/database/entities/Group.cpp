#include <include/database/entities/Group.h>

#include "include/database/ProfilesRepo.h"
#include "include/global/Configs.hpp"

#include <algorithm>
#include <QHash>

namespace Configs
{
    void Group::clearCalculatedColumnWidth() {
        calculated_column_width.clear();
    }

    QList<int> Group::Profiles() const {
        return profiles;
    }

    double bitrateToBps(const QString& str)
    {
        if (str.endsWith("Gbps", Qt::CaseInsensitive)) {
            double val = str.left(str.size() - 4).toDouble();
            return val * 1e9;
        }
        if (str.endsWith("Mbps", Qt::CaseInsensitive)) {
            double val = str.left(str.size() - 4).toDouble();
            return val * 1e6;
        }
        if (str.endsWith("Kbps", Qt::CaseInsensitive)) {
            double val = str.left(str.size() - 4).toDouble();
            return val * 1e3;
        }
        if (str == "N/A") return -1;
        return 0.0;
    }

    bool Group::SortProfiles(GroupSortAction sortAction) {
        QList<int> original;
        testBy testOrder;
        trafficBy trafficOrder;
        {
            if (!mutex.tryLock()) return false;
            original = profiles;
            testOrder = test_sort_by;
            trafficOrder = traffic_sort_by;
            mutex.unlock();
        }
        if (sortAction.method == GroupSortMethod::Raw || sortAction.method == GroupSortMethod::ById) return true;

        // Never enter ProfilesRepo while holding the group lock. Snapshot all keys once,
        // so the comparator neither nests locks nor observes changing profile objects.
        struct SortKey {
            QString text;
            double number = 0;
            quint64 traffic = 0;
            int security = 0;
            int latency = 0;
        };
        QHash<int, SortKey> keys;
        const auto allProfs = dataManager->profilesRepo->GetProfileBatch(original);
        for (const auto& profile : allProfs) {
            if (profile == nullptr || profile->outbound == nullptr) continue;
            SortKey key;
            key.latency = profile->latency;
            switch (sortAction.method) {
            case GroupSortMethod::ByType: key.text = profile->outbound->DisplayType(); break;
            case GroupSortMethod::ByName: key.text = profile->outbound->name; break;
            case GroupSortMethod::ByAddress: key.text = profile->outbound->DisplayAddress(); break;
            case GroupSortMethod::BySecurity: {
                const auto security = profile->outbound->EffectiveSecurity();
                key.security = static_cast<int>(security.level);
                key.text = security.transport + security.label;
                break;
            }
            case GroupSortMethod::ByTestResult:
                switch (testOrder) {
                case testBy::latency: key.number = key.latency == 0 ? 100000 : key.latency < 0 ? 99999 : key.latency; break;
                case testBy::dlSpeed: key.number = bitrateToBps(profile->dl_speed); break;
                case testBy::ulSpeed: key.number = bitrateToBps(profile->ul_speed); break;
                case testBy::ipOut: key.text = profile->ip_out; break;
                }
                break;
            case GroupSortMethod::ByTraffic:
                switch (trafficOrder) {
                case trafficBy::total: key.traffic = profile->traffic_downlink + profile->traffic_uplink; break;
                case trafficBy::dl: key.traffic = profile->traffic_downlink; break;
                case trafficBy::ul: key.traffic = profile->traffic_uplink; break;
                }
                break;
            default: break;
            }
            keys.insert(profile->id, key);
        }
        auto sorted = original;
        std::stable_sort(sorted.begin(), sorted.end(), [&](int a, int b) {
            const auto ka = keys.constFind(a), kb = keys.constFind(b);
            if (ka == keys.cend() || kb == keys.cend()) return ka != keys.cend() && kb == keys.cend();
            const auto compare = [&](const auto& left, const auto& right) {
                return sortAction.descending ? left > right : left < right;
            };
            if (sortAction.method == GroupSortMethod::ByLatency) {
                // Unknown, failed and connect-only verdicts never outrank a measured URL latency.
                if ((ka->latency > 0) != (kb->latency > 0)) return ka->latency > 0;
                if (ka->latency <= 0) return false;
                return compare(ka->latency, kb->latency);
            }
            if (sortAction.method == GroupSortMethod::ByTraffic) return compare(ka->traffic, kb->traffic);
            if (sortAction.method == GroupSortMethod::ByTestResult && testOrder != testBy::ipOut)
                return compare(ka->number, kb->number);
            if (sortAction.method == GroupSortMethod::BySecurity && ka->security != kb->security)
                return compare(ka->security, kb->security);
            return compare(ka->text, kb->text);
        });
        QMutexLocker locker(&mutex);
        // A concurrent import/add/remove wins: do not replace it with our older snapshot.
        if (profiles != original || test_sort_by != testOrder || traffic_sort_by != trafficOrder) return false;
        profiles = std::move(sorted);
        return true;
    }

    bool Group::AddProfile(int ID)
    {
        QMutexLocker locker(&mutex);
        if (HasProfile(ID))
        {
            return false;
        }
        profiles.append(ID);
        return true;
    }

    bool Group::AddProfileBatch(const QList<int>& IDs) {
        QMutexLocker locker(&mutex);
        QSet<int> currentProfiles(profiles.cbegin(), profiles.cend());
        for (auto profileID : IDs) {
            if (!currentProfiles.contains(profileID)) {
                profiles.append(profileID);
                currentProfiles.insert(profileID);
            }
        }
        return true;
    }

    bool Group::RemoveProfile(int ID)
    {
        QMutexLocker locker(&mutex);
        if (!HasProfile(ID)) return false;
        profiles.removeAll(ID);
        return true;
    }

    bool Group::RemoveProfileBatch(const QList<int>& IDs) {
        QSet<int> toDel;
        for (auto ID : IDs) {
            toDel.insert(ID);
        }
        QList<int> newIDs;
        QMutexLocker locker(&mutex);
        for (auto inID : profiles) {
            if (!toDel.contains(inID)) {
                newIDs.append(inID);
            }
        }
        profiles = newIDs;
        return true;
    }

    bool Group::SwapProfiles(int idx1, int idx2)
    {
        QMutexLocker locker(&mutex);
        if (profiles.size() <= idx1 || profiles.size() <= idx2) return false;
        profiles.swapItemsAt(idx1, idx2);
        return true;
    }

    bool Group::EmplaceProfile(int idx, int newIdx)
    {
        QMutexLocker locker(&mutex);
        if (profiles.size() <= idx || profiles.size() <= newIdx) return false;
        profiles.insert(newIdx+1, profiles[idx]);
        if (idx < newIdx) profiles.remove(idx);
        else profiles.remove(idx+1);
        return true;
    }

    bool Group::HasProfile(int ID) const
    {
        return profiles.contains(ID);
    }
}
