#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace pred
{

// A data file as a person would write it.
//
// nlohmann's own dump writes a float the way it is stored -- a hold offset of -0.106 comes out as
// -0.10599999874830246 -- and every number of a colour or a position on a line of its own, so a
// three-number vector is five lines. The files in Assets are meant to be read and edited by hand,
// and a file the game has saved once should not be harder to read than the one it loaded.
//
// So: numbers rounded to `decimals` places with the trailing zeros taken off, and an array holding
// nothing but numbers (and not too many of them) kept on one line. Everything else is laid out as
// dump(2) would. Keys keep the order the object gives them.
std::string JsonText(const nlohmann::json& value, int decimals = 4);

} // namespace pred
