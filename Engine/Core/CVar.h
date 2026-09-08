#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pred
{

// Console variables: named, typed values that can be changed from config files,
// the command line, and the in-game console. Gameplay tuning that designers
// edit lives in data files; cvars are for engine and application settings.
enum class CVarFlags : uint32_t
{
    None = 0,
    Archive = 1u << 0,  // persisted to the user settings file
    ReadOnly = 1u << 1, // cannot be changed from console/config
    Cheat = 1u << 2,    // only settable in developer mode (enforced later)
    Hidden = 1u << 3,   // not listed by default
};

constexpr CVarFlags operator|(CVarFlags a, CVarFlags b)
{
    return static_cast<CVarFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

constexpr bool HasFlag(CVarFlags value, CVarFlags flag)
{
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

enum class CVarType : uint8_t
{
    Bool,
    Int,
    Float,
    String
};

const char* CVarTypeName(CVarType type);

class CVarBase
{
public:
    using ChangeCallback = std::function<void(CVarBase&)>;

    CVarBase(std::string_view name, std::string_view description, CVarFlags flags, CVarType type);
    virtual ~CVarBase();
    CVarBase(const CVarBase&) = delete;
    CVarBase& operator=(const CVarBase&) = delete;

    const std::string& Name() const { return m_name; }
    const std::string& Description() const { return m_description; }
    CVarFlags Flags() const { return m_flags; }
    CVarType Type() const { return m_type; }
    bool HasFlag(CVarFlags flag) const { return pred::HasFlag(m_flags, flag); }

    virtual std::string GetString() const = 0;
    virtual std::string GetDefaultString() const = 0;
    // Returns false if the text could not be parsed; the value is left unchanged.
    virtual bool SetFromString(std::string_view text) = 0;
    virtual void ResetToDefault() = 0;
    virtual bool IsDefault() const = 0;

    // Callbacks run synchronously on the thread that changed the value.
    void OnChange(ChangeCallback callback);
    void ClearOnChange() { m_callbacks.clear(); }

protected:
    void NotifyChanged();

    // Applies a value that arrived from config before this cvar existed.
    //
    // This MUST be called from the most-derived constructor's body, never from CVarBase's own
    // constructor: during base-class construction the object's dynamic type is still CVarBase, so a
    // virtual call would dispatch to the pure virtual SetFromString and abort the process.
    void ApplyPendingValue();

private:
    std::string m_name;
    std::string m_description;
    CVarFlags m_flags;
    CVarType m_type;
    std::vector<ChangeCallback> m_callbacks;
};

namespace detail
{
bool ParseCVarValue(std::string_view text, bool& out);
bool ParseCVarValue(std::string_view text, int& out);
bool ParseCVarValue(std::string_view text, float& out);
bool ParseCVarValue(std::string_view text, std::string& out);

std::string CVarValueToString(bool value);
std::string CVarValueToString(int value);
std::string CVarValueToString(float value);
std::string CVarValueToString(const std::string& value);

template <typename T>
constexpr CVarType CVarTypeOf();
template <>
constexpr CVarType CVarTypeOf<bool>()
{
    return CVarType::Bool;
}
template <>
constexpr CVarType CVarTypeOf<int>()
{
    return CVarType::Int;
}
template <>
constexpr CVarType CVarTypeOf<float>()
{
    return CVarType::Float;
}
template <>
constexpr CVarType CVarTypeOf<std::string>()
{
    return CVarType::String;
}
} // namespace detail

template <typename T>
class CVar final : public CVarBase
{
public:
    CVar(std::string_view name, T defaultValue, std::string_view description, CVarFlags flags = CVarFlags::None)
        : CVarBase(name, description, flags, detail::CVarTypeOf<T>()), m_value(defaultValue),
          m_default(std::move(defaultValue))
    {
        // Safe here: this object is fully constructed, so the virtual call resolves correctly.
        ApplyPendingValue();
    }

    const T& Get() const { return m_value; }
    operator const T&() const { return m_value; }

    void Set(T value)
    {
        if (value != m_value)
        {
            m_value = std::move(value);
            NotifyChanged();
        }
    }

    std::string GetString() const override { return detail::CVarValueToString(m_value); }
    std::string GetDefaultString() const override { return detail::CVarValueToString(m_default); }

    bool SetFromString(std::string_view text) override
    {
        T parsed{};
        if (!detail::ParseCVarValue(text, parsed))
        {
            return false;
        }
        Set(std::move(parsed));
        return true;
    }

    void ResetToDefault() override { Set(m_default); }
    bool IsDefault() const override { return m_value == m_default; }

private:
    T m_value;
    T m_default;
};

class CVarRegistry
{
public:
    enum class SetResult
    {
        Applied,
        Pending, // no cvar with that name yet; value stored and applied on registration
        ParseError,
        ReadOnly
    };

    static CVarRegistry& Instance();

    void Register(CVarBase& var);
    void Unregister(CVarBase& var);

    CVarBase* Find(std::string_view name) const;
    SetResult Set(std::string_view name, std::string_view value, bool allowReadOnly = false);

    // Removes and returns a value stored for a cvar that had not registered yet.
    bool TakePending(const std::string& name, std::string& outValue);

    std::vector<CVarBase*> All() const; // sorted by name
    size_t PendingCount() const;
    void ClearPending();

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, CVarBase*> m_vars;
    std::unordered_map<std::string, std::string> m_pending;
};

} // namespace pred
