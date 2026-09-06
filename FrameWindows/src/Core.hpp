#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fwl {
constexpr int maxWindow = 20;
constexpr double tickRate = 240.0;
// Schema 2 identifies this clock/measurement format. UI-only Pulse updates
// should not invalidate otherwise matching saved references.
inline std::string compatibleEnvironment(std::string environment) {
    auto begin=environment.find(";alan.pulse_macro@");
    if(begin!=std::string::npos) {
        auto end=environment.find(';',begin+1);
        environment.erase(begin,end==std::string::npos?std::string::npos:end-begin);
    }
    return environment;
}
inline std::string pulseSlotKey(std::string const& fingerprint,int slot) {
    if(slot<1 || slot>20)throw std::runtime_error("Invalid Pulse slot");
    return fingerprint+"-pulse-"+std::to_string(slot);
}
struct Input {
    int tick = 0;
    int player = 0;
    bool down = false;
    float x = 0, y = 0;
    bool wave=false;
};
struct Pose {
    int tick = 0;
    std::array<double, 6> values{}; // x, y, y-velocity for each player
    // Added in Pulse 1.3.6. Older saved references do not have this field;
    // in that case validation falls back to the replay's current dual state.
    std::optional<bool> dual;
};
// Validation is only a pre-shift determinism guard. 0.001 GD units was
// unnecessarily strict for real hook stacks and could reject harmless floating-point
// drift. 0.1 GD units is still only 1/300 of a 30-unit block, while large/relevant
// replay divergence is still rejected.
inline bool sameValidationPose(Pose const& expected, Pose const& actual, double tolerance = 0.1) {
    // A dual-state disagreement is a real replay mismatch when the reference
    // recorded that metadata. Legacy references intentionally omit it.
    if (expected.dual && actual.dual && *expected.dual != *actual.dual) return false;

    for (int k = 0; k < 3; ++k) {
        if (!std::isfinite(expected.values[k]) || !std::isfinite(actual.values[k]) ||
            std::abs(expected.values[k] - actual.values[k]) > tolerance) return false;
    }

    // Player 2 is dormant outside dual mode and GD is free to leave its
    // coordinates/velocity in a different placeholder state after a reset.
    // Comparing those dormant values caused false "replay differs" aborts.
    // In dual mode P2 remains part of deterministic validation.
    bool checkPlayer2 = actual.dual.value_or(expected.dual.value_or(true));
    if (checkPlayer2) {
        for (int k = 3; k < 6; ++k) {
            if (!std::isfinite(expected.values[k]) || !std::isfinite(actual.values[k]) ||
                std::abs(expected.values[k] - actual.values[k]) > tolerance) return false;
        }
    }
    return true;
}
struct Window {
    int early = 0, late = 0;
    bool measured = false;
    bool overflow = false;
    int earlyFailureTick=-1,lateFailureTick=-1;
    bool unstable=false, checkpointFallback=false;
    std::string warning() const {
        if(unstable && checkpointFallback)return "WARNING: unstable double-check; checkpoint fallback used";
        if(unstable)return "WARNING: unstable double-check";
        if(checkpointFallback)return "Checkpoint fallback used";
        return {};
    }
    int width() const { return early + late + 1; }
    double nominalMilliseconds() const { return width()*1000.0/tickRate; }
};
struct Run {
    std::string fingerprint, levelName;
    std::vector<Input> inputs;
    std::vector<Pose> poses;
    std::vector<Window> windows;
    int endTick = 0;
    bool completed = false;
    bool pulseInputs = false;
    std::string macroRevision;
    uint64_t seed = 0;
    std::string environment;
    bool nextClick=false;
};
inline int trialEndpoint(Run const& run,size_t index,bool nextClick) {
    if(nextClick && index<run.inputs.size()) {
        for(size_t j=index+1;j<run.inputs.size();++j)
            if(run.inputs[j].player==run.inputs[index].player && (run.inputs[index].wave || run.inputs[j].down))
                return std::min(run.endTick,run.inputs[j].tick);
    }
    return run.endTick;
}
inline bool needsCompletion(Run const& run,int endpoint) {return run.completed && endpoint==run.endTick;}
inline bool repeatableFailure(bool passed,int firstTick,int repeatedTick) {
    return !passed && firstTick==repeatedTick;
}
struct FailureConfirmation {
    bool stable = true;
    bool acceptedPass = false;
    int acceptedFailureTick = -1;
};
// A failed boundary is replayed once using the same batched fixed-240-Hz
// engine path as ordinary trials. If the double-check disagrees, continue
// conservatively instead of aborting the full
// analysis: treat the boundary as failed using the first observed failure tick
// and flag the result as potentially inaccurate. This can only make a measured
// window narrower, never falsely widen it.
inline FailureConfirmation resolveFailureConfirmation(bool repeatedPassed,int firstTick,int repeatedTick) {
    if (repeatableFailure(repeatedPassed,firstTick,repeatedTick))
        return {true,false,repeatedTick};
    return {false,false,firstTick >= 0 ? firstTick : repeatedTick};
}
struct ReferenceCheckpoint {
    size_t inputs=0,poses=0;
    int physicsTick=0,recordedThrough=0,lastPoseTick=-1;
    std::array<bool,2> held{};
    bool invalid=false;
    std::string reason;
    std::uint64_t ordinal=0;
};
// Re-entering a level must resolve the same slot-qualified key used by Begin.
// Never fall back to a different take when a macro is selected.
template<class Loader>
std::optional<Run> loadSelectedReference(std::string const& level,int slot,
    std::string const& revision,Loader&& loader,std::string& error) {
    error.clear();
    auto run=loader(pulseSlotKey(level,slot),error);
    if(run && (revision.empty() || run->macroRevision!=revision)) {
        error="The selected macro changed since its reference. Record both with F6.";
        return {};
    }
    if(!run && revision.empty() && error.empty())return loader(level,error);
    if(!run && error.empty())error="No frame reference saved for this slot. Use F6, then F8 to save both.";
    return run;
}
inline void rewindReference(Run& run,ReferenceCheckpoint const& point) {
    if(point.inputs>run.inputs.size() || point.poses>run.poses.size() || point.physicsTick<0)
        throw std::runtime_error("Frame reference checkpoint is outside this recording");
    run.inputs.resize(point.inputs);run.poses.resize(point.poses);run.windows.clear();
    run.completed=false;run.endTick=point.recordedThrough;run.macroRevision.clear();
}

