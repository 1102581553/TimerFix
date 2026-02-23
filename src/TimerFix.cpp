#include "TimerFix.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/io/Logger.h"
#include "mc/util/Timer.h"
#include <cmath>

namespace timer_fix {

TimerFix& TimerFix::getInstance() {
    static TimerFix instance;
    return instance;
}

bool TimerFix::load() {
    getSelf().getLogger().debug("Loading...");
    return true;
}

bool TimerFix::enable() {
    getSelf().getLogger().debug("Enabling...");
    return true;
}

bool TimerFix::disable() {
    getSelf().getLogger().debug("Disabling...");
    return true;
}

float inlineClamp(float v, float low, float high) {
    if (v > high) return high;
    if (v <= low) return low;
    return v;
}

// ==================== Timer::advanceTime 钩子 ====================
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

    // ----- 健壮性增强：检查时间回调函数是否有效 -----
    // 使用 operator->() 获取底层指针并判空
    if (!this->mGetTimeMSCallback.operator->()) {
        timer_fix::TimerFix::getInstance().getSelf().getLogger().error(
            "Timer::mGetTimeMSCallback is null, falling back to original advanceTime"
        );
        origin(preferredFrameStep);
        return;
    }

    // 获取当前系统毫秒数
    int64 nowMs    = (*this->mGetTimeMSCallback)();
    int64 passedMs = nowMs - this->mLastMs;

    // 长时间间隔（>1秒）处理：更新调整因子 mAdjustTime
    if (passedMs > 1000) {
        int64 passedMsSysTime = nowMs - this->mLastMsSysTime;
        if (passedMsSysTime == 0) {
            passedMsSysTime = 1;
            passedMs        = 1;
        }
        double adjustTimeT    = static_cast<double>(passedMs) / static_cast<double>(passedMsSysTime);
        this->mAdjustTime    += static_cast<float>((adjustTimeT - this->mAdjustTime) * 0.2);

        if (!std::isfinite(this->mAdjustTime)) {
            this->mAdjustTime = 1.0f;
        }

        this->mLastMs         = nowMs;
        this->mLastMsSysTime  = nowMs;
    }

    // 时间回退处理
    if (passedMs < 0) {
        this->mLastMs        = nowMs;
        this->mLastMsSysTime = nowMs;
    }

    double passedSeconds = (nowMs * 0.001 - this->mLastTimeSeconds) * this->mAdjustTime;
    this->mLastTimeSeconds = static_cast<float>(nowMs * 0.001);

    if (!std::isfinite(passedSeconds) || passedSeconds < 0.0) {
        passedSeconds = 0.0;
    }

    // 合并溢出时间
    if (this->mOverflowTime != 0.0f) {
        float factor = this->mTimeScale * this->mTicksPerSecond;
        if (factor > 0.0f) {
            float overflowSeconds = this->mOverflowTime / factor;
            passedSeconds += overflowSeconds;
            this->mOverflowTime = 0.0f;
        } else {
            timer_fix::TimerFix::getInstance().getSelf().getLogger().warn(
                "Invalid time factor (mTimeScale * mTicksPerSecond = {}), skipping overflow merge", factor
            );
        }
    }

    // 限制单步不超过 0.1 秒
    if (passedSeconds > 0.1) {
        float overflowTick = static_cast<float>((passedSeconds - 0.1) * this->mTimeScale * this->mTicksPerSecond);
        if (std::isfinite(overflowTick)) {
            this->mOverflowTime += overflowTick;
        } else {
            timer_fix::TimerFix::getInstance().getSelf().getLogger().warn(
                "Overflow tick calculation resulted in invalid value, ignoring"
            );
        }
        passedSeconds = 0.1;
    }

    if (passedSeconds < 0.0) passedSeconds = 0.0;

    this->mLastTimestep  = static_cast<float>(passedSeconds);
    this->mPassedTime   += static_cast<float>(passedSeconds * this->mTimeScale * this->mTicksPerSecond);

    if (!std::isfinite(this->mPassedTime)) {
        this->mPassedTime = 0.0f;
    }

    this->mTicks         = static_cast<int>(this->mPassedTime);
    this->mPassedTime   -= static_cast<float>(this->mTicks);
    if (this->mTicks > 10) this->mTicks = 10;
    this->mAlpha = this->mPassedTime;
}

} // namespace timer_fix

LL_REGISTER_MOD(timer_fix::TimerFix, timer_fix::TimerFix::getInstance());
