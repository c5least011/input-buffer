#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/RingObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/binding/RingObject.hpp>
#include <Geode/utils/cocos.hpp>
#ifdef GEODE_IS_WINDOWS
#include <Geode/modify/CCScheduler.hpp>
#endif
#include "Config.hpp"
#include "Engine.hpp"
#include "ConfigPopup.hpp"
#include "OpenUiSetting.hpp"

#ifdef GEODE_IS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

using namespace geode::prelude;
using buf::g_engine;

$on_mod(Loaded) {
    // "Open settings window" button inside Geode's own mod settings page
    auto result = Mod::get()->registerCustomSettingType(
        "open-ui",
        [](std::string key, std::string modID, matjson::Value const& json) {
            return buf::OpenUiSetting::parse(key, modID, json);
        }
    );
    if (!result) {
        log::error("Could not register the settings window button: {}", result.unwrapErr());
    }
    buf::loadConfig();
}

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", -2147483647);  // run before noclip mods
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", -1000000);
        (void)self.setHookPriority("PlayLayer::flipGravity", -1000000);
    }

    bool init(GJGameLevel* level, bool dontCreateObjects) {
        if (!PlayLayer::init(level, dontCreateObjects)) return false;
        g_engine.init();
        return true;
    }

    void resetLevel() {
        g_engine.onReset();
        g_engine.resetMarks();
        PlayLayer::resetLevel();
    }

    void destroyPlayer(PlayerObject* player, GameObject* gameObject) {
        if (g_engine.onSimDeath(player)) return;       // ghost player
        PlayLayer::destroyPlayer(player, gameObject);
        g_engine.onRealDestroyed(player);              // real death, or one a noclip mod blocked
    }

    void playEndAnimationToPos(cocos2d::CCPoint p0) {
        if (g_engine.isSimulating()) return;
        PlayLayer::playEndAnimationToPos(p0);
    }

    void flipGravity(PlayerObject* p0, bool p1, bool p2) {
        if (g_engine.isSimulating()) {
            if (!g_engine.isSimPlayer(p0)) return;
            PlayLayer::flipGravity(p0, p1, true);
            return;
        }
        PlayLayer::flipGravity(p0, p1, p2);
    }

    void onQuit() {
        g_engine.quit();
        PlayLayer::onQuit();
    }
};

class $modify(BufPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        // OFF: the settings window is only reachable from the mod's page in Geode
        if (!Mod::get()->getSettingValue<bool>("pause-button")) return;
        auto sprite = ButtonSprite::create("BUF", "bigFont.fnt", "GJ_button_05.png", .6f);
        sprite->setScale(.7f);
        auto button = CCMenuItemExt::createSpriteExtra(
            sprite, [](CCMenuItemSpriteExtra*) {
                if (auto popup = buf::ConfigPopup::create()) popup->show();
            }
        );
        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        auto win = CCDirector::sharedDirector()->getWinSize();
        button->setPosition({win.width - 46.f, 46.f});
        menu->addChild(button);
        this->addChild(menu, 10);
    }
};

class $modify(BufEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool unk) {
        bool res = LevelEditorLayer::init(level, unk);
        g_engine.quit();
        return res;
    }
};

class $modify(BufHardStreak, HardStreak) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("HardStreak::addPoint", -1000000);
    }

    void addPoint(cocos2d::CCPoint p0) {
        if (g_engine.isSimulating()) return;
        HardStreak::addPoint(p0);
    }
};

class $modify(BufEffectGameObject, EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("EffectGameObject::triggerObject", -1000000);
    }

    void triggerObject(GJBaseGameLayer* p0, int p1, std::vector<int> const* p2) {
        if (g_engine.isSimulating()) {
            g_engine.simGravityPortal(this);
            return;
        }
        if (m_objectID == 901) g_engine.onRealMoveTrigger(m_duration);   // move trigger
        EffectGameObject::triggerObject(p0, p1, p2);
    }
};

class $modify(BufGameObject, GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameObject::playShineEffect", -1000000);
    }

    void playShineEffect() {
        if (g_engine.isSimulating()) return;
        GameObject::playShineEffect();
    }
};

