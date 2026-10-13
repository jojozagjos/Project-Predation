$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_scene, 0); // what the picture's brightness is now (fs_post_avg)
SAMPLER2D(s_bloom, 1); // what the eye had adapted to by last frame
uniform vec4 u_postAdaptRate; // x = seconds since last frame, y = how quickly to brighter, z = to darker, w = 1: start again

// The eye coming round to what it sees: to bright light quickly, to the dark slowly -- the dazzle stepping out of a dark
// room into day, and the dark room slowly showing itself after.
void main()
{
	float now = texture2D(s_scene, vec2_splat(0.5)).r;
	float before = texture2D(s_bloom, vec2_splat(0.5)).r;
	float rate = now > before ? u_postAdaptRate.y : u_postAdaptRate.z;
	float adapted = u_postAdaptRate.w > 0.5 ? now : mix(before, now, 1.0 - exp(-u_postAdaptRate.x * rate));
	gl_FragColor = vec4(adapted, 0.0, 0.0, 1.0);
}
