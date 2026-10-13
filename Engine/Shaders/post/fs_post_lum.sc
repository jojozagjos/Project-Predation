$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_scene, 0);
uniform vec4 u_postTexel; // xy = one texel of the source

// How bright the picture is, a small square of it at a time (64 across, for the eye to adapt by -- PostProcess): the light
// in a few places over each texel of this, averaged, as its logarithm, so a lamp in the picture does not outweigh the room.
void main()
{
	vec2 t = u_postTexel.xy * 6.0;
	vec3 c = texture2D(s_scene, v_texcoord0 + vec2(-t.x, -t.y)).rgb + texture2D(s_scene, v_texcoord0 + vec2(t.x, -t.y)).rgb +
	         texture2D(s_scene, v_texcoord0 + vec2(-t.x, t.y)).rgb + texture2D(s_scene, v_texcoord0 + vec2(t.x, t.y)).rgb;
	float luminance = dot(c * 0.25, vec3(0.2126, 0.7152, 0.0722));
	gl_FragColor = vec4(log(max(luminance, 1.0e-4)), 0.0, 0.0, 1.0);
}
