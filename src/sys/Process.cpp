#include "include/sys/Process.hpp"
#include "include/global/Configs.hpp"
#include "include/global/Logger.hpp"

#include <QTimer>
#include <QDir>
#include <QApplication>
#include <QThread>
#include <QDebug>



#include "include/ui/mainwindow.h"

namespace {
    // Must match egressForwardingMarker in core/internal/rpc/forwarding_windows.go.
    constexpr char kEgressForwardingMarker[] = "IPv4 forwarding is enabled on the Tun egress adapter: ";

    void warnEgressForwarding(const QByteArray &log) {
        const auto at = log.indexOf(kEgressForwardingMarker);
        if (at < 0) return;
        const auto from = at + qsizetype(sizeof(kEgressForwardingMarker) - 1);
        const auto end = log.indexOf('\n', from);
        const auto adapter = QString::fromUtf8(log.mid(from, end < 0 ? -1 : end - from)).trimmed();
        PostPassiveWarning(QObject::tr("IPv4 forwarding breaks Tun mode"),
                           QObject::tr("IPv4 forwarding is on for the network adapter \"%1\", usually because Mobile Hotspot or Internet Connection Sharing is sharing it.\n\n"
                                       "Windows then ignores the adapter binding that keeps Throne's own connections out of the Tun, so they loop back into it and fail.\n\n"
                                       "To fix this, share the hotspot from the throne-tun adapter instead of \"%1\" (Settings > Mobile hotspot > Share my internet connection from), or turn the hotspot off while using Tun mode.")
                               .arg(adapter));
    }
}

namespace Configs_sys {
    CoreProcess::~CoreProcess() {
        Kill();
    }

    void CoreProcess::SyncRestartPolicy(RestartToken token) {
        if (m_policyGeneration == token) return;
        m_restartTimer->stop();
        m_restartPolicy.Reset();
        m_policyGeneration = token;
    }

    void CoreProcess::CancelAutomaticRestart() {
        const auto token = m_restartIntent.Cancel();
        start_profile_when_core_is_up.store(-1);
        // Invalidation above is immediate even while DS_cores is blocked in cleanup.
        auto cancel = [this, token] {
            if (m_restartIntent.Snapshot() == token) SyncRestartPolicy(token);
        };
        if (QThread::currentThread() == thread()) cancel();
        else QMetaObject::invokeMethod(this, cancel, Qt::QueuedConnection);
    }

    CoreProcess::RestartToken CoreProcess::NoteProfileExpected(int profileId) {
        const auto token = m_restartIntent.Expect(profileId);
        auto sync = [this, token] {
            if (m_restartIntent.IsCurrent(token)) SyncRestartPolicy(token);
        };
        if (QThread::currentThread() == thread()) sync();
        else QMetaObject::invokeMethod(this, sync, Qt::QueuedConnection);
        return token;
    }

    void CoreProcess::NoteProfileRunning(int profileId, RestartToken token) {
        auto ready = [this, profileId, token] {
            if (!m_restartIntent.IsCurrent(token) || CoreRestartIntent::Profile(token) != profileId
                || state() != Running || Configs::dataManager->settingsRepo->prepare_exit) return;
            SyncRestartPolicy(token);
            m_restartPolicy.NoteRunning(m_restartClock.elapsed());
        };
        if (QThread::currentThread() == thread()) ready();
        else QMetaObject::invokeMethod(this, ready, Qt::QueuedConnection);
    }

    void CoreProcess::NoteConfigurationRejected(int profileId, RestartToken token) {
        if (CoreRestartIntent::Profile(token) != profileId || !m_restartIntent.CancelIfCurrent(token)) return;
        start_profile_when_core_is_up.store(-1);
        const auto cancelled = m_restartIntent.Snapshot();
        auto sync = [this, cancelled] {
            if (m_restartIntent.Snapshot() == cancelled) SyncRestartPolicy(cancelled);
        };
        if (QThread::currentThread() == thread()) sync();
        else QMetaObject::invokeMethod(this, sync, Qt::QueuedConnection);
    }

    CoreProcess::RestartToken CoreProcess::SetProfileToRestore(int profileId) {
        const auto token = NoteProfileExpected(profileId);
        m_restoreGeneration.store(token);
        start_profile_when_core_is_up.store(profileId);
        return token;
    }

