#pragma once
#include <Geode/binding/PlayLayer.hpp>
#include <string>
#include <cstdint>
#include "Lifecycle.hpp"

// Synchronous, main-thread-only internal calls in the single bundled DLL.
// Never keep a Message pointer after send() returns.
namespace pulse_frame_link {
enum class Command { Query, SavedSelection, TogglePause, Begin, End, Show, Select, Suspend, Resume, Checkpoint, Rewind, Input };
struct Message {
    Command command = Command::Query;
    PlayLayer* layer = nullptr;
    CheckpointObject* checkpoint = nullptr;
    int button=1;
    std::uint64_t physicsTick=0;
    bool down=false,player1=true;
    int state = 0; // 0 idle, 1 recording, 2 playback / analysis
    int slot = 1;
    bool completed = false;
    bool analyze = false;
    bool savedMacro = false;
    std::string macroRevision;
    bool accepted = false;
    std::string reason;
};
bool toFrames(Message*);
bool toPulse(Message*);
struct Endpoint { bool frames; bool send(Message* m) const { return frames?toFrames(m):toPulse(m); } };
inline Endpoint frames() { return {true}; }
inline Endpoint pulse() { return {false}; }
}
