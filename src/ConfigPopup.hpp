#pragma once
#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/Scrollbar.hpp>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>
#include "Config.hpp"
#include "Engine.hpp"

using namespace geode::prelude;

namespace buf {

class ConfigPopup : public Popup {
protected:
    static inline ConfigPopup* s_current = nullptr;   // the open window (for the hotkey)
    CCNode* m_page = nullptr;
    std::vector<CCNode*> m_pageButtons;
    std::vector<CCMenuItemSpriteExtra*> m_tabs;
    std::vector<bool*> m_bools;
    bool m_building = false;
    int m_tab = 0;

    struct Item {
        std::string label;
        bool* b = nullptr;
        int* n = nullptr;
        int lo = 0;
        int hi = 0;
    };

    static Item toggle(char const* label, bool* ref) {
        Item i;
        i.label = label;
        i.b = ref;
        return i;
    }
    static Item number(std::string label, int* ref, int lo, int hi) {
        Item i;
        i.label = std::move(label);
        i.n = ref;
        i.lo = lo;
        i.hi = hi;
        return i;
    }

    bool init() {
        if (!Popup::init(440.f, 300.f)) return false;
        s_current = this;
        m_noElasticity = true;
        setTitle("Dynamic Buffer");

        // page ids: 0 General, 1 Delays, 2 Physics, 3 Search, 4 Display, 5 Auto, 6 Log.
        // The log is the first page.
        static constexpr char const* names[7] = {"General", "Delays", "Physics", "Search", "Display", "Auto", "Log"};
        static constexpr int order[7] = {6, 0, 1, 2, 3, 4, 5};
        for (int pos = 0; pos < 7; ++pos) {
            int id = order[pos];
            auto sprite = ButtonSprite::create(names[id], "bigFont.fnt", "GJ_button_05.png", .6f);
            sprite->setScale(.5f);
            auto tab = CCMenuItemSpriteExtra::create(
                sprite, this, menu_selector(ConfigPopup::onTab)
            );
            tab->setTag(id);
            m_tabs.push_back(tab);
            m_buttonMenu->addChildAtPosition(tab, Anchor::Center, ccp(-180.f + pos * 60.f, 112.f));
        }
        buildPage(6);
        return true;
    }

    void onTab(CCObject* sender) {
        buildPage(sender->getTag());
    }

    void onToggle(CCObject* sender) {
        auto t = static_cast<CCMenuItemToggler*>(sender);
        size_t idx = static_cast<size_t>(t->getTag());
        if (idx < m_bools.size() && m_bools[idx]) {
            *m_bools[idx] = !t->isToggled(); // callback runs before the toggler flips
            saveConfig();
        }
    }

    void clearPage() {
        if (m_page) {
            m_page->removeFromParentAndCleanup(true);
            m_page = nullptr;
        }
        for (auto* b : m_pageButtons) b->removeFromParentAndCleanup(true);
        m_pageButtons.clear();
        m_bools.clear();
    }

    void addNote(std::string const& text, float y) {
        auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
        label->setScale(.27f);
        label->setOpacity(190);
        label->setAlignment(kCCTextAlignmentCenter);
        m_page->addChild(label);
        label->setPosition(ccp(0.f, y));
    }

    void layoutItems(std::vector<Item> const& items) {
        int rows = std::max(4, static_cast<int>((items.size() + 1) / 2));
        auto& c = cfg();
        (void)c;
        for (size_t i = 0; i < items.size(); ++i) {
            int col = static_cast<int>(i) / rows;
            int row = static_cast<int>(i) % rows;
            float labelX = col == 0 ? -205.f : 15.f;
            float ctrlX = col == 0 ? -40.f : 180.f;
            float y = 72.f - row * 30.f;

            auto label = CCLabelBMFont::create(items[i].label.c_str(), "bigFont.fnt");
            label->setScale(.36f);
            label->setAnchorPoint({0.f, .5f});
            m_page->addChild(label);
            label->setPosition(ccp(labelX, y));

            if (items[i].b) {
                m_bools.push_back(items[i].b);
                auto toggler = CCMenuItemToggler::createWithStandardSprites(
                    this, menu_selector(ConfigPopup::onToggle), .55f
                );
                toggler->setTag(static_cast<int>(m_bools.size()) - 1);
                toggler->toggle(*items[i].b);
                m_buttonMenu->addChildAtPosition(toggler, Anchor::Center, ccp(ctrlX, y));
                m_pageButtons.push_back(toggler);
            } else if (items[i].n) {
                auto input = TextInput::create(64.f, "0", "bigFont.fnt");
                input->setScale(.75f);
                input->setFilter("0123456789");
                input->setMaxCharCount(5);
                int* ref = items[i].n;
                int lo = items[i].lo;
                int hi = items[i].hi;
                input->setCallback([this, ref, lo, hi](std::string const& s) {
                    if (m_building || s.empty()) return;
                    *ref = clampi(std::atoi(s.c_str()), lo, hi);
                    saveConfig();
                });
                input->setString(std::to_string(*ref));
                m_page->addChild(input);
                input->setPosition(ccp(ctrlX, y));
            }
        }
    }

