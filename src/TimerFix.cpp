#include "TimerFix.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/io/Logger.h"
#include "mc/util/Timer.h"
#include <cmath>   // for std::isfinite

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
    else return v;
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
    if (!this->mGetTimeMSCallback) {
        // 回调为空，无法获取时间，回退到原版实现
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
            passedMs        = 1;   // 避免除零，同时保留真实流逝时间（尽管系统回调停滞）
        }
        double adjustTimeT    = static_cast<double>(passedMs) / static_cast<double>(passedMsSysTime);
        this->mAdjustTime    += static_cast<float>((adjustTimeT - this->mAdjustTime) * 0.2);

        // 确保 mAdjustTime 是有效的浮点数，否则重置为 1.0
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

    // 关键修改：直接使用原版的 mLastTimeSeconds 作为上次时间基准
    double passedSeconds = (nowMs * 0.001 - this->mLastTimeSeconds) * this->mAdjustTime;
    this->mLastTimeSeconds = static_cast<float>(nowMs * 0.001);

    // 确保 passedSeconds 是有效的非负浮点数
    if (!std::isfinite(passedSeconds) || passedSeconds < 0.0) {
        passedSeconds = 0.0;
    }

    // ========== 修复溢出时间永久丢失问题 ==========
    // 将之前累积的溢出时间（以 tick 为单位）转换回秒，合并到本次时间步中
    if (this->mOverflowTime != 0.0f) {
        // 转换因子：tick → 秒
        float factor = this->mTimeScale * this->mTicksPerSecond;
        // 只有因子为正时才进行合并，避免除零或无效值
        if (factor > 0.0f) {
            float overflowSeconds = this->mOverflowTime / factor;
            passedSeconds += overflowSeconds;
            this->mOverflowTime = 0.0f; // 已合并，先清零
        } else {
            // 因子无效时记录警告，跳过合并
            timer_fix::TimerFix::getInstance().getSelf().getLogger().warn(
                "Invalid time factor (mTimeScale * mTicksPerSecond = {}), skipping overflow merge", factor
            );
        }
    }

    // 限制单次时间步不超过 0.1 秒，超出部分重新存入溢出
    if (passedSeconds > 0.1) {
        // 计算新的溢出（以 tick 为单位）
        float overflowTick = static_cast<float>((passedSeconds - 0.1) * this->mTimeScale * this->mTicksPerSecond);
        // 确保 overflowTick 是有限的，否则忽略
        if (std::isfinite(overflowTick)) {
            this->mOverflowTime += overflowTick;
        } else {
            timer_fix::TimerFix::getInstance().getSelf().getLogger().warn(
                "Overflow tick calculation resulted in invalid value, ignoring"
            );
        }
        passedSeconds = 0.1;
    }

    // 确保 passedSeconds 非负（再次）
    if (passedSeconds < 0.0) passedSeconds = 0.0;

    // 更新 tick 计数与插值因子
    this->mLastTimestep  = static_cast<float>(passedSeconds);
    this->mPassedTime   += static_cast<float>(passedSeconds * this->mTimeScale * this->mTicksPerSecond);

    // 检查 mPassedTime 是否有效，否则重置
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
