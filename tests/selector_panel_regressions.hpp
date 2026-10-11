#pragma once
#include <QListWidget>
#include <QScrollBar>
#include <atomic>
#include <future>
#include <iostream>
#include <thread>
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/configs/outbounds/selector.h"

inline void runSelectorPanelRegressions() {
    auto check=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    auto group=Configs::GroupsRepo::NewGroup();group->name="Selector refresh concurrency";
    check(Configs::dataManager->groupsRepo->AddGroup(group),"selector fixture group");
    auto member=Configs::ProfilesRepo::NewProfile("vless");
    member->outbound->ParseFromLink("vless://11111111-2222-3333-4444-555555555555@127.0.0.1:443?encryption=none#fixture");
    check(Configs::dataManager->profilesRepo->AddProfile(member,group->id),"selector member fixture");
    QSet<int> expected;
    for(const QString& name:QStringList{"GLOBAL","Google","AI","GLM","streaming","Steam","Node.js","Roblox"}) {
        auto profile=Configs::ProfilesRepo::NewProfile("selector");profile->outbound->name=name;
        auto* selector=profile->Selector();selector->members={member->id};selector->selectedID=member->id;
        check(Configs::dataManager->profilesRepo->AddProfile(profile,group->id),"selector profile fixture");expected.insert(profile->id);
    }
    // Taking a copy must participate in the same lock as SortProfiles' commit.
    group->mutex.lock();std::promise<void> started;
    auto blocked=std::async(std::launch::async,[&]{started.set_value();return group->Profiles();});
    started.get_future().wait();const bool respectedLock=blocked.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout;
    group->mutex.unlock();blocked.get();check(respectedLock,"group ID snapshot bypassed writer lock");
    std::atomic<bool> stop=false;std::atomic<int> sorts=0;
    std::thread writer([&]{while(!stop.load()){group->SortProfiles({GroupSortMethod::ByName,(sorts.load()%2)!=0});sorts.fetch_add(1);}});
    QListWidget list;list.resize(350,400);list.show();
    bool complete=true;QSet<int> observed;
    for(int round=0;round<300;++round) {
        // Snapshot first, then release group lock before repository calls.
        const auto ids=group->Profiles();list.clear();observed.clear();
        for(int id:ids) {
            auto profile=Configs::dataManager->profilesRepo->GetProfile(id);
            if(!profile||profile->type!="selector")continue;
            observed.insert(id);auto* item=new QListWidgetItem(profile->name,&list);item->setData(Qt::UserRole,id);item->setSizeHint(QSize(300,46));
            if(profile->Selector()->selectedID!=member->id)complete=false;
        }
        if(observed!=expected||list.count()!=8)complete=false;
        list.setCurrentRow(round%8);list.scrollToItem(list.currentItem());QCoreApplication::processEvents();
        for(int row=0;row<list.count();++row)if(list.visualItemRect(list.item(row)).height()!=46)complete=false;
    }
    stop=true;writer.join();check(complete&&sorts.load()>0,"concurrent sorting dropped selector rows or changed member selection");
    std::cout<<"PASS selector refresh: lock-participating snapshot / 8 rows / 300 concurrent cycles / selected members preserved, sorts="<<sorts.load()<<"\n";
}
