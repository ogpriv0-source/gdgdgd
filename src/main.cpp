// Click Snap - real-time "input buffer" assist.
//
// You press early -> the press is held back. Every physics step we test (by
// simulating ahead from a checkpoint) whether pressing one step LATER would
// still survive. If yes, keep waiting. If no, fire now. A press that is never
// held back further than "max early steps". Late presses are never touched.
//
// NOTE: written without being able to compile/run GD. Names marked CHECK may
// need adjusting to your Geode bindings version.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <deque>

using namespace geode::prelude;

namespace {
    struct Ev { bool down; int64_t step; };

    struct Buffer {
        std::deque<Ev> q;
        int64_t step = 0;
        int64_t offset = 0;      // delay applied to the last fired press
        float lastDt = 1.f / 240.f;
        bool simulating = false;
        bool simDied = false;
        bool realDown = false;   // what the game currently believes

        void reset() { q.clear(); step = 0; offset = 0; realDown = false; simulating = false; }
    };
    Buffer g;

    bool on()        { return Mod::get()->getSettingValue<bool>("enabled"); }
    int  maxEarly()  { return (int)Mod::get()->getSettingValue<int64_t>("max-early"); }
    int  lookahead() { return (int)Mod::get()->getSettingValue<int64_t>("lookahead"); }
    bool flyingOk()  { return Mod::get()->getSettingValue<bool>("flying"); }
}

class $modify(CSLayer, GJBaseGameLayer) {

    bool bufferable() {
        if (!on() || this != PlayLayer::get()) return false;
        auto p = m_player1;
        if (!p) return false;
        if (!flyingOk() && (p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing)) return false;
        return true;
    }

    // Would a tap started `delay` steps from now survive `lookahead` steps?
    bool survivesIfPressedIn(int delay) {
        auto pl = PlayLayer::get();
        if (!pl) return true;

        CheckpointObject* snap = pl->createCheckpoint();   // CHECK: exists in your bindings
        if (!snap) return true;
        snap->retain();

        g.simulating = true;
        g.simDied = false;

        int total = delay + lookahead();
        for (int i = 0; i < total && !g.simDied; ++i) {
            if (i == delay)     GJBaseGameLayer::handleButton(true, 1, true);
            if (i == delay + 2) GJBaseGameLayer::handleButton(false, 1, true);
            GJBaseGameLayer::processCommands(g.lastDt);
        }

        pl->loadFromCheckpoint(snap);                       // CHECK
        snap->release();
        // put the real button state back
        GJBaseGameLayer::handleButton(g.realDown, 1, true);

        g.simulating = false;
        bool ok = !g.simDied;
        g.simDied = false;
        return ok;
    }

    void forwardEvent(bool down) {
        GJBaseGameLayer::handleButton(down, 1, true);
        g.realDown = down;
    }

    void flushBuffer() {
        while (!g.q.empty()) {
            Ev e = g.q.front();
            if (e.down) {
                int64_t age = g.step - e.step;
                bool fire = age >= maxEarly();
                // wait only if pressing one step later is still survivable
                if (!fire) fire = !this->survivesIfPressedIn(1);
                if (!fire) break;
                g.offset = age;
                this->forwardEvent(true);
            } else {
                // keep the original hold length
                if (g.step < e.step + g.offset) break;
                this->forwardEvent(false);
            }
            g.q.pop_front();
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (g.simulating || button != 1 || !isPlayer1) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        // Keep ordering correct if events are already queued.
        if (g.q.empty() && !this->bufferable()) {
            g.realDown = down;
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        g.q.push_back({down, g.step});
    }

    void processCommands(float dt) {
        if (g.simulating) return GJBaseGameLayer::processCommands(dt);
        g.lastDt = dt;
        if (this == PlayLayer::get()) this->flushBuffer();
        GJBaseGameLayer::processCommands(dt);
        g.step++;
    }
};

class $modify(CSPlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (g.simulating) { g.simDied = true; return; }   // test run only, don't really die
        PlayLayer::destroyPlayer(player, obj);
    }
    void resetLevel() {
        g.reset();
        PlayLayer::resetLevel();
    }
};
