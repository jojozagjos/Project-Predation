#include "Engine/Core/CVar.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace pred
{

const char* CVarTypeName(CVarType type)
{
    switch (type)
    {
    case CVarType::Bool:
        return "bool";
    case CVarType::Int:
        return "int";
    case CVarType::Float:
        return "float";
    case CVarType::String:
        return "string";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// CVarBase
// ---------------------------------------------------------------------------

CVarBase::CVarBase(std::string_view name, std::string_view description, CVarFlags flags, CVarType type)
    : m_name(name), m_description(description), m_flags(flags), m_type(type)
{
    CVarRegistry::Instance().Register(*this);
}

CVarBase::~CVarBase()
{
    CVarRegistry::Instance().Unregister(*this);
}

void CVarBase::OnChange(ChangeCallback callback)
{
    m_callbacks.push_back(std::move(callback));
}

void CVarBase::NotifyChanged()
{
    for (auto& callback : m_callbacks)
    {
        callback(*this);
    }
}

// ---------------------------------------------------------------------------
// Parsing / formatting
// ---------------------------------------------------------------------------

namespace detail
{

namespace
{
std::string_view Trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.remove_suffix(1);
    }
    return text;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}
} // namespace

bool ParseCVarValue(std::string_view text, bool& out)
{
    text = Trim(text);
    if (EqualsIgnoreCase(text, "true") || EqualsIgnoreCase(text, "on") || EqualsIgnoreCase(text, "yes") || text == "1")
    {
        out = true;
        return true;
    }
    if (EqualsIgnoreCase(text, "false") || EqualsIgnoreCase(text, "off") || EqualsIgnoreCase(text, "no") || text == "0")
    {
        out = false;
        return true;
    }
    return false;
}

bool ParseCVarValue(std::string_view text, int& out)
{
    text = Trim(text);
    int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size())
    {
        // Allow "12.0" style input for ints by parsing as float and truncating exact integers.
        float asFloat = 0.0f;
        if (!ParseCVarValue(text, asFloat) || asFloat != static_cast<float>(static_cast<int>(asFloat)))
        {
            return false;
        }
        value = static_cast<int>(asFloat);
    }
    out = value;
    return true;
}

bool ParseCVarValue(std::string_view text, float& out)
{
    text = Trim(text);
    float value = 0.0f;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size())
    {
        return false;
    }
    out = value;
    return true;
}

bool ParseCVarValue(std::string_view text, std::string& out)
{
    out.assign(text.begin(), text.end());
    return true;
}

std::string CVarValueToString(bool value)
{
    return value ? "true" : "false";
}

std::string CVarValueToString(int value)
{
    return std::to_string(value);
}

std::string CVarValueToString(float value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
    return buffer;
}

std::string CVarValueToString(const std::string& value)
{
    return value;
}

} // namespace detail

// ---------------------------------------------------------------------------
// CVarRegistry
// ---------------------------------------------------------------------------

CVarRegistry& CVarRegistry::Instance()
{
    static CVarRegistry registry;
    return registry;
}

void CVarRegistry::Register(CVarBase& var)
{
    std::string pendingValue;
    bool hasPending = false;
    {
        std::lock_guard lock(m_mutex);
        m_vars[var.Name()] = &var;
        const auto it = m_pending.find(var.Name());
        if (it != m_pending.end())
        {
            pendingValue = it->second;
            hasPending = true;
            m_pending.erase(it);
        }
    }
    if (hasPending)
    {
        // Applied outside the lock: change callbacks may query the registry.
        var.SetFromString(pendingValue);
    }
}

void CVarRegistry::Unregister(CVarBase& var)
{
    std::lock_guard lock(m_mutex);
    const auto it = m_vars.find(var.Name());
    if (it != m_vars.end() && it->second == &var)
    {
        m_vars.erase(it);
    }
}

CVarBase* CVarRegistry::Find(std::string_view name) const
{
    std::lock_guard lock(m_mutex);
    const auto it = m_vars.find(std::string(name));
    return it == m_vars.end() ? nullptr : it->second;
}

CVarRegistry::SetResult CVarRegistry::Set(std::string_view name, std::string_view value, bool allowReadOnly)
{
    CVarBase* var = nullptr;
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_vars.find(std::string(name));
        if (it == m_vars.end())
        {
            m_pending[std::string(name)] = std::string(value);
            return SetResult::Pending;
        }
        var = it->second;
    }
    if (var->HasFlag(CVarFlags::ReadOnly) && !allowReadOnly)
    {
        return SetResult::ReadOnly;
    }
    return var->SetFromString(value) ? SetResult::Applied : SetResult::ParseError;
}

std::vector<CVarBase*> CVarRegistry::All() const
{
    std::vector<CVarBase*> result;
    {
        std::lock_guard lock(m_mutex);
        result.reserve(m_vars.size());
        for (const auto& [name, var] : m_vars)
        {
            result.push_back(var);
        }
    }
    std::sort(result.begin(), result.end(), [](const CVarBase* a, const CVarBase* b) { return a->Name() < b->Name(); });
    return result;
}

size_t CVarRegistry::PendingCount() const
{
    std::lock_guard lock(m_mutex);
    return m_pending.size();
}

void CVarRegistry::ClearPending()
{
    std::lock_guard lock(m_mutex);
    m_pending.clear();
}

} // namespace pred
