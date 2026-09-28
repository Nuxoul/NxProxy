#include "include/configs/sub/GroupUpdater.hpp"

#include "include/configs/sub/SubscriptionParser.hpp"
#include "include/configs/sub/SubscriptionReconcile.hpp"
#include "include/database/GroupsRepo.h"
#include "include/database/RoutesRepo.h"
#include "include/global/HTTPRequestHelper.hpp"
#include "include/global/Utils.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QUrl>
#include <algorithm>

namespace Subscription {
    namespace {
        constexpr int kInsertChunk = 500;
        constexpr qint64 kMaxSubscriptionBytes = 64LL * 1024 * 1024;

        QByteArray digest(const QByteArray &data) {
            return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
        }

        QByteArray contentKeyOf(const Configs::Profile &ent) {
            return digest(QJsonDocument(ent.outbound->ExportToJson()).toJson(QJsonDocument::Compact));
        }

        QByteArray identityKeyOf(const Configs::Profile &ent) {
            return digest(ent.type.toUtf8() + '|' + QJsonDocument(ent.outbound->ExportIdentity()).toJson(QJsonDocument::Compact));
        }

        // BatchDeleteProfiles silently drops the running profile from the list it was handed (#1753).
        struct DeleteOutcome {
            bool ok = false;
            QList<int> deleted;
            QList<int> kept;
        };

        DeleteOutcome deleteProfiles(QList<int> ids) {
            DeleteOutcome outcome;
            const QSet<int> requested(ids.begin(), ids.end());
            outcome.ok = Configs::dataManager->profilesRepo->BatchDeleteProfiles(
                ids, Configs::dataManager->settingsRepo->allow_stopping_active_profile);
            const QSet<int> deleted(ids.begin(), ids.end());
            outcome.deleted = std::move(ids);
            for (int id : requested) {
                if (!deleted.contains(id)) outcome.kept << id;
            }
            return outcome;
        }

        // Inserts in chunks; only per-profile digests survive a flush.
        class ImportSink {
        public:
            ImportSink(int gid, ContentIndex *index) : gid(gid), index(index) {}

            void add(std::shared_ptr<Configs::Profile> ent) {
                NewEntry entry;
                entry.display = ent->outbound->DisplayTypeAndName();
                if (index != nullptr) {
                    entry.keys.content = contentKeyOf(*ent);
                    index->noteArrival(entry.keys.content);
                    entry.contentKnown = index->knows(entry.keys.content);
                    if (const auto oldId = index->claim(entry.keys.content)) {
                        entry.id = *oldId;
                        entry.reused = true;
                        entries.append(std::move(entry));
                        return;
                    }
                    if (!entry.contentKnown) entry.keys.identity = identityKeyOf(*ent);
                }
                entryIndex.append(entries.size());
                entries.append(std::move(entry));
                chunk.append(std::move(ent));
                if (chunk.size() >= kInsertChunk) flush();
            }

            void flush() {
                if (chunk.isEmpty()) return;
                if (Configs::dataManager->profilesRepo->AddProfileBatch(chunk, gid)) {
                    for (qsizetype i = 0; i < chunk.size(); ++i) entries[entryIndex[i]].id = chunk[i]->id;
                }
                chunk.clear();
                entryIndex.clear();
            }

            QList<NewEntry> entries;
            QList<ProxyGroup> proxyGroups;
            QStringList ruleLines;
        private:
            int gid;
            ContentIndex *index;
            QList<std::shared_ptr<Configs::Profile>> chunk;
            QList<qsizetype> entryIndex;
        };

