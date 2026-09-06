#include "AnalysisCache.hpp"
#include <cstdlib>
#include <iostream>
#include <random>
using namespace fwl;
void require(bool ok,char const* message) {if(!ok){std::cerr<<"FAIL: "<<message<<'\n';std::exit(1);}}
int main() {
    std::vector<AnalysisTimeline> points(4);
    for(int i=0;i<4;++i)points[i].tick=(i+1)*240;
    require(selectAnalysisCheckpoint(points,751)==1,"latest safe checkpoint");
    require(selectAnalysisCheckpoint(points,752)==2,"inclusive safety boundary");
    require(!selectAnalysisCheckpoint(points,250),"no safe checkpoint falls back to start");
    require(!selectAnalysisCheckpoint(points,0),"first input falls back to start");
    require(selectAnalysisCheckpoint(points,752,40)==1,"larger maxWindow excludes unsafe checkpoint");
    require(selectAnalysisCheckpoint(points,752,20,40)==1,"larger safety excludes unsafe checkpoint");
    require(selectAnalysisCheckpoint(points,760)==selectAnalysisCheckpoint(points,761),"close inputs reuse snapshot");
    points[2].usable=false;
    require(selectAnalysisCheckpoint(points,760)==1,"unusable checkpoint falls back to earlier usable one");
    points[0].usable=points[1].usable=points[3].usable=false;
    require(!selectAnalysisCheckpoint(points,2000),"all unusable falls back to start");
    for(int duration: {1,240,7200,30000,3456000}) {
        int spacing=checkpointSpacing(duration);
        require((duration-1)/spacing<=checkpointCap,"spacing enforces snapshot cap");
        require(spacing>=180,"default spacing remains at least 0.75 seconds");
    }
    // Candidate-aware planning should keep a universal safe anchor for every
    // input when the cache budget allows it, then spend spare slots on closer
    // candidate-specific anchors.
    Run planned;planned.endTick=7200;
    for(int t=80;t<7100;t+=35)planned.inputs.push_back({t,0,true});
    auto plan=inputAwareCheckpointPlan(planned);
    require(!plan.empty() && plan.size()<=checkpointCap,"input-aware plan obeys cap");
    require(std::is_sorted(plan.begin(),plan.end()),"input-aware plan is sorted");
    for(int tick:plan)require(tick>0 && tick<planned.endTick,"planned checkpoint is inside reference");
    int worstUniversalDistance=0;
    for(auto const& in:planned.inputs) {
        int safe=in.tick-maxWindow-checkpointSafetyTicks;
        if(safe<=0)continue;
        auto it=std::upper_bound(plan.begin(),plan.end(),safe);
        require(it!=plan.begin(),"every eligible dense input has a prior universal-safe checkpoint");
        --it;worstUniversalDistance=std::max(worstUniversalDistance,safe-*it);
    }
    require(worstUniversalDistance<80,"larger dense cache stays close to universal boundaries");
    auto limitedPlan=inputAwareCheckpointPlan(planned,128);
    require(limitedPlan.size()<=128,"user-lowered checkpoint limit is obeyed");
    require(std::is_sorted(limitedPlan.begin(),limitedPlan.end()),"limited plan stays sorted");
    require(checkpointSpacing(planned.endTick,180,128)>=checkpointSpacing(planned.endTick,180,256),
        "smaller checkpoint budget never requests denser periodic spacing");

    Run sparse;sparse.endTick=2000;sparse.inputs={{100,0,true},{1000,0,true},{1900,0,true}};
    auto sparsePlan=inputAwareCheckpointPlan(sparse);
    // Universal anchors at T-32 and close -1/late anchors at T-13 must both be
    // present; additional mid-early tiers are allowed when there is cache room.
    for(int t:{100,1000,1900}) {
        require(std::binary_search(sparsePlan.begin(),sparsePlan.end(),t-maxWindow-checkpointSafetyTicks),
            "sparse plan keeps universal anchor");
        require(std::binary_search(sparsePlan.begin(),sparsePlan.end(),t-1-checkpointSafetyTicks),
            "sparse plan adds close candidate anchor");
    }
    require(sparsePlan.size()>3,"sparse plan adds more than one checkpoint per input");

    // Candidate selection must never restore after the changed event. Late
    // offsets can use a much closer checkpoint; earlier offsets progressively
    // require earlier snapshots.
    std::vector<AnalysisTimeline> candidatePoints(6);
    for(size_t i=0;i<candidatePoints.size();++i)candidatePoints[i].tick=60+int(i)*5; // 60..85
    Run candidateRun;candidateRun.endTick=200;candidateRun.inputs={{100,0,true}};
    auto late=selectCandidateCheckpoint(candidatePoints,candidateRun,0,+6);
    auto early1=selectCandidateCheckpoint(candidatePoints,candidateRun,0,-1);
    auto early10=selectCandidateCheckpoint(candidatePoints,candidateRun,0,-10);
    require(late && candidatePoints[*late].tick==85,"late candidate uses checkpoint close to original input");
    require(early1 && candidatePoints[*early1].tick==85,"-1 candidate uses latest checkpoint before shifted input");
    require(early10 && candidatePoints[*early10].tick==75,"deeper early candidate selects an earlier safe checkpoint");
    require(candidatePoints[*late].tick<=candidateRestoreBoundary(candidateRun,0,+6),"late restore boundary is safe");
    require(candidatePoints[*early10].tick<=candidateRestoreBoundary(candidateRun,0,-10),"early restore boundary is safe");


    Run held;held.inputs={{800,0,true},{820,1,true},{900,0,false},{920,1,false}};
    held.poses={{840},{850},{900}};
    auto cp=timelineAt(held,850);
    require(cp.inputCursor==2 && cp.poseCursor==1,"cursors address first unconsumed step");
    require(cp.jumpHeld[0] && cp.jumpHeld[1],"held input across checkpoint for both players");
    require(!timelineAt(held,901).jumpHeld[0] && timelineAt(held,901).jumpHeld[1],"independent dual held state");
    require(timelineAt(held,900).jumpHeld[0],"release at checkpoint tick is not consumed early");
    // Every merge (including simultaneous cross-player inputs) must exactly match
    // the old stable-sort implementation, from both zero and a safe checkpoint.
    std::mt19937 rng(47);
    for(int iteration=0;iteration<100;++iteration) {
        Run run;run.endTick=2000;
        for(int j=0;j<100;++j)run.inputs.push_back({int(rng()%1800),int(rng()%2),bool(rng()%2),float(j)});
        std::stable_sort(run.inputs.begin(),run.inputs.end(),[](auto const&a,auto const&b){return a.tick<b.tick;});
        for(size_t target=0;target<run.inputs.size();++target)for(int offset=-20;offset<=20;++offset) {
            if(!canShift(run,target,offset))continue;
            auto expected=shifted(run,target,offset);
            int start=std::max(0,candidateRestoreBoundary(run,target,offset));
            auto metadata=timelineAt(run,start);CandidatePlayback playback;
            playback.reset(metadata.inputCursor,target,offset);
            size_t i=0;while(i<expected.size() && expected[i].tick<start)++i;
            while(auto next=playback.next(run.inputs)) {
                require(i<expected.size(),"overlay size");
                require(playback.tick(run.inputs,*next)==expected[i].tick && run.inputs[*next].x==expected[i].x,"overlay matches stable sort from checkpoint");
                playback.consume(*next);++i;
            }
            require(i==expected.size(),"overlay delivers all remaining events exactly once");
        }
    }
    // Search stops each side at its first failure. Early failure switches to
    // +frames; late failure immediately advances to the next input. This is the
    // intended no-wasted-trials behavior for a contiguous frame window.
    Search search(2);search.accept(true);
    require(search.stage==Search::Stage::Early && search.offset==-1,"baseline starts early search");
    search.accept(true);require(search.offset==-2,"successful early frame expands outward");
    search.accept(false,45);
    require(search.stage==Search::Stage::Late && search.offset==1,"first early failure stops negative search");
    search.accept(true);require(search.offset==2,"successful late frame expands outward");
    search.accept(false,55);
    require(search.index==1 && search.stage==Search::Stage::Early && search.offset==-1,"first late failure advances to next input");
    search.accept(false,60);require(search.stage==Search::Stage::Late,"second input early failure switches sides");
    search.accept(false,61);require(search.stage==Search::Stage::FinalBaseline,"second input late failure advances to final baseline");
    search.accept(true);require(search.stage==Search::Stage::Done,"only final baseline completes analysis");

    auto changed=resolveFailureConfirmation(true,45,-1);
    require(!changed.stable && !changed.acceptedPass,"unstable failure remains conservative");
    std::cout<<"Checkpoint selection, timeline, overlay and search tests passed\n";
}
