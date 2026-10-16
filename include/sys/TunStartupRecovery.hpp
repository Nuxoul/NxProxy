#pragma once
#include <algorithm>
#include <cstdint>

namespace TunStartupRecovery {
struct AttemptResult {
    bool rpcOK = false;
    bool hasError = false;
    bool tunAdapterRetryable = false;
};
struct Outcome {
    int attempts = 0;
    bool cancelled = false;
    bool exhausted = false;
};
inline constexpr int MaxAttempts = 3;
inline constexpr std::int64_t TotalBudgetMs = 60000;
inline constexpr int CallTimeoutMs = 30000;

// Wait between completed failed attempts, never while an atomic Start is active.
template<class Current, class Sleep, class Now>
bool Wait(int delayMs, Current current, Sleep sleep, Now now) {
    const auto started = now();
    while (now() - started < delayMs) {
        if (!current()) return false;
        const auto left = delayMs - (now() - started);
        if (left <= 0) break;
        if (!sleep(static_cast<unsigned long>(std::min<std::int64_t>(50, left)))) return false;
    }
    return current();
}

// Boundary: one safe-completion Start RPC per attempt, caller owns the current
// launch token and final response. Wait must honor token/shutdown cancellation.
template<class Attempt, class Current, class Wait, class Now, class Retry>
Outcome Run(Attempt attempt, Current current, Wait wait, Now now, Retry retry) {
    Outcome outcome;
    const auto started = now();
    for (;;) {
        if (!current()) { outcome.cancelled = true; return outcome; }
        const auto remaining = TotalBudgetMs - std::max<std::int64_t>(0, now() - started);
        if (remaining <= 0) { outcome.exhausted = true; return outcome; }
        const auto result = attempt(static_cast<int>(std::min<std::int64_t>(CallTimeoutMs, remaining)));
        ++outcome.attempts;
        if (!current()) { outcome.cancelled = true; return outcome; }
        // A transport timeout has unknown completion: never issue a second Start.
        if (!result.rpcOK || !result.hasError || !result.tunAdapterRetryable) return outcome;
        const int delay = outcome.attempts == 1 ? 1000 : 2000;
        const auto left = TotalBudgetMs - std::max<std::int64_t>(0, now() - started);
        if (outcome.attempts >= MaxAttempts || left <= delay) {
            outcome.exhausted = true;
            return outcome;
        }
        retry(outcome.attempts + 1, delay);
        if (!current() || !wait(delay)) { outcome.cancelled = true; return outcome; }
    }
}
}
