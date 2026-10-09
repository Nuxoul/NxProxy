#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace Configs_sys {
    // Monotonic milliseconds supplied by the caller; no Qt/event loop dependency.
    // Five retries per failure streak: 200/400/800/1600/3200 ms (5000 ms cap).
    // Only 30 s of continuous *successfully loaded configuration* resets a streak.
    // Time in backoff, IPC readiness, duplicate readiness and idle uptime do not.
    // A sixth exit exhausts the budget until a new intent or stable configured run.
    // FailedToStart and explicit configuration rejection are permanent for this intent;
    // every other unexpected exit (including code 0) uses the same finite budget.
    class CoreRestartPolicy {
    public:
        static constexpr int BaseDelayMs = 200;
        static constexpr int MaxDelayMs = 5000;
        static constexpr int RetryBudget = 5;
        static constexpr std::int64_t StableWindowMs = 30000;
        enum class Failure { UnexpectedExit, LaunchFailed, ConfigurationRejected };
        enum class Action { Retry, AlreadyPending, Exhausted, Disabled };
        struct Decision { Action action; int delayMs = 0; int attempt = 0; };

        static constexpr int DelayForAttempt(int attempt) {
            int delay = BaseDelayMs;
            for (int i = 1; i < attempt && delay < MaxDelayMs; ++i)
                delay = std::min(delay * 2, MaxDelayMs);
            return delay;
        }

        void Reset() { *this = CoreRestartPolicy(); }

        void NoteRunning(std::int64_t now) {
            if (!blocked_ && !pending_ && runningSince_ < 0) runningSince_ = now;
        }

        Decision OnExit(std::int64_t now, Failure failure = Failure::UnexpectedExit) {
            if (failure != Failure::UnexpectedExit) {
                pending_ = false;
                blocked_ = true;
                runningSince_ = -1;
                return {Action::Disabled, 0, attempts_};
            }
            if (blocked_) return {Action::Disabled, 0, attempts_};
            if (pending_) return {Action::AlreadyPending, 0, attempts_};
            if (runningSince_ >= 0 && now >= runningSince_ && now - runningSince_ >= StableWindowMs)
                attempts_ = 0;
            runningSince_ = -1;
            if (attempts_ >= RetryBudget) return {Action::Exhausted, 0, attempts_};
            const int delay = DelayForAttempt(++attempts_);
            dueAt_ = now + delay;
            pending_ = true;
            return {Action::Retry, delay, attempts_};
        }

        bool Consume(std::int64_t now) {
            if (!pending_ || now < dueAt_) return false;
            pending_ = false;
            return true;
        }
        bool Pending() const { return pending_; }
        int Attempts() const { return attempts_; }

    private:
        int attempts_ = 0;
        bool pending_ = false;
        bool blocked_ = false;
        std::int64_t runningSince_ = -1;
        std::int64_t dueAt_ = 0;
    };

    // Atomically publishes generation + expected profile together. Cancellation cannot
    // tear the pair or let a callback for an old profile become a new intent (ABA).
    // Low word: 0 = cancelled, 1 = idle core, profile + 2 = desired running profile.
    class CoreRestartIntent {
    public:
        using Token = std::uint64_t;
        Token Snapshot() const { return state_.load(); }
        bool IsCurrent(Token token) const { return Snapshot() == token && Enabled(token); }
        static bool Enabled(Token token) { return static_cast<std::uint32_t>(token) != 0; }
        static int Profile(Token token) {
            const auto value = static_cast<std::uint32_t>(token);
            return value < 2 ? -1 : static_cast<int>(value - 2);
        }
        Token Expect(int profile) {
            const auto value = profile < 0 ? 1u : static_cast<std::uint32_t>(profile) + 2u;
            auto old = Snapshot();
            for (;;) {
                if (static_cast<std::uint32_t>(old) == value) return old;
                const auto next = Next(old, value);
                if (state_.compare_exchange_weak(old, next)) return next;
            }
        }
        // For recovery code that captured intent before cleanup: never replace a newer
        // user intent/cancellation with that old snapshot, even during publication.
        Token ExpectIfCurrent(Token expected, int profile) {
            const auto value = profile < 0 ? 1u : static_cast<std::uint32_t>(profile) + 2u;
            if (static_cast<std::uint32_t>(expected) == value) return expected;
            const auto next = Next(expected, value);
            auto old = expected;
            return state_.compare_exchange_strong(old, next) ? next : expected;
        }
        bool CancelIfCurrent(Token expected) {
            const auto next = Next(expected, 0);
            return state_.compare_exchange_strong(expected, next);
        }
        Token Cancel() {
            auto old = Snapshot();
            for (;;) {
                const auto next = Next(old, 0);
                if (state_.compare_exchange_weak(old, next)) return next;
            }
        }
    private:
        static Token Next(Token old, std::uint32_t value) {
            return ((old + (Token{1} << 32)) & 0xffffffff00000000ULL) | value;
        }
        std::atomic<Token> state_{1};
    };
}