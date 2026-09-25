$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_scene, 0);
uniform vec4 u_postTexel; // xy = one texel of the smaller level being brought up

// The smaller level spread back over the larger one, added to it: a tent over eight neighbours, so
// the glow is round and soft rather than square.
void main()
{
	vec2 t = u_postTexel.xy;
	vec3 c = texture2D(s_scene, v_texcoord0).rgb * 4.0;
	c += (texture2D(s_scene, v_texcoord0 + vec2(-t.x, 0.0)).rgb + texture2D(s_scene, v_texcoord0 + vec2(t.x, 0.0)).rgb +
	      texture2D(s_scene, v_texcoord0 + vec2(0.0, -t.y)).rgb + texture2D(s_scene, v_texcoord0 + vec2(0.0, t.y)).rgb) * 2.0;
	c += texture2D(s_scene, v_texcoord0 + vec2(-t.x, -t.y)).rgb + texture2D(s_scene, v_texcoord0 + vec2(t.x, -t.y)).rgb +
	     texture2D(s_scene, v_texcoord0 + vec2(-t.x, t.y)).rgb + texture2D(s_scene, v_texcoord0 + vec2(t.x, t.y)).rgb;
	gl_FragColor = vec4(c / 16.0, 1.0);
}
