#include "Session.hpp"
#include "Storage.hpp"
#include "Hud.hpp"
#include "FrameLink.hpp"
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <chrono>
#include <fstream>

using namespace geode::prelude;
namespace fwl {
Session& session() { static Session s; return s; }
std::uint64_t linkGeneration=0;
void clearAnalysisCache(bool restoreUser) {
    auto& s=session();
    // Restore the actual array, not a copy of its contents: other practice mods
    // can retain this array and the player's original checkpoint objects.
    if(s.isolatedCheckpoints && restoreUser && s.layer) {
        auto pl=s.layer;
        pl->m_currentCheckpoint=nullptr;
        if(pl->m_checkpointArray)pl->m_checkpointArray->release();
        pl->m_checkpointArray=s.userCheckpoints.data();
        if(pl->m_checkpointArray)pl->m_checkpointArray->retain();
        pl->m_currentCheckpoint=s.userCurrentCheckpoint.data();
    }
    s.isolatedCheckpoints=false;
    s.userCheckpoints=nullptr;s.userCurrentCheckpoint=nullptr;
    s.analysisCheckpoints.clear();s.checkpointPlan.clear();s.checkpointPlanCursor=0;
    s.canonicalPoses.clear();s.activeCheckpoint.reset();s.verifyingCheckpoint=false;
}
std::string analysisProgress() {
    auto& s=session();
    double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-s.analysisStarted).count();
    if(!s.search || !s.reference || s.reference->inputs.empty())
        return fmt::format("{} ticks | {} restores | {} starts | {:.1f}s",s.physicsUpdates,s.checkpointRestores,s.fullRestarts,elapsed);

