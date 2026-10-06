$input v_worldPos, v_normal, v_texcoord0, v_color0, v_organic

#include <bgfx_shader.sh>

SAMPLER2D(s_baseColor, 0);     // multiplied into the albedo; white when a material has none
SAMPLER2D(s_normalMap, 10);    // a surface set's normal map (OpenGL style); flat when there is none
SAMPLER2D(s_roughnessMap, 11); // and its roughness, multiplied in; white when there is none
SAMPLER2D(s_steepColor, 12);     // a second set where the surface is steep (Material::steepScale)
SAMPLER2D(s_steepNormal, 13);
SAMPLER2D(s_steepRoughness, 14);
uniform vec4 u_steepParams;      // xyz = its tint, w = repeats a metre (0: none)
uniform vec4 u_surfaceParams;  // x = repeats a metre of the surface set, laid on from every side (0: off); y = how much of its own colour shows; z = natural ground (1) or laid in a grid (0)

uniform vec4 u_baseColor;       // rgb = albedo
uniform vec4 u_materialParams;  // x = metallic, y = roughness, z = how much of the mirror it shows
uniform vec4 u_emissive;        // rgb = emissive radiance
uniform vec4 u_lightDirection;  // xyz = direction towards the light
uniform vec4 u_lightColor;      // rgb = colour, w = intensity
uniform vec4 u_ambientSky;      // rgb = light from above
uniform vec4 u_ambientGround;   // rgb = bounce from below
uniform vec4 u_fogColor;        // rgb
uniform vec4 u_fogParams;       // x = start distance, y = end distance
uniform vec4 u_cameraPosition;  // xyz
uniform vec4 u_output;          // x = 1: write linear light for post-processing to finish
uniform vec4 u_grade;           // x = exposure, y = contrast, z = light where the sky cannot reach,
                                // w = 0 normal, 1 show the sun's occlusion, 2 show the sky's

// Lights that have a place. Four vec4 each: position and range, colour and intensity, direction and
// the cosine of the inner cone, then the cosine of the outer cone, whether it is on at all, and how
// big the source is.
#define MAX_LIGHTS 12
uniform vec4 u_lights[MAX_LIGHTS * 6];

// The two depth maps, and what turns a world position into a lookup in each.
//
// The sun's says what the sun can see. The sky's is rendered from straight overhead and says what
// the sky can see, which is the whole reason a room with a roof on it is dark: there is no authored
// darkness anywhere in this game, only geometry that light does not get past.
//
// Each map comes with a matrix from world space to its texture coordinates, an axis that turns a
// world position into a distance from that light in metres, and four numbers: the size of a texel
// in texture coordinates, how much slack to allow in metres, whether the map is on at all, and how
// far along the surface normal to take the reading.
uniform mat4 u_sunShadowMtx;
uniform vec4 u_sunShadowAxis;
uniform vec4 u_sunShadowParams;
uniform mat4 u_skyShadowMtx;
uniform vec4 u_skyShadowAxis;
uniform vec4 u_skyShadowParams;
SAMPLER2D(s_sunShadow, 1);
SAMPLER2D(s_skyShadow, 2);
// The torch's, rendered as a cone from wherever the brightest punctual light is.
//
// Without it a light that has a place has no occlusion at all: it lights whatever is inside its
// cone and inside its range, wall or no wall. In a game whose whole tension is what a beam reaches,
// a torch that shines through a wall is not a detail.
uniform mat4 u_spotShadowMtx;
uniform vec4 u_spotShadowAxis;
uniform vec4 u_spotShadowParams;
// How much world one texel of each map covers, in metres: x the sun, y the sky, z the torch. The
// slack every map needs is a distance in the world, so it is worked out from this rather than being
// a number somebody has to re-tune whenever a map changes size.
uniform vec4 u_shadowTexelWorld;
SAMPLER2D(s_spotShadow, 3);

// Soft patches over the ground, 0 to 1, a patch a unit across: smooth noise from a hash of where.
float SurfaceHash(vec2 p)
{
	p = fract(p * vec2(0.1031, 0.1030));
	p += dot(p, p.yx + 33.33);
	return fract((p.x + p.y) * p.x);
}

