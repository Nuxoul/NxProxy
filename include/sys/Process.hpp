#pragma once

#include "CoreRestartPolicy.hpp"

#include <QElapsedTimer>
#include <QProcess>
#include <QTimer>

namespace Configs_sys {
    class CoreProcess : public QProcess
    {
    public:
        using RestartToken = CoreRestartIntent::Token;
        QString tag;
        QString program;
        QStringList arguments;

        CoreProcess(const QString &core_path, const QString &socketName, bool debugMode);
        ~CoreProcess();

        // QProcess operations run on this object's owning thread.
        void Start();
        void Kill();
        void Restart();

        // Thread-safe; call synchronously at manual stop/exit, before any early return.
        // Also call before an explicit user start (including restarting the same profile).
        void CancelAutomaticRestart();
        RestartToken NoteProfileExpected(int profileId);
        RestartToken RestartGeneration() const { return m_restartIntent.Snapshot(); }
        bool IsRestartCurrent(RestartToken token) const { return m_restartIntent.IsCurrent(token); }
        // Call only after Start RPC succeeded, with the token captured before that RPC.
        // IPC/QProcess started is NOT evidence of a stable running configuration.
        void NoteProfileRunning(int profileId, RestartToken token);
        void NoteConfigurationRejected(int profileId, RestartToken token);

        // Thread-safe IPC handoff; use instead of direct reads/writes of the legacy field.
        RestartToken SetProfileToRestore(int profileId);
        int ConsumeProfileToRestore();
        std::atomic<int> start_profile_when_core_is_up{-1};

    private:
        void SyncRestartPolicy(RestartToken token);
        void HandleUnexpectedExit(CoreRestartPolicy::Failure failure, const QString &reason);
        bool StopProcess(int timeoutMs);

        QString m_socketName;
        bool m_debugMode = false;
        bool m_suppressExit = false;
        CoreRestartIntent m_restartIntent;
        CoreRestartPolicy m_restartPolicy;
        RestartToken m_policyGeneration = 1;
        RestartToken m_timerGeneration = 0;
        std::atomic<RestartToken> m_restoreGeneration{0};
        QElapsedTimer m_restartClock;
        QTimer *m_restartTimer = nullptr;
    };

    inline QAtomicInt logCounter;
} // namespace Configs_sys