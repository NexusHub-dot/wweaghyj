#pragma once
#include "Core.hpp"
#include "AnalysisCache.hpp"
#include <Geode/Geode.hpp>
#include <chrono>
#include <memory>
#include <unordered_map>
namespace fwl {
class Hud;
enum class Mode { Idle, Recording, Analyzing };
struct AnalysisCheckpoint : AnalysisTimeline {
    geode::Ref<CheckpointObject> native;
    std::vector<Pose> validation;
};
struct Session {
    PlayLayer* layer=nullptr;
    Hud* hud=nullptr;
    Mode mode=Mode::Idle;
    Run recording;
    std::optional<Run> reference;
    std::unique_ptr<Search> search;
    std::vector<Input> playback;
    size_t cursor=0, poseCursor=0;
    std::array<bool,2> held{};
    CandidatePlayback candidate;
    std::array<bool,2> playbackHeld{};
    std::vector<AnalysisCheckpoint> analysisCheckpoints;
    std::vector<int> checkpointPlan;
    size_t checkpointPlanCursor=0;
    std::vector<Pose> canonicalPoses;
    geode::Ref<cocos2d::CCArray> userCheckpoints;
    geode::Ref<CheckpointObject> userCurrentCheckpoint;
    bool isolatedCheckpoints=false, cacheEnabled=false, inputAwareCache=true, verifyingCheckpoint=false;
    bool forceFullRestart=false;
    std::optional<size_t> activeCheckpoint;
    size_t verificationCursor=0;
    int cacheSpacing=180;
    size_t checkpointLimit=checkpointCap;
    uint64_t physicsUpdates=0, checkpointRestores=0, fullRestarts=0;
    int checkpointFallbacks=0, trialStartTick=0;
    std::chrono::steady_clock::time_point analysisStarted{};
    bool injecting=false, resetting=false, insideStep=false;
    bool failedTrial=false, completedTrial=false, needReset=false;
    bool oldTest=false, oldPractice=false, locked=false;
    bool oldCBS=false, oldCOS=false;
    bool pulseLinked=false, savedReference=false;
    std::unordered_map<CheckpointObject*,ReferenceCheckpoint> checkpoints;
    std::uint64_t nextCheckpoint=0,physicsRevision=0;
    int physicsTick=0;
    bool invalidRecording=false;
    int lastTick=-1, lastPoseTick=-1, trialUpdates=0;
    int recordedThrough=0;
    bool analyzeNextClick=true;
    int trialEnd=0,failureTick=-1,firstFailureTick=-1;
    bool confirmingFailure=false;
    bool analysisUncertain=false;
    int unstableChecks=0;
    std::string status="F4: controls | F3: hide";
    std::string reason;
    double precisionValue=-1;
    int precisionTick=-1;
};
Session& session();
void showControls();
void startRecording();
void stopRecording(bool completed=false);
void startAnalysis();
void stopAnalysis(std::string message);
void clearAnalysisCache(bool restoreUser=true);
std::string analysisProgress();
std::string fingerprint(PlayLayer* pl);
std::string environment();
}
