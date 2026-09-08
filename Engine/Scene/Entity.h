#pragma once

#include <cstdint>
#include <functional>

namespace pred
{

// A generational handle to a scene entity.
//
// `index` addresses a slot in the scene's arrays; `generation` is bumped every time that slot is
// reused. Holding a stale Entity is therefore safe: the generation no longer matches and every
// lookup returns null instead of silently pointing at a different object. Generation 0 means "never
// used", so a default-constructed Entity is always invalid.
struct Entity
{
    uint32_t index = 0;
    uint32_t generation = 0;

    bool IsValid() const { return generation != 0; }
    bool operator==(const Entity& other) const = default;

    uint64_t Key() const { return (static_cast<uint64_t>(generation) << 32) | index; }
};

} // namespace pred

template <>
struct std::hash<pred::Entity>
{
    size_t operator()(const pred::Entity& entity) const noexcept
    {
        return std::hash<uint64_t>{}(entity.Key());
    }
};
