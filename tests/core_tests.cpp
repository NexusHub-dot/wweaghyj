#include "replay.hpp"
#include <iostream>
#include <stdexcept>
using namespace pulse;
void check(bool value, char const* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    bool threw = false;
    try { action(); } catch (std::exception const&) { threw = true; }
    check(threw, "Expected rejection");
}
Step sampleAt(int tick) { return {1.f / 240, float(tick), 15.f, float(tick), 15.f}; }
int main() {
    try {
        Recorder recorder;
        recorder.tape.levelID = 123;
        recorder.tape.fingerprint = fingerprint("level data");
        recorder.tape.levelName = "A \"quoted\" level";
        recorder.tape.seed = std::numeric_limits<std::uint64_t>::max();
        std::vector<Event> applied;
        std::size_t recordingTick = 0;
        auto recordApply = [&](Input i) { Event e; static_cast<Input&>(e) = i; e.tick = recordingTick; applied.push_back(e); };
        for (int i = 0; i < 960; ++i) {
            recordingTick = i;
            if (i == 1 || i == 55 || i == 731) { recorder.queue({1,true,true}); recorder.queue({1,true,true}); }
            if (i == 22 || i == 600 || i == 800) recorder.queue({1,false,true});
            if (i == 40) { recorder.queue({2,true,false}); recorder.queue({3,true,true}); }
            if (i == 500) { recorder.queue({2,false,false}); recorder.queue({3,false,true}); }
            if (i == 700) { recorder.queue({1,true,false}); recorder.queue({1,false,false}); }
            recorder.step(sampleAt(i), recordApply);
        }
        check(recorder.tape.events.size() == 12, "Duplicate suppression or dual-player inputs failed");
        std::stringstream stream;
        write(stream, recorder.tape);
        auto loaded = read(stream);
        check(loaded.seed == recorder.tape.seed && loaded.levelName == recorder.tape.levelName, "Persistence lost metadata");
        // Identical physics ticks grouped into different render frames: 30/60/120/240/480 FPS.
        for (int fps : {30,60,120,144,240,360,480,1000}) {
            Player player;
            std::vector<Event> replayed;
            double accumulator = 0;
            while (player.tick() < loaded.steps.size()) {
                accumulator += 240.0 / fps;
                while (accumulator >= 1 && player.tick() < loaded.steps.size()) {
                    const auto tick = player.tick();
                    player.step(loaded, sampleAt(int(tick)), 1.f, [&](Input i) {
                        Event e; static_cast<Input&>(e)=i; e.tick=tick; replayed.push_back(e);
                    });
                    accumulator -= 1;
                }
            }
            check(replayed.size() == applied.size(), "Render FPS changed input count");
            for (std::size_t i=0; i<applied.size(); ++i)
                check(replayed[i].tick == applied[i].tick && replayed[i].button == applied[i].button &&
                    replayed[i].down == applied[i].down && replayed[i].player1 == applied[i].player1,
                    "Render FPS changed replay timing or order");
            check(!player.step(loaded, sampleAt(960), 1.f, [](Input){}), "End of tape failed");
            player.reset();
            check(player.tick() == 0 && player.held == std::array<bool,6>{}, "Restart did not reset state");
        }
        Recorder practice;
        practice.queue({1,true,true}); practice.step(sampleAt(0), [](Input){});
        auto checkpoint = practice.checkpoint();
        practice.queue({1,false,true}); practice.step(sampleAt(1), [](Input){});
        practice.queue({2,true,false});
        practice.rewind(checkpoint);
        check(practice.tape.steps.size()==1 && practice.tape.events.size()==1 && practice.held[0] && practice.pending.empty(), "Checkpoint rewind failed");
        practice.queue({1,false,true}); practice.step(sampleAt(1), [](Input){});
        check(practice.tape.events.back().tick==1 && !practice.tape.events.back().down, "Checkpoint continuation failed");
        rejects([&] { practice.rewind({100,{}}); });
        Player player;
        rejects([&] { auto s=sampleAt(0); s.dt=1.f/360; player.step(loaded,s,1.f,[](Input){}); });
        rejects([&] { auto s=sampleAt(0); s.x2+=5; player.step(loaded,s,1.f,[](Input){}); });
        check(player.tick()==0, "Rejected step consumed input");
        Player relaxed;
        std::vector<Input> relaxedInputs;
        for(size_t tick=0;tick<loaded.steps.size();++tick) {
            auto actual=loaded.steps[tick];actual.x1+=2.5f;actual.y2-=4.f;
            check(relaxed.step(loaded,actual,5.f,[&](Input in){relaxedInputs.push_back(in);}),"Minor drift stopped relaxed playback");
        }
        check(relaxed.tick()==loaded.steps.size() && relaxedInputs.size()==loaded.events.size(),"Relaxed playback lost ticks or inputs");
        for(size_t i=0;i<relaxedInputs.size();++i)
            check(key(relaxedInputs[i])==key(loaded.events[i]) && relaxedInputs[i].down==loaded.events[i].down,"Relaxed playback changed input order");
        relaxed.reset();
        rejects([&]{auto actual=loaded.steps[0];actual.x1+=6.f;relaxed.step(loaded,actual,5.f,[](Input){});});
        rejects([&]{auto actual=loaded.steps[0];actual.dt=1.f/360;relaxed.step(loaded,actual,5.f,[](Input){});});
        check(relaxed.tick()==0,"Excessive drift or incompatible timing advanced relaxed playback");
        Player single;
        auto dormant=loaded.steps[0];dormant.x2=11931.99f;dormant.y2=1580.8f;
        check(single.step(loaded,dormant,5.f,[](Input){},false),"Inactive player reset stopped single-player playback");
        single.reset();
        rejects([&]{single.step(loaded,dormant,5.f,[](Input){},true);});
        check(single.tick()==0,"Dual mode ignored active player 2 mismatch");
        auto wrongP1=dormant;wrongP1.x1+=6.f;
        rejects([&]{single.step(loaded,wrongP1,5.f,[](Input){},false);});
        auto wrongTiming=dormant;wrongTiming.dt=1.f/360;
        rejects([&]{single.step(loaded,wrongTiming,5.f,[](Input){},false);});
        for (auto data : {"BAD 1", "PULSE_MACRO 2", "PULSE_MACRO 1\n1 0 0 0 \"x\"\n999999999 0", "PULSE_MACRO 1\n1 0 0 0 \"x\"\n1 0\n"})
            rejects([&] { std::istringstream input(data); read(input); });
        rejects([&] { auto bad=loaded; bad.events[1].tick=0; std::ostringstream output; write(output,bad); });
        rejects([&] { auto bad=loaded; bad.steps[0].dt=std::numeric_limits<float>::quiet_NaN(); validate(bad); });
        rejects([&] { auto bad=loaded; bad.events[0].button=4; validate(bad); });
        rejects([&] { std::stringstream output; write(output,loaded); output<<"junk"; read(output); });
        // Auto-repair helpers: preserve same-channel order, search recent jump
        // transitions newest-first, and pick the center of the widest passing
        // full-route offset region.
        Tape repair=loaded;
        auto target=repair.events.size()-1;
        check(canShiftEvent(repair,target,-1),"Valid repair shift was rejected");
        auto shifted=shiftedEvents(repair,target,-1);
        check(shifted.size()==repair.events.size(),"Repair shift changed event count");
        for(std::size_t i=1;i<shifted.size();++i)check(shifted[i-1].tick<=shifted[i].tick,"Repair events were not sorted");
        auto candidates=repairEventCandidates(repair,repair.steps.size()-1,repair.events[target].player1,4,repair.steps.size());
        check(!candidates.empty(),"Repair candidate scan found no recent jump transition");
        check(candidates.front()==target || repair.events[candidates.front()].tick>=repair.events[target].tick,"Repair candidates were not newest-first");
        check(robustPassingCenter({-5,-4,-3,2,3}).value()==-4,"Repair did not choose widest passing region center");
        check(robustPassingCenter({-2,-1,1,2}).value()==-1,"Repair center tie did not prefer original timing");
        check(!robustPassingCenter({}),"Empty repair passing set returned a frame");
        std::cout << "PASS: persistence, 8 render rates, dual/platformer inputs, same-tick ordering, checkpoint rewind, restart, desync/timing rejection, auto-repair helpers, malformed files\n";
        return 0;
    } catch (std::exception const& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
