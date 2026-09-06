#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Windows.h>
#include <unordered_map>
#include <ctime>
#include <chrono>
#include "replay.hpp"
#include "FrameLink.hpp"
#include "library.hpp"

using namespace geode::prelude;
namespace {
enum class Mode { Off, Record, Play, Repair };
struct RepairState {
    bool needReset = false;
    bool failed = false, completed = false, baselineDone = false, confirming = false, capture = false, targetPrepared = false;
    bool deadPlayer1 = true;
    bool oldTest = false, oldPractice = false;
    std::size_t deathTick = 0, failureTick = 0;
    std::vector<std::size_t> targets;
    std::size_t targetCursor = 0, offsetCursor = 0;
    std::size_t currentTarget = 0, winnerTarget = 0;
    int currentOffset = 0, winnerOffset = 0;
    std::vector<int> offsets, passingOffsets;
    std::vector<pulse::Event> trialEvents, winnerEvents;
    std::size_t eventCursor = 0, tick = 0, trialUpdates = 0;
    std::vector<pulse::Step> capturedSteps;
    std::uint64_t trials = 0;
    std::chrono::steady_clock::time_point started{};
};
struct Session {
    PlayLayer* layer = nullptr;
    Mode mode = Mode::Off;
    pulse::Recorder recorder;
    pulse::Tape playback;
    pulse::Player player;
    pulse::FixedUpdateClock clock;
    RepairState repair;
    bool fixedUpdates = false;
    bool frameLinked = false;
    std::unordered_map<CheckpointObject*, pulse::Checkpoint> checkpoints;
    bool injecting = false, resetting = false, restoredCheckpoint = false;
    bool oldCBS = false, oldCOS = false, settingsHeld = false, dirty = false;
    int slot = 1;
    CCLabelBMFont* status = nullptr;
    std::string message = "F6 Record | F7 Play | F8 Stop";
} session;

std::filesystem::path macroDir() { return Mod::get()->getSaveDir() / "macros"; }
std::filesystem::path macroPath(pulse::Tape const& tape, int slot) {
    return macroDir() / fmt::format("{}-{:016x}-slot{}.pulse", tape.levelID, tape.fingerprint, slot);
}
std::string nameKey(pulse::Tape const& tape,int slot) {
    return fmt::format("name-{}-{:016x}-{}",tape.levelID,tape.fingerprint,slot);
}
std::string slotName(pulse::Tape const& tape,int slot) {
    return Mod::get()->getSavedValue<std::string>(nameKey(tape,slot),fmt::format("Slot {}",slot));
}
pulse::Tape identity(PlayLayer* layer) {
    pulse::Tape tape;
    tape.levelID = layer->m_level->m_levelID.value();
    tape.levelName = std::string(layer->m_level->m_levelName);
    tape.fingerprint = pulse::fingerprint(std::string(layer->m_level->m_levelString));
    tape.seed = layer->m_randomSeed;
    tape.fixedUpdates = session.fixedUpdates;
    return tape;
}
void notify(std::string const& message, bool error = false) {
    session.message = message;
    Notification::create(message, error ? NotificationIcon::Error : NotificationIcon::Info, 5.f)->show();
    if (error) log::warn("{}", message); else log::info("{}", message);
}
void apply(pulse::Input input) {
    if (!session.layer) return;
    const bool previous = session.injecting;
    session.injecting = true;
    if(session.mode==Mode::Record && session.frameLinked && !session.resetting) {
        pulse_frame_link::Message msg;msg.command=pulse_frame_link::Command::Input;msg.layer=session.layer;
        msg.physicsTick=session.recorder.tape.steps.empty()?0:session.recorder.tape.steps.size()-1;
        msg.button=input.button;msg.down=input.down;msg.player1=input.player1;pulse_frame_link::frames().send(&msg);
    }
    session.layer->handleButton(input.down, input.button, input.player1);
    session.injecting = previous;
}
void releaseInputs() {
    if (!session.layer) return;
    session.layer->m_queuedButtons.clear();
    for (int p = 0; p < 2; ++p) for (int b = 1; b <= 3; ++b) apply({b, false, p == 0});
    session.recorder.pending.clear();
}
void restoreInputSettings() {
    if (session.layer && session.settingsHeld) {
        session.layer->m_clickBetweenSteps = session.oldCBS;
        session.layer->m_clickOnSteps = session.oldCOS;
    }
    session.settingsHeld = false;
}
void useStepInputs() {
    auto layer = session.layer;
    if (!session.settingsHeld) {
        session.oldCBS = layer->m_clickBetweenSteps;
        session.oldCOS = layer->m_clickOnSteps;
        session.settingsHeld = true;
    }
    // Session-local input quantization. Never change the user's saved game options.
    layer->m_clickBetweenSteps = false;
    layer->m_clickOnSteps = false;
}
void saveRecording() {
    if (!session.dirty || session.recorder.tape.steps.empty()) return;
    pulse::saveRecordingFile(macroPath(session.recorder.tape,session.slot),session.recorder.tape);
    session.dirty=false;
    log::info("Saved macro slot {}: {} steps, {} inputs",session.slot,session.recorder.tape.steps.size(),session.recorder.tape.events.size());
}
void endFrameLink(bool completed, bool analyze, bool saved = false) {
    if (!session.frameLinked) return;
    session.frameLinked = false;
    pulse_frame_link::Message msg;
    msg.command = pulse_frame_link::Command::End;
    msg.layer = session.layer; msg.completed = completed; msg.analyze = analyze;
    msg.savedMacro=saved && !session.recorder.tape.steps.empty();
    if(msg.savedMacro) {
        try{msg.macroRevision=pulse::revision(macroPath(session.recorder.tape,session.slot));}
        catch(std::exception const& e){log::warn("Frame reference revision: {}",e.what());msg.savedMacro=false;}
    }
    pulse_frame_link::frames().send(&msg);
    if(saved && !msg.reason.empty())notify("Macro saved. Frame Windows: " + msg.reason,true);
}
void beginFrameLink() {
    if (!Mod::get()->getSettingValue<bool>("link-frame-windows")) return;
    pulse_frame_link::Message msg;
    msg.command = pulse_frame_link::Command::Begin; msg.layer = session.layer; msg.slot = session.slot;
    pulse_frame_link::frames().send(&msg);
    session.frameLinked = msg.accepted;
    if (!msg.accepted && !msg.reason.empty()) notify("Frame Windows: " + msg.reason, true);
}
void frameCheckpoint(pulse_frame_link::Command command,CheckpointObject* checkpoint=nullptr) {
    if(!session.frameLinked)return;
    pulse_frame_link::Message msg;msg.command=command;msg.layer=session.layer;msg.checkpoint=checkpoint;
    msg.physicsTick=session.recorder.tape.steps.size();
    pulse_frame_link::frames().send(&msg);
    if(!msg.accepted && !msg.reason.empty())notify("Frame Windows: "+msg.reason,true);
}
void pauseAfterPlayback() {
    auto layer = session.layer;
    queueInMainThread([layer] {
        if (layer && PlayLayer::get() == layer && session.layer == layer && session.mode == Mode::Off &&
            !layer->m_isPaused && !layer->m_hasCompletedLevel) layer->pauseGame(false);
    });
}
void restoreRepairEnvironment() {
    if (!session.layer) return;
    session.layer->m_isTestMode = session.repair.oldTest;
    session.layer->m_isPracticeMode = session.repair.oldPractice;
    session.layer->m_uncommittedJumps = 0;
}
void finishRepairSession(std::string const& message, bool error=false, bool resetToStart=true) {
    auto layer=session.layer;
    restoreRepairEnvironment();
    session.mode=Mode::Off;
    session.clock.reset();
    releaseInputs();
    restoreInputSettings();
    session.repair={};
    notify(message,error);
    if(resetToStart && layer) queueInMainThread([layer]{
        if(layer && PlayLayer::get()==layer && session.layer==layer && session.mode==Mode::Off) layer->resetLevelFromStart();
    });
}
void commitRepair() {
    try {
        if(session.repair.winnerEvents.empty() || session.repair.capturedSteps.empty())
            throw std::runtime_error("Repair confirmation did not capture a complete replay");
        auto repaired=session.playback;
        repaired.events=session.repair.winnerEvents;
        repaired.steps=session.repair.capturedSteps;
        pulse::validate(repaired);
        auto path=macroPath(repaired,session.slot);
        pulse::saveRecordingFile(path,repaired); // atomic replace + .bak of the original
        session.playback=std::move(repaired);
        session.player.reset();
        pulse_frame_link::Message frames;frames.command=pulse_frame_link::Command::Select;frames.layer=session.layer;frames.slot=session.slot;
        frames.macroRevision=pulse::revision(path);pulse_frame_link::frames().send(&frames);
        auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-session.repair.started).count();
        auto input=session.repair.winnerTarget+1;
        auto offset=session.repair.winnerOffset;
        finishRepairSession(fmt::format(
            "Auto-repair saved: input {} moved {:+}f. Full macro replay passed ({:.1f}s repair). Original kept as .bak.",
            input,offset,elapsed));
    } catch(std::exception const& e) {
        finishRepairSession(std::string("Auto-repair found a route but could not save it: ")+e.what(),true);
    }
}
void prepareRepairTarget() {
    auto& r=session.repair;
    r.offsets.clear();r.offsetCursor=0;r.passingOffsets.clear();r.targetPrepared=true;
    if(r.targetCursor>=r.targets.size())return;
    r.currentTarget=r.targets[r.targetCursor];
    int radius=static_cast<int>(Mod::get()->getSettingValue<int64_t>("repair-radius"));
    for(int offset=-radius;offset<=radius;++offset) {
        if(offset==0)continue;
        if(pulse::canShiftEvent(session.playback,r.currentTarget,offset))r.offsets.push_back(offset);
    }
}
void beginRepairTrial() {
    auto& r=session.repair;auto pl=session.layer;
    if(!pl || session.mode!=Mode::Repair)return;
    while(true) {
        r.failed=false;r.completed=false;r.failureTick=0;r.capture=false;
        if(!r.baselineDone) {
            if(r.targets.empty()) {finishRepairSession("Auto-repair could not find a recent jump input to test.",true);return;}
            r.currentTarget=r.targets.front();r.currentOffset=0;r.confirming=false;
            break;
        }
        if(r.confirming) {
            r.currentTarget=r.winnerTarget;r.currentOffset=r.winnerOffset;r.capture=true;
            break;
        }
        if(r.targetCursor>=r.targets.size()) {
            finishRepairSession("Auto-repair could not find a nearby frame that survives the full remaining macro. Original macro was left unchanged.",true);
            return;
        }
        if(!r.targetPrepared)prepareRepairTarget();
        if(r.offsetCursor<r.offsets.size()) {
            r.currentTarget=r.targets[r.targetCursor];
            r.currentOffset=r.offsets[r.offsetCursor++];
            break;
        }
        if(!r.passingOffsets.empty()) {
            auto best=pulse::robustPassingCenter(r.passingOffsets);
            if(best) {
                r.winnerTarget=r.targets[r.targetCursor];r.winnerOffset=*best;r.confirming=true;
                r.currentTarget=r.winnerTarget;r.currentOffset=r.winnerOffset;r.capture=true;
                break;
            }
        }
        ++r.targetCursor;r.targetPrepared=false;
    }

    try {
        r.trialEvents=r.currentOffset==0?session.playback.events:pulse::shiftedEvents(session.playback,r.currentTarget,r.currentOffset);
    } catch(std::exception const& e) {
        r.failed=true;r.failureTick=0;r.needReset=true;log::warn("Repair shift rejected: {}",e.what());return;
    }
    r.winnerEvents.clear();
    r.eventCursor=0;r.tick=0;r.trialUpdates=0;r.capturedSteps.clear();++r.trials;
    session.resetting=true;session.injecting=true;
    pl->m_uncommittedJumps=0;pl->m_isTestMode=true;pl->m_isPracticeMode=false;
    pl->m_randomSeed=session.playback.seed;pl->m_replayRandSeed=session.playback.seed;
    pl->m_currentCheckpoint=nullptr;
    pl->resetLevelFromStart();
    pl->m_randomSeed=session.playback.seed;pl->m_replayRandSeed=session.playback.seed;
    pl->m_isTestMode=true;pl->m_isPracticeMode=false;
    pl->m_clickBetweenSteps=false;pl->m_clickOnSteps=false;pl->m_queuedButtons.clear();
    pl->m_player1->releaseAllButtons();pl->m_player2->releaseAllButtons();
    session.injecting=false;session.resetting=false;r.needReset=false;
    if(r.capture)r.capturedSteps.reserve(session.playback.steps.size());
    session.message=!r.baselineDone?"REPAIR | validating original death":
        (r.confirming?fmt::format("REPAIR | confirming input {} {:+}f",r.currentTarget+1,r.currentOffset):
         fmt::format("REPAIR | input {} {:+}f | full-route test",r.currentTarget+1,r.currentOffset));
}
void finishRepairTrial(bool passed) {
    auto& r=session.repair;
    if(session.mode!=Mode::Repair)return;
    if(!r.baselineDone) {
        r.baselineDone=true;
        if(passed) {
            finishRepairSession("Auto-repair retry passed with the original timing, so no macro edit was made. The death was not deterministic.",false);
            return;
        }
        auto delta=r.failureTick>r.deathTick?r.failureTick-r.deathTick:r.deathTick-r.failureTick;
        if(r.failureTick && delta>120) {
            finishRepairSession(fmt::format("Auto-repair stopped: the clean replay failed {} ticks away from the original death, so the failure is not deterministic enough to edit safely.",delta),true);
            return;
        }
        r.needReset=true;return;
    }
    if(r.confirming) {
        if(passed) {
            r.winnerEvents=r.trialEvents;
            commitRepair();
            return;
        }
        log::warn("Repair winner input {} {:+}f failed its final confirmation at tick {}",r.winnerTarget+1,r.winnerOffset,r.failureTick);
        std::erase(r.passingOffsets,r.winnerOffset);
        r.confirming=false;
        if(r.passingOffsets.empty()) {++r.targetCursor;r.targetPrepared=false;}
        r.needReset=true;return;
    }
    if(passed)r.passingOffsets.push_back(r.currentOffset);
    r.needReset=true;
}
void beginAutoRepair(std::size_t deathTick,bool deadPlayer1) {
    if(!session.layer || session.mode!=Mode::Play || session.playback.steps.empty())return;
    auto lookback=static_cast<std::size_t>(Mod::get()->getSettingValue<int64_t>("repair-lookback-inputs"));
    auto targets=pulse::repairEventCandidates(session.playback,deathTick,deadPlayer1,lookback,720);
    if(targets.empty()) {
        auto tick=session.player.tick();
        auto message=fmt::format("Playback died at {:.1f}% (step {} / {}). No recent jump input was available for auto-repair.",
            session.layer->getCurrentPercent(),tick,session.playback.steps.size());
        session.mode=Mode::Off;session.clock.reset();releaseInputs();restoreInputSettings();notify(message,true);pauseAfterPlayback();return;
    }
    RepairState repair;repair.deathTick=deathTick;repair.deadPlayer1=deadPlayer1;repair.targets=std::move(targets);
    repair.oldTest=session.layer->m_isTestMode;repair.oldPractice=session.layer->m_isPracticeMode;
    repair.started=std::chrono::steady_clock::now();repair.needReset=true;
    session.repair=std::move(repair);session.mode=Mode::Repair;session.clock.reset();
    session.message="REPAIR | preparing deterministic full-route search";
    Notification::create("Playback died - searching nearby input frames and verifying the full remaining macro",NotificationIcon::Info,5.f)->show();
    log::info("Auto-repair started after death at tick {} with {} recent input candidates",deathTick,session.repair.targets.size());
}
void stop(bool save, std::string const& message, bool analyze = true) {
    if(session.mode==Mode::Repair) {finishRepairSession("Auto-repair cancelled. Original macro was left unchanged.",false);return;}
    session.mode = Mode::Off;
    session.clock.reset();
    std::string error;
    try { if (save) saveRecording(); }
    catch (std::exception const& e) { error=e.what(); }
    endFrameLink(session.recorder.tape.completed, save && analyze && error.empty(),save && error.empty());
    releaseInputs();
    restoreInputSettings();
    notify(error.empty()?message:error,!error.empty());
}
void abort(std::string const& message) {
    if(session.mode==Mode::Repair) {
        finishRepairSession("Auto-repair stopped: "+message,true);
        log::warn("Auto-repair stopped: {}",message);
        return;
    }
    stop(false, message, false);
    log::warn("Macro stopped: {}", message);
    pauseAfterPlayback();
}
bool attachActiveLevel() {
    // A PlayLayer can leave and re-enter the scene without another init call.
    // Resolve the game's active layer at the point of use instead of treating
    // the init/onExit cache as proof that a level is open.
    auto layer = PlayLayer::get();
    if (!layer || !layer->m_level || !layer->m_player1 || !layer->m_player2) return false;
    if (session.layer == layer) return true;

    // Saving only uses recording data; never dereference the previous layer,
    // which may already have been destroyed during a scene transition.
    try { saveRecording(); }
    catch (std::exception const& e) { log::error("{}", e.what()); }
    session = {};
    session.layer = layer;
    auto parent = layer->m_uiLayer ? static_cast<CCNode*>(layer->m_uiLayer) : static_cast<CCNode*>(layer);
    auto label = typeinfo_cast<CCLabelBMFont*>(parent->getChildByID("pulse-status"_spr));
    if (!label) {
        label = CCLabelBMFont::create("Pulse | F6 record | F7 play | F8 stop", "chatFont.fnt");
        label->setAnchorPoint({0.f, 0.f});
        label->setPosition({8.f, 5.f});
        label->setID("pulse-status"_spr);
        parent->addChild(label, 100);
    }
    session.status = label;
    return true;
}
void checkCompatible(bool playback) {
    if (!attachActiveLevel()) throw std::runtime_error("Open a level first");
    auto layer = session.layer;
    pulse_frame_link::Message link; link.layer = layer;
    pulse_frame_link::frames().send(&link);
    if (link.state == 2) throw std::runtime_error("Frame Windows is analyzing. Use F4 to cancel before starting Pulse.");
    if (link.state == 1 && !session.frameLinked)
        throw std::runtime_error("Stop the separate Frame Windows recording with F5 first. F6 will record both together.");
    if (layer->m_startPosObject) throw std::runtime_error("Start from the beginning; Start Pos runs are not supported");
    if (playback && layer->m_isPracticeMode) throw std::runtime_error("Leave practice mode before playback");
    if (auto cbf = Loader::get()->getLoadedMod("syzzi.click_between_frames")) {
        if (!cbf->getSettingValue<bool>("soft-toggle") || cbf->getSettingValue<bool>("physics-bypass"))
            throw std::runtime_error("In CBF settings, enable Disable CBF and turn off Physics Bypass before using Pulse");
    }
}
void startRecord() {
    try {
        if (!attachActiveLevel()) throw std::runtime_error("Open a level first");
        if(session.mode==Mode::Repair){finishRepairSession("Auto-repair cancelled. Original macro was left unchanged.",false);return;}
        if (session.mode == Mode::Record) { stop(true, "Recording saved"); return; }
        checkCompatible(false);
        saveRecording();
        session.mode = Mode::Off;
        endFrameLink(false, false, true);
        releaseInputs();
        restoreInputSettings();
        session.slot = static_cast<int>(Mod::get()->getSettingValue<int64_t>("slot"));
        session.fixedUpdates = Mod::get()->getSettingValue<bool>("speedhack-compatibility");
        session.clock.reset();
        session.recorder = {};
        session.recorder.tape = identity(session.layer);
        session.checkpoints.clear();
        session.mode = Mode::Record;
        useStepInputs();
        session.layer->resetLevelFromStart();
        if(!session.frameLinked)beginFrameLink();
        notify(fmt::format("Recording slot {} | {}",session.slot,session.frameLinked?"macro + frame reference":"macro only - check Frame Windows"));
    } catch (std::exception const& e) { notify(e.what(), true); }
}
void startPlay() {
    try {
        if (!attachActiveLevel()) throw std::runtime_error("Open a level first");
        if(session.mode==Mode::Repair){finishRepairSession("Auto-repair cancelled. Original macro was left unchanged.",false);return;}
        if (session.mode == Mode::Play) { stop(false, "Playback stopped"); return; }
        checkCompatible(true);
        saveRecording();
        auto slot = static_cast<int>(Mod::get()->getSettingValue<int64_t>("slot"));
        auto current = identity(session.layer);
        auto path = macroPath(current, slot);
        if (!std::filesystem::exists(path)) throw std::runtime_error("No recording in this level's selected slot");
        auto loaded = pulse::load(path);
        if (loaded.levelID != current.levelID || loaded.fingerprint != current.fingerprint)
            throw std::runtime_error("This recording belongs to different level data");
        session.mode = Mode::Off;
        endFrameLink(false, false, true);
        releaseInputs();
        restoreInputSettings();
        session.slot = slot;
        session.playback = std::move(loaded);
        session.fixedUpdates = session.playback.fixedUpdates;
        session.clock.reset();
        session.player.reset();
        pulse_frame_link::Message frames;frames.command=pulse_frame_link::Command::Select;frames.layer=session.layer;frames.slot=slot;
        frames.macroRevision=pulse::revision(path);
        pulse_frame_link::frames().send(&frames);
        session.mode = Mode::Play;
        useStepInputs();
        session.layer->resetLevelFromStart();
        notify(fmt::format("Playing {}: {:.2f}s game time{}", slotName(session.playback,slot), pulse::duration(session.playback),
            session.playback.completed ? " (full run)" : " (saved segment; pauses at its end)"));
    } catch (std::exception const& e) { notify(e.what(), true); }
}
pulse::Step sample(PlayLayer* layer, float dt) {
    auto p1 = layer->m_player1->getPosition();
    auto p2 = layer->m_player2->getPosition();
    return {dt, p1.x, p1.y, p2.x, p2.y};
}
void updateStatus() {
    if (!session.status) return;
    session.status->setVisible(Mod::get()->getSettingValue<bool>("show-status"));
    std::string text;
    if (session.mode == Mode::Record) text = fmt::format("REC {} | {} inputs | F8 save", session.slot, session.recorder.tape.events.size());
    else if (session.mode == Mode::Play) text = fmt::format("PLAY {} | {} / {} | F8 stop", session.slot, session.player.tick(), session.playback.steps.size());
    else if (session.mode == Mode::Repair) {
        auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-session.repair.started).count();
        text=fmt::format("REPAIR {} | {} trials | {:.1f}s | F8 cancel",session.slot,session.repair.trials,elapsed);
    } else text = "Pulse | F6 record | F7 play | F8 stop";
    session.status->setString(text.c_str());
    session.status->setColor(session.mode == Mode::Record ? ccColor3B{255, 115, 110} :
        session.mode == Mode::Play ? ccColor3B{100, 230, 210} :
        session.mode == Mode::Repair ? ccColor3B{255, 205, 95} : ccColor3B{210, 210, 220});
    session.status->limitLabelWidth(300.f, .3f, .15f);
}
}

