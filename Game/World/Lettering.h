#pragma once

#include "Engine/Render/Mesh.h"

#include <string>

namespace pred
{

// Words drawn on things -- a door's plate, a floor's number, a way out, a building's name: a plain block letter five dots wide
// and seven high, each row's run of dots one flat piece, facing out of the thing's front (its -z, as everything here faces),
// centred on its origin and reading left to right to somebody standing in front of it. Capitals, figures, a space and a few
// marks (- . / < > &, and ^ and ~ for arrows up and down); anything else is a space.
namespace Lettering
{

// `height`: a capital's, in metres.
MeshData Mesh(const std::string& text, float height);
// How wide that is.
float Width(const std::string& text, float height);

} // namespace Lettering

} // namespace pred
