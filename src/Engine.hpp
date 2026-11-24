#pragma once
#include <Geode/Geode.hpp>
#include <Geode/binding/RingObject.hpp>
#include <algorithm>
#include <cmath>
#include <deque>
#include <string>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Config.hpp"
#include "DBlock.hpp"

using namespace geode::prelude;

namespace buf {

// ---------------------------------------------------------------------------
// Engine: input interception + ghost-player prediction.
//
//  * Every jump/hold event (press AND release) is swallowed while the mod is
//    active. onTick() then decides when the game gets to see it.
//  * For a pending event the engine tries delays 0..maxDelay (per game mode).
//    For each delay a hidden ghost player predicts the next ticks and checks
//    whether it survives. Only physics is predicted: the ghost never invents
//    inputs, and an orb reacts only to the click being predicted, on the tick
//    that click is pressed.
//      - click modes: press at the delay, hold a short tap, then release;
//        survive until a stable landing (or the whole trajectory);
//      - ship / wave: the new hold / release state is assumed to stay for the
//        whole look-ahead. The event is only delayed when doing it NOW kills
//        you but doing it a few ticks later does not. If nothing helps, the
//        event passes untouched.
//  * Events are queued in order, so a tap that starts and ends inside one
//    frame (spam) is never lost. Deaths blocked by a noclip mod do not
//    disturb the engine.
//  * Control layouts: solo (also "2-player mode" while dual is off) = both
//    screen sides are player 1; 1-player dual = one input drives both players
//    (the ghost mirrors it, fire() mirrors it to the game); 2-player mode with
//    dual = each player has its own input and delay.
//  * The ghost handles solids, hazards, slopes, orbs (only on the predicted
//    click), gravity portals and speed portals. Anything else it cannot
//    predict (pads, mode / size / mirror / dual / teleport portals) ends the
//    prediction there: it counts as survived instead of guessing.
//  * No autopilot: the engine only ever delays what you pressed or released, or (late rescue)
//    moves a press / release that would be fatal a few ticks BACK in time: the ghost replays
//    those ticks from a stored state with the input applied, and the real player takes over
//    the replayed state.
//  * Click Between Frames: CBF splits the physics step itself (inside PlayerObject::update).
//    While it is active this engine never splits a tick on its own, counts a tick once (the
//    nested sub-step updates are ignored, see inUpdate()) and uses CBF's exact click time.
// ---------------------------------------------------------------------------
class Engine {
public:
    struct TrailPt {
        CCPoint p;
        int tag; // 0 path, 1 click, 2 orb click, 3 death
    };
    struct Decision {
        bool known = false;   // false = pass the event straight through
        int delay = 0;        // ticks to hold the event back
        bool drop = false;    // swallow the click entirely
        float frac = 1.f;     // 1 = at the start of tick `delay`; < 1 = inside the tick before, after this part of it
        bool orb = false;     // the chosen delay is the one that lets the click hit an orb
        bool doomed = false;  // no delay survives and the prediction is reliable (late rescue may be tried)
        int late = 0;         // > 0: the input would have been safe this many ticks earlier (state in the late slot)
    };

private:
    static constexpr int kSlots = 14;

    // Simulated (upcoming, not-yet-triggered) move command started when the ghost crosses a move trigger
    struct SimMove {
        int group = 0;
        CCPoint amp;                   // world-unit offset
        double start = 0.0;            // simulated seconds (SlotInfo::simTime) when crossed
        double len = 0.0;              // duration in seconds
        int easing = 0;
        float rate = 2.f;
        bool lockX = false, lockY = false;
        float modX = 1.f, modY = 1.f;
        float startP1X = 0.f, startP1Y = 0.f;
        bool unsupported = false;
    };

    struct SlotInfo {
        bool holding[2] = {false, false};
        bool grounded = false;
        int groundTicks = 0;
        bool unknown = false;          // touched something we cannot predict
        int ticks = 0;                 // ticks simulated in this slot
        double simTime = 0.0;          // simulated seconds elapsed since search start
        double prevSimTime = 0.0;      // simTime at the previous tick (for moving object m_lastPosition)
        float firstScale = 1.f;        // part of a tick the NEXT step simulates (CBF: the rest of the running tick)
        std::vector<int> touched;      // portals already applied (uniqueID*2 + player)
        std::vector<int> usedRings;    // orbs activated in THIS slot (uniqueID*2 + player)
        std::vector<int> simTrigIDs;   // upcoming x-move triggers already crossed in THIS slot
        std::vector<SimMove> simMoves; // upcoming x-move commands started during THIS slot's simulation
    };

    enum class Layout { Solo, DualSolo, TwoPlayer };

    // ghost players: [player][slot]
    PlayerObject* m_p[2][kSlots] = {};
    SlotInfo m_info[kSlots];
    bool m_two = false;            // ghost simulates both objects (2-player mode)
    int m_tp = 0;                  // target player of the running search
    bool m_mirror = false;         // 1-player dual: player 2 copies the input
    Layout m_layout = Layout::Solo;
    Layout m_prevLayout = Layout::Solo;
    // true while the game's hold state may differ from the finger because of something THIS
    // mod did (held back / dropped / autoplay). Only then is anything ever re-sent.
    bool m_owed[2] = {false, false};
    int m_curSlot = 0;
    bool m_hitUnknown = false;
    int m_evGrav = 0;
    int m_evSpeed = 0;
    int m_evUnknown = 0;
    int m_lastTickFrame = -1000;   // frame of the last real physics tick
    // trajectory drawing
    std::vector<TrailPt> m_trailHold;      // live preview: path if the button is HELD
    std::vector<TrailPt> m_trailRelease;   // live preview: path if the button is RELEASED
    bool m_liveValid = false;
    int m_liveSkip = 0;                    // ticks since the last refresh (the path is trimmed)
    bool m_prevHold = false;               // input the live preview was computed for
    // show path (scan of a feasible route, display only)
    std::vector<TrailPt> m_pathPts;
    bool m_pathOk = false;
    bool m_pathValid = false;
    int m_pathCounter = 0;
    int m_pathSkip = 0;
    bool m_pathPrevHold = false;
    std::vector<TrailPt> m_keepOk;         // last click decision (kept on screen)
    std::vector<TrailPt> m_keepFail;
    int m_keepTicks = 0;
    int m_previewCounter = 0;
    bool m_justDecided = false;
    bool m_hadDraw = false;

    int m_lastP1Frame = -1000;     // frame of the last tick driven by player 1's update
    int m_dbgIn[2] = {0, 0};

    // hook-visible state
    bool m_simulating = false;
    bool m_simDead = false;
    PlayerObject* m_active = nullptr;
    PlayerObject* m_stepObj[2] = {nullptr, nullptr};
    bool m_stepEdge[2] = {false, false};   // this tick presses the button (false -> true)
    // Everything the ghost knows about orbs lives in m_orb (see the "orbs" block below).
    struct OrbState {
        bool done[2] = {false, false};     // an orb already reacted to this step's click
        bool used = false;                 // the run being simulated activated an orb (see orbBegin)
        int hitTick = -1;                  // slot tick when the first orb in this run activated
        bool allowBuffer = false;          // allow GD-style held-in-air orb buffering (m_stateRingJump)
        bool enabled = true;               // false while the late search runs: a rescue never uses an orb
        long seen = 0;                     // stats for the Physics page: orbs the ghost touched,
        long hit = 0;                      // ...that reacted to the clicked tick,
        long delays = 0;                   // ...and delays that were chosen because of an orb
    } m_orb;

    // search bookkeeping
    int m_used = 0;
    int m_budget = 3000;
    bool m_aborted = false;
    bool m_wantTrail = false;
    int m_reach = 0;
    std::vector<TrailPt> m_trail;
    std::vector<TrailPt> m_trailOk;
    std::vector<TrailPt> m_trailFail;

    // real-game input state, per player
    bool m_ready = false;
    bool m_phys[2] = {false, false};   // what your finger does
    bool m_game[2] = {false, false};   // what the game has been told
    bool m_supp[2] = {false, false};
    struct Pend { long tick; bool push; float frac = 1.f; int saved = 0; bool flag = true; };   // saved = ticks it was held back, flag = isPlayer1 of the original event
    std::deque<Pend> m_pend[2];        // events held back (only ever filled by a rescue)
    long m_arr[2] = {0, 0};            // arrival tick of the last held-back event
    long m_now = 0;                    // physics ticks started so far
    struct MoveTrack {
        CCPoint last;
        CCPoint vel;                   // units per unit of game time
        double changeClock = 0;        // game time of the last position change
        bool seen = false;
        bool moving = false;
        bool hasVel = false;
    };
    std::unordered_map<int, MoveTrack> m_moveTrk;
    // Move triggers, read straight from GJEffectManager::createMoveCommand (already active)
    // and EffectGameObject (upcoming x-activated triggers). The ghost computes the exact
    // eased offset at its own simulated time for every object belonging to that group ID,
    // including the final gap once the move finishes.
    struct MoveCmd {
        int group = 0;
        int controlID = 0;
        CCPoint amp;                   // full offset of the move (world units, straight from GD)
        double start = 0.0;            // m_secClock (seconds) when it started
        double len = 0.0;              // duration in seconds (0 = instant)
        int easing = 0;
        float rate = 2.f;
        bool lockX = false, lockY = false;
        float modX = 1.f, modY = 1.f;
        bool unsupported = false;      // camera lock / stopped / dynamic: cannot be predicted
    };
    std::vector<MoveCmd> m_moves;                  // running (real) moves
    std::unordered_set<int> m_firedMoveIDs;        // uniqueIDs of real move triggers already fired
    struct MoveTrig { EffectGameObject* obj; float x; int uniqueID; };
    std::vector<MoveTrig> m_moveTrigs;             // x-activated move triggers of the level, sorted by x
    float m_ghostPlayerX = 0.f;                    // x of the ghost being collision-checked
    float m_ghostPlayerY = 0.f;
    float m_prevGhostPlayerX = 0.f;                // x/y of the ghost at the previous tick
    float m_prevGhostPlayerY = 0.f;
    float m_realP1X = 0.f;                         // x/y of real player 1 when the search started
    float m_realP1Y = 0.f;
    // CBF: where player 1 stood when the running tick started, and how far the tick before moved it
    float m_tickStartX = 0.f;
    float m_prevTickDx = 0.f;
    float m_loadScale = 1.f;                       // first step of a decision search (see SlotInfo::firstScale)
    double m_clock = 0.0;              // game time = sum of the dt of every real physics step (60fps units)
    double m_secClock = 0.0;           // game time in seconds (= sum of dt / 60, already timeWarp-scaled)
    float m_stepDtNow = 0.f;
    float m_dtScale = 1.f;             // ghost: part of a tick to simulate (sub-tick click)
    bool m_partial = false;            // ghost: movement only, no collision check (first half of a split tick)
    // 2-player mode: which player the GAME really sends each side to (it depends on the
    // game's "flip 2-player controls" option). Learned from the real input, no settings read.
    int m_dbgFlag = -1;                // isPlayer1 of the last real input
    int m_dbgRecv = -1;                // -1 none, 0 / 1 = player that got it, 2 = both
    int m_dbgPi = -1;                  // player the mod judged it as
    int m_sideMap[2] = {-1, -1};       // [isPlayer1 ? 1 : 0] -> player index (2-player mode, dual on)
    // 2-player level, dual OFF: [isPlayer1 ? 1 : 0] -> player that gets it. "Always limit
    // controls" can leave one side driving nobody, "flip 2-player controls" decides which side
    // drives player 1. kNobody = the game gave that side to no player.
    int m_sideMapSolo[2] = {-1, -1};
    static constexpr int kNobody = 3;
    // The real fingers, per screen side ([isPlayer1 ? 1 : 0]), taken from every real event
    // before anything is filtered, delayed or remapped. This is the ground truth the held-back
    // state is checked against (reconcile): a missed event must never leave a hold stuck.
    bool m_raw[2] = {false, false};
    int m_stuck[2] = {0, 0};           // ticks the game has been held with no finger on it
    int m_sweep = 0;                   // >0: just back in solo, one release sweep is due
    bool m_inHandle = false;
    PlayerObject* m_recvP = nullptr;
    int m_recvCount = 0;
    long m_moveUntil = 0;              // a move trigger is running until this tick
    int m_otherOverride = -1;          // dual: assumed hold of the other player (-1 = as the game has it)
    bool m_firing = false;
    float m_load = 0.f;                // recent search load (spam) -> smaller budgets

    // autoplay planner + memory
    struct AiCell { float bad[2] = {0.f, 0.f}; };      // [0] release, [1] hold
    struct AiStep { int cell; bool hold; int tick; };
    std::unordered_map<int, AiCell> m_ai;
    std::deque<AiStep> m_aiTrail;
    std::pair<int, bool> m_aiLastForgiven{std::numeric_limits<int>::min(), false};
    int m_aiTick = 0;
    bool m_aiHold = false;
    int m_aiHoldLeft = 0;
    int m_aiDeaths = 0;
    int m_aiHorizon = 70;
    bool m_autoWas = false;
    std::unordered_set<uint64_t> m_failMemo;
    std::vector<int> m_levels;
    struct AiSeg {
        bool hold;
        int seg;
    };
    std::vector<AiSeg> m_aiPlan;       // the route autoplay is following
    int m_aiPlanPos = 0;               // ticks already played of it
    int m_aiPlanTotal = 0;             // its length in ticks
    std::vector<AiSeg> m_aiNext;       // the next route, built at 80% from where this one is at 90%
    int m_aiNextTotal = 0;
    bool m_aiNextReady = false;
    bool m_aiNextTried = false;        // built (or tried) for the current route already
    int m_forceLen = 0;                // route search: the first levels are fixed (a branch)
    bool m_force[kSlots] = {};
    bool m_aiContinuous = false;       // ship / wave: the default is "keep the input"; click modes: "do nothing"
    bool m_planHold[kSlots] = {};
    int m_planSeg[kSlots] = {};
    int m_planLen = 0;
    bool m_bestHold[kSlots] = {};
    int m_bestSeg[kSlots] = {};
    int m_bestLen = 0;
    int m_bestReach = -1;
    int m_noclipTicks = 0;             // >0: a death was recently blocked (noclip)

    float m_measuredDt = 0.25f;
    CCDrawNode* m_draw = nullptr;
    // "saved" marks: a small ring + the tick count, left in the level where the mod saved an input
    std::vector<CCLabelBMFont*> m_fxLabels;
    std::vector<CCPoint> m_fxPts;
    CCDrawNode* m_fxDraw = nullptr;

    // late-click rescue: state of player 1 at the start of the last kHist ticks (ghost copies)
    static constexpr int kHist = 12;
    static constexpr int kLateSlot = 3;          // ghost slot that holds the state to hand to the real player
    PlayerObject* m_hist[kHist] = {};
    bool m_histHold[kHist] = {};                 // hold state the game had when that tick started
    long m_histTick[kHist] = {};
    Mode m_histMode[kHist] = {};
    int m_histHead = 0;                          // newest entry
    int m_histN = 0;                             // valid entries
    int m_dbgLate = 0;
    int m_latePending = 0;                       // k of a rescue whose press is about to be delivered
    int m_lateIdx = 0;                           // history entry the rescue starts from
    Mode m_lateMode = M_CUBE;
    std::vector<CCPoint> m_latePath;             // positions of the replayed ticks (wave trail)

    // Click Between Frames
    bool m_inUpdate = false;                     // player 1's update is running (CBF delivers clicks inside it)
    bool m_cbfOn = false;
    int m_cbfFrame = -1000;

    // wave slope tracking (HUD)
    float m_prevX = 0.f;
    float m_prevY = 0.f;
    float m_realAngle = 0.f;
    float m_ghostAngle = 0.f;

    // rescue log (shown on the first page of the settings window)
    std::deque<std::string> m_log;

    int m_dbgInputs = 0;
    int m_dbgSearches = 0;
    int m_dbgDelayed = 0;
    std::string m_lastText = "-";

public:
    // ---------------------------------------------------------------- lifecycle
    void init() {
        quit();
        // m_sideMap / m_sideMapSolo are NOT reset here: which side drives which player is a game
        // setting, learned once per game session (first click of a 2-player level).
        m_raw[0] = m_raw[1] = false;
        m_stuck[0] = m_stuck[1] = 0;
        m_sweep = 0;
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return;
        for (int i = 0; i < kSlots; ++i) {
            m_p[0][i] = createSimPlayer(pl);
        }
        m_draw = CCDrawNode::create();
        m_draw->setZOrder(5000);
        pl->m_objectLayer->addChild(m_draw);
        m_fxDraw = CCDrawNode::create();
        m_fxDraw->setZOrder(5001);
        pl->m_objectLayer->addChild(m_fxDraw);
        m_fxLabels.clear();
        m_fxPts.clear();

        m_dbgInputs = m_dbgSearches = m_dbgDelayed = 0;
        m_lastText = "-";

        m_ready = true;
        for (int i = 0; i < kSlots; ++i) {
            if (!m_p[0][i]) m_ready = false;
        }
        buildMoveTrigs(pl);
        m_lastTickFrame = -1000;
        m_lastP1Frame = -1000;
        m_tickStartX = 0.f;
        m_prevTickDx = 0.f;
        m_dbgIn[0] = m_dbgIn[1] = 0;
        if (!cfg().autoKeep) {              // a new level: forget what was learned
            m_ai.clear();
            m_aiDeaths = 0;
        }
        m_aiTrail.clear();
        m_aiTick = 0;
        m_aiHoldLeft = 0;
        m_aiHold = false;
        m_autoWas = false;
        m_moves.clear();
        m_firedMoveIDs.clear();
        clearOverlay();
        resetInput();
    }

    // ================================================================== orbs
    // True only for deterministic physics jump/flip/dash orbs (excludes trigger/custom orbs,
    // teleport orbs and teleport portals, which also inherit from RingObject!).
    static bool isSimOrb(GameObject* obj) {
        if (!obj) return false;
        switch (obj->m_objectType) {
            case GameObjectType::YellowJumpRing:
            case GameObjectType::PinkJumpRing:
            case GameObjectType::GravityRing:
            case GameObjectType::GreenRing:
            case GameObjectType::DropRing:
            case GameObjectType::RedJumpRing:
            case GameObjectType::SpiderOrb:
            case GameObjectType::DashRing:
            case GameObjectType::GravityDashRing:
                return geode::cast::typeinfo_cast<RingObject*>(obj) != nullptr;
            default:
                return false;
        }
    }

    static bool isDashOrb(GameObject* obj) {
        return obj && (obj->m_objectType == GameObjectType::DashRing ||
                       obj->m_objectType == GameObjectType::GravityDashRing);
    }

    // Call before every simulated run whose orb use matters; read the answer with orbUsed() right
    // after that run.
    void orbBegin(bool allowBuffer = false) {
        m_orb.used = false;
        m_orb.hitTick = -1;
        m_orb.allowBuffer = allowBuffer;
    }
    bool orbUsed() const { return m_orb.used; }
    int orbHitTick() const { return m_orb.hitTick; }

