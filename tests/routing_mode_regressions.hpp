#pragma once
#include <stdexcept>
#include <iostream>
#include <future>
#include <thread>
#include <atomic>
#include <QLocalServer>
#include <QLocalSocket>
#include <QDataStream>
#include "include/api/RPC.h"
#if __has_include("include/configs/RoutingIntent.h")
#include "include/configs/RoutingIntent.h"
#include "include/configs/generate.h"
#include "include/configs/outbounds/selector.h"
#include "include/ui/utils/RoutingModeMenu.hpp"
namespace RoutingModeTests {
inline void check(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
// Actual IPC framing for the test builder's mandatory CheckConfig validation.
// No installed core is started and no production port is used.
struct ValidationPeer {
 QString name="mode-validation-"+QUuid::createUuid().toString(QUuid::Id128);
 std::atomic<bool> stop=false;std::promise<void> ready;std::thread worker;
 API::Client client;API::Client* previous=API::defaultClient;
 ValidationPeer():worker([this]{
  QLocalServer server;if(!server.listen(name))std::terminate();ready.set_value();
  while(!stop){if(!server.waitForNewConnection(50))continue;std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());QByteArray bytes;
   while(!stop&&socket->state()==QLocalSocket::ConnectedState){if(!socket->bytesAvailable())socket->waitForReadyRead(50);bytes+=socket->readAll();
    while(bytes.size()>=10){quint32 id,length;quint16 count;QDataStream head(bytes);head.setByteOrder(QDataStream::LittleEndian);head>>id>>count;if(bytes.size()<10+count)break;QDataStream tail(bytes.mid(6+count,4));tail.setByteOrder(QDataStream::LittleEndian);tail>>length;if(bytes.size()<10+count+length)break;auto method=bytes.mid(6,count);bytes.remove(0,10+count+length);
     QByteArray frame;QDataStream output(&frame,QIODevice::WriteOnly);output.setByteOrder(QDataStream::LittleEndian);output<<id<<quint8(method=="CheckConfig"?0:1)<<quint32(0);socket->write(frame);socket->waitForBytesWritten(1000);
    }
   }
  }
 }){ready.get_future().wait();auto* socket=new QLocalSocket;socket->connectToServer(name);check(socket->waitForConnected(2000),"test validator connect");client.Reconnect(socket);API::defaultClient=&client;}
 ~ValidationPeer(){API::defaultClient=previous;stop=true;worker.join();}
};
inline void run(){
 auto& dm=*Configs::dataManager;auto& settings=*dm.settingsRepo;
 auto legacy=settings.GetRoutingIntent();check(legacy.mode==Configs::RoutingMode::Rule&&legacy.targetID<0&&legacy.valid,"legacy Rule default");
 QString error;dm.getDatabase().execThrow("CREATE TRIGGER fail_mode BEFORE INSERT ON settings WHEN NEW.key='global_outbound_id' BEGIN SELECT RAISE(ABORT,'injected mode failure'); END");
 check(!settings.SetRoutingIntent(Configs::RoutingMode::GlobalProxy,123,&error),"mode save failure returned success");
 check(settings.GetRoutingIntent().revision==legacy.revision&&settings.GetRoutingIntent().mode==Configs::RoutingMode::Rule,"mode failure published intent");
 dm.getDatabase().execThrow("DROP TRIGGER fail_mode");
 {Configs::Database db(dm.getDatabase().Path());Configs::SettingsRepo reload(db);check(reload.GetRoutingIntent().mode==Configs::RoutingMode::Rule,"mode transaction partially committed");}
 auto group=Configs::GroupsRepo::NewGroup();group->name="Mode fixture";check(dm.groupsRepo->AddGroup(group),"mode group");
 auto node=Configs::ProfilesRepo::NewProfile("vless");node->name="mode node";node->outbound->ParseFromLink("vless://11111111-2222-3333-4444-555555555555@127.0.0.1:15443?encryption=none#mode");check(dm.profilesRepo->AddProfile(node,group->id),"mode node");
 auto nodeB=Configs::ProfilesRepo::NewProfile("vless");nodeB->name="mode node B";nodeB->outbound->ParseFromLink("vless://11111111-2222-3333-4444-555555555555@127.0.0.1:15444?encryption=none#modeB");check(dm.profilesRepo->AddProfile(nodeB,group->id),"mode node B");
 auto target=Configs::ProfilesRepo::NewProfile("selector");target->name="GLOBAL";target->Selector()->members={node->id,nodeB->id};target->Selector()->selectedID=node->id;check(dm.profilesRepo->AddProfile(target,group->id),"mode selector");
 auto route=Configs::RoutesRepo::NewRouteProfile();route->name="mode route";route->managedBySubscription=true;route->managedGroupID=group->id;
 auto rule=std::make_shared<Configs::RouteRule>();rule->domain_suffix={"example.test"};rule->outboundID=Configs::directID;route->Rules={rule};check(dm.routesRepo->AddRouteProfile(route),"mode route");
 settings.current_route_id=route->id;settings.spmode_vpn=false;settings.adblock_enable=false;settings.enable_dns_routing=true;settings.remote_dns="8.8.8.8";settings.direct_dns="1.1.1.1";settings.dns_final_out="direct";
 check(dm.routesRepo->SetLocalOverride(route->id,"suffix:local.test",Configs::bypass),"mode local seed");
 check(Configs::PreferredGlobalOutbound(node->id)==target->id,"relevant GLOBAL not preferred");
 check(settings.SetRoutingIntent(Configs::RoutingMode::GlobalProxy,target->id,&error),"mode save");
 {QMenu menu;int selected=0;RoutingModeMenu::Populate(&menu,settings.GetRoutingIntent(),false,{},"fixture",[&](auto mode){selected=mode==Configs::RoutingMode::Rule?1:2;},[]{});
 auto* ruleAction=menu.findChild<QAction*>("routingRule");auto* globalAction=menu.findChild<QAction*>("routingGlobalProxy");check(ruleAction&&globalAction&&globalAction->isChecked()&&!ruleAction->isChecked(),"mode menu checked intent");ruleAction->trigger();check(selected==1&&ruleAction->isChecked()&&!globalAction->isChecked(),"mode menu exclusive interaction");}
 {QMenu menu;RoutingModeMenu::Populate(&menu,settings.GetRoutingIntent(),true,{},"fixture",[](auto){},[]{});check(!menu.findChild<QAction*>("routingGlobalProxy")->isEnabled()&&!menu.findChild<QAction*>("routingGlobalOutbound")->isEnabled(),"mode controls enabled during transition");}
 {Configs::Database db(dm.getDatabase().Path());Configs::SettingsRepo reload(db);check(reload.GetRoutingIntent().mode==Configs::RoutingMode::GlobalProxy&&reload.GetRoutingIntent().targetID==target->id,"mode restart persistence");}
 auto result=Configs::BuildSingBoxConfig(node,Configs::BuildConfigOptions{true});if(!result->error.isEmpty())throw std::runtime_error(result->error.toStdString());
 const auto tag=QString("selector-%1").arg(target->id);auto generated=result->coreConfig["route"].toObject();
 check(generated["final"]==tag&&result->involvedProfiles.contains(target->id)&&result->selectorMemberTags.contains(target->id),"Global omitted independent selector dependency/final");
 for(auto v:generated["rules"].toArray()){auto o=v.toObject();check(!o.contains("domain_suffix"),"Global retained business/local rule");if(o.contains("port")&&o["action"]=="route")check(o["outbound"]==tag,"Global STUN used inactive Rule exit");}
 auto dns=result->coreConfig["dns"].toObject();bool remote=false;for(auto v:dns["servers"].toArray()){auto o=v.toObject();if(o["tag"]=="dns-remote")remote=o["detour"]==tag;}check(remote,"Global remote DNS detour disagrees");
 check(dns["rules"].toArray().last().toObject()["server"]=="dns-remote","Global inherited direct DNS final");
 check(dm.routesRepo->GetRoutingSnapshot(route->id).local.size()==1&&dm.routesRepo->GetRoutingSnapshot(route->id).profile->Rules.size()==1,"Global mutated saved rules");
 if(auto out=qEnvironmentVariable("NX_ROUTING_FIXTURE_DIR");!out.isEmpty()){QDir().mkpath(out);QFile f(out+"/global-generated.json");check(f.open(QIODevice::WriteOnly),"global fixture");f.write(QJsonDocument(result->coreConfig).toJson());}
 settings.spmode_vpn=true;settings.fake_dns=true;settings.fakeip_disable_ipv6=true;settings.fakeip_exclude={"suffix:excluded.test"};settings.vpn_l3_bridge=true;settings.adblock_enable=true;
 auto protectedBuild=Configs::BuildSingBoxConfig(node);check(protectedBuild->error.isEmpty(),"Global safety build");auto protectedDNS=protectedBuild->coreConfig["dns"].toObject();bool exclusion=false,fake=false,adblock=false;
 for(auto v:protectedDNS["rules"].toArray()){auto o=v.toObject();if(o["domain_suffix"].toArray().contains("excluded.test")&&o["server"]=="dns-direct")exclusion=true;if(o["server"]=="dns-fake")fake=true;}
 check(exclusion&&fake&&protectedDNS["independent_cache"].toBool(),"Global FakeIP exclusion/cache missing");
 for(auto v:protectedDNS["servers"].toArray()){auto o=v.toObject();if(o["tag"]=="dns-fake")check(!o.contains("inet6_range"),"Global IPv6 FakeIP policy changed");}
 for(auto v:protectedBuild->coreConfig["route"].toObject()["rules"].toArray())if(v.toObject()["rule_set"].toArray().contains("throne-adblocksingbox")&&v.toObject()["action"]=="reject")adblock=true;check(adblock,"Global removed configured adblock");
 check(protectedBuild->coreConfig["experimental"].toObject()["cache_file"].toObject()["store_fakeip"].toBool(),"Global FakeIP persistence lost");
 settings.spmode_vpn=false;settings.adblock_enable=false;settings.fake_dns=false;
 ValidationPeer validator;
 auto independentTest=Configs::BuildTestConfig({node});check(independentTest->error.isEmpty(),"standalone test inherited production Global");check(independentTest->outboundTags.size()==1,"test candidate was skipped rather than generated");check(!independentTest->coreConfig["route"].toObject()["final"].toString().startsWith("selector-"),"test config inherited Global target");
 auto missing=std::make_shared<Configs::RouteRule>();missing->domain_suffix={"inactive.test"};missing->outboundID=999998;auto draft=std::make_shared<Configs::RouteProfile>(*route);draft->Rules.append(missing);check(dm.routesRepo->Save(draft),"inactive missing dependency seed");check(Configs::BuildSingBoxConfig(node)->error.isEmpty(),"Global loaded inactive missing dependency");check(dm.routesRepo->Save(route),"restore base");
 auto raw=std::make_shared<Configs::RouteProfile>(*route);raw->isRaw=true;raw->rawRoute="{\"final\":\"direct\"}";check(dm.routesRepo->Save(raw),"raw seed");check(!Configs::BuildSingBoxConfig(node)->error.isEmpty(),"Global transformed raw route");check(dm.routesRepo->Save(route),"raw restore");
 settings.use_dns_object=true;settings.dns_object="{\"servers\":[{\"type\":\"local\",\"tag\":\"mine\"}],\"final\":\"mine\"}";check(Configs::BuildSingBoxConfig(node)->coreConfig["dns"].toObject()==QJsonDocument::fromJson(settings.dns_object.toUtf8()).object(),"Global rewrote custom DNS");settings.use_dns_object=false;
 target->Selector()->members.clear();dm.profilesRepo->Save(target);check(!Configs::BuildSingBoxConfig(node)->error.isEmpty(),"empty global target silently fell back");target->Selector()->members={node->id,nodeB->id};dm.profilesRepo->Save(target);
 target->name="Renamed";dm.profilesRepo->Save(target);check(Configs::BuildSingBoxConfig(node)->error.isEmpty(),"rename invalidated stable target ID");
 check(settings.SetRoutingIntent(Configs::RoutingMode::GlobalProxy,999999),"missing target intent");check(!Configs::BuildSingBoxConfig(node)->error.isEmpty(),"missing target silently fell back");
 check(settings.SetRoutingIntent(Configs::RoutingMode::Rule,target->id),"restore Rule");auto restored=Configs::BuildSingBoxConfig(node);check(restored->error.isEmpty(),"Rule restore failed");bool direct=false;for(auto v:restored->coreConfig["route"].toObject()["rules"].toArray())if(v.toObject()["domain_suffix"].toArray().contains("example.test"))direct=true;check(direct,"Rule round trip lost splits");
 if(auto out=qEnvironmentVariable("NX_ROUTING_FIXTURE_DIR");!out.isEmpty()){QFile f(out+"/rule-generated.json");check(f.open(QIODevice::WriteOnly),"Rule fixture");f.write(QJsonDocument(restored->coreConfig).toJson());}
 if(auto out=qEnvironmentVariable("NX_ROUTING_GUI_FIXTURE_DIR");!out.isEmpty()){
  auto smoke=Configs::ProfilesRepo::NewProfile("socks");smoke->name="Loopback GUI fixture";smoke->outbound->ParseFromLink("socks5://127.0.0.1:19080#loopback");check(dm.profilesRepo->AddProfile(smoke,group->id),"GUI loopback node");
  auto smokeRoute=Configs::RoutesRepo::NewRouteProfile();smokeRoute->name="Synthetic GUI smoke";check(dm.routesRepo->AddRouteProfile(smokeRoute),"GUI route");
  settings.current_route_id=smokeRoute->id;settings.current_group=group->id;settings.remember_id=smoke->id;settings.remember_enable=true;settings.disable_tray=true;
  settings.spmode_vpn=false;settings.remember_tun=false;settings.remember_system_proxy=false;settings.system_dns_set=false;settings.enable_dns_server=false;settings.windows_set_admin=false;settings.url_scheme_auto_register=false;settings.file_assoc_auto_register=false;
  settings.inbound_socks_port=2081;settings.core_dns_in_port=5534;settings.core_box_clash_api=9091;settings.core_box_api_port=0;settings.inbound_address="127.0.0.1";settings.sub_auto_update=-30;settings.route_auto_update=-1440;settings.auto_latency_test=-10;settings.direct_dns="local";settings.dns_final_out="direct";settings.Save();
  check(settings.SetRoutingIntent(Configs::RoutingMode::GlobalProxy,smoke->id),"GUI global intent");QDir().mkpath(out+"/config");const auto destination=out+"/config/throne.db";check(!QFile::exists(destination),"GUI fixture must be fresh");dm.getDatabase().backupTo(destination.toStdString());
  settings.remember_enable=false;settings.core_box_clash_api=0;
 }
 check(settings.SetRoutingIntent(Configs::RoutingMode::Rule,-1),"reset mode fixture");
 std::cout<<"PASS acknowledged mode transaction, target identity, Global dependencies/DNS/STUN and Rule round trip"<<std::endl;
}
}
#else
namespace RoutingModeTests {inline void run(){throw std::runtime_error("desktop routing intent and effective mode are missing");}}
#endif
