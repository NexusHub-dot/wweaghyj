#include "replay.hpp"
#include "Core.hpp"
#include "Lifecycle.hpp"
#include <iostream>
void check(bool yes,char const* message){if(!yes)throw std::runtime_error(message);}
struct Simulation {
    float x=0,y=0,vy=0;bool held=false;
    void input(pulse::Input i){if(i.button==1&&i.player1){if(i.down&&!held&&y==0)vy=12;held=i.down;}}
    pulse::Step sample()const{return {1.f/240,x,y,0,0};}
    void advance(){x+=2.f/240;vy-=25.f/240;y=std::max(0.f,y+vy/240);if(y==0)vy=0;}
};
int main(){
    try{
        pulse::Recorder macro;macro.tape.fixedUpdates=true;
        fwl::Run reference;reference.pulseInputs=true;
        Simulation model;
        auto step=[&](bool down){
            auto tick=macro.tape.steps.size();macro.queue({1,down,true});
            macro.step(model.sample(),[&](pulse::Input i){reference.inputs.push_back({int(tick),0,i.down,model.x,model.y});model.input(i);});
            if(tick%60==0){fwl::Pose p;p.tick=int(tick);p.values={model.x,model.y,model.vy,0,0,0};reference.poses.push_back(p);}
            model.advance();reference.endTick=int(macro.tape.steps.size());
        };
        for(int i=0;i<120;++i)step(i>=20&&i<40);
        auto macroCP=macro.checkpoint();auto modelCP=model;
        // Native pause can emit exit BEFORE setting its paused flag. Simulate
        // that ordering and repeated pause/resume, preserving the same take.
        int saves=0;
        auto exitLayer=[&](bool active,bool paused){
            if(!pulse_lifecycle::preserveOnExit(active,paused)){++saves;macro={};reference={};}
        };
        for(int pause=0;pause<10;++pause){
            {pulse_lifecycle::PauseScope scope;exitLayer(true,false);}
            exitLayer(true,true);
            check(saves==0 && macro.tape.steps.size()==120 && reference.inputs.size()==macro.tape.events.size(),"Pause finalized or lost the active take");
        }
        check(!pulse_lifecycle::preserveOnExit(true,false),"Resume retained a stale pause transition");
        check(!pulse_lifecycle::preserveOnExit(false,true),"Actual level departure mistaken for pause");
        fwl::ReferenceCheckpoint frameCP;frameCP.inputs=reference.inputs.size();frameCP.poses=reference.poses.size();frameCP.physicsTick=120;frameCP.recordedThrough=120;frameCP.held={macro.held[0],false};
        // Failed branches may contain presses, releases, sampled poses, and
        // pending input at death. Each retry must restore the same prefix.
        for(int retry=0;retry<100;++retry){
            for(int i=0;i<75;++i)step(i>=retry%15&&i<40);
            macro.queue({1,true,false});macro.tape.completed=true;reference.completed=true;
            macro.rewind(macroCP);fwl::rewindReference(reference,frameCP);model=modelCP;
            check(macro.tape.steps.size()==120&&reference.endTick==120,"Death retry changed checkpoint timeline");
            check(macro.pending.empty()&&!macro.tape.completed&&!reference.completed,"Death retry retained failed-branch data");
            check(macro.tape.events.size()==reference.inputs.size()&&reference.inputs.size()==frameCP.inputs,"Macro/reference inputs diverged after retry");
        }
        for(int i=120;i<480;++i)step((i>=210&&i<230)||(i>=410&&i<430));
        check(reference.inputs.size()==macro.tape.events.size(),"Stitched input counts differ");
        for(size_t i=0;i<reference.inputs.size();++i)check(size_t(reference.inputs[i].tick)==macro.tape.events[i].tick&&reference.inputs[i].down==macro.tape.events[i].down,"Stitched input timing differs");
        // Replay the stitched result from zero with changing speed; no snapshot
        // or position correction is used during playback.
        pulse::Player player;pulse::FixedUpdateClock clock;Simulation replay;
        for(int frame=0;frame<100000&&player.tick()<macro.tape.steps.size();++frame){
            double speed=frame%120<60?.25:2.;clock.add(float(speed/144));
            while(player.tick()<macro.tape.steps.size()&&clock.take()){
                check(player.step(macro.tape,replay.sample(),.000001f,[&](pulse::Input i){replay.input(i);}),"Stitched replay ended early");replay.advance();
            }
        }
        check(player.tick()==480&&pulse::samePosition(model.sample(),replay.sample(),0),"Stitched replay did not reproduce its final state");
        // An older checkpoint can remove a later successful branch as well.
        macro.rewind(macroCP);fwl::rewindReference(reference,frameCP);
        check(macro.tape.steps.size()==120&&reference.inputs.size()==frameCP.inputs,"Older checkpoint did not prune future inputs");
        fwl::Search disconnected(1);
        while(!disconnected.finished())disconnected.accept(disconnected.baseline()||disconnected.offset==-4||disconnected.offset==-3||disconnected.offset==-1||disconnected.offset==1);
        check(disconnected.results[0].width()==3,"Disconnected alternate solutions inflated window width");
        check(std::abs(disconnected.results[0].nominalMilliseconds()-12.5)<1e-9,"Nominal millisecond conversion incorrect");
        std::cout<<"PASS: 100 failed practice branches, matching macro/reference rewind, pending-input cleanup, stitched replay at changing speed, older checkpoint restore, contiguous windows, nominal milliseconds\n";
        return 0;
    }catch(std::exception const& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
