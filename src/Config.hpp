#pragma once
#include <Geode/Geode.hpp>
#include <algorithm>

namespace buf {

enum Mode { M_CUBE = 0, M_SHIP, M_BALL, M_WAVE, M_ROBOT, M_SPIDER, M_UFO, M_SWING, M_COUNT };

inline constexpr char const* kModeNames[M_COUNT] = {
    "Cube", "Ship", "Ball", "Wave", "Robot", "Spider", "UFO", "Swing"
};

struct Config {
    // general
    bool enabled = true;
    bool showTrajectory = true;         // draw the predicted path, always (not only after a click)
    bool logEnabled = false;            // rescue log (first page of the settings window)
    bool pauseButton = true;            // BUF button in the pause menu (Geode setting "pause-button")
    bool dualSupport = true;            // 1-player dual: one input drives both players
    bool bothSides = true;              // solo: both screen sides are player 1
    bool orbDetect = true;
    bool dropFatal = false;
    bool subTick = true;                // click inside a tick, at the exact spot the ghost found (CBF-like)
    bool twoPlayerMode = true;          // handle real 2-player mode (off = untouched)
    bool swapTwoPlayer = true;          // unused since the side -> player mapping is learned; kept so old saves load
    bool moveDetect = true;             // a running move trigger makes the prediction back off
    bool cbfSupport = true;             // Click Between Frames active: leave the step splitting to it (no own sub-tick)
    bool saveFx = true;                 // ring around the player when the mod saves an input
    bool lateRescue = true;             // a click that would be fatal is moved BACK in time (see maxLate)
    // display
    int previewLen = 60;                // ticks of the live preview
    int previewEvery = 6;               // ticks between preview refreshes
    int keepTicks = 240;                // how long the last click path stays on screen
    int lineWidth = 8;                  // 1/10 px
    bool previewBoth = true;            // draw BOTH paths: "if you hold" and "if you release"
    // show path: scans a feasible hold/release route and only DRAWS it (no effect on the game)
    bool showPath = false;
    int pathHorizon = 90;               // ticks looked ahead
    int pathEvery = 6;                  // ticks between rescans
    int pathBudget = 2500;              // simulated ticks per scan
    int trajOpacity = 100;              // %

    // advanced search
    int settle = 6;                     // ticks on the ground that count as a stable landing
    int airHorizon = 45;                // UFO / Swing look-ahead cap
    int robotHold = 18;                 // robot long-hold length
    int gainClick = 6;                  // extra ticks a delay must survive to be preferred
    int minBudget = 300;                // smallest search budget under spam
    int noclipTicks = 480;              // how long NOCLIP stays flagged after a blocked death
    int failOpen = 4;                   // frames without a physics tick before input is never held

    // ship / wave: only act when holding or releasing NOW is about to kill you
    int shipImminent = 16;              // ticks: death sooner than this counts as imminent
    int waveImminent = 10;
    int holdGain = 4;                   // a delay must push the death back by this many ticks

    // autoplay (learns from every death, independent of the click assist)
    bool autoplay = false;
    bool autoLearn = true;
    bool autoKeep = false;              // keep what it learned when another level loads
    int autoHorizon = 70;               // ticks looked ahead
    int autoEvery = 4;                  // (unused: routes are replanned at 90%)
    int autoPaths = 3;                  // routes thought up per plan; the longest-living one is followed
    int autoStride = 3;                 // decision step near the player
    int autoBudget = 1500;              // simulated ticks per plan
    int autoCell = 8;                   // memory cell width (px)
    int autoWindow = 48;                // ticks before a death that get blamed
    int autoPrune = 4;                  // blame score after which an action is avoided
    int autoForget = 120;               // ticks after which a survived action is forgiven

    int trajectory = 120;               // lookahead (ticks) for click-style modes