    // Has this orb already been consumed for `who` (either in the real game before the search,
    // or earlier inside the currently simulated ghost slot)? Note: GD's PlayerObject::m_touchedRings
    // marks orbs the player merely OVERLAPPED (without clicking!), so we NEVER check m_touchedRings
    // here — we check the real orb's activation flags and our per-slot usedRings list.
    bool isRingConsumed(int slot, int who, RingObject* ring) const {
        if (!ring || ring->m_isDisabled || ring->m_isDisabled2 ||
            ring->m_isGroupDisabled || ring->m_isSpawnOnly) {
            return true;
        }
        if (ring->m_isMultiActivate) return false;
        if (who == 0 ? ring->m_activatedByPlayer1 : ring->m_activatedByPlayer2) return true;
        int key = ring->m_uniqueID * 2 + who;
        auto const& used = m_info[slot].usedRings;
        return std::find(used.begin(), used.end(), key) != used.end();
    }

    // May an orb activate right now for ghost `player`?
    bool canFireOrbNow(PlayerObject* player, int who) const {
        if (!cfg().orbDetect || !m_orb.enabled || m_orb.done[who] || !player) return false;
        if (m_stepEdge[who]) return true;
        return m_orb.allowBuffer && player->m_stateRingJump;
    }

    // One orb reacted (through PlayerObject::ringJump on a ghost).
    void orbNoteHit(int who) {
        ++m_orb.hit;
        m_orb.done[who] = true;
        if (!m_orb.used) m_orb.hitTick = m_info[m_curSlot].ticks;
        m_orb.used = true;
    }

    // GJBaseGameLayer::canBeActivatedByPlayer hook while simulating:
    // Allow GD's own collisionCheckObjects to test exact orb hitboxes and call playerTouchedRing
    // for any valid, unconsumed physics orb (even on non-click ticks, so m_touchingRings stays
    // populated for the next tick's pushButton/update just like the real game).
    bool canSimRingTouch(PlayerObject* player, EffectGameObject* object) {
        if (!m_simulating || !isSimPlayer(player) || !object) return false;
        if (object->m_objectType == GameObjectType::CustomRing ||
            object->m_objectType == GameObjectType::TeleportOrb ||
            object->m_objectType == GameObjectType::TeleportPortal) {
            return false;
        }
        if (!isSimOrb(object)) return false;
        auto ring = static_cast<RingObject*>(object);
        int who = player == m_stepObj[1] ? 1 : 0;
        return !isRingConsumed(m_curSlot, who, ring);
    }

    // GJBaseGameLayer::playerTouchedRing hook while simulating
    void simTouchedRing(PlayerObject* player, RingObject* ring) {
        if (!m_simulating || !isSimPlayer(player) || !ring) return;
        if (!isSimOrb(ring)) {
            if (ring->m_objectType == GameObjectType::CustomRing ||
                ring->m_objectType == GameObjectType::TeleportOrb ||
                ring->m_objectType == GameObjectType::TeleportPortal) {
                ++m_evUnknown;
                m_hitUnknown = true;
            }
            return;
        }
        int who = player == m_stepObj[1] ? 1 : 0;
        if (isRingConsumed(m_curSlot, who, ring)) return;
        ++m_orb.seen;
        if (player->m_touchingRings && !player->m_touchingRings->containsObject(ring)) {
            player->m_touchingRings->addObject(ring);
        }
        if (canFireOrbNow(player, who)) {
            // Ensure GD's ringJump sees an active ring-jump click when triggered on the click tick
            bool prevState = player->m_stateRingJump;
            if (m_stepEdge[who]) player->m_stateRingJump = true;
            player->ringJump(ring, true);
            if (!m_orb.done[who]) player->m_stateRingJump = prevState;
        }
    }

    // PlayerObject::ringJump hook guards while simulating (catches pushButton, update, AND playerTouchedRing)
    bool beforeSimRingJump(PlayerObject* player, RingObject* ring) {
        if (!isSimOrb(ring)) {
            if (ring->m_objectType == GameObjectType::CustomRing ||
                ring->m_objectType == GameObjectType::TeleportOrb ||
                ring->m_objectType == GameObjectType::TeleportPortal) {
                ++m_evUnknown;
                m_hitUnknown = true;
            }
            return false;
        }
        int who = player == m_stepObj[1] ? 1 : 0;
        if (!canFireOrbNow(player, who)) return false;
        if (isRingConsumed(m_curSlot, who, ring)) return false;
        return true;
    }

    void afterSimRingJump(PlayerObject* player, RingObject* ring, bool changed) {
        if (!changed || !player || !ring) return;
        int who = player == m_stepObj[1] ? 1 : 0;
        orbNoteHit(who);
        if (!ring->m_isMultiActivate) {
            int key = ring->m_uniqueID * 2 + who;
            auto& used = m_info[m_curSlot].usedRings;
            if (std::find(used.begin(), used.end(), key) == used.end()) used.push_back(key);
        }
    }

    // shown on the Physics page: tells whether the ghost sees orbs at all
    std::string orbInfo() const {
        return fmt::format("Orb sim: touched {} | hit on click tick {} | delays for orbs {}",
                           m_orb.seen, m_orb.hit, m_orb.delays);
    }

    void quit() {
        for (int pl = 0; pl < 2; ++pl) {
            for (int i = 0; i < kSlots; ++i) {
                if (m_p[pl][i]) {
                    m_p[pl][i]->removeFromParent();
                    m_p[pl][i]->release();
                    m_p[pl][i] = nullptr;
                }
            }
        }
        destroyHist();
        m_draw = nullptr; // owned by the layer
        m_fxDraw = nullptr;
        m_fxLabels.clear();                            // owned by the layer
        m_fxPts.clear();
        m_moveTrigs.clear();
        m_moves.clear();
        m_firedMoveIDs.clear();
        m_ready = false;
        m_inUpdate = false;
        m_simulating = false;
        m_active = nullptr;
        resetInput();
    }

    void resetInput() {
        for (int i = 0; i < 2; ++i) {
            m_phys[i] = false;
            m_game[i] = false;
            m_supp[i] = false;
            m_owed[i] = false;
            m_pend[i].clear();
        }
        m_prevLayout = Layout::Solo;
    }

    void onReset() { // level restart / respawn
        m_histN = 0;           // (the saved marks stay: a death is not a restart, see resetMarks)
        for (int i = 0; i < 2; ++i) {
            m_pend[i].clear();
            m_supp[i] = false;
            m_game[i] = false;
            m_owed[i] = false;
        }
        m_moves.clear();
        m_firedMoveIDs.clear();
        m_tickStartX = 0.f;
        m_prevTickDx = 0.f;
        m_aiTrail.clear();
        m_aiTick = 0;
        m_aiHoldLeft = 0;
        m_aiHold = false;
        m_aiLastForgiven = {std::numeric_limits<int>::min(), false};
        clearOverlay();
        if (m_draw) m_draw->clear();
    }

    void clearOverlay() {
        clearAutoPlan();
        m_moveUntil = 0;
        m_moveTrk.clear();
        m_trailHold.clear();
        m_trailRelease.clear();
        m_pathPts.clear();
        m_liveValid = false;
        m_pathValid = false;
        m_liveSkip = 0;
        m_pathSkip = 0;
        m_previewCounter = 0;
        m_pathCounter = 0;
    }

    // ---------------------------------------------------------------- log
    static constexpr int kLogMax = 30;
    std::deque<std::string> const& logLines() const { return m_log; }
    void clearLog() { m_log.clear(); }

    // ---------------------------------------------------------------- hook API
    bool isSimulating() const { return m_simulating; }
    bool isFiring() const { return m_firing; }

    bool isSimPlayer(PlayerObject* p) const {
        if (!p) return false;
        for (int pl = 0; pl < 2; ++pl) {
            for (int i = 0; i < kSlots; ++i) {
                if (p == m_p[pl][i]) return true;
            }
        }
        return false;
    }

    // Ghost deaths are swallowed here. Real deaths are left to the game.
    bool onSimDeath(PlayerObject* player) {
        if (m_simulating && isSimPlayer(player)) {
            m_simDead = true;
            return true;
        }
        return false;
    }

