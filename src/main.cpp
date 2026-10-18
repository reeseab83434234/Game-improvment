// Predictive Input Buffer - Geode mod
//
// Idea: when you press jump while the player can't act yet (e.g. cube still
// in the air), the press is SAVED for up to N physics ticks and fired the
// moment the player becomes ready. Every saved press is logged to a live
// console window, similar to:
//
//   [InputBuffer] *** SAVED +2 TICKS (0.50 frames @ 60fps) PRESS! (Frame0: 8 -> Delayed: 10) [Limit: 4] ***
//
// 1 tick = 1/240s = 0.25 frames at 60fps.
// F6 cycles: STAGE 1 -> 2 -> 3 -> 4 -> 5 (OFF) -> STAGE 1 ...

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

#include <deque>
#include <cstdio>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;

namespace ib {

struct Stage {
    const char* name;
    int allLimit;   // max ticks to hold a press (cube, ship, ball, ...)
    int waveLimit;  // max ticks to hold a press in wave
    bool off;
};

constexpr Stage STAGES[] = {
    {"STAGE 1",  2, 2, false},
    {"STAGE 2",  4, 4, false},
    {"STAGE 3",  8, 8, false},
    {"STAGE 4", 16, 8, false},
    {"STAGE 5",  0, 0, true },
};
constexpr int STAGE_COUNT = sizeof(STAGES) / sizeof(STAGES[0]);

struct Pending {
    int button;
    int64_t arrivalTick;
};

inline int stageIdx = 0;
inline int64_t tickCounter = 0;
inline int64_t lastChangeTick = 0;
inline std::deque<Pending> queue;
inline bool consoleOpen = false;

// ---- console ---------------------------------------------------------

inline void openConsole() {
#ifdef GEODE_IS_WINDOWS
    if (consoleOpen) return;
    if (!AllocConsole()) return;
    SetConsoleTitleA("Geode Predictive Input Buffer - Live Console");
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    consoleOpen = true;
#endif
}

inline void say(std::string const& msg) {
    log::info("{}", msg);
    if (consoleOpen) {
        std::printf("[InputBuffer] %s\n", msg.c_str());
        std::fflush(stdout);
    }
}

inline float ticksToFrames(int ticks) { return ticks / 4.0f; }

inline std::string describeStage(int idx) {
    auto const& s = STAGES[idx];
    if (s.off) return fmt::format("{}: OFF (100% Raw Input)", s.name);
    if (s.allLimit == s.waveLimit) {
        return fmt::format(
            "{}: {} Ticks All | {} Ticks Wave ({:.1f}f @ 60fps)",
            s.name, s.allLimit, s.waveLimit, ticksToFrames(s.allLimit)
        );
    }
    return fmt::format(
        "{}: {} Ticks All | {} Ticks Wave ({:.1f}f / {:.1f}f @ 60fps)",
        s.name, s.allLimit, s.waveLimit,
        ticksToFrames(s.allLimit), ticksToFrames(s.waveLimit)
    );
}

inline bool enabled() { return !STAGES[stageIdx].off; }

inline int limitFor(PlayerObject* p) {
    auto const& s = STAGES[stageIdx];
    return p->m_isDart ? s.waveLimit : s.allLimit;
}

// "Ready" = the game would actually accept the press as a meaningful input.
inline bool isReady(PlayerObject* p) {
    if (p->m_isDart) {
        // wave: just avoid sub-tick flicker between input changes
        return tickCounter - lastChangeTick >= 1;
    }
    return p->m_isOnGround || p->m_isShip || p->m_isBird || p->m_isSwing;
}

} // namespace ib

// ---- startup -----------------------------------------------------------

$on_mod(Loaded) {
    ib::stageIdx = static_cast<int>(Mod::get()->getSettingValue<int64_t>("start-stage")) - 1;
    if (ib::stageIdx < 0 || ib::stageIdx >= ib::STAGE_COUNT) ib::stageIdx = 0;

    if (Mod::get()->getSettingValue<bool>("show-console")) {
        ib::openConsole();
    }
    ib::say("Starting Mode: " + ib::describeStage(ib::stageIdx));
    ib::say("Press F6 in-level to cycle: 2/2 -> 4/4 -> 8/8 -> 16/8 -> OFF");
    if (ib::consoleOpen) {
        std::printf("==================================================\n");
    }
}

// ---- F6 cycling ----------------------------------------------------------
// NOTE: GD 2.2081 / Geode v5 include a `double timestamp` parameter here.
// If your build errors on the signature, check the CCKeyboardDispatcher
// binding in your SDK and match it exactly.

class $modify(IBKeyboard, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down && !repeat && key == KEY_F6) {
            ib::stageIdx = (ib::stageIdx + 1) % ib::STAGE_COUNT;
            ib::queue.clear();
            ib::say("F6 Cycle -> " + ib::describeStage(ib::stageIdx));
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};

// ---- buffer core -----------------------------------------------------------

class $modify(IBLayer, GJBaseGameLayer) {
    struct Fields {
        bool replaying = false;
    };

    // Intercept raw button events.
    void handleButton(bool down, int button, bool isPlayer1) {
        auto passthrough = [&] { GJBaseGameLayer::handleButton(down, button, isPlayer1); };

        if (m_fields->replaying || !ib::enabled() || !isPlayer1 || button != 1) {
            return passthrough();
        }

        if (down) {
            if (ib::isReady(m_player1) && ib::queue.empty()) {
                ib::lastChangeTick = ib::tickCounter;
                return passthrough();
            }
            // Can't act yet: save the press.
            ib::queue.push_back({button, ib::tickCounter});
            return;
        }

        // Release while a press is still pending: fire the press first so
        // quick taps are not lost, then let the release through.
        if (!ib::queue.empty()) {
            auto p = ib::queue.front();
            ib::queue.pop_front();
            m_fields->replaying = true;
            GJBaseGameLayer::handleButton(true, p.button, true);
            m_fields->replaying = false;
            ib::say(fmt::format(
                "*** FLUSHED on release (Frame0: {} -> Delayed: {}) ***",
                p.arrivalTick, ib::tickCounter
            ));
        }
        ib::lastChangeTick = ib::tickCounter;
        passthrough();
    }

    // Runs once per physics step: advance our tick clock and release
    // buffered presses when the player is ready (or the limit expires).
    void processCommands(float dt) {
        ib::tickCounter++;

        if (ib::enabled() && !ib::queue.empty() && m_player1) {
            auto const& front = ib::queue.front();
            int64_t elapsed = ib::tickCounter - front.arrivalTick;
            int limit = ib::limitFor(m_player1);

            if (ib::isReady(m_player1) || elapsed >= limit) {
                auto p = front;
                ib::queue.pop_front();

                m_fields->replaying = true;
                GJBaseGameLayer::handleButton(true, p.button, true);
                m_fields->replaying = false;
                ib::lastChangeTick = ib::tickCounter;

                ib::say(fmt::format(
                    "*** SAVED +{} TICKS ({:.2f} frames @ 60fps) PRESS! "
                    "(Frame0: {} -> Delayed: {}) [Limit: {}] ***",
                    elapsed, ib::ticksToFrames(static_cast<int>(elapsed)),
                    p.arrivalTick, ib::tickCounter, limit
                ));
            }
        }

        GJBaseGameLayer::processCommands(dt);
    }
};

// Clear stale buffered inputs on respawn.
class $modify(IBPlayLayer, PlayLayer) {
    void resetLevel() {
        ib::queue.clear();
        PlayLayer::resetLevel();
    }
};