// Preserve the recorded order on each input channel. Cross-player events may
// move past one another; press and release on the same player may not.
inline bool canShift(Run const& run, size_t index, int offset) {
    if (index >= run.inputs.size()) return false;
    auto const& in = run.inputs[index];
    int target = in.tick + offset;
    if (target < 0 || target >= run.endTick) return false;
    for (size_t j = index; j-- > 0;) {
        if (run.inputs[j].player == in.player) {
            if (target <= run.inputs[j].tick) return false;
            break;
        }
    }
    for (size_t j = index + 1; j < run.inputs.size(); ++j) {
        if (run.inputs[j].player == in.player) {
            if (target >= run.inputs[j].tick) return false;
            break;
        }
    }
    return true;
}
inline std::vector<Input> shifted(Run const& run, size_t index, int offset) {
    auto result = run.inputs;
    if (index < result.size()) result[index].tick += offset;
    std::stable_sort(result.begin(), result.end(), [](auto const& a, auto const& b) { return a.tick < b.tick; });
    return result;
}

// A trial is a complete replay from the same seed, with all other inputs fixed.
// Each side is searched until its first failure. A censored width is never put
// into an exact bin. Baselines are checked before AND after the candidate suite.
class Search {
public:
    enum class Stage { Baseline, Early, Late, FinalBaseline, Done, Failed };
    Stage stage = Stage::Baseline;
    size_t index = 0;
    int offset = 0, trials = 0;
    std::vector<Window> results;
    std::string error;
    bool finalValidationWarning = false;
    std::string finalValidationMessage;
    explicit Search(size_t count) : results(count) {}
    bool baseline() const { return stage == Stage::Baseline || stage == Stage::FinalBaseline; }
    bool finished() const { return stage == Stage::Done || stage == Stage::Failed; }
    bool allMeasured() const {
        return std::all_of(results.begin(), results.end(), [](Window const& w) { return w.measured; });
    }
    void completeFinalValidationWarning(std::string why) {
        if (stage != Stage::FinalBaseline || !allMeasured()) {
            stage = Stage::Failed;
            error = std::move(why);
            return;
        }
        finalValidationWarning = true;
        finalValidationMessage =
            "Final reference validation failed after every input window was measured. "
            "Measurements were preserved, but the saved results are UNVERIFIED and COULD be inaccurate. " + why;
        stage = Stage::Done;
    }
    void fail(std::string why) {
        // Once every input has already been measured, the final baseline is a
        // post-measurement determinism guard. Losing hours of completed work at
        // this point is worse than preserving it with an explicit global
        // warning. Earlier/global failures remain fatal and accept no widths.
        if (stage == Stage::FinalBaseline && allMeasured()) {
            completeFinalValidationWarning(std::move(why));
            return;
        }
        stage = Stage::Failed; error = std::move(why);
    }
    void accept(bool success,int failureTick=-1) {
        ++trials;
        if (baseline()) {
            if (!success) {
                if (stage == Stage::FinalBaseline)
                    return completeFinalValidationWarning("The unshifted final replay did not reproduce the reference endpoint.");
                return fail("Reference replay desynced. No new measurements were accepted.");
            }
            if (stage == Stage::FinalBaseline || results.empty()) { stage = Stage::Done; return; }
            stage = Stage::Early; offset = -1; return;
        }
        if (finished()) return;
        auto& w = results[index];
        if (stage == Stage::Early) {
            if(!success)w.earlyFailureTick=failureTick;
            if (success) ++w.early;
            if (success && w.width() <= maxWindow) { --offset; return; }
            if (w.width() > maxWindow) { w.overflow = true; advance(); return; }
            stage = Stage::Late; offset = 1; return;
        }
        if(!success)w.lateFailureTick=failureTick;
        if (success) ++w.late;
        if (success && w.width() <= maxWindow) { ++offset; return; }
        w.overflow = w.width() > maxWindow;
        advance();
    }
private:
    void advance() {
        results[index].measured = true;
        ++index;
        if (index == results.size()) { stage = Stage::FinalBaseline; offset = 0; }
        else { stage = Stage::Early; offset = -1; }
    }
};