        ParseSink sinkFor(ImportSink &sink) {
            ParseSink parseSink;
            parseSink.profile = [&sink](std::shared_ptr<Configs::Profile> ent) { sink.add(std::move(ent)); };
            parseSink.proxyGroup = [&sink](const ProxyGroup &group) { sink.proxyGroups.append(group); };
            parseSink.ruleLine = [&sink](const QString &line) { sink.ruleLines.append(line); };
            parseSink.log = [](const QString &line) { MW_show_log(line); };
            parseSink.warn = [](const QString &title, const QString &text) {
                runOnUiThread([=] { MessageBoxWarning(title, text); });
            };
            return parseSink;
        }
        void applyProxyGroups(int gid, const QList<ProxyGroup> &groups) {
            if (groups.isEmpty()) return;
            const auto group = Configs::dataManager->groupsRepo->GetGroup(gid < 0 ? Configs::dataManager->settingsRepo->current_group : gid);
            if (group == nullptr) return;
            QHash<QString, int> profilesByName;
            for (const int id : group->profiles) {
                const auto profile = Configs::dataManager->profilesRepo->GetProfile(id);
                if (profile == nullptr || profile->type == "selector" || profile->type == "autoselector" || profile->outbound == nullptr) continue;
                const auto name = profile->outbound->name.trimmed();
                if (!name.isEmpty()) profilesByName.insert(name.toLower(), id);
            }
            int changed = 0;
            for (const auto &remote : groups) {
                if (remote.type.compare("select", Qt::CaseInsensitive) != 0 || remote.name.trimmed().isEmpty()) continue;
                QList<int> members;
                for (const auto &name : remote.proxies) {
                    const int id = profilesByName.value(name.trimmed().toLower(), -1);
                    if (id >= 0 && !members.contains(id)) members.append(id);
                }
                if (members.isEmpty()) continue;

                std::shared_ptr<Configs::Profile> selectorProfile;
                for (const int id : group->profiles) {
                    const auto candidate = Configs::dataManager->profilesRepo->GetProfile(id);
                    const auto candidateSelector = candidate == nullptr ? nullptr : candidate->Selector();
                    if (candidate != nullptr && candidate->type == "selector" && candidateSelector != nullptr &&
                        candidateSelector->managedBySubscription && candidateSelector->remoteGroup == remote.name) {
                        selectorProfile = candidate;
                        break;
                    }
                }
                if (selectorProfile == nullptr) {
                    selectorProfile = Configs::ProfilesRepo::NewProfile("selector");
                    if (selectorProfile == nullptr || selectorProfile->outbound == nullptr) continue;
                    selectorProfile->name = remote.name;
                    selectorProfile->outbound->name = remote.name;
                    const auto createdSelector = selectorProfile->Selector();
                    if (createdSelector == nullptr) continue;
                    createdSelector->managedBySubscription = true;
                    createdSelector->remoteGroup = remote.name;
                    if (!Configs::dataManager->profilesRepo->AddProfile(selectorProfile, group->id)) continue;
                }

                const auto selector = selectorProfile->Selector();
                if (selector == nullptr) continue;
                const int oldSelected = selector->selectedID;
                selector->members = members;
                int selected = profilesByName.value(remote.selected.trimmed().toLower(), -1);
                if (!members.contains(selected)) selected = members.contains(oldSelected) ? oldSelected : members.first();
                selector->selectedID = selected;
                selector->name = remote.name;
                selectorProfile->name = remote.name;
                if (selectorProfile->outbound != nullptr) selectorProfile->outbound->name = remote.name;
                Configs::dataManager->profilesRepo->Save(selectorProfile);
                ++changed;
            }
            if (changed > 0) MW_show_log(QObject::tr("Imported or updated %1 selector group(s).").arg(changed));
        }

        int routeOutboundId(const QString &action, const QHash<QString, int> &selectorIds) {
            const QString key = action.trimmed();
            if (key.compare("DIRECT", Qt::CaseInsensitive) == 0) return Configs::directID;
            if (key.compare("REJECT", Qt::CaseInsensitive) == 0) return Configs::blockID;
            if (key.compare("PROXY", Qt::CaseInsensitive) == 0) return Configs::proxyID;
            if (key.compare("GLOBAL", Qt::CaseInsensitive) == 0) {
                const auto selector = selectorIds.constFind(key.toLower());
                return selector == selectorIds.constEnd() ? Configs::proxyID : selector.value();
            }
            const auto selector = selectorIds.constFind(key.toLower());
            return selector == selectorIds.constEnd() ? Configs::proxyID : selector.value();
        }

