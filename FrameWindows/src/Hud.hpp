#pragma once
#include "Session.hpp"
namespace fwl {
class Hud : public cocos2d::CCNode {
    cocos2d::CCDrawNode* panel=nullptr;
    cocos2d::CCLabelBMFont* title=nullptr;
    cocos2d::CCLabelBMFont* status=nullptr;
    cocos2d::CCLabelBMFont* totals=nullptr;
    cocos2d::CCLabelBMFont* footer=nullptr;
    std::array<cocos2d::CCLabelBMFont*,20> rows{};
    cocos2d::CCDrawNode* circles=nullptr;
    cocos2d::CCNode* circleText=nullptr;
    float refresh=0;
    int lastMarkerTick=-1;
public:
    static Hud* create(PlayLayer* pl);
    bool initWithLayer(PlayLayer* pl);
    void update(float dt) override;
    void render();
};
}
