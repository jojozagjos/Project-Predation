#include "Engine/Core/JsonText.h"

#include <cmath>
#include <cstdio>

namespace pred
{
namespace
{

std::string Number(double value, int decimals)
{
    if (!std::isfinite(value))
    {
        return "0";
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    std::string out = text;
    // No trailing zeros, and no point with nothing after it; a whole number stays a float to the eye
    // with one decimal place, so a size of 1.0 does not read back as an integer to anybody editing it.
    if (out.find('.') != std::string::npos)
    {
        while (!out.empty() && out.back() == '0')
        {
            out.pop_back();
        }
        if (!out.empty() && out.back() == '.')
        {
            out += '0';
        }
    }
    if (out == "-0.0")
    {
        out = "0.0";
    }
    return out;
}

bool NumbersOnly(const nlohmann::json& array)
{
    if (!array.is_array() || array.empty() || array.size() > 12)
    {
        return false;
    }
    for (const nlohmann::json& item : array)
    {
        if (!item.is_number())
        {
            return false;
        }
    }
    return true;
}

void Write(const nlohmann::json& value, int decimals, int depth, std::string& out)
{
    const std::string indent(static_cast<size_t>(depth) * 2, ' ');
    const std::string inner(static_cast<size_t>(depth + 1) * 2, ' ');
    switch (value.type())
    {
    case nlohmann::json::value_t::number_float:
        out += Number(value.get<double>(), decimals);
        return;
    case nlohmann::json::value_t::array:
    {
        if (value.empty())
        {
            out += "[]";
            return;
        }
        if (NumbersOnly(value))
        {
            out += '[';
            for (size_t i = 0; i < value.size(); ++i)
            {
                if (i > 0)
                {
                    out += ", ";
                }
                Write(value[i], decimals, depth + 1, out);
            }
            out += ']';
            return;
        }
        out += "[\n";
        for (size_t i = 0; i < value.size(); ++i)
        {
            out += inner;
            Write(value[i], decimals, depth + 1, out);
            out += i + 1 < value.size() ? ",\n" : "\n";
        }
        out += indent + "]";
        return;
    }
    case nlohmann::json::value_t::object:
    {
        if (value.empty())
        {
            out += "{}";
            return;
        }
        out += "{\n";
        size_t i = 0;
        for (auto it = value.begin(); it != value.end(); ++it, ++i)
        {
            out += inner + nlohmann::json(it.key()).dump() + ": ";
            Write(it.value(), decimals, depth + 1, out);
            out += i + 1 < value.size() ? ",\n" : "\n";
        }
        out += indent + "}";
        return;
    }
    default:
        out += value.dump();
        return;
    }
}

} // namespace

std::string JsonText(const nlohmann::json& value, int decimals)
{
    std::string out;
    Write(value, decimals, 0, out);
    out += '\n';
    return out;
}

} // namespace pred