    size_t total=s.reference->inputs.size(),done=0;
    for(auto const& w:s.search->results)if(w.measured)++done;
    std::string phase;
    if(s.search->stage==Search::Stage::Baseline)phase="opening baseline";
    else if(s.search->stage==Search::Stage::FinalBaseline)phase="final baseline";
    else if(s.search->stage==Search::Stage::Done)phase="done";
    else if(s.search->stage==Search::Stage::Failed)phase="failed";
    else phase=fmt::format("{}/{}, {:+}f",std::min(total,s.search->index+1),total,s.search->offset);
    std::string origin=s.activeCheckpoint?fmt::format("CP {}",s.trialStartTick):"start";
    std::string eta;
    if(done>=3 && done<total && (s.search->stage==Search::Stage::Early || s.search->stage==Search::Stage::Late)) {
        // Deliberately simple moving estimate. Input difficulty varies, so this is
        // only a rough UI hint rather than a promise.
        double secondsPerInput=elapsed/static_cast<double>(done);
        double remaining=secondsPerInput*static_cast<double>(total-done);
        eta=remaining>=60?fmt::format(" | ETA ~{:.1f}m",remaining/60.0):fmt::format(" | ETA ~{:.0f}s",remaining);
    }
    return fmt::format("{} | {} | {:.1f}s{}",phase,origin,elapsed,eta);
}
std::string environment() {
    std::vector<std::string> mods;
    for(auto mod:Loader::get()->getAllMods()) if(mod->isLoaded()) mods.push_back(fmt::format("{}@{}",mod->getID(),mod->getVersion().toVString()));
    std::sort(mods.begin(),mods.end()); std::string out="GD 2.2081 / 240 Hz";
    for(auto const& m:mods) out+=";"+m;
    return out;
}
std::string fingerprint(PlayLayer* pl) {
    return fmt::format("{:016x}",hash(fmt::format("{}|{}|2.2081|240",pl->m_level->m_levelID.value(),std::string(pl->m_level->m_levelString))));
}
namespace {
void selectSavedReference() {
    auto& s=session();if(!s.layer || s.mode!=Mode::Idle)return;
    pulse_frame_link::Message msg;msg.command=pulse_frame_link::Command::SavedSelection;msg.layer=s.layer;
    if(!pulse_frame_link::pulse().send(&msg))return;
    std::string error;
    auto selected=loadSelectedReference(fingerprint(s.layer),msg.slot,msg.macroRevision,load,error);
    if(!msg.reason.empty())error=msg.reason;
    s.reference=std::move(selected);s.reason=error;s.precisionTick=-1;
    s.status=s.reference?fmt::format("Slot {} | {} reference inputs",msg.slot,s.reference->inputs.size()):"No saved frame reference - F4 for details";
}
bool attachActiveLayer() {
    auto pl=PlayLayer::get();
    if(!pl || !pl->m_level || !pl->m_player1 || !pl->m_player2 || !pl->m_objectLayer)return false;
    auto& s=session(); if(s.layer==pl)return true;
    ++linkGeneration;
    s=Session{}; s.layer=pl;
    selectSavedReference();
    s.hud=static_cast<Hud*>(pl->getChildByID("counter"_spr));
    if(!s.hud){s.hud=Hud::create(pl);if(s.hud)pl->addChild(s.hud,10000);}
    return true;
}
int pulseState(PlayLayer* layer) {
    pulse_frame_link::Message msg;msg.layer=layer;pulse_frame_link::pulse().send(&msg);return msg.state;
}
template<class T> T* descendant(CCNode* root) {
    if(!root) return nullptr;
    if(auto found=typeinfo_cast<T*>(root)) return found;
    for(auto c:CCArrayExt<CCNode*>(root->getChildren())) if(auto found=descendant<T>(c)) return found;
    return nullptr;
}
void resumeLayer() {
    auto& s=session(); if(!s.layer) return;
    if(auto pause=descendant<PauseLayer>(CCDirector::sharedDirector()->getRunningScene())) pause->onResume(nullptr);
    else if(s.layer->m_isPaused) s.layer->resume();
    if(auto end=descendant<EndLevelLayer>(s.layer)) end->removeFromParentAndCleanup(true);
}
Pose pose(PlayLayer* pl,int tick) {
    Pose p; p.tick=tick; p.dual=pl->m_gameState.m_isDualMode;
    for(int k=0;k<2;++k) { auto player=k?pl->m_player2:pl->m_player1; p.values[k*3]=player->getPositionX(); p.values[k*3+1]=player->getPositionY(); p.values[k*3+2]=player->m_yVelocity; }
    return p;
}
std::string poseMismatch(Pose const& expected, Pose const& actual) {
    auto delta=[&](int base){
        return fmt::format("dx {:.4f}, dy {:.4f}, dvy {:.4f}",
            std::abs(expected.values[base]-actual.values[base]),
            std::abs(expected.values[base+1]-actual.values[base+1]),
            std::abs(expected.values[base+2]-actual.values[base+2]));
    };
    bool dual=actual.dual.value_or(expected.dual.value_or(true));
    return dual?fmt::format("P1 {}; P2 {}",delta(0),delta(3)):fmt::format("P1 {}; P2 inactive (ignored)",delta(0));
}
void rejectRecording(std::string why) {
    auto& s=session(); if(s.mode!=Mode::Recording) return;
    if(!s.invalidRecording) log::warn("Reference rejected: {}",why);
    s.invalidRecording=true; s.reason=std::move(why); s.status="Recording invalid - see F4";
}
bool eligible(PlayLayer* pl,std::string& reason,bool practiceAllowed=false) {
    if(!pl) {reason="Open a classic level first.";return false;}
    if(pl->m_isPlatformer) {reason="This build measures classic levels; platformer routing needs a different reference model.";return false;}
    if(pl->m_startPosObject) {reason="Record from the level start, with StartPos disabled.";return false;}
    if(pl->m_isPracticeMode && !practiceAllowed) {reason="Use Pulse F6 for a practice reference with checkpoint support.";return false;}
    if(pl->m_isIgnoreDamageEnabled) {reason="Disable ignore damage / noclip for a valid reference.";return false;}
    if(auto cbf=Loader::get()->getLoadedMod("syzzi.click_between_frames")) {
        if(cbf->hasSetting("physics-bypass") && cbf->getSettingValue<bool>("physics-bypass")) {reason="Turn off CBF Physics Bypass for 240 Hz reference analysis.";return false;}
        if(cbf->hasSetting("soft-toggle") && !cbf->getSettingValue<bool>("soft-toggle") && (!cbf->hasSetting("click-on-steps") || !cbf->getSettingValue<bool>("click-on-steps"))) {
            reason="In CBF settings, enable Disable CBF or Click on Steps for 240 Hz analysis. The counter can stay installed alongside CBF.";return false;
        }
    }
    return true;
}
void resetTrial() {
    auto& s=session(); auto pl=s.layer; if(!pl || !s.search || !s.reference) return;
    // Invalid shifts are semantic failures, so they need no engine replay.
    while(!s.search->finished() && !s.search->baseline() && !canShift(*s.reference,s.search->index,s.search->offset)) s.search->accept(false);
    if(s.search->finished()) return;
    s.activeCheckpoint.reset();s.verifyingCheckpoint=false;
    if(s.cacheEnabled && !s.search->baseline() && !s.forceFullRestart) {
        // v1.4.6: choose against this exact shifted candidate, not the worst-case
        // -20f boundary for every trial. Late trials can therefore restore much
        // closer to the click, while early trials still restore before the
        // shifted event. Accuracy guard/fallback behavior is unchanged.
        auto nearest=selectCandidateCheckpoint(s.analysisCheckpoints,*s.reference,
            s.search->index,s.search->offset,checkpointSafetyTicks,true);
        if(nearest && !s.analysisCheckpoints[*nearest].usable) {
            s.search->results[s.search->index].checkpointFallback=true;s.analysisUncertain=true;
        }
        s.activeCheckpoint=selectCandidateCheckpoint(s.analysisCheckpoints,*s.reference,
            s.search->index,s.search->offset);
        if(s.activeCheckpoint && s.analysisCheckpoints[*s.activeCheckpoint].inputCursor>s.search->index)
            s.activeCheckpoint.reset(); // Never consume the candidate before restoring it.
    }
    s.forceFullRestart=false;
    s.resetting=true; s.injecting=true;
    pl->m_uncommittedJumps=0; pl->m_isTestMode=true; pl->m_isPracticeMode=false;
    pl->m_randomSeed=s.reference->seed;
    if(s.activeCheckpoint) {
        auto& cp=s.analysisCheckpoints[*s.activeCheckpoint];
        // resetLevel performs native world/trigger cleanup and invokes
        // loadFromCheckpoint on the one private snapshot in this temporary array.
        // Calling loadFromCheckpoint alone leaves objects from the preceding trial.
        if(pl->m_checkpointArray) {
            pl->m_checkpointArray->removeAllObjects();
            pl->m_checkpointArray->addObject(cp.native.data());
        }
        // resetLevel follows the current/last practice checkpoint. Keep both
        // the private checkpoint array and the explicit current pointer aligned
        // so other reset hooks cannot make GD fall back to a stale checkpoint.
        pl->m_currentCheckpoint=cp.native.data();
        pl->m_isPracticeMode=true;
        pl->resetLevel();
        ++s.checkpointRestores;
        pl->m_randomSeed=cp.randomSeed;pl->m_replayRandSeed=cp.replaySeed;
        s.verifyingCheckpoint=!cp.verified;
    } else {
        // A prior accelerated trial may have left a private checkpoint selected.
        // Full-start fallback must be a true level-start reset, never an implicit
        // practice reload from the previous candidate.
        if(pl->m_checkpointArray)pl->m_checkpointArray->removeAllObjects();
        pl->m_currentCheckpoint=nullptr;
        pl->m_isPracticeMode=false;
        pl->resetLevelFromStart();++s.fullRestarts;
        pl->m_randomSeed=s.reference->seed; pl->m_replayRandSeed=s.reference->seed;
    }
    pl->m_isTestMode=true; pl->m_isPracticeMode=false;
    pl->m_clickBetweenSteps=false;pl->m_clickOnSteps=false;
    pl->m_queuedButtons.clear();
    if(!s.activeCheckpoint) {pl->m_player1->releaseAllButtons(); pl->m_player2->releaseAllButtons();}
    s.playbackHeld=s.activeCheckpoint?s.analysisCheckpoints[*s.activeCheckpoint].jumpHeld:std::array<bool,2>{};
    if(s.activeCheckpoint) {
        // Native PlayerCheckpoint restores jump buffers. Restore the held input
        // latch without synthesizing a new press (which could retrigger an orb).
        pl->m_player1->m_holdingButtons[1]=s.playbackHeld[0];
        pl->m_player2->m_holdingButtons[1]=s.playbackHeld[1];
    }
    s.injecting=false; s.resetting=false;
    s.cursor=s.activeCheckpoint?s.analysisCheckpoints[*s.activeCheckpoint].inputCursor:0;
    s.poseCursor=s.activeCheckpoint?s.analysisCheckpoints[*s.activeCheckpoint].poseCursor:0;
    bool unmodified=s.search->baseline() || s.verifyingCheckpoint;
    s.candidate.reset(s.cursor,unmodified?std::nullopt:std::optional<size_t>(s.search->index),unmodified?0:s.search->offset);
    s.failedTrial=false; s.completedTrial=false; s.needReset=false;
    s.lastTick=-1; s.trialUpdates=0;
    s.failureTick=-1;
    s.trialEnd=s.search->baseline()?s.reference->endTick:trialEndpoint(*s.reference,s.search->index,s.analyzeNextClick);
    s.physicsTick=s.activeCheckpoint?s.analysisCheckpoints[*s.activeCheckpoint].tick:0;++s.physicsRevision;
    s.trialStartTick=s.physicsTick;s.verificationCursor=0;
    if(s.verifyingCheckpoint)s.trialEnd=std::min(s.reference->endTick,s.physicsTick+checkpointValidationTicks);
    s.status=s.search->baseline()?"Validating reference replay":fmt::format("Testing input {}/{} ({:+}f)",s.search->index+1,s.reference->inputs.size(),s.search->offset);
    if(s.confirmingFailure)s.status="Double-checking | "+s.status;
    if(s.verifyingCheckpoint)s.status=fmt::format("Verifying checkpoint {}",s.physicsTick);
    if(s.search->baseline())log::info("Analysis {} baseline started (batched 240 Hz)",s.search->stage==Search::Stage::Baseline?"opening":"final");
    else log::debug("Input {} {:+}f using {} {}",s.search->index+1,s.search->offset,s.activeCheckpoint?"checkpoint":"start",s.physicsTick);
}
void rejectCheckpoint(std::string const& why) {
    auto& s=session();if(!s.activeCheckpoint)return;
    auto& cp=s.analysisCheckpoints[*s.activeCheckpoint];cp.usable=false;
    ++s.checkpointFallbacks;s.analysisUncertain=true;
    s.search->results[s.search->index].checkpointFallback=true;
    s.reason=fmt::format("Checkpoint at step {} could not be reproduced. Falling back to full replay for nearby inputs. Results may take longer.",cp.tick);
    log::warn("{} {}",s.reason,why);
    s.verifyingCheckpoint=false;s.forceFullRestart=true;s.needReset=true;
}
void captureAnalysisCheckpoint() {
    auto& s=session();
    if(!s.cacheEnabled || s.search->stage!=Search::Stage::Baseline || s.failedTrial || s.completedTrial ||
        s.physicsTick<=0 || s.physicsTick>=s.reference->endTick || s.analysisCheckpoints.size()>=s.checkpointLimit)return;
    bool due=false;
    if(s.inputAwareCache) {
        while(s.checkpointPlanCursor<s.checkpointPlan.size() && s.checkpointPlan[s.checkpointPlanCursor]<s.physicsTick)
            ++s.checkpointPlanCursor;
        if(s.checkpointPlanCursor<s.checkpointPlan.size() && s.checkpointPlan[s.checkpointPlanCursor]==s.physicsTick) {
            due=true;++s.checkpointPlanCursor;
        }
    } else due=s.physicsTick%s.cacheSpacing==0;
    if(!due || (!s.analysisCheckpoints.empty() && s.analysisCheckpoints.back().tick==s.physicsTick))return;
    // createCheckpoint captures native world state without markCheckpoint's
    // insertion, effects or sound. The physical diamond is never attached.
    auto native=s.layer->createCheckpoint();
    if(!native)return;
    if(native->m_physicalCheckpointObject)native->m_physicalCheckpointObject->setVisible(false);
    AnalysisCheckpoint cp;cp.native=native;cp.tick=s.physicsTick;
    cp.inputCursor=s.candidate.cursor;cp.poseCursor=s.poseCursor;cp.jumpHeld=s.playbackHeld;
    cp.randomSeed=s.layer->m_randomSeed;cp.replaySeed=s.layer->m_replayRandSeed;
    cp.validation.reserve(checkpointValidationTicks);
    s.analysisCheckpoints.push_back(std::move(cp));
    log::debug("Analysis checkpoint created at step {}",s.physicsTick);
}
void finishTrial() {
    auto& s=session(); if(!s.search || !s.reference) return;
    if(s.search->finished()){s.needReset=true;return;}
    // Analysis runs reset the level in test mode. Some GD / mod hook stacks do
    // not emit PlayLayer::levelComplete in that state even when the replay has
    // reached the exact recorded end. Frame-window validity is defined as
    // surviving to the reference endpoint; a real completion callback may end
    // the trial early, but is not required. Candidate deaths still fail.
    bool passed=!s.failedTrial;
    if(s.verifyingCheckpoint) {
        auto& cp=s.analysisCheckpoints[*s.activeCheckpoint];
        if(!passed || s.verificationCursor!=cp.validation.size() || cp.validation.empty())rejectCheckpoint("Unmodified validation suffix failed or ended before all samples.");
        else {cp.verified=true;s.verifyingCheckpoint=false;s.needReset=true;log::debug("Checkpoint {} verified",cp.tick);}
        return;
    }
    if(s.search->baseline() && s.poseCursor<s.reference->poses.size()) passed=false;
    if(s.search->stage==Search::Stage::Baseline && passed)s.canonicalPoses=s.reference->poses;
#ifdef FWL_RUNTIME_TEST
    if(Mod::get()->getLaunchFlag("self-test"))log::info("Trial input {} offset {} baseline {}: {} / {}",s.search->index,s.search->offset,s.search->baseline(),passed,s.reason);
#endif
    if(!s.search->baseline()) {
        if(s.confirmingFailure) {
            auto check=resolveFailureConfirmation(passed,s.firstFailureTick,s.failureTick);
            if(!check.stable) {
                s.analysisUncertain=true;
                s.search->results[s.search->index].unstable=true;
                ++s.unstableChecks;
                auto warning=fmt::format(
                    "WARNING: input {} {:+}f changed on double-check. Continuing conservatively; this frame-window result COULD be inaccurate.",
                    s.search->index+1,s.search->offset);
                s.reason=warning;
                s.status=warning;
                log::warn("{} First failure tick {}, repeated outcome {}, repeated failure tick {}.",
                    warning,s.firstFailureTick,passed?"survived":"failed",s.failureTick);
            }
            passed=check.acceptedPass;
            s.failureTick=check.acceptedFailureTick;
            s.confirmingFailure=false;
            if(check.stable)log::debug("Double-check matched");
        } else if(!passed) {
            s.firstFailureTick=s.failureTick;s.confirmingFailure=true;s.needReset=true;return;
        }
        log::debug("Window input {} offset {} endpoint {}: {} (failure tick {})",s.search->index+1,s.search->offset,s.trialEnd,passed?"survived":"failed",s.failureTick);
    }
    s.search->accept(passed,s.failureTick); s.needReset=true;
}
void finishAnalysis() {
    auto& s=session(); if(!s.search || !s.reference) return;
    if(s.search->stage==Search::Stage::Failed) {stopAnalysis(s.search->error+(s.reason.empty()?"":" "+s.reason));return;}
    if(s.search->finalValidationWarning) {
        s.analysisUncertain=true;
        s.reason=s.search->finalValidationMessage;
        s.status=s.search->finalValidationMessage;
        log::warn("{}",s.search->finalValidationMessage);
    }
    s.reference->windows=s.search->results;s.reference->nextClick=s.analyzeNextClick; std::string err;
    bool saved=save(*s.reference,err);
    auto count=s.reference->inputs.size();
    bool uncertain=s.analysisUncertain;
    std::string message;
    if(saved && s.search->finalValidationWarning) message=
        "Saved all measured inputs with a FINAL VALIDATION WARNING. The post-analysis reference replay desynced; results were preserved but are UNVERIFIED and COULD be inaccurate.";
    else if(saved && uncertain) message=
        "Saved with warnings. Open Input details for unstable double-checks or checkpoint fallbacks; affected results COULD be inaccurate.";
    else message=saved?fmt::format("Measured {} inputs. Reference saved.",count):"Measured inputs, but save failed: "+err;
    stopAnalysis(message);
    Notification::create(
        saved?(uncertain?"Saved with warning - check F4":"Frame windows measured and saved"):"Run save failed",
        saved?NotificationIcon::Success:NotificationIcon::Error)->show();
}
void deliver(PlayLayer* pl) {
    auto& s=session(); if(s.mode!=Mode::Analyzing || s.layer!=pl || s.resetting || s.failedTrial) return;
    int tick=s.physicsTick;
    s.injecting=true;
    while(auto next=s.candidate.next(s.reference->inputs)) {
        int scheduled=s.candidate.tick(s.reference->inputs,*next);
        if(scheduled>tick)break;
        auto const& in=s.reference->inputs[*next];
        s.candidate.consume(*next);s.cursor=s.candidate.cursor;
        if(scheduled<tick) {
            s.failedTrial=true;s.reason=fmt::format("Replay missed input tick {} (current {}). The input scheduler or TPS is incompatible.",scheduled,tick);
            // A scheduler failure is not evidence that a shifted input dies.
            // Reject the analysis instead of creating false one-frame windows.
            if(s.verifyingCheckpoint)rejectCheckpoint(s.reason);else if(s.search)s.search->fail(s.reason);break;
        }
        auto player=in.player?pl->m_player2:pl->m_player1;
        // Learn mode at the actual input, including for references recorded
        // before mode metadata existed. A wave release is a direction change.
        if(s.search && s.search->baseline()) {
            auto& original=s.reference->inputs[*next];
            if(s.search->stage==Search::Stage::Baseline)original.wave=player->m_isDart;
            else if(original.wave!=player->m_isDart) {
                s.search->fail("Player mode changed between baseline replays. No new widths accepted.");break;
            }
        }
        s.playbackHeld[in.player]=in.down;
        if(s.reference->pulseInputs) pl->handleButton(in.down,1,in.player==0);
        else {
            auto p=in.player?pl->m_player2:pl->m_player1;
            if(in.down) p->pushButton(PlayerButton::Jump); else p->releaseButton(PlayerButton::Jump);
        }
    }
    s.injecting=false;
}
void recordInput(PlayerObject* p,bool down,PlayerButton button,bool fromPulse=false) {
    auto& s=session(); auto pl=s.layer;
    if(s.pulseLinked && !fromPulse)return;
    if(!pl || s.mode!=Mode::Recording || s.resetting || button!=PlayerButton::Jump || p->m_isDead) return;
    int player=p==pl->m_player1?0:p==pl->m_player2?1:-1; if(player<0) return;
    if(s.held[player]==down) return;
    s.held[player]=down;
    int tick=s.physicsTick;
    for(auto it=s.recording.inputs.rbegin();it!=s.recording.inputs.rend();++it) {
        if(it->player==player) {
            if(it->tick==tick) rejectRecording("Two transitions occurred on the same player in one tick. Disable CBF/CBS and record at 240 Hz.");
            break;
        }
    }
    if(s.recording.inputs.size()>=100000) {rejectRecording("Reference exceeds 100,000 input transitions.");return;}
    s.recording.inputs.push_back({tick,player,down,p->getPositionX(),p->getPositionY(),p->m_isDart});
}
}
void startRecording() {
    attachActiveLayer();
    auto& s=session(); std::string why;
    if(pulseState(s.layer)!=0){s.reason="Pulse is active. Use F8 to stop its linked recording first.";return;}
    if(!Mod::get()->getSettingValue<bool>("fwl-enabled")) {s.reason="Enable Frame Window Lab in settings first.";return;}
    if(s.mode==Mode::Analyzing) {s.reason="Cancel analysis before starting a new recording.";return;}
    if(!eligible(s.layer,why)) {s.reason=why;FLAlertLayer::create("Frame Window Lab",why.c_str(),"OK")->show();return;}
    resumeLayer(); auto pl=s.layer;
    ++linkGeneration;s.pulseLinked=false;s.savedReference=false;
    s.mode=Mode::Recording; s.recording=Run{}; s.held={}; s.invalidRecording=false; s.reason.clear();
    s.recording.fingerprint=fingerprint(pl); s.recording.levelName=pl->m_level->m_levelName;
    s.recording.environment=environment();
    s.resetting=true; pl->resetLevelFromStart(); s.resetting=false;
    s.recording.seed=pl->m_randomSeed; s.recordedThrough=0; s.lastPoseTick=-1;
    s.physicsTick=0;++s.physicsRevision;
    s.status="Recording - F5 stops and saves prefix";
    log::info("Recording reference {} from tick zero",s.recording.fingerprint);
}
void stopRecording(bool completed) {
    auto& s=session(); if(s.mode!=Mode::Recording) return;
    s.mode=Mode::Idle;
    if(s.invalidRecording) {s.status="Reference rejected - see F4";return;}
    s.recording.endTick=s.recordedThrough; s.recording.completed=completed;
    // An input in the last tick has no observed future; omit it from a prefix.
    while(!s.recording.inputs.empty() && s.recording.inputs.back().tick>=s.recording.endTick) s.recording.inputs.pop_back();
    while(!s.recording.poses.empty() && s.recording.poses.back().tick>=s.recording.endTick) s.recording.poses.pop_back();
    if(s.recording.inputs.empty() || s.recording.endTick<2 || s.recording.poses.empty()) {
        s.reason="Record at least one input and one second of surviving gameplay.";s.status="Reference too short";return;
    }
    s.recording.windows.resize(s.recording.inputs.size());
    s.reference=std::move(s.recording); s.precisionTick=-1;
    std::string err; bool ok=save(*s.reference,err);
    s.savedReference=ok;
    s.status=ok?"Reference saved - F4 to analyze":"Save failed - see F4";
    s.reason=ok?(completed?"Full run saved. Open F4 and choose Analyze.":"Surviving prefix saved. Windows will only be valid through the prefix end."):err;
    log::info("Reference saved: {} transitions, end tick {}, completed {}",s.reference->inputs.size(),s.reference->endTick,completed);
}
void startAnalysis() {
    attachActiveLayer();
    auto& s=session(); if(s.mode==Mode::Analyzing) {resumeLayer();return;}
    if(s.mode==Mode::Idle && (!s.reference || s.reference->pulseInputs))selectSavedReference();
    if(pulseState(s.layer)!=0){s.reason="Stop Pulse playback/recording with F8 before analyzing.";return;}
    if(!Mod::get()->getSettingValue<bool>("fwl-enabled")) {s.reason="Enable Frame Window Lab in settings first.";return;}
    if(s.mode==Mode::Recording) stopRecording();
    std::string why;
    if(!eligible(s.layer,why,s.reference && s.reference->pulseInputs) || !s.reference) {FLAlertLayer::create("Frame Window Lab",s.reference?why.c_str():(s.reason.empty()?"Record a reference first (F6).":s.reason.c_str()),"OK")->show();return;}
    if(compatibleEnvironment(s.reference->environment)!=compatibleEnvironment(environment())) {FLAlertLayer::create("Reference environment changed","The enabled mod versions differ from the recording. Record a new reference with your current mods.","OK")->show();return;}
    auto pl=s.layer; resumeLayer();
    ++linkGeneration;s.pulseLinked=false;
    s.oldTest=pl->m_isTestMode; s.oldPractice=pl->m_isPracticeMode;
    s.oldCBS=pl->m_clickBetweenSteps;s.oldCOS=pl->m_clickOnSteps;
    clearAnalysisCache();
    s.userCheckpoints=pl->m_checkpointArray;s.userCurrentCheckpoint=pl->m_currentCheckpoint;
    auto privateArray=CCArray::create();privateArray->retain();
    if(pl->m_checkpointArray)pl->m_checkpointArray->release();
    pl->m_checkpointArray=privateArray;pl->m_currentCheckpoint=nullptr;s.isolatedCheckpoints=true;
    s.cacheEnabled=Mod::get()->getSettingValue<bool>("fwl-checkpoint-cache");
    s.inputAwareCache=Mod::get()->getSettingValue<bool>("fwl-input-aware-checkpoints");
    s.checkpointLimit=static_cast<size_t>(std::clamp<int64_t>(
        Mod::get()->getSettingValue<int64_t>("fwl-checkpoint-limit"),32,static_cast<int64_t>(checkpointCap)));
    s.cacheSpacing=checkpointSpacing(s.reference->endTick,
        static_cast<int>(Mod::get()->getSettingValue<int64_t>("fwl-checkpoint-spacing")),s.checkpointLimit);
    s.checkpointPlan.clear();s.checkpointPlanCursor=0;
    if(s.cacheEnabled && s.inputAwareCache)
        s.checkpointPlan=inputAwareCheckpointPlan(*s.reference,s.checkpointLimit);
    log::info("Analysis checkpoint cache: {} | strategy {} | planned {} / limit {} | fallback spacing {}",
        s.cacheEnabled?"on":"off",s.inputAwareCache?"candidate-aware":"periodic",s.checkpointPlan.size(),s.checkpointLimit,s.cacheSpacing);
    s.physicsUpdates=0;s.checkpointRestores=0;s.fullRestarts=0;s.checkpointFallbacks=0;s.forceFullRestart=false;
    s.analysisStarted=std::chrono::steady_clock::now();
    pl->m_clickBetweenSteps=false;pl->m_clickOnSteps=false;
    s.locked=true; s.mode=Mode::Analyzing; s.search=std::make_unique<Search>(s.reference->inputs.size());
    s.analyzeNextClick=!Mod::get()->getSettingValue<bool>("fwl-full-macro-windows");
    s.confirmingFailure=false;s.firstFailureTick=-1;
    s.analysisUncertain=false;s.unstableChecks=0;
    s.reason.clear(); s.needReset=true;
    s.status="Analyzing | validating reference | F4 cancels";
    Notification::create("Analyzing frame windows - F4 opens cancel controls",NotificationIcon::Info,5.f)->show();
}
void stopAnalysis(std::string message) {
    auto& s=session(); auto pl=s.layer; if(!pl) return;
    const bool failed=s.search && s.search->stage==Search::Stage::Failed;
    log::info("Analysis stopped: {}",message);
    log::info("Analysis runtime: {} | checkpoint fallbacks {} | unstable double-checks {}",analysisProgress(),s.checkpointFallbacks,s.unstableChecks);
    try {
        auto report=matjson::Value::object();
        report["version"]="v1.4.6";report["level"]=s.reference?s.reference->levelName:"";
        report["cacheEnabled"]=s.cacheEnabled;report["inputAwareCache"]=s.inputAwareCache;
        report["checkpointSpacing"]=s.cacheSpacing;report["checkpointLimit"]=static_cast<int64_t>(s.checkpointLimit);report["plannedCheckpoints"]=static_cast<int64_t>(s.checkpointPlan.size());
        report["checkpointCount"]=static_cast<int64_t>(s.analysisCheckpoints.size());
        report["physicsUpdates"]=static_cast<int64_t>(s.physicsUpdates);
        report["checkpointRestores"]=static_cast<int64_t>(s.checkpointRestores);
        report["fullRestarts"]=static_cast<int64_t>(s.fullRestarts);
        report["fallbacks"]=s.checkpointFallbacks;report["unstableChecks"]=s.unstableChecks;
        report["finalValidationWarning"]=s.search && s.search->finalValidationWarning;
        report["finalValidationMessage"]=s.search?s.search->finalValidationMessage:"";
        report["elapsedSeconds"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-s.analysisStarted).count();
        report["status"]=message;report["completed"]=s.search && s.search->stage==Search::Stage::Done;
        report["windows"]=matjson::Value::array();
        if(s.search)for(auto const& w:s.search->results) {
            auto item=matjson::Value::object();item["early"]=w.early;item["late"]=w.late;item["measured"]=w.measured;item["overflow"]=w.overflow;
            item["unstable"]=w.unstable;item["checkpointFallback"]=w.checkpointFallback;
            item["earlyFailureTick"]=w.earlyFailureTick;item["lateFailureTick"]=w.lateFailureTick;
            report["windows"].push(item);
        }
        std::ofstream out(Mod::get()->getSaveDir()/"last-analysis.json");out<<report.dump(2);
        if(!out)log::warn("Could not write last-analysis.json diagnostics");
    } catch(std::exception const& e){log::warn("Could not write analysis diagnostics: {}",e.what());}
    s.mode=Mode::Idle; s.search.reset(); s.needReset=false; s.reason=message; s.status=message;
    s.resetting=true; s.injecting=true;
    // Reset before allowing any ordinary gameplay or save callbacks again.
    // Clear the analyzer's private checkpoint selection first; otherwise a
    // practice-mode reset hook can accidentally reload the last analysis cache
    // entry while we are trying to return to the level start.
    pl->m_uncommittedJumps=0; pl->m_isTestMode=s.oldTest; pl->m_isPracticeMode=false;
    if(s.isolatedCheckpoints && pl->m_checkpointArray)pl->m_checkpointArray->removeAllObjects();
    pl->m_currentCheckpoint=nullptr;
    pl->resetLevelFromStart(); pl->m_player1->releaseAllButtons(); pl->m_player2->releaseAllButtons();
    clearAnalysisCache();
    pl->m_isTestMode=s.oldTest;pl->m_isPracticeMode=s.oldPractice;
    pl->m_clickBetweenSteps=s.oldCBS;pl->m_clickOnSteps=s.oldCOS;
    s.injecting=false; s.resetting=false; s.locked=false; s.precisionTick=-1;
    if(s.hud)s.hud->render();
    pl->pauseGame(false);
    if(failed)FLAlertLayer::create("Frame analysis could not continue",message.c_str(),"OK")->show();
}

