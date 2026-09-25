$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_scene, 0);
uniform vec4 u_postTexel; // xy = one texel of the source

// Half the size again, blurring as it goes: four bilinear reads a texel off each corner.
void main()
{
	vec2 t = u_postTexel.xy;
	vec3 c = texture2D(s_scene, v_texcoord0 + vec2(-t.x, -t.y)).rgb + texture2D(s_scene, v_texcoord0 + vec2(t.x, -t.y)).rgb +
	         texture2D(s_scene, v_texcoord0 + vec2(-t.x, t.y)).rgb + texture2D(s_scene, v_texcoord0 + vec2(t.x, t.y)).rgb;
	gl_FragColor = vec4(c * 0.25, 1.0);
}
