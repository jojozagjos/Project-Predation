$input a_position
$output v_ray

#include <bgfx_shader.sh>

uniform mat4 u_skyRays; // clip space back to a world direction

// One triangle covering the screen, with the direction each pixel looks in carried across it.
//
// A box or a sphere around the camera would need geometry, a size chosen against the far plane, and
// care that the player never reaches the edge of it. A triangle needs none of that: the sky is not a
// thing in the world, it is what is left when nothing in the world was drawn.
void main()
{
	gl_Position = vec4(a_position.xy, 1.0, 1.0);
	vec4 ray = mul(u_skyRays, vec4(a_position.xy, 1.0, 1.0));
	v_ray = ray.xyz / ray.w;
}
