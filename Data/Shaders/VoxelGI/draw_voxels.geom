#version 450

// receive voxels points position
layout(points) in;
// outputs voxels as cubes
layout(triangle_strip, max_vertices = 24) out;


layout(set = 0, binding = 1) uniform uVoxelInfo
{
	mat4 modelViewProjection;
	vec4 frustumPlanes[6];
	vec4 worldMinPointVoxelSize;
} ubo;

// Input from vertex shader
layout(location = 0) in GS_IN {
	vec4 albedo;
} gs_in[];

layout(location = 0) out vec4 voxelColor;

bool VoxelInFrustum(vec3 center, vec3 extent)
{
	vec4 plane;

	for(int i = 0; i < 6; i++)
	{
		plane = ubo.frustumPlanes[i];
		float d = dot(extent, abs(plane.xyz));
		float r = dot(center, plane.xyz) + plane.w;

		if(d + r > 0.0f == false)
		{
			return false;
		}
	}

	return true;
}

vec3 VoxelToWorld(vec3 pos)
{
	vec3 result = pos;
	result *= ubo.worldMinPointVoxelSize.w;

	return result + ubo.worldMinPointVoxelSize.xyz;
}

void main()
{
	if(gs_in[0].albedo.a == 0.0f) { return; }

	const vec4 cubeVertices[8] = vec4[8]
	(
		// Y coordinates inverted for Vulkan's NDC
		vec4( 0.5f, -0.5f,  0.5f, 0.0f), // 0
		vec4( 0.5f, -0.5f, -0.5f, 0.0f), // 1
		vec4( 0.5f,  0.5f,  0.5f, 0.0f), // 2
		vec4( 0.5f,  0.5f, -0.5f, 0.0f), // 3
		vec4(-0.5f, -0.5f,  0.5f, 0.0f), // 4
		vec4(-0.5f, -0.5f, -0.5f, 0.0f), // 5
		vec4(-0.5f,  0.5f,  0.5f, 0.0f), // 6
		vec4(-0.5f,  0.5f, -0.5f, 0.0f)  // 7
	);

	const int cubeIndices[24] = int[24]
	(
		0, 1, 2, 3, // right   (0 2 1 → 0 1 2 and 1 2 3 → 2 1 3)
		4, 6, 5, 7, // left    (6 4 7 → 4 6 5 and 7 5 6 → 6 5 7)
		4, 5, 0, 1, // up      (5 4 1 → 4 5 0 and 0 1 4 → 1 0 2)
		2, 3, 6, 7, // down    (6 7 2 → 2 6 3 and 2 3 6 → 3 2 7)
		4, 0, 6, 2, // front   (4 6 0 → 4 0 6 and 0 2 6 → 2 0 6)
		3, 1, 7, 5  // back    (1 3 5 → 1 5 3 and 5 7 3 → 3 5 7)
	);

//	const vec4 cubeVertices[8] = vec4[8]
//	(
//	vec4( 0.5f,  0.5f,  0.5f, 0.0f),
//	vec4( 0.5f,  0.5f, -0.5f, 0.0f),
//	vec4( 0.5f, -0.5f,  0.5f, 0.0f),
//	vec4( 0.5f, -0.5f, -0.5f, 0.0f),
//	vec4(-0.5f,  0.5f,  0.5f, 0.0f),
//	vec4(-0.5f,  0.5f, -0.5f, 0.0f),
//	vec4(-0.5f, -0.5f,  0.5f, 0.0f),
//	vec4(-0.5f, -0.5f, -0.5f, 0.0f)
//	);
//
//	const int cubeIndices[24] = int[24]
//	(
//	0, 2, 1, 3, // right
//	6, 4, 7, 5, // left
//	5, 4, 1, 0, // up
//	6, 7, 2, 3, // down
//	4, 6, 0, 2, // front
//	1, 3, 5, 7  // back
//	);

	vec3 center = VoxelToWorld(gl_in[0].gl_Position.xyz);
	vec3 extent = vec3(ubo.worldMinPointVoxelSize.w);

	if(gs_in[0].albedo.a == 0.0f || !VoxelInFrustum(center, extent)) { return; }


	vec4 projectedVertices[8];

	for(int i = 0; i < 8; ++i)
	{
		vec4 vertex = gl_in[0].gl_Position + cubeVertices[i];
		projectedVertices[i] = ubo.modelViewProjection * vertex;
	}

	for(int face = 0; face < 6; ++face)
	{
		for(int vertex = 0; vertex < 4; ++vertex)
		{
			gl_Position = projectedVertices[cubeIndices[face * 4 + vertex]];

			// multply per color channel, 0 = delete channel, 1 = see channel
			// on alpha zero if the colors channels are positive, alpha will be passed as one
			// with alpha enabled and color channels > 1,  the albedo alpha will be passed.
			voxelColor = gs_in[0].albedo;
//			voxelColor = vec4(0.0f, 1.0f, 0.0f, 1.0f);
			EmitVertex();
		}

		EndPrimitive();
	}
}