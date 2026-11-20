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

    struct SlotInfo {
        bool holding[2] = {false, false};
        bool grounded = false;
        int groundTicks = 0;
        bool unknown = false;          // touched something we cannot predict
        int ticks = 0;                 // ticks simulated in this slot
        std::vector<int> touched;      // portals already applied (uniqueID*2 + player)
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
    struct RingEntry { RingObject* obj; float x; };
    struct OrbState {
        std::vector<RingEntry> rings;      // every orb of the level, sorted by x (see buildRings)
        bool done[2] = {false, false};     // an orb already reacted to this step's click
        bool used = false;                 // the run being simulated activated an orb (see orbBegin)
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
    struct Pend { long tick; bool push; float frac = 1.f; int saved = 0; };   // saved = ticks it was held back
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
    double m_clock = 0;                // game time = sum of the dt of every real physics step
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
        buildRings(pl);
        m_lastTickFrame = -1000;
        m_lastP1Frame = -1000;
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
        clearOverlay();
        resetInput();
    }

    // ================================================================== orbs
    // The ghost must not depend on the game handing it the orbs: every orb of the level is
    // collected once, so a click is tested against the orbs around the ghost by geometry.
    void buildRings(PlayLayer* pl) {
        m_orb.rings.clear();
        if (!pl || !pl->m_objects) return;
        for (auto* obj : geode::cocos::CCArrayExt<GameObject*>(pl->m_objects)) {
            if (auto ring = geode::cast::typeinfo_cast<RingObject*>(obj)) {
                m_orb.rings.push_back({ring, ring->getPositionX()});
            }
        }
        std::sort(m_orb.rings.begin(), m_orb.rings.end(), [](RingEntry const& a, RingEntry const& b) { return a.x < b.x; });
    }

    // Call before every simulated run whose orb use matters; read the answer with orbUsed() right
    // after that run. (This replaces comparing a global hit counter before / after, which also
    // counted the runs of other candidates.)
    void orbBegin() { m_orb.used = false; }
    bool orbUsed() const { return m_orb.used; }

    // May the orb of this step react to the click? Only on the very tick the predicted click is
    // pressed (holding through an orb never activates it), only for the ghost, never in a rescue.
    bool orbAllowed(int who) const { return cfg().orbDetect && m_orb.enabled && m_stepEdge[who]; }

    // One orb reacted (by geometry or through the game's own playerTouchedRing).
    void orbNoteHit(int who) {
        ++m_orb.seen;
        ++m_orb.hit;
        m_orb.done[who] = true;
        m_orb.used = true;
    }

    // Ghost, click tick: the orb that overlaps the player reacts (once per click), exactly like
    // the game's own playerTouchedRing would, but found by geometry.
    void simRingsByGeometry(PlayerObject* player, int who, CCRect const& pr) {
        if (m_orb.done[who] || m_orb.rings.empty()) return;
        float px = pr.getMidX();
        auto it = std::lower_bound(m_orb.rings.begin(), m_orb.rings.end(), px - 90.f,
                                   [](RingEntry const& r, float v) { return r.x < v; });
        for (; it != m_orb.rings.end() && it->x <= px + 90.f; ++it) {
            RingObject* ring = it->obj;
            if (player->m_touchedRings.find(ring->m_uniqueID) != player->m_touchedRings.end()) continue;
            CCRect rr = ring->getObjectRect();
            float r = std::min(rr.size.width, rr.size.height) * 0.5f;
            float cx = rr.getMidX(), cy = rr.getMidY();
            float dx = cx - std::clamp(cx, pr.getMinX(), pr.getMaxX());
            float dy = cy - std::clamp(cy, pr.getMinY(), pr.getMaxY());
            if (dx * dx + dy * dy > r * r) continue;       // orbs are round
            orbNoteHit(who);
            player->m_touchedRings.insert(ring->m_uniqueID);   // the game's own path must not fire it again
            player->ringJump(ring, true);
            break;
        }
    }

    // The game's own path (playerTouchedRing hook): may this orb react?
    bool canSimRing(PlayerObject* player, EffectGameObject* object) {
        auto ring = geode::cast::typeinfo_cast<RingObject*>(object);
        if (!m_simulating || !isSimPlayer(player) || !ring) return false;
        ++m_orb.seen;
        if (player->m_touchedRings.find(ring->m_uniqueID) != player->m_touchedRings.end()) return false;
        int who = player == m_stepObj[0] ? 0 : 1;
        if (!orbAllowed(who)) return false;
        orbNoteHit(who);
        return true;
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
        m_orb.rings.clear();
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
        m_histN = 0;
        clearFx();
        for (int i = 0; i < 2; ++i) {
            m_pend[i].clear();
            m_supp[i] = false;
            m_game[i] = false;
        }
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

    // The collision hook hands us the objects near a ghost player. Portals the
    // game would apply with side effects (or not at all) are applied here, by
    // geometry, with no visuals and without touching any real object state.
    void simTouchObjects(PlayerObject* player, gd::vector<GameObject*>* vec, int count) {
        PlayLayer* pl = PlayLayer::get();
        if (!m_simulating || !pl || !isSimPlayer(player) || !vec) return;
        int who = player == m_stepObj[1] ? 1 : 0;
        CCRect pr = player->getObjectRect();
        if (orbAllowed(who)) simRingsByGeometry(player, who, pr);
        for (int i = 0; i < count; ++i) {
            GameObject* obj = vec->at(i);
            if (!obj) continue;
            TouchKind kind = classify(obj);
            if (kind == TouchKind::None) continue;
            CCRect orect = obj->getObjectRect();
            if (!orect.intersectsRect(pr)) continue;
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
    // releaseButton tell which player the game gave the input to.
    void beginHandle() {
        m_inHandle = true;
        m_recvP = nullptr;
        m_recvCount = 0;
    }
    void noteReceiver(PlayerObject* p) {
        if (!m_inHandle || m_simulating || !p) return;
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
    void endLate() { finishLate(); }
    void setInUpdate(bool v) { m_inUpdate = v; }

    // Real game: a move trigger just fired. For its duration the objects it moves are
    // not where the ghost sees them, so the prediction must not be trusted near them.
    void onRealMoveTrigger(float duration) {
        if (!cfg().moveDetect) return;
        float tps = effectiveTps();
        if (tps <= 0.f) tps = 240.f;
        long ticks = static_cast<long>(std::ceil(std::max(0.f, duration) * tps)) + 4;
        m_moveUntil = std::max(m_moveUntil, m_now + ticks);
    }

    bool moveActive() const { return cfg().moveDetect && m_now < m_moveUntil; }

    // Ghost: an object that may be moving and cannot be predicted: stop trusting the sim
    void flagMovingObject() { m_hitUnknown = true; }

    // Moving objects. Their speed is MEASURED in the real game against game time (the sum
    // of the dt of the physics steps), not against frames or ticks, so Click Between
    // Frames (extra sub-steps with odd dt, objects only move once per frame) cannot skew it.
    // The ghost then shifts those objects along their measured speed for each simulated tick.
    void addClock(PlayerObject* p, float dt) {
        PlayLayer* pl = PlayLayer::get();
        if (pl && p == pl->m_player1 && dt > 0.f && dt < 5.f) m_clock += dt;
    }

    float moveFresh() const { return 40.f * std::max(1e-4f, stepDt()); }

    // real game, every collision pass of player 1
    void observeObjects(gd::vector<GameObject*>* vec, int count) {
        if (!cfg().moveDetect || !vec) return;
        if (m_moveTrk.size() > 4000) m_moveTrk.clear();
        double fresh = moveFresh();
        for (int i = 0; i < count; ++i) {
            GameObject* o = vec->at(i);
            if (!o || !o->m_groups || o->m_groups->at(0) == 0) continue;     // not movable
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

    // ghost: where is this object during the tick that is being simulated?
    MoveState simMove(GameObject* o, CCPoint& offset) {
        auto it = m_moveTrk.find(o->m_uniqueID);
        if (it == m_moveTrk.end()) return moveActive() ? MoveState::Unknown : MoveState::Static;
        auto& t = it->second;
        if (!t.moving || m_clock - t.changeClock > moveFresh()) return MoveState::Static;
        if (!t.hasVel) return MoveState::Unknown;
        float elapsed = static_cast<float>(m_info[m_curSlot].ticks + 1) * m_stepDtNow;
        offset = t.vel * elapsed;
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
        if (m_layout == Layout::TwoPlayer) {
            for (int sd = 0; sd < 2; ++sd) {
                if (sideOwner(sd == 1) == pi) return m_raw[sd];
            }
        }
        known = false;                                  // dual in a 1-player level: not sure how
        return false;                                   // the game spreads a tap, so no guessing
    }

    // Safety net for the input bookkeeping. It only ever RELEASES (a press made up from stale
    // state would be worse than the bug): if the game was told "hold", nothing is being held
    // back, and no finger is on that player's side for 3 ticks, the hold is stuck: release it.
    // After coming back to solo (dual ended) the game gets one release regardless of what the
    // mod believes, because the game itself may have kept the hold.
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
            if (p1 && !p1->m_isDead && m_pend[0].empty() && known && !down) fire(pl, 0, false);
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
    // isP1 may be rewritten: in solo both screen sides are player 1.
    // Returns false when the event was swallowed (it may still have been
    // delivered through fire(), see below).
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

        // "both sides = player 1" only in solo outside 2-player levels: there the
        // game already routes left / right to its own players
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
        }
        // Solo: one player. Dual / 2-player: every event belongs to the player it
        // came from, and every player is its own buffer (own mode, own delay).
        int pi = (lay != Layout::Solo && !origP1) ? 1 : 0;
        // 2-player mode: the game decides which side drives which player (flip 2-player
        // controls). Never guessed: the real input shows it, and one known side gives the other.
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
        m_phys[pi] = push;
        PlayerObject* po = pi == 0 ? layer->m_player1 : layer->m_player2;

        // fail-open: if the physics tick hook is not running, do not touch anything
        int frames = static_cast<int>(CCDirector::sharedDirector()->getTotalFrames());
        bool tickAlive = frames - m_lastTickFrame <= c.failOpen;

        bool active = c.enabled && m_ready && tickAlive && po && !po->m_isDead &&
                      c.maxDelay[detectMode(po)] > 0 &&
                      !(lay == Layout::TwoPlayer && !c.twoPlayerMode) &&
                      !(lay == Layout::DualSolo && !c.dualSupport);
        if (!active) {
            m_pend[pi].clear();
            m_supp[pi] = false;
            if (remap) {
                isP1 = true;                         // both screen sides are player 1
                // some builds send the same press as a "player 2" copy: drop duplicates
                if (!origP1 && push == m_game[pi]) return false;
            }
            m_game[pi] = push;
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

        Mode mode = detectMode(po);
        m_otherOverride = (m_layout == Layout::DualSolo) ? (push ? 1 : 0) : -1;
        Decision d;
        if (isContinuous(mode)) d = decideHold(pl, mode, pi, push);
        else if (push) d = decideClick(pl, mode, pi);
        m_otherOverride = -1;
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

        {   // measure the real wave slope (degrees) for the HUD
            float px = p1->getPositionX();
            float py = p1->getPositionY();
            float dx = px - m_prevX;
            float dy = py - m_prevY;
            if (p1->m_isDart && dx > 0.0001f && dx < 50.f && dy != 0.f && std::fabs(dy) < 80.f) {
                m_realAngle = std::atan2(std::fabs(dy), dx) * 57.29578f;
            }
            m_prevX = px;
            m_prevY = py;
        }
        m_load *= 0.97f;
        if (m_noclipTicks > 0) --m_noclipTicks;

        m_layout = layoutOf(pl);
        if (m_layout != m_prevLayout) {
            // dual / 2-player started or ended: nothing held back may survive the change,
            // and the second player's input state belongs to the old layout
            for (int i = 0; i < 2; ++i) {
                if (m_owed[i] && !m_pend[i].empty() && (i == 0 || m_layout != Layout::Solo)) {
                    // let the game have what it was owed before the switch
                    m_pend[i].clear();
                    m_supp[i] = false;
                    matchPhysical(pl, i);
                }
                m_pend[i].clear();
                m_supp[i] = false;
                m_owed[i] = false;
            }
            if (m_layout == Layout::Solo) {
                m_phys[1] = false;
                m_game[1] = false;
            }
            if (m_layout == Layout::Solo) m_sweep = 6;     // back to solo: check nothing stays held
            m_prevLayout = m_layout;
        }
        auto& cf = cfg();
        if (m_keepTicks > 0 && --m_keepTicks == 0) {
            m_keepOk.clear();
            m_keepFail.clear();
            drawResult();
        }

        histPush(pl);

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
            if (m_layout != Layout::Solo) {
                m_pend[1].clear();
                m_supp[1] = false;
                matchPhysical(pl, 1);
            }
        }

        int n = m_layout == Layout::Solo ? 1 : 2;
        m_justDecided = false;
        for (int pi = 0; pi < n; ++pi) tickPlayer(pl, pi);
        // The safety net also runs for a few ticks after dual ends while the mod is OFF: the game
        // itself may keep a hold across the close portal (a wave stuck holding), and with the
        // mod off nothing else would ever release it.
        if (m_ready && (cf.enabled || m_sweep > 0)) reconcile(pl, n);
        if (!m_justDecided) overlayTick(pl);
    }

private:
    // Fires the held-back events once their tick has come (one per tick).
    void tickPlayer(PlayLayer* pl, int pi) {
        auto& c = cfg();
        PlayerObject* po = pi == 0 ? pl->m_player1 : pl->m_player2;
        if (!po) return;
        Mode mode = detectMode(po);
        bool active = c.enabled && m_ready && !po->m_isDead && c.maxDelay[mode] > 0 &&
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
            case GameObjectType::Breakable:            // D block (always on)
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
        constexpr float r = 13.f;
        constexpr int n = 28;
        for (auto const& c : pts) {
            m_fxDraw->drawDot(c, r, {0.f, 0.f, 0.f, 0.6f});
            CCPoint prev = ccp(c.x + r, c.y);
            for (int i = 1; i <= n; ++i) {
                float ang = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
                CCPoint cur = ccp(c.x + r * std::cos(ang), c.y + r * std::sin(ang));
                m_fxDraw->drawSegment(prev, cur, 1.2f, {1.f, 1.f, 1.f, 0.95f});
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
            label->setScale(0.45f);
            label->setAnchorPoint({0.f, 0.5f});
            label->setPosition(ccp(at.x + 17.f, at.y));
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

    void syncPlayer(PlayerObject* sim, PlayerObject* src) {
        sim->setVisible(false);
        sim->copyAttributes(src);
        sim->setPosition(src->getPosition());
        sim->setScale(src->getScale());
        sim->setContentSize(src->getContentSize());
        sim->m_width = src->m_width;
        sim->m_height = src->m_height;
        sim->m_gravityMod = src->m_gravityMod;
        sim->m_isOnGround = src->m_isOnGround;
        sim->m_isUpsideDown = src->m_isUpsideDown;
        sim->m_vehicleSize = src->m_vehicleSize;
        sim->m_playerSpeed = src->m_playerSpeed;
        sim->m_yVelocity = src->m_yVelocity;
        sim->m_gravity = src->m_gravity;
        sim->m_speedMultiplier = src->m_speedMultiplier;
        sim->m_touchedRings = src->m_touchedRings;
        if (sim->m_touchingRings) sim->m_touchingRings->removeAllObjects();
    }

    void forceHoldObj(int s, int who, bool hold) {
        m_info[s].holding[who] = hold;
        PlayerObject* p = m_p[who][s];
        if (!p) return;
        if (hold) p->pushButton(PlayerButton::Jump);
        else p->releaseButton(PlayerButton::Jump);
    }

    // sets the hold state of the TARGET player
    void setHold(int s, bool hold) {
        if (m_info[s].holding[m_tp] != hold) forceHoldObj(s, m_tp, hold);
        if (m_mirror && m_info[s].holding[1] != hold) forceHoldObj(s, 1, hold);
    }

    // hold state the ghost starts with for a player (the target's own state is the
    // one the game has; the other player may be assumed to mirror the target)
    bool holdFor(int who) const {
        if (m_mirror) return m_game[0];
        if (who != m_tp && m_otherOverride >= 0) return m_otherOverride == 1;
        return m_game[who];
    }

    void loadReal(int s) {
        PlayLayer* pl = PlayLayer::get();
        syncPlayer(m_p[0][s], pl->m_player1);
        if (m_two) syncPlayer(m_p[1][s], pl->m_player2);
        m_info[s] = SlotInfo();
        m_info[s].grounded = m_p[m_tp][s]->m_isOnGround;
        m_info[s].groundTicks = m_info[s].grounded ? 1 : 0;
        forceHoldObj(s, 0, holdFor(0));
        if (m_two) forceHoldObj(s, 1, holdFor(1));
    }

    void copySlot(int dst, int src) {
        syncPlayer(m_p[0][dst], m_p[0][src]);
        if (m_two) syncPlayer(m_p[1][dst], m_p[1][src]);
        SlotInfo keep = m_info[src];
        m_info[dst] = keep;
        forceHoldObj(dst, 0, keep.holding[0]);
        if (m_two) forceHoldObj(dst, 1, keep.holding[1]);
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

    // One physics tick for a slot. `hold` is the target player's input; the
    // other player (2-player mode) keeps whatever the game currently has.
    // false = a player died or the budget ran out.
    bool step(int s, bool hold) {
        if (m_used >= m_budget) {
            m_aborted = true;
            return false;
        }
        ++m_used;
        PlayLayer* pl = PlayLayer::get();
        PlayerObject* a = m_p[0][s];
        PlayerObject* b = m_two ? m_p[1][s] : nullptr;
        // 2-player mode: the two players are independent. Simulating the other one with a
        // frozen input made it die in the ghost, which failed EVERY delay of the target
        // (so nothing was buffered, logged or orb-detected). Only the target runs now.
        if (m_two && m_layout == Layout::TwoPlayer) {
            if (m_tp == 0) b = nullptr;
            else a = nullptr;
        }
        float full = stepDt();
        float dt = full * m_dtScale;
        m_stepDtNow = full;

        bool wasHold = m_info[s].holding[m_tp];
        setHold(s, hold);
        m_simDead = false;
        m_stepObj[0] = a;
        m_stepObj[1] = b;
        m_stepEdge[0] = m_stepEdge[1] = false;
        m_orb.done[0] = m_orb.done[1] = false;
        m_stepEdge[m_tp] = hold && !wasHold;
        if (m_mirror) m_stepEdge[1] = m_stepEdge[0];
        m_curSlot = s;
        m_hitUnknown = false;

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
        if (m_partial) {                  // first half of a split tick: the game checks collisions once, at the end
            m_active = nullptr;
            return true;
        }

        if (a) {
            m_active = a;
            resetCollisionLog(a);
            pl->checkCollisions(a, full, false);
        }
        if (!m_simDead && b) {
            m_active = b;
            resetCollisionLog(b);
            pl->checkCollisions(b, full, false);
        }
        m_active = nullptr;
        if (m_simDead) return false;

        SlotInfo& inf = m_info[s];
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
    // true  = survived until a stable landing (ground modes) or the whole
    //         trajectory (UFO / Swing, which have no ground to settle on).
    // false = died / budget; `reach` is the tick reached.
    // `unk` is set when the run only "survived" because it reached something the ghost cannot
    // predict (pad, orb, portal...): that is not a verified survival.
    bool rollClick(int s, int d, int holdTicks, int horizon, bool airMode, int& reach, bool& unk) {
        int tick = d;
        int holdLeft = holdTicks;
        while (true) {
            if (tick >= horizon) return true;
            if (!airMode && holdLeft == 0 && m_info[s].groundTicks >= cfg().settle) return true;
            if (m_aborted) return false;
            if (!step(s, holdLeft > 0)) {
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
    // A delay of d ticks works, d-1 does not: the real limit is somewhere inside tick d-1.
    // Bisect it with the ghost (slot 2 = state at the start of that tick) and return the part of
    // the tick after which the click can happen (1 = at the start of tick d, i.e. no gain).
    // The tick limit (max delay) only bounds the search; the click itself goes where the ghost says.
    bool trialSplit(bool click, int pi, int d, int h, int horizon, bool airMode, bool target, float f, bool groundMode) {
        orbBegin();
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
            bool good = rollClick(1, d, std::max(0, h - 1), horizon, airMode, reach, unk);
            // a ground mode only counts if the click really does something (jump from the ground, or an orb)
            if (good && groundMode && !m_info[2].grounded && !orbUsed()) return false;
            return good && !unk && !m_aborted;
        }
        for (int t = d; t < horizon; ++t) {
            if (!step(1, target)) return false;
            if (m_info[1].unknown) return false;       // a delay must survive the whole look-ahead
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
        return hi;
    }

    Decision decideClick(PlayLayer* pl, Mode mode, int pi) {
        auto& c = cfg();
        Decision r;
        int maxDelay = c.maxDelay[mode];
        if (maxDelay <= 0) return r;

        bool airMode = mode == M_UFO || mode == M_SWING;
        // Cube / ball / robot / spider only react to a click from the ground (or on an orb). A click in
        // the air that merely "survives" is wasted, so for these the buffer waits for the landing.
        bool groundMode = mode == M_CUBE || mode == M_BALL || mode == M_ROBOT || mode == M_SPIDER;
        int horizon = airMode ? std::min(c.trajectory, c.airHorizon) : c.trajectory;
        std::vector<int> holds = {c.tapTicks};
        if (mode == M_ROBOT) holds.push_back(std::max(c.tapTicks, c.robotHold));

        beginSearch(pl, pi);
        loadReal(0);

        // A delay is "safe" if the click survives. If no delay (0..max) survives, nothing
        // can be saved: the click goes through raw.
        int foundDelay = -1;

        int foundHold = holds[0];
        bool sawUnknown = false;                 // the prediction ended on something it cannot predict
        for (int d = 0; d <= maxDelay; ++d) {
            if (d > 0) {
                copySlot(2, 0);                  // state at the start of tick d-1 (for the sub-tick search)
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
            bool canAct = !groundMode || m_info[0].grounded;      // slot 0 = state at the start of tick d
            for (int h : holds) {
                copySlot(1, 0);
                m_info[1].groundTicks = 0;
                size_t mark = m_trail.size();
                trailPush(0, 1);
                bool unk = false;
                orbBegin();
                if (groundMode && !canAct) {
                    // In the air only an orb can make the click useful. Probing 2 ticks is enough to see
                    // that, so a long fall does not cost a full look-ahead per tick (that used to eat
                    // the whole budget and the click went through raw: no buffer at all).
                    int r0 = d;
                    bool u0 = false;
                    bool probe = rollClick(1, d, h, d + 2, false, r0, u0);
                    if (!probe || !orbUsed()) {
                        m_trail.resize(mark);
                        if (m_aborted) break;
                        continue;
                    }
                    copySlot(1, 0);
                    m_info[1].groundTicks = 0;
                    orbBegin();
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
                    r.orb = orbUsed();               // this very run is the chosen one
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
            if (foundDelay > 0 && !m_aborted && c.subTick && m_layout == Layout::Solo && !cbfActive()) {
                r.frac = refineFrac(true, pi, foundDelay, foundHold, horizon, airMode, false, groundMode);
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
        forceHoldObj(s, 0, m_histHold[idx]);
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
        po->m_gravityMod = g->m_gravityMod;
        po->m_isOnGround = g->m_isOnGround;
        po->m_isUpsideDown = g->m_isUpsideDown;
        po->m_vehicleSize = g->m_vehicleSize;
        po->m_playerSpeed = g->m_playerSpeed;
        po->m_yVelocity = g->m_yVelocity;
        po->m_gravity = g->m_gravity;
        po->m_speedMultiplier = g->m_speedMultiplier;
        po->m_lastPosition = g->m_lastPosition;
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
    // The new hold / release state is assumed to stay for the look-ahead.
    //  - a delay (0..max) survives the whole look-ahead -> the smallest one wins (a delay that
    //    only gets there by ending on something unpredictable does not count);
    //  - otherwise nothing can be saved and the input passes raw, so the controls stay free.
    Decision decideHold(PlayLayer* pl, Mode mode, int pi, bool target) {
        auto& c = cfg();
        Decision r;
        int maxDelay = c.maxDelay[mode];
        if (maxDelay <= 0) return r;
        int horizon = std::max(1, mode == M_WAVE ? c.waveHorizon : c.shipHorizon);

        beginSearch(pl, pi);
        loadReal(0);

        int foundDelay = -1;
        int firstReach = -1;
        bool sawUnknown = false;                 // the prediction ended on something it cannot predict

        for (int d = 0; d <= maxDelay; ++d) {
            if (d > 0) {
                copySlot(2, 0);                  // state at the start of tick d-1 (for the sub-tick search)
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
            bool alive = true;
            bool ended = false;                  // the run stopped on something unpredictable
            int reach = d;
            for (int t = d; t < horizon; ++t) {
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
            // A DELAY is only a rescue if it survives the whole look-ahead: steering towards a
            // pad / orb / portal until the prediction stops proves nothing.
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
            if (c.subTick && m_layout == Layout::Solo && !cbfActive()) {
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
    bool planAuto(PlayLayer* pl) {
        auto& c = cfg();
        int s0 = std::max(1, c.autoStride);
        m_aiHorizon = c.autoHorizon;
        m_levels.clear();
        int total = 0;
        for (int i = 0; total < m_aiHorizon && i < kSlots - 2; ++i) {
            int seg = i < 6 ? s0 : (s0 << std::min(i - 5, 5));
            m_levels.push_back(seg);
            total += seg;
        }

        beginSearch(pl, 0, false, m_layout == Layout::DualSolo);   // autoplay: one input, both players
        loadReal(0);
        m_failMemo.clear();                                        // dead states stay dead for every route
        m_aiContinuous = isContinuous(detectMode(pl->m_player1));

        int paths = std::max(1, c.autoPaths);
        bool prev = m_game[0];
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
            m_budget = m_used + static_cast<int>(routes.size()) * 4 * m_aiHorizon + 64;
            for (size_t i = 0; i < routes.size(); ++i) {
                static constexpr int shifts[4] = {-2, -1, 1, 2};
                for (int sh : shifts) {
                    if (aiSurvives(routes[i].plan, sh, prev)) ++routes[i].tol;
                }
                long score = 1000000L + routes[i].tol * 10000L - routes[i].toggles * 100L + routes[i].len;
                if (score > bestScore) {
                    bestScore = score;
                    best = static_cast<int>(i);
                }
            }
        }

        bool ok = best >= 0;
        if (ok) {
            m_aiPlan = routes[best].plan;
            m_aiPlanTotal = std::max(1, routes[best].len);
            if (m_wantTrail) m_trailOk = routes[best].trail;
        } else if (!partial.empty()) {
            m_aiPlan = partial;
            m_aiPlanTotal = std::max(1, partialReach);
        } else {
            m_aiPlan.assign(1, AiSeg{prev, 1});
            m_aiPlanTotal = 1;
        }
        m_aiPlanPos = 0;
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

        if (m_aiPlan.empty() || m_aiPlanPos * 10 >= m_aiPlanTotal * 9) {   // 90% played: think up new routes
            bool ok = planAuto(pl);
            ++m_dbgSearches;
            m_lastText = fmt::format("AI {} paths={} len={}t used={}", ok ? "plan" : "best",
                                     c.autoPaths, m_aiPlanTotal, m_used);
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
        if (m_layout == Layout::DualSolo && m_game[1] != m_aiHold) fire(pl, 1, m_aiHold);

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
        m_budget = c.previewLen + 8;
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
            m_budget = m_used + m_aiHorizon + 16;
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
