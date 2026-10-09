#include "include/sys/CoreRestartPolicy.hpp"

#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using namespace Configs_sys;
static int checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::cerr << __LINE__ << ": " #expr "\n"; std::exit(1); } } while (false)

int main() {
    using Policy = CoreRestartPolicy;
    using Action = Policy::Action;
    Policy policy;
    std::int64_t now = 0;
    for (int attempt = 1; attempt <= Policy::RetryBudget; ++attempt) {
        auto decision = policy.OnExit(now);
        CHECK(decision.action == Action::Retry);
        CHECK(decision.delayMs == 200 * (1 << (attempt - 1)));
        CHECK(decision.attempt == attempt);
        CHECK(policy.OnExit(now).action == Action::AlreadyPending);
        CHECK(!policy.Consume(now + decision.delayMs - 1));
        now += decision.delayMs;
        CHECK(policy.Consume(now));
        CHECK(!policy.Consume(now));
        policy.NoteRunning(now);
        now += 1;
    }
    CHECK(policy.OnExit(now).action == Action::Exhausted);
    CHECK(!policy.Pending());
    CHECK(policy.OnExit(now + 1000000).action == Action::Exhausted); // idle is not stable
    CHECK(Policy::DelayForAttempt(6) == 5000);
    CHECK(Policy::DelayForAttempt(1000000) == 5000);

    policy.Reset();
    auto first = policy.OnExit(0);
    CHECK(policy.Consume(first.delayMs));
    policy.NoteRunning(200);
    policy.NoteRunning(29999); // duplicate ready must not restart the stable window
    CHECK(policy.OnExit(30199).attempt == 2); // 29,999 ms is not stable
    CHECK(policy.Consume(30599));
    policy.NoteRunning(31000);
    CHECK(policy.OnExit(61000).attempt == 1); // exactly 30,000 ms is stable

    policy.Reset();
    CHECK(policy.OnExit(0).attempt == 1);
    CHECK(policy.Consume(1000000));
    CHECK(policy.OnExit(2000000).attempt == 2); // no loaded config, no reset
    policy.Reset();
    policy.NoteRunning(1000);
    CHECK(policy.OnExit(500).attempt == 1); // backwards fake time cannot reset

    policy.Reset();
    CHECK(policy.OnExit(0, Policy::Failure::LaunchFailed).action == Action::Disabled);
    policy.NoteRunning(0);
    CHECK(policy.OnExit(1000000).action == Action::Disabled);
    policy.Reset();
    CHECK(policy.OnExit(0).action == Action::Retry);
    CHECK(policy.OnExit(1, Policy::Failure::ConfigurationRejected).action == Action::Disabled);
    CHECK(!policy.Consume(1000000));
    CHECK(policy.OnExit(1000000).action == Action::Disabled);
    policy.Reset();
    CHECK(policy.OnExit(0).attempt == 1);

    CoreRestartIntent intent;
    const auto a = intent.Expect(7);
    CHECK(intent.IsCurrent(a));
    CHECK(CoreRestartIntent::Profile(a) == 7);
    CHECK(intent.Expect(7) == a); // automatic recovery preserves the failure streak
    const auto b = intent.Expect(8);
    CHECK(!intent.IsCurrent(a));
    CHECK(intent.IsCurrent(b));
    intent.Cancel();
    CHECK(!intent.IsCurrent(b));
    CHECK(!CoreRestartIntent::Enabled(intent.Snapshot()));
    const auto a2 = intent.Expect(7);
    CHECK(a != a2 && !intent.IsCurrent(a));
    intent.Cancel();
    CHECK(!intent.IsCurrent(a2));
    CHECK(!intent.IsCurrent(intent.ExpectIfCurrent(a2, 7)));
    const auto cancelled = intent.Snapshot();
    const auto restored = intent.ExpectIfCurrent(cancelled, 7);
    CHECK(intent.IsCurrent(restored));
    intent.Cancel();
    CHECK(!intent.IsCurrent(intent.ExpectIfCurrent(cancelled, 7)));
    const auto idle = intent.Expect(-1);
    CHECK(intent.IsCurrent(idle) && CoreRestartIntent::Profile(idle) == -1);

    // Concurrent cancel/expect never tears profile and generation; repeated same-profile
    // cancels still invalidate every previously captured callback.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([&, t] {
        for (int i = 0; i < 10000; ++i) {
            if (t % 2) intent.Expect(t);
            else intent.Cancel();
        }
    });
    for (auto &thread : threads) thread.join();
    const auto before = intent.Expect(19);
    intent.Cancel();
    CHECK(!intent.IsCurrent(before));
    std::cout << "core restart policy: " << checks << " checks passed\n";
}