        bool isRouteAction(const QString &candidate, const QHash<QString, int> &selectorIds) {
            const QString key = candidate.trimmed();
            return key.compare("DIRECT", Qt::CaseInsensitive) == 0 ||
                   key.compare("REJECT", Qt::CaseInsensitive) == 0 ||
                   key.compare("PROXY", Qt::CaseInsensitive) == 0 ||
                   key.compare("GLOBAL", Qt::CaseInsensitive) == 0 ||
                   selectorIds.contains(key.toLower());
        }

        QString ruleAction(const QStringList &fields, const QHash<QString, int> &selectorIds) {
            for (int i = 2; i < fields.size(); ++i) {
                if (isRouteAction(fields[i], selectorIds)) return fields[i].trimmed();
            }
            return fields.value(2).trimmed();
        }

        QString routeSetKey(const QString &kind, const QString &value) {
            const QString key = value.trimmed().toLower();
            if (kind == "GEOSITE") return "geosite-" + key;
            if (kind == "GEOIP") return "geoip-" + key;
            if (kind != "RULE-SET") return {};
            if (key == "private-domain") return "geosite-private";
            if (key == "private-ip") return "geoip-private";
            if (key == "cn-domain") return "geosite-cn";
            if (key == "cn-ip") return "geoip-cn";
            if (key.startsWith("geosite-") || key.startsWith("geoip-")) return key;
            return {};
        }

        QString wildcardToRegex(const QString &wildcard) {
            QString result;
            for (const auto ch : wildcard.trimmed()) {
                if (ch == '*') result += ".*";
                else if (ch == '?') result += '.';
                else result += QRegularExpression::escape(QString(ch));
            }
            return '^' + result + '$';
        }

        std::shared_ptr<Configs::RouteProfile> managedRoute(int gid, const QString &sourceName) {
            for (const auto &route : Configs::dataManager->routesRepo->GetAllRouteProfiles()) {
                if (route != nullptr && route->managedBySubscription && route->managedGroupID == gid && route->managedSourceName == sourceName)
                    return route;
            }
            auto route = Configs::RoutesRepo::NewRouteProfile();
            route->name = QObject::tr("Mihomo - %1").arg(sourceName);
            route->managedBySubscription = true;
            route->managedGroupID = gid;
            route->managedSourceName = sourceName;
            if (!Configs::dataManager->routesRepo->AddRouteProfile(route)) return nullptr;
            return route;
        }

