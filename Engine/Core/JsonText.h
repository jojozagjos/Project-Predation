#pragma once

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <string>
#include <vector>

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

// The keys of an object that are not among `known` -- almost always a typo. A misspelled key in a data
// file is otherwise simply ignored, and whatever it was meant to set silently keeps its default, which
// is the hardest kind of mistake to see. Keys starting with an underscore are comments and are never
// reported.
std::vector<std::string> UnknownKeys(const nlohmann::json& object, std::initializer_list<const char*> known);

} // namespace pred
