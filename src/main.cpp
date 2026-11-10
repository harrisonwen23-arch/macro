// main.cpp - "Frame Window Counter" overlay (all timing standardised to 240 Hz)
//
// This replaces the old macro-bot main.cpp. Everything lives in namespace fwc and the hook
// classes are prefixed FWC.
//
// HOW IT WORKS, in one paragraph:
//   Once per physics step we take the cube's current state, find the next cluster of hazards
//   ahead of it, and ask "if I click N steps from now, do I survive?" for every N. We answer by
//   running a tiny cube-physics simulation (see survives()). The N values that survive form the
//   window: lo = first surviving N, hi = last surviving N, width = how many survive. Width is
//   in 240 Hz physics steps ("frames"), and that number drives the colour, the text and the bar.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace fwc {

// ============================================================================================
// 1. TIMING + PHYSICS CONSTANTS  (the knobs you will tweak most)
// ============================================================================================

// GD's physics constants are expressed per 60 Hz frame. One 240 Hz step is a quarter of that,
// so every per-frame quantity is multiplied by kFramesPer60 when it is applied once per step.
constexpr float kHz          = 240.f;
constexpr float kStepDt      = 1.f / kHz;       // real seconds per physics step
constexpr float kFramesPer60 = 60.f / kHz;      // 0.25: converts "per 60 Hz frame" -> "per step"

// Cube jump model (APPROXIMATION: tune these if the reported windows disagree with what you
// measure with your replay-based window test).
constexpr float kJumpVel   = 11.180032f;        // initial upward speed, units per 60 Hz frame
constexpr float kGravity   = 0.958199f;         // units per 60 Hz frame^2
constexpr float kTermVel   = 15.f;              // max fall speed, units per 60 Hz frame
constexpr float kHalfBox   = 15.f;              // half of the 30x30 cube hitbox

// Horizontal speed in units per real second, indexed by the speed portal multiplier.
// (Not linear in the multiplier, so it is a lookup table.)
struct SpeedEntry { float mult; float unitsPerSec; };
constexpr SpeedEntry kSpeeds[] = {
    { 0.7f, 251.16f }, { 0.9f, 311.58f }, { 1.1f, 387.42f }, { 1.3f, 468.0f }, { 1.6f, 576.0f },
};

// Search limits
constexpr int   kMaxSimSteps = 480;             // give up on a simulation after 2 seconds
constexpr float kClusterGap  = 90.f;            // hazards closer than this are treated as one obstacle
constexpr float kLookahead   = 700.f;           // ignore obstacles further away than this (units)

// HUD
constexpr int   kCells     = 16;                // segments in the bar; one segment = one frame of window
constexpr float kHudWidth  = 140.f;
constexpr float kHudHeight = 62.f;

// ============================================================================================
// 2. STATE
// ============================================================================================

enum class State {
    Idle,           // airborne: a click now is not a jump, so no window is computed
    Unsupported,    // not a plain cube at 1x speed, normal gravity
    NoHazard,       // nothing ahead within kLookahead
    NoClickNeeded,  // the obstacle is cleared without jumping
    Open,           // there is a window: lo/hi/width are valid
    Missed,         // no click timing survives any more
};

struct Window {
    State state = State::Idle;
    int lo = 0;     // steps from now until the window opens
    int hi = 0;     // steps from now until the window closes
    int width = 0;  // number of surviving click steps = THE number we display (240 Hz frames)
};

struct Globals {
    std::vector<CCRect> hazards;   // snapshot of every hazard hitbox, sorted by left edge
    Window last;
    int lastStep = -1;             // last physics step we computed on
    std::string clickMsg;          // result of the player's most recent click
} g;

static bool enabled() {
    return Mod::get()->getSettingValue<bool>("enabled");
}

// ============================================================================================
// 3. WINDOW CALCULATION
// ============================================================================================

static float unitsPerSecond(float speedMult) {
    SpeedEntry best = kSpeeds[1];
    float bestDiff = 1e9f;
    for (auto const& e : kSpeeds) {
        float d = std::abs(e.mult - speedMult);
        if (d < bestDiff) { bestDiff = d; best = e; }
    }
    return best.unitsPerSec;
}

