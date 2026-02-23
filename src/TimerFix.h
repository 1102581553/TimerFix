#pragma once

#include "ll/api/mod/NativeMod.h"

namespace timer_fix {

class TimerFix {
public:
    static TimerFix& getInstance();

    TimerFix() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

private:
    ll::mod::NativeMod& mSelf;
};

} // namespace timer_fix
