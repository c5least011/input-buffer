#pragma once
#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/Scrollbar.hpp>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>
#include <vector>
#include "Config.hpp"
#include "Engine.hpp"
#include "ConfigIO.hpp"
#include "ConfigHub.hpp"

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

    // Config tab (page id 7): sub page 0 = Local, 1 = Explore
    struct Entry {
        int id = 0;
        std::string name, author;
        int64_t likes = 0, downloads = 0;
    };
    int m_sub = 0;
    std::string m_status;
    CCLabelBMFont* m_statusLabel = nullptr;
    TextInput* m_search = nullptr;
    std::string m_query, m_draft;
    std::vector<Entry> m_entries;
    int m_pageNo = 1, m_pages = 1;
    int64_t m_total = 0;
    bool m_sortTop = true, m_fetched = false, m_loading = false;
    std::string m_error;
    geode::async::TaskHolder<geode::utils::file::PickResult> m_pick;
    geode::async::TaskHolder<geode::utils::web::WebResponse> m_web;

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
        static constexpr char const* names[8] = {"General", "Delays", "Physics", "Search", "Display", "Auto", "Log", "Config"};
        static constexpr int order[8] = {6, 0, 1, 2, 3, 4, 5, 7};
        for (int pos = 0; pos < 8; ++pos) {
            int id = order[pos];
            auto sprite = ButtonSprite::create(names[id], "bigFont.fnt", "GJ_button_05.png", .6f);
            sprite->setScale(.5f);
            auto tab = CCMenuItemSpriteExtra::create(
                sprite, this, menu_selector(ConfigPopup::onTab)
            );
            tab->setTag(id);
            m_tabs.push_back(tab);
            m_buttonMenu->addChildAtPosition(tab, Anchor::Center, ccp(-182.f + pos * 52.f, 112.f));
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

    // ------------------------------------------------------------ Config tab

    void setStatus(std::string const& s) {
        m_status = s;
        if (m_statusLabel) m_statusLabel->setString(s.c_str());
    }

    void addButton(char const* text, float scale, CCPoint pos, std::function<void()> cb) {
        auto b = hub::button(text, scale, std::move(cb));
        m_buttonMenu->addChildAtPosition(b, Anchor::Center, pos);
        m_pageButtons.push_back(b);
    }

    void buildConfigPage() {
        for (int s = 0; s < 2; ++s) {
            addButton(s == 0 ? "Local" : "Explore", s == m_sub ? .55f : .45f, ccp(-35.f + s * 70.f, 86.f), [this, s] {
                m_sub = s;
                buildPage(7);
            });
        }
        if (m_sub == 0) buildLocal();
        else buildExplore();
    }

    void buildLocal() {
        addNote("Export writes your settings to a .json file.\n"
                "Import loads one and applies it (a backup of your current settings is kept).", 52.f);
        addButton("Export", .7f, ccp(-100.f, 10.f), [this] { doExport(); });
        addButton("Import", .7f, ccp(0.f, 10.f), [this] { doImport(); });
        addButton("Open folder", .55f, ccp(100.f, 10.f), [] { geode::utils::file::openFolder(cfgio::configsDir()); });

        m_statusLabel = CCLabelBMFont::create(m_status.c_str(), "chatFont.fnt");
        m_statusLabel->setScale(.55f);
        m_statusLabel->setWidth(600.f);
        m_page->addChild(m_statusLabel);
        m_statusLabel->setPosition(ccp(0.f, -40.f));
        addNote("Default folder: " + geode::utils::string::pathToString(cfgio::configsDir()), -100.f);
    }

    void doExport() {
        geode::utils::file::FilePickOptions opts;
        opts.defaultPath = cfgio::configsDir();
        m_pick.spawn(
            geode::utils::file::pick(geode::utils::file::PickMode::OpenFolder, std::move(opts)),
            [this](geode::utils::file::PickResult r) {
                std::filesystem::path dir = cfgio::configsDir();
                std::string note;
                if (r.isErr()) note = " (folder picker failed, used the default folder)";
                else if (!r.unwrap()) return;                       // cancelled
                else dir = *r.unwrap();
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                auto path = dir / fmt::format("buf-config-{}.json", static_cast<long long>(std::time(nullptr)));
                auto w = geode::utils::file::writeString(path, cfgio::exportJson().dump(2));
                setStatus(w.isOk() ? "Exported: " + geode::utils::string::pathToString(path) + note
                                   : "Export failed: " + w.unwrapErr());
            }
        );
    }

    void doImport() {
        geode::utils::file::FilePickOptions opts;
        opts.defaultPath = cfgio::configsDir();
        opts.filters.push_back({"Config", {"*.json"}});
        m_pick.spawn(
            geode::utils::file::pick(geode::utils::file::PickMode::OpenFile, std::move(opts)),
            [this](geode::utils::file::PickResult r) {
                if (r.isErr()) { setStatus("Picker failed: " + r.unwrapErr()); return; }
                if (!r.unwrap()) return;
                auto json = cfgio::readFile(*r.unwrap());
                if (json.isErr()) { setStatus("Import failed: " + json.unwrapErr()); return; }
                cfgio::backup();
                auto n = cfgio::apply(json.unwrap());
                if (n.isErr()) { setStatus("Import failed: " + n.unwrapErr()); return; }
                m_status = fmt::format("Imported {} settings from {}", n.unwrap(), r.unwrap()->filename().string());
                buildPage(7);
            }
        );
    }

    void fetchList() {
        m_fetched = true;
        m_loading = true;
        m_error.clear();
        auto url = fmt::format("{}/configs?q={}&page={}&sort={}", hub::baseUrl(), hub::urlEncode(m_query),
                               m_pageNo, m_sortTop ? "top" : "new");
        m_web.spawn(hub::get(url), [this](geode::utils::web::WebResponse res) {
            m_loading = false;
            m_entries.clear();
            auto j = hub::parse(res);
            if (j.isErr()) {
                m_error = j.unwrapErr();
            } else {
                auto const& v = j.unwrap();
                m_pages = std::max<int>(1, static_cast<int>(hub::ji(v, "pages", 1)));
                m_total = hub::ji(v, "total");
                if (v.contains("items")) {
                    auto arr = v["items"].asArray();
                    if (arr.isOk()) {
                        for (auto const& it : arr.unwrap()) {
                            Entry e;
                            e.id = static_cast<int>(hub::ji(it, "id"));
                            e.name = hub::js(it, "name");
                            e.author = hub::js(it, "author");
                            e.likes = hub::ji(it, "likes");
                            e.downloads = hub::ji(it, "downloads");
                            m_entries.push_back(std::move(e));
                        }
                    }
                }
            }
            if (m_tab == 7 && m_sub == 1) buildPage(7);
        });
    }

    static std::string clip(std::string s, size_t n) {
        if (s.size() > n) { s.resize(n - 2); s += ".."; }
        return s;
    }

    void search() {
        m_query = m_draft;
        m_pageNo = 1;
        fetchList();
        buildPage(7);
    }

    void gotoPage(int p) {
        if (p < 1 || p > m_pages || m_loading) return;
        m_pageNo = p;
        fetchList();
        buildPage(7);
    }

    void buildExplore() {
        if (!m_fetched) fetchList();

        m_search = TextInput::create(170.f, "Search name...", "bigFont.fnt");
        m_search->setScale(.7f);
        m_search->setMaxCharCount(32);
        m_search->setString(m_query);
        m_draft = m_query;
        m_search->setCallback([this](std::string const& s) { if (!m_building) m_draft = s; });
        m_page->addChild(m_search);
        m_search->setPosition(ccp(-138.f, 62.f));

        addButton("Search", .5f, ccp(-40.f, 62.f), [this] { search(); });
        addButton(m_sortTop ? "Top" : "New", .5f, ccp(10.f, 62.f), [this] {
            m_sortTop = !m_sortTop;
            m_pageNo = 1;
            fetchList();
            buildPage(7);
        });
        addButton("Post", .5f, ccp(160.f, 62.f), [this] {
            if (hub::userKey().empty()) {
                FLAlertLayer::create("Post", "Log in to your Geometry Dash account to post configs.", "OK")->show();
                return;
            }
            if (auto p = hub::ConfigPostPopup::create([] { ConfigPopup::reloadExplore(); })) p->show();
        });

        constexpr float w = 392.f, h = 112.f, rowH = 30.f, cy = -10.f;
        auto bg = CCLayerColor::create({0, 0, 0, 110}, w, h);
        bg->setPosition(ccp(-w / 2.f, cy - h / 2.f));
        m_page->addChild(bg);

        auto scroll = ScrollLayer::create({w, h});
        scroll->setPosition(ccp(-w / 2.f, cy - h / 2.f));
        m_page->addChild(scroll);

        float total = std::max(h, static_cast<float>(m_entries.size()) * rowH);
        scroll->m_contentLayer->setContentSize({w, total});

        std::string msg = m_loading ? "Loading..." : !m_error.empty() ? m_error : m_entries.empty() ? "No configs found" : "";
        if (!msg.empty()) {
            auto l = CCLabelBMFont::create(msg.c_str(), "chatFont.fnt");
            l->setScale(.6f);
            l->setPosition({w / 2.f, total - h / 2.f});
            scroll->m_contentLayer->addChild(l);
        }

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setContentSize({w, total});
        scroll->m_contentLayer->addChild(menu);
        float y = total - rowH / 2.f;
        for (auto const& e : m_entries) {
            auto name = CCLabelBMFont::create(clip(e.name, 28).c_str(), "bigFont.fnt");
            name->setScale(.38f);
            name->setAnchorPoint({0.f, .5f});
            name->setPosition({8.f, y + 5.f});
            scroll->m_contentLayer->addChild(name);
            auto meta = CCLabelBMFont::create(
                fmt::format("by {}  |  {} likes  |  {} dl", clip(e.author, 18), e.likes, e.downloads).c_str(), "chatFont.fnt");
            meta->setScale(.5f);
            meta->setAnchorPoint({0.f, .5f});
            meta->setOpacity(200);
            meta->setPosition({8.f, y - 8.f});
            scroll->m_contentLayer->addChild(meta);
            auto view = hub::button("View", .5f, [id = e.id, nm = e.name] {
                if (auto p = hub::ConfigViewPopup::create(id, nm, [] { ConfigPopup::reloadExplore(); })) p->show();
            });
            view->setPosition({w - 34.f, y});
            menu->addChild(view);
            y -= rowH;
        }
        scroll->m_contentLayer->setPositionY(h - total);        // first row in view

        auto bar = Scrollbar::create(scroll);
        bar->setPosition(ccp(w / 2.f + 8.f, cy));
        m_page->addChild(bar);

        addButton("<", .5f, ccp(-40.f, -84.f), [this] { gotoPage(m_pageNo - 1); });
        addButton(">", .5f, ccp(40.f, -84.f), [this] { gotoPage(m_pageNo + 1); });
        auto pg = CCLabelBMFont::create(fmt::format("Page {}/{}  ({} configs)", m_pageNo, m_pages, m_total).c_str(), "chatFont.fnt");
        pg->setScale(.5f);
        m_page->addChild(pg);
        pg->setPosition(ccp(0.f, -84.f));
        addNote(hub::playerName().empty() ? std::string("Not logged in (needed to post and like)") : "Logged in as " + hub::playerName(), -108.f);
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
        if (tab == 7) {
            buildConfigPage();
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
                toggle("Sub-tick click (CBF)", &c.subTick),
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
            items.push_back(number("Cube late rescue", &c.maxLate[M_CUBE], 0, 12));
            items.push_back(number("Ship late rescue", &c.maxLate[M_SHIP], 0, 12));
            items.push_back(number("Wave late rescue", &c.maxLate[M_WAVE], 0, 12));
            items.push_back(number("Robot late rescue", &c.maxLate[M_ROBOT], 0, 12));
            note = "Max delay: ticks an EARLY press / release may be held back (0 = mode off).\n"
                   "Late rescue: ticks a LATE press / release may be moved back in time (0 = off).";
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
            note = g_engine.timingInfo() + (g_engine.cbfActive() ? "\nClick Between Frames: active" : (Engine::cbfLoaded() ? "\nClick Between Frames: loaded (off)" : "\nClick Between Frames: not found")) +
                   "\n" + g_engine.orbInfo() +
                   "\nWave angles: ghost wave follows these slopes (4510 = 45.10 deg).";
        } else if (tab == 3) {
            items = {
                toggle("Late click rescue", &c.lateRescue),
                toggle("CBF support", &c.cbfSupport),
                number("Ship look (ticks)", &c.shipHorizon, 4, 300),
                number("Wave look (ticks)", &c.waveHorizon, 4, 300),
                number("Landing settle (ticks)", &c.settle, 1, 30),
                number("Air modes look (ticks)", &c.airHorizon, 10, 300),
                number("Robot long hold", &c.robotHold, 1, 60),
                number("Min budget (ticks)", &c.minBudget, 50, 5000),
                number("Noclip memory (ticks)", &c.noclipTicks, 0, 5000),
                number("Fail-open frames", &c.failOpen, 1, 60),
            };
            note = "Ship/Wave look: a hold/release must survive this many ticks.\n"
                   "If no delay saves it, a late rescue is tried, else the input goes through raw.\n"
                   "CBF support: with Click Between Frames on, it splits the steps (own sub-tick is off).";
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
    // called by the hub popups (no captured pointers: the window may be gone)
    static void refreshCurrent() {
        if (s_current && s_current->getParent()) s_current->buildPage(s_current->m_tab);
    }
    static void reloadExplore() {
        if (!s_current || !s_current->getParent()) return;
        s_current->m_fetched = false;
        s_current->m_pageNo = 1;
        if (s_current->m_tab == 7) s_current->buildPage(7);
    }

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