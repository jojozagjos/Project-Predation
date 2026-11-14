// The body of the planets' fragment shader (fs_planet, and fs_planet_behind -- the same, its depth set behind everything else: the
// world under a site, round its land, never over it). Written apart from the two so neither copies it; only the one that needs it
// sets its own depth, which costs the others the hardware's early depth test.

// The system map's bodies (PlanetRenderer): a planet, a star, the glow round a star, or a planet's rings.
uniform vec4 u_planetA;          // ground colour A, sea's share
uniform vec4 u_planetB;          // ground colour B, ice
uniform vec4 u_planetC;          // sea colour, cloud
uniform vec4 u_planetD;          // cloud colour, 1 for a gas giant
uniform vec4 u_planetE;          // the air's colour, how much air
uniform vec4 u_planetLight;      // xyz towards the light, w how bright
uniform vec4 u_planetLightColor; // rgb the light's colour (a star's own, for a star), w 1 for linear light (the world's picture)
uniform vec4 u_planetMode;       // x what is drawn (0 body, 1 star, 2 glow, 3 rings, 4 body from its baked maps, 5 the air round its edge), y seed, z cloud drift, w (5) how far out its air goes, in edges
uniform vec4 u_planetEye;        // xyz where the camera is, w exposure
uniform vec4 u_planetRingPole; // xyz = the way its rings face (its pole), in the world; nought: no rings. (The air round its edge: its middle, w its radius)
uniform vec4 u_planetTrace;     // a body worked out exactly, a ray to its sphere for each pixel rather than its facets: its middle, w its radius (nought: none)
uniform vec4 u_planetBehind;     // y = the depth just in front of the cleared one: drawn behind everything else in the picture (fs_planet_behind: the world under a site, round its land)
uniform vec4 u_planetSite;      // a place on it coming down to: its middle in the body's frame, w the cosine of how far out it is all the land's colour (nought: none)
uniform vec4 u_planetSiteColor; // the land's colour there, as it is seen from high up; w the cosine of how far out it is gone into the world's own
uniform vec4 u_planetRingSize; // x, y = its rings, inside and outside, in its radii; z = its seed (their bands)
// A body's face baked (fs_planet_bake): colour (square-rooted) and open water; the ground's tip and the cloud.
SAMPLER2D(s_planetSurface, 0);
SAMPLER2D(s_planetDetail, 1);

// Where a direction on the body is in its baked maps -- longitude across, from the pole down -- taking whichever of two
// longitudes runs on smoothly here, so the line where it wraps round does not show (Tarini's trick).
vec2 BakedAt(vec3 d)
{
	float u = atan2(d.z, d.x) / 6.28318530718 + 0.5;
	float other = fract(u + 0.5) - 0.5;
	u = fwidth(u) <= fwidth(other) + 0.001 ? u : other;
	return vec2(u, acos(clamp(d.y, -1.0, 1.0)) / 3.14159265359);
}

vec3 Develop(vec3 colour)
{
	colour *= max(u_planetEye.w, 0.0);
	// Into the world's picture: light as it is, developed with everything else after.
	if (u_planetLightColor.w > 0.5)
	{
		return colour;
	}
	colour = clamp((colour * (2.51 * colour + 0.03)) / (colour * (2.43 * colour + 0.59) + 0.14), 0.0, 1.0);
	return pow(colour, vec3_splat(1.0 / 2.2));
}

