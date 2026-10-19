#pragma once
#include "include/ui/setting/RouteItem.h"
#include "include/ui/setting/RawRouteItem.h"
namespace RoutingFindingsTests {
inline void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
inline void staleDestination(){
 auto& repo=*Configs::dataManager->routesRepo;auto route=Configs::RoutesRepo::NewRouteProfile();route->name="stale destination";check(repo.AddRouteProfile(route),"stale route");
 check(repo.SetLocalOverride(route->id,"suffix:old.test",Configs::proxy),"old row");auto expected=repo.GetRoutingSnapshot(route->id).local.first();
 check(repo.SetLocalOverride(route->id,"suffix:new.test",Configs::bypass),"newer destination row");auto newer=repo.GetRoutingSnapshot(route->id).local.first();
 check(!repo.SetLocalOverride(route->id,"suffix:new.test",Configs::proxy,nullptr,&expected),"stale edit overwrote another destination key");
 auto rows=repo.GetRoutingSnapshot(route->id).local;check(rows.size()==2&&rows.first().id==newer.id&&rows.first().priority==newer.priority&&rows.first().action==Configs::bypass,"conflicting edit mutated either row");repo.DeleteRouteProfile(route->id);
}
inline void editedClone(bool raw){
 auto& repo=*Configs::dataManager->routesRepo;auto route=Configs::RoutesRepo::NewRouteProfile();route->name=raw?"raw clone source":"structured clone source";check(repo.AddRouteProfile(route),"clone source");check(repo.SetLocalOverride(route->id,"suffix:clone.test",Configs::proxy),"clone local");
 auto pending=std::make_shared<Configs::RouteProfile>(*route);pending->id=-1;pending->localOverridesSourceID=route->id;pending->name="edited clone";
 std::shared_ptr<Configs::RouteProfile> edited;
 if(raw){RawRouteItem editor(nullptr,pending);edited=editor.chain;}else{RouteItem editor(nullptr,pending);edited=editor.chain;}
 check(edited->localOverridesSourceID==route->id,"editor copy lost clone local provenance");
 auto all=repo.GetAllRouteProfiles();all.append(edited);check(repo.UpdateRouteProfiles(all),"edited clone commit");check(repo.GetRoutingSnapshot(edited->id).local.size()==1,"edited clone omitted local layer");repo.DeleteRouteProfile(edited->id);repo.DeleteRouteProfile(route->id);
}
inline void crossGroup(){
 auto& dm=*Configs::dataManager;auto& settings=*dm.settingsRepo;auto a=Configs::GroupsRepo::NewGroup(),b=Configs::GroupsRepo::NewGroup();a->name="Start A";b->name="Target B";check(dm.groupsRepo->AddGroup(a)&&dm.groupsRepo->AddGroup(b),"cross group fixture");
 auto add=[&](int gid,const QString& address){auto node=Configs::ProfilesRepo::NewProfile("vless");node->outbound->ParseFromLink("vless://11111111-2222-3333-4444-555555555555@"+address+":443?encryption=none#cross");check(dm.profilesRepo->AddProfile(node,gid),"cross group node");return node;};
 auto start=add(a->id,"203.0.113.11"),target=add(b->id,"198.51.100.42"),v6=add(b->id,"[2001:db8::42]");
 auto selector=Configs::ProfilesRepo::NewProfile("selector");selector->name="GLOBAL";selector->Selector()->members={target->id,v6->id};selector->Selector()->selectedID=target->id;check(dm.profilesRepo->AddProfile(selector,b->id),"cross target selector");
 auto route=Configs::RoutesRepo::NewRouteProfile();route->name="Cross Global";check(dm.routesRepo->AddRouteProfile(route),"cross route");settings.current_route_id=route->id;settings.spmode_vpn=true;settings.adblock_enable=false;settings.enable_tun_routing=false;check(settings.SetRoutingIntent(Configs::RoutingMode::GlobalProxy,selector->id),"cross mode");
 auto result=Configs::BuildSingBoxConfig(start);check(result->error.isEmpty(),"cross group build");QJsonArray exclude;for(auto v:result->coreConfig["inbounds"].toArray())if(v.toObject()["type"]=="tun")exclude=v.toObject()["route_exclude_address"].toArray();
 settings.spmode_vpn=false;settings.SetRoutingIntent(Configs::RoutingMode::Rule,-1);
 check(exclude.contains("198.51.100.42/32")&&exclude.contains("2001:db8::42/128"),"cross-group Global endpoint missing TUN exclusion");check(!settings.enable_tun_routing,"test enabled domain-derived TUN routing");
}
inline void run(){int failures=0;auto one=[&](const char* name,auto test){try{test();std::cout<<"PASS review regression "<<name<<std::endl;}catch(const std::exception& e){++failures;std::cerr<<"FAIL review regression "<<name<<": "<<e.what()<<std::endl;}};one("cross-group TUN exclusions",crossGroup);one("stale destination key",staleDestination);one("structured clone edit",[]{editedClone(false);});one("raw clone edit",[]{editedClone(true);});if(failures)throw std::runtime_error("bounded integration review regressions failed");}
}
