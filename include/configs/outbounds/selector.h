#pragma once

#include "include/configs/common/Outbound.h"
#include <QJsonArray>

namespace Configs {
    // A named manual proxy group. Members are profile IDs in the owning group;
    // the routing generator resolves them to sing-box outbound tags.
    class selectorOutbound : public outbound {
    public:
        QList<int> members;
        int selectedID = -1;
        bool managedBySubscription = false;
        QString remoteGroup;

        QString DisplayType() override { return QObject::tr("Selector"); }
        QString DisplayAddress() override { return QObject::tr("%1 member(s)").arg(members.size()); }
        SecurityInfo GetSecurity() override { return {}; }

        bool ParseFromJson(const QJsonObject& object) override {
            if (object.isEmpty()) return false;
            if (object.contains("name")) name = object.value("name").toString();
            if (object.contains("members")) {
                members.clear();
                for (const auto& value : object.value("members").toArray()) {
                    const int id = value.toInt(-1);
                    if (id >= 0 && !members.contains(id)) members.append(id);
                }
            }
            if (object.contains("selected_id")) selectedID = object.value("selected_id").toInt(-1);
            managedBySubscription = object.value("managed_by_subscription").toBool(false);
            remoteGroup = object.value("remote_group").toString();
            if (!members.contains(selectedID)) selectedID = members.isEmpty() ? -1 : members.first();
            return true;
        }

        QJsonObject ExportToJson() override {
            QJsonArray values;
            for (const int id : members) values.append(id);
            return QJsonObject{
                {"name", name}, {"type", "selector"}, {"members", values}, {"selected_id", selectedID},
                {"managed_by_subscription", managedBySubscription}, {"remote_group", remoteGroup}
            };
        }

        BuildResult Build() override {
            return {{}, "Selector groups are assembled by the routing generator"};
        }
    };
} // namespace Configs