        void applyProxyRules(int gid, const QStringList &lines) {
            const int targetGroup = gid < 0 ? Configs::dataManager->settingsRepo->current_group : gid;
            const auto group = Configs::dataManager->groupsRepo->GetGroup(targetGroup);
            if (group == nullptr) return;
            if (lines.isEmpty()) {
                for (const auto &route : Configs::dataManager->routesRepo->GetAllRouteProfiles()) {
                    if (route == nullptr || !route->managedBySubscription || route->managedGroupID != targetGroup || route->managedSourceName != group->name) continue;
                    route->Rules.clear();
                    route->defaultOutboundID = Configs::proxyID;
                    Configs::dataManager->routesRepo->Save(route);
                }
                return;
            }

            QHash<QString, int> selectorIds;
            for (const int id : group->profiles) {
                const auto profile = Configs::dataManager->profilesRepo->GetProfile(id);
                if (profile == nullptr || profile->type != "selector") continue;
                const QString name = profile->name.trimmed();
                if (!name.isEmpty()) selectorIds.insert(name.toLower(), id);
            }

            auto route = managedRoute(targetGroup, group->name);
            if (route == nullptr) return;
            route->Rules.clear();
            route->defaultOutboundID = Configs::proxyID;
            int order = 0;
            for (const auto &raw : lines) {
                const auto fields = raw.split(',', Qt::KeepEmptyParts);
                if (fields.isEmpty()) continue;
                const QString kind = fields.first().trimmed().toUpper();
                if (kind == "MATCH" || kind == "FINAL") {
                    if (fields.size() >= 2) route->defaultOutboundID = routeOutboundId(fields.last().trimmed(), selectorIds);
                    continue;
                }
                if (fields.size() < 3) continue;

                auto rule = std::make_shared<Configs::RouteRule>();
                rule->name = QStringLiteral("mihomo-%1").arg(order++);
                rule->outboundID = routeOutboundId(ruleAction(fields, selectorIds), selectorIds);
                const QString value = fields[1].trimmed();
                if (kind == "DOMAIN") rule->domain << value;
                else if (kind == "DOMAIN-SUFFIX") rule->domain_suffix << value;
                else if (kind == "DOMAIN-KEYWORD") rule->domain_keyword << value;
                else if (kind == "DOMAIN-REGEX") rule->domain_regex << value;
                else if (kind == "IP-CIDR" || kind == "IP-CIDR6") rule->ip_cidr << value;
                else if (kind == "PROCESS-NAME") rule->process_name << value;
                else if (kind == "PROCESS-NAME-WILDCARD") rule->process_path_regex << wildcardToRegex(value);
                else if (kind == "PROCESS-PATH") rule->process_path << value;
                else if (kind == "GEOSITE" || kind == "GEOIP" || kind == "RULE-SET") {
                    const auto key = routeSetKey(kind, value);
                    if (key.isEmpty()) {
                        MW_show_log(QObject::tr("Skipped unsupported Mihomo rule-set: %1").arg(value));
                        continue;
                    }
                    rule->rule_set << key;
                } else continue;
                route->Rules.append(rule);
            }

            Configs::dataManager->routesRepo->Save(route);
            Configs::dataManager->settingsRepo->current_route_id = route->id;
            Configs::dataManager->settingsRepo->Save();
            MW_show_log(QObject::tr("Imported %1 routing rules into %2.").arg(route->Rules.size()).arg(route->name));
        }
        QString notice(const QStringList &names, const QString &prefix, const QString &action) {
            if (names.size() >= 1000) return QStringLiteral("%1 %2 %3\n").arg(prefix, action).arg(names.size());
            QString result;
            for (const auto &name : names) {
                result += prefix;
                result += ' ';
                result += name;
                result += '\n';
            }
            return result;
        }

        QString groupLabel(int gid) {
            const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
            return group == nullptr ? Int2String(gid) : group->name;
        }
    } // namespace

    GroupUpdater *updater() {
        static auto *instance = new GroupUpdater;
        return instance;
    }

    void GroupUpdater::RefreshGroup(int gid, const Finish &finish, bool showDiff) {
        QMutexLocker locker(&mutex);
        if (pending.contains(gid)) {
            locker.unlock();
            MW_show_log(QObject::tr("Subscription update already queued: %1").arg(groupLabel(gid)));
            if (finish != nullptr) finish();
            return;
        }
        pending.insert(gid);
        enqueueLocked({gid, false, [=, this] {
            refresh(gid, showDiff);
            emit asyncUpdateCallback(gid);
            if (finish != nullptr) finish();
        }});
    }

    void GroupUpdater::RefreshAll(bool onlyAllowed) {
        QMutexLocker locker(&mutex);
        if (pendingBatch > 0) {
            locker.unlock();
            MW_show_log("The last subscription update has not exited.");
            return;
        }
        for (const int gid : Configs::dataManager->groupsRepo->GetGroupsTabOrder()) {
            const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
            if (group == nullptr || group->url.isEmpty() || group->archive || (onlyAllowed && group->skip_auto_update)) continue;
            if (pending.contains(gid)) continue;
            pending.insert(gid);
            ++pendingBatch;
            enqueueLocked({gid, true, [=, this] {
                refresh(gid, false);
                emit asyncUpdateCallback(gid);
            }});
        }
    }

