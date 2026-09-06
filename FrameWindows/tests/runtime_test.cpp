// Developer-only engine smoke test. Not compiled into the delivered package.
// Enable FWL_RUNTIME_TEST and launch GD with --geode:alan.frame_window_lab.self-test.
#include "../src/Session.hpp"
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/AppDelegate.hpp>
#include <fstream>
using namespace geode::prelude;
namespace {
bool started=false, active=false, analyzing=false, requested=false, reported=false;
int prior=-1;
void report(std::string text) {
    log::info("FWL ENGINE TEST: {}",text);
    std::ofstream(Mod::get()->getSaveDir()/"self-test-report.txt") << text << '\n';
    reported=true;active=false;
}
class $modify(FWLTestApp,AppDelegate) {
    void applicationWillResignActive() {
        if(Mod::get()->getLaunchFlag("self-test"))return;
        AppDelegate::applicationWillResignActive();
    }
    void applicationDidEnterBackground() {
        if(Mod::get()->getLaunchFlag("self-test"))return;
        AppDelegate::applicationDidEnterBackground();
    }
};
}
class $modify(FWLTestMenu,MenuLayer) {
    bool init() {
        if(!MenuLayer::init())return false;
        if(!started && Mod::get()->getLaunchFlag("self-test")) {
            started=true;
            runAction(CCSequence::create(CCDelayTime::create(1),CCCallFunc::create(this,callfunc_selector(FWLTestMenu::begin)),nullptr));
        }
        return true;
    }
    void begin() {
        auto level=GJGameLevel::create();level->m_levelName="FWL engine smoke test";
        level->m_levelString="kA4,0,kA6,0,kA7,0,kA13,0;1,1,2,9000,3,15;";
        level->m_levelType=GJLevelType::Editor;level->m_levelID=0;
        auto scene=PlayLayer::scene(level,false,false);
        CCDirector::sharedDirector()->replaceScene(scene);
        auto pl=PlayLayer::get();pl->m_isTestMode=true;fwl::startRecording();active=true;
    }
};
class $modify(FWLTestBase,GJBaseGameLayer) {
    void processQueuedButtons(float dt,bool clear) {
        auto& s=fwl::session();int tick=static_cast<int>(m_gameState.m_currentProgress);
        if(active && static_cast<GJBaseGameLayer*>(s.layer)==this && s.mode==fwl::Mode::Recording && tick!=prior) {
            prior=tick;
            if(tick==120 || tick==240)m_player1->pushButton(PlayerButton::Jump);
            if(tick==150 || tick==270)m_player1->releaseButton(PlayerButton::Jump);
        }
        GJBaseGameLayer::processQueuedButtons(dt,clear);
    }
    void update(float dt) {
        GJBaseGameLayer::update(dt);
        auto& s=fwl::session();if(!active || reported || static_cast<GJBaseGameLayer*>(s.layer)!=this)return;
        if(s.mode==fwl::Mode::Recording && m_gameState.m_currentProgress>=360 && !requested) {
            requested=true;queueInMainThread([] {fwl::stopRecording();fwl::startAnalysis();analyzing=true;});
        }
        if(analyzing && s.mode==fwl::Mode::Idle) {
            bool ok=s.reference && s.reference->inputs.size()==4 && s.reference->windows.size()==4;
            if(ok)for(auto const& w:s.reference->windows)ok=ok && w.measured && w.overflow;
            report(ok?"PASS: real-engine reference replay, four inputs, 21+ windows, final baseline, and persistence.":"FAIL: "+s.reason);
        }
    }
};
