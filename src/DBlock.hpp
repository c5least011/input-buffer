#pragma once
// D / J / S / H blocks and collision blocks: strict detection by object ID (not by object type).
//
// These are the special hitbox blocks at the end of the orbs tab of the editor:
//   D (1755), J (1813), S (1829), H (1859)      + the collision block (1816)
// Their effect (wave slides on them, no death on a ceiling, a dash stops, ...) is done by the game's
// own collision code, but only for objects that are IN the list that code is given. The ghost's
// object filter used to go by object type and let them through only by luck, so they are now matched
// by ID, whatever type the game gave them. Used by main.cpp (ghost object filter) and Engine.hpp.
#include <Geode/Geode.hpp>

using namespace geode::prelude;

namespace buf {

inline constexpr int kDBlockId = 1755;
inline constexpr int kJBlockId = 1813;
inline constexpr int kSBlockId = 1829;
inline constexpr int kHBlockId = 1859;
inline constexpr int kCollisionBlockId = 1816;

inline bool isDBlock(GameObject* o) { return o && o->m_objectID == kDBlockId; }

// D, J, S or H
inline bool isSpecialBlock(GameObject* o) {
    if (!o) return false;
    int id = o->m_objectID;
    return id == kDBlockId || id == kJBlockId || id == kSBlockId || id == kHBlockId;
}

// collision block with the "Dynamic Block" option: solid, may be moved by triggers
inline bool isDynamicCollisionBlock(GameObject* o) {
    if (!o || o->m_objectID != kCollisionBlockId) return false;
    auto* eo = geode::cast::typeinfo_cast<EffectGameObject*>(o);
    return eo && eo->m_isDynamicBlock;
}

} // namespace buf
