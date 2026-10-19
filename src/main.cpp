#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <deque>
#include <chrono>

using namespace geode::prelude;

namespace {
    struct Ev { bool down; int64_t step; };

    struct Buffer {
        std::deque<Ev> q;
        int64_t step = 0;
        int64_t offset = 0;
        int64_t lastPressFireStep = -1;
        float lastDt = 1.f / 240.f;
        bool simulating = false;
        bool simDied = false;
        bool realDown = false;
        int64_t skipUntil = 0;
        void reset() {
            q.clear();
            step = 0;
            offset = 0;
            lastPressFireStep = -1;
            realDown = false;
            simulating = false;
            skipUntil = 0;
        }
    };
    Buffer g;
    bool on()        { return Mod::get()->getSettingValue<bool>("enabled"); }
    int  maxEarly()  { return (int)Mod::get()->getSettingValue<int64_t>("max-early"); }
    int  lookahead() { return (int)Mod::get()->getSettingValue<int64_t>("lookahead"); }
    bool flyingOk()  { return Mod::get()->getSettingValue<bool>("flying"); }
    double limitMs() { return (double)Mod::get()->getSettingValue<int64_t>("time-limit-ms"); }
    bool dbg()       { return Mod::get()->getSettingValue<bool>("debug"); }
    bool isPlayLayer(GJBaseGameLayer* layer) {
        return layer != nullptr && layer == static_cast<GJBaseGameLayer*>(PlayLayer::get());
    }
}

class $modify(CSLayer, GJBaseGameLayer) {
    const char* whyNot() {
        if (!on()) return "mod disabled";
        if (!isPlayLayer(this)) return "not a PlayLayer";
        if (g.step < 120) return "level just started";
        if (g.step < g.skipUntil) return "paused (simulation was too slow)";
        auto p = m_player1;
        if (!p) return "no player";
        if (!flyingOk() && (p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing)) return "flying mode";
        return nullptr;
    }

    bool survivesIfPressedIn(int delay) {
        auto pl = PlayLayer::get();
        if (!pl) return true;

        CheckpointObject* snap = pl->createCheckpoint();
        if (!snap) return true;
        snap->retain();

        g.simulating = true;
        g.simDied = false;
        int total = delay + lookahead();
        for (int i = 0; i < total && !g.simDied; ++i) {
            if (i == delay)     GJBaseGameLayer::handleButton(true, 1, true);
            if (i == delay + 2) GJBaseGameLayer::handleButton(false, 1, true);
            GJBaseGameLayer::processCommands(g.lastDt, false, true);
        }

        pl->loadFromCheckpoint(snap);
        snap->release();
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
        if (this->whyNot()) {
            while (!g.q.empty()) {
                this->forwardEvent(g.q.front().down);
                g.q.pop_front();
            }
            return;
        }

        while (!g.q.empty()) {
            Ev e = g.q.front();
            if (e.down) {
                int64_t age = g.step - e.step;
                bool fire = age >= maxEarly();
                const char* reason = fire ? "reached max early steps" : "";
                if (!fire) {
                    auto t0 = std::chrono::steady_clock::now();
                    bool safe = this->survivesIfPressedIn(1);
                    double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
                    if (dbg()) log::info("[ClickSnap] test: age={} later-press-safe={} took {:.2f} ms", age, safe, ms);
                    if (ms > limitMs()) {
                        g.skipUntil = g.step + 480;
                        fire = true;
                        reason = "simulation too slow, pausing";
                    } else if (!safe) {
                        fire = true;
                        reason = "waiting longer would fail";
                    }
                }
                if (!fire) break;
                if (dbg()) log::info("[ClickSnap] FIRE press after holding {} steps ({})", age, reason);
                g.offset = age;
                g.lastPressFireStep = g.step;
                this->forwardEvent(true);
            } else {
                // A very short click can have its release queued for the same
                // step as its press. Never send both edges in one physics step:
                // that can collapse the buffered click into an unregistered tap.
                if (g.step <= g.lastPressFireStep) break;
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
        const char* why = this->whyNot();
        if (g.q.empty() && why) {
            if (down && dbg()) log::info("[ClickSnap] press passed through: {}", why);
            g.realDown = down;
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        if (down && dbg()) log::info("[ClickSnap] press queued at step {}", g.step);
        g.q.push_back({down, g.step});
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (g.simulating) return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        g.lastDt = dt;
        if (isPlayLayer(this)) this->flushBuffer();
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        g.step++;
    }
};

class $modify(CSPlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (g.simulating) { g.simDied = true; return; }
        PlayLayer::destroyPlayer(player, obj);
    }
    void resetLevel() {
        g.reset();
        PlayLayer::resetLevel();
    }
};
