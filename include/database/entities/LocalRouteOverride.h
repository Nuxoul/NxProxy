#pragma once
#include "include/database/entities/RouteProfile.h"
#include <QRegularExpression>

namespace Configs {
    struct LocalRouteOverride {
        qint64 id = -1;
        QString kind;
        QString value;
        simpleAction action = proxy;
        qint64 priority = 0;

        [[nodiscard]] QString RawRule() const { return kind + ':' + value; }
        [[nodiscard]] std::shared_ptr<RouteRule> ToRule() const {
            auto rule = std::make_shared<RouteRule>();
            rule->name = QStringLiteral("local-%1").arg(id);
            rule->outboundID = action == proxy ? proxyID : action == bypass ? directID
                             : action == warpBypass ? warpBypassID : blockID;
            if (action == block) rule->action = "reject";
            if (kind == "domain") rule->domain = {value};
            else if (kind == "suffix") rule->domain_suffix = {value};
            else if (kind == "ip") rule->ip_cidr = {value};
            else if (kind == "processName" || kind == "processPath") {
#ifdef Q_OS_WIN
                const auto escaped = QRegularExpression::escape(value);
                rule->process_path_regex = {kind == "processName"
                    ? QStringLiteral("(?i)(?:^|[\\\\/])%1$").arg(escaped)
                    : QStringLiteral("(?i)^%1$").arg(escaped)};
#else
                if (kind == "processName") rule->process_name = {value};
                else rule->process_path = {value};
#endif
            }
            return rule;
        }
    };

    struct RoutingSnapshot {
        std::shared_ptr<RouteProfile> profile;
        QList<LocalRouteOverride> local;
        // Detached effective value: callers may modify it without publishing or changing base rules.
        [[nodiscard]] std::shared_ptr<RouteProfile> EffectiveProfile() const {
            if (!profile) return nullptr;
            auto effective = std::make_shared<RouteProfile>(*profile);
            QList<std::shared_ptr<RouteRule>> rules;
            for (const auto& entry : local) rules.append(entry.ToRule());
            rules.append(effective->Rules);
            effective->Rules = rules;
            return effective;
        }
    };
}
