#pragma once

#include "ll/api/mod/NativeMod.h"
#include <ll/api/Config.h>

namespace timer_fix {

struct Config {
    int version = 1;
    bool debug = false;
};

Config& getConfig();
bool loadConfig();
bool saveConfig();

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
