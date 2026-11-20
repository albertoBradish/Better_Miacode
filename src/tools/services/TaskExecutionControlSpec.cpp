#include "common/TaskCancellation.h"
#include <QCoreApplication>
#include <QTextStream>
#include <chrono>
#include <cmath>
#include <future>
#include <thread>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

double threadCpuMs()
{
#ifdef Q_OS_WIN
    FILETIME created, exited, kernel, user;
    GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    const auto number = [](FILETIME value) { return (std::uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
    return double(number(kernel) + number(user)) / 10000.0;
#else
    timespec value{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value);
    return value.tv_sec * 1000.0 + value.tv_nsec / 1000000.0;
#endif
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using namespace miacode::task;
    using Clock = std::chrono::steady_clock;
    bool ok = true;
    const auto expect = [&](bool passed, const char* message) {
        ok &= passed; QTextStream(passed ? stdout : stderr) << (passed ? "PASS " : "FAIL ") << message << '\n';
    };
    auto token = std::make_shared<std::atomic_bool>(false);
    auto control = std::make_shared<ExecutionControl>();
    std::atomic<std::uint64_t> count{0};
    std::thread worker([&] {
        CancellationScope scope(token, control);
        std::uint64_t state = 1;
        try {
            for (;;) {
                state = state * 6364136223846793005ULL + 1;
                count.fetch_add((state >> 63) + 1, std::memory_order_relaxed);
                CancellationScope::checkpoint();
            }
        } catch (const Cancelled&) {}
    });
    const auto started = Clock::now();
    while (count.load() < 10000 && Clock::now() - started < std::chrono::seconds(1)) std::this_thread::yield();
    control->setPaused(true);
    expect(control->waitUntilPaused(std::chrono::seconds(1)), "pause reaches a real worker checkpoint");
    const auto pausedCount = count.load();
    double busy = 1; const double busyStart = threadCpuMs();
    while (threadCpuMs() - busyStart < 25) busy = std::sin(busy + 0.25);
    expect(count.load() == pausedCount && std::isfinite(busy), "real work stops while its stack is paused");
    control->setPaused(false);
    const auto resumed = Clock::now();
    while (count.load() <= pausedCount + 10000 && Clock::now() - resumed < std::chrono::seconds(1)) std::this_thread::yield();
    expect(count.load() > pausedCount + 10000, "resume continues the existing stateful loop");
    control->setPaused(true); control->waitUntilPaused(std::chrono::seconds(1));
    token->store(true); control->wake(); worker.join();
    expect(true, "cancellation wakes a paused worker");
    for (int attempt = 0; attempt < 200; ++attempt) {
        auto cancelled = std::make_shared<std::atomic_bool>(false);
        auto gate = std::make_shared<ExecutionControl>(); gate->setPaused(true);
        std::promise<void> done; auto finished = done.get_future();
        std::thread racing([&] {
            CancellationScope scope(cancelled, gate);
            try { CancellationScope::check(); } catch (const Cancelled&) {}
            done.set_value();
        });
        cancelled->store(true); gate->wake();
        if (finished.wait_for(std::chrono::seconds(2)) != std::future_status::ready) std::abort();
        racing.join();
    }
    expect(true, "200 pause/cancel entry races retain cancellation wakeups");
    auto idle = std::make_shared<ExecutionControl>(); idle->setBackground(true);
    auto idleToken = std::make_shared<std::atomic_bool>(false);
    const auto wallStart = Clock::now(); const auto cpuStart = threadCpuMs();
    {
        CancellationScope scope(idleToken, idle);
        do {
            for (int i = 0; i < 2048; ++i) { busy = std::sin(busy + 0.25); CancellationScope::checkpoint(); }
        } while (threadCpuMs() - cpuStart < 120);
    }
    const double cpu = threadCpuMs() - cpuStart;
    const double wall = std::chrono::duration<double, std::milli>(Clock::now() - wallStart).count();
    QTextStream(stdout) << "MEASURE busy_cpu_ms=" << cpu << " wall_ms=" << wall << " core_fraction=" << cpu / wall << '\n';
    expect(std::isfinite(busy) && wall > cpu && cpu / wall < 0.5,
        "background duty cycle yields CPU during actual numerical work");
    return ok ? 0 : 1;
}
