#pragma once
#include "include/stats/connections/ConnectionSnapshot.hpp"
#include "include/stats/connections/ConnectionAccounting.hpp"
#include "include/ui/utils/ConnectionsTreeModel.h"
#include <QElapsedTimer>
#include <QPersistentModelIndex>
#include <stdexcept>
#include <iostream>
inline void runConnectionRegressions() {
    auto check=[](bool ok,const char* text){if(!ok)throw std::runtime_error(text);};
    Stats::ConnectionSnapshot snapshot;
    libcore::ConnectionMetaData raw; raw.id="one";raw.created_at=1;raw.upload=10;raw.download=20;raw.closed_at=0;
    libcore::QueryConnectionsDeltaResponse full;full.full=true;full.epoch=1;full.cursor=1;full.added={raw};
    check(snapshot.apply(full)&&snapshot.snapshot().active.size()==1,"client full reconstruction");
    libcore::QueryConnectionsDeltaResponse delta;delta.full=false;delta.epoch=1;delta.cursor=2;
    libcore::ConnectionCounterDelta value;value.id="one";value.upload=15;value.download=40;value.closed_at=99;delta.changed={value};
    check(snapshot.apply(delta)&&snapshot.snapshot().closed.size()==1&&snapshot.snapshot().closed[0].download.value()==40,"client final closed counters");
    check(!snapshot.apply(delta),"client accepted duplicate cursor");delta.epoch=2;delta.cursor=3;
    check(!snapshot.apply(delta),"client accepted wrong epoch");
    delta.epoch=1;value.id="missing";delta.changed={value};check(!snapshot.apply(delta),"client accepted missing metadata");
    delta.changed.clear();delta.removed={"one"};check(snapshot.apply(delta)&&snapshot.snapshot().closed.empty(),"client ring removal");
    full.epoch=2;full.cursor=10;check(snapshot.apply(full),"client full resync after restart");

    Stats::ConnectionAccounting accounting;
    qint64 totalUp=0,totalDown=0;
    auto credit=[&](const auto&,qint64 up,qint64 down){totalUp+=up;totalDown+=down;};
    libcore::QueryConnectionsResp sample;raw.id="transition";raw.upload=10;raw.download=20;raw.closed_at=0;sample.active={raw};
    auto closedRaw=raw;closedRaw.upload=15;closedRaw.download=40;closedRaw.closed_at=99;sample.closed={closedRaw};
    accounting.consume(sample,credit);check(totalUp==10&&totalDown==20,"active/closed overlap double credit");
    sample.active.clear();accounting.consume(sample,credit);check(totalUp==15&&totalDown==40,"transition final tail lost");
    accounting.consume(sample,credit);check(totalUp==15&&totalDown==40,"closed ring repeated credit");
    raw.id="short";raw.upload=3;raw.download=7;raw.closed_at=100;sample.closed.push_back(raw);
    accounting.consume(sample,credit);check(totalUp==18&&totalDown==47,"short-lived connection lost");

    Stats::SetOutboundDisplayNames({{"shared","Node"},{"selector-31","AI"},{"selector-32","Google"}});
    Stats::ConnectionMetadata a{};a.id="a";a.process="test.exe";a.dest="example.com:443";a.network="tcp";a.outbound="shared";a.chain={"shared","selector-31"};a.upload=10;a.download=20;a.sourceDisplay="Local";
    a.outboundDisplay=Stats::ConnectionOutboundDisplayName(a.outbound,a.chain);
    auto b=a;b.id="b";b.chain={"shared","selector-32"};b.outboundDisplay=Stats::ConnectionOutboundDisplayName(b.outbound,b.chain);
    check(a.outboundDisplay==QStringLiteral("AI → Node")&&b.outboundDisplay==QStringLiteral("Google → Node"),"actual chain strategy label");
    ConnectionsTreeModel model;int layouts=0,changes=0;
    QObject::connect(&model,&QAbstractItemModel::layoutChanged,[&]{++layouts;});
    QObject::connect(&model,&QAbstractItemModel::dataChanged,[&]{++changes;});
    model.setConnections({a,b},Stats::Default,false);
    auto process=model.index(0,0);check(model.rowCount(process)==2,"shared node merged different strategy groups");
    QPersistentModelIndex index=model.index(0,0,process);const auto* identity=index.internalPointer();
    const auto oldIDs=model.connectionIdsAt(index);a.upload=50;b.download=80;
    layouts=changes=0;model.setConnections({a,b},Stats::Default,false);
    check(layouts==0&&changes>0&&index.isValid()&&index.internalPointer()==identity&&model.connectionIdsAt(index)==oldIDs,"numeric update lost persistent row/close IDs");
    model.setConnections({b},Stats::Default,false);check(!index.isValid(),"removed connection retained close target");
    Stats::SetOutboundDisplayNames({});
    for(int count:{100,1000,5000}) {
        QList<Stats::ConnectionMetadata> rows;rows.reserve(count);
        for(int i=0;i<count;++i){auto c=a;c.id=QString::number(i);c.dest=QString::number(i)+".example:443";c.process=QString("process-%1").arg(i/100);rows.append(c);}
        for(int repeat=0;repeat<3;++repeat){
            ConnectionsTreeModel measured;QElapsedTimer timer;timer.start();
            for(int i=0;i<30;++i){measured.setConnections(rows,Stats::ByTraffic,false);measured.setConnections(rows,Stats::Default,false);}
            const auto rebuildNs=timer.nsecsElapsed();timer.restart();
            for(int i=0;i<60;++i)measured.setConnections(rows,Stats::Default,false);
            const auto numericNs=timer.nsecsElapsed();
            std::cout<<"MEASURE O5 UI rows="<<count<<" repeat="<<repeat+1<<" rebuild60_us="<<rebuildNs/1000<<" incremental60_us="<<numericNs/1000<<"\n";
        }
    }
    std::cout<<"PASS O5 client cursor/epoch/reconstruction and actual Qt chain grouping/persistent IDs\n";
}
