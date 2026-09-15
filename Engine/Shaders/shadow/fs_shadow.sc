$input v_depth

#include <bgfx_shader.sh>

uniform vec4 u_shadowRange; // x = how far this map reaches along its axis, in metres

void main()
{
	// Stored upside down: how far this surface is from the *back* of the map, not from the light.
	//
	// The difference matters for a texel nothing was drawn into. Storing distance-from-the-light
	// means an empty texel has to hold a very large number to mean "nothing is in the way", and the
	// only way to put a number like that into a float target is a clear palette entry, which is one
	// more thing that has to be working for the lighting to be right. Stored this way an empty texel
	// holds zero, zero means "as far away as this map can see", and far away is exactly what "there
	// is nothing here" should mean. The comparison at the other end is simply reversed.
	gl_FragColor = vec4(u_shadowRange.x - v_depth.x, 0.0, 0.0, 1.0);
}
