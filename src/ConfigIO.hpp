#pragma once
#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
#include "Config.hpp"

// Import / export of the saved settings (the same keys saveConfig() writes).
// Everything coming from a file or the server goes through sanitize():
// only known keys survive and numbers are clamped to the ranges loadConfig() uses.

namespace buf::cfgio {

struct Key {
    std::string name;
    bool isBool;
    int lo;
    int hi;
};

inline std::vector<Key> const& keys() {
    static std::vector<Key> k = [] {
        std::vector<Key> v;
        for (char const* n : {"enabled", "showTraj", "logOn", "dualOn", "bothSides", "previewBoth", "showPath",
                              "orbDetect", "dropFatal", "twoPlayerOn", "moveDetect", "swapTwoP", "waveAnglesOn",
                              "autoplay", "autoLearn", "autoKeep"}) {
            v.push_back({n, true, 0, 0});
        }
        struct I { char const* n; int lo; int hi; };
        for (I i : {I{"previewLen", 10, 400}, I{"previewEvery", 1, 60}, I{"keepTicks", 0, 2000},
                    I{"lineWidth", 2, 40}, I{"pathHorizon", 20, 300}, I{"pathEvery", 1, 60},
                    I{"pathBudget", 200, 20000}, I{"trajOpacity", 10, 100}, I{"settle", 1, 30},
                    I{"airHorizon", 10, 300}, I{"robotHold", 1, 60}, I{"gainClick", 1, 60},
                    I{"minBudget", 50, 5000}, I{"noclipTicks", 0, 5000}, I{"failOpen", 1, 60},
                    I{"waveUp", 3000, 8000}, I{"waveDown", 3000, 8000}, I{"miniUp", 3000, 8500},
                    I{"miniDown", 3000, 8500}, I{"trajectory", 20, 400}, I{"shipImminent", 2, 120},
                    I{"waveImminent", 2, 120}, I{"holdGain", 1, 30}, I{"autoHorizon", 20, 300},
                    I{"autoEvery", 1, 30}, I{"autoPaths", 1, 12}, I{"autoStride", 1, 12},
                    I{"autoBudget", 200, 20000}, I{"autoCell", 2, 60}, I{"autoWindow", 5, 400},
                    I{"autoPrune", 1, 20}, I{"autoForget", 20, 2000}, I{"tps", 0, 10000},
                    I{"fps", 0, 1000}, I{"tapTicks", 1, 40}, I{"budget", 200, 40000},
                    I{"shipLook", 4, 300}, I{"waveLook", 4, 300}}) {
            v.push_back({i.n, false, i.lo, i.hi});
        }
        for (int m = 0; m < M_COUNT; ++m) v.push_back({"maxDelay" + std::to_string(m), false, 0, 64});
        return v;
    }();
    return k;
}

inline matjson::Value wrap(matjson::Value values) {
    auto root = matjson::Value::object();
    root["format"] = "buf-config";
    root["version"] = 1;
    root["values"] = std::move(values);
    return root;
}

// Current settings -> {"format","version","values":{...}}
inline matjson::Value exportJson() {
    saveConfig();
    auto* m = geode::Mod::get();
    auto values = matjson::Value::object();
    for (auto const& k : keys()) {
        if (k.isBool) values[k.name] = m->getSavedValue<bool>(k.name, false);
        else values[k.name] = m->getSavedValue<int64_t>(k.name, k.lo);
    }
    return wrap(std::move(values));
}

// Accepts a full file ({"values":{...}}) and returns a clean "values" object.
inline geode::Result<matjson::Value> sanitize(matjson::Value const& root) {
    if (!root.isObject() || !root.contains("values") || !root["values"].isObject()) {
        return geode::Err("Not a Dynamic Buffer config file");
    }
    auto const& vals = root["values"];
    auto out = matjson::Value::object();
    int n = 0;
    for (auto const& k : keys()) {
        if (!vals.contains(k.name)) continue;
        auto const& v = vals[k.name];
        if (k.isBool) {
            if (!v.isBool()) continue;
            out[k.name] = v.asBool().unwrapOr(false);
        } else {
            if (!v.isNumber()) continue;
            out[k.name] = std::clamp<int64_t>(v.asInt().unwrapOr(k.lo), k.lo, k.hi);
        }
        ++n;
    }
    if (n == 0) return geode::Err("No known settings in this file");
    return geode::Ok(std::move(out));
}

// Writes the sanitized values into the saved values and reloads cfg().
inline geode::Result<int> apply(matjson::Value const& root) {
    auto clean = sanitize(root);
    if (clean.isErr()) return geode::Err(clean.unwrapErr());
    auto values = std::move(clean).unwrap();
    auto* m = geode::Mod::get();
    int n = 0;
    for (auto const& k : keys()) {
        if (!values.contains(k.name)) continue;
        if (k.isBool) m->setSavedValue<bool>(k.name, values[k.name].asBool().unwrapOr(false));
        else m->setSavedValue<int64_t>(k.name, values[k.name].asInt().unwrapOr(k.lo));
        ++n;
    }
    m->setSavedValue<int64_t>("cfgVersion", 3);
    loadConfig();
    saveConfig();
    return geode::Ok(n);
}

inline std::filesystem::path configsDir() {
    auto dir = geode::Mod::get()->getSaveDir() / "configs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

inline geode::Result<matjson::Value> readFile(std::filesystem::path const& path) {
    auto text = geode::utils::file::readString(path);
    if (text.isErr()) return geode::Err(text.unwrapErr());
    auto json = matjson::Value::parse(text.unwrap());
    if (json.isErr()) return geode::Err("Not valid JSON");
    return geode::Ok(std::move(json).unwrap());
}

inline void backup() {
    (void)geode::utils::file::writeString(
        geode::Mod::get()->getSaveDir() / "backup-before-import.json", exportJson().dump(2)
    );
}

inline std::string safeName(std::string s) {
    for (auto& ch : s) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_') ch = '_';
    }
    if (s.size() > 40) s.resize(40);
    return s.empty() ? "config" : s;
}

} // namespace buf::cfgio
