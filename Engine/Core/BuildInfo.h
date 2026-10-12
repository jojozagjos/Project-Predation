#pragma once

// Which build this is, stamped in at every build (cmake/PredBuildInfo.cmake): the version (bumped by hand as the game
// moves on), the build number (how many commits there are, with "+" when it was built with changes not yet committed)
// and the commit. Label() is what the game shows: "v0.2.0 build 314".
namespace pred::BuildInfo
{

const char* Version();
const char* Build();
const char* Commit();
const char* Label();

} // namespace pred::BuildInfo