// Finds the next obstacle ahead of x: the first hazard not yet passed, plus every following
// hazard that is within kClusterGap of the previous one (so a double spike is ONE obstacle).
static bool findCluster(float x, std::vector<CCRect>& out, float& clusterMaxX) {
    out.clear();
    // Hazards are sorted by left edge; start a little to the left so we do not skip one
    // that is currently overlapping the player.
    auto it = std::lower_bound(g.hazards.begin(), g.hazards.end(), x - 60.f,
        [](CCRect const& r, float v) { return r.origin.x < v; });
    for (; it != g.hazards.end(); ++it) {
        float maxX = it->origin.x + it->size.width;
        if (maxX > x - kHalfBox) break;       // first hazard that is still ahead (or under us)
    }
    if (it == g.hazards.end()) return false;
    if (it->origin.x - x > kLookahead) return false;

    clusterMaxX = it->origin.x + it->size.width;
    out.push_back(*it);
    for (++it; it != g.hazards.end(); ++it) {
        if (it->origin.x - clusterMaxX > kClusterGap) break;
        out.push_back(*it);
        clusterMaxX = std::max(clusterMaxX, it->origin.x + it->size.width);
    }
    return true;
}

// The core of the tool: simulate the cube at 240 Hz.
//   clickStep  = the step (counted from now) on which the jump input is applied, or -1 = never.
// Returns true if the cube gets past the whole cluster without touching a hazard.
//
// MAPPING TO THE OVERLAY: the loop below advances one 240 Hz step per iteration, so the
// click step "t" that survives IS a frame count. Counting how many t survive gives the window.
static bool survives(float x, float groundY, float vx, int clickStep,
                     std::vector<CCRect> const& hz, float clusterMaxX) {
    float y = groundY, vy = 0.f;
    bool air = false;
    float const gStep = kGravity * kFramesPer60;   // vy lost per step
    float const dyMul = kFramesPer60;              // converts vy (per 60 Hz frame) to y change per step
    float const dxStep = vx * kStepDt;             // horizontal distance per step

    for (int t = 0; t < kMaxSimSteps; ++t) {
        if (t == clickStep) { vy = kJumpVel; air = true; }
        if (air) {
            vy = std::max(vy - gStep, -kTermVel);
            y += vy * dyMul;
            if (y <= groundY && vy < 0.f) { y = groundY; vy = 0.f; air = false; } // landed
        }
        x += dxStep;

        CCRect player(x - kHalfBox, y - kHalfBox, kHalfBox * 2.f, kHalfBox * 2.f);
        for (auto const& r : hz) {
            if (player.intersectsRect(r)) return false;
        }
        if (x - kHalfBox > clusterMaxX) return true;   // fully past the obstacle
    }
    return true;
}

static Window computeWindow(PlayLayer* pl) {
    Window w;
    auto p = pl->m_player1;
    if (!p) return w;

    // Only a plain cube at 1x speed is modelled. Extend here for other gamemodes.
    if (p->m_isShip || p->m_isBird || p->m_isBall || p->m_isDart || p->m_isRobot ||
        p->m_isSpider || p->m_isSwing || p->m_isUpsideDown ||
        std::abs(pl->m_gameState.m_timeWarp - 1.f) > 0.01f) {
        w.state = State::Unsupported;
        return w;
    }
    if (!p->m_isOnGround) return w;   // State::Idle

    float px = p->getPositionX();
    float py = p->getPositionY();
    float vx = unitsPerSecond(p->m_playerSpeed);

    static std::vector<CCRect> cluster;   // reused to avoid reallocating every step
    float clusterMaxX = 0.f;
    if (!findCluster(px, cluster, clusterMaxX)) { w.state = State::NoHazard; return w; }

    if (survives(px, py, vx, -1, cluster, clusterMaxX)) { w.state = State::NoClickNeeded; return w; }

    // A jump that starts after the cluster has already gone by is pointless, so only test
    // click steps up to the moment the cube reaches the far edge of the cluster.
    float stepsToEdge = (clusterMaxX + kHalfBox - px) / (vx * kStepDt);
    int maxDelay = std::clamp(static_cast<int>(std::ceil(stepsToEdge)), 0, kMaxSimSteps);

    int first = -1, last = -1, count = 0;
    for (int d = 0; d <= maxDelay; ++d) {
        if (survives(px, py, vx, d, cluster, clusterMaxX)) {
            if (first < 0) first = d;
            last = d;
            ++count;
        }
    }
    if (count == 0) { w.state = State::Missed; return w; }

    w.state = State::Open;
    w.lo = first;
    w.hi = last;
    w.width = count;
    return w;
}