class $modify(BufPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", -1000000);
        (void)self.setHookPriority("PlayerObject::incrementJumps", -1000000);
        (void)self.setHookPriority("PlayerObject::ringJump", -1000000);
        // outermost: Click Between Frames splits the step INSIDE its own update hook. This hook then
        // sees one call per physics tick and CBF's sub-steps run below it.
        (void)self.setHookPriority("PlayerObject::update", -1000000);
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        if (g_engine.isSimulating()) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    bool pushButton(PlayerButton button) {
        if (button == PlayerButton::Jump) g_engine.noteReceiver(this);
        return PlayerObject::pushButton(button);
    }

    bool releaseButton(PlayerButton button) {
        if (button == PlayerButton::Jump) g_engine.noteReceiver(this);
        return PlayerObject::releaseButton(button);
    }

    void incrementJumps() {
        if (g_engine.isSimulating() && !g_engine.isSimPlayer(this)) return;
        PlayerObject::incrementJumps();
    }

    void update(float dt) {
        PlayLayer* pl = PlayLayer::get();
        bool real = pl && !g_engine.isSimulating();
        if (real && g_engine.inUpdate()) {
            // nested call (CBF updates the players again for each sub-step of the tick): not a new tick
            PlayerObject::update(dt);
            return;
        }
        float split = 1.f;
        if (real) {
            g_engine.setFrameDelta(dt);
            g_engine.addClock(this, dt);
            g_engine.onTick(this);
            split = g_engine.splitFor(this);            // always 1 while CBF is active
        }
        bool track = real && this == pl->m_player1;
        if (track) g_engine.setInUpdate(true);          // clicks delivered from here land inside the tick
        if (split < 1.f) {
            // sub-tick click (own CBF-like split): run the first part of the tick, click, run the rest
            PlayerObject::update(dt * split);
            g_engine.fireSplit();
            PlayerObject::update(dt * (1.f - split));
        } else {
            PlayerObject::update(dt);
        }
        if (track) g_engine.setInUpdate(false);
    }

    void ringJump(RingObject* p0, bool p1) {
        if (g_engine.isSimulating() && !g_engine.isSimPlayer(this)) return;
        PlayerObject::ringJump(p0, p1);
    }

    void flipGravity(bool flip, bool noEffects) {
        if (g_engine.isSimulating() && g_engine.isSimPlayer(this)) {
            PlayerObject::flipGravity(flip, true);          // ghost: never show the effect
            return;
        }
        PlayerObject::flipGravity(flip, noEffects);
    }

    void spawnPortalCircle(cocos2d::ccColor3B color, float startRadius) {
        if (g_engine.isSimulating()) return;
        PlayerObject::spawnPortalCircle(color, startRadius);
    }

    void playBumpEffect(int objectType, GameObject* object) {
        if (g_engine.isSimulating()) return;
        PlayerObject::playBumpEffect(objectType, object);
    }
};

// Orbs touched by a ghost player must not flash / pulse on screen.
class $modify(BufRingObject, RingObject) {
    void powerOnObject(int state) {
        if (g_engine.isSimulating()) return;
        RingObject::powerOnObject(state);
    }

    void spawnCircle() {
        if (g_engine.isSimulating()) return;
        RingObject::spawnCircle();
    }
};

