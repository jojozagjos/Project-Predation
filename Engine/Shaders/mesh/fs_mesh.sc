$input v_worldPos, v_normal, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_baseColor, 0);     // multiplied into the albedo; white when a material has none

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
uniform vec4 u_grade;           // x = exposure, y = contrast, z = light where the sky cannot reach,
                                // w = 0 normal, 1 show the sun's occlusion, 2 show the sky's

// Lights that have a place. Four vec4 each: position and range, colour and intensity, direction and
// the cosine of the inner cone, then the cosine of the outer cone, whether it is on at all, and how
// big the source is.
#define MAX_LIGHTS 4
uniform vec4 u_lights[MAX_LIGHTS * 4];

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
SAMPLER2D(s_spotShadow, 3);
// The planar reflection: the world rendered again from a camera reflected across one flat surface.
//
// Sampled at the fragment's own place on the screen, which is what makes it a mirror rather than an
// approximation: the reflected image was rendered with the same projection from the mirrored camera,
// so for any point lying in the mirror's plane the two line up exactly.
SAMPLER2D(s_reflection, 4);
// xyz = the plane's normal, w = its offset. Fragments behind it are thrown away while the reflection
// is being drawn, or the mirror shows the things standing behind it. Zero means no clipping, which
// is what the ordinary pass uses.
uniform vec4 u_clipPlane;
// x = whether a reflection is available to sample at all. Off while the reflection itself is being
// rendered, so a mirror cannot reflect a mirror reflecting a mirror.
uniform vec4 u_reflectParams;

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
#define SKY_REACH 0.75

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
	vec3 probe = P + sideways * SKY_REACH * lateral;
	float there = lightReachesWide(s_skyShadow, u_skyShadowMtx, u_skyShadowAxis, u_skyShadowParams,
	                               probe, N, slack);
	return mix(here, max(here, there), lateral);
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
float shadowSlack(float base, float NoL, float mostSlope)
{
	// tan of the angle from the surface normal to the light, which is how much depth one step
	// across the surface covers. It runs to infinity at ninety degrees, so it is clamped -- and the
	// two maps want very different clamps.
	//
	// The sun can be generous: a surface edge-on to it is barely lit anyway, so slack there costs
	// nothing visible. The sky cannot. A vertical wall reads as maximum slope against a map that
	// looks straight down, and the generous clamp handed it nearly four metres of slack -- more than
	// the height of a roof, so every wall in a sealed room ignored the roof over it and the room lit
	// up. Its clamp has to stay well under the shortest ceiling in the game.
	float slope = sqrt(max(1.0 - NoL * NoL, 0.0)) / max(NoL, 0.08);
	return base * (1.0 + min(slope, mostSlope));
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
	return lightReachesSharp(s_sunShadow, u_sunShadowMtx, u_sunShadowAxis, u_sunShadowParams, P, N,
	                        shadowSlack(u_sunShadowParams.y, NoL, 6.0));
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

	vec3 N = normalize(v_normal);
	vec3 V = normalize(u_cameraPosition.xyz - v_worldPos);
	vec3 L = normalize(u_lightDirection.xyz);
	vec3 H = normalize(L + V);

	float NoL = max(dot(N, L), 0.0);
	float NoV = max(dot(N, V), 1e-4);
	float NoH = max(dot(N, H), 0.0);
	float VoH = max(dot(V, H), 0.0);

	// The texture is always bound. A material without one samples a single white pixel, so this is
	// a multiply by one rather than a branch, and there is only ever one mesh program.
	vec3 textured = texture2D(s_baseColor, v_texcoord0).rgb;
	// Downloads arrive with textures authored in gamma space, which is what an image viewer shows
	// and what a lighting calculation must not be given: multiplying light by a gamma-encoded
	// colour washes everything out. Decoded here rather than by asking bgfx for an sRGB format,
	// because that would have to be decided at upload for every image the game will ever load.
	textured = pow(textured, vec3_splat(2.2));
	vec3 albedo = u_baseColor.rgb * textured;
	float metallic = clamp(u_materialParams.x, 0.0, 1.0);
	// Clamp roughness away from zero: perfectly smooth surfaces alias badly with a single light.
	float roughness = clamp(u_materialParams.y, 0.045, 1.0);

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

	// And the lights that have a place: a torch, a flare, a lamp on a wall.
	//
	// The same shading as the sun, with two things added. Distance, which falls off with the square
	// and is cut off at the light's range so a corridor does not pay for a lamp three rooms away.
	// And the cone, which is the difference between a bulb and a torch: full brightness within the
	// inner angle, fading to nothing by the outer one, and a wide-open inner angle makes it a bulb.
	for (int i = 0; i < MAX_LIGHTS; ++i)
	{
		vec4 posRange = u_lights[i * 4 + 0];
		vec4 colorIntensity = u_lights[i * 4 + 1];
		vec4 dirInner = u_lights[i * 4 + 2];
		vec4 outerOn = u_lights[i * 4 + 3];
		if (outerOn.y < 0.5)
		{
			continue;
		}

		vec3 toLight = posRange.xyz - v_worldPos;
		float distance = length(toLight);
		if (distance > posRange.w)
		{
			continue;
		}
		vec3 Lp = toLight / max(distance, 1e-4);

		// Inverse square, with the singularity at zero removed and a window that reaches exactly
		// nothing at the range rather than being cut off at some visible brightness.
		// Inside the source radius the brightness stops climbing. Without that a torch on the eye
		// puts a hundred times its own intensity onto the weapon a hand span in front of it.
		float attenuation = 1.0 / max(distance * distance, outerOn.z * outerOn.z);
		float window = clamp(1.0 - pow(distance / posRange.w, 4.0), 0.0, 1.0);
		attenuation *= window * window;

		float cosAngle = dot(-Lp, normalize(dirInner.xyz));
		float cone = clamp((cosAngle - outerOn.x) / max(dirInner.w - outerOn.x, 1e-4), 0.0, 1.0);
		// Squared, so the edge of the beam softens rather than ending on a line.
		cone *= cone;
		if (cone <= 0.0)
		{
			continue;
		}

		vec3 Hp = normalize(Lp + V);
		float NoLp = max(dot(N, Lp), 0.0);
		float NoHp = max(dot(N, Hp), 0.0);
		float VoHp = max(dot(V, Hp), 0.0);

		float Dp = distributionGGX(NoHp, roughness);
		float Visp = visibilitySmith(NoV, NoLp, roughness);
		vec3 Fp = fresnelSchlick(f0, VoHp);
		// Whatever is in the way of the torch.
		//
		// Only the first slot, because only the first slot has a map: one shadowed punctual light
		// rather than four, and the game sorts the slots so that the first is whatever contributes
		// most at the eye -- which is the player's own torch whenever it is lit, because it is at
		// the eye. Every other light still shines through walls, and that is a deliberate limit
		// rather than an oversight: each one would be another whole pass over the scene.
		float reaches = 1.0;
		if (i == 0)
		{
			reaches = lightReachesSpot(s_spotShadow, u_spotShadowMtx, u_spotShadowAxis,
			                           u_spotShadowParams, v_worldPos, N,
			                           shadowSlack(u_spotShadowParams.y, NoLp, 4.0));
		}

		vec3 lightRadiance = colorIntensity.rgb * colorIntensity.w * attenuation * cone * reaches;
		color += (diffuse + Dp * Visp * Fp) * lightRadiance * NoLp;
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
	float skyReaches =
		skyReaching(v_worldPos, N, shadowSlack(u_skyShadowParams.y, abs(N.y), 1.0));
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

	color += u_emissive.rgb;

	// Linear distance fog. Cheap, and it does most of the atmospheric work outdoors.
	//
	// Thinned where the sky cannot reach, because this fog is daylight scattering in the air between
	// the eye and the surface. Left at full strength it lifts the inside of a sealed room back to
	// the colour of the sky outside it, which undoes the occlusion above at exactly the distances an
	// interior is viewed at.
	float distanceToCamera = length(u_cameraPosition.xyz - v_worldPos);
	float fogAmount = clamp((distanceToCamera - u_fogParams.x) / max(u_fogParams.y - u_fogParams.x, 1e-4), 0.0, 1.0);
	color = mix(color, u_fogColor.rgb, fogAmount * mix(0.15, 1.0, skyReaches));

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
