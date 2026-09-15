$input v_ray

#include <bgfx_shader.sh>

uniform vec4 u_skyZenith;  // rgb
uniform vec4 u_skyHorizon; // rgb
uniform vec4 u_skyGround;  // rgb
uniform vec4 u_skySun;     // xyz = direction towards the sun, w = how sharp the glow is
uniform vec4 u_skySunColor; // rgb, w = how bright
uniform vec4 u_skyGrade;    // x = exposure, y = contrast

void main()
{
	vec3 ray = normalize(v_ray);

	// Two gradients meeting at the horizon, not one. The sky does not fade smoothly from overhead to
	// underfoot: it is brightest just above the horizon, where the light has most air to scatter
	// through, and the ground below is a different colour entirely. A single lerp from zenith to
	// ground gives the flat grey band that says "programmer sky" from across a room.
	float up = ray.y;
	float height = pow(clamp(up, 0.0, 1.0), 0.45);
	vec3 color = mix(u_skyHorizon.rgb, u_skyZenith.rgb, height);
	// Below the horizon it turns towards the ground colour over a few degrees rather than at a line,
	// because a hard edge there reads as a wall rather than as distance.
	color = mix(color, u_skyGround.rgb, clamp(-up * 6.0, 0.0, 1.0));

	// And the sun, as a glow rather than a disc. A disc needs to be the right size and to survive
	// being looked straight at; a glow is what is actually visible through air and costs one dot
	// product. The tight core sits inside a wide halo, which is the difference between a light in
	// the sky and a white circle painted on it.
	float toSun = max(dot(ray, normalize(u_skySun.xyz)), 0.0);
	float halo = pow(toSun, max(u_skySun.w, 1.0));
	float core = pow(toSun, max(u_skySun.w, 1.0) * 24.0);
	color += u_skySunColor.rgb * u_skySunColor.w * (halo * 0.35 + core) * clamp(up * 4.0 + 0.4, 0.0, 1.0);

	// Through the same curve the world goes through, or the sky is the one thing on screen that was
	// not developed: colours written straight out land far darker than the same numbers do on a
	// surface, and the horizon reads as a black band where it should be haze.
	color *= max(u_skyGrade.x, 0.0);
	color = clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);
	color = pow(color, vec3_splat(1.0 / 2.2));
	color = clamp((color - vec3_splat(0.5)) * max(u_skyGrade.y, 0.0) + vec3_splat(0.5), 0.0, 1.0);

	// The same dither the world gets. A sky is the largest, smoothest gradient on screen and the
	// first place eight bits per channel shows as bands.
	float dither = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));
	color += vec3_splat((dither - 0.5) / 255.0);

	gl_FragColor = vec4(color, 1.0);
}