    // Call after the game handled a REAL destroyPlayer. Only a death that
    // really happened resets the engine; one blocked by a noclip mod is
    // ignored (it used to desync the input state every frame inside a hazard).
    void onRealDestroyed(PlayerObject* player) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl || !player || (player != pl->m_player1 && player != pl->m_player2)) return;
        if (player->m_isDead) {
            if (player == pl->m_player1 && cfg().autoplay) aiOnDeath();   // learn first
            onReset();
        } else {
            m_noclipTicks = cfg().noclipTicks;
        }
    }

    // Gravity portal reached through the game's own dispatch (triggerObject).
    // Skipped when simTouchObjects already applied it.
    void simGravityPortal(EffectGameObject* object) {
        PlayLayer* pl = PlayLayer::get();
        PlayerObject* player = m_active;
        if (!m_simulating || !pl || !isSimPlayer(player) || !object) return;
        int who = player == m_stepObj[1] ? 1 : 0;
        if (markTouched(object, who)) return;
        applyGravityPortal(pl, player, object->m_objectType);
    }

    // The collision hook hands us the objects near a ghost player. Portals & special blocks
    // (D/J/S/H) the game would apply with side effects (or not at all) are applied here, by
    // geometry, with no visuals and without touching any real object state.
    void simTouchObjects(PlayerObject* player, gd::vector<GameObject*>* vec, int count) {
        PlayLayer* pl = PlayLayer::get();
        if (!m_simulating || !pl || !isSimPlayer(player) || !vec) return;
        int who = player == m_stepObj[1] ? 1 : 0;
        CCRect pr = player->getObjectRect();
        for (int i = 0; i < count; ++i) {
            GameObject* obj = vec->at(i);
            if (!obj || obj->m_isDisabled || obj->m_isDisabled2 || obj->m_isGroupDisabled) continue;
            // Special blocks: D (1755), J (1813), S (1829), H (1859)
            if (isSpecialBlock(obj)) {
                if (cfg().dBlock) {
                    if (obj->getObjectRect().intersectsRect(pr)) {
                        int id = obj->m_objectID;
                        if (id == kDBlockId) player->m_stateDartSlide = 2;
                        else if (id == kJBlockId) player->m_stateNoAutoJump = 2;
                        else if (id == kSBlockId) {
                            if (player->m_isDashing) {
                                player->stopDashing();
                                player->m_jumpBuffered = false;
                            }
                        } else if (id == kHBlockId) player->m_stateHitHead = 2;
                    }
                    continue;
                }
            }
            TouchKind kind = classify(obj);
            if (kind == TouchKind::None) continue;
            CCRect orect = obj->getObjectRect();
            if (!orect.intersectsRect(pr)) continue;
            if (auto eff = geode::cast::typeinfo_cast<EffectGameObject*>(obj)) {
                if ((kind == TouchKind::Gravity || kind == TouchKind::Speed) &&
                    !eff->m_isMultiActivate &&
                    (who == 0 ? eff->m_activatedByPlayer1 : eff->m_activatedByPlayer2)) {
                    continue;
                }
            }
            if (markTouched(obj, who)) continue;
            if (kind == TouchKind::Gravity) {
                ++m_evGrav;
                applyGravityPortal(pl, player, obj->m_objectType);
            } else if (kind == TouchKind::Speed) {
                ++m_evSpeed;
                player->updateTimeMod(speedOf(obj->m_objectID), true);
            } else {
                ++m_evUnknown;
                m_hitUnknown = true;
            }
        }
    }

    // ------------------------------------------------- who really gets the input
    // handleButton hook: around the original call, PlayerObject::pushButton /
    // releaseButton tell which player the game gave the input to, AND keep m_game[]
    // permanently synchronized with GD's real player hold state.
    void beginHandle() {
        m_inHandle = true;
        m_recvP = nullptr;
        m_recvCount = 0;
    }
    void noteRealButton(PlayerObject* p, bool down) {
        if (m_simulating || !p) return;
        if (PlayLayer* pl = PlayLayer::get()) {
            if (p == pl->m_player1) m_game[0] = down;
            else if (p == pl->m_player2) m_game[1] = down;
        }
        if (!m_inHandle) return;
        if (m_recvCount == 0) {
            m_recvP = p;
            m_recvCount = 1;
        } else if (p != m_recvP) {
            m_recvCount = 2;
        }
    }
    void endHandle(bool isPlayer1, bool down) {
        m_inHandle = false;
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return;
        if (m_recvCount == 0) m_dbgRecv = -1;
        else if (m_recvCount > 1) m_dbgRecv = 2;
        else m_dbgRecv = m_recvP == pl->m_player1 ? 0 : (m_recvP == pl->m_player2 ? 1 : -1);
        int side = isPlayer1 ? 1 : 0;
        if (twoFlagOf(pl)) {                                // 2-player level: learn the sides
            if (m_dbgRecv == 0 || m_dbgRecv == 1) {
                setSides(side, m_dbgRecv);
                syncDelivered(m_dbgRecv, down);
            } else if (m_dbgRecv == -1 && pl->m_player1 && !pl->m_player1->m_isDead) {
                // nobody got it while player 1 is alive: this side is player 2's, so the
                // other one is player 1's
                setSides(side, 1);
                if (m_layout == Layout::Solo) m_sideMapSolo[side] = kNobody;
            }
        }
    }

    // `side` (1 = the isPlayer1 flag) drives `who`; the two sides always drive different players,
    // so the other side is known too. Kept for the whole session (every 2-player level, dual or not).
    void setSides(int side, int who) {
        m_sideMap[side] = who;
        m_sideMap[1 - side] = 1 - who;
        m_sideMapSolo[side] = who;
        m_sideMapSolo[1 - side] = 1 - who;
    }

    // The game just gave this event to player `idx`: that is what the game has been told, whatever
    // the mod judged beforehand. Only a real tap moves the finger state (fire() replays old ones).
    void syncDelivered(int idx, bool down) {
        m_game[idx] = down;
        if (!m_firing) m_phys[idx] = down;
    }

    // shown at the bottom of the Log page: what the mod sees for the last input
    std::string inputInfo() const {
        static constexpr char const* lay[3] = {"Solo", "Dual", "2P"};
        auto pn = [](int v) {
            return v < 0 ? std::string("?") : (v == kNobody ? std::string("none")
                   : (v > 1 ? std::string("both") : "P" + std::to_string(v + 1)));
        };
        return fmt::format("{} | game sends side true->{} false->{} (dual off: true->{} false->{}) | last input: side {} game gave it to {}, mod judged {} | inputs P1 {} P2 {}",
                           lay[static_cast<int>(m_layout)], pn(m_sideMap[1]), pn(m_sideMap[0]),
                           pn(m_sideMapSolo[1]), pn(m_sideMapSolo[0]),
                           m_dbgFlag < 0 ? "?" : (m_dbgFlag ? "true" : "false"), pn(m_dbgRecv), pn(m_dbgPi),
                           m_dbgIn[0], m_dbgIn[1]);
    }

    // ---------------------------------------------------------- CBF / move objects
    // Click Between Frames: inputs arrive between frames. Nothing breaks, but it is shown.
    static bool cbfLoaded() {
        return Loader::get()->isModLoaded("syzzi.click_between_frames");
    }

    // CBF loaded, enabled and not switched off by its own "soft-toggle" setting.
    // Cached for a few frames: this is asked on the physics hot path.
    bool cbfActive() {
        if (!cfg().cbfSupport) return false;
        int fr = static_cast<int>(CCDirector::sharedDirector()->getTotalFrames());
        int since = fr - m_cbfFrame;
        if (since >= 0 && since < 30) return m_cbfOn;      // refreshed every 30 frames
        m_cbfFrame = fr;
        m_cbfOn = false;
        if (auto* m = Loader::get()->getLoadedMod("syzzi.click_between_frames")) {
            m_cbfOn = m->isLoaded() && !(m->hasSetting("soft-toggle") && m->getSettingValue<bool>("soft-toggle"));
        }
        return m_cbfOn;
    }

    // Player 1's PlayerObject::update is running. A nested update (CBF calls it again for each
    // sub-step) is not a new tick, and a click delivered now lands inside the tick.
    bool inUpdate() const { return m_inUpdate; }
    bool dBlockSim() const { return cfg().dBlock; }
    void resetMarks() { clearFx(); }   // level restart: the saved marks go
    void endLate() { finishLate(); }
    void setInUpdate(bool v) { m_inUpdate = v; }

    // ---------------------------------------------------------- move trigger model
    // Collect all x-activated Move triggers (object ID 901) once when the level loads.
    void buildMoveTrigs(PlayLayer* pl) {
        m_moveTrigs.clear();
        if (!pl || !pl->m_objects) return;
        for (auto* obj : geode::cocos::CCArrayExt<GameObject*>(pl->m_objects)) {
            if (!obj || obj->m_objectID != 901) continue;
            auto eff = geode::cast::typeinfo_cast<EffectGameObject*>(obj);
            if (!eff) continue;
            if (eff->m_isSpawnTriggered || eff->m_isTouchTriggered) continue;
            if (eff->m_isDisabled || eff->m_isDisabled2) continue;
            if (eff->m_targetGroupID <= 0) continue;
            m_moveTrigs.push_back({eff, eff->getPositionX(), eff->m_uniqueID});
        }
        std::sort(m_moveTrigs.begin(), m_moveTrigs.end(),
                  [](MoveTrig const& a, MoveTrig const& b) { return a.x < b.x; });
    }

    static double evalEase(double u, int easingType, float easingRate) {
        if (u <= 0.0) return 0.0;
        if (u >= 1.0) return 1.0;
        if (easingType == 0) return u;
        float rate = easingRate > 0.f ? easingRate : 2.f;
        return static_cast<double>(GameToolbox::getEasedValue(static_cast<float>(u), easingType, rate));
    }

    // Real game: GJEffectManager::createMoveCommand gives us the EXACT resolved world-unit offset,
    // remapped groupID, duration (seconds), easing, and player/camera locks for EVERY move trigger
    // (x-triggered, touch-triggered, spawn-triggered, move-to-target, etc.).
    void onCreateMoveCommand(
        GJEffectManager* mgr, CCPoint pt, int groupID, float duration,
        int easingType, float easingRate,
        bool lockPlayerX, bool lockPlayerY,
        bool lockCameraX, bool lockCameraY,
        float moveModX, float moveModY,
        int /*uniqueID*/, int controlID
    ) {
        PlayLayer* pl = PlayLayer::get();
        if (!cfg().moveDetect || !pl || mgr != pl->m_effectManager || groupID <= 0) return;

        // Prune finished commands so m_moves stays tiny
        double now = m_secClock;
        m_moves.erase(std::remove_if(m_moves.begin(), m_moves.end(), [&](MoveCmd const& c) {
            return now > c.start + c.len + 0.15;
        }), m_moves.end());

        bool hasLock = lockPlayerX || lockPlayerY || lockCameraX || lockCameraY;
        if (duration <= 1e-5f && !hasLock) return;  // instant move: already applied on next tick

        MoveCmd cmd;
        cmd.group = groupID;
        cmd.controlID = controlID;
        cmd.amp = pt;
        cmd.start = now;
        cmd.len = std::max(0.0, static_cast<double>(duration));
        cmd.easing = easingType;
        cmd.rate = easingRate > 0.f ? easingRate : 2.f;
        cmd.lockX = lockPlayerX;
        cmd.lockY = lockPlayerY;
        cmd.modX = moveModX != 0.f ? moveModX : 1.f;
        cmd.modY = moveModY != 0.f ? moveModY : 1.f;
        cmd.unsupported = lockCameraX || lockCameraY;
        if (m_moves.size() >= 256) m_moves.erase(m_moves.begin());
        m_moves.push_back(cmd);
    }

    // Real game: an x/touch/spawn move trigger fired. Remember its uniqueID so the upcoming
    // x-trigger scanner never double-starts it (its actual command is captured by onCreateMoveCommand).
    void onRealMoveTrigger(EffectGameObject* obj) {
        if (!cfg().moveDetect || !obj) return;
        m_firedMoveIDs.insert(obj->m_uniqueID);
        float tps = effectiveTps();
        if (tps <= 0.f) tps = 240.f;
        long ticks = static_cast<long>(std::ceil(std::max(0.f, obj->m_duration) * tps)) + 4;
        m_moveUntil = std::max(m_moveUntil, m_now + ticks);
    }

    // Real game: Stop / Pause / Resume trigger (1616) fired. Mark matching active moves unsupported
    // so we do not keep extrapolating a stopped move.
    void onRealStopTrigger(EffectGameObject* obj) {
        if (!cfg().moveDetect || !obj || obj->m_targetGroupID <= 0) return;
        int tid = obj->m_targetGroupID;
        for (auto& cmd : m_moves) {
            if (cmd.controlID == tid || cmd.group == tid) cmd.unsupported = true;
        }
    }

    bool moveActive() const { return cfg().moveDetect && m_now < m_moveUntil; }

    // Ghost: an object that may be moving and cannot be predicted: stop trusting the sim
    void flagMovingObject() { m_hitUnknown = true; }

    // Ghost: when the ghost crosses an upcoming x-activated Move trigger between prevX and newX,
    // start a SimMove in slot `s` using the trigger's exact group ID, offset, duration and easing.
    void triggerSimMoves(PlayLayer* pl, int s, float prevX, float newX, float newY) {
        if (!cfg().moveDetect || m_moveTrigs.empty() || !pl) return;
        float lo = std::min(prevX, newX);
        float hi = std::max(prevX, newX);
        if (hi - lo < 1e-4f) return;
        auto it = std::lower_bound(m_moveTrigs.begin(), m_moveTrigs.end(), lo,
                                   [](MoveTrig const& t, float x) { return t.x < x; });
        auto& inf = m_info[s];
        for (; it != m_moveTrigs.end() && it->x <= hi; ++it) {
            if (prevX <= newX && it->x <= prevX) continue;
            if (prevX > newX && it->x >= prevX) continue;
            if (m_firedMoveIDs.count(it->uniqueID)) continue;
            if (std::find(inf.simTrigIDs.begin(), inf.simTrigIDs.end(), it->uniqueID) != inf.simTrigIDs.end()) continue;
            EffectGameObject* eff = it->obj;
            if (!eff || eff->m_isDisabled || eff->m_isDisabled2 || eff->m_isGroupDisabled) continue;
            if (eff->m_targetGroupID <= 0) continue;

            inf.simTrigIDs.push_back(it->uniqueID);
            SimMove sm;
            sm.group = eff->m_targetGroupID;
            sm.start = inf.simTime;
            sm.len = std::max(0.0, static_cast<double>(eff->m_duration));
            sm.easing = static_cast<int>(eff->m_easingType);
            sm.rate = eff->m_easingRate > 0.f ? eff->m_easingRate : 2.f;
            sm.lockX = eff->m_lockToPlayerX;
            sm.lockY = eff->m_lockToPlayerY;
            sm.modX = eff->m_moveModX != 0.f ? eff->m_moveModX : 1.f;
            sm.modY = eff->m_moveModY != 0.f ? eff->m_moveModY : 1.f;
            sm.startP1X = newX;
            sm.startP1Y = newY;
            sm.unsupported = eff->m_lockToCameraX || eff->m_lockToCameraY || eff->m_isDynamicMode;
            if (!sm.unsupported) {
                if (!eff->m_useMoveTarget) {
                    sm.amp = eff->m_moveOffset;
                } else {
                    auto singlePos = [&](int gid, CCPoint& out) -> bool {
                        if (gid <= 0) return false;
                        CCArray* arr = pl->getGroup(gid);
                        if (!arr || arr->count() == 0) return false;
                        auto* o = static_cast<GameObject*>(arr->objectAtIndex(0));
                        if (!o) return false;
                        out = o->getPosition();
                        return true;
                    };
                    int srcGid = eff->m_targetModCenterID > 0 ? eff->m_targetModCenterID : eff->m_targetGroupID;
                    CCPoint srcPos, dstPos;
                    bool hasSrc = singlePos(srcGid, srcPos);
                    bool hasDst = false;
                    if (eff->m_targetPlayer1 || eff->m_targetPlayer2) {
                        dstPos = ccp(newX, newY);
                        hasDst = true;
                    } else {
                        hasDst = singlePos(eff->m_centerGroupID, dstPos);
                    }
                    if (hasSrc && hasDst) {
                        CCPoint delta = dstPos - srcPos;
                        if (eff->m_moveTargetMode == MoveTargetType::XOnly) delta.y = 0.f;
                        else if (eff->m_moveTargetMode == MoveTargetType::YOnly) delta.x = 0.f;
                        sm.amp = delta;
                    } else {
                        sm.unsupported = true;
                    }
                }
            }
            inf.simMoves.push_back(sm);
        }
    }

    // Compute the predicted offset of a single group `g` at simulated time `simSec` (relative to t_now),
    // with ghost player 1 at `(gx, gy)`. Returns: 0 = no move affects this group, 1 = predicted, -1 = unsupported.
    int groupOffsetAt(int s, int g, double simSec, float gx, float gy, CCPoint& acc) const {
        int status = 0;
        double now = m_secClock;
        double tTarget = now + simSec;

        for (auto const& cmd : m_moves) {
            if (cmd.group != g) continue;
            double endT = cmd.start + cmd.len;
            if (now >= endT) continue;                // already finished before search started
            if (cmd.unsupported) return -1;
            status = 1;
            double eNow = cmd.len <= 1e-6 ? 1.0 : evalEase((now - cmd.start) / cmd.len, cmd.easing, cmd.rate);
            double eTar = cmd.len <= 1e-6 ? 1.0 : evalEase((tTarget - cmd.start) / cmd.len, cmd.easing, cmd.rate);
            double de = eTar - eNow;
            if (!cmd.lockX) acc.x += static_cast<float>(cmd.amp.x * de);
            else if (simSec > 1e-6) {
                double frac = std::min(1.0, (endT - now) / simSec);
                acc.x += static_cast<float>((gx - m_realP1X) * cmd.modX * frac);
            }
            if (!cmd.lockY) acc.y += static_cast<float>(cmd.amp.y * de);
            else if (simSec > 1e-6) {
                double frac = std::min(1.0, (endT - now) / simSec);
                acc.y += static_cast<float>((gy - m_realP1Y) * cmd.modY * frac);
            }
        }

        for (auto const& sm : m_info[s].simMoves) {
            if (sm.group != g) continue;
            if (simSec < sm.start) continue;          // not triggered yet at this time
            if (sm.unsupported) return -1;
            status = 1;
            double eTar = sm.len <= 1e-6 ? 1.0 : evalEase((simSec - sm.start) / sm.len, sm.easing, sm.rate);
            if (!sm.lockX) acc.x += static_cast<float>(sm.amp.x * eTar);
            else {
                double span = simSec - sm.start;
                double frac = (span > 1e-6 && sm.len > 1e-6) ? std::min(1.0, sm.len / span) : 1.0;
                acc.x += static_cast<float>((gx - sm.startP1X) * sm.modX * frac);
            }
            if (!sm.lockY) acc.y += static_cast<float>(sm.amp.y * eTar);
            else {
                double span = simSec - sm.start;
                double frac = (span > 1e-6 && sm.len > 1e-6) ? std::min(1.0, sm.len / span) : 1.0;
                acc.y += static_cast<float>((gy - sm.startP1Y) * sm.modY * frac);
            }
        }
        return status;
    }

    void beginSimCollision(PlayerObject* player) {
        if (!player) return;
        PlayerObject* p1 = m_p[0][m_curSlot];
        if (p1) {
            m_ghostPlayerX = p1->getPositionX();
            m_ghostPlayerY = p1->getPositionY();
            m_prevGhostPlayerX = p1->m_lastPosition.x;
            m_prevGhostPlayerY = p1->m_lastPosition.y;
        } else {
            m_ghostPlayerX = player->getPositionX();
            m_ghostPlayerY = player->getPositionY();
            m_prevGhostPlayerX = player->m_lastPosition.x;
            m_prevGhostPlayerY = player->m_lastPosition.y;
        }
    }

    // If an active or simulated move trigger slides a group horizontally into the ghost's current
    // collision section from another X section, pull those group objects into `cand` so the ghost
    // collides with the real final gap after the horizontal move!
    void appendMovingGroupObjects(GJBaseGameLayer* layer, PlayerObject* player, gd::vector<GameObject*>& cand) {
        if (!cfg().moveDetect || !layer || !player) return;
        if (m_moves.empty() && m_info[m_curSlot].simMoves.empty()) return;

        thread_local std::vector<int> horizGroups;
        horizGroups.clear();
        auto checkGroup = [&](int g) {
            if (g <= 0) return;
            if (std::find(horizGroups.begin(), horizGroups.end(), g) != horizGroups.end()) return;
            CCPoint off(0.f, 0.f);
            if (groupOffsetAt(m_curSlot, g, m_info[m_curSlot].simTime, m_ghostPlayerX, m_ghostPlayerY, off) == 1) {
                if (std::fabs(off.x) > 15.f) horizGroups.push_back(g);
            }
        };
        for (auto const& c : m_moves) {
            if (m_secClock < c.start + c.len) checkGroup(c.group);
        }
        for (auto const& sm : m_info[m_curSlot].simMoves) checkGroup(sm.group);
        if (horizGroups.empty()) return;

        float px = player->getPositionX();
        for (int g : horizGroups) {
            CCArray* arr = layer->getGroup(g);
            if (!arr || arr->count() == 0) continue;
            CCPoint off(0.f, 0.f);
            groupOffsetAt(m_curSlot, g, m_info[m_curSlot].simTime, m_ghostPlayerX, m_ghostPlayerY, off);
            for (auto* obj : geode::cocos::CCArrayExt<GameObject*>(arr)) {
                if (!obj || obj->m_isDisabled || obj->m_isDisabled2 || obj->m_isGroupDisabled) continue;
                auto type = obj->m_objectType;
                if (type != GameObjectType::Solid &&
                    type != GameObjectType::Hazard &&
                    type != GameObjectType::AnimatedHazard &&
                    type != GameObjectType::CollisionObject &&
                    type != GameObjectType::Slope &&
                    type != GameObjectType::Special &&
                    type != GameObjectType::NormalGravityPortal &&
                    type != GameObjectType::InverseGravityPortal &&
                    type != GameObjectType::GravityTogglePortal &&
                    !isSimOrb(obj)) {
                    continue;
                }
                float predX = obj->getPositionX() + off.x;
                if (std::fabs(predX - px) > 120.f) continue;
                if (std::find(cand.begin(), cand.end(), obj) == cand.end()) {
                    cand.push_back(obj);
                }
            }
        }
    }

    // Moving objects fallback clock: tracks both 60fps-normalized game time (m_clock) and exact
    // seconds (m_secClock, already timeWarp-scaled) for move triggers.
    void addClock(PlayerObject* p, float dt) {
        PlayLayer* pl = PlayLayer::get();
        if (pl && p == pl->m_player1 && dt > 0.f && dt < 5.f) {
            m_clock += dt;
            m_secClock += static_cast<double>(dt) / 60.0;
        }
    }

    float moveFresh() const { return 40.f * std::max(1e-4f, stepDt()); }

    // real game, every collision pass of player 1 (fallback for Follow / AreaMove / Practice checkpoints)
    void observeObjects(gd::vector<GameObject*>* vec, int count) {
        if (!cfg().moveDetect || !vec) return;
        if (m_moveTrk.size() > 4000) m_moveTrk.clear();
        double fresh = moveFresh();
        for (int i = 0; i < count; ++i) {
            GameObject* o = vec->at(i);
            if (!o || !o->m_groups || o->m_groupCount <= 0 || o->m_groups->at(0) == 0) continue;
            auto& t = m_moveTrk[o->m_uniqueID];
            CCPoint p = o->getPosition();
            if (!t.seen) {
                t.seen = true;
                t.last = p;
                t.changeClock = m_clock;
                continue;
            }
            if (p.x != t.last.x || p.y != t.last.y) {
                double dtc = m_clock - t.changeClock;
                if (t.moving && dtc > 1e-6 && dtc <= fresh) {
                    t.vel = (p - t.last) * static_cast<float>(1.0 / dtc);
                    t.hasVel = true;
                } else {
                    t.hasVel = false;
                }
                t.moving = true;
                t.last = p;
                t.changeClock = m_clock;
            } else if (t.moving && m_clock - t.changeClock > fresh) {
                t.moving = false;
                t.hasVel = false;
            }
        }
    }

    enum class MoveState { Static, Predicted, Unknown };

    // ghost: where is this object at the current and previous simulated ticks?
    // 1. First computes exact eased offset from active + upcoming Move Triggers matching any group ID of `o`.
    // 2. Falls back to observed velocity ONLY if no Move Trigger matches AND `o` was observed moving
    //    (e.g. Follow trigger, Area Move, or respawn mid-move in Practice Mode).
    MoveState simMove(GameObject* o, CCPoint& offset, CCPoint& prevOffset) {
        offset = CCPoint(0.f, 0.f);
        prevOffset = CCPoint(0.f, 0.f);
        if (!cfg().moveDetect || !o || !o->m_groups || o->m_groupCount <= 0) return MoveState::Static;

        int nGroups = std::min<int>(o->m_groupCount, 10);
        auto const& inf = m_info[m_curSlot];
        bool matchedTrigger = false;
        if (!m_moves.empty() || !inf.simMoves.empty()) {
            for (int i = 0; i < nGroups; ++i) {
                int g = o->m_groups->at(i);
                if (g <= 0) continue;
                int stCur = groupOffsetAt(m_curSlot, g, inf.simTime, m_ghostPlayerX, m_ghostPlayerY, offset);
                if (stCur < 0) return MoveState::Unknown;
                if (stCur > 0) {
                    matchedTrigger = true;
                    groupOffsetAt(m_curSlot, g, inf.prevSimTime, m_prevGhostPlayerX, m_prevGhostPlayerY, prevOffset);
                }
            }
        }
        if (matchedTrigger) return MoveState::Predicted;

        // Fallback: no Move Trigger targets any of this object's groups. Check if it moved anyway.
        auto it = m_moveTrk.find(o->m_uniqueID);
        if (it == m_moveTrk.end()) return MoveState::Static;
        auto& t = it->second;
        if (!t.moving || m_clock - t.changeClock > moveFresh()) return MoveState::Static;
        if (!t.hasVel) return MoveState::Unknown;
        float elCur = static_cast<float>(inf.simTime * 60.0);
        float elPrev = static_cast<float>(inf.prevSimTime * 60.0);
        offset = t.vel * elCur;
        prevOffset = t.vel * elPrev;
        return MoveState::Predicted;
    }

    void setFrameDelta(float dt) {
        if (dt > 0.001f && dt < 2.f) m_measuredDt = dt;
    }

    // ------------------------------------------------------------ timing info
    float timeWarp() const {
        PlayLayer* pl = PlayLayer::get();
        float w = pl ? pl->m_gameState.m_timeWarp : 1.f;
        return w > 0.f ? w : 1.f;
    }

    float stepDt() const {
        auto& c = cfg();
        if (c.tps > 0) return 60.f / static_cast<float>(c.tps) * timeWarp();
        return m_measuredDt;
    }

    float effectiveTps() const {
        auto& c = cfg();
        if (c.tps > 0) return static_cast<float>(c.tps);
        return 60.f / std::max(0.01f, m_measuredDt / timeWarp());
    }

    float effectiveFps() const {
        auto& c = cfg();
        if (c.fps > 0) return static_cast<float>(c.fps);
        double iv = CCDirector::sharedDirector()->getAnimationInterval();
        return iv > 0.0 ? static_cast<float>(1.0 / iv) : 60.f;
    }

    float ticksPerFrame() const {
        return std::max(1.f, effectiveTps() / std::max(1.f, effectiveFps()));
    }

    std::string timingInfo() const {
        return fmt::format(
            "Detected: {:.0f} TPS | {:.0f} FPS | {:.2f} ticks/frame | dt {:.4f}",
            effectiveTps(), effectiveFps(), ticksPerFrame(), stepDt()
        );
    }

    // ------------------------------------------------------------ mode helpers
    static Mode detectMode(PlayerObject* p) {
        if (!p) return M_CUBE;
        if (p->m_isShip) return M_SHIP;
        if (p->m_isDart) return M_WAVE;
        if (p->m_isBall) return M_BALL;
        if (p->m_isRobot) return M_ROBOT;
        if (p->m_isSpider) return M_SPIDER;
        if (p->m_isBird) return M_UFO;
        if (p->m_isSwing) return M_SWING;
        return M_CUBE;
    }
    static bool isContinuous(Mode m) { return m == M_SHIP || m == M_WAVE; }

    // Which control scheme is active. Player 2 only exists while dual mode is
    // on, so "2-player mode" without dual is just solo.
    static bool twoFlagOf(PlayLayer* pl) {
        return (pl->m_level && pl->m_level->m_twoPlayerMode) ||
               (pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode);
    }

    // 2-player mode, dual on: the player a side belongs to (-1 = not learned). The two sides
    // always drive different players, so one known side gives the other.
    int sideOwner(bool isPlayer1) const {
        int own = m_sideMap[isPlayer1 ? 1 : 0];
        if (own == 0 || own == 1) return own;
        int other = m_sideMap[isPlayer1 ? 0 : 1];
        return (other == 0 || other == 1) ? 1 - other : -1;
    }

    // Is a finger on a side that drives player `pi`? known = false when that cannot be told.
    bool fingerOn(PlayLayer* pl, int pi, bool& known) const {
        known = true;
        if (m_layout == Layout::Solo) {
            if (twoFlagOf(pl)) {
                bool seen = false;
                bool any = false;
                for (int sd = 0; sd < 2; ++sd) {
                    if (m_sideMapSolo[sd] != 0) continue;
                    seen = true;
                    any = any || m_raw[sd];
                }
                known = seen;
                return any;
            }
            return cfg().bothSides ? (m_raw[0] || m_raw[1]) : m_raw[1];
        }
        if (m_layout == Layout::DualSolo) {
            return m_raw[0] || m_raw[1];
        }
        if (m_layout == Layout::TwoPlayer) {
            for (int sd = 0; sd < 2; ++sd) {
                if (sideOwner(sd == 1) == pi) return m_raw[sd];
            }
        }
        known = false;
        return false;
    }

    // Safety net for the input bookkeeping (ONLY runs while the mod is enabled). It only ever
    // RELEASES (never synthesizes a press): if the game is held, nothing is queued, and no finger
    // is down for 3 ticks, release it.
    void reconcile(PlayLayer* pl, int n) {
        for (int pi = 0; pi < n; ++pi) {
            PlayerObject* po = pi == 0 ? pl->m_player1 : pl->m_player2;
            bool known = false;
            bool candidate = po && !po->m_isDead && m_pend[pi].empty() && m_game[pi];
            bool down = candidate ? fingerOn(pl, pi, known) : true;
            if (!candidate || !known || down) {
                m_stuck[pi] = 0;
                continue;
            }
            if (++m_stuck[pi] < 3) continue;
            m_stuck[pi] = 0;
            fire(pl, pi, false);
        }
        if (m_sweep > 0 && --m_sweep == 0 && m_layout == Layout::Solo) {
            PlayerObject* p1 = pl->m_player1;
            bool known = false;
            bool down = fingerOn(pl, 0, known);
            if (p1 && !p1->m_isDead && m_pend[0].empty() && m_game[0] && known && !down) {
                fire(pl, 0, false);
            }
        }
    }

    // the isPlayer1 flag the game turns into player `pi` (inverse of sideOwner)
    bool sideFlagFor(int pi) const {
        if (sideOwner(true) == pi) return true;
        if (sideOwner(false) == pi) return false;
        return pi == 0;
    }

    static Layout layoutOf(PlayLayer* pl) {
        bool dual = pl->m_gameState.m_isDualMode && pl->m_player2;
        if (!dual) return Layout::Solo;
        return twoFlagOf(pl) ? Layout::TwoPlayer : Layout::DualSolo;
    }

    // ================================================================ input
    // Returns false when the event was swallowed (held back / dropped / autoplay).
    // When the mod is disabled (and autoplay is off), this is a 100% pure passthrough:
    // never rewrites isP1, never swallows any press or release.
    bool onInput(GJBaseGameLayer* layer, bool push, bool& isP1) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return true;
        auto& c = cfg();

        Layout lay = layoutOf(pl);
        m_layout = lay;
        bool origP1 = isP1;
        m_raw[origP1 ? 1 : 0] = push;                  // the real finger, before any filtering

        {   // autoplay: the bot drives the input, your taps are ignored
            int fr = static_cast<int>(CCDirector::sharedDirector()->getTotalFrames());
            PlayerObject* pa = layer->m_player1;
            if (c.autoplay && m_ready && fr - m_lastTickFrame <= c.failOpen &&
                lay != Layout::TwoPlayer && pa && !pa->m_isDead) {
                m_phys[0] = push;
                return false;
            }
        }

        // Mod disabled: pure passthrough (never swallow, never remap isP1, never get holds stuck)
        if (!c.enabled) {
            for (int i = 0; i < 2; ++i) {
                m_pend[i].clear();
                m_supp[i] = false;
                m_owed[i] = false;
                m_stuck[i] = 0;
            }
            m_sweep = 0;
            if (lay == Layout::TwoPlayer) {
                int who = sideOwner(origP1);
                if (who == 0 || who == 1) m_phys[who] = push;
            } else {
                m_phys[0] = m_raw[0] || m_raw[1];
            }
            return true;
        }

        // "both sides = player 1" only outside 2-player levels: there the game already
        // routes left / right to its own players.
        bool twoLvl = twoFlagOf(pl);
        bool remap = lay == Layout::Solo && c.bothSides && !twoLvl;
        if (lay == Layout::Solo && twoLvl) {
            // 2-player level while dual is off: which side drives player 1 depends on the game's
            // "flip 2-player controls" and "always limit controls". The game shows it on the
            // first input (endHandle), so until then the event is left alone.
            int who = m_sideMapSolo[origP1 ? 1 : 0];
            if (who != 0) {
                m_dbgFlag = origP1 ? 1 : 0;
                m_dbgPi = -1;
                if (who == 1) m_phys[1] = push;           // not ours, but the finger is real
                return true;
            }
        } else if (lay == Layout::Solo && !remap && !origP1) {
            m_phys[1] = push;                          // not ours to handle, but the finger is real
            return true;
        } else if (lay == Layout::DualSolo && !c.dualSupport) {
            // 1-player dual with dualSupport disabled: pure passthrough
            m_pend[0].clear();
            m_supp[0] = false;
            m_owed[0] = false;
            m_phys[0] = m_raw[0] || m_raw[1];
            return true;
        }

        // Solo and 1-player Dual: ONE unified input buffer (pi = 0) drives the game.
        // Only true 2-player mode splits left / right into P1 (0) and P2 (1).
        int pi = 0;
        if (lay == Layout::TwoPlayer) {
            int who = sideOwner(origP1);
            if (who < 0) {
                m_dbgFlag = origP1 ? 1 : 0;
                m_dbgPi = -1;
                return true;                               // not learned yet: leave it alone
            }
            pi = who;
        }

        ++m_dbgInputs;
        ++m_dbgIn[pi];
        m_dbgFlag = origP1 ? 1 : 0;
        m_dbgPi = pi;
        m_phys[pi] = (lay == Layout::DualSolo || remap) ? (m_raw[0] || m_raw[1]) : push;
        PlayerObject* po = pi == 0 ? layer->m_player1 : layer->m_player2;

        // fail-open: if the physics tick hook is not running, do not touch anything
        int frames = static_cast<int>(CCDirector::sharedDirector()->getTotalFrames());
        bool tickAlive = frames - m_lastTickFrame <= c.failOpen;

        bool modeActive = po && !po->m_isDead && c.maxDelay[detectMode(po)] > 0;
        if (lay == Layout::DualSolo && layer->m_player1 && layer->m_player2) {
            modeActive = !layer->m_player1->m_isDead && !layer->m_player2->m_isDead &&
                         (c.maxDelay[detectMode(layer->m_player1)] > 0 ||
                          c.maxDelay[detectMode(layer->m_player2)] > 0);
        }
        bool active = c.enabled && m_ready && tickAlive && modeActive &&
                      !(lay == Layout::TwoPlayer && !c.twoPlayerMode) &&
                      !(lay == Layout::DualSolo && !c.dualSupport);
        if (!active) {
            m_pend[pi].clear();
            m_supp[pi] = false;
            m_owed[pi] = false;
            // Never drop a release (!push); only deduplicate an identical duplicate press if remapped
            if (remap && !origP1 && push && m_game[pi]) return false;
            return true;
        }
        return rescue(pl, pi, push, po);
    }