    void GroupUpdater::SubscribeUrl(const QString &url, const Finish &finish) {
        const auto content = url.trimmed();
        enqueue({-1, false, [=, this] {
            auto group = Configs::GroupsRepo::NewGroup();
            group->name = QUrl(content).host();
            group->url = content;
            Configs::dataManager->groupsRepo->AddGroup(group);
            MW_dialog_message(MwMessage::SubscriptionNewGroup, {});
            refresh(group->id, false);
            emit asyncUpdateCallback(group->id);
            if (finish != nullptr) finish();
        }});
    }

    void GroupUpdater::ImportUrl(const QString &url, const Finish &finish) {
        const auto content = url.trimmed();
        const int targetGroup = Configs::dataManager->settingsRepo->current_group;
        enqueue({-1, false, [=, this] {
            QByteArray body;
            QString userInfo;
            if (fetch(content, content, body, userInfo)) importDocuments(targetGroup, {std::move(body)});
            emit asyncUpdateCallback(targetGroup);
            if (finish != nullptr) finish();
        }});
    }

    void GroupUpdater::ImportText(const QString &text, int gid, const Finish &finish) {
        QByteArray body = text.toUtf8();
        const int targetGroup = gid < 0 ? Configs::dataManager->settingsRepo->current_group : gid;
        enqueue({-1, false, [=, this]() mutable {
            importDocuments(targetGroup, {std::move(body)});
            emit asyncUpdateCallback(targetGroup);
            if (finish != nullptr) finish();
        }});
    }
    void GroupUpdater::ImportBatch(const QStringList &payloads, const Finish &finish) {
        if (payloads.isEmpty()) return;
        QList<QByteArray> documents;
        for (const auto &payload : payloads) documents << payload.trimmed().toUtf8();
        const int targetGroup = Configs::dataManager->settingsRepo->current_group;
        enqueue({-1, false, [=, this]() mutable {
            importDocuments(targetGroup, std::move(documents));
            emit asyncUpdateCallback(targetGroup);
            if (finish != nullptr) finish();
        }});
    }

    void GroupUpdater::enqueue(Job job) {
        QMutexLocker locker(&mutex);
        enqueueLocked(std::move(job));
    }

    void GroupUpdater::enqueueLocked(Job job) {
        queue.append(std::move(job));
        if (running) return;
        running = true;
        runOnNewThread([this] { drain(); });
    }

    void GroupUpdater::drain() {
        for (;;) {
            Job job;
            {
                QMutexLocker locker(&mutex);
                if (queue.isEmpty()) {
                    running = false;
                    return;
                }
                job = queue.takeFirst();
            }
            try {
                job.run();
            } catch (const std::exception &ex) {
                MW_show_log(QString("Subscription task failed: %1").arg(ex.what()));
            } catch (...) {
                MW_show_log("Subscription task failed.");
            }
            QMutexLocker locker(&mutex);
            if (job.gid >= 0) pending.remove(job.gid);
            if (job.batch) --pendingBatch;
        }
    }

    bool GroupUpdater::fetch(const QString &url, const QString &name, QByteArray &body, QString &userInfo) {
        MW_show_log(">>>>>>>> " + QObject::tr("Requesting subscription: %1").arg(name));
        auto resp = NetworkRequestHelper::HttpGet(url, Configs::dataManager->settingsRepo->sub_send_hwid, false, kMaxSubscriptionBytes);
        if (!resp.error.isEmpty()) {
            MW_show_log("<<<<<<<< " + QObject::tr("Requesting subscription %1 error: %2").arg(name, resp.error + "\n" + resp.data));
            return false;
        }
        body = std::move(resp.data);
        userInfo = NetworkRequestHelper::GetHeader(resp.header, "Subscription-UserInfo");
        MW_show_log("<<<<<<<< " + QObject::tr("Subscription request fininshed: %1").arg(name));
        return true;
    }

