#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace pulse {
constexpr std::size_t maxSteps = 4'000'000;
constexpr std::size_t maxEvents = 1'000'000;
constexpr std::uintmax_t maxFileBytes = 512 * 1024 * 1024;
// Consume speed-scaled GAME time, never unscaled wall time. Retain fractional
// updates across render frames, including frames that do not advance physics.
class FixedUpdateClock {
    double m_pending = 0;
    std::uint64_t m_revision = 0;
public:
    static constexpr double seconds = 1.0 / 240.0;
    void reset() { m_pending = 0; ++m_revision; }
    std::uint64_t revision() const { return m_revision; }
    void add(double gameDelta) {
        if (!std::isfinite(gameDelta) || gameDelta < 0 || gameDelta > 10 || m_pending + gameDelta > 10)
            throw std::runtime_error("Speedhack timing backlog exceeded 10 seconds. Lower the speed and restart.");
        m_pending += gameDelta;
    }
    bool take() {
        if (m_pending + 1e-12 < seconds) return false;
        m_pending = std::max(0.0, m_pending - seconds);
        return true;
    }
};
struct Input { int button = 1; bool down = false; bool player1 = true; };
struct Event : Input { std::size_t tick = 0; };
struct Step { float dt = 0; float x1 = 0, y1 = 0, x2 = 0, y2 = 0; };
struct Tape {
    int levelID = 0;
    std::uint64_t fingerprint = 0, seed = 0;
    std::string levelName;
    bool completed = false;
    bool fixedUpdates = false; // false for legacy v1 recordings
    std::vector<Step> steps;
    std::vector<Event> events;
};
inline std::uint64_t fingerprint(std::string const& data) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : data) { hash ^= byte; hash *= 1099511628211ULL; }
    return hash;
}
inline std::size_t key(Input const& input) {
    if (input.button < 1 || input.button > 3) throw std::runtime_error("Invalid input button");
    return (input.player1 ? 0 : 3) + input.button - 1;
}
inline bool sameTiming(float a, float b) {
    return std::isfinite(a) && std::isfinite(b) && a > 0 && b > 0 &&
        std::abs(a - b) <= std::max(0.0000001f, std::abs(a) * 0.0001f);
}
inline bool samePosition(Step const& a, Step const& b, float tolerance,bool checkPlayer2=true) {
    return std::isfinite(b.x1) && std::isfinite(b.y1) &&
        std::hypot(a.x1 - b.x1, a.y1 - b.y1) <= tolerance &&
        (!checkPlayer2 || (std::isfinite(b.x2) && std::isfinite(b.y2) && std::hypot(a.x2 - b.x2, a.y2 - b.y2) <= tolerance));
}
inline void validate(Tape const& tape) {
    if (tape.levelName.size() > 1024 || tape.steps.empty() || tape.steps.size() > maxSteps || tape.events.size() > maxEvents)
        throw std::runtime_error("Invalid macro size");
    for (auto const& s : tape.steps) {
        if (!std::isfinite(s.dt) || s.dt <= 0 || s.dt > 10 || !std::isfinite(s.x1) ||
            !std::isfinite(s.y1) || !std::isfinite(s.x2) || !std::isfinite(s.y2))
            throw std::runtime_error("Invalid physics sample");
    }
    std::size_t previous = 0;
    for (auto const& e : tape.events) {
        key(e);
        if (e.tick < previous || e.tick >= tape.steps.size()) throw std::runtime_error("Invalid event order");
        previous = e.tick;
    }
}
inline void write(std::ostream& stream, Tape const& tape) {
    validate(tape);
    stream.imbue(std::locale::classic());
    stream << "PULSE_MACRO 2\n" << tape.levelID << ' ' << tape.fingerprint << ' ' << tape.seed << ' '
           << int(tape.completed) << ' ' << int(tape.fixedUpdates) << '\n' << std::quoted(tape.levelName) << '\n'
           << tape.steps.size() << ' ' << tape.events.size() << '\n'
           << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (auto const& s : tape.steps) stream << s.dt << ' ' << s.x1 << ' ' << s.y1 << ' ' << s.x2 << ' ' << s.y2 << '\n';
    for (auto const& e : tape.events) stream << e.tick << ' ' << e.button << ' ' << int(e.down) << ' ' << int(e.player1) << '\n';
    if (!stream) throw std::runtime_error("Could not write macro");
}
inline Tape read(std::istream& stream) {
    stream.imbue(std::locale::classic());
    Tape tape;
    std::string magic;
    int version = 0, completed = 0;
    std::size_t steps = 0, events = 0;
    if (!(stream >> magic >> version) || magic != "PULSE_MACRO" || (version != 1 && version != 2))
        throw std::runtime_error("Unsupported macro format");
    if (!(stream >> tape.levelID >> tape.fingerprint >> tape.seed >> completed))
        throw std::runtime_error("Invalid macro header");
    if (version == 2) {
        int fixed = 0;
        if (!(stream >> fixed) || fixed < 0 || fixed > 1) throw std::runtime_error("Invalid timing mode");
        tape.fixedUpdates = fixed != 0;
    }
    if (!(stream >> std::quoted(tape.levelName) >> steps >> events) ||
        completed < 0 || completed > 1 || steps == 0 || steps > maxSteps || events > maxEvents || tape.levelName.size() > 1024)
        throw std::runtime_error("Invalid macro header");
    tape.completed = completed != 0;
    // Read incrementally: hostile counts must not cause an up-front huge allocation.
    for (std::size_t i = 0; i < steps; ++i) {
        Step s;
        if (!(stream >> s.dt >> s.x1 >> s.y1 >> s.x2 >> s.y2)) throw std::runtime_error("Truncated physics samples");
        tape.steps.push_back(s);
    }
    for (std::size_t i = 0; i < events; ++i) {
        Event e; int down = 0, p1 = 0;
        if (!(stream >> e.tick >> e.button >> down >> p1) || down < 0 || down > 1 || p1 < 0 || p1 > 1)
            throw std::runtime_error("Invalid input event");
        e.down = down != 0; e.player1 = p1 != 0;
        tape.events.push_back(e);
    }
    stream >> std::ws;
    if (!stream.eof()) throw std::runtime_error("Unexpected trailing macro data");
    validate(tape);
    return tape;
}
inline Tape load(std::filesystem::path const& path) {
    if (std::filesystem::file_size(path) > maxFileBytes) throw std::runtime_error("Macro exceeds file size limit");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Could not open macro");
    return read(stream);
}