struct Counts {
    std::array<int, maxWindow + 1> exact{};
    int above = 0, unknown = 0, presses = 0, releases = 0;
};
inline Counts count(Run const& r, int throughTick, bool releases, int playerFilter = 0) {
    Counts out;
    for (size_t i = 0; i < r.inputs.size(); ++i) {
        auto const& in = r.inputs[i];
        if (in.tick > throughTick || (!releases && !in.down) || (playerFilter && in.player + 1 != playerFilter)) continue;
        in.down ? ++out.presses : ++out.releases;
        if (i >= r.windows.size() || !r.windows[i].measured) { ++out.unknown; continue; }
        auto const& w = r.windows[i];
        if (w.overflow || w.width() > maxWindow) ++out.above;
        else ++out.exact[w.width()];
    }
    return out;
}

// NaN's BASE precision model: independent symmetric normal input errors, no
// nerve/fatigue modifiers. Reverse recurrence avoids multiplying tiny success
// probabilities across thousands of inputs. Censored widths use their measured
// lower bound, giving an upper-bound estimate of the required precision.
inline double expectedSeconds(Run const& r, double precision, int throughTick) {
    double duration = static_cast<double>(std::min(throughTick, r.endTick)) / tickRate;
    double logExpected = std::log(std::max(duration, 1e-12));
    double logInverseTail = 0;
    for (size_t i = r.inputs.size(); i-- > 0;) {
        if (r.inputs[i].tick > throughTick) continue;
        if (i >= r.windows.size() || !r.windows[i].measured) return std::numeric_limits<double>::infinity();
        double t = r.inputs[i].tick / tickRate;
        double p = std::erf(r.windows[i].width() / tickRate * precision / (2.0 * std::sqrt(2.0)));
        if (!(p > 0)) return std::numeric_limits<double>::infinity();
        // E[completion] = T + sum(t_i * (1-p_i) / product(p_i..p_n)).
        logInverseTail -= std::log(p);
        if (t > 0 && p < 1) {
            double term = std::log(t) + std::log1p(-p) + logInverseTail;
            double hi = std::max(logExpected, term);
            logExpected = hi + std::log1p(std::exp(std::min(logExpected, term) - hi));
        }
    }
    return std::exp(logExpected);
}
inline std::optional<double> precision(Run const& r, int throughTick) {
    if(r.nextClick)return {}; // Next-click successes do not imply whole-run survival.
    bool any = false;
    for (size_t i = 0; i < r.inputs.size(); ++i) {
        if (r.inputs[i].tick > throughTick) break;
        any = true;
        if (i >= r.windows.size() || !r.windows[i].measured) return {};
    }
    if (!any || throughTick <= 0 || throughTick / tickRate >= 86400.0) return {};
    double low = 0, high = 1;
    while (expectedSeconds(r, high, throughTick) > 86400.0 && high < 1e7) high *= 2;
    for (int n = 0; n < 70; ++n) {
        double mid = (low + high) / 2;
        if (expectedSeconds(r, mid, throughTick) > 86400.0) low = mid;
        else high = mid;
    }
    return high;
}
inline uint64_t hash(std::string const& text) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : text) { h ^= c; h *= 1099511628211ull; }
    return h;
}
}
