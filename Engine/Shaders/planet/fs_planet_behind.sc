$input v_objectPos, v_normal, v_worldPos, v_texcoord0, v_lightObj

// fs_planet with its depth behind everything else (planet_fragment.sh): the world under a site, round its land.
#define PLANET_BEHIND 1
#include <bgfx_shader.sh>
#include "planet/planet_surface.sh"
#include "planet/planet_fragment.sh"
