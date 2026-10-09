#pragma once

#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/global/Configs.hpp"

namespace UrlTestCleanup {
inline bool allowed(bool isUrl, bool cancelled, bool aborted, bool currentGeneration) {
    return isUrl && !cancelled && !aborted && currentGeneration;
}
// Input is the explicit failure set from this completed sweep, never all stored latencies.
inline QList<int> eligibleFailures(const QList<int>& failed) {
    QList<int> eligible;
    for (const int id : failed) {
        const auto profile = Configs::dataManager->profilesRepo->GetProfile(id);
        if (profile == nullptr || profile->latency >= 0 || profile->latency == Configs::kLatencyConnectOnly) continue;
        const auto group = Configs::dataManager->groupsRepo->GetGroup(profile->gid);
        if (group != nullptr && group->auto_clear_unavailable) eligible.append(id);
    }
    return eligible;
}
}
