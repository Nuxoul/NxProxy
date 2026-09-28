#include "include/global/RunningProfiles.hpp"

#include <QMutex>
#include <QMutexLocker>

namespace Configs {
    namespace {
        QMutex runningProfilesMu;
        QSet<int> runningProfiles;
        QMap<int, QMap<int, QString>> selectorMemberTags;
    }

    void SetRunningProfiles(const QSet<int> &profileIDs)
    {
        QMutexLocker lk(&runningProfilesMu);
        runningProfiles.clear();
        for (const auto id : profileIDs) {
            if (id < 0) continue;
            runningProfiles.insert(id);
        }
    }

    void ClearRunningProfiles()
    {
        QMutexLocker lk(&runningProfilesMu);
        runningProfiles.clear();
    }

    bool RunningUsesProfile(int profileID)
    {
        if (profileID < 0) return false;
        QMutexLocker lk(&runningProfilesMu);
        return runningProfiles.contains(profileID);
    }

    void SetSelectorMemberTags(const QMap<int, QMap<int, QString>> &tags)
    {
        QMutexLocker lk(&runningProfilesMu);
        selectorMemberTags.clear();
        for (auto groupIt = tags.constBegin(); groupIt != tags.constEnd(); ++groupIt) {
            if (groupIt.key() < 0) continue;
            for (auto memberIt = groupIt->constBegin(); memberIt != groupIt->constEnd(); ++memberIt) {
                if (memberIt.key() < 0 || memberIt->isEmpty()) continue;
                selectorMemberTags[groupIt.key()].insert(memberIt.key(), *memberIt);
            }
        }
    }

    void ClearSelectorMemberTags()
    {
        QMutexLocker lk(&runningProfilesMu);
        selectorMemberTags.clear();
    }

    QString SelectorMemberTag(int selectorID, int memberID)
    {
        if (selectorID < 0 || memberID < 0) return {};
        QMutexLocker lk(&runningProfilesMu);
        const auto groupIt = selectorMemberTags.constFind(selectorID);
        if (groupIt == selectorMemberTags.constEnd()) return {};
        return groupIt->value(memberID);
    }
}
