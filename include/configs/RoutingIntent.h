#pragma once
#include <QtGlobal>

namespace Configs {
    enum class RoutingMode { Rule = 0, GlobalProxy = 1 };
    struct RoutingIntent {
        RoutingMode mode = RoutingMode::Rule;
        int targetID = -1;
        quint64 revision = 0;
        bool valid = true;
        bool operator==(const RoutingIntent&) const = default;
    };
}