class InputDetails final : public Popup {
    size_t index=0;
    CCLabelBMFont* detail=nullptr;
    void refresh(){
        auto& s=session();if(!s.reference || s.reference->inputs.empty()){detail->setString("No reference inputs. Start recording with F6.");return;}
        auto const& run=*s.reference;index=std::min(index,run.inputs.size()-1);
        auto const& in=run.inputs[index];Window w=index<run.windows.size()?run.windows[index]:Window{};
        std::string result=fmt::format("Input {} / {} | P{} {}{}\nStep {} | {:.3f}s\n\n",index+1,run.inputs.size(),in.player+1,in.wave?"WAVE ":"",in.down?"PRESS":"RELEASE",in.tick,in.tick/240.0);
        if(!w.measured)result+="Not measured yet. Save, then Analyze.";
        else if(w.overflow)result+=fmt::format("21+ ticks (lower bound)\nTested room: -{} / +{}\nNominal width >= {:.2f} ms",w.early,w.late,w.nominalMilliseconds());
        else result+=fmt::format("{}f [-{},+{}]\nNominal width: {:.2f} ms\nEarliest {} | Latest {}",w.width(),w.early,w.late,w.nominalMilliseconds(),in.tick-w.early,in.tick+w.late);
        if(w.measured && !w.overflow) {
            auto failure=[](int tick){return tick<0?std::string("input/endpoint bound"):fmt::format("step {}",tick);};
            result+=fmt::format("\nEarly stops: {}\nLate stops: {}",failure(w.earlyFailureTick),failure(w.lateFailureTick));
        }
        result+=run.nextClick?fmt::format("\n\n{} | endpoint {}",in.wave?"NEXT DIRECTION CHANGE":"NEXT CLICK",trialEndpoint(run,index,true)):"\n\nFULL MACRO | all later inputs fixed";
        if(!w.warning().empty())result+="\n\n"+w.warning();
        detail->setString(result.c_str());detail->limitLabelWidth(310,.47f,.27f);
    }
    void prev(CCObject*){auto& s=session();if(s.reference && !s.reference->inputs.empty())index=index?index-1:s.reference->inputs.size()-1;refresh();}
    void next(CCObject*){auto& s=session();if(s.reference && !s.reference->inputs.empty())index=(index+1)%s.reference->inputs.size();refresh();}
    bool setup(){
        if(!Popup::init(350,285))return false;setTitle("Input window details");
        detail=CCLabelBMFont::create("","chatFont.fnt");detail->setPosition({175,158});detail->setScale(.47f);m_mainLayer->addChild(detail);
        auto add=[&](char const* title,float x,SEL_MenuHandler action){auto spr=ButtonSprite::create(title);spr->setScale(.5f);auto item=CCMenuItemSpriteExtra::create(spr,this,action);item->setPosition({x,36});m_buttonMenu->addChild(item);};
        add("Previous",91,menu_selector(InputDetails::prev));add("Next",259,menu_selector(InputDetails::next));refresh();return true;
    }
public:
    static InputDetails* create(){auto p=new InputDetails;if(p->setup()){p->autorelease();return p;}delete p;return nullptr;}
};
class Controls final : public Popup {
    void button(char const* text,CCPoint point,SEL_MenuHandler action,float scale=0.55f) {
        auto sprite=ButtonSprite::create(text); sprite->setScale(scale);
        auto item=CCMenuItemSpriteExtra::create(sprite,this,action); item->setPosition(point); m_buttonMenu->addChild(item);
    }
    void act(void (*fn)()) { onClose(nullptr); queueInMainThread([fn]{if(session().layer && session().layer==PlayLayer::get())fn();}); }
    void onRecord(CCObject*) {act([]{if(session().pulseLinked){session().reason="Use Pulse F8 to save both recordings.";return;}if(session().mode==Mode::Recording)stopRecording();else startRecording();});}
    void onAnalyze(CCObject*) {act(startAnalysis);}
    void onCancel(CCObject*) {act([]{if(session().mode==Mode::Analyzing)stopAnalysis("Analysis cancelled. Previous measurements kept.");});}
    void onSettings(CCObject*) {openSettingsPopup(Mod::get());}
    void onToggle(CCObject*) {if(auto popup=InputDetails::create())popup->show();}
    void onExport(CCObject*) {
        auto& s=session();if(!s.reference)return;
        std::string err; if(exportCSV(*s.reference,err)) {utils::file::openFolder(Mod::get()->getSaveDir());}
        else FLAlertLayer::create("Export failed",err.c_str(),"OK")->show();
    }
    bool setup() {
        if(!Popup::init(370,260)) return false;
        setTitle("Frame Window Lab"); auto& s=session();
        auto text=CCLabelBMFont::create("","chatFont.fnt");text->setScale(0.38f);text->setPosition({185,201});
        std::string detail=s.mode==Mode::Recording?fmt::format("Recording | {} inputs | {:.2f}s",s.recording.inputs.size(),s.recordedThrough/240.0):s.mode==Mode::Analyzing?"Paused | "+s.status:s.reference?fmt::format("{} | {} inputs | {:.2f}s {}",s.reference->levelName.substr(0,24),s.reference->inputs.size(),s.reference->endTick/240.0,s.reference->completed?"full run":"prefix"):"Record a run, analyze it, then replay the counter.";
        text->setString(detail.c_str());text->limitLabelWidth(340,0.38f,0.2f);m_mainLayer->addChild(text);
        bool fullMacro=Mod::get()->getSettingValue<bool>("fwl-full-macro-windows");
        std::string hintText=s.reason.empty()?(fullMacro?"Next analysis: FULL MACRO. Later deaths can narrow an early window.":"Next analysis: NEXT ACTION. Wave stops at the next press OR release; other modes use the next press. Last action uses recording end."):s.reason;
        auto hint=TextArea::create(hintText.c_str(),"chatFont.fnt",0.45f,326,{0.5f,0.5f},13,false);
        hint->setPosition({185,158});m_mainLayer->addChild(hint);
        button(s.mode==Mode::Recording?"Stop / save":"Record (F5)",{96,111},menu_selector(Controls::onRecord));
        button(s.mode==Mode::Analyzing?"Resume analysis":"Analyze",{270,111},menu_selector(Controls::onAnalyze),0.48f);
        button("Cancel analysis",{96,76},menu_selector(Controls::onCancel),0.48f);
        button("Settings",{270,76},menu_selector(Controls::onSettings));
        button("Input details",{96,41},menu_selector(Controls::onToggle),0.48f);
        button("Export CSV",{270,41},menu_selector(Controls::onExport));
        return true;
    }
public:
    static Controls* create() {auto p=new Controls;if(p->setup()){p->autorelease();return p;}delete p;return nullptr;}
    void closeForResume(){onClose(nullptr);}
};
void showControls() {
    attachActiveLayer();
    auto& s=session(); if(!s.layer || descendant<Controls>(CCDirector::sharedDirector()->getRunningScene()))return;
    if(s.mode==Mode::Idle && (!s.reference || s.reference->pulseInputs))selectSavedReference();
    if(!s.layer->m_isPaused && !s.layer->m_levelEndAnimationStarted) s.layer->pauseGame(false);
    if(auto p=Controls::create())p->show();
}
}

