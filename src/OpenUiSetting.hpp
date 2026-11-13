#pragma once
#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include "ConfigPopup.hpp"

// A "setting" that is only a button: it opens the mod's own settings window from
// Geode's mod settings page (so the window is reachable even when the pause
// menu button is turned off).

namespace buf {

using namespace geode::prelude;

class OpenUiSetting final : public SettingBaseValueV3<bool> {
public:
    static Result<std::shared_ptr<SettingV3>> parse(
        std::string const& key,
        std::string const& modID,
        matjson::Value const& json
    ) {
        auto setting = std::make_shared<OpenUiSetting>();
        auto root = checkJson(json, "OpenUiSetting");
        setting->parseBaseProperties(key, modID, root);
        root.checkUnknownKeys();
        return root.ok(std::static_pointer_cast<SettingV3>(setting));
    }

    SettingNodeV3* createNode(float width) override;
};

class OpenUiSettingNode final : public SettingNodeV3 {
protected:
    bool init(std::shared_ptr<OpenUiSetting> setting, float width) {
        if (!SettingNodeV3::init(setting, width)) return false;

        auto sprite = ButtonSprite::create("Open", "bigFont.fnt", "GJ_button_05.png", .7f);
        sprite->setScale(.65f);
        auto button = CCMenuItemSpriteExtra::create(
            sprite, this, menu_selector(OpenUiSettingNode::onOpen)
        );
        getButtonMenu()->addChildAtPosition(button, Anchor::Center);
        getButtonMenu()->setContentWidth(90.f);
        updateState(nullptr);
        return true;
    }

    void updateState(CCNode* invoker) override {
        SettingNodeV3::updateState(invoker);
        if (auto buttonMenu = getButtonMenu()) {
            buttonMenu->setEnabled(getSetting()->shouldEnable());
        }
    }

    void onCommit() override {}

    void onOpen(CCObject*) {
        if (auto popup = ConfigPopup::create()) popup->show();
    }

public:
    static OpenUiSettingNode* create(std::shared_ptr<OpenUiSetting> setting, float width) {
        auto ret = new OpenUiSettingNode();
        if (ret->init(std::move(setting), width)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool hasUncommittedChanges() const override { return false; }
    bool hasNonDefaultValue() const override { return false; }
    void onResetToDefault() override {}
};

inline SettingNodeV3* OpenUiSetting::createNode(float width) {
    auto setting = std::static_pointer_cast<OpenUiSetting>(shared_from_this());
    return OpenUiSettingNode::create(std::move(setting), width);
}

} // namespace buf
