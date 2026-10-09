#pragma once
#include "include/global/Configs.hpp"
#include <QByteArray>
#include <QStringList>
#include <functional>
#include <vector>

namespace RestartTest {
    inline std::vector<QString> logs;
    inline int cleanupCalls = 0;
    inline bool manualCleanup = false;
    inline std::function<void()> duringCleanup;
}
inline void MW_show_log(const QString &log) { RestartTest::logs.push_back(log); }
inline void MW_show_log(const QByteArray &log) { MW_show_log(QString::fromUtf8(log)); }
inline void MW_show_log(const char *log) { MW_show_log(QString::fromUtf8(log)); }
enum class MwMessage { CoreCrashed };
inline void MW_dialog_message(MwMessage, const QStringList &) {}
inline void PostPassiveWarning(const QString &, const QString &) {}
class MainWindow {
public:
    void profile_stop(bool crash, bool block, bool manual = true) {
        ++RestartTest::cleanupCalls;
        RestartTest::manualCleanup = RestartTest::manualCleanup || manual || !crash || !block;
        if (manual) Configs::restartTestSettings.started_id = -1;
        if (RestartTest::duringCleanup) RestartTest::duringCleanup();
    }
};
inline MainWindow *GetMainWindow() { static MainWindow window; return &window; }