class $modify(BufBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", -1000000);
    }

    bool canBeActivatedByPlayer(PlayerObject* p0, EffectGameObject* p1) {
        if (g_engine.isSimulating()) {
            return g_engine.canSimRing(p0, p1);
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(p0, p1);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!g_engine.isSimulating() && !g_engine.isFiring() && button == 1 && PlayLayer::get()) {
            if (!g_engine.onInput(this, down, isPlayer1)) {
                return;
            }
        }
        bool track = button == 1 && !g_engine.isSimulating() && PlayLayer::get();
        if (track) g_engine.beginHandle();
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (track) g_engine.endHandle(isPlayer1, down);      // learn which player the game gave it to
        if (track) g_engine.endLate();                        // late rescue: the press is in, finish the rewind
    }

    void collisionCheckObjects(PlayerObject* player, std::vector<GameObject*>* vec, int objectsCount, float dt) {
        if (g_engine.isSimulating()) {
            g_engine.simTouchObjects(player, vec, objectsCount);   // gravity / speed portals, unknown objects
            thread_local std::vector<GameObject*> extra;
            thread_local std::vector<std::pair<GameObject*, CCPoint>> moved;
            extra.clear();
            moved.clear();
            if (extra.capacity() < static_cast<size_t>(objectsCount)) {
                extra.reserve(objectsCount);
            }
            for (int i = 0; i < objectsCount; i++) {
                GameObject* obj = vec->at(i);
                if (!obj) continue;
                if (obj->m_groups && obj->m_groups->at(0) != 0) {
                    CCPoint off;
                    auto st = g_engine.simMove(obj, off);
                    if (st == buf::Engine::MoveState::Predicted) {
                        moved.emplace_back(obj, obj->getPosition());   // restored right after the check
                        obj->setPosition(obj->getPosition() + off);
                    } else if (st == buf::Engine::MoveState::Unknown) {
                        g_engine.flagMovingObject();       // moving, speed unknown: do not trust it
                    }
                }
                auto type = obj->m_objectType;
                if (type == GameObjectType::Solid ||
                    (g_engine.dBlockSim() && buf::isSpecialBlock(obj)) ||             // D / J / S / H blocks, by ID
                    type == GameObjectType::Hazard ||
                    type == GameObjectType::AnimatedHazard ||
                    type == GameObjectType::CollisionObject ||
                    type == GameObjectType::Slope ||
                    type == GameObjectType::NormalGravityPortal ||
                    type == GameObjectType::InverseGravityPortal ||
                    type == GameObjectType::GravityTogglePortal ||
                    geode::cast::typeinfo_cast<RingObject*>(obj)) {
                    extra.push_back(obj);
                }
            }
            // dynamic collision blocks (the "D" ones) are solid: wave slides on them. They may not be in
            // the list the game handed us, so the ones near the ghost are added.
            if (g_engine.dBlockSim()) {
                auto* pl2 = PlayLayer::get();
                if (pl2 && pl2->m_collisionBlocks && pl2->m_collisionBlocks->count() > 0) {
                    CCRect pr = player->getObjectRect();
                    pr.origin.x -= 60.f;
                    pr.origin.y -= 60.f;
                    pr.size.width += 120.f;
                    pr.size.height += 120.f;
                    for (auto* o : geode::cocos::CCArrayExt<GameObject*>(pl2->m_collisionBlocks)) {
                        if (!buf::isDynamicCollisionBlock(o)) continue;
                        bool dup = false;
                        for (size_t k = 0; k < extra.size(); ++k) {
                            if (extra[k] == o) {
                                dup = true;
                                break;
                            }
                        }
                        if (dup || !o->getObjectRect().intersectsRect(pr)) continue;
                        extra.push_back(o);
                    }
                }
            }
            GJBaseGameLayer::collisionCheckObjects(player, &extra, extra.size(), dt);
            for (auto& mv : moved) mv.first->setPosition(mv.second);
            moved.clear();
            return;
        }
        if (auto pl = PlayLayer::get()) {
            if (player == pl->m_player1) g_engine.observeObjects(vec, objectsCount);
        }
        GJBaseGameLayer::collisionCheckObjects(player, vec, objectsCount, dt);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (g_engine.isSimulating()) {
            if (g_engine.canSimRing(player, ring)) {
                player->ringJump(ring, true);
            }
            return;
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }
};

#ifdef GEODE_IS_WINDOWS
// Right Alt toggles the settings window (Windows only). Polled once per frame, so it
// does not depend on the keyboard dispatcher signature of the current GD version.
class $modify(BufScheduler, CCScheduler) {
    void update(float dt) {
        CCScheduler::update(dt);

        static bool was = false;
        bool down = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
        if (down && !was) {
            HWND fg = GetForegroundWindow();
            DWORD pid = 0;
            if (fg) GetWindowThreadProcessId(fg, &pid);
            if (pid == GetCurrentProcessId()) {       // only while the game window has focus
                buf::ConfigPopup::toggle();
            }
        }
        was = down;
    }
};
#endif