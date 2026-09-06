// Opt-in in-game test harness. Never enabled in the delivered binary.
#include <Geode/modify/MenuLayer.hpp>
namespace {
int smokeStage = 0;
bool smokeOldCBF = false, smokeOldPB = false;
bool smokeHasCBF = false;
void restoreSmokeSettings() {
    if (smokeHasCBF) {
        if (auto cbf = Loader::get()->getLoadedMod("syzzi.click_between_frames")) {
            cbf->setSettingValue("physics-bypass", smokeOldPB);
            cbf->setSettingValue("soft-toggle", smokeOldCBF);
        }
        smokeHasCBF = false;
    }
}
void finishSmoke(std::string result) {
    smokeStage = 5;
    log::info("INTEGRATION: {}", result);
    std::ofstream stream(Mod::get()->getSaveDir() / "integration-result.txt");
    stream << result << '\n';
    stop(false, result);
    restoreSmokeSettings();
    queueInMainThread([] { if (session.layer) session.layer->pauseGame(false); });
}
void smokeBeforeStep(GJBaseGameLayer* layer) {
    if (layer != session.layer || !layer->m_started || session.resetting || smokeStage < 2 || smokeStage >= 5) return;
    if (smokeStage == 2 && session.mode == Mode::Record) {
        auto tick = session.recorder.tape.steps.size();
        if (tick == 100 || tick == 400 || tick == 700 || tick == 1000) layer->handleButton(true, 1, true);
        if (tick == 120 || tick == 420 || tick == 720 || tick == 1020) layer->handleButton(false, 1, true);
        if (tick == 1500) {
            smokeStage = 3;
            queueInMainThread([] {
                stop(true, "Integration recording saved");
                startPlay();
                smokeStage = 4;
                if (session.mode != Mode::Play) finishSmoke("FAIL: playback could not start: " + session.message);
            });
        }
    } else if (smokeStage == 4) {
        if (session.mode != Mode::Play) { finishSmoke("FAIL: " + session.message); return; }
        if (session.player.tick() >= 1500) {
            auto result = fmt::format("PASS: {} real physics steps replayed, {} inputs", session.player.tick(),
                session.playback.events.size());
            finishSmoke(result);
        }
    } else if (smokeStage == 2 && session.mode != Mode::Record) finishSmoke("FAIL: recording stopped: " + session.message);
}
}
class $modify(PulseSmokeMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        if (smokeStage == 0) {
            smokeStage = 1;
            scheduleOnce(schedule_selector(PulseSmokeMenu::launchSmoke), 1.f);
        }
        return true;
    }
    void launchSmoke(float) {
        if (auto cbf = Loader::get()->getLoadedMod("syzzi.click_between_frames")) {
            smokeHasCBF = true;
            smokeOldCBF = cbf->getSettingValue<bool>("soft-toggle");
            smokeOldPB = cbf->getSettingValue<bool>("physics-bypass");
            cbf->setSettingValue("soft-toggle", true);
            cbf->setSettingValue("physics-bypass", false);
        }
        auto level = GJGameLevel::create();
        level->m_levelName = "Pulse Integration Test";
        level->m_levelType = GJLevelType::Editor;
        level->m_levelString = "kA2,0;1,1,2,6000,3,600;";
        auto scene = PlayLayer::scene(level, false, false);
        CCDirector::get()->replaceScene(scene);
        queueInMainThread([] {
            startRecord();
            smokeStage = 2;
            if (session.mode != Mode::Record) finishSmoke("FAIL: record could not start: " + session.message);
        });
    }
};