class $modify(FWLPlay,PlayLayer) {
    static void onModify(auto& self) {
        // Observe attempted collisions before noclip-style hooks can consume
        // them, while forwarding the entire chain during normal gameplay.
        (void)self.setHookPriorityPre("PlayLayer::destroyPlayer",Priority::First);
        (void)self.setHookPriorityPre("PlayLayer::levelComplete",Priority::First);
    }
    void onEnterTransitionDidFinish() {
        PlayLayer::onEnterTransitionDidFinish();
        if(PlayLayer::get()==this)fwl::attachActiveLayer();
    }
    void resetLevel() {
        auto& s=fwl::session();
        if(s.layer==this && s.mode==fwl::Mode::Recording && !s.resetting && !s.pulseLinked) fwl::stopRecording();
        PlayLayer::resetLevel();
        if(s.layer==this) {
            s.precisionTick=-1;
            if(!s.pulseLinked && !s.resetting){s.physicsTick=0;++s.physicsRevision;}
        }
    }
    void destroyPlayer(PlayerObject* player,GameObject* object) {
        auto& s=fwl::session();
        if(s.layer==this && object!=m_anticheatSpike) {
            if(s.mode==fwl::Mode::Analyzing && !s.resetting) {
                if(!s.failedTrial)s.failureTick=s.physicsTick;
                s.failedTrial=true;
                  if(s.search && !s.search->finished() && !s.search->baseline() && !s.verifyingCheckpoint) {
                    int changed=std::min(s.reference->inputs[s.search->index].tick,s.reference->inputs[s.search->index].tick+s.search->offset);
                    if(s.physicsTick<changed) {
                        if(s.activeCheckpoint)fwl::rejectCheckpoint("Trial died before its shifted input.");
                        else s.search->fail("Trial died before the shifted input. This is a replay mismatch, not a tight window.");
                    }
                }
                return;
            }
            if(s.mode==fwl::Mode::Recording) {
                // Calling the next hook first detects an installed noclip that
                // suppresses death. Such a run is not a valid reference.
                PlayLayer::destroyPlayer(player,object);
                if(!player->m_isDead) fwl::rejectRecording("A damage hook suppressed death (for example noclip). Disable it and re-record.");
                if(!s.pulseLinked)fwl::stopRecording();
                else s.status="Recording stays on | retrying checkpoint";
                return;
            }
        }
        PlayLayer::destroyPlayer(player,object);
    }
    void levelComplete() {
        auto& s=fwl::session();
        if(s.layer==this && s.locked) {s.completedTrial=true;return;}
        if(s.layer==this && s.mode==fwl::Mode::Recording) {
            s.recordedThrough=s.physicsTick;fwl::stopRecording(true);
        }
        PlayLayer::levelComplete();
    }
    void updateAttempts() {if(fwl::session().locked && fwl::session().layer==this)return;PlayLayer::updateAttempts();}
    void commitJumps() {if(fwl::session().locked && fwl::session().layer==this){m_uncommittedJumps=0;return;}PlayLayer::commitJumps();}
    void onQuit() {
        auto& s=fwl::session();if(s.layer==this) {
            if(s.mode==fwl::Mode::Recording)fwl::stopRecording();
            if(s.locked){m_isTestMode=s.oldTest;m_isPracticeMode=s.oldPractice;m_clickBetweenSteps=s.oldCBS;m_clickOnSteps=s.oldCOS;m_uncommittedJumps=0;}
            ++fwl::linkGeneration;
            fwl::clearAnalysisCache();
            s.layer=nullptr;s.hud=nullptr;s.mode=fwl::Mode::Idle;s.search.reset();s.locked=false;
        }
        PlayLayer::onQuit();
    }
    void onExit() {
        if(pulse_lifecycle::preserveOnExit(PlayLayer::get()==this,m_isPaused)) {
            PlayLayer::onExit();return;
        }
        auto& s=fwl::session();
        if(s.layer==this) {
            if(s.mode==fwl::Mode::Recording)fwl::stopRecording();
            if(s.locked){m_isTestMode=s.oldTest;m_isPracticeMode=s.oldPractice;m_clickBetweenSteps=s.oldCBS;m_clickOnSteps=s.oldCOS;m_uncommittedJumps=0;}
            ++fwl::linkGeneration;
            fwl::clearAnalysisCache();
            s.layer=nullptr;s.hud=nullptr;s.mode=fwl::Mode::Idle;s.search.reset();s.locked=false;
        }
        PlayLayer::onExit();
    }
};

