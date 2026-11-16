#pragma once
#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/TextArea.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/cocos.hpp>
#include <chrono>
#include <functional>
#include <string>
#include "ConfigIO.hpp"

using namespace geode::prelude;

// Online config sharing: HTTP client + the "view" and "post" popups.
// The server is server/server.js (plain Node, no dependencies).

namespace buf::hub {

inline std::string baseUrl() {
    auto s = Mod::get()->getSettingValue<std::string>("server-url");
    while (!s.empty() && (s.back() == '/' || s.back() == ' ')) s.pop_back();
    return s;
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

inline web::WebFuture get(std::string url) {
    web::WebRequest r;
    r.userAgent("input-buffer-mod");
    r.timeout(std::chrono::seconds(15));
    return r.get(std::move(url));
}

inline web::WebFuture post(std::string url, matjson::Value const& body) {
    web::WebRequest r;
    r.userAgent("input-buffer-mod");
    r.timeout(std::chrono::seconds(15));
    r.bodyJSON(body);
    return r.post(std::move(url));
}

// JSON body of a response, or the server's error message.
inline Result<matjson::Value> parse(web::WebResponse const& res) {
    std::string msg = res.code() == 0 ? "Can't reach the server" : "HTTP " + std::to_string(res.code());
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

inline std::string playerName() {
    return std::string(GJAccountManager::get()->m_username);
}
inline int accountID() {
    return GJAccountManager::get()->m_accountID;
}
// Likes / posts are tied to the GD account (0 = not logged in).
inline std::string userKey() {
    int id = accountID();
    return id > 0 ? "a" + std::to_string(id) : "";
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

// ---------------------------------------------------------------- view popup

class ConfigViewPopup : public Popup {
protected:
    int m_id = 0;
    std::string m_name, m_author, m_desc;
    int64_t m_likes = 0, m_downloads = 0;
    bool m_liked = false;
    bool m_loaded = false;
    bool m_busy = false;
    std::function<void()> m_onApplied;
    async::TaskHolder<web::WebResponse> m_info, m_likeTask, m_dlTask;
    CCLabelBMFont* m_meta = nullptr;
    CCLabelBMFont* m_state = nullptr;
    ButtonSprite* m_likeSpr = nullptr;
    CCNode* m_body = nullptr;

    bool init(int id, std::string name, std::function<void()> onApplied) {
        if (!Popup::init(320.f, 220.f)) return false;
        m_noElasticity = true;
        m_id = id;
        m_name = std::move(name);
        m_onApplied = std::move(onApplied);
        setTitle(m_name.c_str());

        m_body = CCNode::create();
        m_mainLayer->addChildAtPosition(m_body, Anchor::Center, ccp(0.f, 0.f));
        m_meta = label(m_body, "Loading...", "chatFont.fnt", .6f, ccp(0.f, 62.f));
        m_state = label(m_body, "", "chatFont.fnt", .5f, ccp(0.f, -92.f));

        auto dl = button("Download", .6f, [this] { download(); });
        m_buttonMenu->addChildAtPosition(dl, Anchor::Center, ccp(-65.f, -66.f));
        auto like = button("Like", .6f, [this] { toggleLike(); }, &m_likeSpr);
        m_buttonMenu->addChildAtPosition(like, Anchor::Center, ccp(65.f, -66.f));

        auto url = fmt::format("{}/configs/{}?user={}", baseUrl(), m_id, urlEncode(userKey()));
        m_info.spawn(get(url), [this](web::WebResponse res) {
            auto j = parse(res);
            if (j.isErr()) { m_meta->setString("Failed to load"); setState(j.unwrapErr()); return; }
            auto const& v = j.unwrap();
            m_author = js(v, "author", "?");
            m_desc = js(v, "desc");
            m_likes = ji(v, "likes");
            m_downloads = ji(v, "downloads");
            m_liked = jb(v, "liked");
            m_loaded = true;
            auto area = SimpleTextArea::create(m_desc.empty() ? "(no description)" : m_desc, "chatFont.fnt", .6f, 270.f);
            area->setAnchorPoint({.5f, 1.f});
            m_body->addChild(area);
            area->setPosition(ccp(0.f, 40.f));
            refreshMeta();
        });
        return true;
    }

    void setState(std::string const& s) { m_state->setString(s.c_str()); }

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
        m_busy = true;
        setState("Downloading...");
        m_dlTask.spawn(get(fmt::format("{}/configs/{}/download", baseUrl(), m_id)), [this](web::WebResponse res) {
            m_busy = false;
            auto j = parse(res);
            if (j.isErr()) { setState(j.unwrapErr()); return; }
            auto clean = cfgio::sanitize(j.unwrap());   // never trust the server's JSON
            if (clean.isErr()) { setState(clean.unwrapErr()); return; }
            auto root = cfgio::wrap(std::move(clean).unwrap());
            auto path = cfgio::configsDir() / fmt::format("{}-{}.json", m_id, cfgio::safeName(m_name));
            auto w = file::writeString(path, root.dump(2));
            ++m_downloads;
            refreshMeta();
            setState(w.isOk() ? "Saved: " + path.filename().string() : "Couldn't save the file");
            createQuickPopup(
                "Config downloaded", "Apply it now?\nYour current settings are backed up first.",
                "Later", "Apply", [root, cb = m_onApplied](FLAlertLayer*, bool apply) {
                    if (!apply) return;
                    cfgio::backup();
                    auto r = cfgio::apply(root);
                    if (r.isErr()) { FLAlertLayer::create("Error", r.unwrapErr(), "OK")->show(); return; }
                    if (cb) cb();
                    Notification::create("Config applied", NotificationIcon::Success)->show();
                }
            );
        });
    }

public:
    static ConfigViewPopup* create(int id, std::string name, std::function<void()> onApplied) {
        auto ret = new ConfigViewPopup();
        if (ret->init(id, std::move(name), std::move(onApplied))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------- post popup

class ConfigPostPopup : public Popup {
protected:
    TextInput* m_name = nullptr;
    TextInput* m_desc = nullptr;
    CCLabelBMFont* m_fileLabel = nullptr;
    CCLabelBMFont* m_state = nullptr;
    matjson::Value m_values;
    bool m_hasFile = false;
    bool m_busy = false;
    std::function<void()> m_onDone;
    async::TaskHolder<file::PickResult> m_pick;
    async::TaskHolder<web::WebResponse> m_web;

    bool init(std::function<void()> onDone) {
        if (!Popup::init(320.f, 230.f)) return false;
        m_noElasticity = true;
        m_onDone = std::move(onDone);
        setTitle("Publish config");

        m_name = TextInput::create(260.f, "Config name", "bigFont.fnt");
        m_name->setMaxCharCount(32);
        m_mainLayer->addChildAtPosition(m_name, Anchor::Center, ccp(0.f, 50.f));
        m_desc = TextInput::create(260.f, "Description", "bigFont.fnt");
        m_desc->setMaxCharCount(200);
        m_mainLayer->addChildAtPosition(m_desc, Anchor::Center, ccp(0.f, 14.f));

        m_fileLabel = CCLabelBMFont::create("No file picked", "chatFont.fnt");
        m_fileLabel->setScale(.55f);
        m_mainLayer->addChildAtPosition(m_fileLabel, Anchor::Center, ccp(0.f, -45.f));
        m_state = CCLabelBMFont::create("", "chatFont.fnt");
        m_state->setScale(.5f);
        m_mainLayer->addChildAtPosition(m_state, Anchor::Center, ccp(0.f, -97.f));

        m_buttonMenu->addChildAtPosition(button("Pick file", .6f, [this] { pickFile(); }), Anchor::Center, ccp(-70.f, -22.f));
        m_buttonMenu->addChildAtPosition(button("Use current", .6f, [this] { useCurrent(); }), Anchor::Center, ccp(65.f, -22.f));
        m_buttonMenu->addChildAtPosition(button("Post", .7f, [this] { submit(); }), Anchor::Center, ccp(0.f, -72.f));
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
            if (r.isErr()) { setState(r.unwrapErr()); return; }
            if (!r.unwrap()) return;
            auto path = *r.unwrap();
            auto json = cfgio::readFile(path);
            if (json.isErr()) { setState(json.unwrapErr()); return; }
            auto clean = cfgio::sanitize(json.unwrap());
            if (clean.isErr()) { setState(clean.unwrapErr()); return; }
            setValues(std::move(clean).unwrap(), path.filename().string());
        });
    }

    void submit() {
        if (m_busy) return;
        auto name = std::string(m_name->getString());
        if (name.size() < 3) { setState("Name must be at least 3 characters"); return; }
        if (!m_hasFile) { setState("Pick a file or use the current settings"); return; }
        if (userKey().empty()) { setState("Log in to your GD account to post"); return; }
        m_busy = true;
        setState("Posting...");
        auto body = matjson::Value::object();
        body["name"] = name;
        body["desc"] = std::string(m_desc->getString());
        body["author"] = playerName();
        body["user"] = userKey();
        body["values"] = m_values;
        m_web.spawn(post(baseUrl() + "/configs", body), [this](web::WebResponse res) {
            m_busy = false;
            auto j = parse(res);
            if (j.isErr()) { setState(j.unwrapErr()); return; }
            Notification::create("Config posted", NotificationIcon::Success)->show();
            if (m_onDone) m_onDone();
            onClose(nullptr);
        });
    }

public:
    static ConfigPostPopup* create(std::function<void()> onDone) {
        auto ret = new ConfigPostPopup();
        if (ret->init(std::move(onDone))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

} // namespace buf::hub