#ifdef PULSE_INTEGRATION_TEST
#include "../tests/integration.inc"
#endif

class $modify(PulseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::First);
        (void)self.setHookPriority("GJBaseGameLayer::processCommands", Priority::First);
        // Receive the delta after earlier hooks have applied their speed scale.
        (void)self.setHookPriority("GJBaseGameLayer::update", Priority::Last);
    }
    void update(float dt) {
        const bool ours=static_cast<GJBaseGameLayer*>(this)==session.layer;
        if(ours && session.mode==Mode::Repair) {
            if(session.layer->m_isPaused)return;
            auto begin=std::chrono::steady_clock::now();
            int limit=static_cast<int>(Mod::get()->getSettingValue<int64_t>("repair-speed"));
            int budget=static_cast<int>(Mod::get()->getSettingValue<int64_t>("repair-budget"));
            for(int i=0;i<limit;++i) {
                if(session.mode!=Mode::Repair || !session.layer || session.layer->m_isPaused)break;
                if(session.repair.needReset)beginRepairTrial();
                if(session.mode!=Mode::Repair)break;
                GJBaseGameLayer::update(static_cast<float>(pulse::FixedUpdateClock::seconds));
                if(session.mode!=Mode::Repair)break;
                ++session.repair.trialUpdates;
                if(session.repair.trialUpdates>session.playback.steps.size()*4+2400) {
                    finishRepairSession("Auto-repair trial stopped advancing. Disable TPS/speed/physics modifiers and try again.",true);
                    break;
                }
                bool endpoint=session.repair.failed || session.repair.completed || session.repair.tick>=session.playback.steps.size();
                if(endpoint)finishRepairTrial(!session.repair.failed);
                if(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count()>=budget)break;
            }
            return;
        }
        if(ours && session.mode!=Mode::Off && session.layer->m_isPaused) {
            session.clock.reset();return;
        }
        if (!ours || session.mode == Mode::Off || !session.fixedUpdates || session.resetting || session.layer->m_isPaused) {
            if (ours) session.clock.reset();
            GJBaseGameLayer::update(dt);
            return;
        }
        try { session.clock.add(dt); }
        catch (std::exception const& e) { abort(e.what()); return; }
        const auto revision = session.clock.revision();
        const auto mode = session.mode;
        // Keep surplus time for the next render frame instead of dropping it.
        // Limit work per frame when an unusually high speed is selected.
        for (int count = 0; count < 240 && session.clock.take(); ++count) {
            GJBaseGameLayer::update(static_cast<float>(pulse::FixedUpdateClock::seconds));
            // Death, checkpoint restore, quit, or completion can reset/replace
            // the session inside the native update. Never run leftover old time.
            if (static_cast<GJBaseGameLayer*>(this) != session.layer || PlayLayer::get() != session.layer ||
                session.mode != mode || session.layer->m_isPaused || session.clock.revision() != revision) break;
        }
    }
    void handleButton(bool down, int button, bool isPlayer1) {
        if (static_cast<GJBaseGameLayer*>(this) != session.layer || session.mode == Mode::Off || session.injecting || session.resetting) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1); return;
        }
        if (button < 1 || button > 3) { GJBaseGameLayer::handleButton(down, button, isPlayer1); return; }
        if (session.mode == Mode::Play || session.mode == Mode::Repair) return;
        if (session.layer->m_isPaused || m_player1->m_isDead) return;
        try { session.recorder.queue({button, down, isPlayer1}); }
        catch (std::exception const& e) { abort(e.what()); }
    }
    void processCommands(float dt, bool halfTick, bool lastTick) {
#ifdef PULSE_INTEGRATION_TEST
        smokeBeforeStep(this);
#endif
        if (static_cast<GJBaseGameLayer*>(this) == session.layer && session.mode != Mode::Off && !session.resetting &&
            !session.layer->m_isPaused && m_started && !m_player1->m_isDead && !session.layer->m_hasCompletedLevel && dt > 0) {
            try {
                if (halfTick) throw std::runtime_error("Sub-step input timing detected; disable other input/physics modifiers");
                auto actual = sample(session.layer, dt);
                if (session.mode == Mode::Record) {
                    session.recorder.step(actual, apply);
                    session.dirty = true;
                } else if(session.mode==Mode::Repair) {
                    auto& r=session.repair;
                    if(r.capture)r.capturedSteps.push_back(actual);
                    while(r.eventCursor<r.trialEvents.size() && r.trialEvents[r.eventCursor].tick<=r.tick) {
                        auto const& event=r.trialEvents[r.eventCursor];
                        if(event.tick<r.tick) {r.failed=true;r.failureTick=r.tick;break;}
                        apply(static_cast<pulse::Input const&>(event));
                        ++r.eventCursor;
                    }
                    ++r.tick;
                } else {
                    auto tolerance = static_cast<float>(Mod::get()->getSettingValue<double>("desync-tolerance"));
                    if(Mod::get()->getSettingValue<bool>("allow-small-drift"))tolerance=std::max(tolerance,5.f);
                    if (!session.player.step(session.playback, actual, tolerance, apply,session.layer->m_gameState.m_isDualMode)) {
                        stop(false, "Reached the end of the saved inputs", false);
                        pauseAfterPlayback();
                    }
                }
            } catch (std::exception const& e) { abort(e.what()); }
        }
        GJBaseGameLayer::processCommands(dt, halfTick, lastTick);
    }
};

