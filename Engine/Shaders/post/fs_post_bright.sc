$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_scene, 0);
uniform vec4 u_postTexel; // xy = one texel of the source
uniform vec4 u_postTone;  // x = exposure, w = the brightness bloom starts at, once exposed

// What glows: the parts of the picture brighter than the threshold once exposed, at half size. A soft
// knee, so a lamp does not switch from not glowing to glowing across one step of brightness; and a cap,
// so one stray very bright pixel does not become a disc.
void main()
{
	vec2 t = u_postTexel.xy;
	vec3 c = texture2D(s_scene, v_texcoord0 + vec2(-t.x, -t.y)).rgb + texture2D(s_scene, v_texcoord0 + vec2(t.x, -t.y)).rgb +
	         texture2D(s_scene, v_texcoord0 + vec2(-t.x, t.y)).rgb + texture2D(s_scene, v_texcoord0 + vec2(t.x, t.y)).rgb;
	c *= 0.25;
	float exposed = max(max(c.r, c.g), c.b) * max(u_postTone.x, 0.0);
	float knee = u_postTone.w * 0.5;
	float soft = clamp(exposed - u_postTone.w + knee, 0.0, 2.0 * knee);
	soft = soft * soft / (4.0 * knee + 1e-4);
	float over = max(soft, exposed - u_postTone.w) / max(exposed, 1e-4);
	gl_FragColor = vec4(min(c * over, vec3_splat(16.0)), 1.0);
}
