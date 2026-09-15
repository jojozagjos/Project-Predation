$input a_position
$output v_depth

#include <bgfx_shader.sh>

// The depth pass, run once from the sun and once from straight overhead.
//
// What it writes is not the hardware depth buffer but a number in metres: how far this surface is
// from the light along the light's own axis. A depth buffer would do, and every backend stores it
// differently and with a different amount of precision near the near plane. A metre is a metre, the
// bias that goes with it is in metres too, and the same shader serves an orthographic sun and an
// orthographic sky with nothing to convert between them.
void main()
{
	vec4 worldPosition = mul(u_model[0], vec4(a_position, 1.0));
	gl_Position = mul(u_viewProj, worldPosition);
	// The view matrix here is the light's. Its -Z is the direction the light travels, so negating
	// gives a distance that grows as the surface gets further from the light.
	v_depth = vec4(-mul(u_view, worldPosition).z, 0.0, 0.0, 1.0);
}