    void onClearLog(CCObject*) {
        g_engine.clearLog();
        buildPage(6);
    }

    // The log: newest line at the bottom, scrollable, at most Engine::kLogMax lines.
    void buildLogPage() {
        auto const& lines = g_engine.logLines();
        constexpr float w = 392.f;
        constexpr float h = 150.f;
        constexpr float lineH = 15.f;
        float cy = 14.f;                                    // list centre, relative to the popup centre

        auto bg = CCLayerColor::create({0, 0, 0, 110}, w, h);
        bg->setPosition(ccp(-w / 2.f, cy - h / 2.f));
        m_page->addChild(bg);

        auto scroll = ScrollLayer::create({w, h});
        scroll->setPosition(ccp(-w / 2.f, cy - h / 2.f));
        m_page->addChild(scroll);

        float total = std::max(h, static_cast<float>(lines.size()) * lineH + 8.f);
        scroll->m_contentLayer->setContentSize({w, total});
        if (lines.empty()) {
            auto label = CCLabelBMFont::create("No rescues logged yet.", "chatFont.fnt");
            label->setScale(.6f);
            label->setAnchorPoint({0.f, 1.f});
            label->setPosition({8.f, total - 6.f});
            scroll->m_contentLayer->addChild(label);
        }
        float y = total - 5.f;
        for (auto const& line : lines) {
            auto label = CCLabelBMFont::create(line.c_str(), "chatFont.fnt");
            label->setScale(.55f);
            label->setAnchorPoint({0.f, 1.f});
            label->setPosition({6.f, y});
            scroll->m_contentLayer->addChild(label);
            y -= lineH;
        }
        scroll->m_contentLayer->setPositionY(0.f);          // newest (bottom) in view

        auto bar = Scrollbar::create(scroll);
        bar->setPosition(ccp(w / 2.f + 8.f, cy));
        m_page->addChild(bar);

        auto sprite = ButtonSprite::create("Clear", "bigFont.fnt", "GJ_button_05.png", .6f);
        sprite->setScale(.6f);
        auto clear = CCMenuItemSpriteExtra::create(
            sprite, this, menu_selector(ConfigPopup::onClearLog)
        );
        m_buttonMenu->addChildAtPosition(clear, Anchor::Center, ccp(150.f, -95.f));
        m_pageButtons.push_back(clear);

        auto dbg = CCLabelBMFont::create(g_engine.inputInfo().c_str(), "chatFont.fnt");
        dbg->setScale(.4f);
        dbg->setAnchorPoint({0.f, .5f});
        dbg->setOpacity(200);
        m_page->addChild(dbg);
        dbg->setPosition(ccp(-190.f, -110.f));

        std::string status = std::to_string(lines.size()) + "/" + std::to_string(Engine::kLogMax) +
                             " lines";
        if (!cfg().logEnabled) status += "  |  log is OFF (Display > Enable log)";
        auto note = CCLabelBMFont::create(status.c_str(), "bigFont.fnt");
        note->setScale(.27f);
        note->setOpacity(190);
        note->setAnchorPoint({0.f, .5f});
        m_page->addChild(note);
        note->setPosition(ccp(-190.f, -97.f));
    }

