// Standalone real-source regression: QApplication + a temporary SQLite database.
// Build/run instructions: selector_resource_run.ps1 (no project CMake changes).
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include "include/global/Configs.hpp"
#include "include/configs/generate.h"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/database/RoutesRepo.h"
#include "include/database/TrafficStatsRepo.h"
#include "include/database/OtpProfilesRepo.h"
#ifdef SELECTOR_RESOURCE_STANDALONE
#include "include/ui/mainwindow.h"
// A DB deletion callback references the GUI stop hook, which this fixture never calls.
void MainWindow::profile_stop(bool, bool, bool, bool) { throw std::runtime_error("unexpected GUI stop"); }
#endif

namespace {
using namespace Configs;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::shared_ptr<Profile> add(int gid, const QString& type, const QString& name) {
    auto p = ProfilesRepo::NewProfile(type);
    p->name = name; p->outbound->name = name;
    require(dataManager->profilesRepo->AddProfile(p, gid), "insert profile");
    return p;
}
QJsonObject outbound(const BuildConfigResult& r, const QString& tag) {
    for (const auto v : r.coreConfig["outbounds"].toArray()) {
        auto o = v.toObject(); if (o["tag"].toString() == tag) return o;
    }
    return {};
}
std::shared_ptr<BuildConfigResult> build(const std::shared_ptr<Profile>& main, bool sharing) {
    auto r = BuildSingBoxConfig(main, BuildConfigOptions{sharing});
    if (!r->error.isEmpty()) throw std::runtime_error(r->error.toStdString());
    QSet<QString> tags;
    for (const auto v : r->coreConfig["outbounds"].toArray()) {
        const auto tag = v.toObject()["tag"].toString();
        require(!tags.contains(tag), "duplicate outbound tag"); tags.insert(tag);
    }
    QSet<QString> watched;
    for (const auto& g : r->chainGroups) {
        require(!watched.contains(g.watchTag), "duplicate traffic watchTag"); watched.insert(g.watchTag);
        require(tags.contains(g.watchTag) || !r->coreConfig["endpoints"].toArray().isEmpty(), "watchTag does not exist");
        QSet<int> ids;
        for (const auto& p : g.profiles) { require(!ids.contains(p->id), "duplicate profile traffic credit"); ids.insert(p->id); }
    }
    return r;
}
void dataset(int count) {
    auto group = GroupsRepo::NewGroup(); group->name = "Sharing fixtures";
    require(dataManager->groupsRepo->AddGroup(group), "insert group");
    auto main = add(group->id, "socks", "Main");
    main->outbound->server = "127.0.0.1"; main->outbound->server_port = 1080;
    require(dataManager->profilesRepo->Save(main), "save main");
    QList<int> nodes;
    for (int i = 0; i < count; ++i) {
        auto p = add(group->id, "vless", "Node " + QString::number(i));
        require(p->outbound->ParseFromLink("vless://11111111-2222-3333-4444-555555555555@127.0.0.1:443?encryption=none#Node" + QString::number(i)), "parse vless");
        require(dataManager->profilesRepo->Save(p), "save node"); nodes.append(p->id);
    }
    auto route = RoutesRepo::NewRouteProfile(); route->name = "Selector sharing";
    QList<std::shared_ptr<Profile>> selectors;
    for (int i = 0; i < 8; ++i) {
        auto s = add(group->id, "selector", "Group " + QString::number(i));
        s->Selector()->members = nodes; s->Selector()->selectedID = nodes[i % count];
        require(dataManager->profilesRepo->Save(s), "save selector"); selectors.append(s);
        auto rule = std::make_shared<RouteRule>(); rule->domain = {QString("group%1.invalid").arg(i)};
        rule->outboundID = s->id; route->Rules.append(rule);
    }
    require(dataManager->routesRepo->AddRouteProfile(route), "insert route");
    dataManager->settingsRepo->current_route_id = route->id;
    for (int repeat = 0; repeat < 3; ++repeat) {
        QElapsedTimer t; t.start(); auto old = build(main, false); const auto oldUs = t.nsecsElapsed()/1000;
        t.restart(); auto shared = build(main, true); const auto sharedUs = t.nsecsElapsed()/1000;
        const auto oldCount = old->coreConfig["outbounds"].toArray().size();
        const auto newCount = shared->coreConfig["outbounds"].toArray().size();
        require(oldCount == 8*count+10 && newCount == count+10, "8 groups must share exactly one ordinary outbound per profile");
        require(old->chainGroups.size() == 8*count+1 && shared->chainGroups.size() == count+1, "traffic credits not deduplicated");
        require(old->involvedProfiles == shared->involvedProfiles, "sharing lost involved profile IDs");
        for (const auto& s : selectors) {
            const auto groupTag = QString("selector-%1").arg(s->id);
            const auto o = outbound(*shared, groupTag);
            require(o["outbounds"].toArray().size() == count && o["interrupt_exist_connections"].toBool(), "independent interrupting selector missing");
            require(o["default"].toString() == shared->selectorMemberTags[s->id][s->Selector()->selectedID], "independent default changed");
            require(shared->outboundDisplayNames[groupTag] == s->name, "selector label must contain only group name");
            for (int id : nodes) {
                const auto tag = shared->selectorMemberTags[s->id][id];
                require(tag == shared->selectorMemberTags[selectors.first()->id][id], "runtime member mapping not shared");
                auto before = outbound(*old, old->selectorMemberTags[s->id][id]); before.remove("tag");
                auto after = outbound(*shared, tag); after.remove("tag");
                require(before == after, "effective runtime config changed during sharing");
                require(!shared->outboundDisplayNames[tag].contains(" → "), "shared member mislabeled with owning group");
            }
        }
        std::cout << "8x" << count << " repeat=" << repeat+1 << " outbounds=" << oldCount << "->" << newCount
                  << " bytes=" << QJsonDocument(old->coreConfig).toJson(QJsonDocument::Compact).size()
                  << "->" << QJsonDocument(shared->coreConfig).toJson(QJsonDocument::Compact).size()
                  << " build_us=" << oldUs << "->" << sharedUs << '\n';
    }
    // Rebuilding after an edit has no process-global registry or stale cached config.
    auto changed = dataManager->profilesRepo->GetProfile(nodes.first()); changed->outbound->server_port = 8443;
    require(dataManager->profilesRepo->Save(changed), "edit node");
    auto updated = build(main, true);
    require(outbound(*updated, updated->selectorMemberTags[selectors.first()->id][changed->id])["server_port"].toInt() == 8443, "stale runtime identity after edit");
    changed->VLESS()->tls->enabled = true;
    changed->VLESS()->tls->server_name = "changed.invalid";
    changed->outbound->dialFields->connect_timeout = "7s";
    require(dataManager->profilesRepo->Save(changed), "save TLS/dial edit");
    auto tlsUpdated = build(main, true);
    const auto effective = outbound(*tlsUpdated, tlsUpdated->selectorMemberTags[selectors.first()->id][changed->id]);
    require(effective["tls"].toObject()["server_name"].toString() == "changed.invalid"
            && effective["connect_timeout"].toString() == "7s", "stale TLS/dial runtime config");
    for (const auto& s : selectors) require(tlsUpdated->selectorMemberTags[s->id][changed->id]
                                          == tlsUpdated->selectorMemberTags[selectors.first()->id][changed->id],
                                          "compatible TLS node not shared");

    // Same visible name is insufficient, even for different profile IDs with identical JSON.
    auto twin = add(group->id, "vless", changed->name);
    require(twin->outbound->ParseFromJson(changed->outbound->ExportToJson()), "copy node");
    require(dataManager->profilesRepo->Save(twin), "save twin");
    for (const auto& s : selectors) { s->Selector()->members.append(twin->id); require(dataManager->profilesRepo->Save(s), "append twin"); }
    auto distinct = build(main, true);
    require(distinct->selectorMemberTags[selectors.first()->id][twin->id] != distinct->selectorMemberTags[selectors.first()->id][changed->id], "shared by name or across profile IDs");

    // Opaque custom outbound and endpoint remain independent; full configs stay unusable members.
    auto opaque = add(group->id, "custom", "Opaque outbound");
    opaque->Custom()->type = Custom::CustomOutbound;
    opaque->Custom()->config = R"({"type":"socks","server":"127.0.0.1","server_port":1081,"detour":"direct"})";
    auto endpoint = add(group->id, "wireguard", "Endpoint");
    endpoint->outbound->ParseFromJson(QJsonObject{{"type","wireguard"},{"name","Endpoint"},{"server","127.0.0.1"},{"server_port",51820},{"private_key","AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="},{"peer_public_key","AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="},{"local_address",QJsonArray{"10.0.0.2/32"}}});
    auto full = add(group->id, "custom", "Full config"); full->Custom()->type = Custom::CustomFullConfig;
    full->Custom()->config = R"({"outbounds":[{"type":"direct","tag":"user-direct"}]})";
    require(dataManager->profilesRepo->Save(opaque) && dataManager->profilesRepo->Save(endpoint) && dataManager->profilesRepo->Save(full), "save fallback fixtures");
    for (const auto& s : selectors) {
        s->Selector()->members << opaque->id << endpoint->id << full->id;
        require(dataManager->profilesRepo->Save(s), "append fallback fixtures");
    }
    auto fallback = build(main, true);
    QSet<QString> opaqueTags, endpointTags;
    for (const auto& s : selectors) {
        opaqueTags.insert(fallback->selectorMemberTags[s->id][opaque->id]);
        endpointTags.insert(fallback->selectorMemberTags[s->id][endpoint->id]);
        require(!fallback->selectorMemberTags[s->id].contains(full->id), "full config became selector member");
    }
    require(opaqueTags.size() == 8 && endpointTags.size() == 8, "unsupported runtime resources shared");
    require(fallback->coreConfig["endpoints"].toArray().size() == 8, "endpoint fallback count");
    auto verbatim = BuildSingBoxConfig(full, BuildConfigOptions{true});
    auto independentFull = BuildSingBoxConfig(full, BuildConfigOptions{false});
    require(verbatim->coreConfig == independentFull->coreConfig, "sharing modified full config fallback");
    // Invalid stored picks repaired locally without mutating persisted selector state.
    selectors.first()->Selector()->selectedID = -12345;
    require(dataManager->profilesRepo->Save(selectors.first()), "save invalid pick");
    auto repaired = build(main, true);
    require(outbound(*repaired, QString("selector-%1").arg(selectors.first()->id))["default"].toString() == repaired->selectorMemberTags[selectors.first()->id][nodes.first()], "invalid selectedID fallback");
    require(selectors.first()->Selector()->selectedID == -12345, "build mutated persisted selection");
    std::cout << "PASS mapping, identity rebuild, distinct profiles, traffic, custom/full/endpoint fallback\n";
    require(BuildSingBoxConfig(main)->coreConfig == build(main, false)->coreConfig,
            "default build enabled sharing before chain-display integration");
}
}
int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    QApplication app(argc, argv); QTemporaryDir dir;
    if (!dir.isValid()) return 2;
    QDir::setCurrent(dir.path()); MW_show_log = [](const QString&){}; MW_dialog_message = [](MwMessage, QStringList){};
    Configs::initDB((dir.path()+"/sharing.db").toStdString());
    Configs::dataManager->settingsRepo->spmode_vpn = false;
    Configs::dataManager->settingsRepo->spmode_system_proxy = false;
    Configs::dataManager->settingsRepo->enable_warp = false;
    int code = 0;
    try { dataset(15); dataset(30); std::cout << "ALL SELECTOR RESOURCE GENERATION TESTS PASSED\n"; }
    catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; code = 1; }
    delete Configs::dataManager; Configs::dataManager = nullptr; return code;
}