class $modify(PulsePlayLayer, PlayLayer) {
    void pauseGame(bool unfocused) {
        // GD may send onExit while constructing the pause scene, even before
        // m_isPaused is set. This is a temporary suspension, not a saved take.
        pulse_lifecycle::PauseScope scope;
        if(session.layer==this)session.clock.reset();
        PlayLayer::pauseGame(unfocused);
    }
    void onEnterTransitionDidFinish() {
        PlayLayer::onEnterTransitionDidFinish();
        if (PlayLayer::get() == this && attachActiveLevel()) updateStatus();
    }
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (PlayLayer::get() == this && attachActiveLevel()) updateStatus();
        if (session.layer == this && session.mode == Mode::Play && !session.playback.completed &&
            session.player.tick() >= session.playback.steps.size() && !m_hasCompletedLevel && !m_player1->m_isDead) {
            stop(false, "Saved segment finished. Record farther to extend it.", false);
            pauseAfterPlayback();
        }
    }
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        PlayLayer::destroyPlayer(player, object);
        if (session.layer != this || !player->m_isDead || session.resetting || object == m_anticheatSpike) return;
        if(session.mode==Mode::Repair) {
            session.repair.failed=true;session.repair.failureTick=session.repair.tick;
            return;
        }
        if (session.mode == Mode::Play) {
            auto tick = session.player.tick();
            if(Mod::get()->getSettingValue<bool>("auto-repair-playback"))beginAutoRepair(tick,player==m_player1);
            else {
                stop(false, fmt::format("Playback died at {:.1f}% (step {} / {}). Re-record with the same physics settings.",
                    getCurrentPercent(), tick, session.playback.steps.size()), false);
                pauseAfterPlayback();
            }
        }
    }
    void resetLevel() {
        if (session.layer != this || session.mode == Mode::Off) { PlayLayer::resetLevel(); return; }
        if(session.mode==Mode::Repair) {
            if(session.resetting) {PlayLayer::resetLevel();return;}
            finishRepairSession("Auto-repair cancelled by level reset. Original macro was left unchanged.",false,false);
            PlayLayer::resetLevel();return;
        }
        session.resetting = true;
        session.clock.reset();
        frameCheckpoint(pulse_frame_link::Command::Suspend);
        session.restoredCheckpoint = false;
        releaseInputs();
        if (session.mode == Mode::Play) m_randomSeed = session.playback.seed;
        PlayLayer::resetLevel();
        if (session.mode == Mode::Off) { session.resetting = false; return; }
        useStepInputs();
        if (session.mode == Mode::Record) {
            if (!session.restoredCheckpoint) {
                endFrameLink(false,false);
                session.recorder = {};
                session.recorder.tape = identity(this);
                session.checkpoints.clear();
                session.dirty = false;
            } else {
                for (int p = 0; p < 2; ++p) for (int b = 1; b <= 3; ++b) {
                    pulse::Input input{b, false, p == 0};
                    input.down = session.recorder.held[pulse::key(input)];
                    apply(input);
                }
            }
        } else if (session.mode == Mode::Play) {
            if (m_currentCheckpoint) {
                session.resetting = false;
                abort("Playback requires a full restart from the beginning");
                return;
            }
            session.player.reset();
            m_randomSeed = session.playback.seed;
        }
        session.resetting = false;
        if (session.mode == Mode::Record) {
            if(!session.restoredCheckpoint)beginFrameLink();
            else frameCheckpoint(pulse_frame_link::Command::Resume);
        }
    }
    void resume() {
        PlayLayer::resume();
        if (session.layer == this) session.clock.reset();
        // A release while paused can be swallowed by the pause UI. Record a
        // well-defined release boundary so resumed recording cannot stick.
        if (session.layer == this && session.mode == Mode::Record) {
            session.recorder.pending.clear();
            for (int p = 0; p < 2; ++p) for (int b = 1; b <= 3; ++b)
                session.recorder.queue({b, false, p == 0});
        }
    }
    void storeCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::storeCheckpoint(checkpoint);
        if (session.layer == this && session.mode == Mode::Record) {
            session.checkpoints[checkpoint] = session.recorder.checkpoint();
            frameCheckpoint(pulse_frame_link::Command::Checkpoint,checkpoint);
        }
    }
    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::loadFromCheckpoint(checkpoint);
        if (session.layer == this && session.mode == Mode::Record) {
            auto it = session.checkpoints.find(checkpoint);
            if (it == session.checkpoints.end()) { abort("This checkpoint predates the recording. Start recording again."); return; }
            try {
                const auto saved = it->second;
                session.recorder.rewind(saved);
                std::erase_if(session.checkpoints, [&](auto const& entry) { return entry.second.ticks > saved.ticks; });
                session.restoredCheckpoint = true;
                session.dirty = !session.recorder.tape.steps.empty();
                frameCheckpoint(pulse_frame_link::Command::Rewind,checkpoint);
            } catch (std::exception const& e) { abort(e.what()); }
        }
    }
    void levelComplete() {
        if(session.layer==this && session.mode==Mode::Repair) {
            // A repair trial only needs the deterministic success signal; do not
            // open the normal end screen or modify stats during fast search.
            session.repair.completed=true;return;
        }
        // Let Geometry Dash and the existing mod chain handle completion normally.
        PlayLayer::levelComplete();
        if (session.layer != this) return;
        if (session.mode == Mode::Record) {
            session.recorder.tape.completed = true;
            session.dirty = true;
            stop(true, "Level complete - recording saved");
        } else if (session.mode == Mode::Play) stop(false, "Playback reached the end");
    }
    void onQuit() {
        if (session.layer == this) {
            if(session.mode==Mode::Repair)finishRepairSession("Auto-repair cancelled. Original macro was left unchanged.",false,false);
            else stop(true, "Pulse stopped", false);
            session.status = nullptr;
            session.layer = nullptr;
        }
        PlayLayer::onQuit();
    }
    void onExit() {
        if(pulse_lifecycle::preserveOnExit(PlayLayer::get()==this,m_isPaused)) {
            if(session.layer==this)session.clock.reset();
            PlayLayer::onExit();return;
        }
        if (session.layer == this) {
            if(session.mode==Mode::Repair)restoreRepairEnvironment();
            session.mode = Mode::Off;
            session.clock.reset();
            bool saved=true;
            try { saveRecording(); } catch (std::exception const& e) { saved=false;log::error("{}", e.what()); }
            endFrameLink(session.recorder.tape.completed, false, saved);
            restoreInputSettings();
            session.repair={};
            session.status = nullptr;
            session.layer = nullptr;
        }
        PlayLayer::onExit();
    }
};

