
/* Based on Frosbite Unified Volumetric.
 * https://www.ea.com/frostbite/news/physically-based-unified-volumetric-rendering-in-frostbite */

#define NODETREE_EXEC

uniform ivec3 volumeTextureSize;
uniform vec3 volume_jitter;

flat in int slice;

/* Warning: theses are not attributes, theses are global vars. */
vec3 worldPosition = vec3(0.0);
vec3 viewPosition = vec3(0.0);
vec3 viewNormal = vec3(0.0);

layout(location = 0) out vec4 volumeScattering;
layout(location = 1) out vec4 volumeExtinction;
layout(location = 2) out vec4 volumeEmissive;
layout(location = 3) out vec4 volumePhase;

/* Store volumetric properties into the froxel textures. */

void main()
{
	ivec3 volume_cell = ivec3(gl_FragCoord.xy, slice);
	vec3 ndc_cell = volume_to_ndc((vec3(volume_cell) + volume_jitter) / volumeTextureSize);

	viewPosition = get_view_space_from_depth(ndc_cell.xy, ndc_cell.z);
	worldPosition = transform_point(ViewMatrixInverse, viewPosition);

	Closure cl = nodetree_exec();

	volumeScattering = vec4(cl.scatter, 1.0);
	volumeExtinction = vec4(max(vec3(1e-4), cl.absorption + cl.scatter), 1.0);
	volumeEmissive = vec4(cl.emission, 1.0);
	volumePhase = vec4(cl.anisotropy, vec3(1.0));
}