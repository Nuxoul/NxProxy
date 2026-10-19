#pragma once
#include <QMenu>
#include <QActionGroup>
#include <functional>
#include "include/configs/RoutingIntent.h"

namespace RoutingModeMenu {
inline void Populate(QMenu* menu, const Configs::RoutingIntent& saved, bool busy,
                     const QString& unsupported, const QString& targetLabel,
                     const std::function<void(Configs::RoutingMode)>& change,
                     const std::function<void()>& choose) {
    auto* modes = new QActionGroup(menu);
    modes->setExclusive(true);
    for (auto mode : {Configs::RoutingMode::Rule, Configs::RoutingMode::GlobalProxy}) {
        const bool global = mode == Configs::RoutingMode::GlobalProxy;
        auto* action = menu->addAction(global ? QObject::tr("Global Proxy") : QObject::tr("Rule"));
        action->setObjectName(global ? "routingGlobalProxy" : "routingRule");
        action->setCheckable(true);
        action->setChecked(saved.valid && saved.mode == mode);
        modes->addAction(action);
        action->setEnabled(!busy && (!global || unsupported.isEmpty()));
        action->setToolTip(busy ? QObject::tr("Wait until the current start or stop has finished.") : global ? unsupported : QString{});
        QObject::connect(action, &QAction::triggered, menu, [change, mode] { change(mode); });
    }
    auto* target = menu->addAction(QObject::tr("Global Outbound: %1").arg(targetLabel));
    target->setObjectName("routingGlobalOutbound");
    target->setEnabled(!busy);
    QObject::connect(target, &QAction::triggered, menu, choose);
    menu->addSeparator();
}
}
