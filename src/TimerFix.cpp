#include "TimerFix.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/io/Logger.h"
#include "ll/api/io/LoggerRegistry.h"
#include "mc/util/Timer.h"
#include <cmath>

namespace timer_fix {

static ll::io::Logger& getLogger() {
    static auto instance = ll::io::LoggerRegistry::getInstance()
                              .getOrCreate("TimerFix");
    return *instance;
}

TimerFix& TimerFix::getInstance() {
    static TimerFix instance;
    return instance;
}

bool TimerFix::load() {
    getLogger().info("TimerFix 已加载");
    return true;
}

bool TimerFix::enable() {
    getLogger().info("TimerFix 已启用");
    return true;
}

bool TimerFix::disable() {
    getLogger().info("TimerFix 已禁用");
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
    // 步进模式处理（与原版一致）
    if (this->mSteppingTick >= 0) {
        if (this->mSteppingTick) {
            this->mTicks = 1;
            --this->mSteppingTick;
        } else {
            this->mTicks = 0;
            this->mAlpha = 0.0f;
        }
        return;
    }

    // 检查回调是否有效（std::function<int()> 的 bool 转换）
    if (!this->mGetTimeMSCallback) {
        static bool callbackWarnedOnce = false;
        if (!callbackWarnedOnce) {
            timer_fix::getLogger().error(
                "Timer::mGetTimeMSCallback is null, falling back to original advanceTime"
            );
            callbackWarnedOnce = true;
        }
        origin(preferredFrameStep);
        return;
    }

    // 获取当前系统毫秒数（回调返回 int，成员也是 int）
    int nowMs    = this->mGetTimeMSCallback();
    int passedMs = nowMs - this->mLastMs;

    // 长时间间隔（>1秒）处理：更新调整因子 mAdjustTime
    if (passedMs > 1000) {
        int passedMsSysTime = nowMs - this->mLastMsSysTime;
        if (passedMsSysTime <= 0) {
            passedMsSysTime = 1;
            passedMs        = 1;
        }
        double adjustTimeT = static_cast<double>(passedMs) / static_cast<double>(passedMsSysTime);
        this->mAdjustTime += static_cast<float>((adjustTimeT - this->mAdjustTime) * 0.2);

        if (!std::isfinite(this->mAdjustTime)) {
            this->mAdjustTime = 1.0f;
        }

        this->mLastMs        = nowMs;
        this->mLastMsSysTime = nowMs;
    }

    // 时间回退处理
    if (passedMs < 0) {
        this->mLastMs        = nowMs;
        this->mLastMsSysTime = nowMs;
    }

    // 用 double 做中间计算，减少大数相减的精度丢失
    double nowSeconds    = static_cast<double>(nowMs) * 0.001;
    double passedSeconds = (nowSeconds - static_cast<double>(this->mLastTimeSeconds))
                           * static_cast<double>(this->mAdjustTime);
    this->mLastTimeSeconds = static_cast<float>(nowSeconds);

    if (!std::isfinite(passedSeconds) || passedSeconds < 0.0) {
        passedSeconds = 0.0;
    }

    // 合并溢出时间
    if (this->mOverflowTime != 0.0f) {
        float factor = this->mTimeScale * this->mTicksPerSecond;
        if (factor > 0.0f) {
            passedSeconds += static_cast<double>(this->mOverflowTime) / static_cast<double>(factor);
            this->mOverflowTime = 0.0f;
        } else {
            static bool factorWarnedOnce = false;
            if (!factorWarnedOnce) {
                timer_fix::getLogger().warn(
                    "Invalid time factor (mTimeScale * mTicksPerSecond = {}), skipping overflow merge",
                    factor
                );
                factorWarnedOnce = true;
            }
        }
    }

    // 限制单步不超过 0.1 秒
    if (passedSeconds > 0.1) {
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
        this->mPassedTime = 0.0f;
    }

    this->mTicks       = static_cast<int>(this->mPassedTime);
    this->mPassedTime -= static_cast<float>(this->mTicks);
    if (this->mTicks > 10) this->mTicks = 10;
    this->mAlpha       = this->mPassedTime;
}

// ====================== 注册插件 ======================
LL_REGISTER_MOD(timer_fix::TimerFix, timer_fix::TimerFix::getInstance());
