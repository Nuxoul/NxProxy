#pragma once
#include <stdexcept>
#include <thread>
#include <atomic>
#include <iostream>
#include <QFile>
#include <QPushButton>
#include <QTableWidget>
#include <QLineEdit>
#include <QTimer>
#include "include/ui/setting/dialog_manage_routes.h"
#include "include/configs/generate.h"
#include "include/database/RoutesRepo.h"
namespace LocalRouteTests {
inline void require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
template<class Repo> void commands(Repo& repo,int id){
 if constexpr(requires {repo.SetLocalOverride(id,QString{},Configs::proxy);repo.GetRoutingSnapshot(id);}) {
  QString error;
  require(repo.SetLocalOverride(id,"suffix: API.Example.Test. ",Configs::proxy,&error),"save local suffix");
  auto snapshot=repo.GetRoutingSnapshot(id,&error);
  require(snapshot.profile&&snapshot.local.size()==1,"local round trip");
  require(snapshot.local.first().value=="api.example.test","normalize domain without widening");
  const auto priority=snapshot.local.first().priority;
  require(repo.SetLocalOverride(id,"suffix:api.example.test",Configs::proxy),"duplicate accepted");
  require(repo.GetRoutingSnapshot(id).local.first().priority==priority,"duplicate must be a no-op");
  require(repo.SetLocalOverride(id,"suffix:api.example.test",Configs::bypass),"replace opposite action");
  require(repo.GetRoutingSnapshot(id).local.size()==1&&repo.GetRoutingSnapshot(id).local.first().action==Configs::bypass,"opposite conflict reconciled");
  require(repo.SetLocalOverride(id,"suffix:api.example.test",Configs::proxy),"restore proxy");
  require(repo.SetLocalOverride(id,"processName:Sample.EXE",Configs::proxy),"local process");
  require(!repo.SetLocalOverride(id,"suffix:https://example.test/path",Configs::proxy),"reject URL not domain");
  require(!repo.SetLocalOverride(id,"suffix:",Configs::proxy),"reject empty match");
  const auto before=repo.GetRoutingSnapshot(id);
  auto base=std::make_shared<Configs::RouteProfile>(*before.profile);base->Rules.clear();base->name="refreshed";
  require(repo.Save(base),"replace imported rules");
  require(repo.GetRoutingSnapshot(id).local.size()==2,"base replacement lost local overrides");
  require(repo.GetRoutingSnapshot(id).profile->Rules.isEmpty(),"local overrides polluted base rules");
  auto item=repo.GetRoutingSnapshot(id).local.first();
  require(repo.SetLocalOverride(id,item.RawRule(),Configs::bypass),"new local decision");
  require(!repo.RemoveLocalOverride(id,item.id,item.priority),"stale removal must fail");
  const auto current=repo.GetRoutingSnapshot(id).local.first();
  require(repo.RemoveLocalOverride(id,current.id,current.priority),"remove current local entry");
  require(repo.GetRoutingSnapshot(id).local.size()==1,"local removal scope");
  std::cout<<"PASS local normalization, persistence, replacement, stale edit and removal"<<std::endl;
 } else {throw std::runtime_error("persistent local override command is missing");}
}
inline void generation() {
 auto& dm=*Configs::dataManager;
 auto group=Configs::GroupsRepo::NewGroup();group->name="Local generation";require(dm.groupsRepo->AddGroup(group),"generation group");
 auto node=Configs::ProfilesRepo::NewProfile("vless");node->name="synthetic";
 node->outbound->ParseFromLink("vless://11111111-2222-3333-4444-555555555555@127.0.0.1:15443?encryption=none#synthetic");
 require(dm.profilesRepo->AddProfile(node,group->id),"generation node");
 auto route=Configs::RoutesRepo::NewRouteProfile();route->name="generation";
 auto direct=std::make_shared<Configs::RouteRule>();direct->outboundID=Configs::directID;direct->domain_suffix={"example.test"};direct->process_name={"sample.exe"};
 // Separate ordinary domain/process rules, not an accidental AND condition.
 auto process=std::make_shared<Configs::RouteRule>(*direct);process->domain_suffix.clear();direct->process_name.clear();route->Rules={direct,process};
 require(dm.routesRepo->AddRouteProfile(route),"generation route");
 require(dm.routesRepo->SetLocalOverride(route->id,"suffix:api.example.test",Configs::proxy),"generation local domain");
 require(dm.routesRepo->SetLocalOverride(route->id,"processName:sample.exe",Configs::proxy),"generation local process");
 dm.settingsRepo->current_route_id=route->id;dm.settingsRepo->adblock_enable=false;dm.settingsRepo->enable_dns_routing=true;
 dm.settingsRepo->spmode_vpn=false;dm.settingsRepo->remote_dns="8.8.8.8";dm.settingsRepo->direct_dns="1.1.1.1";dm.settingsRepo->dns_final_out="remote";
 auto result=Configs::BuildSingBoxConfig(node,Configs::BuildConfigOptions{true});
 if(!result->error.isEmpty())throw std::runtime_error(result->error.toStdString());
 auto rules=result->coreConfig["route"].toObject()["rules"].toArray();int local=-1,base=-1;bool processProxy=false;
 for(int i=0;i<rules.size();++i){auto rule=rules[i].toObject();if(rule["domain_suffix"].toArray().contains("api.example.test")&&rule["outbound"]=="proxy")local=i;if(rule["domain_suffix"].toArray().contains("example.test")&&rule["outbound"]=="direct")base=i;if((rule["process_name"].toArray().contains("sample.exe")||!rule["process_path_regex"].toArray().isEmpty())&&rule["outbound"]=="proxy")processProxy=true;}
 require(local>=0&&base>local&&processProxy,"production generator omitted/ordered local proxy overrides incorrectly");
 auto dns=result->coreConfig["dns"].toObject()["rules"].toArray();int localDns=-1,baseDns=-1;
 for(int i=0;i<dns.size();++i){auto rule=dns[i].toObject();if(rule["domain_suffix"].toArray().contains("api.example.test")&&rule["server"]=="dns-remote")localDns=i;if(rule["domain_suffix"].toArray().contains("example.test")&&rule["server"]=="dns-direct")baseDns=i;}
 require(localDns>=0&&baseDns>localDns,"local proxy DNS is shadowed by imported direct DNS");
 if(auto out=qEnvironmentVariable("NX_ROUTING_FIXTURE_DIR");!out.isEmpty()){QDir().mkpath(out);QFile file(out+"/local-generated.json");require(file.open(QIODevice::WriteOnly),"write generated fixture");file.write(QJsonDocument(result->coreConfig).toJson());}
 std::cout<<"PASS actual production local route and DNS precedence"<<std::endl;
}
inline void storage() {
 auto& dm=*Configs::dataManager;auto& repo=*dm.routesRepo;
 auto route=Configs::RoutesRepo::NewRouteProfile();route->name="durability";require(repo.AddRouteProfile(route),"durability route");
 require(repo.SetLocalOverride(route->id,"suffix:keep.test",Configs::proxy),"durability seed");
 dm.getDatabase().execThrow("CREATE TRIGGER fail_local_write BEFORE INSERT ON route_local_overrides BEGIN SELECT RAISE(ABORT,'injected local failure'); END");
 require(!repo.SetLocalOverride(route->id,"suffix:new.test",Configs::bypass),"local write failure returned success");
 require(repo.GetRoutingSnapshot(route->id).local.size()==1,"local failed transaction changed rows");
 dm.getDatabase().execThrow("DROP TRIGGER fail_local_write");
 auto stale=std::make_shared<Configs::RouteProfile>(*route);
 std::atomic<bool> writesOK=true;
 std::thread refresh([&]{for(int i=0;i<25;++i){auto draft=std::make_shared<Configs::RouteProfile>(*route);draft->name=QString::number(i);if(!repo.Save(draft))writesOK=false;}});
 for(int i=0;i<25;++i)if(!repo.SetLocalOverride(route->id,QString("domain:concurrent%1.test").arg(i),Configs::proxy))writesOK=false;
 refresh.join();require(writesOK.load()&&repo.GetRoutingSnapshot(route->id).local.size()==26,"concurrent base/local update lost entry");
 require(repo.Save(stale)&&repo.GetRoutingSnapshot(route->id).local.size()==26,"stale editor removed local layer");
 auto clone=std::make_shared<Configs::RouteProfile>(*route);clone->id=-1;clone->localOverridesSourceID=route->id;clone->name="clone";
 auto all=repo.GetAllRouteProfiles();all.append(clone);require(repo.UpdateRouteProfiles(all),"clone commit");
 require(repo.GetRoutingSnapshot(clone->id).local.size()==26,"clone omitted local layer");
 auto copied=repo.GetRoutingSnapshot(clone->id).local.first();require(repo.RemoveLocalOverride(clone->id,copied.id,copied.priority),"clone local remove");
 require(repo.GetRoutingSnapshot(route->id).local.size()==26,"clone mutated source local layer");
 const auto path=dm.getDatabase().Path()+".routes-backup";Configs::BackupParts parts;parts.routes=true;
 dm.getDatabase().backupSelective(path,parts);
 require(repo.SetLocalOverride(route->id,"domain:after-backup.test",Configs::proxy),"post-backup local write");
 dm.getDatabase().restoreSelective(path,parts);
 require(repo.GetRoutingSnapshot(route->id).local.size()==26,"selective route restore lost or retained wrong local data");
 {SQLite::Database old(path,SQLite::OPEN_READWRITE);old.exec("DROP TABLE route_local_overrides");}
 dm.getDatabase().restoreSelective(path,parts);
 require(repo.GetRoutingSnapshot(route->id).local.isEmpty(),"old backup retained previous local layer");
 repo.DeleteRouteProfile(route->id);repo.DeleteRouteProfile(clone->id);
 std::cout<<"PASS local rollback, concurrent refresh, stale editor, clone and old/new backup"<<std::endl;
}
inline void managerUI() {
 auto& dm=*Configs::dataManager;auto route=Configs::RoutesRepo::NewRouteProfile();route->name="UI local route";
 require(dm.routesRepo->AddRouteProfile(route),"UI fixture route");
 require(dm.routesRepo->SetLocalOverride(route->id,"suffix:ui.test",Configs::proxy),"UI local seed");
 DialogManageRoutes settings;
 auto* list=settings.findChild<QListWidget*>("route_profiles");auto* button=settings.findChild<QPushButton*>("localOverridesButton");
 require(list&&button,"local manager button missing");
 for(int i=0;i<list->count();++i)if(list->item(i)->text()==route->name)list->setCurrentRow(i);
 QString failure;
 QTimer::singleShot(0,[&]{
  auto* dialog=settings.findChild<QDialog*>("localOverridesDialog");
  try {
   require(dialog!=nullptr,"local manager did not open");
   auto* table=dialog->findChild<QTableWidget*>("localOverridesTable");
   require(table&&table->rowCount()==1,"local manager omitted stored row");
   table->selectRow(0);
   auto* match=dialog->findChild<QLineEdit*>("localOverrideMatch");
   auto* save=dialog->findChild<QPushButton*>("localOverrideSave");
   require(match&&save,"local manager controls");match->setText("suffix:edited-ui.test");save->click();
   require(dm.routesRepo->GetRoutingSnapshot(route->id).local.first().value=="edited-ui.test","UI save did not persist edit");
   auto* remove=dialog->findChild<QPushButton*>("localOverrideDelete");require(remove!=nullptr,"local delete control");
   table->selectRow(0);remove->click();require(dm.routesRepo->GetRoutingSnapshot(route->id).local.isEmpty(),"UI delete did not persist");
  } catch(const std::exception& e){failure=QString::fromUtf8(e.what());}
  if(dialog)dialog->reject();
 });
 button->click();settings.reject();
 if(!failure.isEmpty())throw std::runtime_error(failure.toStdString());
 require(dm.routesRepo->GetRoutingSnapshot(route->id).local.isEmpty(),"parent Cancel undid immediate local save");
 dm.routesRepo->DeleteRouteProfile(route->id);
 std::cout<<"PASS actual local manager open/edit/delete and independent parent cancellation"<<std::endl;
}
inline void saveFailure() {
 auto& dm=*Configs::dataManager;auto route=Configs::RoutesRepo::NewRouteProfile();route->name="before";
 require(dm.routesRepo->AddRouteProfile(route),"save-failure fixture");
 dm.getDatabase().execThrow("CREATE TRIGGER local_fail_save BEFORE UPDATE ON route_profiles BEGIN SELECT RAISE(ABORT,'injected'); END");
 auto draft=std::make_shared<Configs::RouteProfile>(*route);draft->name="after";
 require(!dm.routesRepo->Save(draft),"failed route Save returned success");
 require(dm.routesRepo->GetRouteProfile(route->id)->name=="before","failed Save published cache");
 dm.getDatabase().execThrow("DROP TRIGGER local_fail_save");dm.routesRepo->DeleteRouteProfile(route->id);
}
inline void run(){
 saveFailure();storage();managerUI();
 auto& repo=*Configs::dataManager->routesRepo;
 auto route=Configs::RoutesRepo::NewRouteProfile();route->name="Local override regression";
 auto rule=std::make_shared<Configs::RouteRule>();rule->domain_suffix={"example.test"};rule->outboundID=Configs::directID;route->Rules.append(rule);
 require(repo.AddRouteProfile(route),"create local test route");commands(repo,route->id);repo.DeleteRouteProfile(route->id);generation();
}
}
