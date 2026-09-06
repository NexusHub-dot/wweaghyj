#include "Hud.hpp"
#include <Geode/utils/cocos.hpp>
#include <vector>
using namespace geode::prelude;
namespace fwl {
namespace {
CCLabelBMFont* label(CCNode* parent,float scale=0.32f) {
    auto n=CCLabelBMFont::create("","chatFont.fnt");
    n->setAnchorPoint({0,0.5f});
    n->setScale(scale);
    parent->addChild(n);
    return n;
}
ccColor3B color(int n) { return Mod::get()->getSettingValue<ccColor3B>(fmt::format("fwl-color-{}",std::clamp(n,1,20))); }
ccColor4F rgba(ccColor3B c,float a=1) { return {c.r/255.f,c.g/255.f,c.b/255.f,a}; }
void ring(CCDrawNode* draw,CCPoint p,float radius,ccColor4F fill,ccColor4F stroke,float border=0.8f) {
    std::array<CCPoint,32> verts;
    for(int j=0;j<32;++j) {
        float a=static_cast<float>(j)*6.2831853f/32;
        verts[j]=CCPoint{p.x+std::cos(a)*radius,p.y+std::sin(a)*radius};
    }
    draw->drawPolygon(verts.data(),32,fill,border,stroke);
}
void line(CCDrawNode* draw,CCPoint a,CCPoint b,ccColor4F c,float w=1.f) {
    draw->drawSegment(a,b,w,c);
}
struct Bucket { int lo; int hi; };
std::vector<Bucket> bucketsFor(int lo,int hi) {
    std::vector<Bucket> out;
    auto push=[&](int a,int b){
        a=std::max(a,lo); b=std::min(b,hi);
        if(a<=b) out.push_back({a,b});
    };
    push(13,20);
    push(9,12);
    push(7,8);
    push(5,6);
    push(4,4);
    push(3,3);
    push(2,2);
    push(1,1);
    if(out.empty()) push(lo,hi);
    return out;
}
std::string bucketName(Bucket b,bool cumulative) {
    if(cumulative) return fmt::format("<={}", b.hi);
    return b.lo==b.hi ? fmt::format("{}", b.lo) : fmt::format("{}-{}", b.lo, b.hi);
}
int bucketCount(Counts const& counts,Bucket b,bool cumulative) {
    int total=0;
    int a=cumulative?1:b.lo;
    for(int i=a;i<=b.hi && i<=maxWindow;++i) total+=counts.exact[i];
    return total;
}
ccColor3B bucketColor(Bucket b) {
    if(b.lo==b.hi) return color(b.hi);
    return color((b.lo+b.hi+1)/2);
}
}
Hud* Hud::create(PlayLayer* pl) { auto h=new Hud; if(h->initWithLayer(pl)) {h->autorelease();return h;} delete h;return nullptr; }
bool Hud::initWithLayer(PlayLayer* pl) {
    if(!CCNode::init()) return false;
    setID("counter"_spr); setAnchorPoint({0,1});
    panel=CCDrawNode::create(); addChild(panel);
    title=label(this,0.44f); title->setAnchorPoint({0.5f,0.5f});
    status=label(this,0.42f); status->setAnchorPoint({1.f,0.5f});
    totals=label(this,0.36f);
    footer=label(this,0.34f);
    for(auto& row:rows) row=label(this,0.78f);
    circles=CCDrawNode::create(); circles->setID("input-circles"_spr); pl->m_objectLayer->addChild(circles,1000);
    circleText=CCNode::create(); circleText->setID("input-circle-labels"_spr); pl->m_objectLayer->addChild(circleText,1001);
    scheduleUpdate(); return true;
}
void Hud::update(float dt) { refresh+=dt; if(refresh>=0.05f) {refresh=0;render();} }
void Hud::render() {
    auto& s=session(); auto mod=Mod::get();
    if(!s.layer || s.hud!=this) return;
    bool enabled=mod->getSettingValue<bool>("fwl-enabled");
    setVisible(enabled && (mod->getSettingValue<bool>("fwl-show-hud") || s.mode==Mode::Analyzing));
    circles->setVisible(enabled && mod->getSettingValue<bool>("fwl-show-circles"));
    circleText->setVisible(circles->isVisible());
    if(!enabled) return;

    auto screen=CCDirector::sharedDirector()->getWinSize();
    int lo=static_cast<int>(mod->getSettingValue<int64_t>("fwl-min-frame"));
    int hi=static_cast<int>(mod->getSettingValue<int64_t>("fwl-max-frame"));
    if(lo>hi) std::swap(lo,hi);
    hi=std::clamp(hi,1,maxWindow);
    lo=std::clamp(lo,1,hi);
    bool cumulative=mod->getSettingValue<bool>("fwl-cumulative");
    auto buckets=bucketsFor(lo,hi);

    float width=164.f;
    float header=16.f;
    float footerH=16.f;
    float rowStep=13.f;
    float height=header + footerH + 8.f + static_cast<float>(buckets.size())*rowStep;

    float scale=static_cast<float>(mod->getSettingValue<double>("fwl-hud-scale"));
    scale=std::min(scale,(screen.height-12)/height);
    setContentSize({width,height}); setScale(scale);

    float x=static_cast<float>(mod->getSettingValue<int64_t>("fwl-hud-x"));
    float y=static_cast<float>(mod->getSettingValue<int64_t>("fwl-hud-y"));
    setPosition({std::clamp(x,0.f,std::max(0.f,screen.width-width*scale)),std::clamp(screen.height-y,height*scale,screen.height)});

    int tick=s.physicsTick;
    Run const* run=s.mode==Mode::Recording?&s.recording:(s.reference?&*s.reference:nullptr);
    bool releases=mod->getSettingValue<bool>("fwl-include-releases");
    int player=static_cast<int>(mod->getSettingValue<int64_t>("fwl-player-filter"));
    Counts counts; if(run) counts=count(*run,tick,releases,player);

    panel->clear();
    float opacity=mod->getSettingValue<int64_t>("fwl-panel-opacity")/255.f;
    std::array<CCPoint,4> rect={CCPoint{0,0},CCPoint{width,0},CCPoint{width,height},CCPoint{0,height}};
    panel->drawPolygon(rect.data(),4,{0.03f,0.02f,0.08f,std::max(0.12f,opacity*0.65f)},0.f,{0,0,0,0});
    line(panel,{0,height-2.f},{width,height-2.f},{1.f,1.f,1.f,0.7f},0.75f);
    line(panel,{0,2.f},{width,2.f},{1.f,1.f,1.f,0.55f},0.75f);

    std::string head = "PULSE COUNTER";
    title->setPosition({width*0.5f,height-8.5f});
    title->setString(head.c_str());
    title->setColor({230,230,245});
    title->setOpacity(120);

    float percent = s.layer ? s.layer->getCurrentPercent() : 0.f;
    status->setPosition({width-4.f,height-8.5f});
    status->setString(fmt::format("{:.2f}%", percent).c_str());
    status->setColor({255,255,255});
    status->setOpacity(230);

    totals->setPosition({4.f,8.f});
    std::string totalsText;
    if(s.mode==Mode::Analyzing) totalsText = analysisProgress();
    else if(run) {
        int outside=counts.above; for(int i=hi+1;i<=maxWindow;++i) outside+=counts.exact[i];
        totalsText = fmt::format("P:{} R:{}  >{}:{}  ?:{}", counts.presses, counts.releases, hi, outside, counts.unknown);
    } else totalsText = "Record or load a frame reference";
    if(totalsText.size()>34) totalsText=totalsText.substr(0,31)+"...";
    totals->setString(totalsText.c_str());
    totals->setColor({210,215,235});
    totals->limitLabelWidth(width-8.f,0.36f,0.20f);

    footer->setPosition({4.f,height-20.f});
    std::string modeText=run && run->nextClick?"TO NEXT ACTION":"FULL MACRO";
    if(s.mode==Mode::Analyzing) modeText = s.analysisUncertain ? "ANALYZING (WARNING)" : "ANALYZING";
    else if(run && s.mode!=Mode::Analyzing && mod->getSettingValue<bool>("fwl-show-precision")) {
        if(tick<s.precisionTick || tick-s.precisionTick>=120 || s.precisionTick<0) {
            s.precisionValue=precision(*run,std::min(tick,run->endTick)).value_or(-1); s.precisionTick=tick;
        }
        if(s.precisionValue>=0) modeText=fmt::format("BASE <= {:.2f} sigma/s",s.precisionValue);
    }
    footer->setString(modeText.c_str());
    footer->setColor({186,194,228});
    footer->limitLabelWidth(width-8.f,0.34f,0.18f);

    for(auto row:rows) row->setVisible(false);
    float yCursor = height - 33.f;
    for(size_t idx=0; idx<buckets.size() && idx<rows.size(); ++idx) {
        auto b=buckets[idx];
        auto c=bucketColor(b);
        auto text=rows[idx];
        text->setVisible(true);
        text->setColor(c);
        text->setPosition({6.f,yCursor});
        text->setString(fmt::format("{}: {}", bucketName(b,cumulative), bucketCount(counts,b,cumulative)).c_str());
        yCursor -= rowStep;
    }

    circles->clear();
    circleText->removeAllChildrenWithCleanup(true);
    if(!run || !circles->isVisible() || s.mode==Mode::Analyzing) return;

    int lifetime=static_cast<int>(mod->getSettingValue<double>("fwl-circle-lifetime")*240);
    float radius=static_cast<float>(mod->getSettingValue<double>("fwl-circle-radius"));
    radius=std::max(radius, 5.f);
    auto start=std::lower_bound(run->inputs.begin(),run->inputs.end(),tick-lifetime,[](auto const& in,int t){return in.tick<t;});
    int drawn=0;
    for(auto it=start;it!=run->inputs.end() && it->tick<=tick && drawn<100;++it) {
        size_t i=static_cast<size_t>(it-run->inputs.begin());
        if((!releases&&!it->down)||(player&&it->player+1!=player)) continue;
        Window w=i<run->windows.size()?run->windows[i]:Window{};
        ccColor3B c=w.measured&&!w.overflow?color(w.width()):ccColor3B{170,175,185};
        CCPoint p{it->x,it->y};
        float alpha=std::clamp(1.f-static_cast<float>(tick-it->tick)/lifetime,0.15f,1.f);
        ring(circles,p,radius,{0.02f,0.03f,0.06f,alpha*0.82f},rgba(c,alpha),1.15f);
        ring(circles,p,std::max(1.f,radius-1.8f),{0.f,0.f,0.f,0.f},rgba(ccColor3B{215,245,255},alpha*0.45f),0.5f);
        ring(circles,{p.x+radius*0.46f,p.y+radius*0.46f},std::max(1.1f,radius*0.16f),rgba(c,alpha),rgba(ccColor3B{240,255,255},alpha),0.45f);
        auto text=label(circleText,0.25f);
        text->setAnchorPoint({0.5f,0.f});
        text->setPosition({p.x,p.y+radius+1.8f});
        text->setColor(c);
        text->setOpacity(static_cast<GLubyte>(alpha*255));
        text->setString((w.measured?(w.overflow?std::string("21+"):std::to_string(w.width())):std::string("?")).c_str());
        ++drawn;
    }
}
}