    int CoreProcess::ConsumeProfileToRestore() {
        const auto token = m_restoreGeneration.exchange(0);
        start_profile_when_core_is_up.store(-1);
        return m_restartIntent.IsCurrent(token) ? CoreRestartIntent::Profile(token) : -1;
    }

    bool CoreProcess::StopProcess(int timeoutMs) {
        m_suppressExit = true;
        if (state() == NotRunning) return true;
        kill();
        if (waitForFinished(timeoutMs) || state() == NotRunning) return true;
        LOG_ERROR("Core did not exit within the bounded kill wait; refusing a duplicate start.");
        return false;
    }

    void CoreProcess::Kill() {
        CancelAutomaticRestart();
        StopProcess(1000);
    }

    bool CoreProcess::ResetUnconfirmedStart(LaunchToken token) {
        if (token == 0 || m_launchGeneration.load() != token) return false;
        const auto cancelled = m_restartIntent.Cancel();
        start_profile_when_core_is_up.store(-1);
        m_restoreGeneration.store(0);
        SyncRestartPolicy(cancelled);
        if (!StopProcess(1000)) return false;
        // A newer stop/exit during process teardown wins; never revive its profile.
        if (m_launchGeneration.load() != token || m_restartIntent.Snapshot() != cancelled ||
            Configs::dataManager->settingsRepo->prepare_exit) return true;
        Start(); // Idle core only. The unconfirmed configuration is not restored.
        return true;
    }

    CoreProcess::CoreProcess(const QString &core_path, const QString &socketName, bool debugMode)
        : m_socketName(socketName), m_debugMode(debugMode) {
        program = core_path;
        m_restartClock.start();
        m_restartTimer = new QTimer(this);
        m_restartTimer->setSingleShot(true);
        m_restartTimer->setTimerType(Qt::PreciseTimer);
        connect(m_restartTimer, &QTimer::timeout, this, [this] {
            const auto token = m_timerGeneration;
            if (!m_restartIntent.IsCurrent(token) || Configs::dataManager->settingsRepo->prepare_exit) return;
            const int profile = CoreRestartIntent::Profile(token);
            const int current = Configs::dataManager->settingsRepo->started_id;
            if (profile >= 0 && current >= 0 && current != profile) return;
            if (state() != NotRunning || !m_restartPolicy.Consume(m_restartClock.elapsed())) return;
            // Automatic attempts must not call public Restart(): that resets the budget.
            m_restoreGeneration.store(token);
            start_profile_when_core_is_up.store(profile);
            if (m_restartIntent.IsCurrent(token)) Start();
        });

        connect(this, &QProcess::readyReadStandardOutput, this, [this]() {
            auto log = readAllStandardOutput();
            if (log.contains("Extra process exited unexpectedly"))
            {
                MW_show_log("Extra Core exited, stopping profile...");
                MW_dialog_message(MwMessage::CoreCrashed, {});
            }
            warnEgressForwarding(log);
            if (logCounter.fetchAndAddRelaxed(log.count("\n")) > Configs::dataManager->settingsRepo->max_log_line) return;
            MW_show_log(log);
        });
        connect(this, &QProcess::readyReadStandardError, this, [this]() {
            auto log = readAllStandardError().trimmed();
            MW_show_log(log);
        });
        connect(this, &QProcess::errorOccurred, this, [this](ProcessError error) {
            const auto message = QString("core process error %1: %2").arg(static_cast<int>(error)).arg(errorString());
            Logging::Write(m_suppressExit || Configs::dataManager->settingsRepo->prepare_exit
                               ? Logging::Level::Info : Logging::Level::Error, message);
            if (error == FailedToStart) {
                HandleUnexpectedExit(CoreRestartPolicy::Failure::LaunchFailed,
                                     QString("start core error occurred: %1").arg(errorString()));
            }
        });
        connect(this, &QProcess::finished, this, [this](int exitCode, ExitStatus exitStatus) {
            const bool crashed = exitStatus == CrashExit || exitCode != 0;
            const QString reason = QString("core process exited: code=%1 status=%2%3")
                                       .arg(exitCode)
                                       .arg(exitStatus == CrashExit ? "crash" : "normal")
                                       .arg(Configs::dataManager->settingsRepo->prepare_exit ? " (during shutdown)" : "");
            Logging::Write(crashed && !m_suppressExit && !Configs::dataManager->settingsRepo->prepare_exit
                               ? Logging::Level::Error : Logging::Level::Info, reason);
            HandleUnexpectedExit(CoreRestartPolicy::Failure::UnexpectedExit, reason);
        });
        connect(this, &QProcess::stateChanged, this, [this](ProcessState state) {
            if (state == NotRunning) {
                Configs::dataManager->settingsRepo->core_running = false;
                qDebug() << "Core state changed to not running";
            }
        });
    }

