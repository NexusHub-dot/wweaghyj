#include "Core.hpp"
#include <iostream>
#include <cstdlib>
#include <unordered_map>
using namespace fwl;
void require(bool ok, char const* name) { if (!ok) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); } }
int main() {
    std::unordered_map<std::string,Run> saved;
    Run linked;linked.pulseInputs=true;linked.macroRevision="take-1";
    linked.inputs={{82,0,true},{123,0,false},{150,0,true},{200,0,false},{250,0,true},{300,0,false}};
    saved[pulseSlotKey("level-a",1)]=linked;
    auto readSaved=[&](std::string const& key,std::string&)->std::optional<Run>{
        auto found=saved.find(key);return found==saved.end()?std::nullopt:std::optional<Run>(found->second);
    };
    std::string selectionError;
    auto reopened=loadSelectedReference("level-a",1,"take-1",readSaved,selectionError);
    require(reopened && reopened->inputs.size()==6 && selectionError.empty(),"Reopened level loads all inputs from selected macro slot without standalone file");
    require(!loadSelectedReference("level-a",2,"take-2",readSaved,selectionError),"Changing slot does not reuse another slot's reference");
    require(!loadSelectedReference("level-a",1,"replacement",readSaved,selectionError),"Replacing macro rejects stale reference");
    require(!loadSelectedReference("level-a",1,"",readSaved,selectionError),"Deleted macro cannot select its old linked reference");
    saved["level-b"]=Run{};
    require(loadSelectedReference("level-b",1,"",readSaved,selectionError).has_value(),"Standalone reference remains available without a selected macro");
    require(!loadSelectedReference("level-b",1,"other-take",readSaved,selectionError),"Missing linked reference never silently selects standalone take");
    require(compatibleEnvironment("GD;alan.pulse_macro@v1.3.0;other@v1")==compatibleEnvironment("GD;alan.pulse_macro@v1.3.1;other@v1"),"UI update retains schema-compatible references");
    require(compatibleEnvironment("GD;alan.pulse_macro@v1.3.1;other@v1")!=compatibleEnvironment("GD;alan.pulse_macro@v1.3.1;other@v2"),"Other mod version changes still reject references");
    require(pulseSlotKey("level-a",1)!=pulseSlotKey("level-a",2),"Pulse slots have separate references");
    require(pulseSlotKey("level-a",1)!=pulseSlotKey("level-b",1),"Different levels have separate linked references");
    // Regression: outside dual mode GD may reset dormant P2 to a completely
    // different placeholder position/velocity. That must not invalidate a
    // perfectly deterministic P1 replay. In dual mode the same P2 mismatch
    // must still be rejected.
    Pose expectedPose; expectedPose.values={100,200,3,11931.9863,777,25};
    Pose singleActual=expectedPose; singleActual.values[3]=0;singleActual.values[4]=0;singleActual.values[5]=0;singleActual.dual=false;
    require(sameValidationPose(expectedPose,singleActual),"Dormant P2 mismatch ignored outside dual mode for legacy reference");
    Pose dualActual=singleActual;dualActual.dual=true;
    require(!sameValidationPose(expectedPose,dualActual),"P2 mismatch still rejects active dual replay");
    Pose p1Bad=singleActual;p1Bad.values[0]+=0.2;
    require(!sameValidationPose(expectedPose,p1Bad),"P1 mismatch is never ignored");
    Pose recordedDual=expectedPose;recordedDual.dual=true;Pose replaySingle=expectedPose;replaySingle.dual=false;
    require(!sameValidationPose(recordedDual,replaySingle),"Recorded dual-state mismatch is rejected");
    Run r; r.endTick = 2400;
    r.inputs = {{240,0,true},{480,0,false},{500,1,true}};
    require(!canShift(r, 0, -241), "negative frame forbidden");
    require(!canShift(r, 0, 240), "press may not cross release");
    require(canShift(r, 2, -30), "independent dual channel may reorder");
    Run next;next.endTick=1379;next.completed=true;
    next.inputs={{80,0,true},{115,0,false},{160,1,true},{268,0,true},{400,0,false},{436,0,true}};
    require(trialEndpoint(next,0,true)==268,"Press endpoint is next press, not release or other player");
    require(trialEndpoint(next,1,true)==268,"Release endpoint is next press for same player");
    require(trialEndpoint(next,3,true)==436,"Later input has its own next-click endpoint");
    require(trialEndpoint(next,5,true)==1379,"Last click falls back to reference end");
    require(trialEndpoint(next,0,false)==1379,"Full-macro mode retains entire endpoint");
    require(!needsCompletion(next,268)&&needsCompletion(next,1379),"Local endpoint does not require level completion");
    next.inputs[3].wave=true;next.inputs[4].wave=true;
    require(trialEndpoint(next,3,true)==400,"Wave press stops at release, its next direction change");
    require(trialEndpoint(next,4,true)==436,"Wave release stops at next press");
    require(trialEndpoint(next,0,true)==268,"Non-wave click keeps next-press semantics");
    require(trialEndpoint(next,3,false)==1379,"Full-macro wave mode retains full endpoint");
    // Observed regression: first wave press 268, release 400, next press 436.
    // Shift -1 died at 412, after the control changed: not a failure of the
    // 268->400 wave segment. Shift +1 died at 295 and remains a real failure.
    require(412>=trialEndpoint(next,3,true),"Post-release death cannot narrow preceding wave segment");
    require(295<trialEndpoint(next,3,true),"Death within wave segment still limits its window");
    // Oracle: small timing changes pass the nearby obstacle, but die much
    // later with the untouched suffix. Only the full-route metric is 1f.
    auto measure=[&](bool local){
        Search probe(1);
        while(!probe.finished()) {
            int death=(probe.offset>=-2&&probe.offset<=3)?700:120;
            probe.accept(probe.baseline() || death>=trialEndpoint(next,0,local),death);
        }
        return probe.results[0];
    };
    auto local=measure(true),full=measure(false);
    require(local.width()==6 && full.width()==1,"Later failure must not shrink next-click window to one frame");
    require(local.earlyFailureTick==120 && local.lateFailureTick==120,"Window retains limiting failure locations");
    require(repeatableFailure(false,120,120),"Identical slow boundary replay accepted");
    auto stableCheck=resolveFailureConfirmation(false,120,120);
    require(stableCheck.stable && !stableCheck.acceptedPass && stableCheck.acceptedFailureTick==120,"Stable boundary confirmation keeps failure");
    auto becamePass=resolveFailureConfirmation(true,120,-1);
    require(!becamePass.stable && !becamePass.acceptedPass && becamePass.acceptedFailureTick==120,"Failure becoming success continues conservatively at first failure");
    auto movedDeath=resolveFailureConfirmation(false,120,121);
    require(!movedDeath.stable && !movedDeath.acceptedPass && movedDeath.acceptedFailureTick==120,"Moved failure tick continues conservatively and flags uncertainty");
    Search search(3);
    while (!search.finished()) {
        bool ok = search.baseline() || (search.index == 0 ? search.offset >= -2 && search.offset <= 3 : search.index == 1 ? false : true);
        search.accept(ok);
    }
    require(search.results[0].width() == 6, "inclusive asymmetric six-frame window");
    require(search.results[1].width() == 1, "true one-frame input");
    require(search.results[2].overflow && search.results[2].width() == 21, "wide window censored above 20");
    Search bad(1); bad.accept(false); require(bad.stage == Search::Stage::Failed, "failed opening baseline stops analysis");
    Search changed(1); changed.accept(true); changed.accept(false); changed.accept(false); changed.accept(false);
    require(changed.stage == Search::Stage::Done && changed.finalValidationWarning && changed.results[0].measured,
        "final baseline failure preserves completed measurements with an explicit warning");
    Search lateDirectFail(1); lateDirectFail.accept(true); lateDirectFail.accept(false); lateDirectFail.accept(false);
    require(lateDirectFail.stage == Search::Stage::FinalBaseline, "single input reaches final baseline after both failed sides");
    lateDirectFail.fail("post-measurement pose mismatch");
    require(lateDirectFail.stage == Search::Stage::Done && lateDirectFail.finalValidationWarning,
        "direct final-baseline validation errors are late-save warnings, not data loss");
    r.windows = search.results;
    auto counts = count(r, r.endTick, true);
    require(counts.exact[6] == 1 && counts.exact[1] == 1 && counts.above == 1, "exact and overflow histogram");
    require(count(r,2400,false).releases == 0, "release filtering");
    require(count(r,300,true).presses == 1, "timeline counter");
    require(count(r,2400,true,2).above == 1, "dual filtering");
    // Independent forward formula is the oracle for the precision calculation.
    double L = 100, reach = 1, time = 0;
    for (size_t i = 0; i < r.inputs.size(); ++i) {
        double p = std::erf(r.windows[i].width()/240.0*L/(2*std::sqrt(2.0)));
        time += r.inputs[i].tick/240.0*reach*(1-p); reach *= p;
    }
    time += r.endTick/240.0*reach;
    require(std::abs(expectedSeconds(r,L,r.endTick)-time/reach) < 1e-7, "precision matches probability model");
    auto p = precision(r,r.endTick);
    require(p && std::abs(expectedSeconds(r,*p,r.endTick)-86400) < 0.001, "24-hour precision root");
    r.windows[0].measured = false;
    require(!precision(r,r.endTick), "unknown windows cannot produce a score");
    r.windows[0].measured=true;
    r.nextClick=true;
    require(!precision(r,r.endTick),"Next-click windows cannot claim full-run precision");
    std::cout << "Analysis checks passed, including next-click vs full-route scope and conservative handling of unstable boundary double-checks.\n";
}
