$input a_position, a_normal, a_texcoord0, a_color0
$output v_worldPos, v_normal, v_texcoord0, v_color0, v_organic, v_localPos, v_localNormal

#include <bgfx_shader.sh>

uniform vec4 u_organic;     // see Material::organic
uniform vec4 u_organicBeat; // see Material::organicBeat
uniform vec4 u_materialParams; // w = 1 cut out (Material::cutout): a plant's card, which sways
uniform vec4 u_wind;           // xy = the wind, metres a second along x and z; z = a clock it gusts by (Environment::wind)
uniform vec4 u_surfaceWeather;  // y = how much snow lies (Environment::snowCover): ground cover half buried in it

float organicBump(float t, float at, float width)
{
	float x = (t - at) / width;
	return x > 0.0 && x < 1.0 ? sin(x * 3.14159265) : 0.0;
}

void main()
{
	vec3 position = a_position;
	v_organic = vec2(1.0, 0.0);
	if (u_organic.x > 0.5)
	{
		// Grown: a front creeping out from the source, a metre and a bit wide; beyond it the tissue lies
		// flat into the surface under it, and past its very edge it is not drawn at all.
		float along = a_texcoord0.x;
		float height = a_texcoord0.y;
		float grown = clamp((u_organic.y - along) / 1.2, 0.0, 1.0);
		// Dead from the source outwards, and then rotted away into the surface to nothing.
		float dead = clamp((u_organic.z - along) / 1.5, 0.0, 1.0);
		float rotted = u_organic.w > 0.0 ? clamp((u_organic.z - along - 3.0) / u_organic.w, 0.0, 1.0) : 0.0;
		// And each beat, going out across it as a swell.
		float beat = fract(u_organicBeat.x - along * u_organicBeat.z);
		float swell = (organicBump(beat, 0.0, 0.14) + 0.6 * organicBump(beat, 0.2, 0.12)) * u_organicBeat.y * (1.0 - dead);
		// What hangs from it -- a strand and the drop at its end -- is held still, and grows down from where it hangs rather than
		// being drawn in towards the surface along its normal (sideways, for a strand: it started fat and shrank): a height given
		// as less than nothing, how far down it this point is.
		float hanging = 1.0 - step(0.0, height);
		swell *= 1.0 - hanging;
		height = abs(height);
		float thickness = grown * (1.0 + 0.4 * swell) * (1.0 - rotted);
		position -= a_normal * height * (1.0 - thickness) * (1.0 - hanging);
		position.y += height * (1.0 - grown * (1.0 - rotted)) * hanging;
		v_organic = vec2(grown * (1.0 - rotted), dead);
	}
	vec4 worldPosition = mul(u_model[0], vec4(position, 1.0));
	vec4 colour = a_color0;
	// Ground cover (Material::cover, materialParams.w 2): all of it out to sixteen metres, fewer tufts further off, each
	// one fading in or out (its alpha, which fs_mesh scatters away by) by where the eye is this frame, not when its patch was
	// last made -- so none pops in or out, none is seen growing, and none is left standing at the edge of what was made. Red
	// says how soon a tuft goes (0 last), green how high this point is over the ground (over 1.5 metres), blue how light its
	// card is (0.6 to 1.4 of the material). Dark at the roots, where the blades shade each other.
	if (u_materialParams.w > 1.5)
	{
		vec3 eye = mul(u_invView, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
		float far = length(worldPosition.xz - eye.xz);
		// Each tuft comes in over a short way as the eye nears where its share is reached (sixteen metres over the root of
		// its rank), a few percent of that distance: over a fixed band of share, a far one took tens of metres of walking to
		// come in, and the far field was always half scattered in, seen fading.
		float comesIn = 16.0 / sqrt(max(a_color0.r, 1.0e-4));
		float grown = clamp((comesIn - far) / (0.06 * comesIn), 0.0, 1.0) * (1.0 - smoothstep(105.0, 120.0, far));
		// A little sunk where snow lies, the snow round its foot.
		worldPosition.y -= u_surfaceWeather.y * 0.1;
		float up = 1.0 - fract(a_texcoord0.y * 2.0);
		colour = vec4(vec3_splat(mix(0.5, 1.0, smoothstep(0.0, 0.7, up)) * (0.6 + 0.8 * a_color0.b)), grown);
	}
	// A plant's card (cut out: Material::cutout) moving in the wind: its foot held, its top bent with the wind -- leaning
	// with it in waves rolling across the ground the way it blows, and fluttering quicker on top. How far up the card a
	// corner is comes from where it is in its cell of the foliage picture (Foliage::Cell: two rows, the top of each first).
	if (u_materialParams.w > 0.5)
	{
		float up = 1.0 - fract(a_texcoord0.y * 2.0);
		float strength = max(length(u_wind.xy), 1.5);
		vec2 way = length(u_wind.xy) > 0.01 ? u_wind.xy / length(u_wind.xy) : vec2(0.8, 0.6);
		float t = u_wind.z;
		float wave = 0.6 + 0.4 * sin(t * 1.3 - dot(worldPosition.xz, way) * 0.35);
		float flutter = sin(t * 7.0 + dot(worldPosition.xz, vec2(2.9, 2.3))) * 0.3;
		float bend = min(strength, 18.0) * 0.012 * up * up;
		worldPosition.xz += way * bend * (wave + flutter) + vec2(-way.y, way.x) * bend * flutter * 0.5;
		worldPosition.y -= bend * 0.25 * up;
	}

	v_worldPos = worldPosition.xyz;
	// And where it is on the thing itself, for a surface set laid on the thing rather than the world (one that moves).
	v_localPos = position;
	v_localNormal = a_normal;
	v_normal = mul(u_model[0], vec4(a_normal, 0.0)).xyz;
	v_texcoord0 = a_texcoord0;
	v_color0 = colour;

	gl_Position = mul(u_viewProj, worldPosition);
}
