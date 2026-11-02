$input v_color0, v_worldPos

// A mark laid on a surface (Material::decal) -- a stain, soot, damp: its colour multiplied into what is drawn there already, so
// where it is white it changes nothing, its edge fades into the surface under it whatever that is, and two over each other
// both darken it. Less of it the further off it is, lost in the fog as the surface is.

#include <bgfx_shader.sh>

uniform vec4 u_cameraPosition; // xyz
uniform vec4 u_fogParams;      // x = start distance, y = end distance

void main()
{
	float far = length(u_cameraPosition.xyz - v_worldPos);
	float fog = clamp((far - u_fogParams.x) / max(u_fogParams.y - u_fogParams.x, 1.0e-4), 0.0, 1.0);
	gl_FragColor = vec4(mix(v_color0.rgb, vec3_splat(1.0), fog), 1.0);
}