    void buildPage(int tab) {
        m_building = true;
        cfg().pauseButton = Mod::get()->getSettingValue<bool>("pause-button");   // may have been changed in Geode's settings
        clearPage();
        m_tab = tab;
        for (size_t i = 0; i < m_tabs.size(); ++i) {
            m_tabs[i]->setOpacity(m_tabs[i]->getTag() == tab ? 255 : 170);
        }

        m_page = CCNode::create();
        m_mainLayer->addChildAtPosition(m_page, Anchor::Center, ccp(0.f, 0.f));

        if (tab == 6) {
            buildLogPage();
            m_building = false;
            return;
        }

        auto& c = cfg();
        std::vector<Item> items;
        std::string note;

        if (tab == 0) {
            items = {
                toggle("Enabled", &c.enabled),
                toggle("Orb detect(test)", &c.orbDetect),
                toggle("Drop fatal clicks", &c.dropFatal),
                toggle("Pause menu button", &c.pauseButton),
                toggle("Dual support", &c.dualSupport),
                toggle("2-player support", &c.twoPlayerMode),
                toggle("Both screen sides", &c.bothSides),
                toggle("Wave angle override", &c.waveAngles),
                toggle("Move object detect(test)", &c.moveDetect),
                number("Trajectory (ticks)", &c.trajectory, 20, 400),
            };
            note = "Dual support: one input drives both players.\n"
                   "Orb detect(test): an orb detect\n"
                   "Move detect(test): moving objects are predicted from their measured speed.\n"
                   "Right Alt (Windows): open / close this window.";
        } else if (tab == 1) {
            for (int i = 0; i < M_COUNT; ++i) {
                items.push_back(number(
                    std::string(kModeNames[i]) + " max delay", &c.maxDelay[i], 0, 64
                ));
            }
            items.push_back(number("Ship imminent (ticks)", &c.shipImminent, 2, 120));
            items.push_back(number("Wave imminent (ticks)", &c.waveImminent, 2, 120));
            items.push_back(number("Hold min gain (ticks)", &c.holdGain, 1, 30));
            note = "Max ticks a press / release may be held back (0 = mode off).\n"
                   "Ship/Wave only act when holding or releasing NOW would kill you\n"
                   "within 'imminent' ticks and waiting pushes it back by 'min gain'.";
        } else if (tab == 2) {
            items = {
                number("TPS (0 = auto)", &c.tps, 0, 10000),
                number("FPS (0 = auto)", &c.fps, 0, 1000),
                number("Tap length (ticks)", &c.tapTicks, 1, 40),
                number("Sim budget (ticks)", &c.budget, 200, 40000),
                number("Wave up (1/100 deg)", &c.waveUp, 3000, 8000),
                number("Wave down (1/100 deg)", &c.waveDown, 3000, 8000),
                number("Mini up (1/100 deg)", &c.miniUp, 3000, 8500),
                number("Mini down (1/100 deg)", &c.miniDown, 3000, 8500),
            };
            note = g_engine.timingInfo() + (Engine::cbfLoaded() ? "\nClick Between Frames: detected" : "\nClick Between Frames: not found") +
                   "\n" + g_engine.orbInfo() +
                   "\nWave angles: ghost wave follows these slopes (4510 = 45.10 deg).";
        } else if (tab == 3) {
            items = {
                number("Ship look (ticks)", &c.shipHorizon, 4, 300),
                number("Wave look (ticks)", &c.waveHorizon, 4, 300),
                number("Landing settle (ticks)", &c.settle, 1, 30),
                number("Air modes look (ticks)", &c.airHorizon, 10, 300),
                number("Robot long hold", &c.robotHold, 1, 60),
                number("Min gain (ticks)", &c.gainClick, 1, 60),
                number("Min budget (ticks)", &c.minBudget, 50, 5000),
                number("Noclip memory (ticks)", &c.noclipTicks, 0, 5000),
                number("Fail-open frames", &c.failOpen, 1, 60),
            };
            note = "Ship/Wave look: a hold/release must survive this many ticks\n"
                   "Min gain: with no safe delay, a delay\n"
                   "must survive this much longer than the ones before it.";
        } else if (tab == 5) {
            items = {
                toggle("Autoplay", &c.autoplay),
                toggle("Learn from deaths", &c.autoLearn),
                toggle("Keep memory (levels)", &c.autoKeep),
                number("Look ahead (ticks)", &c.autoHorizon, 20, 300),
                number("Path number", &c.autoPaths, 1, 12),
                number("Decision step (ticks)", &c.autoStride, 1, 12),
                number("Search budget (ticks)", &c.autoBudget, 200, 20000),
                number("Memory cell (px)", &c.autoCell, 2, 60),
                number("Blame window (ticks)", &c.autoWindow, 5, 400),
                number("Avoid after (strikes)", &c.autoPrune, 1, 20),
                number("Forgive after (ticks)", &c.autoForget, 20, 2000),
            };
            note = "Autoplay plays by itself\n";
        } else {
            items = {
                toggle("Show trajectory", &c.showTrajectory),
                toggle("Hold + release paths", &c.previewBoth),
                toggle("Show path (scan)", &c.showPath),
                toggle("Enable log", &c.logEnabled),
                number("Preview length", &c.previewLen, 10, 400),
                number("Preview refresh", &c.previewEvery, 1, 60),
                number("Keep click path", &c.keepTicks, 0, 2000),
                number("Line width (1/10 px)", &c.lineWidth, 2, 40),
                number("Opacity (%)", &c.trajOpacity, 10, 100),
                number("Path look (ticks)", &c.pathHorizon, 20, 300),
                number("Path refresh", &c.pathEvery, 1, 60),
                number("Path budget", &c.pathBudget, 200, 20000),
            };
            note = "Cyan = path if you HOLD, orange = path if you RELEASE\n"
                   "(the one that matches your current input is brighter).\n"
                   "Pink = Show path: a feasible route it scanned (display only).\n"
                   "Green = chosen click path, red dot = death, yellow dot = click.\n"
                   "Enable log: every rescued event is written to the Log page.";
        }

        layoutItems(items);
        addNote(note, -114.f);
        m_building = false;
    }

public:
    ~ConfigPopup() override {
        if (s_current == this) s_current = nullptr;
    }
    static void toggle() {
        if (s_current && s_current->getParent()) {
            auto* cur = s_current;
            s_current = nullptr;
            cur->onClose(nullptr);
            return;
        }
        if (auto popup = create()) popup->show();
    }

    static ConfigPopup* create() {
        auto ret = new ConfigPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

} // namespace buf