void main()
{
	float mode = u_planetMode.x;
	vec3 V = normalize(u_planetEye.xyz - v_worldPos);
	if (mode > 4.5)
	{
		// The air round a world's edge (PlanetRenderer::Body): how far out from the edge as seen this point is, in edges (0 at the
		// edge, 1 at the shell's top); the light scattered along a line through the shell there, thickest grazing the ground and
		// thinning away up it; lit where the sun is on that part of the edge -- the day side bright, the night side's edge only a
		// thin line of light coming through from behind, brighter looking towards the star.
		vec2 p = (v_texcoord0 * 2.0 - vec2_splat(1.0)) * u_planetMode.w;
		float beyond = (length(p) - 1.0) / max(u_planetMode.w - 1.0, 1.0e-3);
		if (beyond > 1.0 || beyond < -0.35)
		{
			discard;
		}
		float above = max(beyond, 0.0);
		// (Full just inside the edge too: the world itself hides it wherever the world is, and its drawn outline is not exactly the
		// edge reckoned here -- a dark sliver showed between them.)
		float density = exp(-above * 3.2) * (1.0 - smoothstep(0.7, 1.0, beyond)) * smoothstep(-0.35, -0.2, beyond);
		// (The bright line right at the edge, where a line of sight crosses the most air: as photographs of a world's limb show.)
		density += 0.9 * exp(-above * 16.0) * smoothstep(-0.35, -0.2, beyond);
		vec3 L = normalize(u_planetLight.xyz);
		vec3 dir = normalize(v_worldPos - u_planetRingPole.xyz);
		float sun = dot(dir, L);
		float day = smoothstep(-0.3, 0.35, sun);
		float behind = pow(max(dot(-V, L), 0.0), 6.0);
		float lit = day + (1.0 - day) * (0.06 + 0.9 * behind) * smoothstep(-0.75, -0.1, sun);
		float thick = clamp(u_planetE.w, 0.0, 1.5);
		// Its colour: the air's own, paler where it is thickest (as a sky is paler at its horizon).
		vec3 tint = mix(u_planetE.rgb, vec3_splat(dot(u_planetE.rgb, vec3(0.3, 0.5, 0.2))) * 1.2, 0.12 * exp(-above * 10.0));
		// (Not so bright it is washed out white: its air's own colour, blue on a world like home, ochre in a hazy one.)
		// (Air scatters the blue of any star's light most, so a warm star's light tints it only half: its colour kept.)
		vec3 starlight = mix(vec3_splat(dot(u_planetLightColor.rgb, vec3(0.3, 0.5, 0.2))), u_planetLightColor.rgb, 0.5);
		vec3 colour = tint * starlight * u_planetLight.w * density * lit * (0.2 + 0.3 * min(thick, 1.0));
		gl_FragColor = vec4(Develop(colour), 1.0);
#ifdef PLANET_BEHIND
		gl_FragDepth = gl_FragCoord.z;
#endif
		return;
	}
	if (mode > 2.5 && mode < 3.5)
	{
		// Rings: banded across their width, thin at both edges, lit from whichever side the star is on.
		float t = v_texcoord0.x;
		// Their bands evened out where more of them fall in a pixel than it can show, and the rings themselves faded as they
		// come edge on: seen across, a ring a few pixels high with dozens of bands in each was a line of flickering noise.
		float across = abs(dFdx(t * 46.0)) + abs(dFdy(t * 46.0));
		float bands = mix(PlanetFbm(vec3(t * 46.0, u_planetMode.y, 0.5)), 0.5, smoothstep(0.3, 1.2, across));
		vec3 N = normalize(v_normal);
		float edgeOn = smoothstep(0.02, 0.12, abs(dot(N, V)));
		float alpha = smoothstep(0.0, 0.06, t) * smoothstep(1.0, 0.92, t) * (0.25 + 0.6 * bands) * edgeOn;
		float lit = abs(dot(N, normalize(u_planetLight.xyz))) * 0.75 + 0.25;
		vec3 colour = u_planetE.rgb * lit * u_planetLight.w * u_planetLightColor.rgb;
		gl_FragColor = vec4(Develop(colour), alpha);
#ifdef PLANET_BEHIND
		gl_FragDepth = gl_FragCoord.z;
#endif
		return;
	}
	if (mode > 1.5 && mode < 2.5)
	{
		// The corona round a star, on a square facing the camera: tight to the disc, faint streamers out from it, and a
		// soft glow beyond that falls off fast -- not a fog over the inner planets.
		vec2 p = v_texcoord0 * 2.0 - vec2_splat(1.0);
		float r = length(p);
		float angle = atan2(p.y, p.x);
		float streamers = 0.75 + 0.25 * PlanetNoise(vec3(cos(angle) * 3.0, sin(angle) * 3.0, u_planetMode.z * 0.02));
		float glow = exp(-max(r - 0.2, 0.0) * 14.0) * 0.9 * streamers + exp(-r * 7.0) * 0.12;
		glow *= smoothstep(1.0, 0.8, r);
		gl_FragColor = vec4(Develop(mix(u_planetLightColor.rgb, vec3_splat(1.0), 0.35) * glow * 0.9), 1.0);
#ifdef PLANET_BEHIND
		gl_FragDepth = gl_FragCoord.z;
#endif
		return;
	}
	vec3 n = normalize(v_objectPos);
	vec3 N = normalize(v_normal);
	// Seen close -- the shuttle coming down to it, a world filling the windows -- the sphere itself, not its facets: a facet of a
	// moon's is hundreds of metres deep, its horizon from a few kilometres up a line of them. A ray from the eye to the sphere.
	if (u_planetTrace.w > 0.0 && (mode < 0.5 || (mode > 3.5 && mode < 4.5)))
	{
		vec3 ray = normalize(v_worldPos - u_planetEye.xyz);
		vec3 oc = u_planetEye.xyz - u_planetTrace.xyz;
		float b = dot(oc, ray);
		float c = dot(oc, oc) - u_planetTrace.w * u_planetTrace.w;
		float h = b * b - c;
		if (h < 0.0)
		{
			discard;
		}
		float t = -b - sqrt(h);
		if (t < 0.0)
		{
			discard;
		}
		N = (oc + ray * t) / u_planetTrace.w;
		n = normalize(mul(vec4(N, 0.0), u_model[0]).xyz);
		V = -ray;
	}
	if (mode > 0.5 && mode < 1.5)
	{
		// A star: its surface a boil of small cells, a spot or two, and its limb darker and redder than its middle --
		// the way a real one looks through a filter, and white hot in the middle.
		float mu = max(dot(N, V), 0.0);
		vec3 drift = vec3_splat(u_planetMode.z * 0.05);
		float cells = PlanetNoise(n * 70.0 + drift) * 0.6 + PlanetNoise(n * 140.0 - drift) * 0.4;
		float faculae = PlanetFbmN(n * 9.0 + drift, 4);
		float spots = smoothstep(0.7, 0.78, PlanetFbmN(n * 4.0 + vec3_splat(u_planetMode.y), 4));
		vec3 hot = mix(u_planetLightColor.rgb, vec3_splat(1.0), 0.55);
		vec3 rim = u_planetLightColor.rgb * vec3(1.0, 0.62, 0.36);
		vec3 surface = mix(rim, hot, pow(mu, 0.5)) * (0.5 + 0.5 * pow(mu, 0.6));
		surface *= 0.82 + 0.3 * cells + 0.12 * faculae;
		surface *= 1.0 - spots * 0.7;
		gl_FragColor = vec4(Develop(surface * 3.4), 1.0);
#ifdef PLANET_BEHIND
		gl_FragDepth = gl_FragCoord.z;
#endif
		return;
	}

	float sea = 0.0;
	vec4 surface;
	vec3 bumped;
	// The cloud seen as the three-dimensional thing it is, from a body's maps: its tops lit on the side towards the star and dark
	// in their own lee, and its shadows cast on the ground and the sea beside it.
	float cloudRelief = 1.0;
	float cloudShadow = 1.0;
	if (mode > 3.5)
	{
		// Read from its maps: the same as worked out below, at a fraction of the cost.
		vec4 ground = texture2D(s_planetSurface, BakedAt(n));
		vec4 tip = texture2D(s_planetDetail, BakedAt(n));
		// (Turned as its winds carry it, a world's cloud going round it in a few hours -- not in minutes: u_planetMode.z is the clock
		// times 0.02, for the storms' flicker below too.)
		float drift = u_planetMode.z * 0.03;
		vec3 m = vec3(n.x * cos(drift) - n.z * sin(drift), n.y, n.x * sin(drift) + n.z * cos(drift));
		surface = vec4(ground.rgb * ground.rgb, texture2D(s_planetDetail, BakedAt(m)).z);
		sea = ground.a;
		vec3 t1 = normalize(cross(n, abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
		vec3 t2 = cross(n, t1);
		bumped = normalize(n + t1 * tip.x + t2 * tip.y);
		// Seen close -- from a low orbit, a texel of its maps bigger than a pixel -- finer than the maps hold, worked out here:
		// the ground's grain and its roughness in the light, and the cloud's edges broken into wisps. Nothing of it when
		// the body is small, where the maps already hold more than the screen does.
		float texels = length(fwidth(n)) * 2048.0 / 6.28318530718;
		float close = 1.0 - smoothstep(0.35, 1.2, texels);
		if (close > 0.001)
		{
			vec3 offset = vec3(u_planetMode.y * 1.7, u_planetMode.y * 0.9, u_planetMode.y * 2.3);
			float grain = PlanetFbmN(n * 170.0 + offset, 4);
			surface.rgb *= mix(1.0, 0.78 + 0.44 * grain, close * (1.0 - sea));
			float wisps = PlanetFbmN(m * 24.0 + offset * 3.0, 4);
			float broken = clamp(surface.w + (wisps - 0.5) * 0.35 * (1.0 - abs(surface.w * 2.0 - 1.0) * 0.6), 0.0, 1.0);
			surface.w = mix(surface.w, smoothstep(0.05, 0.95, broken), close);
			float rough = PlanetFbmN(n * 240.0 + offset * 0.5, 3) - 0.5;
			bumped = normalize(bumped + (t1 * rough - t2 * rough * 0.6) * 0.12 * close * (1.0 - sea));
		}
		{
			vec3 Lo = normalize(v_lightObj);
			vec3 Lm = vec3(Lo.x * cos(drift) - Lo.z * sin(drift), Lo.y, Lo.x * sin(drift) + Lo.z * cos(drift));
			float sunward = texture2D(s_planetDetail, BakedAt(normalize(m + Lm * 0.012))).z;
			cloudRelief = clamp(1.0 - (sunward - surface.w) * 1.6, 0.5, 1.2);
			float over = texture2D(s_planetDetail, BakedAt(normalize(m + Lm * 0.025))).z;
			cloudShadow = 1.0 - 0.6 * over * (1.0 - surface.w);
		}
	}
	else
	{
		// Cratered as far as it has no air to speak of to wear them away; a gas giant has no ground.
		float craters = u_planetD.w > 0.5 ? 0.0 : 1.0 - smoothstep(0.02, 0.15, u_planetE.w);
		surface = PlanetAlbedo(n, u_planetA, u_planetB, u_planetC, u_planetD, u_planetMode.y, u_planetMode.z * 0.03, craters, sea);
		// The lie of the land in the light (exact when the body is drawn unturned, as the globe view draws it).
		bumped = PlanetBump(n, u_planetA, u_planetD, u_planetMode.y, 1.0, craters);
	}
	// The place being come down to, the colour the land round it is from up there (u_planetSite): so the land, coming up out of the
	// distance, is the globe and the globe the land -- no seam where the one meets the other, and none as the one becomes the other.
	float siteNear = 0.0;
	if (u_planetSite.w > 0.0)
	{
		siteNear = smoothstep(u_planetSiteColor.w, u_planetSite.w, dot(n, u_planetSite.xyz));
		// (Its craters, its grain and its lie kept, only in the land's colour: from high up it is still a cratered, rough world being
		// come down to, not a blank sheet; the land fades at its rim into the same colour on average.)
		vec3 usual = max((u_planetA.rgb + u_planetB.rgb) * 0.5, vec3_splat(0.02));
		vec3 tinted = u_planetSiteColor.rgb * mix(vec3_splat(1.0), clamp(surface.rgb / usual, 0.0, 2.5), 0.7);
		// (Its land only: its sea kept as sea -- tinted the land's colour, the sea round a coast or over a site on the sea's floor
		// was a brown plain from up there.)
		surface.rgb = mix(surface.rgb, tinted, siteNear * (1.0 - sea));
		cloudShadow = mix(cloudShadow, 1.0, siteNear);
	}
	vec3 L = normalize(u_planetLight.xyz);
	float facing = dot(N, L);
	// A soft edge between day and night: an atmosphere carries a little light round it.
	float lit = smoothstep(-0.1, 0.3, facing);
	// Its rings' shadow across it: from where this point is, towards the star, through the rings' plane -- where that is
	// within them, as much of the light kept off as the rings are thick there.
	if (u_planetRingSize.y > 0.0)
	{
		vec3 pole = normalize(u_planetRingPole.xyz);
		vec3 here = normalize(N);
		float towards = dot(L, pole);
		float t = abs(towards) > 1.0e-4 ? -dot(here, pole) / towards : -1.0;
		if (t > 0.0)
		{
			float r = length(here + L * t);
			float across = (r - u_planetRingSize.x) / max(u_planetRingSize.y - u_planetRingSize.x, 1.0e-3);
			if (across > 0.0 && across < 1.0)
			{
				float bands = PlanetFbm(vec3(across * 46.0, u_planetRingSize.z, 0.5));
				float thick = smoothstep(0.0, 0.06, across) * smoothstep(1.0, 0.92, across) * (0.25 + 0.6 * bands);
				lit *= 1.0 - 0.85 * thick;
			}
		}
	}
	vec3 bumpedN = normalize(N + (bumped - n));
	float relief = clamp(1.0 + (dot(bumpedN, L) - facing) * 1.8, 0.5, 1.45);
	vec3 albedo = mix(surface.rgb * relief * cloudShadow, u_planetD.rgb * cloudRelief, surface.w);
	vec3 light = u_planetLightColor.rgb * u_planetLight.w;
	// A little light on the night side, so a world seen from behind is still a world and not a hole.
	// (The cloud tops, high up, catch the light a little past where the ground under them has gone dark.)
	float litCloud = smoothstep(-0.2, 0.25, facing);
	vec3 colour = albedo * (mix(lit, litCloud, surface.w) * light + vec3_splat(0.05));
	// And the air over the day side: a veil of its colour thickening towards the limb, over ground and cloud alike.
	colour = mix(colour, u_planetE.rgb * light * 0.55, clamp(u_planetE.w, 0.0, 1.0) * pow(1.0 - max(dot(N, V), 0.0), 2.0) * 0.35 * lit * (1.0 - siteNear));
	// A stormy world's storms flickering with lightning (thunderstorms and ash storms: their storms are over 0.6, a dust
	// storm's or a blizzard's under it), seen from space on its night side: a few storms at a time, each now and then lit up
	// from inside for a moment -- a soft glow some hundreds of kilometres across, flickering, broken up by the cloud it is in
	// -- and somewhere else the next time.
	float storms = clamp(-u_planetD.w, 0.0, 1.0);
	if (storms > 0.6 && surface.w > 0.4 && u_planetE.w > 0.015)
	{
		float seconds = u_planetMode.z * 50.0; // (the cloud drift is the clock times 0.02)
		float flash = 0.0;
		for (int k = 0; k < 6; ++k)
		{
			float fk = float(k);
			float t = seconds * 0.7 + fk * 0.618;
			float slot = floor(t);
			float phase = fract(t);
			vec3 at = normalize(vec3(PlanetHash(vec3(slot, fk, 1.3)), PlanetHash(vec3(slot, fk, 7.9)), PlanetHash(vec3(slot, fk, 4.1))) * 2.0 - 1.0);
			float on = step(PlanetHash(vec3(slot, fk, 2.7)), 0.55 * storms);
			float pulse = exp(-phase * 9.0) * (0.65 + 0.35 * sin(phase * 90.0 + fk));
			flash += on * pulse * exp(-(1.0 - dot(n, at)) * 900.0);
		}
		float cloudy = smoothstep(0.35, 0.75, PlanetFbm(n * 40.0 + vec3_splat(u_planetMode.y)) + surface.w * 0.3);
		colour += vec3(0.7, 0.75, 1.0) * flash * cloudy * smoothstep(0.4, 0.8, surface.w) * (1.0 - lit) * 1.2;
	}
	// The sun off open water: a hard bright core where it mirrors the sun and a broad sheen round it, spread by the sea's waves --
	// what makes an ocean world read as water from space, not a blue ground -- and the sky in it, brighter towards its edge.
	float mirror = max(dot(reflect(-L, N), V), 0.0);
	float open = sea * (1.0 - surface.w) * step(0.0, facing);
	float glint = (pow(mirror, 400.0) * 3.0 + pow(mirror, 40.0) * 0.55 + pow(mirror, 8.0) * 0.07) * open;
	colour += u_planetE.rgb * light * lit * open * pow(1.0 - max(dot(N, V), 0.0), 4.0) * 0.35;
	colour += light * glint * 0.6;
	// The air at the edge, on the lit side and a little past it.
	float edge = pow(1.0 - max(dot(N, V), 0.0), 3.0);
	colour += u_planetE.rgb * u_planetE.w * edge * (smoothstep(-0.3, 0.3, facing) + 0.02) * light * 0.9 * (1.0 - siteNear);
	gl_FragColor = vec4(Develop(colour), 1.0);
	// The world under a site: behind all its land wherever that is, near or far -- never a sheet over it.
#ifdef PLANET_BEHIND
	gl_FragDepth = u_planetBehind.y;
#endif
}