class PulsePopup : public Popup {
    CCLabelBMFont* m_slotLabel = nullptr;
    CCLabelBMFont* m_details = nullptr;
    TextInput* m_name = nullptr;
    pulse::Tape m_identity;
    int selected() const { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("slot")); }
    void button(char const* title, CCPoint position, SEL_MenuHandler callback, float scale=.55f) {
        auto sprite=ButtonSprite::create(title);sprite->setScale(scale);
        auto item=CCMenuItemSpriteExtra::create(sprite,this,callback);
        item->setPosition(position);m_buttonMenu->addChild(item);
    }
    void updateSlot() {
        const int slot=selected();
        m_slotLabel->setString(fmt::format("Slot {} / 20",slot).c_str());
        m_name->setString(slotName(m_identity,slot));
        try {
            auto path=macroPath(m_identity,slot);
            std::string detail;
            if(std::filesystem::exists(path)) {
                auto tape=pulse::load(path);
                auto stamp=std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(path));
                auto raw=std::chrono::system_clock::to_time_t(stamp);std::tm local{};localtime_s(&local,&raw);
                char time[32]{};std::strftime(time,sizeof(time),"%m/%d %H:%M:%S",&local);
                detail=fmt::format("Saved: {:.2f}s | {} inputs | {}\nUpdated {}",pulse::duration(tape),tape.events.size(),tape.completed?"Full run":"Segment",time);
            } else detail="Empty slot - New Recording starts a take here";
            if(session.mode==Mode::Record)
                detail+=fmt::format("\nRecording slot {}: {:.2f}s / {} inputs {}",session.slot,pulse::duration(session.recorder.tape),session.recorder.tape.events.size(),session.dirty?"(unsaved)":"(saved)");
            else if(session.mode==Mode::Play)detail+=fmt::format("\nPlaying slot {}",session.slot);
            else if(session.mode==Mode::Repair)detail+=fmt::format("\nAuto-repairing slot {}: {} full-route trials",session.slot,session.repair.trials);
            else if(session.dirty)detail+=fmt::format("\nUnsaved take in slot {} - select it and Save",session.slot);
            m_details->setString(detail.c_str());
        } catch(std::exception const& e){m_details->setString(e.what());}
        m_details->limitLabelWidth(355.f,.40f,.22f);
    }
    bool saveName() {
        auto name=std::string(m_name->getString());
        const auto first=name.find_first_not_of(" \t");
        name=first==std::string::npos?fmt::format("Slot {}",selected()):name.substr(first,name.find_last_not_of(" \t")-first+1);
        if(name.size()>48)name.resize(48);
        Mod::get()->setSavedValue<std::string>(nameKey(m_identity,selected()),name);
        auto saved=Mod::get()->saveData();
        if(!saved){notify("Could not save the macro name",true);return false;}
        return true;
    }
    bool setup() {
        if(!attachActiveLevel() || !Popup::init(390.f,300.f))return false;
        m_identity=identity(session.layer);setTitle("Pulse Recordings");
        m_slotLabel=CCLabelBMFont::create("","bigFont.fnt");m_slotLabel->setPosition({195,251});m_slotLabel->setScale(.43f);m_mainLayer->addChild(m_slotLabel);
        button("<",{43,251},menu_selector(PulsePopup::previousSlot));button(">",{347,251},menu_selector(PulsePopup::nextSlot));
        m_name=TextInput::create(260,"Macro name");m_name->setCommonFilter(CommonFilter::Name);m_name->setMaxCharCount(48);m_name->setScale(.8f);m_name->setPosition({159,215});m_mainLayer->addChild(m_name);
        button("Name",{321,215},menu_selector(PulsePopup::rename));
        m_details=CCLabelBMFont::create("","chatFont.fnt");m_details->setPosition({195,171});m_details->setScale(.4f);m_mainLayer->addChild(m_details);
        button("New Recording",{79,120},menu_selector(PulsePopup::record),.43f);
        button("Play",{195,120},menu_selector(PulsePopup::play));
        button("Save",{311,120},menu_selector(PulsePopup::save));
        button("Stop",{79,82},menu_selector(PulsePopup::stopNow));
        button("Delete",{195,82},menu_selector(PulsePopup::remove));
        button("Undo Delete",{311,82},menu_selector(PulsePopup::restore),.43f);
        button("Files",{79,44},menu_selector(PulsePopup::files));
        button("Frame Windows",{254,44},menu_selector(PulsePopup::frames),.43f);
        auto hint=CCLabelBMFont::create("New takes replace this slot when saved. F6 record/save | F8 save","chatFont.fnt");
        hint->setPosition({195,17});hint->limitLabelWidth(360,.34f,.22f);m_mainLayer->addChild(hint);
        updateSlot();return true;
    }
    void previousSlot(CCObject*) {auto n=selected();Mod::get()->setSettingValue<int64_t>("slot",n==1?20:n-1);updateSlot();}
    void nextSlot(CCObject*) {auto n=selected();Mod::get()->setSettingValue<int64_t>("slot",n==20?1:n+1);updateSlot();}
    void rename(CCObject*) {if(saveName())notify("Macro name saved");updateSlot();}
    void record(CCObject*) {
        if(!saveName())return;
        if(session.mode==Mode::Record)stop(true,"Previous take saved",false);
        startRecord();onClose(nullptr);
    }
    void play(CCObject*) {startPlay();onClose(nullptr);}
    void save(CCObject*) {
        if(!attachActiveLevel())return;
        if((session.mode==Mode::Record || session.dirty) && selected()!=session.slot){notify(fmt::format("The current take is in slot {}. Select that slot to save it.",session.slot),true);return;}
        if(!saveName())return;
        if(session.mode==Mode::Record || session.dirty) {
            const auto slot=session.slot;
            stop(true,fmt::format("Saved {}: {:.2f}s / {} inputs",slotName(session.recorder.tape,slot),pulse::duration(session.recorder.tape),session.recorder.tape.events.size()));
            onClose(nullptr);return;
        }
        notify(std::filesystem::exists(macroPath(m_identity,selected()))?"This recording is already saved":"No take to save. Choose New Recording first.");updateSlot();
    }
    void stopNow(CCObject*) {if(attachActiveLevel())stop(true,"Stopped - take saved if present");onClose(nullptr);}
    void remove(CCObject*) {
        try {
            auto path=macroPath(m_identity,selected());
            if(!std::filesystem::exists(path))throw std::runtime_error("No saved macro in this slot");
            if(session.slot==selected() && (session.mode!=Mode::Off || session.dirty)) {
                stop(false,"Stopped for deletion",false);session.dirty=false;session.recorder={};
            }
            pulse::archiveRecording(path);notify("Macro deleted. Undo Delete restores it.");
        }catch(std::exception const& e){notify(e.what(),true);}
        updateSlot();
    }
    void restore(CCObject*) {
        try {
            if(session.slot==selected() && (session.mode==Mode::Record || session.dirty))throw std::runtime_error("Save the current take before restoring a deleted macro");
            pulse::restoreRecording(macroPath(m_identity,selected()));notify("Deleted macro restored");
        }catch(std::exception const& e){notify(e.what(),true);}
        updateSlot();
    }
    void files(CCObject*) {try{std::filesystem::create_directories(macroDir());file::openFolder(macroDir());}catch(std::exception const& e){notify(e.what(),true);}}
    void frames(CCObject*) {
        onClose(nullptr);
        queueInMainThread([]{pulse_frame_link::Message msg;msg.command=pulse_frame_link::Command::Show;msg.layer=PlayLayer::get();pulse_frame_link::frames().send(&msg);if(!msg.accepted)notify("Frame Windows could not find the active level",true);});
    }