float SurfacePatches(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (vec2_splat(3.0) - f * 2.0);
	float a = SurfaceHash(i);
	float b = SurfaceHash(i + vec2(1.0, 0.0));
	float c = SurfaceHash(i + vec2(0.0, 1.0));
	float d = SurfaceHash(i + vec2(1.0, 1.0));
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
// The planar reflection: the world rendered again from a camera reflected across one flat surface.
//
// Sampled at the fragment's own place on the screen, which is what makes it a mirror rather than an
// approximation: the reflected image was rendered with the same projection from the mirrored camera,
// so for any point lying in the mirror's plane the two line up exactly.
SAMPLER2D(s_reflection, 4);
// The near sun map: see sunReaching.
uniform mat4 u_sunNearShadowMtx;
uniform vec4 u_sunNearShadowAxis;
uniform vec4 u_sunNearShadowParams;
SAMPLER2D(s_sunNearShadow, 5);
// xyz = the plane's normal, w = its offset. Fragments behind it are thrown away while the reflection
// is being drawn, or the mirror shows the things standing behind it. Zero means no clipping, which
// is what the ordinary pass uses.
uniform vec4 u_clipPlane;
// x = whether a reflection is available to sample at all. Off while the reflection itself is being
// rendered, so a mirror cannot reflect a mirror reflecting a mirror.
uniform vec4 u_reflectParams;

// The lamps' shadows: six faces a lamp, each a square tile of one atlas (LampShadows). x = one tile's
// width in the atlas, y = tiles across, z = 1 when textures start at the bottom, w = one texel of a tile.
uniform vec4 u_lampShadowParams;
// Living tissue (Material::organic): not drawn where it has not grown yet, grey and dry where it has died.
uniform vec4 u_organic;
SAMPLER2D(s_lampShadow, 6);

// The lamps, clustered (the main view): every lamp that reaches anything on screen, in a texture, and
// which of them reach each cell of a grid laid over the view -- so a surface is lit by exactly the lamps
// that reach where it is, rather than by a dozen chosen for the whole piece of level it is part of, which
// is what put hard lines between two pieces that chose differently.
//
// x = cells across, y = cells up, z = depth slices, w = 1 for the clustered lamps, 0 for the list above.
uniform vec4 u_clusterParams;
// x = the nearest depth sliced, y = slices / log(far / near).
uniform vec4 u_clusterDepth;
// xyz = which way the camera looks.
uniform vec4 u_clusterForward;
SAMPLER2D(s_lightData, 7);   // six texels a lamp, laid out as u_lights, one lamp a row
SAMPLER2D(s_clusterGrid, 8); // one texel a cell, a row a slice: x = first of its lamps in the list, y = how many
SAMPLER2D(s_lightIndex, 9);  // the lamps of every cell, one after another, 256 to a row
// x = 1: every surface its own colour, lit by nothing and fogged by nothing. For seeing what is where.
uniform vec4 u_fullbright;
#define LIGHT_INDEX_ROW 256
#define MOST_CLUSTER_LIGHTS 48

// bgfx gives HLSL a struct for a sampler and GLSL the built-in type, and makes `sampler2D` mean
// whichever of the two this backend has. So a function can take one, and the lookup below is
// written once rather than once per map.

#define PI 3.14159265359

// How much of a light reaches this surface: 1 in the open, 0 behind something, and part of the way
// along an edge. Two sizes, because the two maps are asking different questions.
//
// The sun wants a narrow filter. Its map is fitted tightly enough that a texel is about two
// centimetres, and a wide filter there does not soften a shadow so much as smear it -- the player's
// own shadow came out as a cloud. Nine taps is a two-texel penumbra, which is a soft edge rather
// than a blurred one.
//
// The sky wants a wide one, and not for softness. It is asking how much of the neighbourhood can see
// sky, which is a question about a quarter of a metre around the point rather than about the point,
// so the width is the answer rather than a way of hiding the lack of one.
#define KERNEL_NAME lightReachesSharp
#define KERNEL_TAPS 5
#define KERNEL_RADIUS 2.0
#include "shadow_kernel.sh"

// The torch's, narrower again. Its map is a cone rather than a box, so a texel is centimetres near
// the player and grows with distance; a wide filter in texels is a wide filter in metres out at the
// end of the beam, where the edge of a shadow is the thing being looked at.
#define KERNEL_NAME lightReachesSpot
#define KERNEL_TAPS 3
#define KERNEL_RADIUS 1.0
#include "shadow_kernel.sh"

#define KERNEL_NAME lightReachesWide
#define KERNEL_TAPS 3
#define KERNEL_RADIUS 1.0
#include "shadow_kernel.sh"

// How much sky reaches a surface.
//
// A map rendered straight down can say what is above a point. For a floor that is the whole
// question. For a wall it is the wrong question entirely, and asking it anyway is what put a
// horizontal line across every box in the level.
//
// Work it through. The map is 128 texels over 32 metres, so a texel is 25 cm. On the side of a two
// metre box, the texels covering the box report its own top, two metres up. A vertical surface gets
// the largest slope allowance there is -- 0.45 * 2 = 0.9 m -- so everything on that side lower than
// 0.9 m below the top counts as occluded by the box it is part of, and everything above it does not.
// That is a hard horizontal line at 1.1 m on every box, and no amount of widening the filter moves
// it, because the filter is averaging correct answers to the wrong question.
//
// The right question for a wall is not "what is over me" but "does the sky get to me from the
// direction I face". So there are two lookups: one where the surface is, and one pushed out
// sideways along the way it faces. Whichever finds more sky wins.
//
//   the side of a box   the probe lands off the box, on open ground  -> lit, and evenly
//   a wall in a room    the probe is still under the roof            -> dark, as it must be
//   a wall outdoors     the probe is in the open                     -> lit
//   a crate by a wall   the probe clears it at three quarters of a metre
//   a doorway           the probe crosses the threshold before the surface does, so it fades
//
// The reach has to sit between the two scales: further than half the things a surface is part of,
// nearer than half a room. Three quarters of a metre clears a crate, a bench and a doorframe, and is
// nothing against a room four metres across with a roof at three and a half.
//
// It is scaled by how vertical the surface is, so a floor -- where the straight-down map is exactly
// right -- does not get pushed anywhere and nothing about it changes.
#define SKY_REACH 1.15

float skyReaching(vec3 P, vec3 N, float slack)
{
	float here = lightReachesWide(s_skyShadow, u_skyShadowMtx, u_skyShadowAxis, u_skyShadowParams,
	                              P, N, slack);

	vec3 sideways = vec3(N.x, 0.0, N.z);
	float lateral = length(sideways);
	if (lateral < 0.05)
	{
		return here; // facing straight up or straight down: there is no sideways to probe along
	}
	sideways /= lateral;

	// Three probes at increasing distance, averaged, rather than one.
	//
	// One probe answers a yes-or-no question -- "is there sky three quarters of a metre that way" --
	// and a surface either gets the answer or does not. Put a second box within that distance and
	// the part of the face it covers flips to "no" while the rest stays "yes", with a hard vertical
	// line between them wherever the neighbour's edge happens to fall. That is the band reported as
	// "the side of the block looks different when it is close to another block", and it is a real
	// occlusion -- a face half a metre from another face genuinely sees less sky -- arriving as a
	// step instead of a gradient.
	//
	// Three distances make it a gradient. A neighbour at 0.4 m blocks the second and third probe and
	// leaves the first, so the face darkens by a third rather than switching; as the surface runs
	// past the neighbour's edge each probe clears at a different place, so the three transitions are
	// staggered instead of stacked on one line. It is also a better question: how open is the
	// neighbourhood on the side this surface faces, rather than at one arbitrary distance from it.
	float open = 0.0;
	open += lightReachesWide(s_skyShadow, u_skyShadowMtx, u_skyShadowAxis, u_skyShadowParams,
	                         P + sideways * (0.30 * lateral), N, slack);
	open += lightReachesWide(s_skyShadow, u_skyShadowMtx, u_skyShadowAxis, u_skyShadowParams,
	                         P + sideways * (0.70 * lateral), N, slack);
	open += lightReachesWide(s_skyShadow, u_skyShadowMtx, u_skyShadowAxis, u_skyShadowParams,
	                         P + sideways * (SKY_REACH * lateral), N, slack);
	open *= 1.0 / 3.0;
	return mix(here, max(here, open), lateral);
}

// How much slack a surface needs before it stops shadowing itself.
//
// Not a constant, which is what it was, and that is most of what was wrong with these shadows.
//
// A map texel covers some area of world. A surface square-on to the light crosses almost no depth
// within one texel and needs almost no slack. A surface at a glancing angle crosses a great deal --
// the depth recorded for the texel is the depth somewhere in the middle of it, and the surface is
// above that at one edge and below it at the other -- so it needs slack proportional to how steeply
// it is tilted away from the light. One number cannot serve both: big enough for the glancing case
// it lifts every shadow off the thing casting it, and small enough for the square-on case the
// glancing surfaces stripe themselves with their own shadow, in a pattern that crawls as the map
// snaps to its grid while the player walks. That crawl is what "flickering" was.
float shadowSlack(float base, float NoL, float mostSlope, float texelWorld, float reachTexels)
{
	// How much depth the surface crosses inside the filter's reach, which is the whole of what the
	// slack is for -- and it is a distance in the world, so it has to be measured in the world.
	//
	// It used to be `base * (1 + slope)`: a number in metres, tuned by eye, multiplied by the slope.
	// That works until the map's coverage changes, and then it silently stops working. The sky bias
	// was set to 0.16 when a texel was 25 cm of world; raising the shadow distance from 16 m to 24 m
	// two commits later made a texel 37.5 cm, and nothing said so. A 45 degree ramp crosses 0.375 m
	// within one texel of the filter and had 0.32 m of slack, so it ruled itself in stripes -- which
	// is the ramp banding coming back after it had been fixed once.
	//
	// Derived, it cannot go stale. `texelWorld` is how much world one texel of this map covers and
	// `reachTexels` how far the filter reaches in texels, so their product times the slope is
	// exactly the depth the surface crosses under the filter. Change the resolution or the distance
	// and this follows on its own.
	//
	// The clamp stays, and the two maps still want very different ones. The sun can be generous: a
	// surface edge-on to it is barely lit anyway. The sky cannot -- a vertical wall reads as maximum
	// slope against a map that looks straight down, and too much slack there lets every wall in a
	// sealed room ignore the roof over it and the room lights up. Its clamp has to keep the total
	// well under the shortest ceiling in the game.
	float slope = min(sqrt(max(1.0 - NoL * NoL, 0.0)) / max(NoL, 0.08), mostSlope);
	// Twice the geometric figure, because the depth recorded for a texel is the depth somewhere
	// inside it rather than at its near edge, and the reading is taken a normal-offset away as well.
	return base + texelWorld * reachTexels * slope * 2.0;
}

// The sun, from one map.
//
// There were two for a while -- a fine one around the player and a coarse one beyond it -- and the
// shaded point took the darker of their two answers so that an occluder only had to be in one of
// them to cast. That fixed shadows disappearing as the player walked, and it brought its own
// trouble: taking the darker answer also takes the worse artefact. The two maps have texels of very
// different sizes, they shared one bias, and a bias right for one is wrong for the other, so
// whichever map was striping itself won every pixel. Two maps' worth of acne, and it crawled.
//
// One map, fitted more tightly so its texels are small enough on their own, is less machinery and
// fewer ways to be wrong. What it costs is shadows stopping at the edge of what it covers, which is
// a clean limit rather than a thing that pops.
float sunReaching(vec3 P, vec3 N, float NoL)
{
	// Two maps of the same sun: a fine one over the dozen metres round the player, and a coarse one
	// out to the shadow distance. Whichever covers the point, and a blend across the band where the
	// fine one runs out, so no line is ever seen where one hands over to the other.
	vec4 nearProjected = mul(u_sunNearShadowMtx, vec4(P, 1.0));
	vec2 nearUv = nearProjected.xy / nearProjected.w;
	vec2 fromEdge = min(nearUv, vec2_splat(1.0) - nearUv);
	float nearWeight = clamp((min(fromEdge.x, fromEdge.y) - 0.07) / 0.08, 0.0, 1.0);

	float near = 1.0;
	if (nearWeight > 0.0)
	{
		near = lightReachesSharp(s_sunNearShadow, u_sunNearShadowMtx, u_sunNearShadowAxis, u_sunNearShadowParams, P,
		                         N, shadowSlack(u_sunNearShadowParams.y, NoL, 6.0, u_shadowTexelWorld.w, 2.0));
		if (nearWeight >= 1.0)
		{
			return near;
		}
	}
	float far = lightReachesSharp(s_sunShadow, u_sunShadowMtx, u_sunShadowAxis, u_sunShadowParams, P, N,
	                              shadowSlack(u_sunShadowParams.y, NoL, 6.0, u_shadowTexelWorld.x, 2.0));
	return mix(far, near, nearWeight);
}

// How much of a lamp reaches a surface, from the lamp's own shadow: which of its six faces looks towards
// the surface, where the surface falls in that face's tile, and whether anything nearer the lamp was drawn
// there. The faces are the same table as LampFaceForward / LampFaceUp, and the lookup the same as
// glm::lookAtRH and a 90 degree perspective, so what was drawn and what is read agree.
float lampReaches(float slot, vec3 lamp, float range, vec3 P, vec3 N)
{
	vec3 v = P + N * 0.04 - lamp;
	vec3 a = abs(v);
	float face;
	vec3 f;
	vec3 up;
	if (a.x >= a.y && a.x >= a.z)
	{
		face = v.x > 0.0 ? 0.0 : 1.0;
		f = vec3(v.x > 0.0 ? 1.0 : -1.0, 0.0, 0.0);
		up = vec3(0.0, 1.0, 0.0);
	}
	else if (a.y >= a.z)
	{
		face = v.y > 0.0 ? 2.0 : 3.0;
		f = vec3(0.0, v.y > 0.0 ? 1.0 : -1.0, 0.0);
		up = vec3(0.0, 0.0, v.y > 0.0 ? 1.0 : -1.0);
	}
	else
	{
		face = v.z > 0.0 ? 4.0 : 5.0;
		f = vec3(0.0, 0.0, v.z > 0.0 ? 1.0 : -1.0);
		up = vec3(0.0, 1.0, 0.0);
	}
	vec3 s = normalize(cross(f, up));
	vec3 u = cross(s, f);
	float major = max(dot(v, f), 1e-4);
	vec2 ndc = vec2(dot(v, s), dot(v, u)) / major;

	float tile = slot * 6.0 + face;
	float across = u_lampShadowParams.y;
	float ty = floor(tile / across);
	float tx = tile - ty * across;
	float norm = u_lampShadowParams.x;
	float texel = u_lampShadowParams.w;
	bool bottomUp = u_lampShadowParams.z > 0.5;

	// A little slack, growing with distance as the texels do.
	float bias = 0.03 + major * 0.03;
	// Sixteen readings, the four-by-four texels round the point, each weighted by how far the point is from
	// it: a three-texel filter that slides smoothly across the texels rather than stepping from one to the
	// next. Nine readings a texel apart, as this was, averaged whole texels -- and whole texels are a
	// staircase, which was every doorway's edge of light.
	float size = 1.0 / texel;
	vec2 local = vec2(ndc.x * 0.5 + 0.5, bottomUp ? ndc.y * 0.5 + 0.5 : 0.5 - ndc.y * 0.5);
	vec2 at = local * size - 0.5;
	vec2 base = floor(at);
	vec2 frac = at - base;
	float lit = 0.0;
	for (int k = 0; k < 16; ++k)
	{
		float i = mod(float(k), 4.0);
		float j = floor(float(k) / 4.0);
		vec2 centre = clamp((base + vec2(i, j) - 1.0 + 0.5) * texel, vec2_splat(0.5 * texel), vec2_splat(1.0 - 0.5 * texel));
		vec2 uv = vec2((tx + centre.x) * norm, bottomUp ? 1.0 - (ty + 1.0 - centre.y) * norm : (ty + centre.y) * norm);
		float stored = texture2DLod(s_lampShadow, uv, 0.0).x;
		float wx = i < 0.5 ? 1.0 - frac.x : (i > 2.5 ? frac.x : 1.0);
		float wy = j < 0.5 ? 1.0 - frac.y : (j > 2.5 ? frac.y : 1.0);
		lit += (major <= range - stored + bias ? 1.0 : 0.0) * wx * wy;
	}
	return lit * (1.0 / 9.0);
}

// GGX / Trowbridge-Reitz normal distribution.
float distributionGGX(float NoH, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float d = NoH * NoH * (a2 - 1.0) + 1.0;
	return a2 / max(PI * d * d, 1e-6);
}

// Height-correlated Smith visibility term (already divided by the 4*NoL*NoV denominator).
float visibilitySmith(float NoV, float NoL, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float lambdaV = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
	float lambdaL = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
	return 0.5 / max(lambdaV + lambdaL, 1e-6);
}

vec3 fresnelSchlick(vec3 f0, float VoH)
{
	float f = pow(1.0 - VoH, 5.0);
	return f0 + (vec3_splat(1.0) - f0) * f;
}

// What one light that has a place adds: a torch, a flare, a lamp on a wall.
//
// The same shading as the sun, with two things added. Distance, which falls off with the square and is
// cut off at the light's range so a corridor does not pay for a lamp three rooms away. And the cone, which
// is the difference between a bulb and a torch: full brightness within the inner angle, fading to nothing
// by the outer one, and a wide-open inner angle makes it a bulb.
//
// `torch`: the one light with the spot shadow (the first of the list, the brightest at the eye).
vec3 shadeLight(vec4 posRange, vec4 colorIntensity, vec4 dirInner, vec4 outerOn, vec4 boxMin, vec4 boxMax, bool torch,
                vec3 P, vec3 N, vec3 V, float NoV, float roughness, vec3 f0, vec3 diffuse, vec3 diffuseColor)
{
	if (outerOn.y < 0.5)
	{
		return vec3_splat(0.0);
	}
	// Kept in its room, when it has one: the far side of a wall is outside the box.
	if (boxMin.w > 0.5 && (any(lessThan(P, boxMin.xyz)) || any(greaterThan(P, boxMax.xyz))))
	{
		return vec3_splat(0.0);
	}
	vec3 toLight = posRange.xyz - P;
	float distance = length(toLight);
	if (distance > posRange.w)
	{
		return vec3_splat(0.0);
	}
	vec3 Lp = toLight / max(distance, 1e-4);

	// Inverse square, with the singularity at zero removed and a window that reaches exactly nothing at
	// the range rather than being cut off at some visible brightness. Inside the source radius the
	// brightness stops climbing: without that a torch on the eye puts a hundred times its own intensity
	// onto the weapon a hand span in front of it.
	float attenuation = 1.0 / max(distance * distance, outerOn.z * outerOn.z);
	float window = clamp(1.0 - pow(distance / posRange.w, 4.0), 0.0, 1.0);
	attenuation *= window * window;

	// A lamp with a shadow of its own: whether this surface can see it. Asked before the cone, because
	// the light bounced round its room below is kept in by the same walls.
	float lampLit = 1.0;
	if (!torch && outerOn.w > -0.5)
	{
		lampLit = lampReaches(outerOn.w, posRange.xyz, posRange.w, P, N);
	}
	// A little of every lamp's light has bounced off the room before it arrives: from every direction,
	// so it reaches what the fitting does not face -- the ceiling over a downlight, a corner behind it.
	// Not the torch: it is the one light with a shadow of the moment, and this has none.
	vec3 color = vec3_splat(0.0);
	if (!torch)
	{
		color += diffuseColor * colorIntensity.rgb * colorIntensity.w * attenuation * 0.05 * lampLit;
	}

	float cosAngle = dot(-Lp, normalize(dirInner.xyz));
	float cone = clamp((cosAngle - outerOn.x) / max(dirInner.w - outerOn.x, 1e-4), 0.0, 1.0);
	// Squared, so the edge of the beam softens rather than ending on a line.
	cone *= cone;
	if (cone <= 0.0)
	{
		return color;
	}

	vec3 Hp = normalize(Lp + V);
	float NoLp = max(dot(N, Lp), 0.0);
	float NoHp = max(dot(N, Hp), 0.0);
	float VoHp = max(dot(V, Hp), 0.0);
	float Dp = distributionGGX(NoHp, roughness);
	float Visp = visibilitySmith(NoV, NoLp, roughness);
	vec3 Fp = fresnelSchlick(f0, VoHp);
	float reaches = lampLit;
	if (torch)
	{
		reaches = lightReachesSpot(s_spotShadow, u_spotShadowMtx, u_spotShadowAxis, u_spotShadowParams, P, N,
		                           shadowSlack(u_spotShadowParams.y, NoLp, 4.0, u_shadowTexelWorld.z, 1.0));
	}
	vec3 lightRadiance = colorIntensity.rgb * colorIntensity.w * attenuation * cone * reaches;
	return color + (diffuse + Dp * Visp * Fp) * lightRadiance * NoLp;
}

// The lamps that reach this point of the view, from the cluster it falls in.
vec3 clusteredLights(vec3 P, vec3 N, vec3 V, float NoV, float roughness, vec3 f0, vec3 diffuse, vec3 diffuseColor)
{
	vec4 clip = mul(u_viewProj, vec4(P, 1.0));
	vec2 ndc = clip.xy / max(clip.w, 1e-4);
	float across = clamp(floor((ndc.x * 0.5 + 0.5) * u_clusterParams.x), 0.0, u_clusterParams.x - 1.0);
	float up = clamp(floor((ndc.y * 0.5 + 0.5) * u_clusterParams.y), 0.0, u_clusterParams.y - 1.0);
	float depth = max(dot(P - u_cameraPosition.xyz, u_clusterForward.xyz), u_clusterDepth.x);
	float slice = clamp(floor(log(depth / u_clusterDepth.x) * u_clusterDepth.y), 0.0, u_clusterParams.z - 1.0);
	vec4 cell = texelFetch(s_clusterGrid, ivec2(int(across + up * u_clusterParams.x), int(slice)), 0);
	int first = int(cell.x + 0.5);
	int count = int(cell.y + 0.5);
	vec3 color = vec3_splat(0.0);
	for (int k = 0; k < MOST_CLUSTER_LIGHTS; ++k)
	{
		if (k >= count)
		{
			break;
		}
		int at = first + k;
		int row = at / LIGHT_INDEX_ROW;
		int light = int(texelFetch(s_lightIndex, ivec2(at - row * LIGHT_INDEX_ROW, row), 0).x + 0.5);
		color += shadeLight(texelFetch(s_lightData, ivec2(0, light), 0), texelFetch(s_lightData, ivec2(1, light), 0),
		                    texelFetch(s_lightData, ivec2(2, light), 0), texelFetch(s_lightData, ivec2(3, light), 0),
		                    texelFetch(s_lightData, ivec2(4, light), 0), texelFetch(s_lightData, ivec2(5, light), 0), false,
		                    P, N, V, NoV, roughness, f0, diffuse, diffuseColor);
	}
	return color;
}


void main()
{
	// Thrown away if it is behind the mirror.
	//
	// The reflection pass renders the world from a camera reflected across the mirror's plane, and
	// everything on the far side of that plane -- the wall the mirror is hung on, the room behind it
	// -- reflects to somewhere in front of the camera and would be drawn. What a mirror shows is
	// only what is in front of it.
	if (u_clipPlane.w != 0.0 || dot(u_clipPlane.xyz, u_clipPlane.xyz) > 0.0)
	{
		if (dot(v_worldPos, u_clipPlane.xyz) + u_clipPlane.w < 0.0)
		{
			discard;
		}
	}

	if (u_organic.x > 0.5 && v_organic.x < 0.02)
	{
		discard;
	}
	vec3 N = normalize(v_normal);
	// A surface set laid on from every side at once ("triplanar"): sampled across x, y and z by where the point is, and
	// blended by which way the surface faces, so a box of any size is covered evenly with no coordinates of its own.
	// Its normal map bends N the same way, each sample turned into the frame of the side it was taken from.
	vec3 surfaceAlbedo = vec3_splat(1.0);
	float surfaceRough = 1.0;
	if (u_surfaceParams.x > 0.0)
	{
		vec3 blend = pow(abs(N), vec3_splat(4.0));
		blend /= max(blend.x + blend.y + blend.z, 1e-4);
		vec3 at = v_worldPos * u_surfaceParams.x;
		// Each side's picture the right way up: an image's top row is read first, so up the picture is down its v -- and
		// an OpenGL normal map's green points up the picture. Laid with v the other way, its bumps would light from the
		// wrong side across one direction and stand in where they should stand out.
		vec2 uvX = vec2(at.z, -at.y);
		vec2 uvY = vec2(at.x, -at.z);
		vec2 uvZ = vec2(at.x, -at.y);
		vec3 nY = texture2D(s_normalMap, uvY).xyz * 2.0 - 1.0;
		vec3 groundAlbedo = pow(texture2D(s_baseColor, uvY).rgb, vec3_splat(2.2));
		float groundRough = texture2D(s_roughnessMap, uvY).r;
		// On the ground, which is where a repeat shows -- the same few marks in rows to the horizon -- the set again, larger
		// and turned, blended in and out over patches some metres across, and the whole of it a little lighter and darker
		// by patches larger still.
		if (blend.y > 0.01 && u_surfaceParams.z > 0.5)
		{
			vec2 wide = mul(mat2(0.788, -0.616, 0.616, 0.788), uvY) * 0.41 + vec2(0.37, 0.71);
			float mixIn = smoothstep(0.35, 0.65, SurfacePatches(v_worldPos.xz * 0.11));
			vec3 nWide = texture2D(s_normalMap, wide).xyz * 2.0 - 1.0;
			// The turned sample's bumps turned back into the ground's frame.
			nWide.xy = mul(mat2(0.788, 0.616, -0.616, 0.788), nWide.xy);
			nY = normalize(mix(nY, nWide, mixIn));
			groundAlbedo = mix(groundAlbedo, pow(texture2D(s_baseColor, wide).rgb, vec3_splat(2.2)), mixIn);
			groundRough = mix(groundRough, texture2D(s_roughnessMap, wide).r, mixIn);
			groundAlbedo *= 0.9 + 0.2 * SurfacePatches(v_worldPos.xz * 0.023 + vec2_splat(17.0));
		}
		surfaceAlbedo = pow(texture2D(s_baseColor, uvX).rgb, vec3_splat(2.2)) * blend.x + groundAlbedo * blend.y +
		                pow(texture2D(s_baseColor, uvZ).rgb, vec3_splat(2.2)) * blend.z;
		surfaceAlbedo = mix(vec3_splat(dot(surfaceAlbedo, vec3(0.2126, 0.7152, 0.0722))), surfaceAlbedo, u_surfaceParams.y);
		surfaceRough = texture2D(s_roughnessMap, uvX).r * blend.x + groundRough * blend.y + texture2D(s_roughnessMap, uvZ).r * blend.z;
		vec3 nX = texture2D(s_normalMap, uvX).xyz * 2.0 - 1.0;
		vec3 nZ = texture2D(s_normalMap, uvZ).xyz * 2.0 - 1.0;
		// Whiteout blend: each tangent-space normal added to the surface's own in that side's plane.
		vec3 s = sign(N);
		nX = vec3(nX.xy * vec2(s.x, 1.0) + N.zy, abs(nX.z) * N.x);
		nY = vec3(nY.xy * vec2(s.y, 1.0) + N.xz, abs(nY.z) * N.y);
		nZ = vec3(nZ.xy * vec2(-s.z, 1.0) + N.xy, abs(nZ.z) * N.z);
		vec3 Ng = N;
		N = normalize(nX.zyx * blend.x + nY.xzy * blend.y + nZ.xyz * blend.z);
		// Where it is steep, the second set -- the world's rock -- showing through as the slope passes what anybody can stand
		// on (SiteTerrain::Rockiness), laid on the same way.
		float rock = u_steepParams.w > 0.0 ? 1.0 - smoothstep(0.72, 0.94, Ng.y) : 0.0;
		if (rock > 0.001)
		{
			vec3 at2 = v_worldPos * u_steepParams.w;
			vec2 sX = vec2(at2.z, -at2.y);
			vec2 sY = vec2(at2.x, -at2.z);
			vec2 sZ = vec2(at2.x, -at2.y);
			// A rock face repeats as plainly as the ground does: blended with a larger copy of itself, turned and shifted,
			// over patches some metres across.
			float other = smoothstep(0.35, 0.65, SurfacePatches(vec2(v_worldPos.x + v_worldPos.y * 0.7, v_worldPos.z - v_worldPos.y * 0.6) * 0.09));
			vec2 oX = vec2(-sX.y, sX.x) * 0.43 + vec2(0.31, 0.57);
			vec2 oY = vec2(-sY.y, sY.x) * 0.43 + vec2(0.31, 0.57);
			vec2 oZ = vec2(-sZ.y, sZ.x) * 0.43 + vec2(0.31, 0.57);
			vec3 rockAlbedo = (pow(mix(texture2D(s_steepColor, sX).rgb, texture2D(s_steepColor, oX).rgb, other), vec3_splat(2.2)) * blend.x +
			                   pow(mix(texture2D(s_steepColor, sY).rgb, texture2D(s_steepColor, oY).rgb, other), vec3_splat(2.2)) * blend.y +
			                   pow(mix(texture2D(s_steepColor, sZ).rgb, texture2D(s_steepColor, oZ).rgb, other), vec3_splat(2.2)) * blend.z) *
			                  u_steepParams.xyz;
			float rockRough = texture2D(s_steepRoughness, sX).r * blend.x + texture2D(s_steepRoughness, sY).r * blend.y +
			                  texture2D(s_steepRoughness, sZ).r * blend.z;
			// The turned copy's bumps turned back (a quarter turn: x from its y, y from minus its x).
			vec3 qX = texture2D(s_steepNormal, oX).xyz * 2.0 - 1.0;
			vec3 qY = texture2D(s_steepNormal, oY).xyz * 2.0 - 1.0;
			vec3 qZ = texture2D(s_steepNormal, oZ).xyz * 2.0 - 1.0;
			vec3 rX = mix(texture2D(s_steepNormal, sX).xyz * 2.0 - 1.0, vec3(qX.y, -qX.x, qX.z), other);
			vec3 rY = mix(texture2D(s_steepNormal, sY).xyz * 2.0 - 1.0, vec3(qY.y, -qY.x, qY.z), other);
			vec3 rZ = mix(texture2D(s_steepNormal, sZ).xyz * 2.0 - 1.0, vec3(qZ.y, -qZ.x, qZ.z), other);
			rX = vec3(rX.xy * vec2(s.x, 1.0) + Ng.zy, abs(rX.z) * Ng.x);
			rY = vec3(rY.xy * vec2(s.y, 1.0) + Ng.xz, abs(rY.z) * Ng.y);
			rZ = vec3(rZ.xy * vec2(-s.z, 1.0) + Ng.xy, abs(rZ.z) * Ng.z);
			vec3 rockN = normalize(rX.zyx * blend.x + rY.xzy * blend.y + rZ.xyz * blend.z);
			// Its own colours mostly let go of -- the moss and lichen of the photograph -- for the world's rock colour.
			rockAlbedo = mix(vec3_splat(dot(rockAlbedo, vec3(0.2126, 0.7152, 0.0722))), rockAlbedo, 0.35);
			surfaceAlbedo = mix(surfaceAlbedo, rockAlbedo, rock);
			surfaceRough = mix(surfaceRough, rockRough, rock);
			N = normalize(mix(N, rockN, rock));
		}
	}
	vec3 V = normalize(u_cameraPosition.xyz - v_worldPos);
	vec3 L = normalize(u_lightDirection.xyz);
	vec3 H = normalize(L + V);

	float NoL = max(dot(N, L), 0.0);
	float NoV = max(dot(N, V), 1e-4);
	float NoH = max(dot(N, H), 0.0);
	float VoH = max(dot(V, H), 0.0);

	// The texture is always bound. A material without one samples a single white pixel, so this is
	// a multiply by one rather than a branch, and there is only ever one mesh program.
	vec4 sampled = u_surfaceParams.x > 0.0 ? vec4_splat(1.0) : texture2D(s_baseColor, v_texcoord0);
	vec3 textured = sampled.rgb;
	// Downloads arrive with textures authored in gamma space, which is what an image viewer shows
	// and what a lighting calculation must not be given: multiplying light by a gamma-encoded
	// colour washes everything out. Decoded here rather than by asking bgfx for an sRGB format,
	// because that would have to be decided at upload for every image the game will ever load.
	textured = pow(textured, vec3_splat(2.2));
	// And the colour painted on each vertex, white for nearly everything. A creature is one mesh with
	// its skin, bone, gums and eye sockets painted on it this way, already in linear space.
	vec3 albedo = u_baseColor.rgb * textured * v_color0.rgb * surfaceAlbedo;
	float metallic = clamp(u_materialParams.x, 0.0, 1.0);
	// Clamp roughness away from zero: perfectly smooth surfaces alias badly with a single light.
	float roughness = clamp(u_materialParams.y * v_color0.a * surfaceRough, 0.045, 1.0);
	if (u_organic.x > 0.5)
	{
		albedo = mix(albedo, vec3(0.42, 0.36, 0.33) * (0.35 + 0.65 * dot(albedo, vec3_splat(0.8))), v_organic.y);
		roughness = mix(roughness, 0.9, v_organic.y);
	}

	vec3 diffuseColor = albedo * (1.0 - metallic);
	vec3 f0 = mix(vec3_splat(0.04), albedo, metallic);

	// Direct lighting from the sun.
	float D = distributionGGX(NoH, roughness);
	float Vis = visibilitySmith(NoV, NoL, roughness);
	vec3 F = fresnelSchlick(f0, VoH);

	vec3 specular = D * Vis * F;
	vec3 diffuse = diffuseColor / PI;
	vec3 radiance = u_lightColor.rgb * u_lightColor.w;
	// Whatever the roof, the wall or the crate in the way is keeping off this surface.
	float sunReaches = sunReaching(v_worldPos, N, NoL);
	vec3 color = (diffuse + specular) * radiance * NoL * sunReaches;

	// And the lights that have a place: the torch first, from the list, with its shadow; then either the
	// lamps of this point's cluster (the main view) or the rest of the list (a mirror, an icon).
	if (u_clusterParams.w > 0.5)
	{
		color += shadeLight(u_lights[0], u_lights[1], u_lights[2], u_lights[3], u_lights[4], u_lights[5], true,
		                    v_worldPos, N, V, NoV, roughness, f0, diffuse, diffuseColor);
		color += clusteredLights(v_worldPos, N, V, NoV, roughness, f0, diffuse, diffuseColor);
	}
	else
	{
		for (int i = 0; i < MAX_LIGHTS; ++i)
		{
			color += shadeLight(u_lights[i * 6 + 0], u_lights[i * 6 + 1], u_lights[i * 6 + 2], u_lights[i * 6 + 3],
			                    u_lights[i * 6 + 4], u_lights[i * 6 + 5], i == 0,
			                    v_worldPos, N, V, NoV, roughness, f0, diffuse, diffuseColor);
		}
	}

	// Hemispheric ambient stands in for indirect light until there is a real probe system.
	float hemisphere = N.y * 0.5 + 0.5;
	vec3 ambient = mix(u_ambientGround.rgb, u_ambientSky.rgb, hemisphere);
	// And the sky is blocked by the same geometry that blocks the sun.
	//
	// This is what makes an interior dark rather than merely unlit by the sun. Shadowing the sun
	// alone leaves a room filled with flat ambient light at the same brightness as the field
	// outside, which is the look of a room somebody forgot to light rather than a dark one. What is
	// left where the sky cannot reach is a small fraction, standing in for the light that would have
	// bounced its way in; without it, geometry out of the torch beam is not dark but absent.
	//
	// Its slack is worked out the same way the sun's is, against how far the surface is tilted from
	// facing straight up -- because this map looks straight down, so an up-facing surface crosses no
	// depth within a texel and a sloped one crosses a great deal. Without it a ramp compares its own
	// height against the height recorded at the middle of each texel, is above it on one side and
	// below it on the other, and rules itself in fine horizontal stripes all the way up.
	// abs, not max-with-zero.
	//
	// The slope term exists because a surface tilted away from the map crosses a lot of depth inside
	// one texel and needs room not to shadow itself. A ceiling is not tilted away from a top-down
	// map at all -- it is exactly square-on to it, the same as a floor, just facing the other way --
	// so it needs the least slack there is, and max(N.y, 0) handed it the most.
	//
	// What that did: a roof 0.30 m thick got 0.90 m of slack on its underside, so it could not
	// shadow itself. The ceiling of a sealed room read as fully lit by the sky, and that lit ceiling
	// is what was bleeding into the corners of the dark room.
	// Capped hard on a wall. Its slack grew with the map's texels, which grow with the shadow distance,
	// and at the higher settings it came to more than the thickness of the roof: the top half metre of
	// every wall under it read as open to the sky and lit up in a band along the ceiling -- worse the
	// higher the graphics were set. A wall is judged by the probes out into the room it faces anyway
	// (skyReaching), which do not need the slack; only floors and ramps, lying along the map, do.
	float skySlack = shadowSlack(u_skyShadowParams.y, abs(N.y), 1.0, u_shadowTexelWorld.y, 1.0);
	skySlack = mix(min(skySlack, 0.2), skySlack, smoothstep(0.3, 0.7, abs(N.y)));
	float skyReaches = skyReaching(v_worldPos, N, skySlack);
	ambient *= mix(u_grade.z, 1.0, skyReaches);

	color += diffuseColor * ambient;

	// What the surface reflects of its surroundings.
	//
	// This was a single number: the same ambient the diffuse uses, tinted by Fresnel and faded out
	// with roughness. That is enough to stop metal rendering black and it is not a reflection -- it
	// does not know which way the surface faces the world, so a steel plate looks identical whether
	// it is angled at the sky or at the floor, and turning the camera changes nothing on it.
	//
	// Now it looks along the reflected view direction and asks the same sky-and-ground hemisphere
	// what is over there. A floor picks up the sky, the underside of a rail picks up the ground, and
	// both change as you walk around them, which is most of what makes a surface read as polished
	// rather than as painted a lighter colour. A rough surface reflects a wide cone rather than a
	// direction, so its reflection is pulled back towards the surface normal in proportion.
	vec3 reflected = reflect(-V, N);
	reflected = normalize(mix(reflected, N, roughness * roughness));
	float reflectedHemisphere = reflected.y * 0.5 + 0.5;
	vec3 environmentColor = mix(u_ambientGround.rgb, u_ambientSky.rgb, reflectedHemisphere);
	// Occluded like the rest of the ambient: a room the sky cannot see into has nothing to reflect.
	environmentColor *= mix(u_grade.z, 1.0, skyReaches);

	// How much of that reflection actually leaves the surface, from Karis' analytic fit to the split
	// sum approximation. It is two numbers -- a scale and a bias on the Fresnel colour -- and they
	// carry the two things the old single fade got wrong: that grazing angles reflect far more than
	// head-on ones whatever the roughness, and that a rough surface keeps a little of that rather
	// than none.
	vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
	vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
	vec4 envFit = roughness * c0 + c1;
	float a004 = min(envFit.x * envFit.x, exp2(-9.28 * NoV)) * envFit.x + envFit.y;
	vec2 envTerm = vec2(-1.04, 1.04) * a004 + envFit.zw;
	color += environmentColor * (f0 * envTerm.x + vec3_splat(envTerm.y));

	// How much of the mirror this surface shows, worked out here and used right at the end.
	//
	// Blended in by Fresnel as well as by the material's own amount, because a mirror at a glancing
	// angle reflects nearly everything and one looked at square on still shows some of its own
	// colour. A mirror that is uniformly a perfect reflector reads as a hole in the wall.
	// Mostly the material's own amount, nudged up at grazing angles.
	//
	// The Fresnel shape used here first was the dielectric one -- a third head-on, everything at a
	// glancing angle -- which is right for glass and water and wrong for this. These are metal, and
	// polished metal reflects most of what hits it from every angle: aluminium is about ninety per
	// cent head-on. Weighted the dielectric way, the panel showed a third of a reflection under two
	// thirds of its own shading and read as pale plastic rather than as a mirror.
	float mirrorAmount =
		clamp(u_materialParams.z * u_reflectParams.x * (0.82 + 0.18 * pow(1.0 - NoV, 5.0)), 0.0, 1.0);

	// A screen glows by its picture, where its alpha says it is a screen (its case, painted in the same texture, is
	// alpha nought and does not glow); everything else glows the same all over.
	color += u_emissive.w > 0.5 ? u_emissive.rgb * textured * sampled.a : u_emissive.rgb;

	// Linear distance fog. Cheap, and it does most of the atmospheric work outdoors.
	//
	// Thinned where the sky cannot reach, because this fog is daylight scattering in the air between
	// the eye and the surface. Left at full strength it lifts the inside of a sealed room back to
	// the colour of the sky outside it, which undoes the occlusion above at exactly the distances an
	// interior is viewed at.
	float distanceToCamera = length(u_cameraPosition.xyz - v_worldPos);
	float fogAmount = clamp((distanceToCamera - u_fogParams.x) / max(u_fogParams.y - u_fogParams.x, 1e-4), 0.0, 1.0);
	color = mix(color, u_fogColor.rgb, fogAmount * mix(0.15, 1.0, skyReaches));
	// Fullbright: the surface's own colour, with a little shading by which way it faces so edges still read.
	if (u_fullbright.x > 0.5)
	{
		color = albedo * (0.55 + 0.45 * abs(dot(N, normalize(vec3(0.3, 0.8, 0.5))))) + u_emissive.rgb;
	}

	// Drawn for post-processing: linear light out, the mirror mixed in as light too (its texture is
	// linear then), and the finishing left to the post pass.
	if (u_output.x > 0.5 && u_grade.w < 0.5)
	{
		if (mirrorAmount > 0.001)
		{
			vec2 screenAt = gl_FragCoord.xy / max(u_viewRect.zw, vec2_splat(1.0));
			color = mix(color, texture2DLod(s_reflection, screenAt, 0.0).rgb, mirrorAmount);
		}
		gl_FragColor = vec4(color, 1.0);
		return;
	}

	// Exposure, then a filmic curve, then contrast, then gamma.
	//
	// Reinhard was here and it is the wrong curve for this game. It rolls everything off towards
	// grey, which is exactly what a dark scene must not do: the difference between a black corridor
	// and a corridor with something at the end of it lives in the bottom of the range, and Reinhard
	// spends its resolution at the top. This is the Narkowicz approximation of the ACES curve, which
	// holds the shadows down and lets the highlights go without turning them to paste.
	color *= max(u_grade.x, 0.0);
	color = clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);

	// Contrast about the middle grey of the encoded range rather than about zero, or turning it up
	// only makes the picture darker.
	color = pow(color, vec3_splat(1.0 / 2.2));
	color = clamp((color - vec3_splat(0.5)) * max(u_grade.y, 0.0) + vec3_splat(0.5), 0.0, 1.0);

	// And the mirror, here at the very end rather than up with the rest of the lighting.
	//
	// Sampled at this fragment's own place on the screen. The reflected image was rendered with the
	// same projection from a camera reflected across the mirror's plane, so for a point lying in
	// that plane the reflected view and this one agree pixel for pixel -- which is the whole trick,
	// and why it is exact for a flat mirror and meaningless for anything else.
	//
	// It has to be after the tone curve because the thing being sampled has already been through it.
	// The reflection pass runs this same shader, so what is in that texture is a finished picture:
	// exposed, tonemapped and gamma encoded. Mixed in before the curve it went through all of that a
	// second time, which lifts the blacks, flattens the highlights and leaves a mirror looking like
	// a sheet of milky plastic -- which is exactly how it looked.
	if (mirrorAmount > 0.001)
	{
		vec2 screen = gl_FragCoord.xy / max(u_viewRect.zw, vec2_splat(1.0));
		vec3 mirrored = texture2DLod(s_reflection, screen, 0.0).rgb;
		color = mix(color, mirrored, mirrorAmount);
	}

	// Occlusion on its own, for when the lighting is wrong and the question is which of the two maps
	// is saying what. 1 and 2 show what reaches, white for "it does". 3 shows whether the sky map
	// has anything at all recorded over this surface, and 4 how far behind the recorded surface this
	// one is: mid grey is level with it, brighter is behind it, darker is in front.
	if (u_grade.w > 0.5)
	{
		if (u_grade.w > 2.5)
		{
			vec4 tc = mul(u_skyShadowMtx, vec4(v_worldPos + N * u_skyShadowParams.w, 1.0));
			float nearest = texture2DLod(s_skyShadow, tc.xy / tc.w, 0.0).x;
			float here = dot(v_worldPos + N * u_skyShadowParams.w, u_skyShadowAxis.xyz) +
			             u_skyShadowAxis.w;
			// Raw, over the map. Black means nothing was drawn into this texel at all.
			float shown = u_grade.w < 3.5 ? clamp(nearest / 220.0, 0.0, 1.0)
			                              : clamp(v_worldPos.y * 0.2, 0.0, 1.0);
			gl_FragColor = vec4(vec3_splat(shown), 1.0);
			return;
		}
		float shown = u_grade.w < 1.5 ? sunReaches : skyReaches;
		gl_FragColor = vec4(vec3_splat(shown), 1.0);
		return;
	}
	// A little noise, smaller than one step of the output, before it is written.
	//
	// The screen holds 256 levels per channel. A wall lit by a torch, or a dark curved surface lit
	// only by ambient, crosses a few of those over hundreds of pixels, so the picture shows the steps
	// between them as bands -- and because the bands follow the shading rather than the geometry they
	// read as something wrong with the surface. It is worst exactly where this game lives: smooth,
	// dark, slowly changing.
	//
	// Adding under half a level of noise before the value is rounded turns each band edge into a
	// scattering of pixels either side of it, which the eye averages back to the gradient that was
	// there all along. This is interleaved gradient noise, which is cheap and, unlike a random hash,
	// does not sparkle from frame to frame because it depends only on where the pixel is.
	float dither = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));
	color += vec3_splat((dither - 0.5) / 255.0);

	gl_FragColor = vec4(color, 1.0);
}