inline bool canShiftEvent(Tape const& tape, std::size_t index, int offset) {
    if (index >= tape.events.size()) return false;
    auto const& event = tape.events[index];
    auto shiftedTick = static_cast<long long>(event.tick) + offset;
    if (shiftedTick < 0 || shiftedTick >= static_cast<long long>(tape.steps.size())) return false;
    const auto channel = key(event);
    for (std::size_t j = index; j-- > 0;) {
        if (key(tape.events[j]) == channel) {
            if (shiftedTick <= static_cast<long long>(tape.events[j].tick)) return false;
            break;
        }
    }
    for (std::size_t j = index + 1; j < tape.events.size(); ++j) {
        if (key(tape.events[j]) == channel) {
            if (shiftedTick >= static_cast<long long>(tape.events[j].tick)) return false;
            break;
        }
    }
    return true;
}

inline std::vector<Event> shiftedEvents(Tape const& tape, std::size_t index, int offset) {
    if (!canShiftEvent(tape,index,offset)) throw std::runtime_error("Invalid repair input shift");
    auto events=tape.events;
    events[index].tick=static_cast<std::size_t>(static_cast<long long>(events[index].tick)+offset);
    std::vector<std::pair<Event,std::size_t>> tagged;tagged.reserve(events.size());
    for(std::size_t i=0;i<events.size();++i)tagged.push_back({events[i],i});
    std::stable_sort(tagged.begin(),tagged.end(),[](auto const& a,auto const& b){
        if(a.first.tick!=b.first.tick)return a.first.tick<b.first.tick;
        return a.second<b.second;
    });
    events.clear();events.reserve(tagged.size());
    for(auto& item:tagged)events.push_back(item.first);
    return events;
}

inline std::vector<std::size_t> repairEventCandidates(Tape const& tape,std::size_t deathTick,bool player1,
        std::size_t lookbackEvents=6,std::size_t lookbackTicks=720) {
    std::vector<std::size_t> out;
    if(tape.events.empty() || lookbackEvents==0)return out;
    auto minTick=deathTick>lookbackTicks?deathTick-lookbackTicks:0;
    for(std::size_t i=tape.events.size();i-- > 0;) {
        auto const& e=tape.events[i];
        if(e.tick>deathTick)continue;
        if(e.tick<minTick)break;
        if(e.button!=1 || e.player1!=player1)continue;
        out.push_back(i);
        if(out.size()>=lookbackEvents)break;
    }
    return out;
}