    void GroupUpdater::importDocuments(int gid, QList<QByteArray> documents) {
        auto &settings = Configs::dataManager->settingsRepo;
        settings->imported_count = 0;
        ImportSink sink(gid, nullptr);

        MW_show_log(">>>>>>>> " + QObject::tr("Processing subscription data..."));
        for (auto &document : documents) ParseDocument(std::move(document), sinkFor(sink));
        sink.flush();
        applyProxyGroups(gid, sink.proxyGroups);
        applyProxyRules(gid, sink.ruleLines);
        settings->imported_count = sink.entries.size();
        MW_dialog_message(MwMessage::SubscriptionFinished, {});
    }

    void GroupUpdater::refresh(int gid, bool showDiff) {
        auto &settings = Configs::dataManager->settingsRepo;
        auto &profilesRepo = Configs::dataManager->profilesRepo;
        auto &groupsRepo = Configs::dataManager->groupsRepo;

        settings->imported_count = 0;
        auto group = groupsRepo->GetGroup(gid);
        if (group == nullptr || group->archive) return;

        QByteArray body;
        QString userInfo;
        if (!fetch(group->url.trimmed(), group->name, body, userInfo)) return;

        group->sub_last_update = QDateTime::currentMSecsSinceEpoch() / 1000;
        group->info = userInfo;
        groupsRepo->Save(group);

        // Auto selectors are local state, not servers the remote sent: keep them out of the diff.
        QList<int> selectorIds;
        for (const int id : group->profiles) {
            const auto profile = profilesRepo->GetProfile(id);
            if (profile != nullptr && (profile->type == "autoselector" || profile->type == "selector")) selectorIds << id;
        }
        const QSet<int> selectors(selectorIds.begin(), selectorIds.end());
        QList<QPair<int, int>> sticky;
        QSet<int> stickyIDs;
        for (int i = 0; i < group->profiles.size(); i++) {
            if (!selectors.contains(group->profiles[i])) continue;
            sticky << qMakePair(i, group->profiles[i]);
            stickyIDs.insert(group->profiles[i]);
        }
        const auto members = [&] {
            QList<int> ids;
            for (int id : group->profiles) {
                if (!stickyIDs.contains(id)) ids << id;
            }
            return ids;
        };

        // Ids a running auto selector can no longer trust: deleted, or same id with new settings.
        QList<int> disturbed;
        bool cleared = false;
        if (settings->sub_clear) {
            MW_show_log(QObject::tr("Clearing servers..."));
            const auto outcome = deleteProfiles(members());
            if (!outcome.ok) {
                runOnUiThread([] { MessageBoxWarning("Internal Error", "DB Error when deleting profiles, Please try again."); });
                return;
            }
            disturbed = outcome.deleted;
            // A survivor still belongs to the subscription: fall through to the diff.
            cleared = outcome.kept.isEmpty();
        }

        QList<OldEntry> old;
        if (!cleared) {
            const auto ids = members();
            for (qsizetype off = 0; off < ids.size(); off += Configs::BATCH_LIMIT_READ) {
                for (const auto &ent : profilesRepo->GetProfileBatch(ids.mid(off, Configs::BATCH_LIMIT_READ))) {
                    if (ent == nullptr) continue;
                    old.append({ent->id, {contentKeyOf(*ent), identityKeyOf(*ent)}, ent->outbound->DisplayTypeAndName()});
                }
            }
        }
        ContentIndex index(old);
        ImportSink sink(gid, cleared ? nullptr : &index);

        MW_show_log(">>>>>>>> " + QObject::tr("Processing subscription data..."));
        ParseDocument(std::move(body), sinkFor(sink));
        sink.flush();

        QString change_text;
        if (cleared) {
            if (sink.entries.size() >= 1000) {
                change_text += "[+] " + Int2String(sink.entries.size()) + " profiles\n";
            } else {
                for (const auto &entry : sink.entries) change_text += "[+] " + entry.display + "\n";
            }
        } else {
            const auto plan = Reconcile(old, sink.entries, index);
            for (const auto &[oldId, newId] : plan.updates) {
                auto oldEnt = profilesRepo->GetProfile(oldId);
                const auto newEnt = profilesRepo->GetProfile(newId);
                if (oldEnt != nullptr && newEnt != nullptr) {
                    oldEnt->outbound = newEnt->outbound;
                    oldEnt->name = oldEnt->outbound->name;
                    profilesRepo->Save(oldEnt);
                }
                disturbed << oldId;
            }

            const auto previousOrder = group->profiles;
            group->profiles = plan.order;
            for (const auto &[position, id] : sticky) {
                group->profiles.insert(std::min<qsizetype>(position, group->profiles.size()), id);
            }
            groupsRepo->Save(group);

            const auto outcome = deleteProfiles(plan.stale);
            if (!outcome.ok) {
                runOnUiThread([] { MessageBoxWarning("Internal error", "DB Error when deleting profiles, data may be corrupted"); });
            }
            disturbed << outcome.deleted;

            // Nothing rebuilds group->profiles from the rows: a survivor left out here is orphaned.
            QString notice_kept;
            for (int id : outcome.kept) {
                if (group->HasProfile(id)) continue;
                const auto position = previousOrder.indexOf(id);
                group->profiles.insert(position < 0 ? group->profiles.size()
                                                    : std::min<qsizetype>(position, group->profiles.size()), id);
                if (const auto ent = profilesRepo->GetProfile(id); ent != nullptr) {
                    notice_kept += "[=] " + ent->outbound->DisplayTypeAndName() + "\n";
                }
            }
            if (!outcome.kept.isEmpty()) groupsRepo->Save(group);

            change_text = "\n" + QObject::tr("Added %1 profiles:\n%2\nUpdated %3 profiles:\n%4\nDeleted %5 Profiles:\n%6")
                                     .arg(plan.added.size())
                                     .arg(notice(plan.added, "[+]", "added"))
                                     .arg(plan.updates.size())
                                     .arg(notice(plan.updated, "[~]", "updated"))
                                     .arg(plan.deleted.size())
                                     .arg(notice(plan.deleted, "[-]", "deleted"));
            if (!notice_kept.isEmpty()) {
                change_text += "\n" + QObject::tr("Still in use, so kept instead of deleted:\n%1").arg(notice_kept);
            }
            if (plan.added.isEmpty() && plan.updates.isEmpty() && plan.deleted.isEmpty()) change_text = QObject::tr("Nothing");
        }
        applyProxyGroups(gid, sink.proxyGroups);
        applyProxyRules(gid, sink.ruleLines);

        MW_show_log("<<<<<<<< " + QObject::tr("Change of %1:").arg(group->name) + "\n" + change_text);
        if (showDiff && settings->sub_show_change_popup) {
            const auto diffTitle = QObject::tr("Change of %1").arg(group->name);
            auto diffBody = change_text.trimmed();
            if (diffBody.isEmpty()) diffBody = QObject::tr("Nothing");
            runOnUiThread([diffTitle, diffBody] { MessageBoxScrollable(diffTitle, diffBody); });
        }
        // Auto selectors resolve members from the group at build time, so a refresh can invalidate an untouched one.
        QStringList selectorArgs{Int2String(group->id)};
        for (int id : disturbed) selectorArgs << Int2String(id);
        MW_dialog_message(MwMessage::SubscriptionGroupChanged, selectorArgs);
        MW_dialog_message(MwMessage::SubscriptionFinished, {MwArg::Quiet});
    }
} // namespace Subscription