private:
    // The whole job: an EARLY press / release is held back a few ticks. Anything
    // else is left completely alone: the original event goes through untouched,
    // nothing is swallowed, queued or re-fired.
    // Returns true = let the original event through, false = swallowed.
    bool rescue(PlayLayer* pl, int pi, bool push, PlayerObject* po) {
        auto& pend = m_pend[pi];
        if (!push && m_supp[pi]) {                  // release of a click that was dropped
            m_supp[pi] = false;
            if (pend.empty()) m_owed[pi] = false;
            return false;
        }

        // vanilla: the click is handled before the next tick. CBF: it is delivered inside the tick
        // that is running (that tick has already been counted).
        long a = m_now + (m_inUpdate ? 0 : 1);
        if (!pend.empty()) {
            // an earlier event is still being held back: stay behind it and keep
            // the spacing you had (no new search, nothing is "re-decided")
            long fireAt = pend.back().tick + std::max<long>(1, a - m_arr[pi]);
            m_arr[pi] = a;
            pend.push_back({fireAt, push, 1.f, static_cast<int>(fireAt - a)});
            m_owed[pi] = true;
            return false;
        }
        if (push == m_game[pi]) return true;        // nothing changes for the game

        bool dual = (m_layout == Layout::DualSolo && pl->m_player1 && pl->m_player2);
        Mode mode = detectMode(po);
        Mode mode2 = dual ? detectMode(pl->m_player2) : mode;
        bool cont0 = isContinuous(mode) || po->m_isDashing;
        bool cont1 = dual ? (isContinuous(mode2) || pl->m_player2->m_isDashing) : cont0;

        Decision d;
        if (!push) {
            // Release: ship / wave / active dash orb (or either player in 1-player dual)
            if (cont0 || cont1) {
                Mode hm = isContinuous(mode) ? mode : (isContinuous(mode2) ? mode2 : M_SHIP);
                d = decideHold(pl, hm, pi, false);
            }
        } else {
            // Press: if both players (or the solo player) are continuous -> hold search; else click search
            if (cont0 && cont1) {
                Mode hm = isContinuous(mode) ? mode : mode2;
                d = decideHold(pl, hm, pi, true);
            } else {
                d = decideClick(pl, mode, pi);
            }
        }
        if (d.known && d.delay > 0 && d.orb) ++m_orb.delays;

        if (d.late > 0 && beginLate(pl, pi, po, mode, d.late)) {   // fatal now, safe k ticks ago: rewound
            m_game[pi] = push;
            return true;                                 // the press / release itself still goes through
        }
        if (!d.known || (!d.drop && d.delay == 0)) {    // not early: let it through
            m_game[pi] = push;
            return true;
        }
        if (d.drop) {
            m_supp[pi] = true;
            m_owed[pi] = true;
            return false;
        }
        ++m_dbgDelayed;
        logSaved(pi, mode, d.delay);
        pend.push_back({a + d.delay, push, d.frac, d.delay});
        m_arr[pi] = a;
        m_owed[pi] = true;
        return false;
    }

public:

    // ---- sub-tick click (CBF-like) -------------------------------------------------------
    // The held-back click is due INSIDE the tick that is about to run: returns the part of the
    // tick to run first (the hook then fires the click and runs the rest), 1 = no split.
    float splitFor(PlayerObject* player) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl || m_layout != Layout::Solo || player != pl->m_player1) return 1.f;
        if (cbfActive()) return 1.f;                 // CBF splits the step; two updates would eat its queue
        auto& pend = m_pend[0];
        if (pend.empty()) return 1.f;
        auto const& e = pend.front();
        if (e.frac >= 1.f || e.tick != m_now + 1) return 1.f;
        return std::clamp(e.frac, 0.03f, 0.97f);
    }
    void fireSplit() {
        PlayLayer* pl = PlayLayer::get();
        auto& pend = m_pend[0];
        if (!pl || pend.empty()) return;
        bool p = pend.front().push;
        int sv = pend.front().saved;
        pend.pop_front();
        fire(pl, 0, p);
        if (pl->m_player1) spawnFx(pl->m_player1->getPosition(), sv);
        if (pend.empty() && !m_supp[0]) m_owed[0] = false;
    }

    // Called at the start of every physics tick of a player (real game only).
    // Player 1 drives it; player 2's update is only a backup heartbeat for the
    // case where player 1's hook is not running.
    void onTick(PlayerObject* player) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl || !player) return;
        int frames = static_cast<int>(CCDirector::sharedDirector()->getTotalFrames());
        if (player == pl->m_player1) {
            m_lastP1Frame = frames;
        } else if (player == pl->m_player2) {
            if (frames - m_lastP1Frame <= 2) return;     // player 1 drives the tick
        } else {
            return;
        }
        m_lastTickFrame = frames;
        ++m_now;
        PlayerObject* p1 = pl->m_player1 ? pl->m_player1 : player;

        {   // measure the real wave slope (degrees) for the HUD + tick dx for CBF partial-tick timing
            float px = p1->getPositionX();
            float py = p1->getPositionY();
            float dx = px - m_prevX;
            float dy = py - m_prevY;
            if (p1->m_isDart && dx > 0.0001f && dx < 50.f && dy != 0.f && std::fabs(dy) < 80.f) {
                m_realAngle = std::atan2(std::fabs(dy), dx) * 57.29578f;
            }
            if (std::fabs(dx) > 0.05f && std::fabs(dx) < 80.f) m_prevTickDx = dx;
            m_tickStartX = px;
            m_prevX = px;
            m_prevY = py;
        }
        m_load *= 0.97f;
        if (m_noclipTicks > 0) --m_noclipTicks;

        auto& cf = cfg();
        m_layout = layoutOf(pl);
        if (m_layout != m_prevLayout) {
            // dual / 2-player started or ended: nothing held back may survive the change
            for (int i = 0; i < 2; ++i) {
                if (cf.enabled && m_owed[i] && !m_pend[i].empty() && (i == 0 || m_layout == Layout::TwoPlayer)) {
                    m_pend[i].clear();
                    m_supp[i] = false;
                    matchPhysical(pl, i);
                }
                m_pend[i].clear();
                m_supp[i] = false;
                m_owed[i] = false;
                m_stuck[i] = 0;
            }
            if (m_layout == Layout::Solo) {
                m_phys[1] = false;
                m_game[1] = false;
                if (cf.enabled) m_sweep = 6;   // only while mod is enabled
                else m_sweep = 0;
            } else {
                m_sweep = 0;
            }
            m_prevLayout = m_layout;
        }
        if (m_keepTicks > 0 && --m_keepTicks == 0) {
            m_keepOk.clear();
            m_keepFail.clear();
            drawResult();
        }

        bool autoOn = cf.autoplay && m_ready && m_layout != Layout::TwoPlayer &&
                      pl->m_player1 && !pl->m_player1->m_isDead;
        if (autoOn) {
            m_autoWas = true;
            m_owed[0] = true;
            autoTick(pl);
            return;
        }
        if (m_autoWas) {                          // autoplay just switched off: hand control back
            m_autoWas = false;
            m_owed[0] = true;
            m_pend[0].clear();
            m_supp[0] = false;
            matchPhysical(pl, 0);
            if (m_layout == Layout::TwoPlayer) {
                m_pend[1].clear();
                m_supp[1] = false;
                matchPhysical(pl, 1);
            }
        }

        // Mod disabled: never touch inputs or run reconcile/m_sweep; release once ONLY if we
        // had an owed hold right before the user switched Enabled off.
        if (!cf.enabled) {
            for (int i = 0; i < 2; ++i) {
                if (m_owed[i] && m_game[i] && !m_phys[i]) fire(pl, i, false);
                m_pend[i].clear();
                m_supp[i] = false;
                m_owed[i] = false;
                m_stuck[i] = 0;
            }
            m_sweep = 0;
            m_histN = 0;
            overlayTick(pl);
            return;
        }

        histPush(pl);

        int n = m_layout == Layout::TwoPlayer ? 2 : 1;
        m_justDecided = false;
        for (int pi = 0; pi < n; ++pi) tickPlayer(pl, pi);
        if (m_ready) reconcile(pl, n);
        if (!m_justDecided) overlayTick(pl);
    }

