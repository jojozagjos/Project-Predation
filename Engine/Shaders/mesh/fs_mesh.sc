$input v_worldPos, v_normal, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_baseColor, 0);     // multiplied into the albedo; white when a material has none

uniform vec4 u_baseColor;       // rgb = albedo
uniform vec4 u_materialParams;  // x = metallic, y = roughness
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
uniform mat4 u_sunNearMtx;
uniform vec4 u_sunNearAxis;
uniform vec4 u_sunNearParams;
uniform mat4 u_sunShadowMtx;
uniform vec4 u_sunShadowAxis;
uniform vec4 u_sunShadowParams;
uniform mat4 u_skyShadowMtx;
uniform vec4 u_skyShadowAxis;
uniform vec4 u_skyShadowParams;
SAMPLER2D(s_sunShadow, 1);
SAMPLER2D(s_sunNear, 3);
SAMPLER2D(s_skyShadow, 2);

// bgfx gives HLSL a struct for a sampler and GLSL the built-in type, and makes `sampler2D` mean
// whichever of the two this backend has. So a function can take one, and the lookup below is
// written once rather than once per map.

#define PI 3.14159265359

// How wide the occlusion filter is, in taps across. Odd, and the radius is half of it rounded down.
#define SHADOW_TAPS 5
#define SHADOW_RADIUS 2.0

// How much of a light reaches this surface: 1 in the open, 0 behind something, and part of the way
// along an edge.
//
// The map holds, for each texel, how far the nearest surface to the light is. So the question is
// whether this surface is that one or something behind it, and the whole of the difficulty is that
// both numbers are approximate. The map has a texel size, and a surface at a glancing angle to the
// light crosses several texels' worth of distance within one texel, so comparing exactly makes
// every lit surface stripe itself with its own shadow.
//
// Two things stop that. Some slack in the comparison, in metres. And taking the reading a little
// way out along the surface normal, which moves the lookup off the surface being tested rather than
// pushing the number around: that costs a thin skirt of missing shadow at the foot of a wall, and
// buys a surface that does not shadow itself at any angle. The normal offset is the load-bearing
// one for the sky map, where it also keeps the outside face of a wall out of its own roof's shade.
float lightReaches(sampler2D map, mat4 mtx, vec4 axis, vec4 params, vec3 P, vec3 N)
{
	if (params.z < 0.5)
	{
		return 1.0;
	}

	vec3 sampleAt = P + N * params.w;
	vec4 projected = mul(mtx, vec4(sampleAt, 1.0));
	vec2 uv = projected.xy / projected.w;
	// Outside the map is not "in shadow", it is "not known": the map only covers the ground around
	// the player, and the world carries on past it.
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
	{
		return 1.0;
	}
	float here = dot(sampleAt, axis.xyz) + axis.w;

	// Twenty-five taps on the map's own texel grid, weighted so the edges of the kernel carry the
	// fraction of a texel the sampling point sits at.
	//
	// Two things have to be true at once here and they pull against each other.
	//
	// The result has to move continuously as the sampling point moves, or a shadow edge climbs in
	// stairs: equal votes from a fixed set of texels can only produce as many shades as there are
	// taps, and the set changes in a jump at every texel boundary. That is what the weighting is for.
	//
	// And the taps have to land on whole texels. The map is snapped to its own texel grid each frame
	// so that walking does not slide the grid under every shadow in the world -- but that only holds
	// if what reads the map is on the same grid. Taps at a fractional spacing are not: point sampling
	// snaps each one to whichever texel it happens to land in, the assignment changes as the map
	// steps, and the shadow crawls and sparkles while the player walks. An earlier version spread the
	// taps 1.7 texels apart to soften the edge and bought exactly that flicker with it.
	//
	// So the kernel is widened by taking more taps rather than by spacing them further out.
	float size = 1.0 / max(params.x, 1e-6);
	vec2 texel = uv * size - vec2_splat(0.5);
	vec2 frac = texel - floor(texel);
	vec2 base = (floor(texel) + vec2_splat(0.5)) * params.x;

	// A box the width of the kernel with the two end taps sharing one texel between them, which is
	// what makes the whole thing slide smoothly rather than step as `frac` passes one.
	float weightX[SHADOW_TAPS];
	float weightY[SHADOW_TAPS];
	for (int i = 0; i < SHADOW_TAPS; ++i)
	{
		float edge = (i == 0) ? (1.0 - frac.x) : ((i == SHADOW_TAPS - 1) ? frac.x : 1.0);
		weightX[i] = edge;
		float edgeY = (i == 0) ? (1.0 - frac.y) : ((i == SHADOW_TAPS - 1) ? frac.y : 1.0);
		weightY[i] = edgeY;
	}

	float reached = 0.0;
	float total = 0.0;
	for (int y = 0; y < SHADOW_TAPS; ++y)
	{
		for (int x = 0; x < SHADOW_TAPS; ++x)
		{
			vec2 tap = base + vec2(float(x) - SHADOW_RADIUS, float(y) - SHADOW_RADIUS) * params.x;
			float nearest = texture2DLod(map, tap, 0.0).x;
			// Both numbers count from the back of the map, so the nearer surface to the light is
			// the larger one, and being lit means not falling short of it by more than the slack.
			float lit = (here + params.y >= nearest) ? 1.0 : 0.0;
			float weight = weightX[x] * weightY[y];
			reached += lit * weight;
			total += weight;
		}
	}
	return reached / max(total, 1e-6);
}

// The sun, from whichever of its two maps covers this point.
//
// The near one covers a few metres around the player at about a centimetre a texel; the far one
// covers the rest of what can be seen, at several. A single map cannot do both: made wide enough for
// a building its texels are coarse enough to show as steps on the shadow of your own head, and made
// fine enough for that it stops a few metres from your feet.
//
// The changeover is at the near map's own edge, pulled in slightly so the outermost texels -- the
// ones whose filter taps would fall outside the map -- are never the ones used.
float sunReaching(vec3 P, vec3 N)
{
	if (u_sunNearParams.z > 0.5)
	{
		vec4 nearClip = mul(u_sunNearMtx, vec4(P + N * u_sunNearParams.w, 1.0));
		vec2 nearUv = nearClip.xy / nearClip.w;
		if (nearUv.x > 0.03 && nearUv.x < 0.97 && nearUv.y > 0.03 && nearUv.y < 0.97)
		{
			return lightReaches(s_sunNear, u_sunNearMtx, u_sunNearAxis, u_sunNearParams, P, N);
		}
	}
	return lightReaches(s_sunShadow, u_sunShadowMtx, u_sunShadowAxis, u_sunShadowParams, P, N);
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
	float sunReaches = sunReaching(v_worldPos, N);
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
		vec3 lightRadiance = colorIntensity.rgb * colorIntensity.w * attenuation * cone;
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
	float skyReaches =
		lightReaches(s_skyShadow, u_skyShadowMtx, u_skyShadowAxis, u_skyShadowParams, v_worldPos, N);
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