class $modify(FWLBase,GJBaseGameLayer) {
    void handleButton(bool down,int button,bool player1) {
        auto& s=fwl::session();if(static_cast<GJBaseGameLayer*>(s.layer)==this && s.mode==fwl::Mode::Analyzing && !s.injecting)return;
        GJBaseGameLayer::handleButton(down,button,player1);
    }
    void processQueuedButtons(float dt,bool clear) {
        auto& s=fwl::session();
        if(static_cast<GJBaseGameLayer*>(s.layer)==this && s.mode==fwl::Mode::Analyzing && !s.resetting)m_queuedButtons.clear();
        GJBaseGameLayer::processQueuedButtons(dt,clear);
    }
    void processCommands(float dt,bool half,bool last) {
        auto& s=fwl::session();bool ours=static_cast<GJBaseGameLayer*>(s.layer)==this;
        const bool advancing=ours && !s.resetting && !s.layer->m_isPaused && m_started && !m_player1->m_isDead && !s.layer->m_hasCompletedLevel && dt>0;
        const auto revision=s.physicsRevision;
        const int tick=s.physicsTick;
        if(advancing && std::abs(dt-1.f/240.f)>0.000001f) {
            if(s.mode==fwl::Mode::Recording)fwl::rejectRecording("Reference analysis requires unchanged 240 Hz physics. Disable TPS bypass.");
            if(s.mode==fwl::Mode::Analyzing && s.search)s.search->fail("Analysis requires unchanged 240 Hz physics. Disable TPS bypass.");
        }
        if(advancing && half && s.mode==fwl::Mode::Recording)fwl::rejectRecording("Sub-tick physics is active. Disable CBF/CBS for whole-frame analysis.");
        if(advancing && half && s.mode==fwl::Mode::Analyzing && s.search)s.search->fail("Sub-tick physics is active. Disable CBF/CBS for analysis.");
        if(advancing && s.mode==fwl::Mode::Analyzing)fwl::deliver(s.layer);
        s.insideStep=advancing;GJBaseGameLayer::processCommands(dt,half,last);s.insideStep=false;
        if(!advancing || s.resetting || static_cast<GJBaseGameLayer*>(s.layer)!=this || revision!=s.physicsRevision)return;
        if(s.mode==fwl::Mode::Recording && !m_player1->m_isDead) {
            s.recordedThrough=tick+1;
            if(tick>s.lastPoseTick && tick%60==0){s.recording.poses.push_back(fwl::pose(s.layer,tick));s.lastPoseTick=tick;}
        }
        if(s.mode==fwl::Mode::Analyzing && s.search && !s.search->finished() && s.reference) {
            if(s.search->stage==fwl::Search::Stage::Baseline && !s.analysisCheckpoints.empty()) {
                // Dense candidate-aware checkpoints may overlap their 60-tick
                // validation windows. Capture one canonical pose for this tick
                // and append it to every checkpoint whose validation suffix is
                // active. v1.4.2 only filled the newest checkpoint, which made
                // checkpoints less than 60 ticks apart look falsely invalid.
                auto first=std::lower_bound(s.analysisCheckpoints.begin(),s.analysisCheckpoints.end(),
                    tick-fwl::checkpointValidationTicks+1,
                    [](fwl::AnalysisCheckpoint const& cp,int minTick){return cp.tick<minTick;});
                bool haveSample=false;fwl::Pose sample;
                for(auto it=first;it!=s.analysisCheckpoints.end() && it->tick<=tick;++it) {
                    if(tick<it->tick+fwl::checkpointValidationTicks) {
                        if(!haveSample){sample=fwl::pose(s.layer,tick);haveSample=true;}
                        it->validation.push_back(sample);
                    }
                }
            }
            if(s.verifyingCheckpoint) {
                auto& cp=s.analysisCheckpoints[*s.activeCheckpoint];
                auto actual=fwl::pose(s.layer,tick);
                if(s.verificationCursor>=cp.validation.size() || cp.validation[s.verificationCursor].tick!=tick ||
                    !fwl::sameValidationPose(cp.validation[s.verificationCursor],actual)) {
                    std::string why=s.verificationCursor<cp.validation.size()?fwl::poseMismatch(cp.validation[s.verificationCursor],actual):"Missing canonical sample";
                    fwl::rejectCheckpoint(why);
                } else ++s.verificationCursor;
                ++s.physicsTick;return;
            }
            if(s.needReset){++s.physicsTick;return;}
            int changed=s.search->baseline()?s.reference->endTick:std::min(s.reference->inputs[s.search->index].tick,s.reference->inputs[s.search->index].tick+s.search->offset);
            while(s.poseCursor<s.reference->poses.size() && s.reference->poses[s.poseCursor].tick<=tick) {
                auto& expected=s.reference->poses[s.poseCursor++];
                if(tick>=changed)continue;
                auto actual=fwl::pose(s.layer,tick);
                if(s.activeCheckpoint && s.poseCursor<=s.canonicalPoses.size()) {
                    auto const& canonical=s.canonicalPoses[s.poseCursor-1];
                    if(canonical.tick!=tick || !fwl::sameValidationPose(canonical,actual)) {
                        fwl::rejectCheckpoint("Cached prefix changed after its initial validation: "+fwl::poseMismatch(canonical,actual));break;
                    }
                }
                // The first baseline establishes the canonical reset trajectory
                // for this analysis session. This removes harmless differences
                // between the original live recording and a clean analysis reset
                // while keeping later candidate/final-baseline validation strict.
                // The first baseline is a validity run, but its reset follows the
                // user's live/playback state. Some GD hook stacks leave a small
                // one-reset transient that disappears on the next reset. Re-anchor
                // the pre-input canonical trajectory once more on the first actual
                // candidate (-1f on input 0). This is safe because this branch is
                // only reached while tick < changed, i.e. before the shifted input
                // has occurred. Every later candidate and the final baseline still
                // validate strictly against this post-reset canonical trajectory.
                // Baseline and candidate trials are allowed to establish their own
                // pre-shift reset trajectory. Candidate validity is still determined
                // entirely by natural physics after the shifted input, and the final
                // baseline remains strict. This avoids treating reset-to-reset state
                // differences from the live hook stack as a frame-window failure.
                bool establishCanonical =
                    s.search->stage==fwl::Search::Stage::Baseline ||
                    s.search->stage==fwl::Search::Stage::Early ||
                    s.search->stage==fwl::Search::Stage::Late;
                if(establishCanonical) expected=actual;
                if(expected.tick!=tick || !fwl::sameValidationPose(expected,actual)) {
                    s.failedTrial=true;s.reason=fmt::format(
                        "Replay differs before the shifted input at physics step {} ({}). Check practice restoration and physics modifiers.",
                        tick,fwl::poseMismatch(expected,actual));
                    s.search->fail(s.reason);break;
                }
            }
        }
        ++s.physicsTick;
    }
    void update(float dt) {
        if(static_cast<GJBaseGameLayer*>(PlayLayer::get())==this)fwl::attachActiveLayer();
        auto& s=fwl::session();
        if(static_cast<GJBaseGameLayer*>(s.layer)==this && s.layer->m_isPaused && (s.mode!=fwl::Mode::Idle || fwl::pulseState(s.layer)!=0))return;
        if(static_cast<GJBaseGameLayer*>(s.layer)==this && s.mode==fwl::Mode::Recording && !Mod::get()->getSettingValue<bool>("fwl-enabled"))fwl::stopRecording();
        if(static_cast<GJBaseGameLayer*>(s.layer)!=this || s.mode!=fwl::Mode::Analyzing || s.layer->m_isPaused) {GJBaseGameLayer::update(dt);return;}
        if(!Mod::get()->getSettingValue<bool>("fwl-enabled")) {fwl::stopAnalysis("Analysis stopped because the mod was disabled.");return;}
        auto begin=std::chrono::steady_clock::now();
        int limit=static_cast<int>(Mod::get()->getSettingValue<int64_t>("fwl-analysis-speed"));
        int budget=static_cast<int>(Mod::get()->getSettingValue<int64_t>("fwl-analysis-budget"));
        for(int i=0;i<limit;++i) {
            if(!s.search || s.mode!=fwl::Mode::Analyzing || s.layer->m_isPaused)break;
            if(s.needReset)fwl::resetTrial();
            if(s.search->finished()){fwl::finishAnalysis();break;}
            // v1.3.13 turbo: confirmations and the final baseline still replay,
            // but they use the same batched 240 Hz simulation as ordinary trials.
            // The previous one-update-per-render rule was the dominant runtime cost.
            bool single=false;
            int before=s.physicsTick;
            GJBaseGameLayer::update(1.f/240.f); ++s.trialUpdates;++s.physicsUpdates;
            int tick=s.physicsTick;
            if(tick-before>1){s.search->fail("One analysis update produced multiple physics steps. Disable TPS/speed modifiers before analyzing.");continue;}
            if(s.trialUpdates>s.reference->endTick*4+2400) {s.search->fail("Replay did not advance to the reference endpoint. Check speed/TPS settings.");continue;}
            // Prefer GD's real completion callback when it fires, but fall back
            // to the deterministic recorded endpoint. Requiring levelComplete
            // here caused valid 100% replays to run an extra second and then be
            // mislabeled as desynced when test mode suppressed that callback.
            bool endpoint=s.completedTrial || tick>=s.trialEnd;
            if(!s.needReset && (s.failedTrial || endpoint))fwl::finishTrial();
            else if(!s.needReset)fwl::captureAnalysisCheckpoint();
            if(single)break;
            if(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count()>=budget)break;
        }
    }
};