// ============================================================================================
// 4. HUD
// ============================================================================================

static ccColor3B colorFor(Window const& w) {
    switch (w.state) {
        case State::Open: {
            int wide  = static_cast<int>(Mod::get()->getSettingValue<int64_t>("wide-threshold"));
            int tight = static_cast<int>(Mod::get()->getSettingValue<int64_t>("tight-threshold"));
            if (w.width >= wide)  return { 90, 225, 110 };   // green: comfortable
            if (w.width >= tight) return { 255, 210, 60 };   // yellow: tight
            return { 255, 70, 70 };                          // red: frame-perfect / near impossible
        }
        case State::Missed:        return { 150, 25, 25 };
        case State::NoClickNeeded: return { 90, 225, 110 };
        default:                   return { 170, 170, 170 };
    }
}

class Hud : public CCNode {
public:
    static Hud* create() {
        auto r = new Hud();
        if (r->init()) { r->autorelease(); return r; }
        delete r;
        return nullptr;
    }

    void show(Window const& w, std::string const& clickMsg) {
        char title[48];
        char sub[64] = "";
        switch (w.state) {
            case State::Open:
                std::snprintf(title, sizeof(title), "%d frame window", w.width);
                // Steps -> milliseconds: one step is 1000/240 = 4.17 ms.
                std::snprintf(sub, sizeof(sub), "opens +%df  closes +%df  (%.1f ms)",
                    w.lo, w.hi, w.width * 1000.f / kHz);
                break;
            case State::Missed:        std::snprintf(title, sizeof(title), "MISSED"); break;
            case State::NoClickNeeded: std::snprintf(title, sizeof(title), "no click needed"); break;
            case State::NoHazard:      std::snprintf(title, sizeof(title), "clear"); break;
            case State::Unsupported:   std::snprintf(title, sizeof(title), "cube only"); break;
            default:                   std::snprintf(title, sizeof(title), "-"); break;
        }

        auto c = colorFor(w);
        m_title->setString(title);
        m_title->setColor(c);
        m_sub->setString(sub);
        m_click->setString(clickMsg.c_str());

        // Bar: one segment per frame of window (capped at kCells), coloured like the text.
        int filled = (w.state == State::Open) ? std::min(w.width, kCells) : 0;
        for (int i = 0; i < kCells; ++i) {
            bool on = i < filled;
            m_cells[i]->setColor(on ? c : ccColor3B{ 70, 70, 70 });
            m_cells[i]->setOpacity(on ? 255 : 90);
        }
    }

protected:
    bool init() override {
        if (!CCNode::init()) return false;
        this->setID("fwc-hud");

        // The node's origin is its TOP-LEFT corner; children extend downward (negative y).
        m_bg = CCLayerColor::create(ccc4(0, 0, 0, 110), kHudWidth, kHudHeight);
        m_bg->setPosition({ 0.f, -kHudHeight });
        this->addChild(m_bg);

        m_title = CCLabelBMFont::create("-", "bigFont.fnt");
        m_title->setAnchorPoint({ 0.f, 1.f });
        m_title->setScale(0.45f);
        m_title->setPosition({ 6.f, -4.f });
        this->addChild(m_title);

        m_sub = CCLabelBMFont::create("", "chatFont.fnt");
        m_sub->setAnchorPoint({ 0.f, 1.f });
        m_sub->setScale(0.5f);
        m_sub->setPosition({ 6.f, -24.f });
        this->addChild(m_sub);

        m_click = CCLabelBMFont::create("", "chatFont.fnt");
        m_click->setAnchorPoint({ 0.f, 1.f });
        m_click->setScale(0.5f);
        m_click->setPosition({ 6.f, -35.f });
        this->addChild(m_click);

        float cellW = (kHudWidth - 12.f) / kCells;
        for (int i = 0; i < kCells; ++i) {
            auto cell = CCLayerColor::create(ccc4(255, 255, 255, 255), cellW - 1.f, 6.f);
            cell->setPosition({ 6.f + i * cellW, -kHudHeight + 5.f });
            this->addChild(cell);
            m_cells[i] = cell;
        }
        return true;
    }

private:
    CCLayerColor* m_bg = nullptr;
    CCLabelBMFont* m_title = nullptr;
    CCLabelBMFont* m_sub = nullptr;
    CCLabelBMFont* m_click = nullptr;
    std::array<CCLayerColor*, kCells> m_cells {};
};