private:
    // Fires the held-back events once their tick has come (one per tick).
    void tickPlayer(PlayLayer* pl, int pi) {
        auto& c = cfg();
        PlayerObject* po = pi == 0 ? pl->m_player1 : pl->m_player2;
        if (!po) return;
        bool modeOk = !po->m_isDead && c.maxDelay[detectMode(po)] > 0;
        if (m_layout == Layout::DualSolo && pl->m_player1 && pl->m_player2) {
            modeOk = !pl->m_player1->m_isDead && !pl->m_player2->m_isDead &&
                     (c.maxDelay[detectMode(pl->m_player1)] > 0 ||
                      c.maxDelay[detectMode(pl->m_player2)] > 0);
        }
        bool active = c.enabled && m_ready && modeOk &&
                      !(m_layout == Layout::TwoPlayer && !c.twoPlayerMode) &&
                      !(m_layout == Layout::DualSolo && !c.dualSupport);
        auto& pend = m_pend[pi];
        if (!active) {
            pend.clear();
            m_supp[pi] = false;
            matchPhysical(pl, pi);
            return;
        }
        if (!pend.empty() && pend.front().tick <= m_now) {
            bool p = pend.front().push;
            int sv = pend.front().saved;
            pend.pop_front();
            fire(pl, pi, p);
            spawnFx(po->getPosition(), sv);
            if (pend.empty() && !m_supp[pi]) m_owed[pi] = false;
        }
    }

    enum class TouchKind { None, Gravity, Speed, Unknown };

    static float speedOf(int id) {
        switch (id) {
            case 200: return 0.7f;     // slow
            case 201: return 0.9f;     // normal
            case 202: return 1.1f;     // fast
            case 203: return 1.3f;     // faster
            case 1334: return 1.6f;    // fastest
            default: return 0.9f;
        }
    }

    static TouchKind classify(GameObject* o) {
        // D / J / S / H blocks are plain collision for the ghost: the game's own collision code
        // does the rest (wave slides on them). With "D block sim" off they stop the prediction.
        if (isSpecialBlock(o)) return cfg().dBlock ? TouchKind::None : TouchKind::Unknown;
        switch (o->m_objectType) {
            case GameObjectType::InverseGravityPortal:
            case GameObjectType::NormalGravityPortal:
            case GameObjectType::GravityTogglePortal:
                return TouchKind::Gravity;
            case GameObjectType::Modifier: {
                int id = o->m_objectID;
                if (id == 200 || id == 201 || id == 202 || id == 203 || id == 1334) {
                    return TouchKind::Speed;
                }
                return TouchKind::None;
            }
            // not predicted: the prediction stops here
            case GameObjectType::YellowJumpPad:
            case GameObjectType::PinkJumpPad:
            case GameObjectType::GravityPad:
            case GameObjectType::RedJumpPad:
            case GameObjectType::SpiderPad:
            case GameObjectType::CustomRing:             // trigger / toggle orb
            case GameObjectType::TeleportOrb:
            case GameObjectType::ShipPortal:
            case GameObjectType::CubePortal:
            case GameObjectType::BallPortal:
            case GameObjectType::UfoPortal:
            case GameObjectType::WavePortal:
            case GameObjectType::RobotPortal:
            case GameObjectType::SpiderPortal:
            case GameObjectType::SwingPortal:
            case GameObjectType::RegularSizePortal:
            case GameObjectType::MiniSizePortal:
            case GameObjectType::InverseMirrorPortal:
            case GameObjectType::NormalMirrorPortal:
            case GameObjectType::DualPortal:
            case GameObjectType::SoloPortal:
            case GameObjectType::TeleportPortal:
            case GameObjectType::Breakable:
                return TouchKind::Unknown;
            default:
                return TouchKind::None;
        }
    }

    // true = this ghost player already used this object
    bool markTouched(GameObject* obj, int who) {
        auto& t = m_info[m_curSlot].touched;
        int key = obj->m_uniqueID * 2 + who;
        if (std::find(t.begin(), t.end(), key) != t.end()) return true;
        t.push_back(key);
        return false;
    }

    void applyGravityPortal(PlayLayer* pl, PlayerObject* player, GameObjectType type) {
        bool upsideDown;
        switch (type) {
            case GameObjectType::InverseGravityPortal: upsideDown = true; break;
            case GameObjectType::NormalGravityPortal: upsideDown = false; break;
            case GameObjectType::GravityTogglePortal: upsideDown = !player->m_isUpsideDown; break;
            default: return;
        }
        pl->flipGravity(player, upsideDown, true);
    }

    // One line per saved input: "saved +4 ticks | Cube | P1".
    // Positive = the input was held back (delay), negative = moved back in time (late rescue).
    void logSaved(int pi, Mode mode, int ticks) {
        if (!cfg().logEnabled) return;
        m_log.push_back(fmt::format("saved {:+d} ticks | {} | P{}", ticks, kModeNames[mode], pi + 1));
        while (static_cast<int>(m_log.size()) > kLogMax) m_log.pop_front();   // oldest goes first
    }

    // ---- "saved" mark: a small white ring with a dark centre and the tick count next to it.
    // It stays where the input was saved (it scrolls with the level, it does not follow the
    // player) until the level restarts. At most kFxMax marks are kept.
    static constexpr size_t kFxMax = 64;

    void clearFx() {
        for (auto* l : m_fxLabels) {
            if (l) l->removeFromParent();
        }
        m_fxLabels.clear();
        m_fxPts.clear();
        if (m_fxDraw) m_fxDraw->clear();
    }

    void redrawFx(std::vector<CCPoint> const& pts) {
        m_fxDraw->clear();
        constexpr float r = 6.5f;
        constexpr int n = 24;
        for (auto const& c : pts) {
            CCPoint prev = ccp(c.x + r, c.y);
            for (int i = 1; i <= n; ++i) {
                float ang = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
                CCPoint cur = ccp(c.x + r * std::cos(ang), c.y + r * std::sin(ang));
                m_fxDraw->drawSegment(prev, cur, 1.4f, {1.f, 1.f, 1.f, 0.95f});
                prev = cur;
            }
        }
    }

    // `ticks` > 0 delay, < 0 rollback (same sign as the log)
    void spawnFx(CCPoint at, int ticks) {
        PlayLayer* pl = PlayLayer::get();
        if (!cfg().saveFx || !m_fxDraw || !pl) return;
        m_fxPts.push_back(at);
        auto* label = CCLabelBMFont::create(fmt::format("{:+d}", ticks).c_str(), "chatFont.fnt");
        if (label) {
            label->setScale(0.9f);
            label->setAnchorPoint({0.f, 0.5f});
            label->setPosition(ccp(at.x + 11.f, at.y));
            label->setZOrder(5002);
            pl->m_objectLayer->addChild(label);
            m_fxLabels.push_back(label);
        }
        if (m_fxPts.size() > kFxMax) {                 // oldest goes first
            m_fxPts.erase(m_fxPts.begin());
            if (!m_fxLabels.empty()) {
                if (m_fxLabels.front()) m_fxLabels.front()->removeFromParent();
                m_fxLabels.erase(m_fxLabels.begin());
            }
        }
        redrawFx(m_fxPts);
    }

    void fire(PlayLayer* pl, int pi, bool push) {
        m_firing = true;
        m_game[pi] = push;
        bool isP1 = (pi == 0);
        if (m_layout == Layout::TwoPlayer) {                // back to the side the game uses for this player
            isP1 = sideFlagFor(pi);
        } else if (m_layout == Layout::Solo && twoFlagOf(pl)) {
            if (m_sideMapSolo[1] == 0) isP1 = true;         // the side that really drives player 1
            else if (m_sideMapSolo[0] == 0) isP1 = false;
        }
        pl->handleButton(push, 1, isP1);
        m_firing = false;
    }

    // Hand the finger's real state back to the game, but ONLY for what this mod held back.
    // (Re-sending a stale "pressed" state was what kept the player holding after the mod
    // was turned off or dual mode ended.)
    void matchPhysical(PlayLayer* pl, int pi) {
        if (m_owed[pi] && m_game[pi] != m_phys[pi]) fire(pl, pi, m_phys[pi]);
        m_owed[pi] = false;
    }

    // ----------------------------------------------------------- ghost players
    PlayerObject* createSimPlayer(PlayLayer* pl) {
        PlayerObject* p = PlayerObject::create(1, 1, pl, pl, true);
        if (!p) return nullptr;
        p->retain();
        p->setPosition({0, 105});
        p->setVisible(false);
        pl->m_objectLayer->addChild(p);
        return p;
    }

    void ensureP2(PlayLayer* pl) {
        if (m_p[1][0]) return;
        for (int i = 0; i < kSlots; ++i) m_p[1][i] = createSimPlayer(pl);
    }

    void resetCollisionLog(PlayerObject* self) {
        self->m_collisionLogTop->removeAllObjects();
        self->m_collisionLogBottom->removeAllObjects();
        self->m_collisionLogLeft->removeAllObjects();
        self->m_collisionLogRight->removeAllObjects();
    }

    static bool isHoldingJump(PlayerObject* p) {
        if (!p) return false;
        auto it = p->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
        return it != p->m_holdingButtons.end() && it->second;
    }

    static void simUpdateJumpVars(PlayerObject* p) {
        if (!p) return;
        p->m_stateJumpBuffered = p->m_jumpBuffered;
        p->m_stateRingJump2 = p->m_stateRingJump;
        p->m_touchedRing = false;
        p->m_touchedCustomRing = false;
        p->m_touchedGravityPortal = false;
        p->m_maybeTouchedBreakableBlock = false;
    }

    void syncPlayer(PlayerObject* sim, PlayerObject* src) {
        sim->setVisible(false);
        // Only invoke heavy copyAttributes when vehicle / size / orientation actually differs
        bool modeDiff =
            sim->m_isShip != src->m_isShip ||
            sim->m_isBird != src->m_isBird ||
            sim->m_isBall != src->m_isBall ||
            sim->m_isDart != src->m_isDart ||
            sim->m_isRobot != src->m_isRobot ||
            sim->m_isSpider != src->m_isSpider ||
            sim->m_isSwing != src->m_isSwing ||
            sim->m_vehicleSize != src->m_vehicleSize ||
            sim->m_isSideways != src->m_isSideways ||
            sim->m_isPlatformer != src->m_isPlatformer;
        if (sim->m_isDashing && !src->m_isDashing) {
            sim->stopDashing();
        }
        if (modeDiff) {
            sim->copyAttributes(src);
        }
        sim->setPosition(src->getPosition());
        sim->m_position = src->m_position;
        sim->m_lastPosition = src->m_lastPosition;
        sim->setRotation(src->getRotation());
        sim->setScaleX(src->getScaleX());
        sim->setScaleY(src->getScaleY());
        sim->setContentSize(src->getContentSize());
        sim->m_width = src->m_width;
        sim->m_height = src->m_height;
        sim->m_isObjectRectDirty = true;
        sim->m_isOrientedBoxDirty = true;
        sim->m_isSecondPlayer = src->m_isSecondPlayer;

        // 2.2 physics flags (critical so ghost matches 2.2 physics!)
        sim->m_enable22Changes = src->m_enable22Changes;
        sim->m_fixRobotJump = src->m_fixRobotJump;
        sim->m_enableImpulseFix = src->m_enableImpulseFix;

        // Velocity / gravity / speed / direction
        sim->m_gravityMod = src->m_gravityMod;
        sim->m_gravity = src->m_gravity;
        sim->m_yVelocity = src->m_yVelocity;
        sim->m_fallSpeed = src->m_fallSpeed;
        sim->m_yStart = src->m_yStart;
        sim->m_vehicleSize = src->m_vehicleSize;
        sim->m_playerSpeed = src->m_playerSpeed;
        sim->m_speedMultiplier = src->m_speedMultiplier;
        sim->m_isUpsideDown = src->m_isUpsideDown;
        sim->m_isGoingLeft = src->m_isGoingLeft;
        sim->m_isSideways = src->m_isSideways;
        sim->m_reverseRelated = src->m_reverseRelated;
        sim->m_maybeReverseSpeed = src->m_maybeReverseSpeed;
        sim->m_maybeReverseAcceleration = src->m_maybeReverseAcceleration;
        sim->m_xVelocityRelated = src->m_xVelocityRelated;
        sim->m_xVelocityRelated2 = src->m_xVelocityRelated2;
        sim->m_isAccelerating = src->m_isAccelerating;
        sim->m_accelerationOrSpeed = src->m_accelerationOrSpeed;
        sim->m_isDead = false;
        sim->m_isLocked = src->m_isLocked;
        sim->m_controlsDisabled = src->m_controlsDisabled;
        sim->m_inputsLocked = src->m_inputsLocked;
        sim->m_isOutOfBounds = src->m_isOutOfBounds;
        sim->m_fallStartY = src->m_fallStartY;
        sim->m_rotationSpeed = src->m_rotationSpeed;
        sim->m_rotateSpeed = src->m_rotateSpeed;
        sim->m_isRotating = src->m_isRotating;
        sim->m_shipRotation = src->m_shipRotation;

        // Ground & collision state
        sim->m_isOnGround = src->m_isOnGround;
        sim->m_isOnGround2 = src->m_isOnGround2;
        sim->m_isOnGround3 = src->m_isOnGround3;
        sim->m_isOnGround4 = src->m_isOnGround4;
        sim->m_stateOnGround = src->m_stateOnGround;
        sim->m_lastGroundedPos = src->m_lastGroundedPos;
        sim->m_lastGroundObject = src->m_lastGroundObject;
        sim->m_collidedObject = src->m_collidedObject;
        sim->m_collidedTopMinY = src->m_collidedTopMinY;
        sim->m_collidedBottomMaxY = src->m_collidedBottomMaxY;
        sim->m_collidedLeftMaxX = src->m_collidedLeftMaxX;
        sim->m_collidedRightMinX = src->m_collidedRightMinX;
        sim->m_maybeIsColliding = src->m_maybeIsColliding;

        // Special blocks (D / J / S / H / F blocks) & forces
        sim->m_stateDartSlide = src->m_stateDartSlide;
        sim->m_stateNoAutoJump = src->m_stateNoAutoJump;
        sim->m_stateHitHead = src->m_stateHitHead;
        sim->m_stateFlipGravity = src->m_stateFlipGravity;
        sim->m_stateNoStickX = src->m_stateNoStickX;
        sim->m_stateNoStickY = src->m_stateNoStickY;
        sim->m_stateBoostX = src->m_stateBoostX;
        sim->m_stateBoostY = src->m_stateBoostY;
        sim->m_stateScale = src->m_stateScale;
        sim->m_stateForce = src->m_stateForce;
        sim->m_stateForceVector = src->m_stateForceVector;
        sim->m_affectedByForces = src->m_affectedByForces;
        sim->m_maybeIsBoosted = src->m_maybeIsBoosted;
        sim->m_decreaseBoostSlide = src->m_decreaseBoostSlide;
        sim->m_blackOrbRelated = src->m_blackOrbRelated;

        // Slope physics state
        sim->m_isOnSlope = src->m_isOnSlope;
        sim->m_wasOnSlope = src->m_wasOnSlope;
        sim->m_slopeVelocity = src->m_slopeVelocity;
        sim->m_currentSlopeYVelocity = src->m_currentSlopeYVelocity;
        sim->m_slopeRotation = src->m_slopeRotation;
        sim->m_isCurrentSlopeTop = src->m_isCurrentSlopeTop;
        sim->m_maybeUpsideDownSlope = src->m_maybeUpsideDownSlope;
        sim->m_maybeGoingCorrectSlopeDirection = src->m_maybeGoingCorrectSlopeDirection;
        sim->m_maybeSlopeForce = src->m_maybeSlopeForce;
        sim->m_slopeEndTime = src->m_slopeEndTime;
        sim->m_currentSlope = src->m_currentSlope;
        sim->m_currentSlope2 = src->m_currentSlope2;

        // Jump / robot / orb / dash timing state
        sim->m_totalTime = src->m_totalTime;
        sim->m_lastJumpTime = src->m_lastJumpTime;
        sim->m_lastFlipTime = src->m_lastFlipTime;
        sim->m_lastLandTime = src->m_lastLandTime;
        sim->m_lastSpiderFlipTime = src->m_lastSpiderFlipTime;
        sim->m_gameModeChangedTime = src->m_gameModeChangedTime;
        sim->m_jumpBuffered = src->m_jumpBuffered;
        sim->m_wasJumpBuffered = src->m_wasJumpBuffered;
        sim->m_wasRobotJump = src->m_wasRobotJump;
        sim->m_stateJumpBuffered = src->m_stateJumpBuffered;
        sim->m_stateRingJump = src->m_stateRingJump;
        sim->m_stateRingJump2 = src->m_stateRingJump2;
        sim->m_ringJumpRelated = src->m_ringJumpRelated;
        sim->m_touchedRing = src->m_touchedRing;
        sim->m_touchedCustomRing = src->m_touchedCustomRing;
        sim->m_touchedGravityPortal = src->m_touchedGravityPortal;
        sim->m_touchedPad = src->m_touchedPad;
        sim->m_isDashing = src->m_isDashing;
        sim->m_dashRing = src->m_dashRing;
        sim->m_dashFireFrame = src->m_dashFireFrame;
        sim->m_holdingButtons = src->m_holdingButtons;
        sim->m_touchedRings = src->m_touchedRings;

        // Copy currently overlapping rings (so pre-update orb checks match real GD!)
        if (sim->m_touchingRings) {
            sim->m_touchingRings->removeAllObjects();
            if (src->m_touchingRings && src->m_touchingRings->count() > 0) {
                for (unsigned int i = 0; i < src->m_touchingRings->count(); ++i) {
                    if (auto* r = src->m_touchingRings->objectAtIndex(i)) {
                        sim->m_touchingRings->addObject(r);
                    }
                }
            }
        }
        resetCollisionLog(sim);
    }

    void forceHoldObj(int s, int who, bool hold) {
        m_info[s].holding[who] = hold;
        PlayerObject* p = m_p[who][s];
        if (!p) return;
        if (hold) p->pushButton(PlayerButton::Jump);
        else p->releaseButton(PlayerButton::Jump);
    }

    // sets the hold state of the TARGET player (and mirrors to the other player in 1-player dual)
    void setHold(int s, bool hold) {
        if (m_info[s].holding[m_tp] != hold) forceHoldObj(s, m_tp, hold);
        if (m_mirror) {
            int other = 1 - m_tp;
            if (m_info[s].holding[other] != hold) forceHoldObj(s, other, hold);
        }
    }

    // hold state the ghost starts with for a player
    bool holdFor(int who) const {
        if (m_mirror) return m_game[0];
        return m_game[who];
    }

    void loadReal(int s) {
        PlayLayer* pl = PlayLayer::get();
        syncPlayer(m_p[0][s], pl->m_player1);
        if (m_two) syncPlayer(m_p[1][s], pl->m_player2);
        m_info[s] = SlotInfo();
        m_info[s].firstScale = m_loadScale;
        m_info[s].grounded = m_p[m_tp][s]->m_isOnGround;
        m_info[s].groundTicks = m_info[s].grounded ? 1 : 0;
        // Sync holding[] from real player without firing a fake pushButton() click!
        // Only call forceHoldObj if holdFor(who) genuinely disagrees with the real player's state.
        m_info[s].holding[0] = isHoldingJump(m_p[0][s]);
        if (m_info[s].holding[0] != holdFor(0)) forceHoldObj(s, 0, holdFor(0));
        if (m_two) {
            m_info[s].holding[1] = isHoldingJump(m_p[1][s]);
            if (m_info[s].holding[1] != holdFor(1)) forceHoldObj(s, 1, holdFor(1));
        }
    }

    void copySlot(int dst, int src) {
        syncPlayer(m_p[0][dst], m_p[0][src]);
        if (m_two) syncPlayer(m_p[1][dst], m_p[1][src]);
        m_info[dst] = m_info[src];
        // Note: NEVER call forceHoldObj here — syncPlayer already copied exact button/buffer state;
        // re-calling pushButton() on a held slot used to trigger phantom UFO/Swing/orb clicks!
    }

    CCPoint pos(int s) const { return m_p[m_tp][s]->getPosition(); }

    // Wave: optional fixed slope (up / down differ slightly, mini is steeper).
    // The ghost's own slope is recorded for the HUD before any override.
    void applyWaveAngle(PlayerObject* w, float x0, float y0, bool record) {
        if (!w->m_isDart) return;
        float dx = w->getPositionX() - x0;
        float dy = w->getPositionY() - y0;
        if (dx <= 0.0001f || dy == 0.f) return;
        if (record) m_ghostAngle = std::atan2(std::fabs(dy), dx) * 57.29578f;
        auto& c = cfg();
        if (!c.waveAngles) return;
        bool mini = w->m_vehicleSize < 0.9f;
        bool up = dy > 0.f;
        float deg = static_cast<float>(up ? (mini ? c.miniUp : c.waveUp)
                                          : (mini ? c.miniDown : c.waveDown)) / 100.f;
        float ndy = dx * std::tan(deg * 0.017453292f);
        w->setPosition(CCPoint(w->getPositionX(), y0 + (up ? ndy : -ndy)));
    }

    // One physics tick for a slot. `hold` is the target player's input (mirrored to both players
    // in 1-player dual). false = a player died or the budget ran out.
    bool step(int s, bool hold) {
        PlayLayer* pl = PlayLayer::get();
        PlayerObject* a = m_p[0][s];
        PlayerObject* b = m_two ? m_p[1][s] : nullptr;
        // 2-player mode: the two players are independent. Only the target runs.
        if (m_two && m_layout == Layout::TwoPlayer) {
            if (m_tp == 0) b = nullptr;
            else a = nullptr;
        }
        int cost = (a ? 1 : 0) + (b ? 1 : 0);
        if (m_used + cost > m_budget) {
            m_aborted = true;
            return false;
        }
        m_used += cost;

        float full = stepDt();
        float scale = (m_dtScale != 1.f) ? m_dtScale : m_info[s].firstScale;
        float dt = full * scale;
        m_stepDtNow = full;

        // Set step context BEFORE setHold() so if pushButton() fires an orb in m_touchingRings,
        // beforeSimRingJump / afterSimRingJump see the exact current slot and click edge!
        bool wasHold = m_info[s].holding[m_tp];
        m_simDead = false;
        m_stepObj[0] = a;
        m_stepObj[1] = b;
        m_stepEdge[0] = m_stepEdge[1] = false;
        m_orb.done[0] = m_orb.done[1] = false;
        m_stepEdge[m_tp] = hold && !wasHold;
        if (m_mirror) m_stepEdge[1 - m_tp] = m_stepEdge[m_tp];
        m_curSlot = s;
        m_hitUnknown = false;

        setHold(s, hold);

        // If we JUST pressed on a tick where the ghost is already overlapping an unconsumed orb
        // (carried over in m_touchingRings from the previous tick) and pushButton() didn't fire it
        // yet, fire the closest one before update(dt) so orbs take priority over UFO/Swing/Cube.
        auto preUpdateOrb = [&](PlayerObject* p, int who) {
            if (!p || !m_stepEdge[who] || m_orb.done[who] || !p->m_touchingRings ||
                p->m_touchingRings->count() == 0 || !canFireOrbNow(p, who)) {
                return;
            }
            CCRect pr = p->getObjectRect();
            RingObject* best = nullptr;
            float bestD = std::numeric_limits<float>::max();
            for (unsigned int i = 0; i < p->m_touchingRings->count(); ++i) {
                auto* ring = geode::cast::typeinfo_cast<RingObject*>(p->m_touchingRings->objectAtIndex(i));
                if (!ring || !isSimOrb(ring) || isRingConsumed(s, who, ring)) continue;
                CCRect rr = ring->getObjectRect();
                if (!rr.intersectsRect(pr)) continue;
                float dx = rr.getMidX() - pr.getMidX();
                float dy = rr.getMidY() - pr.getMidY();
                float d = dx * dx + dy * dy;
                if (d < bestD) {
                    bestD = d;
                    best = ring;
                }
            }
            if (best) {
                m_active = p;
                p->m_stateRingJump = true;
                p->ringJump(best, true);
            }
        };
        preUpdateOrb(a, 0);
        preUpdateOrb(b, 1);

        float aPrevX = a ? a->getPositionX() : 0.f;
        if (a) {
            float ax = a->getPositionX();
            float ay = a->getPositionY();
            m_active = a;
            a->update(dt);
            a->updateRotation(dt);
            applyWaveAngle(a, ax, ay, m_tp == 0);
        }
        if (b) {
            float bx = b->getPositionX();
            float by = b->getPositionY();
            m_active = b;
            b->update(dt);
            b->updateRotation(dt);
            applyWaveAngle(b, bx, by, m_tp == 1);
        }
        if (m_simDead) {
            m_active = nullptr;
            return false;
        }
        SlotInfo& inf = m_info[s];
        if (m_partial) {                  // first half of a split tick: collisions run after the second half
            inf.prevSimTime = inf.simTime;
            inf.simTime += static_cast<double>(dt) / 60.0;
            m_active = nullptr;
            return true;
        }
        if (m_dtScale == 1.f) {           // normal (or CBF-remaining) tick
            inf.prevSimTime = inf.simTime;
        }
        inf.simTime += static_cast<double>(dt) / 60.0;
        inf.firstScale = 1.f;

        // Check if ghost player 1 crossed any upcoming x-activated Move Trigger on this step
        if (a) {
            triggerSimMoves(pl, s, aPrevX, a->getPositionX(), a->getPositionY());
        } else if (b) {
            triggerSimMoves(pl, s, b->m_lastPosition.x, b->getPositionX(), b->getPositionY());
        }

        float colDt = (m_dtScale != 1.f) ? full : dt;
        if (a) {
            m_active = a;
            if (a->m_touchingRings) a->m_touchingRings->removeAllObjects();
            resetCollisionLog(a);
            pl->checkCollisions(a, colDt, false);
            simUpdateJumpVars(a);
        }
        if (!m_simDead && b) {
            m_active = b;
            if (b->m_touchingRings) b->m_touchingRings->removeAllObjects();
            resetCollisionLog(b);
            pl->checkCollisions(b, colDt, false);
            simUpdateJumpVars(b);
        }
        m_active = nullptr;
        if (m_simDead) return false;

        if (m_hitUnknown) inf.unknown = true;
        inf.grounded = m_p[m_tp][s]->m_isOnGround;
        inf.groundTicks = inf.grounded ? inf.groundTicks + 1 : 0;
        ++inf.ticks;
        return true;
    }

    void beginSearch(PlayLayer* pl, int target, bool decision = true, bool mirror = false) {
        auto& c = cfg();
        m_two = m_layout != Layout::Solo;
        m_mirror = mirror;
        m_tp = target;
        bool two = m_two;
        m_evGrav = m_evSpeed = m_evUnknown = 0;
        if (two) ensureP2(pl);
        if (pl && pl->m_player1) {
            m_realP1X = pl->m_player1->getPositionX();
            m_realP1Y = pl->m_player1->getPositionY();
        }
        // CBF: when a click arrives mid-tick (m_inUpdate == true), estimate how much of the
        // running tick remains so the ghost's first step lands on the EXACT next tick boundary
        // where tickPlayer() will fire a delayed input.
        m_loadScale = 1.f;
        if (decision && m_inUpdate && pl && pl->m_player1 && std::fabs(m_prevTickDx) > 0.05f) {
            float frac = (pl->m_player1->getPositionX() - m_tickStartX) / m_prevTickDx;
            frac = std::clamp(frac, 0.f, 0.95f);
            m_loadScale = std::clamp(1.f - frac, 0.05f, 1.f);
        }
        m_simulating = true;
        m_simDead = false;
        m_active = nullptr;
        m_used = 0;
        // spam: the more searches ran recently, the smaller each one may be
        float scale = std::max(0.15f, std::min(1.f, 1.f - (m_load - 2.f) / 6.f));
        m_budget = std::max(c.minBudget, static_cast<int>(c.budget * scale));
        if (decision) {
            m_load += 1.f;
            m_justDecided = true;
        }
        m_aborted = false;
        m_reach = 0;
        m_wantTrail = c.showTrajectory;
        m_trail.clear();
        m_trailOk.clear();
        m_trailFail.clear();
    }

    void endSearch(bool decision = true, bool redraw = true) {
        m_simulating = false;
        m_active = nullptr;
        m_loadScale = 1.f;
        if (decision && m_wantTrail) {      // keep the click path on screen for a while
            m_keepOk = m_trailOk;
            m_keepFail = m_trailFail;
            m_keepTicks = cfg().keepTicks;
        }
        if (redraw) drawResult();
    }

    void trailPush(int s, int tag) {
        if (m_wantTrail) m_trail.push_back({pos(s), tag});
    }

    void noteDeath(int s) {
        if (!m_wantTrail || m_simDead == false) return;
        if (m_trail.size() >= m_trailFail.size()) {
            m_trailFail = m_trail;
            m_trailFail.push_back({pos(s), 3});
        }
    }

    // ============================================================ click modes
    // Press at the delay, hold a short tap, release, and let physics run.
    // `horizon` is counted AFTER the click (not from t = 0) so larger delays are verified
    // for the full jump arc instead of passing early because `d` ate into `horizon`.
    bool rollClick(int s, int d, int holdTicks, int horizon, bool airMode, int& reach, bool& unk) {
        int tick = d;
        int endTick = d + horizon;
        int holdLeft = holdTicks;
        while (true) {
            if (tick >= endTick) return true;
            if (!airMode && holdLeft == 0 && m_info[s].groundTicks >= cfg().settle) return true;
            if (m_aborted) return false;
            bool dashing = m_p[m_tp][s]->m_isDashing || (m_mirror && m_p[1 - m_tp][s] && m_p[1 - m_tp][s]->m_isDashing);
            bool h = (holdLeft > 0) || dashing;
            if (!step(s, h)) {
                noteDeath(s);
                return false;
            }
            trailPush(s, 0);
            if (m_info[s].unknown) {                 // reached something we cannot predict
                unk = true;
                return true;
            }
            ++tick;
            reach = std::max(reach, tick);
            if (holdLeft > 0) --holdLeft;
        }
    }

    // ---- sub-tick search -------------------------------------------------------------
    // Used ONLY on 1-tick tight windows (when d works but d-1 and d+1 both fail).
    // Bisects tick d-1 (slot 2 = state at the start of tick d-1) to find the earliest safe fraction,
    // then aims for the SAFE INTERIOR `(hi + 1.0) * 0.5` instead of the lethal razor edge `hi`.
    bool trialSplit(bool click, int pi, int d, int h, int horizon, bool airMode, bool target, float f, bool groundMode) {
        orbBegin(false);
        copySlot(1, 2);
        m_info[1].groundTicks = 0;
        m_partial = true;
        m_dtScale = f;
        bool ok = step(1, m_game[pi]);                 // movement up to the click, no collision yet
        m_partial = false;
        if (ok) {
            m_dtScale = 1.f - f;
            ok = step(1, click ? true : target);       // rest of the tick WITH the click, collisions now
        }
        m_dtScale = 1.f;
        if (!ok || m_aborted || m_info[1].unknown) return false;
        if (click) {
            int reach = d;
            bool unk = false;
            bool good = rollClick(1, d, std::max(0, h - 1), std::max(1, horizon - 1), airMode, reach, unk);
            if (good && groundMode && !m_info[2].grounded && !orbUsed()) return false;
            return good && !unk && !m_aborted;
        }
        for (int t = 1; t < horizon; ++t) {
            if (!step(1, target)) return false;
            if (m_info[1].unknown) return false;
        }
        return !m_aborted;
    }

    float refineFrac(bool click, int pi, int d, int h, int horizon, bool airMode, bool target, bool groundMode = false) {
        float lo = 0.f, hi = 1.f;                      // lo = fails, hi = works
        for (int i = 0; i < 5; ++i) {
            if (m_used + horizon + 4 > m_budget) break;
            float mid = (lo + hi) * 0.5f;
            if (trialSplit(click, pi, d, h, horizon, airMode, target, mid, groundMode)) hi = mid;
            else lo = mid;
            if (m_aborted) {                           // out of budget: keep the whole-tick delay
                m_aborted = false;
                return 1.f;
            }
        }
        // Stay away from the death edge `hi`: aim for the safe midpoint of [hi, 1.0]
        return hi < 0.88f ? (hi + 1.f) * 0.5f : 1.f;
    }

    static bool isGroundMode(Mode m) {
        return m == M_CUBE || m == M_BALL || m == M_ROBOT || m == M_SPIDER;
    }

    Decision decideClick(PlayLayer* pl, Mode mode, int pi) {
        auto& c = cfg();
        Decision r;
        bool dual = (m_layout == Layout::DualSolo && pl->m_player1 && pl->m_player2);
        Mode m0 = dual ? detectMode(pl->m_player1) : mode;
        Mode m1 = dual ? detectMode(pl->m_player2) : mode;

        int maxDelay = c.maxDelay[mode];
        if (dual) {
            int d0 = c.maxDelay[m0], d1 = c.maxDelay[m1];
            maxDelay = (d0 > 0 && d1 > 0) ? std::min(d0, d1) : std::max(d0, d1);
        }
        if (maxDelay <= 0) return r;

        bool airMode = dual ? (!isGroundMode(m0) || !isGroundMode(m1)) : (mode == M_UFO || mode == M_SWING);
        // Ground modes only react to a click from the ground (or on an orb). In 1-player dual,
        // a click is useful if EITHER player is in an air/continuous mode or on the ground.
        bool groundMode = dual ? (isGroundMode(m0) && isGroundMode(m1)) : isGroundMode(mode);
        int horizon = airMode ? std::min(c.trajectory, c.airHorizon) : c.trajectory;
        if (dual && (isContinuous(m0) || isContinuous(m1))) {
            int contH = (m0 == M_WAVE || m1 == M_WAVE) ? c.waveHorizon : c.shipHorizon;
            horizon = std::min(horizon, std::max(contH, c.airHorizon));
        }
        std::vector<int> holds = {c.tapTicks};
        if (m0 == M_ROBOT || (dual && m1 == M_ROBOT)) holds.push_back(std::max(c.tapTicks, c.robotHold));

        beginSearch(pl, pi, true, dual);
        loadReal(0);

        auto canActAt = [&](int slot) -> bool {
            if (!groundMode) return true;
            if (m_p[m_tp][slot]->m_isOnGround) return true;
            if (dual && m_p[1 - m_tp][slot] && m_p[1 - m_tp][slot]->m_isOnGround) return true;
            return false;
        };

        int foundDelay = -1;
        int foundHold = holds[0];
        bool foundOrb = false;
        bool sawUnknown = false;                 // the prediction ended on something it cannot predict
        for (int d = 0; d <= maxDelay; ++d) {
            if (d > 0) {
                copySlot(2, 0);                  // state at the start of tick d-1 (for sub-tick)
                if (!step(0, m_game[pi])) {
                    noteDeath(0);
                    break;                       // waiting longer is deadly too
                }
                trailPush(0, 0);
                if (m_info[0].unknown) {         // cannot predict any further
                    sawUnknown = true;
                    break;
                }
            }
            int reach = d;
            bool ok = false;
            bool canAct = canActAt(0);
            for (int h : holds) {
                copySlot(1, 0);
                m_info[1].groundTicks = 0;
                size_t mark = m_trail.size();
                trailPush(0, 1);
                bool unk = false;
                if (groundMode && !canAct) {
                    // In the air only an orb hit ON tick `d` itself makes the click reliably useful
                    // (even for a 1-tick micro-tap). Fast 1-tick probe avoids wasting budget during falls.
                    orbBegin(false);
                    int r0 = d;
                    bool u0 = false;
                    bool probe = rollClick(1, d, h, 1, false, r0, u0);
                    if (!probe || !orbUsed()) {
                        m_trail.resize(mark);
                        if (m_aborted) break;
                        continue;
                    }
                    copySlot(1, 0);
                    m_info[1].groundTicks = 0;
                    orbBegin(false);
                } else {
                    // Ground / air-mode click: allow GD-style orb buffering so clicking early into
                    // a deadly orb ahead is accurately detected!
                    orbBegin(true);
                }
                ok = rollClick(1, d, h, horizon, airMode, reach, unk);
                if (ok && unk && d > 0) {             // "cannot predict further" is no rescue
                    ok = false;
                    sawUnknown = true;
                }
                if (ok && !canAct && !orbUsed()) ok = false;   // wasted click in the air
                if (ok) {
                    foundDelay = d;
                    foundHold = h;
                    foundOrb = orbUsed();
                    if (m_wantTrail) m_trailOk = m_trail;
                    break;
                }
                m_trail.resize(mark);
                if (m_aborted) break;
            }
            if (ok || m_aborted) break;
        }

        if (foundDelay >= 0) {
            r.known = true;
            r.delay = foundDelay;
            r.orb = foundOrb;

            // Push a delayed click `safeMargin` ticks INTO the safe window so it never fires
            // on the lethal razor edge (0.1px from dying).
            if (foundDelay > 0 && !m_aborted) {
                int margin = std::max(0, c.safeMargin);
                bool savedPrev = false;
                if (c.subTick && m_layout == Layout::Solo && !cbfActive()) {
                    copySlot(4, 2);                   // keep state at foundDelay - 1 before advancing slot 0
                    savedPrev = true;
                }
                int chosen = foundDelay;
                for (int stepM = 1; stepM <= margin && foundDelay + stepM <= maxDelay; ++stepM) {
                    int d2 = foundDelay + stepM;
                    if (!step(0, m_game[pi])) break;
                    if (m_info[0].unknown) break;
                    bool canAct2 = canActAt(0);
                    if (groundMode && !canAct2 && !foundOrb) break;
                    copySlot(1, 0);
                    m_info[1].groundTicks = 0;
                    size_t mark = m_trail.size();
                    trailPush(0, 1);
                    orbBegin(!foundOrb && canAct2);
                    int r2 = d2;
                    bool u2 = false;
                    bool ok2 = rollClick(1, d2, foundHold, horizon, airMode, r2, u2);
                    if (ok2 && !u2 && !m_aborted && (!foundOrb || orbUsed()) && (canAct2 || orbUsed())) {
                        chosen = d2;
                        if (m_wantTrail) m_trailOk = m_trail;
                    } else {
                        m_trail.resize(mark);
                        break;                        // reached end of safe window
                    }
                }
                r.delay = chosen;
                // Only use sub-tick bisect on a true 1-tick tight window (chosen == foundDelay)
                if (chosen == foundDelay && savedPrev && !m_aborted) {
                    copySlot(2, 4);
                    r.frac = refineFrac(true, pi, foundDelay, foundHold, horizon, airMode, false, groundMode);
                }
            }
        } else {
            // no delay saves it. Moving the click back in time comes first, dropping it is the one
            // rescue left (opt-in, and only when NOT clicking survives)
            if (!m_aborted && !sawUnknown) {
                r.doomed = true;
                r.late = lateSearch(pl, mode, pi, true, holds, horizon);
            }
            if (r.late == 0 && c.dropFatal && !m_aborted) {
                loadReal(0);
                bool alive = true;
                for (int t = 0; t < horizon && alive; ++t) alive = step(0, m_game[pi]);
                if (alive && !m_aborted) {
                    r.known = true;
                    r.drop = true;
                }
            }
        }   // else: nothing saves it -> raw click

        ++m_dbgSearches;
        m_lastText = fmt::format(
            "P{} click d={} {} used={}{}",
            pi + 1, r.delay, foundDelay >= 0 ? "SAFE" : (r.late > 0 ? "LATE" : (r.drop ? "DROP" : "raw")),
            m_used, m_aborted ? " ABORT" : ""
        );
        endSearch();
        return r;
    }

    // ======================================================= late-click rescue
    // A press / release that arrives too late: doing it NOW is fatal and no delay helps. The state of
    // player 1 at the start of each of the last ticks is kept (ghost copies). For k = 1, 2, ... the
    // ghost replays k ticks from there WITH the input applied and checks that the run then survives.
    // The smallest k that does wins and the real player takes over the replayed state (applyLate),
    // as if the input had come k ticks earlier. Cube, ship, wave and robot only: a second press in
    // the air would repeat the impulse of UFO / swing / ball / spider.
    static bool lateMode(Mode m) { return m == M_CUBE || m == M_SHIP || m == M_WAVE || m == M_ROBOT; }

    void destroyHist() {
        for (int i = 0; i < kHist; ++i) {
            if (m_hist[i]) {
                m_hist[i]->removeFromParent();
                m_hist[i]->release();
                m_hist[i] = nullptr;
            }
        }
        m_histN = 0;
        m_histHead = 0;
    }

    bool ensureHist(PlayLayer* pl) {
        if (m_hist[0]) return true;
        for (int i = 0; i < kHist; ++i) {
            m_hist[i] = createSimPlayer(pl);
            if (!m_hist[i]) {
                destroyHist();
                return false;
            }
        }
        return true;
    }

    // start of every real tick, before held-back events fire: remember player 1
    void histPush(PlayLayer* pl) {
        auto& c = cfg();
        PlayerObject* p1 = pl->m_player1;
        if (!c.lateRescue || !m_ready || m_layout != Layout::Solo || !p1 || p1->m_isDead) {
            m_histN = 0;
            return;
        }
        Mode mode = detectMode(p1);
        if (!lateMode(mode) || c.maxLate[mode] <= 0 || !ensureHist(pl)) {
            m_histN = 0;
            return;
        }
        // older states do not apply after a vehicle change or a gap in the ticks
        if (m_histN > 0 && (m_histMode[m_histHead] != mode || m_histTick[m_histHead] != m_now - 1)) m_histN = 0;
        m_histHead = (m_histHead + 1) % kHist;
        syncPlayer(m_hist[m_histHead], p1);
        m_histHold[m_histHead] = m_game[0];
        m_histTick[m_histHead] = m_now;
        m_histMode[m_histHead] = mode;
        if (m_histN < kHist) ++m_histN;
    }

    void loadHist(int s, int idx) {
        syncPlayer(m_p[0][s], m_hist[idx]);
        m_info[s] = SlotInfo();
        m_info[s].grounded = m_p[0][s]->m_isOnGround;
        m_info[s].groundTicks = m_info[s].grounded ? 1 : 0;
        m_info[s].holding[0] = isHoldingJump(m_p[0][s]);
        if (m_info[s].holding[0] != m_histHold[idx]) forceHoldObj(s, 0, m_histHold[idx]);
    }

    // Called inside a decide*() search (beginSearch already ran). Returns k > 0 when the input would
    // have been safe k ticks ago; the state the real player should be in is then in kLateSlot.
    int lateSearch(PlayLayer* pl, Mode mode, int pi, bool target, std::vector<int> const& holds, int horizon) {
        (void)pl;
        auto& c = cfg();
        if (!c.lateRescue || pi != 0 || m_two || m_layout != Layout::Solo || !lateMode(mode)) return 0;
        int maxK = std::min(c.maxLate[mode], kHist);
        if (maxK <= 0 || m_histN <= 0) return 0;

        // vanilla: the real player stands at the start of tick m_now + 1. CBF: inside tick m_now.
        int nowOff = m_inUpdate ? 0 : 1;
        bool continuous = isContinuous(mode);
        bool wantTrail = m_wantTrail;
        m_wantTrail = false;                           // no trail for the late search
        m_orb.enabled = false;                         // a rescue never uses an orb
        int found = 0;
        for (int k = 1; k <= maxK && !found; ++k) {
            int off = k - 1 + (m_inUpdate ? 1 : 0);
            if (off >= m_histN) break;
            int idx = ((m_histHead - off) % kHist + kHist) % kHist;
            if (!m_hist[idx] || m_histMode[idx] != mode || m_histTick[idx] != m_now + nowOff - k) break;

            bool startGround = m_hist[idx]->m_isOnGround;
            // a click only counts when it does something: from the ground (never from an orb)
            if (!continuous && !startGround) continue;
            loadHist(0, idx);
            m_latePath.clear();
            bool alive = true;
            for (int t = 0; t < k && alive; ++t) {
                bool hold = continuous ? target : t < c.tapTicks;      // a click is a tap, ship / wave keep the input
                alive = step(0, hold);
                if (alive && m_info[0].unknown) alive = false;          // something we cannot predict: no rewind
                if (alive) m_latePath.push_back(m_p[0][0]->getPosition());
            }
            if (m_aborted) break;
            if (!alive) continue;

            copySlot(kLateSlot, 0);                    // what the real player should look like right now
            if (continuous) {
                copySlot(1, 0);
                bool ok = true;
                for (int t = 0; t < horizon && ok; ++t) ok = step(1, target) && !m_info[1].unknown;
                if (ok && !m_aborted) {
                    found = k;
                    m_lateIdx = idx;
                }
            } else {
                for (int h : holds) {
                    copySlot(1, 0);
                    m_info[1].groundTicks = 0;
                    int reach = 0;
                    bool unk = false;
                    bool ok = rollClick(1, 0, std::max(0, h - k), horizon, false, reach, unk);
                    if (ok && !unk && !m_aborted) {
                        found = k;
                        m_lateIdx = idx;
                        break;
                    }
                    if (m_aborted) break;
                }
            }
            if (m_aborted) break;
        }
        m_wantTrail = wantTrail;
        m_orb.enabled = true;
        return found;
    }

    // Physics only: no copyAttributes, no rotation, no visibility. Those reset the icon's animation
    // (jump rotation, robot / spider frames), which looked stiff.
    void copyPhysics(PlayerObject* po, PlayerObject* g) {
        po->setPosition(g->getPosition());
        po->m_position = g->m_position;
        po->m_lastPosition = g->m_lastPosition;
        po->m_isObjectRectDirty = true;
        po->m_isOrientedBoxDirty = true;
        po->m_gravityMod = g->m_gravityMod;
        po->m_isOnGround = g->m_isOnGround;
        po->m_isOnGround2 = g->m_isOnGround2;
        po->m_isOnGround3 = g->m_isOnGround3;
        po->m_isOnGround4 = g->m_isOnGround4;
        po->m_stateOnGround = g->m_stateOnGround;
        po->m_isUpsideDown = g->m_isUpsideDown;
        po->m_vehicleSize = g->m_vehicleSize;
        po->m_playerSpeed = g->m_playerSpeed;
        po->m_yVelocity = g->m_yVelocity;
        po->m_gravity = g->m_gravity;
        po->m_speedMultiplier = g->m_speedMultiplier;
        po->m_isOnSlope = g->m_isOnSlope;
        po->m_wasOnSlope = g->m_wasOnSlope;
        po->m_slopeVelocity = g->m_slopeVelocity;
        po->m_currentSlopeYVelocity = g->m_currentSlopeYVelocity;
        po->m_totalTime = g->m_totalTime;
        po->m_lastJumpTime = g->m_lastJumpTime;
        po->m_touchedRings = g->m_touchedRings;
        if (po->m_touchingRings) po->m_touchingRings->removeAllObjects();
        resetCollisionLog(po);
    }

    // Phase 1 (called by rescue(), before the press is delivered): put the real player back at the
    // state k ticks ago. The game then handles the press itself, on the ground, so the jump / its
    // animation really start (a press in the air would not play them).
    bool beginLate(PlayLayer* pl, int pi, PlayerObject* po, Mode mode, int k) {
        PlayerObject* h = m_hist[m_lateIdx];
        if (pi != 0 || !h || !m_p[0][kLateSlot] || !po || po != pl->m_player1 || po->m_isDead || m_simulating) return false;
        copyPhysics(po, h);
        m_latePending = k;
        m_lateMode = mode;
        return true;
    }

    // Phase 2 (hook, right after the press was delivered): the real player takes over the replayed
    // state, and the wave trail is redrawn along the replayed path instead of being dragged.
    void finishLate() {
        if (m_latePending <= 0) return;
        int k = m_latePending;
        m_latePending = 0;
        PlayLayer* pl = PlayLayer::get();
        PlayerObject* po = pl ? pl->m_player1 : nullptr;
        PlayerObject* g = m_p[0][kLateSlot];
        PlayerObject* h = m_hist[m_lateIdx];
        if (!po || !g || !h || po->m_isDead) return;
        float startX = h->getPositionX();
        CCPoint startPos = h->getPosition();
        copyPhysics(po, g);
        if (m_lateMode == M_WAVE && po->m_waveTrail && po->m_waveTrail->m_pointArray) {
            auto* arr = po->m_waveTrail->m_pointArray;
            for (int i = static_cast<int>(arr->count()) - 1; i >= 0; --i) {
                auto* node = static_cast<PointNode*>(arr->objectAtIndex(i));
                if (node && node->m_point.x > startX + 0.001f) arr->removeObjectAtIndex(i);
                else break;                                // points are in order: the rest is older
            }
            for (auto const& pt : m_latePath) po->m_waveTrail->addPoint(pt);
        }
        m_histN = 0;                                   // the stored states are of the old timeline
        ++m_dbgLate;
        m_lastText = fmt::format("P1 LATE rescue: moved back {} ticks", k);
        logSaved(0, m_lateMode, -k);
        spawnFx(startPos, -k);
    }

    // ========================================================= ship / wave
    // The new hold / release state is assumed to stay for `horizon` ticks AFTER switching at `d`.
    //  - a delay (0..max) survives the whole post-switch look-ahead -> pushed safeMargin ticks
    //    into the safe window so it never lands on the lethal edge;
    //  - otherwise nothing can be saved and the input passes raw, so the controls stay free.
    Decision decideHold(PlayLayer* pl, Mode mode, int pi, bool target) {
        auto& c = cfg();
        Decision r;
        bool dual = (m_layout == Layout::DualSolo && pl->m_player1 && pl->m_player2);
        Mode m0 = dual ? detectMode(pl->m_player1) : mode;
        Mode m1 = dual ? detectMode(pl->m_player2) : mode;
        int maxDelay = c.maxDelay[mode];
        if (dual) {
            int d0 = c.maxDelay[m0], d1 = c.maxDelay[m1];
            maxDelay = (d0 > 0 && d1 > 0) ? std::min(d0, d1) : std::max(d0, d1);
        }
        if (maxDelay <= 0) return r;
        bool hasWave = (mode == M_WAVE) || (dual && (m0 == M_WAVE || m1 == M_WAVE));
        int horizon = std::max(1, hasWave ? c.waveHorizon : c.shipHorizon);

        beginSearch(pl, pi, true, dual);
        loadReal(0);

        int foundDelay = -1;
        int firstReach = -1;
        bool sawUnknown = false;                 // the prediction ended on something it cannot predict

        for (int d = 0; d <= maxDelay; ++d) {
            if (d > 0) {
                copySlot(2, 0);                  // state at the start of tick d-1 (for sub-tick)
                if (!step(0, m_game[pi])) {
                    noteDeath(0);
                    break;                       // waiting longer is deadly too
                }
                trailPush(0, 0);
                if (m_info[0].unknown) {         // cannot predict any further
                    sawUnknown = true;
                    break;
                }
            }
            copySlot(1, 0);
            size_t mark = m_trail.size();
            orbBegin(target);                    // pressing in Ship/Wave can buffer an orb ahead
            bool alive = true;
            bool ended = false;                  // the run stopped on something unpredictable
            int reach = 0;
            for (int t = 0; t < horizon; ++t) {
                if (!step(1, target)) {
                    noteDeath(1);
                    alive = false;
                    break;
                }
                trailPush(1, 0);
                if (m_info[1].unknown) {         // prediction ends here
                    ended = true;
                    sawUnknown = true;
                    break;
                }
                ++reach;
            }
            // Right now an unpredictable object ahead counts as fine (the input just passes).
            // A DELAY is only a rescue if it survives the whole post-switch look-ahead.
            if (alive && (d == 0 || !ended)) {
                foundDelay = d;
                if (m_wantTrail) m_trailOk = m_trail;
                break;
            }
            if (m_aborted) break;
            if (d == 0) firstReach = reach;
            m_trail.resize(mark);
        }

        if (foundDelay > 0 && !m_aborted) {
            r.known = true;
            r.delay = foundDelay;
            int margin = std::max(0, c.safeMargin);
            bool savedPrev = false;
            if (c.subTick && m_layout == Layout::Solo && !cbfActive()) {
                copySlot(4, 2);
                savedPrev = true;
            }
            int chosen = foundDelay;
            for (int stepM = 1; stepM <= margin && foundDelay + stepM <= maxDelay; ++stepM) {
                int d2 = foundDelay + stepM;
                if (!step(0, m_game[pi])) break;
                if (m_info[0].unknown) break;
                copySlot(1, 0);
                size_t mark = m_trail.size();
                orbBegin(target);
                bool ok2 = true;
                for (int t = 0; t < horizon; ++t) {
                    if (!step(1, target) || m_info[1].unknown) {
                        ok2 = false;
                        break;
                    }
                    trailPush(1, 0);
                }
                if (ok2 && !m_aborted) {
                    chosen = d2;
                    if (m_wantTrail) m_trailOk = m_trail;
                } else {
                    m_trail.resize(mark);
                    break;
                }
            }
            r.delay = chosen;
            if (chosen == foundDelay && savedPrev && !m_aborted) {
                copySlot(2, 4);
                r.frac = refineFrac(false, pi, foundDelay, 0, horizon, false, target);
            }
        } else if (foundDelay < 0 && !m_aborted && !sawUnknown) {
            // fatal now and no delay helps: the input may still have been safe a few ticks earlier
            r.doomed = true;
            r.late = lateSearch(pl, mode, pi, target, {}, horizon);
        }   // else: fine right now, nothing saves it, or out of budget -> raw input

        ++m_dbgSearches;
        m_lastText = fmt::format(
            "P{} {} d={} {} dies@{} used={}{}",
            pi + 1, target ? "hold" : "release", r.delay,
            foundDelay == 0 ? "SAFE" : (r.delay > 0 ? "DELAY" : (r.late > 0 ? "LATE" : "pass")),
            firstReach, m_used, m_aborted ? " ABORT" : ""
        );
        endSearch();
        return r;
    }

    // ============================================================ autoplay
    // The bot drives the input itself. It looks ahead with the ghost (hold /
    // release segments, fine near the player and coarse far away) and replans
    // every few ticks from the real state. It learns from every real death:
    // the actions it took just before dying are blamed for the places they
    // happened, so the next attempt tries something else there; actions that
    // survived for a while are forgiven again.
    int aiCell(float x) const {
        return static_cast<int>(std::floor(x / static_cast<float>(std::max(1, cfg().autoCell))));
    }

    float aiBad(int cell, bool hold) const {
        auto it = m_ai.find(cell);
        return it == m_ai.end() ? 0.f : it->second.bad[hold ? 1 : 0];
    }

    uint64_t aiKey(int s, int level, bool hold) const {
        PlayerObject* a = m_p[0][s];
        int64_t x = static_cast<int64_t>(aiCell(a->getPositionX()));
        int64_t y = static_cast<int64_t>(std::lround(a->getPositionY() * 0.7f));
        int64_t v = static_cast<int64_t>(std::lround(a->m_yVelocity * 6.f));
        uint64_t k = static_cast<uint64_t>(level & 0xF);
        k = k * 1000003ULL + static_cast<uint64_t>(x + 100000);
        k = k * 1000003ULL + static_cast<uint64_t>(y + 100000);
        k = k * 1000003ULL + static_cast<uint64_t>(v + 100000);
        k = k * 2ULL + (hold ? 1ULL : 0ULL);
        k = k * 2ULL + (a->m_isUpsideDown ? 1ULL : 0ULL);
        return k;
    }

    bool aiDfs(int s, int level, int tick, bool prevHold) {
        if (tick >= m_aiHorizon || level >= static_cast<int>(m_levels.size()) ||
            s + 1 >= kSlots) {
            m_planLen = level;
            return true;
        }
        uint64_t key = aiKey(s, level, prevHold);
        if (m_failMemo.count(key)) return false;

        int seg = m_levels[level];
        int cell = aiCell(m_p[0][s]->getPositionX());
        // default first: click modes do nothing, ship / wave keep their input. The other
        // action is only tried when the default dies (or when a branch forces it).
        bool def = m_aiContinuous ? prevHold : false;
        bool order[2] = {def, !def};
        int count = 2;
        if (level < m_forceLen) {
            order[0] = m_force[level];
            count = 1;
        }
        float prune = static_cast<float>(cfg().autoPrune);

        for (int k = 0; k < count; ++k) {
            bool h = order[k];
            float bh = aiBad(cell, h);
            float bo = aiBad(cell, !h);
            if (bh >= prune && bo < bh) continue;         // learned: this action killed us here
            m_planHold[level] = h;
            m_planSeg[level] = seg;
            copySlot(s + 1, s);
            size_t mark = m_trail.size();
            bool ok = true;
            int stepped = 0;
            for (int t = 0; t < seg && tick + t < m_aiHorizon; ++t) {
                if (!step(s + 1, h)) {
                    noteDeath(s + 1);
                    ok = false;
                    break;
                }
                trailPush(s + 1, 0);
                ++stepped;
                if (m_info[s + 1].unknown) break;
            }
            int reached = tick + stepped;
            if (reached > m_bestReach) {
                m_bestReach = reached;
                m_bestLen = level + 1;
                for (int i = 0; i <= level; ++i) {
                    m_bestHold[i] = m_planHold[i];
                    m_bestSeg[i] = m_planSeg[i];
                }
            }
            if (ok && m_info[s + 1].unknown) {
                m_planLen = level + 1;                    // cannot predict past this: good enough
                return true;
            }
            if (ok && aiDfs(s + 1, level + 1, tick + seg, h)) return true;
            m_trail.resize(mark);
            if (m_aborted) return false;
        }
        if (level >= m_forceLen) m_failMemo.insert(key);   // a forced level only tried one action
        return false;
    }

    void clearAutoPlan() {
        m_aiPlan.clear();
        m_aiPlanPos = 0;
        m_aiPlanTotal = 0;
        m_aiNext.clear();
        m_aiNextTotal = 0;
        m_aiNextReady = false;
        m_aiNextTried = false;
    }

    // hold state of a route at tick `idx` (past its end: the last one)
    static bool aiHoldAt(std::vector<AiSeg> const& plan, int idx, bool dflt) {
        if (plan.empty()) return dflt;
        int acc = 0;
        bool h = plan.back().hold;
        for (auto const& st : plan) {
            if (idx < acc + st.seg) {
                h = st.hold;
                break;
            }
            acc += st.seg;
        }
        return h;
    }

    // Does the plan survive when it is shifted by `shift` ticks (late > 0 / early < 0)?
    bool aiSurvives(std::vector<AiSeg> const& plan, int shift, bool prev) {
        copySlot(1, 0);
        for (int t = 0; t < m_aiHorizon; ++t) {
            int idx = t - shift;
            bool h = prev;
            if (idx >= 0) {
                int acc = 0;
                h = plan.back().hold;
                for (auto const& st : plan) {
                    if (idx < acc + st.seg) {
                        h = st.hold;
                        break;
                    }
                    acc += st.seg;
                }
            }
            if (!step(1, h)) return false;
            if (m_info[1].unknown) break;
        }
        return true;
    }

    // Route search. A route (pattern) is a hold / release sequence over the look-ahead.
    //  1. Route 1: "do nothing" until something ahead would kill, then act; go on until the
    //     look-ahead ends. Dead end (even acting at every step) = that route is over.
    //  2. Every spot where doing nothing was fine but acting was possible becomes a branch:
    //     go back to it, act there, and continue the same way from that point (route 2, 3, ...).
    //  3. Of the complete routes the most stable one is followed: the one that still lives when
    //     all its timings are shifted by -2..+2 ticks, then the one with fewer switches.
    // m_aiPlan = the chosen route. true = it survives the whole look-ahead.
    // lead > 0: build the NEXT route (m_aiNext) instead: it starts where the current route is `lead`
    // ticks from now (that is, the old route is followed for `lead` ticks first).
    bool planAuto(PlayLayer* pl, int lead = 0) {
        auto& c = cfg();
        bool toNext = lead > 0;
        int s0 = std::max(1, c.autoStride);
        m_aiHorizon = c.autoHorizon;
        m_levels.clear();
        // Decision steps: fine near the player (stride), then growing so that the few levels the
        // ghost slots allow still cover the whole look-ahead (it can be thousands of ticks).
        int total = 0;
        int fine = std::min(6, kSlots - 2);
        for (int i = 0; i < fine && total < m_aiHorizon; ++i) {
            m_levels.push_back(s0);
            total += s0;
        }
        int rest = kSlots - 2 - fine;
        if (total < m_aiHorizon && rest > 0) {
            float growth = 2.f;                       // doubling is enough for short look-aheads
            for (;;) {
                float seg = static_cast<float>(s0), sum = 0.f;
                for (int k = 0; k < rest; ++k) {
                    seg *= growth;
                    sum += seg;
                }
                if (static_cast<float>(total) + sum >= static_cast<float>(m_aiHorizon) || growth > 60.f) break;
                growth += 0.25f;
            }
            float seg = static_cast<float>(s0);
            for (int k = 0; k < rest && total < m_aiHorizon; ++k) {
                seg *= growth;
                int sg = std::max(1, static_cast<int>(seg));
                m_levels.push_back(sg);
                total += sg;
            }
        }

        beginSearch(pl, 0, false, m_layout == Layout::DualSolo);   // autoplay: one input, both players
        orbBegin(true);
        loadReal(0);
        m_failMemo.clear();                                        // dead states stay dead for every route
        m_aiContinuous = isContinuous(detectMode(pl->m_player1));

        int paths = std::max(1, c.autoPaths);
        bool prev = m_game[0];
        if (toNext) {
            // follow the current route for `lead` ticks: the new routes start from there
            bool h = prev;
            for (int t = 0; t < lead; ++t) {
                h = aiHoldAt(m_aiPlan, m_aiPlanPos + t, m_aiHold);
                if (!step(0, h) || m_info[0].unknown) {       // dies / cannot be predicted: no next route
                    endSearch();
                    return false;
                }
            }
            prev = h;
        }
        bool genDefault = m_aiContinuous ? prev : false;
        (void)genDefault;

        struct Route {
            std::vector<AiSeg> plan;
            std::vector<bool> holds;
            std::vector<TrailPt> trail;
            int len = 0;
            int toggles = 0;
            int tol = 0;
        };
        std::vector<Route> routes;
        std::vector<AiSeg> partial;
        int partialReach = -1;

        std::deque<std::vector<bool>> seeds;
        seeds.push_back({});                                       // no forced action = route 1
        m_budget = m_used + c.autoBudget;
        int attempts = 0;
        int maxAttempts = paths * 8;

        while (!seeds.empty() && static_cast<int>(routes.size()) < paths && attempts < maxAttempts) {
            std::vector<bool> prefix = std::move(seeds.front());
            seeds.pop_front();
            if (m_aborted || m_used >= m_budget) break;
            ++attempts;

            m_forceLen = static_cast<int>(prefix.size());
            for (int i = 0; i < m_forceLen; ++i) m_force[i] = prefix[i];
            m_bestReach = -1;
            m_bestLen = 0;
            m_planLen = 0;
            m_trail.clear();
            bool ok = aiDfs(0, 0, 0, prev);

            if (ok && m_planLen > 0) {
                std::vector<bool> holds(m_planHold, m_planHold + m_planLen);
                bool dup = false;
                for (auto const& r : routes) dup = dup || r.holds == holds;
                if (!dup) {
                    Route r;
                    r.holds = holds;
                    bool p = prev;
                    for (int i = 0; i < m_planLen; ++i) {
                        r.plan.push_back({m_planHold[i], m_planSeg[i]});
                        r.len += m_planSeg[i];
                        if (m_planHold[i] != p) ++r.toggles;
                        p = m_planHold[i];
                    }
                    if (m_wantTrail) r.trail = m_trail;
                    routes.push_back(std::move(r));
                }
                // branches: every spot where the default was fine but acting was possible too
                bool p = prev;
                for (int i = 0; i < m_planLen; ++i) {
                    bool def = m_aiContinuous ? p : false;
                    if (i >= m_forceLen && m_planHold[i] == def) {
                        std::vector<bool> seed(m_planHold, m_planHold + i);
                        seed.push_back(!def);
                        seeds.push_back(std::move(seed));
                    }
                    p = m_planHold[i];
                }
            } else if (m_bestReach > partialReach && m_bestLen > 0) {
                partialReach = m_bestReach;
                partial.clear();
                for (int i = 0; i < m_bestLen; ++i) partial.push_back({m_bestHold[i], m_bestSeg[i]});
            }
        }
        m_forceLen = 0;

        // stability: survive the same route with every timing shifted
        int best = -1;
        long bestScore = -1;
        if (!routes.empty()) {
            m_aborted = false;
            // only the longest / simplest few are tested with shifted timings (with hundreds of routes
            // and a long look-ahead that would cost far more than the search itself)
            constexpr size_t kStab = 16;
            std::vector<size_t> order;
            for (size_t i = 0; i < routes.size(); ++i) order.push_back(i);
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                if (routes[a].len != routes[b].len) return routes[a].len > routes[b].len;
                return routes[a].toggles < routes[b].toggles;
            });
            size_t testN = std::min(routes.size(), kStab);
            m_budget = m_used + (static_cast<int>(testN) * 4 * m_aiHorizon + 64) * (m_two ? 2 : 1);
            for (size_t k = 0; k < order.size(); ++k) {
                size_t i = order[k];
                static constexpr int shifts[4] = {-2, -1, 1, 2};
                if (k < testN) {
                    for (int sh : shifts) {
                        if (aiSurvives(routes[i].plan, sh, prev)) ++routes[i].tol;
                    }
                }
                long score = 1000000L + routes[i].tol * 10000L - routes[i].toggles * 100L + routes[i].len;
                if (score > bestScore) {
                    bestScore = score;
                    best = static_cast<int>(i);
                }
            }
        }

        bool ok = best >= 0;
        std::vector<AiSeg> outPlan;
        int outTotal = 1;
        bool useful = true;
        if (ok) {
            outPlan = routes[best].plan;
            outTotal = std::max(1, routes[best].len);
            if (m_wantTrail && !toNext) m_trailOk = routes[best].trail;
        } else if (!partial.empty()) {
            outPlan = partial;
            outTotal = std::max(1, partialReach);
        } else {
            outPlan.assign(1, AiSeg{prev, 1});
            useful = false;                                      // nothing was found
        }
        if (toNext) {
            m_aiNextReady = useful;
            if (useful) {
                m_aiNext = std::move(outPlan);
                m_aiNextTotal = outTotal;
            }
        } else {
            m_aiPlan = std::move(outPlan);
            m_aiPlanTotal = outTotal;
            m_aiPlanPos = 0;
        }
        endSearch();
        return ok;
    }

    void autoTick(PlayLayer* pl) {
        auto& c = cfg();
        PlayerObject* p1 = pl->m_player1;
        if (!p1) return;
        ++m_aiTick;

        // forgive what survived long enough (once per spot)
        while (!m_aiTrail.empty() && m_aiTick - m_aiTrail.front().tick > c.autoForget) {
            AiStep st = m_aiTrail.front();
            m_aiTrail.pop_front();
            if (st.cell == m_aiLastForgiven.first && st.hold == m_aiLastForgiven.second) continue;
            m_aiLastForgiven = {st.cell, st.hold};
            auto it = m_ai.find(st.cell);
            if (it != m_ai.end()) it->second.bad[st.hold ? 1 : 0] *= 0.6f;
        }

        // 80% played: build the next route at once (nothing waits). It starts from where this route
        // is at 90%, so it is ready when the 90% mark is reached.
        if (!m_aiPlan.empty() && !m_aiNextReady && !m_aiNextTried && m_aiPlanPos * 10 >= m_aiPlanTotal * 8) {
            m_aiNextTried = true;
            int p90 = (m_aiPlanTotal * 9 + 9) / 10;        // first position that counts as 90% played
            int lead = p90 - m_aiPlanPos;
            if (lead > 0) {
                bool ok = planAuto(pl, lead);
                ++m_dbgSearches;
                m_lastText = fmt::format("AI next {} from +{}t len={}t used={}", ok ? "plan" : "best",
                                         lead, m_aiNextTotal, m_used);
            }
        }
        // 90% played: follow the new route from here (it was built for exactly this point)
        if (m_aiPlan.empty() || m_aiPlanPos * 10 >= m_aiPlanTotal * 9) {
            if (m_aiNextReady && !m_aiPlan.empty()) {
                m_aiPlan = std::move(m_aiNext);
                m_aiPlanTotal = m_aiNextTotal;
                m_aiPlanPos = 0;
                m_aiNext.clear();
            } else {                                        // no next route: plan from the real state
                bool ok = planAuto(pl);
                ++m_dbgSearches;
                m_lastText = fmt::format("AI {} paths={} len={}t used={}", ok ? "plan" : "best",
                                         c.autoPaths, m_aiPlanTotal, m_used);
            }
            m_aiNextReady = false;
            m_aiNextTried = false;
        }
        bool h = m_aiHold;
        int acc = 0;
        for (auto const& st : m_aiPlan) {
            h = st.hold;
            if (m_aiPlanPos < acc + st.seg) break;
            acc += st.seg;
        }
        m_aiHold = h;
        ++m_aiPlanPos;
        if (m_game[0] != m_aiHold) fire(pl, 0, m_aiHold);

        m_aiTrail.push_back({aiCell(p1->getPositionX()), m_aiHold, m_aiTick});
        if (m_aiTrail.size() > 1200) m_aiTrail.pop_front();
    }

    void aiOnDeath() {
        auto& c = cfg();
        ++m_aiDeaths;
        if (c.autoLearn) {
            // blame the actions taken shortly before dying, once per spot,
            // the closer to the death the more
            std::unordered_map<int, float> blame[2];
            for (auto const& st : m_aiTrail) {
                int age = m_aiTick - st.tick;
                if (age > c.autoWindow) continue;
                float w = 1.f + 2.f * (1.f - static_cast<float>(age) /
                                                  static_cast<float>(std::max(1, c.autoWindow)));
                float& slot = blame[st.hold ? 1 : 0][st.cell];
                slot = std::max(slot, w);
            }
            for (int a = 0; a < 2; ++a) {
                for (auto const& kv : blame[a]) m_ai[kv.first].bad[a] += kv.second;
            }
        }
        m_aiTrail.clear();
        m_aiTick = 0;
        m_aiHoldLeft = 0;
        clearAutoPlan();
        m_aiLastForgiven = {std::numeric_limits<int>::min(), false};
    }

    // ================================================================ trajectory
    // Live preview: where player 1 ends up if the button is HELD from now on and
    // where it ends up if it is RELEASED. The path that matches the current input is
    // drawn bright, the other one dim. Between two refreshes the paths are trimmed
    // (one point per tick) so the line never lags behind the player; a change of the
    // input refreshes at once.
    std::vector<TrailPt> simPreview(PlayLayer* pl, bool assumeHold, bool cur, Mode mode) {
        auto& c = cfg();
        beginSearch(pl, 0, false, m_layout == Layout::DualSolo);
        m_budget = (c.previewLen + 8) * (m_two ? 2 : 1);
        orbBegin(assumeHold);
        loadReal(0);
        // hold: ship / wave (and anything already held) keep holding; click modes
        // that are not held yet get a tap, the way you would really press them
        int holdTicks = (isContinuous(mode) || cur) ? std::numeric_limits<int>::max() : c.tapTicks;
        std::vector<TrailPt> path;
        path.push_back({pos(0), 0});
        bool died = false;
        for (int t = 0; t < c.previewLen; ++t) {
            bool h = assumeHold && t < holdTicks;
            if (!step(0, h)) {
                died = !m_aborted;
                break;
            }
            path.push_back({pos(0), 0});
            if (m_info[0].unknown) break;                    // cannot predict past this
        }
        if (died) path.push_back({pos(0), 3});
        endSearch(false, false);
        return path;
    }

    // returns true when the overlay has to be redrawn
    bool previewPath(PlayLayer* pl) {
        auto& c = cfg();
        if (!c.showTrajectory) {
            if (m_trailHold.empty() && m_trailRelease.empty()) return false;
            m_trailHold.clear();
            m_trailRelease.clear();
            m_liveValid = false;
            return true;
        }
        if (!m_ready || m_load > 4.f) return false;         // not ready / busy (spam)
        PlayerObject* p1 = pl->m_player1;
        if (!p1 || p1->m_isDead) return false;

        bool cur = m_game[0];
        bool refresh = ++m_previewCounter >= std::max(1, c.previewEvery) ||
                       cur != m_prevHold || !m_liveValid;
        if (!refresh) {
            ++m_liveSkip;
            return true;
        }
        m_previewCounter = 0;
        m_liveSkip = 0;
        m_prevHold = cur;
        m_liveValid = true;

        Mode mode = detectMode(p1);
        bool both = c.previewBoth;
        if (both || cur) m_trailHold = simPreview(pl, true, cur, mode);
        else m_trailHold.clear();
        if (both || !cur) m_trailRelease = simPreview(pl, false, cur, mode);
        else m_trailRelease.clear();
        return true;
    }

    // Show path: looks for a hold / release route that survives the look-ahead
    // (same planner as autoplay, but it only draws the result: nothing is pressed,
    // delayed or learned). Pink = a route that survives, light red = best effort.
    bool pathScan(PlayLayer* pl) {
        auto& c = cfg();
        if (!c.showPath) {
            if (m_pathPts.empty()) return false;
            m_pathPts.clear();
            m_pathValid = false;
            return true;
        }
        if (!m_ready || m_load > 4.f) return false;
        PlayerObject* p1 = pl->m_player1;
        if (!p1 || p1->m_isDead) return false;

        bool cur = m_game[0];
        bool refresh = ++m_pathCounter >= std::max(1, c.pathEvery) ||
                       cur != m_pathPrevHold || !m_pathValid;
        if (!refresh) {
            ++m_pathSkip;
            return !m_pathPts.empty();
        }
        m_pathCounter = 0;
        m_pathSkip = 0;
        m_pathPrevHold = cur;
        m_pathValid = true;

        int s0 = std::max(1, c.autoStride);
        m_aiHorizon = c.pathHorizon;
        m_levels.clear();
        int total = 0;
        for (int i = 0; total < m_aiHorizon && i < kSlots - 2; ++i) {
            int seg = i < 6 ? s0 : (s0 << std::min(i - 5, 5));
            m_levels.push_back(seg);
            total += seg;
        }

        beginSearch(pl, 0, false, m_layout == Layout::DualSolo);
        m_budget = c.pathBudget;
        m_forceLen = 0;
        m_aiContinuous = isContinuous(detectMode(p1));
        m_wantTrail = true;                                  // aiDfs records the route in m_trail
        orbBegin(true);
        loadReal(0);
        m_failMemo.clear();
        m_bestReach = -1;
        m_bestLen = 0;

        std::vector<TrailPt> path;
        path.push_back({pos(0), 0});
        bool ok = aiDfs(0, 0, 0, cur);
        if (ok) {
            path.insert(path.end(), m_trail.begin(), m_trail.end());
        } else if (m_bestLen > 0) {
            // nothing survives the whole look-ahead: replay the route that got furthest
            m_aborted = false;
            m_budget = m_used + (m_aiHorizon + 16) * (m_two ? 2 : 1);
            loadReal(0);
            bool stop = false;
            bool died = false;
            for (int i = 0; i < m_bestLen && !stop; ++i) {
                for (int t = 0; t < m_bestSeg[i]; ++t) {
                    if (!step(0, m_bestHold[i])) {
                        died = !m_aborted;
                        stop = true;
                        break;
                    }
                    path.push_back({pos(0), 0});
                    if (m_info[0].unknown) {
                        stop = true;
                        break;
                    }
                }
            }
            if (died) path.push_back({pos(0), 3});
        }
        m_pathOk = ok;
        m_pathPts = std::move(path);
        endSearch(false, false);
        return true;
    }

    void overlayTick(PlayLayer* pl) {
        if (!m_draw) return;
        bool a = previewPath(pl);
        bool b = pathScan(pl);
        if (a || b) drawResult();
    }

    void drawResult() {
        auto& c = cfg();
        if (!m_draw) return;
        m_draw->clear();
        float alpha = static_cast<float>(c.trajOpacity) / 100.f;
        float width = static_cast<float>(c.lineWidth) / 10.f;
        auto line = [&](std::vector<TrailPt> const& t, ccColor4F col, float w, size_t skip) {
            col.a *= alpha;
            for (size_t i = std::max<size_t>(1, skip + 1); i < t.size(); ++i) {
                m_draw->drawSegment(t[i - 1].p, t[i].p, w, col);
            }
            for (size_t i = skip; i < t.size(); ++i) {
                auto const& pt = t[i];
                if (pt.tag == 1) m_draw->drawDot(pt.p, 3.f, {1.f, .85f, .1f, alpha});
                else if (pt.tag == 2) m_draw->drawDot(pt.p, 3.5f, {.85f, .3f, 1.f, alpha});
                else if (pt.tag == 3) m_draw->drawDot(pt.p, 4.f, {1.f, .1f, .1f, alpha});
            }
        };

        // scanned feasible route (display only), underneath everything else
        if (c.showPath && !m_pathPts.empty()) {
            ccColor4F col = m_pathOk ? ccColor4F{.95f, .4f, 1.f, .95f} : ccColor4F{1.f, .5f, .5f, .8f};
            line(m_pathPts, col, width * 1.25f, static_cast<size_t>(m_pathSkip));
        }
        if (!c.showTrajectory) return;

        if (m_keepTicks > 0) {
            line(m_keepFail, {1.f, .3f, .3f, .7f}, width, 0);
            line(m_keepOk, {.2f, 1.f, .4f, 1.f}, width, 0);
        }
        // hold = cyan, release = orange; the one matching the current input is brighter
        ccColor4F holdCol = {.35f, .85f, 1.f, .9f};
        ccColor4F relCol = {1.f, .65f, .2f, .9f};
        size_t skip = static_cast<size_t>(m_liveSkip);
        ccColor4F holdDim = holdCol;
        ccColor4F relDim = relCol;
        holdDim.a *= .5f;
        relDim.a *= .5f;
        if (m_prevHold) {
            line(m_trailRelease, relDim, width * .7f, skip);
            line(m_trailHold, holdCol, width, skip);
        } else {
            line(m_trailHold, holdDim, width * .7f, skip);
            line(m_trailRelease, relCol, width, skip);
        }
    }
};

inline Engine g_engine;

} // namespace buf
