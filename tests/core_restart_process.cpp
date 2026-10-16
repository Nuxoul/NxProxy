// Compile the production src/sys/Process.cpp against tiny UI/settings adapters.
// Child processes are this test executable itself, never the installed/live core.
#include "include/sys/Process.hpp"
#include "include/global/Configs.hpp"
#include "include/global/Logger.hpp"
#include "include/ui/mainwindow.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace Logging {
    void Write(Level, const QString &message, const char *, int) { RestartTest::logs.push_back(message); }
}
using namespace Configs_sys;
static int checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::cerr << __LINE__ << ": " #expr "\n"; std::exit(1); } } while (false)

static bool pumpUntil(const std::function<bool()> &done, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    QCoreApplication::processEvents();
    return done();
}
static void pump(int durationMs) { pumpUntil([] { return false; }, durationMs); }
static void reset() {
    Configs::restartTestSettings = {};
    RestartTest::cleanupCalls = 0;
    RestartTest::manualCleanup = false;
    RestartTest::duringCleanup = {};
    RestartTest::logs.clear();
}
static void childMode(CoreProcess &process, const QString &mode) {
    process.arguments = {"--child", mode};
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--child")) {
        if (app.arguments().last() == "hold") QThread::sleep(60);
        else if (app.arguments().last() == "normal") return 0;
        return 23;
    }
    const auto executable = QCoreApplication::applicationFilePath();
    reset();
    {
        CoreProcess process(executable, "isolated-restart-test", false);
        childMode(process, "crash");
        Configs::restartTestSettings.started_id = 7;
        process.NoteProfileExpected(7);
        int launches = 0;
        QObject::connect(&process, &QProcess::started, &process, [&] { ++launches; });
        QElapsedTimer elapsed;
        elapsed.start();
        process.Start();
        CHECK(pumpUntil([&] { return RestartTest::cleanupCalls == 6; }, 12000));
        CHECK(launches == 6); // initial + exactly five retries
        CHECK(elapsed.elapsed() >= 6200);
        CHECK(Configs::restartTestSettings.started_id == 7);
        CHECK(!RestartTest::manualCleanup);
        CHECK(process.state() == QProcess::NotRunning && process.processId() == 0);
        pump(400);
        CHECK(launches == 6);
        for (const auto delay : {200, 400, 800, 1600, 3200}) {
            bool found = false;
            for (const auto &line : RestartTest::logs)
                found = found || line.contains(QString("in %1 ms").arg(delay));
            CHECK(found);
        }
        std::cout << "real child streak: 6 launches, 5 retries, elapsed=" << elapsed.elapsed() << " ms\n";
    }

    reset();
    {
        CoreProcess process(executable, "isolated-restart-test", false);
        childMode(process, "crash");
        int launches = 0;
        QObject::connect(&process, &QProcess::started, &process, [&] { ++launches; });
        process.NoteProfileExpected(8);
        process.Start();
        CHECK(pumpUntil([&] { return RestartTest::cleanupCalls == 1; }, 2000));
        std::thread cancel([&] { process.CancelAutomaticRestart(); });
        cancel.join();
        pump(350);
        CHECK(launches == 1);
        CHECK(process.ConsumeProfileToRestore() == -1);
        CHECK(process.processId() == 0);
    }

    reset();
    {
        CoreProcess process(executable, "isolated-restart-test", false);
        childMode(process, "crash");
        int launches = 0;
        QObject::connect(&process, &QProcess::started, &process, [&] { ++launches; });
        process.NoteProfileExpected(8);
        RestartTest::duringCleanup = [&] { process.CancelAutomaticRestart(); };
        process.Start();
        CHECK(pumpUntil([&] { return RestartTest::cleanupCalls == 1; }, 2000));
        pump(350);
        CHECK(launches == 1); // cancel while crash cleanup was in progress
    }

    reset();
    {
        CoreProcess process(executable, "isolated-restart-test", false);
        childMode(process, "crash");
        int launches = 0;
        QObject::connect(&process, &QProcess::started, &process, [&] { ++launches; });
        process.NoteProfileExpected(9);
        process.Start();
        CHECK(pumpUntil([&] { return RestartTest::cleanupCalls == 1; }, 2000));
        const auto oldToken = process.RestartGeneration();
        const auto newToken = process.NoteProfileExpected(10);
        process.NoteConfigurationRejected(9, oldToken); // stale RPC failure cannot stop new intent
        CHECK(process.IsRestartCurrent(newToken));
        pump(350);
        CHECK(launches == 1); // switching desired profile invalidates the old timer
        const auto token = process.SetProfileToRestore(10);
        CHECK(process.RestartGeneration() == token);
        CHECK(process.ConsumeProfileToRestore() == 10);
        CHECK(process.ConsumeProfileToRestore() == -1);
        process.SetProfileToRestore(10);
        process.CancelAutomaticRestart();
        CHECK(process.ConsumeProfileToRestore() == -1);
    }

    reset();
    {
        CoreProcess process(executable, "isolated-restart-test", false);
        childMode(process, "crash");
        int launches = 0;
        QObject::connect(&process, &QProcess::started, &process, [&] { ++launches; });
        const auto token = process.NoteProfileExpected(9);
        process.Start();
        CHECK(pumpUntil([&] { return RestartTest::cleanupCalls == 1; }, 2000));
        process.NoteConfigurationRejected(9, token);
        pump(350);
        CHECK(launches == 1);
    }

    reset();
    {
        CoreProcess process(executable, "isolated-restart-test", false);
        childMode(process, "crash");
        int launches = 0;
        QObject::connect(&process, &QProcess::started, &process, [&] { ++launches; });
        process.Start();
        CHECK(pumpUntil([&] { return RestartTest::cleanupCalls == 1; }, 2000));
        Configs::restartTestSettings.prepare_exit = true;
        pump(350);
        CHECK(launches == 1);
    }

    reset();
    {
        CoreProcess process(executable, "isolated-restart-test", false);
        childMode(process, "hold");
        int launches = 0;
        QObject::connect(&process, &QProcess::started, &process, [&] { ++launches; });
        process.Start();
        process.Start();
        CHECK(process.waitForStarted(2000));
        CHECK(launches == 1 && process.processId() > 0);
        QElapsedTimer elapsed;
        elapsed.start();
        process.Kill();
        CHECK(elapsed.elapsed() < 1500);
        CHECK(process.state() == QProcess::NotRunning && process.processId() == 0);
        process.Kill();
        pump(350);
        CHECK(launches == 1 && RestartTest::cleanupCalls == 0);
        process.Restart();
        CHECK(process.waitForStarted(2000));
        CHECK(launches == 2);
        process.Kill();
        CHECK(process.processId() == 0 && RestartTest::cleanupCalls == 0);
        // A newer UI stop arriving during an intentional restart's kill must win.
        process.Restart();
        CHECK(process.waitForStarted(2000));
        bool cancelledDuringKill = false;
        const auto connection = QObject::connect(&process, &QProcess::stateChanged, &process,
            [&](QProcess::ProcessState state) {
                if (state == QProcess::NotRunning) {
                    process.CancelAutomaticRestart();
                    cancelledDuringKill = true;
                }
            });
        process.Restart();
        CHECK(cancelledDuringKill && process.state() == QProcess::NotRunning);
        QObject::disconnect(connection);
        pump(350);
        CHECK(launches == 3 && process.processId() == 0);
    }

    reset();
    {
        CoreProcess process(executable, "isolated-restart-test", false);
        childMode(process, "crash");
        int launches = 0;
        QObject::connect(&process, &QProcess::started, &process, [&] { ++launches; });
        process.Start();
        CHECK(pumpUntil([&] { return RestartTest::cleanupCalls == 1; }, 2000));
        childMode(process, "hold");
        process.Restart();
        CHECK(process.waitForStarted(2000));
        pump(350);
        CHECK(launches == 2); // no third launch from the old crash timer
        process.Kill();
        CHECK(process.processId() == 0);
    }

    reset();
    {
        CoreProcess process(executable + ".not-present", "isolated-restart-test", false);
        process.Start();
        CHECK(pumpUntil([&] { return RestartTest::cleanupCalls == 1; }, 2000));
        pump(350);
        CHECK(RestartTest::cleanupCalls == 1 && process.processId() == 0);
    }
    reset();
    {
        CoreProcess process(executable, "isolated-restart-test", false);
        childMode(process, "normal");
        int launches = 0;
        QObject::connect(&process, &QProcess::started, &process, [&] { ++launches; });
        process.Start();
        CHECK(pumpUntil([&] { return launches == 2; }, 3000)); // unsolicited code 0 recovers too
        process.Kill();
        CHECK(process.processId() == 0);
    }
    reset();
    {
        CoreProcess process(executable, "isolated-unconfirmed-start", false);
        childMode(process, "hold");
        process.SetProfileToRestore(77);
        process.Start();
        CHECK(process.waitForStarted(2000));
        const auto oldLaunch = process.LaunchGeneration();
        CHECK(!process.ResetUnconfirmedStart(oldLaunch + 1));
        CHECK(process.state() == QProcess::Running);
        const bool resetConfirmed = process.ResetUnconfirmedStart(oldLaunch);
        if (!resetConfirmed) process.Kill(); // Assertion failure must not leave a held child.
        CHECK(resetConfirmed);
        CHECK(process.waitForStarted(2000));
        CHECK(process.LaunchGeneration() != oldLaunch);
        CHECK(process.ConsumeProfileToRestore() == -1);
        const auto newPid = process.processId();
        CHECK(!process.ResetUnconfirmedStart(oldLaunch));
        CHECK(process.processId() == newPid && process.state() == QProcess::Running);
        bool stoppedDuringReset = false;
        const auto stopConnection = QObject::connect(&process, &QProcess::stateChanged, &process,
            [&](QProcess::ProcessState state) {
                if (state == QProcess::NotRunning) {
                    process.CancelAutomaticRestart();
                    stoppedDuringReset = true;
                }
            });
        CHECK(process.ResetUnconfirmedStart(process.LaunchGeneration()));
        CHECK(stoppedDuringReset && process.state() == QProcess::NotRunning);
        QObject::disconnect(stopConnection);
        pump(200);
        CHECK(process.processId() == 0);
    }
    std::cout << "core restart production QProcess: " << checks << " checks passed, all children reaped\n";
}