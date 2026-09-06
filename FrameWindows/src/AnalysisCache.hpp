#pragma once
#include "Core.hpp"
#include <span>

namespace fwl {
constexpr int checkpointSafetyTicks = 12;
constexpr int checkpointValidationTicks = 60;
// Native checkpoints can retain a substantial amount of level state. 256 is a
// deliberate hard ceiling: high enough to keep dense ~200-input macros close
// to their inputs without allowing an unbounded memory cache.
constexpr size_t checkpointCap = 256;

// A snapshot at tick N represents the boundary BEFORE input/physics step N.
struct AnalysisTimeline {
    int tick = 0;
    size_t inputCursor = 0, poseCursor = 0;
    std::array<bool, 2> jumpHeld{};
    uint64_t randomSeed = 0, replaySeed = 0;
    bool verified = false, usable = true;
};
inline int checkpointSpacing(int endTick, int requested = 180, size_t cap = checkpointCap) {
    cap=std::max<size_t>(1,std::min(cap,checkpointCap));
    return std::max(std::clamp(requested, 180, 2400),
        static_cast<int>((static_cast<int64_t>(std::max(0, endTick)) + static_cast<int64_t>(cap) - 1) /
            static_cast<int64_t>(cap)));
}
template<class Points>
std::optional<size_t> selectAnalysisCheckpoint(Points const& points, int targetTick,
        int window = maxWindow, int safety = checkpointSafetyTicks, bool includeUnusable = false) {
    int64_t latest = static_cast<int64_t>(targetTick) - std::max(0, window) - std::max(0, safety);
    std::optional<size_t> found;
    for (size_t i = 0; i < points.size(); ++i) {
        auto const& point = points[i];
        if (point.tick >= 0 && point.tick <= latest && (point.usable || includeUnusable) &&
            (!found || point.tick > points[*found].tick)) found = i;
    }
    return found;
}
// A shifted target cannot be restored from a state after either the original
// target event or its shifted event. This boundary is candidate-specific:
//   late (+) trials may safely start close to the original event,
//   early (-) trials start just before the shifted event.
// The fixed safety margin is retained, so this optimization changes replay
// work only; it does not weaken the correctness guard.
inline int candidateRestoreBoundary(Run const& run,size_t index,int offset,
        int safety=checkpointSafetyTicks) {
    if(index>=run.inputs.size())return -1;
    int original=run.inputs[index].tick;
    int shifted=original+offset;
    return std::min(original,shifted)-std::max(0,safety);
}
template<class Points>
std::optional<size_t> selectCandidateCheckpoint(Points const& points,Run const& run,
        size_t index,int offset,int safety=checkpointSafetyTicks,bool includeUnusable=false) {
    int latest=candidateRestoreBoundary(run,index,offset,safety);
    if(latest<0)return {};
    std::optional<size_t> found;
    for(size_t i=0;i<points.size();++i) {
        auto const& point=points[i];
        if(point.tick>=0 && point.tick<=latest && (point.usable || includeUnusable) &&
            (!found || point.tick>points[*found].tick))found=i;
    }
    return found;
}

inline void appendSampled(std::vector<int>& plan,std::vector<int> points,size_t room) {
    if(room==0 || points.empty())return;
    std::sort(points.begin(),points.end());
    points.erase(std::unique(points.begin(),points.end()),points.end());
    std::erase_if(points,[&](int tick){return std::binary_search(plan.begin(),plan.end(),tick);});
    if(points.empty())return;
    if(points.size()<=room) {
        plan.insert(plan.end(),points.begin(),points.end());
        std::sort(plan.begin(),plan.end());
        plan.erase(std::unique(plan.begin(),plan.end()),plan.end());
        return;
    }
    std::vector<int> selected;selected.reserve(room);
    if(room==1)selected.push_back(points.front());
    else {
        const int64_t first=points.front(),last=points.back(),range=last-first;
        for(size_t slot=0;slot<room;++slot) {
            int64_t target=first+(range*static_cast<int64_t>(slot))/static_cast<int64_t>(room-1);
            auto it=std::lower_bound(points.begin(),points.end(),static_cast<int>(target));
            int chosen;
            if(it==points.begin())chosen=*it;
            else if(it==points.end())chosen=points.back();
            else {
                int hi=*it,lo=*(it-1);
                chosen=(target-lo<=hi-target)?lo:hi;
            }
            if(selected.empty() || selected.back()!=chosen)selected.push_back(chosen);
        }
    }
    plan.insert(plan.end(),selected.begin(),selected.end());
    std::sort(plan.begin(),plan.end());
    plan.erase(std::unique(plan.begin(),plan.end()),plan.end());
}

// Build analyzer-owned checkpoints around actual inputs. Universal anchors at
// T-maxWindow-safety are always prioritized because they are safe for every
// tested offset. Any remaining cache budget is spent on progressively closer
// candidate anchors. The first close tier (T-1-safety) accelerates the common
// -1f trial and every +f trial; deeper tiers help early windows without ever
// restoring past a shifted event.
inline std::vector<int> inputAwareCheckpointPlan(Run const& run, size_t cap = checkpointCap,
        int window = maxWindow, int safety = checkpointSafetyTicks) {
    cap=std::min(cap,checkpointCap);
    if(cap==0)return {};
    auto makeTier=[&](int depth) {
        std::vector<int> out;out.reserve(run.inputs.size());
        for(auto const& in:run.inputs) {
            int tick=in.tick-std::max(0,depth)-std::max(0,safety);
            if(tick>0 && tick<run.endTick)out.push_back(tick);
        }
        std::sort(out.begin(),out.end());
        out.erase(std::unique(out.begin(),out.end()),out.end());
        return out;
    };

    std::vector<int> plan;
    auto universal=makeTier(std::max(0,window));
    // If the universal set itself exceeds the limit, preserve the old behavior:
    // sample it across the timeline so every region still has a safe fallback.
    if(universal.size()>=cap) {
        appendSampled(plan,std::move(universal),cap);
        return plan;
    }
    plan=std::move(universal);

    // Highest-value candidate-specific tiers first. A tier with depth D can be
    // used for early offsets up to -D and for every late offset.
    std::array<int,5> tiers{1,4,8,12,16};
    for(int depth:tiers) {
        if(plan.size()>=cap)break;
        auto tier=makeTier(depth);
        appendSampled(plan,std::move(tier),cap-plan.size());
    }
    return plan;
}

inline AnalysisTimeline timelineAt(Run const& run, int tick) {
    AnalysisTimeline point; point.tick = tick;
    while (point.inputCursor < run.inputs.size() && run.inputs[point.inputCursor].tick < tick) {
        auto const& in = run.inputs[point.inputCursor++];
        point.jumpHeld.at(in.player) = in.down;
    }
    point.poseCursor = std::lower_bound(run.poses.begin(), run.poses.end(), tick,
        [](Pose const& p, int t) { return p.tick < t; }) - run.poses.begin();
    return point;
}

// Stable merge of one shifted event with the immutable reference. No trial-sized
// allocation/sort. Ties use original index, matching shifted()'s stable_sort.
struct CandidatePlayback {
    size_t cursor = 0;
    std::optional<size_t> target;
    int offset = 0;
    bool deliveredTarget = false;
    void reset(size_t from, std::optional<size_t> index = {}, int shift = 0) {
        cursor = from; target = index; offset = shift; deliveredTarget = false;
    }
    std::optional<size_t> next(std::span<Input const> inputs) {
        if (target && cursor == *target) ++cursor;
        if (target && !deliveredTarget) {
            int shiftedTick = inputs[*target].tick + offset;
            if (cursor >= inputs.size() || shiftedTick < inputs[cursor].tick ||
                (shiftedTick == inputs[cursor].tick && *target < cursor)) return target;
        }
        if (cursor < inputs.size()) return cursor;
        return {};
    }
    int tick(std::span<Input const> inputs, size_t index) const {
        return inputs[index].tick + (target && index == *target ? offset : 0);
    }
    void consume(size_t index) {
        if (target && index == *target) deliveredTarget = true;
        else ++cursor;
    }
};
}
