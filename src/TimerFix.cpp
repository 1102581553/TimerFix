#include "TimerFix.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/io/Logger.h"
#include "ll/api/io/LoggerRegistry.h"
#include "ll/api/chrono/GameChrono.h"
#include "ll/api/coro/CoroTask.h"
#include "ll/api/thread/ServerThreadExecutor.h"
#include "mc/util/Timer.h"
#include <cmath>
#include <cstdint>
#include <filesystem>

namespace timer_fix {

static Config config;
static bool debugTaskRunning = false;

// 调试统计（主线程独占）
static size_t totalCalls = 0;
static size_t steppingCalls = 0;
static size_t fallbackCalls = 0;
static size_t timeRollbacks = 0;
static size_t longGaps = 0;
static size_t overflowEvents = 0;
static size_t nanCorrections = 0;
static size_t invalidFactorEvents = 0;

static ll::io::Logger& getLogger() {
    static auto instance = ll::io::LoggerRegistry::getInstance()
                              .getOrCreate("TimerFix");
    return *instance;
}

Config& getConfig() { return config; }

bool loadConfig() {
    auto path = TimerFix::getInstance().getSelf().getConfigDir() / "config.json";
    return ll::config::loadConfig(config, path);
}

bool saveConfig() {
    auto path = TimerFix::getInstance().getSelf().getConfigDir() / "config.json";
    return ll::config::saveConfig(config, path);
}

static void resetStats() {
    totalCalls = steppingCalls = fallbackCalls = 0;
    timeRollbacks = longGaps = overflowEvents = 0;
    nanCorrections = invalidFactorEvents = 0;
}

static void startDebugTask() {
    if (debugTaskRunning) return;
    debugTaskRunning = true;

    ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
        while (debugTaskRunning) {
            co_await std::chrono::seconds(5);
            ll::thread::ServerThreadExecutor::getDefault().execute([] {
                if (!config.debug) return;
                getLogger().info(
                    "Timer stats (5s): calls={}, stepping={}, fallback={}, "
                    "rollbacks={}, longGaps={}, overflows={}, nanFix={}, badFactor={}",
                    totalCalls, steppingCalls, fallbackCalls,
                    timeRollbacks, longGaps, overflowEvents,
                    nanCorrections, invalidFactorEvents
                );
                resetStats();
            });
        }
        debugTaskRunning = false;
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

static void stopDebugTask() {
    debugTaskRunning = false;
}

TimerFix& TimerFix::getInstance() {
    static TimerFix instance;
    return instance;
}

bool TimerFix::load() {
    std::filesystem::create_directories(getSelf().getConfigDir());
    if (!loadConfig()) {
        getLogger().warn("Failed to load config, using defaults and saving");
        saveConfig();
    }
    getLogger().info("TimerFix loaded. debug: {}", config.debug);
    return true;
}

bool TimerFix::enable() {
    if (config.debug) startDebugTask();
    getLogger().info("TimerFix enabled");
    return true;
}

bool TimerFix::disable() {
    stopDebugTask();
    resetStats();
    getLogger().info("TimerFix disabled");
    return true;
}

} // namespace timer_fix

// ====================== Timer::advanceTime 钩子 ======================
LL_AUTO_TYPE_INSTANCE_HOOK(
    TimerUpdateHook,
    ll::memory::HookPriority::Lowest,
    Timer,
    &Timer::advanceTime,
    void,
    [[maybe_unused]] float preferredFrameStep
) {
    using namespace timer_fix;

    ++totalCalls;

    // 步进模式处理
    if (this->mSteppingTick >= 0) {
        ++steppingCalls;
        if (this->mSteppingTick) {
            this->mTicks = 1;
            --this->mSteppingTick;
        } else {
            this->mTicks = 0;
            this->mAlpha = 0.0f;
        }
        return;
    }

    // mGetTimeMSCallback 是 ll::TypedStorage 包装的 std::function<int64(void)>
    // 通过 operator->() 获取底层 std::function 指针
    auto* callbackPtr = this->mGetTimeMSCallback.operator->();
    if (!callbackPtr || !(*callbackPtr)) {
        ++fallbackCalls;
        static bool warnedOnce = false;
        if (!warnedOnce) {
            getLogger().error("Timer::mGetTimeMSCallback is null, falling back to origin");
            warnedOnce = true;
        }
        origin(preferredFrameStep);
        return;
    }

    // 回调返回 int64_t，成员是 int，需要显式转换
    int64_t nowMs64  = (*callbackPtr)();
    int nowMs        = static_cast<int>(nowMs64);
    int passedMs     = nowMs - this->mLastMs;

    // 长时间间隔（>1秒）处理
    if (passedMs > 1000) {
        ++longGaps;
        int passedMsSysTime = nowMs - this->mLastMsSysTime;
        if (passedMsSysTime <= 0) {
            passedMsSysTime = 1;
            passedMs        = 1;
        }
        double adjustTimeT = static_cast<double>(passedMs) / static_cast<double>(passedMsSysTime);
        this->mAdjustTime += static_cast<float>((adjustTimeT - this->mAdjustTime) * 0.2);

        if (!std::isfinite(this->mAdjustTime)) {
            ++nanCorrections;
            this->mAdjustTime = 1.0f;
        }

        this->mLastMs        = nowMs;
        this->mLastMsSysTime = nowMs;
    }

    // 时间回退处理
    if (passedMs < 0) {
        ++timeRollbacks;
        this->mLastMs        = nowMs;
        this->mLastMsSysTime = nowMs;
    }

    // 用 double 做中间计算减少精度丢失
    double nowSeconds    = static_cast<double>(nowMs) * 0.001;
    double passedSeconds = (nowSeconds - static_cast<double>(this->mLastTimeSeconds))
                           * static_cast<double>(this->mAdjustTime);
    this->mLastTimeSeconds = static_cast<float>(nowSeconds);

    if (!std::isfinite(passedSeconds) || passedSeconds < 0.0) {
        if (!std::isfinite(passedSeconds)) ++nanCorrections;
        passedSeconds = 0.0;
    }

    // 合并溢出时间
    if (this->mOverflowTime != 0.0f) {
        float factor = this->mTimeScale * this->mTicksPerSecond;
        if (factor > 0.0f) {
            passedSeconds += static_cast<double>(this->mOverflowTime) / static_cast<double>(factor);
            this->mOverflowTime = 0.0f;
        } else {
            ++invalidFactorEvents;
            static bool factorWarnedOnce = false;
            if (!factorWarnedOnce) {
                getLogger().warn("Invalid time factor: {}", factor);
                factorWarnedOnce = true;
            }
        }
    }

    // 限制单步不超过 0.1 秒
    if (passedSeconds > 0.1) {
        ++overflowEvents;
        double overflowTick = (passedSeconds - 0.1)
                              * static_cast<double>(this->mTimeScale)
                              * static_cast<double>(this->mTicksPerSecond);
        if (std::isfinite(overflowTick)) {
            this->mOverflowTime += static_cast<float>(overflowTick);
        }
        passedSeconds = 0.1;
    }

    this->mLastTimestep = static_cast<float>(passedSeconds);
    this->mPassedTime  += static_cast<float>(passedSeconds
                          * static_cast<double>(this->mTimeScale)
                          * static_cast<double>(this->mTicksPerSecond));

    if (!std::isfinite(this->mPassedTime) || this->mPassedTime < 0.0f) {
        ++nanCorrections;
        this->mPassedTime = 0.0f;
    }

    this->mTicks       = static_cast<int>(this->mPassedTime);
    this->mPassedTime -= static_cast<float>(this->mTicks);
    if (this->mTicks > 10) this->mTicks = 10;
    this->mAlpha       = this->mPassedTime;
}

// ====================== 注册插件 ======================
LL_REGISTER_MOD(timer_fix::TimerFix, timer_fix::TimerFix::getInstance());