    void CoreProcess::HandleUnexpectedExit(CoreRestartPolicy::Failure failure, const QString &reason) {
        if (m_suppressExit || Configs::dataManager->settingsRepo->prepare_exit) return;
        auto token = m_restartIntent.Snapshot();
        // Compatibility fallback for a running configuration whose caller has not yet
        // published intent. Capture BEFORE profile_stop; never resurrect cancelled intent.
        if (CoreRestartIntent::Enabled(token) && CoreRestartIntent::Profile(token) < 0
            && Configs::dataManager->settingsRepo->started_id >= 0)
            token = m_restartIntent.ExpectIfCurrent(token, Configs::dataManager->settingsRepo->started_id);
        SyncRestartPolicy(token);
        const auto decision = m_restartIntent.IsCurrent(token)
            ? m_restartPolicy.OnExit(m_restartClock.elapsed(), failure)
            : CoreRestartPolicy::Decision{CoreRestartPolicy::Action::Disabled};
        if (decision.action == CoreRestartPolicy::Action::AlreadyPending) return;

        MW_show_log("[Fatal] " + QObject::tr("Core exited, cleaning up...") + " " + reason);
        GetMainWindow()->profile_stop(true, true, false);
        if (!m_restartIntent.IsCurrent(token) || Configs::dataManager->settingsRepo->prepare_exit) return;
        if (decision.action != CoreRestartPolicy::Action::Retry) {
            start_profile_when_core_is_up.store(-1);
            MW_show_log("[ERROR] " + QObject::tr("Automatic core recovery stopped (retry budget exhausted or permanent startup failure). Last exit: %1").arg(reason));
            return;
        }
        m_timerGeneration = token;
        MW_show_log("[Warn] " + QObject::tr("Restarting the core in %1 ms (attempt %2/%3).")
                                  .arg(decision.delayMs).arg(decision.attempt).arg(CoreRestartPolicy::RetryBudget));
        m_restartTimer->start(decision.delayMs);
    }

    void CoreProcess::Start() {
        if (state() != NotRunning || Configs::dataManager->settingsRepo->prepare_exit) return;
        m_suppressExit = false;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("THRONE_CORE_SOCKET", m_socketName);
        // Turns an unrecovered Go panic into a real abort, so all goroutine stacks are dumped and WER captures the core too.
        env.insert("GOTRACEBACK", "crash");
        if (m_debugMode) env.insert("THRONE_CORE_DEBUG", "1");
        // Points Xray's asset loader at our writable config dir, so a geoip.dat/geosite.dat downloaded later is found with no core restart.
        env.insert("XRAY_LOCATION_ASSET", Configs::GetBasePath());
        setProcessEnvironment(env);
        m_launchGeneration.fetch_add(1);
        start(program, arguments);
    }

    void CoreProcess::Restart() {
        // Preserve only an explicit requested handoff, not a cancelled crash recovery.
        const int requested = start_profile_when_core_is_up.exchange(-1);
        const auto cancelled = m_restartIntent.Cancel();
        start_profile_when_core_is_up.store(-1);
        SyncRestartPolicy(cancelled);
        if (!StopProcess(500) || Configs::dataManager->settingsRepo->prepare_exit) return;
        // A UI stop/switch can arrive while waitForFinished pumps process events.
        const auto token = m_restartIntent.ExpectIfCurrent(cancelled, requested);
        if (!m_restartIntent.IsCurrent(token)) return;
        SyncRestartPolicy(token);
        m_restoreGeneration.store(token);
        start_profile_when_core_is_up.store(requested);
        Start();
    }

} // namespace Configs_sys