#include "TimerFix.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"

#include "mc/util/Timer.h"
#include <unordered_map>

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

// 存储每个 Timer 实例上一次记录的绝对时间（秒）
std::unordered_map<Timer*, double> lastTimeSeconds_fixed;

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
        double adjustTimeT    = (double)passedMs / (double)passedMsSysTime;
        this->mAdjustTime    += (adjustTimeT - this->mAdjustTime) * 0.2;
        this->mLastMs         = nowMs;
        this->mLastMsSysTime  = nowMs;
    }

    // 时间回退处理
    if (passedMs < 0) {
        this->mLastMs        = nowMs;
        this->mLastMsSysTime = nowMs;
    }

    // 核心修改：基于独立映射计算本次应流逝的游戏时间
    double passedSeconds =
        (nowMs * 0.001 - lastTimeSeconds_fixed[this]) * this->mAdjustTime;
    this->mLastTimeSeconds = lastTimeSeconds_fixed[this] = nowMs * 0.001;

    // ========== 修复溢出时间永久丢失问题 ==========
    // 将之前累积的溢出时间（以 tick 为单位）转换回秒，合并到本次时间步中
    if (this->mOverflowTime != 0.0f) {
        // 转换因子：tick → 秒
        float factor = this->mTimeScale * this->mTicksPerSecond;
        if (factor > 0.0f) { // 防止除零
            float overflowSeconds = this->mOverflowTime / factor;
            passedSeconds += overflowSeconds;
            this->mOverflowTime = 0.0f; // 已合并，先清零
        }
    }

    // 限制单次时间步不超过 0.1 秒，超出部分重新存入溢出
    if (passedSeconds > 0.1) {
        // 计算新的溢出（以 tick 为单位）
        this->mOverflowTime += (passedSeconds - 0.1) * this->mTimeScale * this->mTicksPerSecond;
        passedSeconds = 0.1;
    }
    // 确保非负
    if (passedSeconds < 0.0) passedSeconds = 0.0;

    // 更新 tick 计数与插值因子
    this->mLastTimestep  = static_cast<float>(passedSeconds);
    this->mPassedTime   += static_cast<float>(passedSeconds * this->mTimeScale * this->mTicksPerSecond);
    this->mTicks         = static_cast<int>(this->mPassedTime);
    this->mPassedTime   -= static_cast<float>(this->mTicks);
    if (this->mTicks > 10) this->mTicks = 10;
    this->mAlpha = this->mPassedTime;
}

// ==================== Timer 析构钩子（防止内存泄漏）====================
// 使用 $dtor 占位符钩取编译器生成的默认析构函数。
// 若编译失败，请用实际修饰名替换，例如：
//   LL_SYMBOL(??1Timer@@QEAA@XZ)   // 假设无虚函数
//   LL_SYMBOL(??1Timer@@UEAA@XZ)   // 假设有虚函数
LL_AUTO_TYPE_INSTANCE_HOOK(
    TimerDestructorHook,
    ll::memory::HookPriority::Normal,
    Timer,
    &Timer::$dtor,   // LeviLamina 内部可能支持 $dtor 占位符
    void
) {
    // 在 Timer 对象销毁前，从全局映射中移除自身条目
    lastTimeSeconds_fixed.erase(this);
    // 调用原析构函数
    origin();
}

} // namespace timer_fix

LL_REGISTER_MOD(timer_fix::TimerFix, timer_fix::TimerFix::getInstance());