    // per mode: max ticks a press / release may be held back. 0 = mode off.
    // ground modes wait for the landing, so they need a long window (a jump lasts far more than 8 ticks)
    int maxDelay[M_COUNT] = {30, 12, 30, 6, 30, 30, 6, 6};

    // per mode: max ticks a LATE press / release may be moved back in time (0 = off).
    // Only cube, ship, wave and robot are supported: for the other modes a second press
    // would repeat the impulse (UFO / swing / ball / spider), so they stay at 0.
    int maxLate[M_COUNT] = {4, 4, 0, 4, 3, 0, 0, 0};

    // physics (0 = auto detect)
    int tps = 0;
    int fps = 0;

    // search
    // wave: fixed slope the ghost wave follows (angles in 1/100 degree)
    bool waveAngles = false;            // off: the ghost uses the game's own physics
    int waveUp = 4510;                  // 45.10 deg going up
    int waveDown = 4490;                // 44.90 deg going down
    int miniUp = 6343;                  // mini wave >= 63.43 deg
    int miniDown = 6343;

    int tapTicks = 4;                   // assumed length of a tap
    int budget = 3000;                  // max simulated ticks per search
    int shipHorizon = 36;               // ticks a held / released state must survive
    int waveHorizon = 24;
};

inline Config& cfg() {
    static Config c;
    return c;
}

inline int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

inline void saveConfig();

inline void loadConfig() {
    auto* m = geode::Mod::get();
    auto& c = cfg();
    if (m->getSavedValue<int64_t>("cfgVersion", 0) < 3) {
        c = Config(); // settings from older builds had the wrong defaults
        c.pauseButton = m->getSettingValue<bool>("pause-button");   // that one lives in Geode's settings
        saveConfig();
        return;
    }
    auto gb = [&](char const* k, bool d) { return m->getSavedValue<bool>(k, d); };
    auto gi = [&](char const* k, int d, int lo, int hi) {
        return clampi(static_cast<int>(m->getSavedValue<int64_t>(k, d)), lo, hi);
    };
    c.enabled = gb("enabled", true);
    c.showTrajectory = gb("showTraj", true);
    c.logEnabled = gb("logOn", false);
    c.dualSupport = gb("dualOn", true);
    c.pauseButton = m->getSettingValue<bool>("pause-button");
    c.bothSides = gb("bothSides", true);
    c.previewLen = gi("previewLen", 60, 10, 400);
    c.previewEvery = gi("previewEvery", 6, 1, 60);
    c.keepTicks = gi("keepTicks", 240, 0, 2000);
    c.lineWidth = gi("lineWidth", 8, 2, 40);
    c.previewBoth = gb("previewBoth", true);
    c.showPath = gb("showPath", false);
    c.pathHorizon = gi("pathHorizon", 90, 20, 300);
    c.pathEvery = gi("pathEvery", 6, 1, 60);
    c.pathBudget = gi("pathBudget", 2500, 200, 20000);
    c.trajOpacity = gi("trajOpacity", 100, 10, 100);
    c.settle = gi("settle", 6, 1, 30);
    c.airHorizon = gi("airHorizon", 45, 10, 300);
    c.robotHold = gi("robotHold", 18, 1, 60);
    c.gainClick = gi("gainClick", 6, 1, 60);
    c.minBudget = gi("minBudget", 300, 50, 5000);
    c.noclipTicks = gi("noclipTicks", 480, 0, 5000);
    c.failOpen = gi("failOpen", 4, 1, 60);
    c.orbDetect = gb("orbDetect", true);
    c.dropFatal = gb("dropFatal", false);
    c.subTick = gb("subTick", true);
    c.twoPlayerMode = gb("twoPlayerOn", true);
    c.moveDetect = gb("moveDetect", true);
    c.cbfSupport = gb("cbfOn", true);
    c.lateRescue = gb("lateOn", true);
    c.saveFx = gb("fxOn", true);
    c.swapTwoPlayer = gb("swapTwoP", true);
    c.waveAngles = gb("waveAnglesOn", false);
    c.waveUp = gi("waveUp", 4510, 3000, 8000);
    c.waveDown = gi("waveDown", 4490, 3000, 8000);
    c.miniUp = gi("miniUp", 6343, 3000, 8500);
    c.miniDown = gi("miniDown", 6343, 3000, 8500);
    c.trajectory = gi("trajectory", 120, 20, 400);
    c.shipImminent = gi("shipImminent", 16, 2, 120);
    c.waveImminent = gi("waveImminent", 10, 2, 120);
    c.holdGain = gi("holdGain", 4, 1, 30);
    c.autoplay = gb("autoplay", false);
    c.autoLearn = gb("autoLearn", true);
    c.autoKeep = gb("autoKeep", false);
    c.autoHorizon = gi("autoHorizon", 70, 20, 300);
    c.autoEvery = gi("autoEvery", 4, 1, 30);
    c.autoPaths = gi("autoPaths", 3, 1, 12);
    c.autoStride = gi("autoStride", 3, 1, 12);
    c.autoBudget = gi("autoBudget", 1500, 200, 20000);
    c.autoCell = gi("autoCell", 8, 2, 60);
    c.autoWindow = gi("autoWindow", 48, 5, 400);
    c.autoPrune = gi("autoPrune", 4, 1, 20);
    c.autoForget = gi("autoForget", 120, 20, 2000);
    Config d;
    for (int i = 0; i < M_COUNT; ++i) {
        c.maxDelay[i] = gi((std::string("maxDelay") + std::to_string(i)).c_str(), d.maxDelay[i], 0, 64);
    }
    if (m->getSavedValue<int64_t>("cfgVersion", 0) < 4) {
        // v4: ground modes got a much longer default delay; only values still at the old default move
        static constexpr int oldDelay[M_COUNT] = {8, 12, 6, 6, 8, 6, 6, 6};
        for (int i : {int(M_CUBE), int(M_BALL), int(M_ROBOT), int(M_SPIDER)}) {
            if (c.maxDelay[i] == oldDelay[i]) c.maxDelay[i] = d.maxDelay[i];
        }
    }
    for (int i = 0; i < M_COUNT; ++i) {
        c.maxLate[i] = gi((std::string("maxLate") + std::to_string(i)).c_str(), d.maxLate[i], 0, 12);
    }
    c.tps = gi("tps", 0, 0, 10000);
    c.fps = gi("fps", 0, 0, 1000);
    c.tapTicks = gi("tapTicks", 4, 1, 40);
    c.budget = gi("budget", 3000, 200, 40000);
    c.shipHorizon = gi("shipLook", 36, 4, 300);
    c.waveHorizon = gi("waveLook", 24, 4, 300);
    if (m->getSavedValue<int64_t>("cfgVersion", 0) < 4) saveConfig();   // after everything is loaded
}

inline void saveConfig() {
    auto* m = geode::Mod::get();
    auto& c = cfg();
    m->setSavedValue<int64_t>("cfgVersion", 4);
    m->setSavedValue<bool>("enabled", c.enabled);
    m->setSavedValue<bool>("showTraj", c.showTrajectory);
    m->setSavedValue<bool>("logOn", c.logEnabled);
    m->setSavedValue<bool>("dualOn", c.dualSupport);
    m->setSettingValue<bool>("pause-button", c.pauseButton);
    m->setSavedValue<bool>("bothSides", c.bothSides);
    m->setSavedValue<int64_t>("previewLen", c.previewLen);
    m->setSavedValue<int64_t>("previewEvery", c.previewEvery);
    m->setSavedValue<int64_t>("keepTicks", c.keepTicks);
    m->setSavedValue<int64_t>("lineWidth", c.lineWidth);
    m->setSavedValue<bool>("previewBoth", c.previewBoth);
    m->setSavedValue<bool>("showPath", c.showPath);
    m->setSavedValue<int64_t>("pathHorizon", c.pathHorizon);
    m->setSavedValue<int64_t>("pathEvery", c.pathEvery);
    m->setSavedValue<int64_t>("pathBudget", c.pathBudget);
    m->setSavedValue<int64_t>("trajOpacity", c.trajOpacity);
    m->setSavedValue<int64_t>("settle", c.settle);
    m->setSavedValue<int64_t>("airHorizon", c.airHorizon);
    m->setSavedValue<int64_t>("robotHold", c.robotHold);
    m->setSavedValue<int64_t>("gainClick", c.gainClick);
    m->setSavedValue<int64_t>("minBudget", c.minBudget);
    m->setSavedValue<int64_t>("noclipTicks", c.noclipTicks);
    m->setSavedValue<int64_t>("failOpen", c.failOpen);
    m->setSavedValue<bool>("orbDetect", c.orbDetect);
    m->setSavedValue<bool>("dropFatal", c.dropFatal);
    m->setSavedValue<bool>("subTick", c.subTick);
    m->setSavedValue<bool>("twoPlayerOn", c.twoPlayerMode);
    m->setSavedValue<bool>("moveDetect", c.moveDetect);
    m->setSavedValue<bool>("cbfOn", c.cbfSupport);
    m->setSavedValue<bool>("lateOn", c.lateRescue);
    m->setSavedValue<bool>("fxOn", c.saveFx);
    m->setSavedValue<bool>("swapTwoP", c.swapTwoPlayer);
    m->setSavedValue<bool>("waveAnglesOn", c.waveAngles);
    m->setSavedValue<int64_t>("waveUp", c.waveUp);
    m->setSavedValue<int64_t>("waveDown", c.waveDown);
    m->setSavedValue<int64_t>("miniUp", c.miniUp);
    m->setSavedValue<int64_t>("miniDown", c.miniDown);
    m->setSavedValue<int64_t>("trajectory", c.trajectory);
    m->setSavedValue<int64_t>("shipImminent", c.shipImminent);
    m->setSavedValue<int64_t>("waveImminent", c.waveImminent);
    m->setSavedValue<int64_t>("holdGain", c.holdGain);
    m->setSavedValue<bool>("autoplay", c.autoplay);
    m->setSavedValue<bool>("autoLearn", c.autoLearn);
    m->setSavedValue<bool>("autoKeep", c.autoKeep);
    m->setSavedValue<int64_t>("autoHorizon", c.autoHorizon);
    m->setSavedValue<int64_t>("autoEvery", c.autoEvery);
    m->setSavedValue<int64_t>("autoPaths", c.autoPaths);
    m->setSavedValue<int64_t>("autoStride", c.autoStride);
    m->setSavedValue<int64_t>("autoBudget", c.autoBudget);
    m->setSavedValue<int64_t>("autoCell", c.autoCell);
    m->setSavedValue<int64_t>("autoWindow", c.autoWindow);
    m->setSavedValue<int64_t>("autoPrune", c.autoPrune);
    m->setSavedValue<int64_t>("autoForget", c.autoForget);
    for (int i = 0; i < M_COUNT; ++i) {
        m->setSavedValue<int64_t>(std::string("maxDelay") + std::to_string(i), c.maxDelay[i]);
    }
    for (int i = 0; i < M_COUNT; ++i) {
        m->setSavedValue<int64_t>(std::string("maxLate") + std::to_string(i), c.maxLate[i]);
    }
    m->setSavedValue<int64_t>("tps", c.tps);
    m->setSavedValue<int64_t>("fps", c.fps);
    m->setSavedValue<int64_t>("tapTicks", c.tapTicks);
    m->setSavedValue<int64_t>("budget", c.budget);
    m->setSavedValue<int64_t>("shipLook", c.shipHorizon);
    m->setSavedValue<int64_t>("waveLook", c.waveHorizon);
}

} // namespace buf