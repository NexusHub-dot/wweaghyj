#pragma once
namespace pulse_lifecycle {
inline unsigned pauseDepth=0;
struct PauseScope {
    PauseScope(){++pauseDepth;}
    ~PauseScope(){--pauseDepth;}
    PauseScope(PauseScope const&)=delete;
    PauseScope& operator=(PauseScope const&)=delete;
};
inline bool preserveOnExit(bool activeLayer,bool paused) {
    return activeLayer && (paused || pauseDepth!=0);
}
}