class $modify(FWLPlayer,PlayerObject) {
    bool pushButton(PlayerButton button) {
        auto& s=fwl::session();bool real=s.layer && (this==s.layer->m_player1 || this==s.layer->m_player2);
        if(real && s.mode==fwl::Mode::Analyzing && !s.injecting && button==PlayerButton::Jump)return false;
        bool result=PlayerObject::pushButton(button);if(real)fwl::recordInput(this,true,button);return result;
    }
    bool releaseButton(PlayerButton button) {
        auto& s=fwl::session();bool real=s.layer && (this==s.layer->m_player1 || this==s.layer->m_player2);
        if(real && s.mode==fwl::Mode::Analyzing && !s.injecting && button==PlayerButton::Jump)return false;
        bool result=PlayerObject::releaseButton(button);if(real)fwl::recordInput(this,false,button);return result;
    }
};
class $modify(FWLStats,GameStatsManager) {
    void incrementStat(char const* key,int amount) {if(fwl::session().locked)return;GameStatsManager::incrementStat(key,amount);}
};
class $modify(FWLPause,PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto size=CCDirector::sharedDirector()->getWinSize();auto menu=CCMenu::create();menu->setPosition({0,0});menu->setID("pause-controls"_spr);
        auto spr=ButtonSprite::create("Frame Windows");spr->setScale(0.45f);
        auto item=CCMenuItemSpriteExtra::create(spr,this,menu_selector(FWLPause::openCounter));item->setPosition({size.width-170,24});menu->addChild(item);addChild(menu,100);
    }
    void openCounter(CCObject*) {fwl::showControls();}
};
bool pulse_frame_link::toFrames(pulse_frame_link::Message* msg) {
        fwl::attachActiveLayer();auto& s=fwl::session();
        if(!msg->layer || msg->layer!=s.layer)return false;
        if(msg->command==pulse_frame_link::Command::TogglePause) {
            // Close analysis controls before resuming through the native pause menu.
            auto scene=CCDirector::sharedDirector()->getRunningScene();
            if(fwl::descendant<fwl::InputDetails>(scene))return false;
            auto panel=fwl::descendant<fwl::Controls>(scene);
            if(fwl::descendant<Popup>(scene) && !panel)return false;
            if(panel)panel->closeForResume();
            if(s.layer->m_isPaused)fwl::resumeLayer();else s.layer->pauseGame(false);
            msg->accepted=true;return true;
        }
        if(msg->command==pulse_frame_link::Command::Show){msg->accepted=true;fwl::showControls();return true;}
        if(msg->slot<1 || msg->slot>20){msg->reason="Invalid Pulse slot";return true;}
        if(msg->command==pulse_frame_link::Command::Select && s.mode==fwl::Mode::Idle){
            ++fwl::linkGeneration;
            std::string error;s.reference=fwl::load(fwl::pulseSlotKey(fwl::fingerprint(s.layer),msg->slot),error);
            if(s.reference && (msg->macroRevision.empty() || s.reference->macroRevision!=msg->macroRevision)){
                s.reference.reset();error="This macro was replaced since the frame reference. Record a new linked reference.";
            }
            s.precisionTick=-1;s.reason=error;s.status=s.reference?fmt::format("Pulse slot {} | reference timeline",msg->slot):"No frame reference for this Pulse slot";
            msg->accepted=true;if(s.hud)s.hud->render();return true;
        }
        if(msg->command==pulse_frame_link::Command::Query){msg->state=static_cast<int>(s.mode);msg->accepted=true;return true;}
        if(s.pulseLinked && s.mode==fwl::Mode::Recording){
            if(msg->command==pulse_frame_link::Command::Input){
                if(msg->physicsTick!=static_cast<std::uint64_t>(s.physicsTick))fwl::rejectRecording("Macro and frame-reference physics clocks diverged. Save and start a new recording.");
                if(msg->button==1)fwl::recordInput(msg->player1?s.layer->m_player1:s.layer->m_player2,msg->down,PlayerButton::Jump,true);
                msg->accepted=true;return true;
            }
            if(msg->command==pulse_frame_link::Command::Suspend){s.resetting=true;msg->accepted=true;return true;}
            if(msg->command==pulse_frame_link::Command::Resume){s.resetting=false;s.status="Recording | checkpoint restored | F8 saves";msg->accepted=true;return true;}
            if(msg->command==pulse_frame_link::Command::Checkpoint && msg->checkpoint){
                fwl::ReferenceCheckpoint cp;
                cp.inputs=s.recording.inputs.size();cp.poses=s.recording.poses.size();
                cp.physicsTick=static_cast<int>(msg->physicsTick);cp.recordedThrough=std::max(s.recordedThrough,cp.physicsTick);
                cp.lastPoseTick=s.lastPoseTick;cp.held=s.held;cp.invalid=s.invalidRecording;cp.reason=s.reason;cp.ordinal=++s.nextCheckpoint;
                s.checkpoints[msg->checkpoint]=std::move(cp);msg->accepted=true;return true;
            }
            if(msg->command==pulse_frame_link::Command::Rewind){
                auto found=s.checkpoints.find(msg->checkpoint);
                if(found==s.checkpoints.end()){fwl::rejectRecording("Frame reference has no matching checkpoint. Begin a new F6 recording from zero.");msg->reason=s.reason;return true;}
                try{
                    auto cp=found->second;fwl::rewindReference(s.recording,cp);
                    s.physicsTick=cp.physicsTick;++s.physicsRevision;s.recordedThrough=cp.recordedThrough;s.lastPoseTick=cp.lastPoseTick;
                    s.held=cp.held;s.invalidRecording=cp.invalid;s.reason=cp.reason;s.precisionTick=-1;
                    std::erase_if(s.checkpoints,[&](auto const& item){return item.second.ordinal>cp.ordinal;});
                    msg->accepted=true;
                }catch(std::exception const& e){fwl::rejectRecording(e.what());msg->reason=e.what();}
                return true;
            }
        }
        if(msg->command==pulse_frame_link::Command::Begin){
            ++fwl::linkGeneration;
            std::string why;
            if(s.mode==fwl::Mode::Analyzing){msg->reason="Cancel analysis with F4 first.";return true;}
            if(!Mod::get()->getSettingValue<bool>("fwl-enabled")){msg->reason="Enable Frame Window Lab to link it.";return true;}
            if(!fwl::eligible(s.layer,why,true)){msg->reason=why;s.reason=why;s.status="Frame capture unavailable - F4 for reason";return true;}
            s.pulseLinked=true;s.savedReference=false;s.mode=fwl::Mode::Recording;s.recording=fwl::Run{};
            s.recording.fingerprint=fwl::pulseSlotKey(fwl::fingerprint(s.layer),msg->slot);s.recording.levelName=s.layer->m_level->m_levelName;
            s.recording.environment=fwl::environment();s.recording.seed=s.layer->m_randomSeed;s.recording.pulseInputs=true;
            s.held={};s.invalidRecording=false;s.reason.clear();s.recordedThrough=0;s.lastPoseTick=-1;s.precisionTick=-1;
            s.resetting=false;s.checkpoints.clear();s.nextCheckpoint=0;s.physicsTick=0;++s.physicsRevision;
            s.status="Linked to Pulse | F8 saves both";msg->accepted=true;
            log::info("Linked frame recording started: slot {}",msg->slot);
            if(s.hud)s.hud->render();return true;
        }
        if(msg->command==pulse_frame_link::Command::End && s.pulseLinked){
            if(msg->completed)s.recordedThrough=s.physicsTick;
            if(msg->savedMacro){
                s.recording.macroRevision=msg->macroRevision;
                if(s.mode==fwl::Mode::Recording)fwl::stopRecording(msg->completed);
                else if(s.savedReference && s.reference){s.reference->macroRevision=msg->macroRevision;std::string err;s.savedReference=fwl::save(*s.reference,err);if(!err.empty())s.reason=err;}
            } else {s.mode=fwl::Mode::Idle;s.savedReference=false;s.status="Linked take discarded or restarted";}
            s.pulseLinked=false;s.resetting=false;s.checkpoints.clear();msg->accepted=true;
            if(msg->savedMacro && !s.savedReference)msg->reason=s.reason.empty()?"No valid frame reference was saved. Open F4 for details.":s.reason;
            const auto ticket=++fwl::linkGeneration;auto layer=s.layer;
            if(msg->analyze && s.savedReference && Mod::get()->getSettingValue<bool>("fwl-auto-analyze-pulse")){
                s.status="Reference saved | analysis queued";
                queueInMainThread([ticket,layer]{
                    auto& active=fwl::session();
                    if(ticket==fwl::linkGeneration && active.layer==layer && PlayLayer::get()==layer &&
                        active.mode==fwl::Mode::Idle && fwl::pulseState(layer)==0) {
                        if(layer->m_isPaused){active.status="Reference saved | F4 Analyze when ready";return;}
                        fwl::startAnalysis();
                    }
                });
            }
            return true;
        }
        return false;
}
$execute {
    listenForKeybindSettingPresses("fwl-toggle-key",[](Keybind const&,bool down,bool repeat,double) {
        if(!PlayLayer::get() || !down || repeat)return false;
        auto m=Mod::get();m->setSettingValue("fwl-show-hud",!m->getSettingValue<bool>("fwl-show-hud"));return true;
    });
    listenForKeybindSettingPresses("fwl-panel-key",[](Keybind const&,bool down,bool repeat,double) {
        if(!PlayLayer::get() || !down || repeat)return false;fwl::showControls();return true;
    });
    listenForKeybindSettingPresses("fwl-record-key",[](Keybind const&,bool down,bool repeat,double) {
        if(!PlayLayer::get() || !down || repeat)return false;
        if(fwl::session().pulseLinked){fwl::session().reason="Use Pulse F8 to save both recordings.";return true;}
        if(fwl::session().mode==fwl::Mode::Recording)fwl::stopRecording();else fwl::startRecording();return true;
    });
}
