#pragma once

#include "include/database/entities/Profile.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace Subscription {
    struct ProxyGroup {
        QString name;
        QString type;
        QStringList proxies;
        QString selected;
    };

    struct ParseSink {
        std::function<void(std::shared_ptr<Configs::Profile>)> profile;
        std::function<void(const ProxyGroup &)> proxyGroup;
        std::function<void(const QString &)> ruleLine;
        std::function<void(const QString &)> log;
        std::function<void(const QString &, const QString &)> warn;
    };

    struct ParseResult {
        bool complete = true;
        int errorCount = 0;
        int profileCount = 0;
    };

    // Profiles are handed to the sink in document order; manual imports may keep
    // partial results, but subscription refreshes must require complete == true.
    // Explicitly empty Clash proxies sequences are valid subscriptions.
    ParseResult ParseDocument(QByteArray body, const ParseSink &sink);

    ParseResult ParseText(const QString &text, const ParseSink &sink);
}
