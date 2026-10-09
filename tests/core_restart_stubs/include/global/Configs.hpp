#pragma once
#include <QString>
namespace Configs {
    struct RestartTestSettings {
        bool prepare_exit = false;
        bool core_running = false;
        int started_id = -1;
        int max_log_line = 100000;
    };
    struct RestartTestManager { RestartTestSettings *settingsRepo; };
    inline RestartTestSettings restartTestSettings;
    inline RestartTestManager restartTestManager{&restartTestSettings};
    inline RestartTestManager *dataManager = &restartTestManager;
    inline QString GetBasePath() { return QString(); }
}