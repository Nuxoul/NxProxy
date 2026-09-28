#pragma once

#include <QMap>
#include <QSet>
#include <QString>

namespace Configs
{
    // Every profile the running config was built from: the started one, its chain hops, the group's
    // front/landing proxies, route-profile outbounds and endpoint hops, auto-selector members.
    void SetRunningProfiles(const QSet<int> &profileIDs);

    void ClearRunningProfiles();

    bool RunningUsesProfile(int profileID);

    // Strategy group profile id -> member profile id -> the member's outbound tag in the live core.
    // Recorded at start so a click can repick a live group; empty once the profile stops.
    void SetSelectorMemberTags(const QMap<int, QMap<int, QString>> &tags);

    void ClearSelectorMemberTags();

    // Empty when the group or member is not part of the running config.
    QString SelectorMemberTag(int selectorID, int memberID);
}
