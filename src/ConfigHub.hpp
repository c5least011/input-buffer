#pragma once
#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/TextArea.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/Scrollbar.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/cocos.hpp>
#include <chrono>
#include <functional>
#include <string>
#include "ConfigIO.hpp"

using namespace geode::prelude;

// Online config sharing: HTTP client + the "view" and "post / edit" popups.
// The backend is worker/ (Cloudflare Worker + D1).

namespace buf::hub {

inline std::string baseUrl() {
    return "https://buf-configs.c5leastdb.workers.dev";
}

inline std::string urlEncode(std::string const& s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

// token = the owner secret of a config (sent as x-token), empty for anonymous calls
inline web::WebFuture request(std::string method, std::string url, matjson::Value const* body = nullptr,
                              std::string const& token = "") {
    web::WebRequest r;
    r.userAgent("input-buffer-mod");
#if BUF_GEODE_V6
    r.timeout(asp::Duration::fromSecs(15));
#else
    r.timeout(std::chrono::seconds(15));
#endif
    if (!token.empty()) r.header("x-token", token);
    if (body) r.bodyJSON(*body);
    return r.send(std::move(method), std::move(url));
}
inline web::WebFuture get(std::string url, std::string const& token = "") {
    return request("GET", std::move(url), nullptr, token);
}
inline web::WebFuture post(std::string url, matjson::Value const& body) {
    return request("POST", std::move(url), &body);
}

// JSON body of a response, or the server's error message.
inline Result<matjson::Value> parse(web::WebResponse const& res) {
    std::string msg = res.code() <= 0 ? "Can't reach the server" : "HTTP " + std::to_string(res.code());
    auto json = res.json();
    if (json.isOk()) {
        auto v = std::move(json).unwrap();
        if (res.ok()) return Ok(std::move(v));
        if (v.isObject() && v.contains("error")) msg = v["error"].asString().unwrapOr(msg);
    }
    return Err(msg);
}

inline std::string js(matjson::Value const& v, char const* k, std::string d = "") {
    return v.isObject() && v.contains(k) ? v[k].asString().unwrapOr(d) : d;
}
inline int64_t ji(matjson::Value const& v, char const* k, int64_t d = 0) {
    return v.isObject() && v.contains(k) ? v[k].asInt().unwrapOr(d) : d;
}
inline bool jb(matjson::Value const& v, char const* k, bool d = false) {
    return v.isObject() && v.contains(k) ? v[k].asBool().unwrapOr(d) : d;
}

inline std::string playerName() { return std::string(GJAccountManager::get()->m_username); }
inline int accountID() { return GJAccountManager::get()->m_accountID; }
// Likes / posts are tied to the GD account (empty = not logged in).
inline std::string userKey() {
    int id = accountID();
    return id > 0 ? "a" + std::to_string(id) : "";
}

// Owner secrets of the configs this install posted: {"<id>": "<token>"}. Needed to edit / delete them.
inline std::string tokenFor(int id) {
    auto all = Mod::get()->getSavedValue<matjson::Value>("myTokens", matjson::Value::object());
    return js(all, std::to_string(id).c_str());
}
inline void saveToken(int id, std::string const& tok) {
    auto all = Mod::get()->getSavedValue<matjson::Value>("myTokens", matjson::Value::object());
    all[std::to_string(id)] = tok;
    Mod::get()->setSavedValue<matjson::Value>("myTokens", all);
}
inline void forgetToken(int id) {
    auto all = Mod::get()->getSavedValue<matjson::Value>("myTokens", matjson::Value::object());
    auto next = matjson::Value::object();
    if (all.isObject()) {
        for (auto& [key, val] : all) {
            if (key != std::to_string(id)) next[key] = val;
        }
    }
    Mod::get()->setSavedValue<matjson::Value>("myTokens", next);
}

inline CCMenuItemSpriteExtra* button(char const* text, float scale, std::function<void()> cb, ButtonSprite** out = nullptr) {
    auto spr = ButtonSprite::create(text, "bigFont.fnt", "GJ_button_05.png", .6f);
    spr->setScale(scale);
    if (out) *out = spr;
    return CCMenuItemExt::createSpriteExtra(spr, [cb = std::move(cb)](CCMenuItemSpriteExtra*) { cb(); });
}

inline CCLabelBMFont* label(CCNode* parent, std::string const& text, char const* font, float scale, CCPoint pos) {
    auto l = CCLabelBMFont::create(text.c_str(), font);
    l->setScale(scale);
    parent->addChild(l);
    l->setPosition(pos);
    return l;
}

// ---------------------------------------------------------------- post / edit popup

class ConfigPostPopup : public Popup {
protected:
    TextInput* m_name = nullptr;
    TextInput* m_desc = nullptr;
    CCLabelBMFont* m_fileLabel = nullptr;
    CCLabelBMFont* m_state = nullptr;
    matjson::Value m_values;
    bool m_hasFile = false;
    bool m_busy = false;
    int m_editId = 0;                       // 0 = new post
    std::function<void()> m_onDone;
    async::TaskHolder<file::PickResult> m_pick;
    async::TaskHolder<web::WebResponse> m_web;

    bool init(std::function<void()> onDone, int editId, std::string const& name, std::string const& desc) {
        if (!Popup::init(320.f, 230.f)) return false;
        m_noElasticity = true;
        m_onDone = std::move(onDone);
        m_editId = editId;
        setTitle(editId ? "Edit config" : "Publish config");

        m_name = TextInput::create(260.f, "Config name", "bigFont.fnt");
        m_name->setMaxCharCount(32);
        m_name->setString(name);
        m_mainLayer->addChildAtPosition(m_name, Anchor::Center, ccp(0.f, 50.f));
        m_desc = TextInput::create(260.f, "Description", "bigFont.fnt");
        m_desc->setMaxCharCount(200);
        m_desc->setString(desc);
        m_mainLayer->addChildAtPosition(m_desc, Anchor::Center, ccp(0.f, 14.f));

        m_fileLabel = CCLabelBMFont::create(editId ? "Settings unchanged (pick a file to replace them)" : "No file picked", "chatFont.fnt");
        m_fileLabel->setScale(.55f);
        m_mainLayer->addChildAtPosition(m_fileLabel, Anchor::Center, ccp(0.f, -45.f));
        m_state = CCLabelBMFont::create("", "chatFont.fnt");
        m_state->setScale(.5f);
        m_mainLayer->addChildAtPosition(m_state, Anchor::Center, ccp(0.f, -97.f));

        m_buttonMenu->addChildAtPosition(button("Pick file", .6f, [this] { pickFile(); }), Anchor::Center, ccp(-70.f, -22.f));
        m_buttonMenu->addChildAtPosition(button("Use current", .6f, [this] { useCurrent(); }), Anchor::Center, ccp(65.f, -22.f));
        m_buttonMenu->addChildAtPosition(button(editId ? "Save" : "Post", .7f, [this] { submit(); }), Anchor::Center, ccp(0.f, -72.f));
        return true;
    }

    void setState(std::string const& s) { m_state->setString(s.c_str()); }

    void setValues(matjson::Value values, std::string const& what) {
        m_values = std::move(values);
        m_hasFile = true;
        m_fileLabel->setString(what.c_str());
        setState("");
    }

    void useCurrent() {
        auto clean = cfgio::sanitize(cfgio::exportJson());
        if (clean.isOk()) setValues(std::move(clean).unwrap(), "Current settings");
    }

    void pickFile() {
        file::FilePickOptions opts;
        opts.defaultPath = cfgio::configsDir();
        opts.filters.push_back({"Config", {"*.json"}});
        m_pick.spawn(file::pick(file::PickMode::OpenFile, std::move(opts)), [this](file::PickResult r) {
            if (r.isErr()) { setState(fmt::format("{}", r.unwrapErr())); return; }
            if (!r.unwrap()) return;
            auto path = *r.unwrap();
            auto json = cfgio::readFile(path);
            if (json.isErr()) { setState(json.unwrapErr()); return; }
            auto clean = cfgio::sanitize(json.unwrap());
            if (clean.isErr()) { setState(clean.unwrapErr()); return; }
            setValues(std::move(clean).unwrap(), cfgio::fileNameStr(path));
        });
    }

    void submit() {
        if (m_busy) return;
        auto name = std::string(m_name->getString());
        if (name.size() < 3) { setState("Name must be at least 3 characters"); return; }
        if (!m_editId && !m_hasFile) { setState("Pick a file or use the current settings"); return; }
        if (userKey().empty()) { setState("Log in to your GD account"); return; }
        m_busy = true;
        setState(m_editId ? "Saving..." : "Posting...");
        auto body = matjson::Value::object();
        body["name"] = name;
        body["desc"] = std::string(m_desc->getString());
        if (m_hasFile) body["values"] = m_values;
        if (m_editId) {
            m_web.spawn(request("PUT", fmt::format("{}/configs/{}", baseUrl(), m_editId), &body, tokenFor(m_editId)),
                        [this](web::WebResponse res) { finished(res, "Config updated"); });
        } else {
            body["author"] = playerName();
            body["user"] = userKey();
            m_web.spawn(post(baseUrl() + "/configs", body), [this](web::WebResponse res) {
                if (res.ok()) {
                    auto j = res.json();
                    if (j.isOk()) saveToken(static_cast<int>(ji(j.unwrap(), "id")), js(j.unwrap(), "token"));
                }
                finished(res, "Config posted");
            });
        }
    }

    void finished(web::WebResponse const& res, char const* ok) {
        m_busy = false;
        auto j = parse(res);
        if (j.isErr()) { setState(j.unwrapErr()); return; }
        Notification::create(ok, NotificationIcon::Success)->show();
        if (m_onDone) m_onDone();
        onClose(nullptr);
    }

public:
    static ConfigPostPopup* create(std::function<void()> onDone, int editId = 0, std::string name = "", std::string desc = "") {
        auto ret = new ConfigPostPopup();
        if (ret->init(std::move(onDone), editId, name, desc)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------- view popup

class ConfigViewPopup : public Popup {
protected:
    int m_id = 0;
    std::string m_name, m_author, m_desc;
    int64_t m_likes = 0, m_downloads = 0;
    bool m_liked = false, m_mine = false, m_loaded = false, m_busy = false;
    matjson::Value m_values;
    std::function<void()> m_onChanged;
    async::TaskHolder<web::WebResponse> m_info, m_likeTask, m_dlTask, m_delTask;
    CCLabelBMFont* m_meta = nullptr;
    CCLabelBMFont* m_state = nullptr;
    ButtonSprite* m_likeSpr = nullptr;
    CCNode* m_body = nullptr;

    bool init(int id, std::string name, std::function<void()> onChanged) {
        if (!Popup::init(360.f, 270.f)) return false;
        m_noElasticity = true;
        m_id = id;
        m_name = std::move(name);
        m_onChanged = std::move(onChanged);
        setTitle(m_name.c_str());

        m_body = CCNode::create();
        m_mainLayer->addChildAtPosition(m_body, Anchor::Center, ccp(0.f, 0.f));
        m_meta = label(m_body, "Loading...", "chatFont.fnt", .6f, ccp(0.f, 88.f));
        m_state = label(m_body, "", "chatFont.fnt", .5f, ccp(0.f, -108.f));

        auto url = fmt::format("{}/configs/{}?user={}", baseUrl(), m_id, urlEncode(userKey()));
        m_info.spawn(get(url, tokenFor(m_id)), [this](web::WebResponse res) {
            auto j = parse(res);
            if (j.isErr()) { m_meta->setString("Failed to load"); setState(j.unwrapErr()); return; }
            auto const& v = j.unwrap();
            auto clean = cfgio::sanitize(v);                       // never trust the server's JSON
            if (clean.isErr()) { m_meta->setString("Invalid config"); setState(clean.unwrapErr()); return; }
            m_values = std::move(clean).unwrap();
            m_author = js(v, "author", "?");
            m_desc = js(v, "desc");
            m_likes = ji(v, "likes");
            m_downloads = ji(v, "downloads");
            m_liked = jb(v, "liked");
            m_mine = jb(v, "mine");
            m_loaded = true;
            buildContent();
        });
        return true;
    }

    void setState(std::string const& s) { m_state->setString(s.c_str()); }

    void buildContent() {
        auto area = SimpleTextArea::create(m_desc.empty() ? "(no description)" : m_desc, "chatFont.fnt", .55f, 320.f);
        area->setAnchorPoint({.5f, 1.f});
        m_body->addChild(area);
        area->setPosition(ccp(0.f, 74.f));

        // preview: exactly which settings this config changes, so nothing is a surprise
        auto pv = cfgio::preview(m_values);
        constexpr float w = 320.f, h = 76.f, cy = -14.f, lineH = 12.f;
        std::string head = pv.changed.empty()
            ? fmt::format("Preview: same as the defaults ({} settings)", pv.total)
            : fmt::format("Preview: {} of {} settings differ from the defaults", pv.changed.size(), pv.total);
        label(m_body, head, "chatFont.fnt", .5f, ccp(0.f, cy + h / 2.f + 9.f));

        auto bg = CCLayerColor::create({0, 0, 0, 110}, w, h);
        bg->setPosition(ccp(-w / 2.f, cy - h / 2.f));
        m_body->addChild(bg);
        auto scroll = ScrollLayer::create({w, h});
        scroll->setPosition(ccp(-w / 2.f, cy - h / 2.f));
        m_body->addChild(scroll);
        float total = std::max(h, static_cast<float>(pv.changed.size()) * lineH + 6.f);
        scroll->m_contentLayer->setContentSize({w, total});
        float y = total - 3.f;
        for (auto const& d : pv.changed) {
            auto l = CCLabelBMFont::create(fmt::format("{}: {}   (default {})", d.key, d.value, d.def).c_str(), "chatFont.fnt");
            l->setScale(.5f);
            l->setAnchorPoint({0.f, 1.f});
            l->setPosition({6.f, y});
            scroll->m_contentLayer->addChild(l);
            y -= lineH;
        }
        scroll->m_contentLayer->setPositionY(h - total);
        auto bar = Scrollbar::create(scroll);
        bar->setPosition(ccp(w / 2.f + 8.f, cy));
        m_body->addChild(bar);

        // buttons
        std::vector<CCMenuItemSpriteExtra*> btns;
        btns.push_back(button("Download", .55f, [this] { download(); }));
        btns.push_back(button("Like", .55f, [this] { toggleLike(); }, &m_likeSpr));
        if (m_mine) {
            btns.push_back(button("Edit", .55f, [this] { edit(); }));
            btns.push_back(button("Delete", .55f, [this] { confirmDelete(); }));
        }
        float step = 85.f, x = -step * (btns.size() - 1) / 2.f;
        for (auto* b : btns) {
            m_buttonMenu->addChildAtPosition(b, Anchor::Center, ccp(x, -84.f));
            x += step;
        }
        refreshMeta();
    }

    void refreshMeta() {
        m_meta->setString(fmt::format("by {}   |   {} likes   |   {} downloads", m_author, m_likes, m_downloads).c_str());
        if (m_likeSpr) m_likeSpr->setString(m_liked ? "Unlike" : "Like");
    }

    void toggleLike() {
        if (!m_loaded || m_busy) return;
        if (userKey().empty()) { setState("Log in to your GD account to like"); return; }
        m_busy = true;
        auto body = matjson::Value::object();
        body["user"] = userKey();
        m_likeTask.spawn(post(fmt::format("{}/configs/{}/like", baseUrl(), m_id), body), [this](web::WebResponse res) {
            m_busy = false;
            auto j = parse(res);
            if (j.isErr()) { setState(j.unwrapErr()); return; }
            m_likes = ji(j.unwrap(), "likes", m_likes);
            m_liked = jb(j.unwrap(), "liked", m_liked);
            setState("");
            refreshMeta();
        });
    }

    void download() {
        if (!m_loaded || m_busy) return;
        auto root = cfgio::wrap(m_values);                        // what the preview showed is what gets saved
        auto path = cfgio::configsDir() / fmt::format("{}-{}.json", m_id, cfgio::safeName(m_name));
        auto w = file::writeString(path, root.dump(2));
        setState(w.isOk() ? "Saved: " + cfgio::fileNameStr(path) : "Couldn't save the file");
        auto pv = cfgio::preview(m_values);
        createQuickPopup(
            "Apply config?",
            fmt::format("{} settings differ from the defaults.\nYour current settings are backed up first.", pv.changed.size()),
            "Later", "Apply", [root, cb = m_onChanged](FLAlertLayer*, bool apply) {
                if (!apply) return;
                cfgio::backup();
                auto r = cfgio::apply(root);
                if (r.isErr()) { FLAlertLayer::create("Error", r.unwrapErr(), "OK")->show(); return; }
                if (cb) cb();
                Notification::create("Config applied", NotificationIcon::Success)->show();
            }
        );
        // count the download (fire and forget, the result isn't needed)
        m_dlTask.spawn(get(fmt::format("{}/configs/{}/download", baseUrl(), m_id)), [this](web::WebResponse res) {
            if (res.ok()) { ++m_downloads; refreshMeta(); }
        });
    }

    void edit() {
        auto self = Ref<ConfigViewPopup>(this);
        if (auto p = ConfigPostPopup::create([self] {
                if (self->m_onChanged) self->m_onChanged();
                self->onClose(nullptr);
            }, m_id, m_name, m_desc)) p->show();
    }

    void confirmDelete() {
        auto self = Ref<ConfigViewPopup>(this);
        createQuickPopup("Delete config", fmt::format("Delete \"{}\" for everyone?", m_name), "Cancel", "Delete",
                         [self](FLAlertLayer*, bool yes) { if (yes) self->doDelete(); });
    }

    void doDelete() {
        if (m_busy) return;
        m_busy = true;
        setState("Deleting...");
        m_delTask.spawn(request("DELETE", fmt::format("{}/configs/{}", baseUrl(), m_id), nullptr, tokenFor(m_id)),
                        [this](web::WebResponse res) {
            m_busy = false;
            auto j = parse(res);
            if (j.isErr()) { setState(j.unwrapErr()); return; }
            forgetToken(m_id);
            Notification::create("Config deleted", NotificationIcon::Success)->show();
            if (m_onChanged) m_onChanged();
            onClose(nullptr);
        });
    }

public:
    static ConfigViewPopup* create(int id, std::string name, std::function<void()> onChanged) {
        auto ret = new ConfigViewPopup();
        if (ret->init(id, std::move(name), std::move(onChanged))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

} // namespace buf::hub