static Hud* findHud(PlayLayer* pl) {
    if (!pl || !pl->m_uiLayer) return nullptr;
    return static_cast<Hud*>(pl->m_uiLayer->getChildByID("fwc-hud"));
}

// ============================================================================================
// 5. HOOK HELPERS
// ============================================================================================

// Called after every processCommands. processCommands can run more than once per physics
// step, so we only recompute when the step counter actually moved.
static void onStep(GJBaseGameLayer* layer) {
    auto pl = PlayLayer::get();
    if (!pl || static_cast<GJBaseGameLayer*>(pl) != layer || !enabled()) return;

    int step = static_cast<int>(pl->m_gameState.m_currentProgress);
    if (step == g.lastStep) return;
    g.lastStep = step;

    g.last = computeWindow(pl);
    if (auto hud = findHud(pl)) hud->show(g.last, g.clickMsg);
}

// Called when the player presses jump. We evaluate the window at THIS instant (delay 0 = the
// step that will apply the click) and report how that click sat inside it.
static void onClick(GJBaseGameLayer* layer) {
    auto pl = PlayLayer::get();
    if (!pl || static_cast<GJBaseGameLayer*>(pl) != layer || !enabled()) return;

    Window w = computeWindow(pl);
    char buf[64];
    if (w.state == State::Open) {
        if (w.lo > 0) std::snprintf(buf, sizeof(buf), "click: %df early", w.lo);
        else          std::snprintf(buf, sizeof(buf), "click: ok, %df to spare of %d", w.hi, w.width);
    } else if (w.state == State::Missed) {
        std::snprintf(buf, sizeof(buf), "click: outside the window");
    } else {
        return;   // airborne / no obstacle: nothing meaningful to report
    }
    g.clickMsg = buf;
}

static void buildHazards(PlayLayer* pl) {
    g.hazards.clear();
    for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
        if (obj->m_objectType != GameObjectType::Hazard) continue;
        // STATIC SNAPSHOT: hazards moved by triggers are not tracked. Turn on "show hitboxes"
        // in-game once to sanity-check that these rects match what you see.
        g.hazards.push_back(obj->getObjectRect());
    }
    std::sort(g.hazards.begin(), g.hazards.end(),
        [](CCRect const& a, CCRect const& b) { return a.origin.x < b.origin.x; });
}

} // namespace fwc

// ============================================================================================
// 6. HOOKS
// ============================================================================================

class $modify(FWCBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        // Sample BEFORE the original so we see the state the click is about to be applied to.
        // (Releases are not evaluated yet, see the notes about extending to release windows.)
        if (down && button == 1 && isPlayer1) fwc::onClick(this);
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        fwc::onStep(this);
    }
};

class $modify(FWCPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        fwc::g.last = fwc::Window();
        fwc::g.lastStep = -1;
        fwc::g.clickMsg.clear();
        fwc::buildHazards(this);

        if (m_uiLayer && fwc::enabled()) {
            auto hud = fwc::Hud::create();
            auto ws = CCDirector::get()->getWinSize();
            hud->setPosition({ 8.f, ws.height - 8.f });   // top-left corner
            hud->show(fwc::g.last, fwc::g.clickMsg);
            m_uiLayer->addChild(hud, 150);
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        fwc::g.lastStep = -1;
        fwc::g.clickMsg.clear();
    }
};
