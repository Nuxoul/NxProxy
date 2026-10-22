#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGridLayout>
#include <QMessageBox>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QDir>
#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "include/global/Configs.hpp"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/database/RoutesRepo.h"
#include "include/database/TrafficStatsRepo.h"
#include "include/database/OtpProfilesRepo.h"
#include "include/configs/sub/SubscriptionParser.hpp"
#include "include/configs/sub/GroupUpdater.hpp"
#include "include/ui/mainWindow/UrlTestCleanup.hpp"
#include "include/ui/utils/ProfilePageLayout.hpp"
#include "include/ui/utils/ProfilesTableModel.h"
#include "include/ui/utils/ProfilesFilterProxyModel.h"

namespace {
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
void pump(int ms) {
    QElapsedTimer time; time.start();
    while (time.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1); }
}
std::shared_ptr<Configs::Group> addGroup(const QString& name) {
    auto g=Configs::GroupsRepo::NewGroup(); g->name=name;
    require(Configs::dataManager->groupsRepo->AddGroup(g), "add group"); return g;
}
std::shared_ptr<Configs::Profile> addNode(int gid, const QString& name, int latency=0) {
    auto p=Configs::ProfilesRepo::NewProfile("vless");
    p->outbound->ParseFromLink("vless://11111111-2222-3333-4444-555555555555@127.0.0.1:443?encryption=none#"+name);
    p->name=name; p->latency=latency;
    require(Configs::dataManager->profilesRepo->AddProfile(p,gid), "add node"); return p;
}
QString yaml(const QString& members="A, B, C", bool invalid=false) {
    QString body="proxies:\n  - {name: A, type: vless, server: 127.0.0.1, port: 443, uuid: 11111111-2222-3333-4444-555555555555}\n";
    if (invalid) body+="  - {name: BAD, type: vmess, server: 127.0.0.1, port: 443, http-opts: {path: /}}\n";
    body+="  - {name: B, type: vless, server: 127.0.0.2, port: 443, uuid: 11111111-2222-3333-4444-555555555555}\n";
    if (members.contains("C")) body+="  - {name: C, type: vless, server: 127.0.0.3, port: 443, uuid: 11111111-2222-3333-4444-555555555555}\n";
    body+="proxy-groups:\n  - {name: GLOBAL, type: select, proxies: ["+members+"], selected: A}\nrules:\n  - DOMAIN,example.org,GLOBAL\n  - MATCH,GLOBAL\n";
    return body;
}
void refresh(int gid) {
    std::atomic<bool> done=false;
    Subscription::updater()->RefreshGroup(gid,[&]{done.store(true);});
    QElapsedTimer timeout;timeout.start();
    while (!done.load() && timeout.elapsed()<10000) pump(5);
    require(done.load(),"subscription refresh timed out"); pump(30);
}
std::shared_ptr<Configs::Profile> selector(int gid) {
    for (int id:Configs::dataManager->groupsRepo->GetGroup(gid)->Profiles()) {
        auto p=Configs::dataManager->profilesRepo->GetProfile(id);
        if (p && p->Selector()) return p;
    }
    throw std::runtime_error("selector missing");
}
int named(int gid,const QString& name) {
    for (int id:Configs::dataManager->groupsRepo->GetGroup(gid)->Profiles()) {
        auto p=Configs::dataManager->profilesRepo->GetProfile(id);
        if (p && p->outbound->name==name) return id;
    }
    return -1;
}
void testParsingAndRefresh() {
    QList<QString> names;
    Subscription::ParseSink sink;
    sink.profile=[&](const auto& p){names.append(p->outbound->name);};
    auto result=Subscription::ParseText(yaml("A, B, C",true),sink);
    require(!result.complete && result.errorCount>0,"bad node must mark document incomplete");
    require(names.contains("C"),"per-node error must not stop later node diagnosis");
    require(!Subscription::ParseText("# proxies:\na\n: b",{}).complete,"syntax failure completeness");
    require(Subscription::ParseText("proxies: []",{}).complete,"explicit empty subscription remains valid");

    QTcpServer server; require(server.listen(QHostAddress::LocalHost,0),"local test server");
    QByteArray served=yaml().toUtf8();
    QObject::connect(&server,&QTcpServer::newConnection,[&]{
        while (server.hasPendingConnections()) {
            auto* socket=server.nextPendingConnection();
            auto responded=std::make_shared<bool>(false);
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket,responded]{
                if (*responded) return;
                if (!socket->readAll().contains("\r\n\r\n")) return;
                *responded=true;
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: "+QByteArray::number(served.size())+"\r\nConnection: close\r\n\r\n"+served);
                socket->disconnectFromHost();
            });
            QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        }
    });
    auto g=addGroup("Subscription regression");
    g->url=QString("http://127.0.0.1:%1/sub").arg(server.serverPort());
    Configs::dataManager->groupsRepo->Save(g);
    Configs::dataManager->settingsRepo->net_use_proxy=false;
    Configs::dataManager->settingsRepo->spmode_system_proxy=false;
    Configs::dataManager->settingsRepo->sub_clear=false;
    refresh(g->id);
    require(named(g->id,"A")>=0 && named(g->id,"B")>=0 && named(g->id,"C")>=0,"normal subscription import");
    auto sel=selector(g->id);
    sel->Selector()->selectedID=named(g->id,"B"); Configs::dataManager->profilesRepo->Save(sel);
    auto custom=Configs::RoutesRepo::NewRouteProfile();custom->name="User route";
    Configs::dataManager->routesRepo->AddRouteProfile(custom);
    Configs::dataManager->settingsRepo->current_route_id=custom->id;
    g->name="Renamed subscription";Configs::dataManager->groupsRepo->Save(g);
    require(Configs::dataManager->settingsRepo->SetRoutingIntent(Configs::RoutingMode::GlobalProxy,sel->id),"refresh mode intent seed");
    const auto modeBeforeRefresh=Configs::dataManager->settingsRepo->GetRoutingIntent();
    const auto routesBeforeRename=Configs::dataManager->routesRepo->GetAllRouteProfileIds();
    refresh(g->id);
    require(Configs::dataManager->routesRepo->GetAllRouteProfileIds()==routesBeforeRename,"group rename created a second managed route");
    require(selector(g->id)->Selector()->selectedID==named(g->id,"B"),"local choice wins over remote selected");
    require(Configs::dataManager->settingsRepo->current_route_id==custom->id,"refresh preserves user route");
    const auto before=g->Profiles();const auto allBefore=Configs::dataManager->profilesRepo->GetAllProfileIds();
    const auto selectedBefore=sel->Selector()->selectedID;const auto updateBefore=g->sub_last_update;
    int managedID=-1;
    for (const auto& route:Configs::dataManager->routesRepo->GetAllRouteProfiles())
        if (route->managedBySubscription && route->managedGroupID==g->id) managedID=route->id;
    require(managedID>=0,"managed route missing");
    require(Configs::dataManager->routesRepo->SetLocalOverride(managedID,"suffix:override.test",Configs::proxy),"managed local decision");
    const auto managedBefore=Configs::dataManager->routesRepo->GetRouteProfile(managedID);
    const int ruleCount=managedBefore->Rules.size();const int defaultBefore=managedBefore->defaultOutboundID;
    for (bool clear:{false,true}) {
        Configs::dataManager->settingsRepo->sub_clear=clear;
        for (const auto& broken:{yaml("A, B, C",true).toUtf8(),QByteArray("# proxies:\na\n: b")}) {
            served=broken;refresh(g->id);
            require(g->Profiles()==before && Configs::dataManager->profilesRepo->GetAllProfileIds()==allBefore,"failed refresh mutated node data");
            require(sel->Selector()->selectedID==selectedBefore,"failed refresh mutated selected member");
            require(Configs::dataManager->settingsRepo->current_route_id==custom->id,"failed refresh changed route");
            require(g->sub_last_update==updateBefore,"failed refresh recorded successful timestamp");
            const auto managed=Configs::dataManager->routesRepo->GetRouteProfile(managedID);
            require(managed->Rules.size()==ruleCount && managed->defaultOutboundID==defaultBefore,"failed refresh changed managed routing rules");
        }
    }
    // A real SQLite trigger rejects replacement insertions. Both refresh modes must retain old rows.
    served=yaml("A, B, C").toUtf8();served.replace("name: A", "name: NewA");
    Configs::dataManager->getDatabase().execThrow("CREATE TRIGGER reject_test_insert BEFORE INSERT ON profiles BEGIN SELECT RAISE(ABORT, 'test write failure'); END");
    for (bool clear:{false,true}) {
        Configs::dataManager->settingsRepo->sub_clear=clear;
        refresh(g->id);
        require(g->Profiles()==before && Configs::dataManager->profilesRepo->GetAllProfileIds()==allBefore,"failed storage deleted old subscription");
    }
    Configs::dataManager->getDatabase().execThrow("DROP TRIGGER reject_test_insert");
    Configs::dataManager->settingsRepo->sub_clear=false;
    const int oldC=named(g->id,"C");served=yaml("A, B").toUtf8();refresh(g->id);
    require(Configs::dataManager->profilesRepo->GetProfile(oldC)==nullptr,"valid refresh must delete truly removed node");
    require(selector(g->id)->Selector()->selectedID==named(g->id,"B"),"valid unchanged local selection lost");
    require(Configs::dataManager->routesRepo->GetRoutingSnapshot(managedID).local.size()==1,"full subscription refresh removed local override");
    served="proxies: []\nrules: []\n";refresh(g->id);
    require(Configs::dataManager->routesRepo->GetRoutingSnapshot(managedID).local.size()==1,"empty subscription rules removed local override");
    served=yaml("A, B").toUtf8();refresh(g->id);
    sel=selector(g->id);sel->Selector()->selectedID=1234567;Configs::dataManager->profilesRepo->Save(sel);
    refresh(g->id);require(selector(g->id)->Selector()->selectedID==named(g->id,"A"),"invalid local selection falls back to remote");
    const auto beforeClear=g->Profiles();
    Configs::dataManager->settingsRepo->sub_clear=true;refresh(g->id);
    require(named(g->id,"A")>=0 && named(g->id,"B")>=0,"successful clear refresh missing replacements");
    require(Configs::dataManager->settingsRepo->current_route_id==custom->id,"clear refresh switched route");
    for (int id:beforeClear) {
        auto p=Configs::dataManager->profilesRepo->GetProfile(id);
        if (p && p->type=="selector") continue;
        require(p==nullptr,"successful clear refresh retained old node");
    }
    Configs::dataManager->settingsRepo->sub_clear=false;
    require(Configs::dataManager->settingsRepo->GetRoutingIntent()==modeBeforeRefresh,"complete/empty/renamed refresh changed mode or global target");
    require(Configs::dataManager->settingsRepo->SetRoutingIntent(Configs::RoutingMode::Rule,-1),"restore refresh mode");
    std::cout<<"PASS R1 R4 R5 parser completeness, failed refresh immutability, valid deletions, local choices and routing intent\n";
}
void testSortingAndConcurrency() {
    auto g=addGroup("Sorting regression");
    auto p80=addNode(g->id,"slow",80), p20=addNode(g->id,"fast",20), unknown=addNode(g->id,"unknown",0), failed=addNode(g->id,"failed",-1);
    for(auto order:{Configs::testBy::dlSpeed,Configs::testBy::ulSpeed,Configs::testBy::ipOut}) {
        g->test_sort_by=order;
        require(g->SortProfiles({GroupSortMethod::ByLatency,false}),"latency sort failed");
        auto ids=g->Profiles();require(ids[0]==p20->id && ids[1]==p80->id,"URL sort not latency ascending");
        require(g->test_sort_by==order,"automatic sorting overwrote manual preference");
    }
    g->profiles.append(999999);require(g->SortProfiles({GroupSortMethod::ByLatency,false}),"missing profile sort");
    require(g->Profiles().last()==999999,"missing profile not last");g->profiles.removeAll(999999);
    auto stress=addGroup("Lock regression");std::atomic<bool> finished=false;
    auto worker=std::async(std::launch::async,[&]{
        std::thread add([&]{ for(int n=0;n<120;++n) addNode(stress->id,QString("p%1").arg(n),n+1);finished=true; });
        std::thread sort([&]{ while(!finished.load()) stress->SortProfiles({GroupSortMethod::ByLatency,false}); });
        add.join();sort.join();
    });
    if(worker.wait_for(std::chrono::seconds(20))!=std::future_status::ready) { std::cerr<<"FAIL concurrent sort/add deadlocked\n";std::quick_exit(2); }
    worker.get();auto ids=stress->Profiles();require(ids.size()==120,"concurrent additions lost");
    QSet<int> distinct(ids.cbegin(),ids.cend());require(distinct.size()==120,"concurrent add duplicated IDs");
    std::cout<<"PASS R2 R7 concurrent real-repo add/sort and dedicated latency ordering\n";
}
void testCleanupAndPersistence() {
    auto a=addGroup("Cleanup enabled"), b=addGroup("Cleanup disabled");a->auto_clear_unavailable=true;b->auto_clear_unavailable=false;
    auto pa=addNode(a->id,"a failed",-1),pb=addNode(b->id,"b failed",-1),stale=addNode(a->id,"old failure not in result",-1),connected=addNode(a->id,"connect only",Configs::kLatencyConnectOnly);
    auto ids=UrlTestCleanup::eligibleFailures({pb->id,pa->id,connected->id});require(ids==QList<int>{pa->id},"cleanup crossed group flag/connect-only");
    auto reversed=UrlTestCleanup::eligibleFailures({pa->id,pb->id});require(reversed==ids,"cleanup depends on first group order");
    Configs::dataManager->profilesRepo->BatchDeleteProfiles(ids);
    require(Configs::dataManager->profilesRepo->GetProfile(pb->id)!=nullptr && Configs::dataManager->profilesRepo->GetProfile(stale->id)!=nullptr,"cleanup deleted disabled group or stale failure");
    require(UrlTestCleanup::eligibleFailures({}).isEmpty(),"empty result means whole group");
    require(UrlTestCleanup::allowed(true,false,false,true),"completed URL cleanup rejected");
    require(!UrlTestCleanup::allowed(true,true,false,true),"cancelled sweep can clean");
    require(!UrlTestCleanup::allowed(true,false,true,true),"aborted results can clean");
    require(!UrlTestCleanup::allowed(true,false,false,false),"stale generation can clean");
    require(!UrlTestCleanup::allowed(false,false,false,true),"IP test can clean");
    auto& settings=Configs::dataManager->settingsRepo;settings->auto_latency_test_last=1791420000;settings->auto_latency_test=10;settings->Save();
    Configs::SettingsRepo reloaded(Configs::dataManager->getDatabase());
    require(reloaded.auto_latency_test_last==1791420000 && reloaded.auto_latency_test==10,"timestamp Save/Load missing");
    std::cout<<"PASS R3 R9 group-scoped explicit cleanup and timestamp round-trip\n";
}
void testNarrowTestResults() {
    auto g=addGroup("Narrow results");auto a=addNode(g->id,"result-a",10),b=addNode(g->id,"result-b",20);
    auto& repo=*Configs::dataManager->profilesRepo;auto& db=Configs::dataManager->getDatabase();
    a->dl_speed="dl";a->ul_speed="ul";a->ip_out="ip";a->test_country="ZZ";repo.Save(a);
    auto saved=repo.SaveTestResults({{a->id,Configs::TestResult::Kind::Url,42,123}});
    require(saved.success && saved.appliedIDs==QList<int>{a->id},"narrow URL save");
    require(a->latency==42 && a->latency_at==123 && a->dl_speed=="dl" && a->test_country=="ZZ","URL field/cache independence");
    saved=repo.SaveTestResults({{a->id,Configs::TestResult::Kind::Ip,999,999,{}, {},QString("US"),QString("1.2.3.4")}});
    require(saved.success && a->latency==42 && a->latency_at==123 && a->dl_speed=="dl" && a->ip_out=="1.2.3.4","IP field independence");
    saved=repo.SaveTestResults({{a->id,Configs::TestResult::Kind::Speed,{},0,QString("new-dl")}});
    require(saved.success && a->ul_speed=="ul" && a->latency==42 && a->ip_out=="1.2.3.4","DL field independence");
    db.execThrow("CREATE TRIGGER reject_result BEFORE UPDATE OF latency ON profiles WHEN NEW.id="+std::to_string(b->id)+" BEGIN SELECT RAISE(ABORT,'injected result failure'); END");
    saved=repo.SaveTestResults({{a->id,Configs::TestResult::Kind::Url,-1,200},{b->id,Configs::TestResult::Kind::Url,-1,200}});
    require(!saved.success && saved.appliedIDs.isEmpty() && a->latency==42 && b->latency==20,"result failure published cache or IDs");
    auto row=db.queryThrow("SELECT latency FROM profiles WHERE id=?",a->id);
    require(row->executeStep() && row->getColumn(0).getInt()==42,"result batch did not roll back");row.reset();
    require(!UrlTestCleanup::allowed(true,false,!saved.success,true),"failed result save can clean");
    db.execThrow("DROP TRIGGER reject_result");
    QList<int> deleted{b->id};repo.BatchDeleteProfiles(deleted);
    saved=repo.SaveTestResults({{b->id,Configs::TestResult::Kind::Url,-1,201}});
    require(saved.success && saved.appliedIDs.isEmpty() && repo.GetProfile(b->id)==nullptr,"result resurrected deleted ID");
    std::atomic<bool> start=false;
    std::thread subscription([&]{
        Configs::Database other(db.Path());while(!start.load())std::this_thread::yield();
        for(int i=0;i<100;++i) other.execThrow("UPDATE profiles SET name='subscription',outbound_json='subscription-json',traffic_dl=333 WHERE id=?",a->id);
    });
    start=true;
    for(int i=0;i<100;++i)require(repo.SaveTestResults({{a->id,Configs::TestResult::Kind::Url,70+i,300+i}}).success,"concurrent result save");
    subscription.join();
    row=db.queryThrow("SELECT name,outbound_json,traffic_dl,latency FROM profiles WHERE id=?",a->id);
    require(row->executeStep() && row->getColumn(0).getText()==std::string("subscription") && row->getColumn(1).getText()==std::string("subscription-json") && row->getColumn(2).getInt()==333 && row->getColumn(3).getInt()==169,"concurrent config/result lost");
    std::cout<<"PASS O1 real repo narrow fields, rollback, deleted IDs and concurrent subscription updates\n";
}
void testPages() {
    auto g=addGroup("Filter regression");auto a=addNode(g->id,"visible a"),b=addNode(g->id,"visible b");
    Configs::dataManager->settingsRepo->current_group=g->id;
    ProfilesTableModel source;ProfilesFilterProxyModel filter;filter.setSourceModel(&source);source.refreshTable(g->Profiles(),true);
    require(filter.rowCount()==2,"base group model");filter.setAllowedProfileIds({a->id});require(filter.rowCount()==1,"selector member filter");
    filter.clearAllowedProfileIds();require(filter.rowCount()==2,"no-selector group must restore full node list");
    QTabWidget tabs;QWidget hint,list,toolbar,table;QList<QWidget*> pages;
    for(int i=0;i<3;++i){auto* page=new QWidget;page->setLayout(new QGridLayout);tabs.addTab(page,QString::number(i));pages.append(page);}
    tabs.resize(900,500);tabs.show();
    for(int i=0;i<3;++i){tabs.setCurrentIndex(i);ProfilePageLayout::attach(pages[i],&hint,&list,&toolbar,&table);hint.show();list.show();toolbar.show();pump(10);
        require(hint.parentWidget()==pages[i] && list.parentWidget()==pages[i] && toolbar.parentWidget()==pages[i] && table.parentWidget()==pages[i],"shared controls left on hidden tab");
        require(list.isVisibleTo(&tabs) && table.isVisibleTo(&tabs),"panel filtered from invisible tab");
        auto* layout=qobject_cast<QGridLayout*>(pages[i]->layout());require(layout->itemAtPosition(1,0)->widget()==&list && layout->itemAtPosition(1,1)->widget()==&table,"two-column layout mismatch");}
    ProfilePageLayout::attach(pages[0],&hint,&list,&toolbar,&table);
    for(int i=2;i>0;--i){tabs.removeTab(i);delete pages[i];}
    require(list.parentWidget()==pages[0] && table.parentWidget()==pages[0],"tab rebuilding destroyed shared controls");
    std::cout<<"PASS R6 live Qt control reparenting across 3 tabs and safe rebuild\n";
}
}
#include "background_regressions.hpp"
#include "connection_regressions.hpp"
#include "selector_panel_regressions.hpp"
#include "tun_recovery_cases.hpp"
#include "tun_recovery_qt.hpp"
#include "local_route_regressions.hpp"
#include "routing_mode_regressions.hpp"
#include "tun_journal_regressions.hpp"
#include "routing_integration_findings.hpp"
int main(int argc,char** argv){
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
    if(app.arguments().size()==3&&app.arguments()[1]=="--tun-journal-child")return TunJournalTests::child(app.arguments()[2]);
    if(app.arguments().size()==4&&app.arguments()[1]=="--tun-evidence-child")return TunJournalTests::evidenceChild(app.arguments()[2],app.arguments()[3]=="failed");
    QTemporaryDir dir;require(dir.isValid(),"temporary database dir");QDir::setCurrent(dir.path());
    MW_show_log=[](const QString&){};MW_dialog_message=[](MwMessage,QStringList){};
    Configs::initDB((dir.path()+"/throne.db").toStdString());
    Configs::dataManager->settingsRepo->started_id=Configs::NoProfileId;
    QTimer dismiss;QObject::connect(&dismiss,&QTimer::timeout,[]{for(auto* w:QApplication::topLevelWidgets()) if(auto* box=qobject_cast<QMessageBox*>(w)) box->done(QMessageBox::Ok);});dismiss.start(5);
    int exit=0;
    try{RoutingFindingsTests::run();TunJournalTests::run();RoutingModeTests::run();LocalRouteTests::run();testParsingAndRefresh();testSortingAndConcurrency();testCleanupAndPersistence();testNarrowTestResults();testPages();runConnectionRegressions();runSelectorPanelRegressions();runTunRecoveryCases();runTunRecoveryQt();runBackgroundRegressions();std::cout<<"ALL REVIEW REGRESSIONS PASSED\n";}
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";exit=1;}
    pump(50);delete Configs::dataManager;Configs::dataManager=nullptr;return exit;
}
