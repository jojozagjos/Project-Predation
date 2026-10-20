$input v_texcoord0

#include <bgfx_shader.sh>

// How shut in each point of the picture is (ambient occlusion): how much of the open half of the world round it, out to a
// fraction of a metre, is taken up by other surfaces -- a corner, the floor under a crate, the ground at a boot, the back of
// a shelf. Worked out from the depth alone: where each pixel is, which way it faces, and a dozen points round it, each
// checked against what the picture has in front of it there. Noisy by itself, a different turn of the dozen each pixel;
// fs_post_aoblur smooths it. 1 open, 0 shut in.

SAMPLER2D(s_scene, 0); // the scene's depth
uniform vec4 u_postTexel; // xy = one pixel, zw = the screen in pixels
uniform vec4 u_postFlip;  // x = 1 when the picture's first row is its bottom
uniform mat4 u_aoProj;    // the picture's projection, and back
uniform mat4 u_aoInvProj;
uniform vec4 u_aoParams;  // x = how much (the final pass's), y = how far round it looks, metres; z = the depth of nothing
uniform vec4 u_aoFog;     // x = where the fog starts, y = where it is all fog (metres from the eye)
                          // drawn (the sky); w = 1 when depth runs -1 to 1

vec3 ViewAt(vec2 uv)
{
	float depth = texture2DLod(s_scene, uv, 0.0).r;
	vec2 ndc = vec2(uv.x * 2.0 - 1.0, u_postFlip.x > 0.5 ? uv.y * 2.0 - 1.0 : 1.0 - uv.y * 2.0);
	vec4 at = mul(u_aoInvProj, vec4(ndc, u_aoParams.w > 0.5 ? depth * 2.0 - 1.0 : depth, 1.0));
	return at.xyz / at.w;
}

vec2 ScreenOf(vec3 at)
{
	vec4 clip = mul(u_aoProj, vec4(at, 1.0));
	vec2 ndc = clip.xy / clip.w;
	return vec2(ndc.x * 0.5 + 0.5, u_postFlip.x > 0.5 ? ndc.y * 0.5 + 0.5 : 0.5 - ndc.y * 0.5);
}

void main()
{
	vec2 uv = v_texcoord0;
	float depth = texture2DLod(s_scene, uv, 0.0).r;
	if (abs(depth - u_aoParams.z) < 1.0e-7)
	{
		gl_FragColor = vec4_splat(1.0);
		return;
	}
	vec3 here = ViewAt(uv);
	// Which way it faces, from its neighbours -- the nearer of the two each way, so an edge against something far behind
	// does not bend it.
	vec2 across = vec2(u_postTexel.x, 0.0);
	vec2 up = vec2(0.0, u_postTexel.y);
	vec3 right = ViewAt(uv + across) - here;
	vec3 left = here - ViewAt(uv - across);
	vec3 above = ViewAt(uv + up) - here;
	vec3 below = here - ViewAt(uv - up);
	vec3 dx = abs(right.z) < abs(left.z) ? right : left;
	vec3 dy = abs(above.z) < abs(below.z) ? above : below;
	vec3 n = normalize(cross(dx, dy));
	if (dot(n, here) > 0.0)
	{
		n = -n;
	}
	vec3 t = normalize(cross(n, abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 b = cross(n, t);

	// A dozen points in the half of the world it faces, out to the radius, more of them near: turned by a different amount
	// at each pixel, so what is left over is fine noise for the blur rather than rings.
	float radius = u_aoParams.y;
	float turn = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) * 6.2831853;
	float shut = 0.0;
	for (int i = 0; i < 12; ++i)
	{
		float fi = float(i);
		float angle = turn + fi * 2.3999632;
		float rise = 0.15 + 0.85 * fract(fi * 0.618034 + turn * 0.159);
		float level = sqrt(1.0 - rise * rise);
		vec3 way = t * (cos(angle) * level) + b * (sin(angle) * level) + n * rise;
		float share = (fi + 0.5) / 12.0;
		vec3 probe = here + way * (radius * mix(0.12, 1.0, share * share));
		vec2 at = ScreenOf(probe);
		if (at.x < 0.0 || at.x > 1.0 || at.y < 0.0 || at.y > 1.0)
		{
			continue;
		}
		vec3 there = ViewAt(at);
		// Shut in where the picture has a surface in front of the probe -- but not by something far in front, which is
		// another thing altogether, not this one's surroundings.
		float inFront = there.z - probe.z;
		float close = 1.0 - smoothstep(radius, radius * 3.0, abs(there.z - here.z));
		shut += step(0.015, inFront) * close;
	}
	float open = 1.0 - shut / 12.0;
	// And less of it far off, where a fraction of a metre is a pixel or two.
	open = mix(open, 1.0, smoothstep(35.0, 70.0, -here.z));
	// And none of it through fog, which the picture under it has already faded into (laid on after, it showed through).
	open = mix(open, 1.0, clamp((length(here) - u_aoFog.x) / max(u_aoFog.y - u_aoFog.x, 1.0e-3), 0.0, 1.0));
	gl_FragColor = vec4_splat(open);
}