inline std::optional<int> robustPassingCenter(std::vector<int> offsets) {
    if(offsets.empty())return {};
    std::sort(offsets.begin(),offsets.end());
    offsets.erase(std::unique(offsets.begin(),offsets.end()),offsets.end());
    int bestStart=offsets.front(),bestEnd=offsets.front();
    int start=offsets.front(),prev=offsets.front();
    auto better=[&](int a,int b,int ca,int cb){
        int width=b-a,curWidth=cb-ca;
        if(width!=curWidth)return width>curWidth;
        int center=(a+b)/2,curCenter=(ca+cb)/2;
        if(std::abs(center)!=std::abs(curCenter))return std::abs(center)<std::abs(curCenter);
        return std::abs(a)+std::abs(b)<std::abs(ca)+std::abs(cb);
    };
    for(std::size_t i=1;i<=offsets.size();++i) {
        bool end=i==offsets.size() || offsets[i]!=prev+1;
        if(end) {
            if(better(start,prev,bestStart,bestEnd)){bestStart=start;bestEnd=prev;}
            if(i<offsets.size())start=offsets[i];
        }
        if(i<offsets.size())prev=offsets[i];
    }
    // Pick the integer midpoint of the widest contiguous full-route passing region.
    // For an even-width region, prefer the midpoint closer to the original timing.
    int low=bestStart+(bestEnd-bestStart)/2;
    if((bestEnd-bestStart)%2==0)return low;
    int high=low+1;
    return std::abs(low)<=std::abs(high)?low:high;
}
struct Checkpoint { std::size_t ticks = 0; std::array<bool, 6> held{}; };
class Recorder {
public:
    Tape tape;
    std::vector<Input> pending;
    std::array<bool, 6> held{};
    void queue(Input input) {
        key(input);
        if (pending.size() >= 4096) throw std::runtime_error("Too many queued inputs");
        pending.push_back(input);
    }
    template<class Apply> void step(Step sample, Apply apply) {
        if (tape.steps.size() >= maxSteps) throw std::runtime_error("Recording reached its size limit");
        const auto tick = tape.steps.size();
        tape.steps.push_back(sample);
        // Move the queue because applying an input can invoke other hooks.
        auto inputs = std::move(pending);
        pending.clear();
        for (auto const& input : inputs) {
            auto i = key(input);
            if (held[i] == input.down) continue;
            if (tape.events.size() >= maxEvents) throw std::runtime_error("Recording reached its input limit");
            held[i] = input.down;
            Event event; static_cast<Input&>(event) = input; event.tick = tick;
            tape.events.push_back(event);
            apply(input);
        }
    }
    Checkpoint checkpoint() const { return {tape.steps.size(), held}; }
    void rewind(Checkpoint const& checkpoint) {
        if (checkpoint.ticks > tape.steps.size()) throw std::runtime_error("Invalid checkpoint timeline");
        tape.steps.resize(checkpoint.ticks);
        std::erase_if(tape.events, [&](auto const& event) { return event.tick >= checkpoint.ticks; });
        held = checkpoint.held;
        pending.clear();
        tape.completed = false;
    }
};
class Player {
    std::size_t m_tick = 0, m_event = 0;
public:
    std::array<bool, 6> held{};
    void reset() { m_tick = m_event = 0; held = {}; }
    std::size_t tick() const { return m_tick; }
    template<class Apply> bool step(Tape const& tape, Step actual, float tolerance, Apply apply,bool checkPlayer2=true) {
        if (m_tick >= tape.steps.size()) return false;
        auto const& expected = tape.steps[m_tick];
        if (!sameTiming(expected.dt, actual.dt)) throw std::runtime_error("Physics timing changed. Use the recording's TPS/physics settings.");
        if (!samePosition(expected, actual, tolerance,checkPlayer2)) {
            std::ostringstream error;error<<std::fixed<<std::setprecision(2);
            error<<"Position drift at step "<<m_tick<<": P1 "<<std::hypot(expected.x1-actual.x1,expected.y1-actual.y1);
            if(checkPlayer2)error<<", P2 "<<std::hypot(expected.x2-actual.x2,expected.y2-actual.y2);
            error<<"; limit "<<tolerance;
            throw std::runtime_error(error.str());
        }
        while (m_event < tape.events.size() && tape.events[m_event].tick == m_tick) {
            auto const& input = tape.events[m_event++];
            held[key(input)] = input.down;
            apply(input);
        }
        ++m_tick;
        return true;
    }
};
}