public:
    static PulsePopup* create(){auto popup=new PulsePopup;if(popup->setup()){popup->autorelease();return popup;}delete popup;return nullptr;}
};
class $modify(PulsePauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto menu = CCMenu::create();
        menu->setID("pulse-menu"_spr);
        menu->setPosition({0, 0});
        auto sprite = ButtonSprite::create("Replay");
        sprite->setScale(.55f);
        auto button = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(PulsePauseLayer::openPulse));
        button->setPosition({CCDirector::get()->getWinSize().width - 45.f, 25.f});
        menu->addChild(button);
        addChild(menu, 100);
    }
    void openPulse(CCObject*) {
        attachActiveLevel();
        if (auto popup = PulsePopup::create()) popup->show();
    }
};

bool pulse_frame_link::toPulse(pulse_frame_link::Message* msg) {
        if(msg->command==pulse_frame_link::Command::SavedSelection) {
            if(!msg->layer || msg->layer!=PlayLayer::get())return false;
            msg->slot=static_cast<int>(Mod::get()->getSettingValue<int64_t>("slot"));
            try{msg->macroRevision=pulse::revision(macroPath(identity(msg->layer),msg->slot));}
            catch(std::exception const& e){msg->reason=e.what();}
            msg->accepted=true;return true;
        }
        if (msg->command != pulse_frame_link::Command::Query) return false;
        msg->accepted = true;
        if (msg->layer == session.layer) msg->state = static_cast<int>(session.mode);
        return true;
}
$execute {
    listenForKeybindSettingPresses("pause-key",[](Keybind const&,bool down,bool repeat,double) {
        if(!PlayLayer::get() || !down || repeat)return false;
        pulse_frame_link::Message msg;msg.command=pulse_frame_link::Command::TogglePause;msg.layer=PlayLayer::get();
        return pulse_frame_link::frames().send(&msg);
    });
    listenForKeybindSettingPresses("record-key", [](Keybind const&, bool down, bool repeat, double) {
        if (!attachActiveLevel()) return false;
        if (down && !repeat) startRecord();
        return true;
    });
    listenForKeybindSettingPresses("play-key", [](Keybind const&, bool down, bool repeat, double) {
        if (!attachActiveLevel()) return false;
        if (down && !repeat) startPlay();
        return true;
    });
    listenForKeybindSettingPresses("stop-key", [](Keybind const&, bool down, bool repeat, double) {
        if (!attachActiveLevel()) return false;
        if (down && !repeat) stop(true, "Stopped - recording saved if present");
        return true;
    });
    log::info("Pulse: controls ready");
}



