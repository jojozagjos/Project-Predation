$input v_worldPos, v_normal, v_texcoord0

#include <bgfx_shader.sh>

uniform vec4 u_baseColor;       // rgb = albedo
uniform vec4 u_materialParams;  // x = metallic, y = roughness
uniform vec4 u_emissive;        // rgb = emissive radiance
uniform vec4 u_lightDirection;  // xyz = direction towards the light
uniform vec4 u_lightColor;      // rgb = colour, w = intensity
uniform vec4 u_ambientSky;      // rgb = light from above
uniform vec4 u_ambientGround;   // rgb = bounce from below
uniform vec4 u_fogColor;        // rgb
uniform vec4 u_fogParams;       // x = start distance, y = end distance
uniform vec4 u_cameraPosition;  // xyz

#define PI 3.14159265359

// GGX / Trowbridge-Reitz normal distribution.
float distributionGGX(float NoH, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float d = NoH * NoH * (a2 - 1.0) + 1.0;
	return a2 / max(PI * d * d, 1e-6);
}

// Height-correlated Smith visibility term (already divided by the 4*NoL*NoV denominator).
float visibilitySmith(float NoV, float NoL, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float lambdaV = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
	float lambdaL = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
	return 0.5 / max(lambdaV + lambdaL, 1e-6);
}

vec3 fresnelSchlick(vec3 f0, float VoH)
{
	float f = pow(1.0 - VoH, 5.0);
	return f0 + (vec3_splat(1.0) - f0) * f;
}

void main()
{
	vec3 N = normalize(v_normal);
	vec3 V = normalize(u_cameraPosition.xyz - v_worldPos);
	vec3 L = normalize(u_lightDirection.xyz);
	vec3 H = normalize(L + V);

	float NoL = max(dot(N, L), 0.0);
	float NoV = max(dot(N, V), 1e-4);
	float NoH = max(dot(N, H), 0.0);
	float VoH = max(dot(V, H), 0.0);

	vec3 albedo = u_baseColor.rgb;
	float metallic = clamp(u_materialParams.x, 0.0, 1.0);
	// Clamp roughness away from zero: perfectly smooth surfaces alias badly with a single light.
	float roughness = clamp(u_materialParams.y, 0.045, 1.0);

	vec3 diffuseColor = albedo * (1.0 - metallic);
	vec3 f0 = mix(vec3_splat(0.04), albedo, metallic);

	// Direct lighting from the sun.
	float D = distributionGGX(NoH, roughness);
	float Vis = visibilitySmith(NoV, NoL, roughness);
	vec3 F = fresnelSchlick(f0, VoH);

	vec3 specular = D * Vis * F;
	vec3 diffuse = diffuseColor / PI;
	vec3 radiance = u_lightColor.rgb * u_lightColor.w;
	vec3 color = (diffuse + specular) * radiance * NoL;

	// Hemispheric ambient stands in for indirect light until there is a real probe system.
	float hemisphere = N.y * 0.5 + 0.5;
	vec3 ambient = mix(u_ambientGround.rgb, u_ambientSky.rgb, hemisphere);
	color += diffuseColor * ambient;

	// Metals have no diffuse, so without an ambient specular term they render black wherever the
	// sun does not hit them. This approximates a uniform environment reflection until there is one.
	vec3 ambientFresnel = fresnelSchlick(f0, NoV) * (1.0 - roughness);
	color += ambient * mix(f0, ambientFresnel, 0.5);

	color += u_emissive.rgb;

	// Linear distance fog. Cheap, and it does most of the atmospheric work in dark interiors.
	float distanceToCamera = length(u_cameraPosition.xyz - v_worldPos);
	float fogAmount = clamp((distanceToCamera - u_fogParams.x) / max(u_fogParams.y - u_fogParams.x, 1e-4), 0.0, 1.0);
	color = mix(color, u_fogColor.rgb, fogAmount);

	// Reinhard tonemap, then encode to gamma space for the non-sRGB backbuffer.
	color = color / (color + vec3_splat(1.0));
	color = pow(color, vec3_splat(1.0 / 2.2));

	gl_FragColor = vec4(color, 1.0);
}
