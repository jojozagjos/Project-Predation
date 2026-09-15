// The occlusion lookup, kept out of the shading function.
//
// Twenty-five taps in the middle of the lighting is a wall of code in the way of the thing anybody
// opens that file to read. It is an include rather than a plain function because it was compiled
// twice for a while, at two kernel sizes, and the next thing that wants a second size will want that
// back. Define KERNEL_NAME, KERNEL_TAPS and KERNEL_RADIUS before including.

float KERNEL_NAME(sampler2D map, mat4 mtx, vec4 axis, vec4 params, vec3 P, vec3 N, float bias)
{
	if (params.z < 0.5)
	{
		return 1.0;
	}

	vec3 sampleAt = P + N * params.w;
	vec4 projected = mul(mtx, vec4(sampleAt, 1.0));
	vec2 uv = projected.xy / projected.w;
	// Outside the map is not "in shadow", it is "not known": the map only covers the ground around
	// the player, and the world carries on past it.
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
	{
		return 1.0;
	}
	// And faded out before that edge rather than cut off at it.
	//
	// A hard boundary means every shadow in the level ends on the same circle around the player, and
	// walking moves that circle: shadows switch on and off as the edge sweeps over them, which is
	// what "shadows start to un-render when not that far away" is. They still stop -- the map cannot
	// cover the world -- but over the last tenth of it rather than at a line, so a shadow thins out
	// instead of blinking.
	vec2 fromEdge = min(uv, vec2_splat(1.0) - uv);
	float edgeFade = clamp(min(fromEdge.x, fromEdge.y) / 0.05, 0.0, 1.0);
	if (edgeFade <= 0.0)
	{
		return 1.0;
	}
	float here = dot(sampleAt, axis.xyz) + axis.w;

	// Twenty-five taps on the map's own texel grid, weighted so the edges of the kernel carry the
	// fraction of a texel the sampling point sits at.
	//
	// Two things have to be true at once here and they pull against each other.
	//
	// The result has to move continuously as the sampling point moves, or a shadow edge climbs in
	// stairs: equal votes from a fixed set of texels can only produce as many shades as there are
	// taps, and the set changes in a jump at every texel boundary. That is what the weighting is for.
	//
	// And the taps have to land on whole texels. The map is snapped to its own texel grid each frame
	// so that walking does not slide the grid under every shadow in the world -- but that only holds
	// if what reads the map is on the same grid. Taps at a fractional spacing are not: point sampling
	// snaps each one to whichever texel it happens to land in, the assignment changes as the map
	// steps, and the shadow crawls and sparkles while the player walks. An earlier version spread the
	// taps 1.7 texels apart to soften the edge and bought exactly that flicker with it.
	//
	// So the kernel is widened by taking more taps rather than by spacing them further out.
	float size = 1.0 / max(params.x, 1e-6);
	vec2 texel = uv * size - vec2_splat(0.5);
	vec2 frac = texel - floor(texel);
	vec2 base = (floor(texel) + vec2_splat(0.5)) * params.x;

	// A box the width of the kernel with the two end taps sharing one texel between them, which is
	// what makes the whole thing slide smoothly rather than step as `frac` passes one.
	float weightX[KERNEL_TAPS];
	float weightY[KERNEL_TAPS];
	for (int i = 0; i < KERNEL_TAPS; ++i)
	{
		float edge = (i == 0) ? (1.0 - frac.x) : ((i == KERNEL_TAPS - 1) ? frac.x : 1.0);
		weightX[i] = edge;
		float edgeY = (i == 0) ? (1.0 - frac.y) : ((i == KERNEL_TAPS - 1) ? frac.y : 1.0);
		weightY[i] = edgeY;
	}

	float reached = 0.0;
	float total = 0.0;
	for (int y = 0; y < KERNEL_TAPS; ++y)
	{
		for (int x = 0; x < KERNEL_TAPS; ++x)
		{
			vec2 tap = base + vec2(float(x) - KERNEL_RADIUS, float(y) - KERNEL_RADIUS) * params.x;
			float nearest = texture2DLod(map, tap, 0.0).x;
			// Both numbers count from the back of the map, so the nearer surface to the light is the
			// larger one, and being lit means not falling short of it by more than the slack. The
			// slack is worked out per fragment and handed in: see where this is called.
			float lit = (here + bias >= nearest) ? 1.0 : 0.0;
			float weight = weightX[x] * weightY[y];
			reached += lit * weight;
			total += weight;
		}
	}
	return mix(1.0, reached / max(total, 1e-6), edgeFade);
}


#undef KERNEL_NAME
#undef KERNEL_TAPS
#undef KERNEL_RADIUS
