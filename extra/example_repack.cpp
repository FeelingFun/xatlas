/*
xatlas
https://github.com/jpcy/xatlas
Copyright (c) 2018 Jonathan Young

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
/*
thekla_atlas
https://github.com/Thekla/thekla_atlas
MIT License
Copyright (c) 2013 Thekla, Inc
Copyright NVIDIA Corporation 2006 -- Ignacio Castano <icastano@nvidia.com>
*/
/*
Uses AddUvMesh and PackCharts to repack existing UVs into a single atlas. Texture data is copied into a new atlas texture.
*/
#include <algorithm>
#include <cmath>
#include <vector>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <stb_image.h>
#include <stb_image_write.h>
#include <objzero/objzero.h>
#include "../xatlas.h"

#ifdef _MSC_VER
#define FOPEN(_file, _filename, _mode) { if (fopen_s(&_file, _filename, _mode) != 0) _file = NULL; }
#define STRCAT(_dest, _size, _src) strcat_s(_dest, _size, _src);
#define STRCPY(_dest, _size, _src) strcpy_s(_dest, _size, _src);
#define STRICMP _stricmp
#else
#define FOPEN(_file, _filename, _mode) _file = fopen(_filename, _mode)
#include <strings.h>
#define STRCAT(_dest, _size, _src) strcat(_dest, _src);
#define STRCPY(_dest, _size, _src) strcpy(_dest, _src);
#define STRICMP strcasecmp
#endif

struct TextureData
{
	uint16_t width;
	uint16_t height;
	int numComponents;
	const uint8_t *data;
};

static TextureData textureLoad(const char *basePath, const char *filename)
{
	char fullFilename[256] = { 0 };
	STRCPY(fullFilename, sizeof(fullFilename), basePath);
	STRCAT(fullFilename, sizeof(fullFilename), filename);
	TextureData td;
	td.data = nullptr;
	FILE *f;
	FOPEN(f, fullFilename, "rb");
	if (!f) {
		fprintf(stderr, "Error opening '%s'\n", fullFilename);
		return td;
	}
	fseek(f, 0, SEEK_END);
	const long length = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> fileData;
	fileData.resize(length);
	if (fread(fileData.data(), 1, (size_t)length, f) < (size_t)length) {
		fclose(f);
		fprintf(stderr, "Error reading '%s'\n", fullFilename);
		return td;
	}
	fclose(f);
	int width, height, numComponents;
	td.data = stbi_load_from_memory(fileData.data(), (int)fileData.size(), &width, &height, &numComponents, 0);
	if (!td.data) {
		fprintf(stderr, "Error loading '%s': %s\n", fullFilename, stbi_failure_reason());
		return td;
	}
	printf("Texture '%s': %dx%d %d bpp\n", fullFilename, width, height, numComponents * 8);
	td.width = (uint16_t)width;
	td.height = (uint16_t)height;
	td.numComponents = numComponents;
	return td;
}

struct CachedTexture
{
	char filename[256];
	TextureData data;
};

static std::vector<CachedTexture> s_textureCache;

static uint32_t textureLoadCached(const char *basePath, const char *filename)
{
	for (uint32_t i = 0; i < (uint32_t)s_textureCache.size(); i++) {
		if (STRICMP(s_textureCache[i].filename, filename) == 0)
			return i;
	}
	CachedTexture texture;
	STRCPY(texture.filename, sizeof(texture.filename), filename);
	texture.data = textureLoad(basePath, filename);
	s_textureCache.push_back(texture);
	return (uint32_t)s_textureCache.size() - 1;
}

struct Vector2
{
	Vector2() {}
	Vector2(float x, float y) : x(x), y(y) {}
	float x, y;
};

static Vector2 operator+(const Vector2 &a, const Vector2 &b)
{
	return Vector2(a.x + b.x, a.y + b.y);
}

static Vector2 operator-(const Vector2 &a, const Vector2 &b)
{
	return Vector2(a.x - b.x, a.y - b.y);
}

static Vector2 operator*(const Vector2 &v, float s)
{
	return Vector2(v.x * s, v.y * s);
}

struct Vector3
{
	Vector3() {}
	Vector3(float x, float y, float z) : x(x), y(y), z(z) {}

	void operator+=(const Vector3 &v)
	{
		x += v.x;
		y += v.y;
		z += v.z;
	}

	float x, y, z;
};

static Vector3 operator+(const Vector3 &a, const Vector3 &b)
{
	return Vector3(a.x + b.x, a.y + b.y, a.z + b.z);
}

static Vector3 operator-(const Vector3 &a, const Vector3 &b)
{
	return Vector3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static Vector3 operator*(const Vector3 &v, float s)
{
	return Vector3(v.x * s, v.y * s, v.z * s);
}

class ClippedTriangle
{
public:
	ClippedTriangle(const Vector2 &a, const Vector2 &b, const Vector2 &c)
	{
		m_numVertices = 3;
		m_activeVertexBuffer = 0;
		m_verticesA[0] = a;
		m_verticesA[1] = b;
		m_verticesA[2] = c;
		m_vertexBuffers[0] = m_verticesA;
		m_vertexBuffers[1] = m_verticesB;
	}

	void clipHorizontalPlane(float offset, float clipdirection)
	{
		Vector2 *v  = m_vertexBuffers[m_activeVertexBuffer];
		m_activeVertexBuffer ^= 1;
		Vector2 *v2 = m_vertexBuffers[m_activeVertexBuffer];
		v[m_numVertices] = v[0];
		float dy2,   dy1 = offset - v[0].y;
		int   dy2in, dy1in = clipdirection * dy1 >= 0;
		uint32_t  p = 0;
		for (uint32_t k = 0; k < m_numVertices; k++) {
			dy2   = offset - v[k + 1].y;
			dy2in = clipdirection * dy2 >= 0;
			if (dy1in) v2[p++] = v[k];
			if ( dy1in + dy2in == 1 ) { // not both in/out
				float dx = v[k + 1].x - v[k].x;
				float dy = v[k + 1].y - v[k].y;
				v2[p++] = Vector2(v[k].x + dy1 * (dx / dy), offset);
			}
			dy1 = dy2;
			dy1in = dy2in;
		}
		m_numVertices = p;
	}

	void clipVerticalPlane(float offset, float clipdirection )
	{
		Vector2 *v  = m_vertexBuffers[m_activeVertexBuffer];
		m_activeVertexBuffer ^= 1;
		Vector2 *v2 = m_vertexBuffers[m_activeVertexBuffer];
		v[m_numVertices] = v[0];
		float dx2,   dx1   = offset - v[0].x;
		int   dx2in, dx1in = clipdirection * dx1 >= 0;
		uint32_t  p = 0;
		for (uint32_t k = 0; k < m_numVertices; k++) {
			dx2 = offset - v[k + 1].x;
			dx2in = clipdirection * dx2 >= 0;
			if (dx1in) v2[p++] = v[k];
			if ( dx1in + dx2in == 1 ) { // not both in/out
				float dx = v[k + 1].x - v[k].x;
				float dy = v[k + 1].y - v[k].y;
				v2[p++] = Vector2(offset, v[k].y + dx1 * (dy / dx));
			}
			dx1 = dx2;
			dx1in = dx2in;
		}
		m_numVertices = p;
	}

	void computeAreaCentroid()
	{
		Vector2 *v  = m_vertexBuffers[m_activeVertexBuffer];
		v[m_numVertices] = v[0];
		m_area = 0;
		float centroidx = 0, centroidy = 0;
		for (uint32_t k = 0; k < m_numVertices; k++) {
			// http://local.wasp.uwa.edu.au/~pbourke/geometry/polyarea/
			float f = v[k].x * v[k + 1].y - v[k + 1].x * v[k].y;
			m_area += f;
			centroidx += f * (v[k].x + v[k + 1].x);
			centroidy += f * (v[k].y + v[k + 1].y);
		}
		m_area = 0.5f * fabsf(m_area);
		if (m_area == 0) {
			m_centroid = Vector2(0.0f, 0.0f);
		} else {
			m_centroid = Vector2(centroidx / (6 * m_area), centroidy / (6 * m_area));
		}
	}

	void clipAABox(float x0, float y0, float x1, float y1)
	{
		clipVerticalPlane(x0, -1);
		clipHorizontalPlane(y0, -1);
		clipVerticalPlane(x1, 1);
		clipHorizontalPlane(y1, 1);
		computeAreaCentroid();
	}

	Vector2 centroid()
	{
		return m_centroid;
	}

	float area()
	{
		return m_area;
	}

private:
	Vector2 m_verticesA[7 + 1];
	Vector2 m_verticesB[7 + 1];
	Vector2 *m_vertexBuffers[2];
	uint32_t m_numVertices;
	uint32_t m_activeVertexBuffer;
	float m_area;
	Vector2 m_centroid;
};

/// A callback to sample the environment. Return false to terminate rasterization.
typedef bool (*SamplingCallback)(void *param, int x, int y, const Vector3 &bar, const Vector3 &dx, const Vector3 &dy, float coverage);

struct Triangle
{
	Triangle(const Vector2 &v0, const Vector2 &v1, const Vector2 &v2, const Vector3 &t0, const Vector3 &t1, const Vector3 &t2)
	{
		// Init vertices.
		this->v1 = v0;
		this->v2 = v2;
		this->v3 = v1;
		// Set barycentric coordinates.
		this->t1 = t0;
		this->t2 = t2;
		this->t3 = t1;
		// Compute deltas.
		computeDeltas();
		computeUnitInwardNormals();
	}

	/// Compute texture space deltas.
	/// This method takes two edge vectors that form a basis, determines the
	/// coordinates of the canonic vectors in that basis, and computes the
	/// texture gradient that corresponds to those vectors.
	bool computeDeltas()
	{
		Vector2 e0 = v3 - v1;
		Vector2 e1 = v2 - v1;
		Vector3 de0 = t3 - t1;
		Vector3 de1 = t2 - t1;
		float denom = 1.0f / (e0.y * e1.x - e1.y * e0.x);
		if (!std::isfinite(denom)) {
			return false;
		}
		float lambda1 = - e1.y * denom;
		float lambda2 = e0.y * denom;
		float lambda3 = e1.x * denom;
		float lambda4 = - e0.x * denom;
		dx = de0 * lambda1 + de1 * lambda2;
		dy = de0 * lambda3 + de1 * lambda4;
		return true;
	}

	// compute unit inward normals for each edge.
	void computeUnitInwardNormals()
	{
		n1 = v1 - v2;
		n1 = Vector2(-n1.y, n1.x);
		n1 = n1 * (1.0f / sqrtf(n1.x * n1.x + n1.y * n1.y));
		n2 = v2 - v3;
		n2 = Vector2(-n2.y, n2.x);
		n2 = n2 * (1.0f / sqrtf(n2.x * n2.x + n2.y * n2.y));
		n3 = v3 - v1;
		n3 = Vector2(-n3.y, n3.x);
		n3 = n3 * (1.0f / sqrtf(n3.x * n3.x + n3.y * n3.y));
	}

	bool drawAA(SamplingCallback cb, void *param)
	{
		const float PX_INSIDE = 1.0f/sqrtf(2.0f);
		const float PX_OUTSIDE = -1.0f/sqrtf(2.0f);
		const float BK_SIZE = 8;
		const float BK_INSIDE = sqrtf(BK_SIZE*BK_SIZE/2.0f);
		const float BK_OUTSIDE = -sqrtf(BK_SIZE*BK_SIZE/2.0f);
		float minx, miny, maxx, maxy;
		// Bounding rectangle
		minx = floorf(std::max(std::min(v1.x, std::min(v2.x, v3.x)), 0.0f));
		miny = floorf(std::max(std::min(v1.y, std::min(v2.y, v3.y)), 0.0f));
		maxx = ceilf(std::max(v1.x, std::max(v2.x, v3.x)));
		maxy = ceilf(std::max(v1.y, std::max(v2.y, v3.y)));
		// There's no reason to align the blocks to the viewport, instead we align them to the origin of the triangle bounds.
		minx = floorf(minx);
		miny = floorf(miny);
		//minx = (float)(((int)minx) & (~((int)BK_SIZE - 1))); // align to blocksize (we don't need to worry about blocks partially out of viewport)
		//miny = (float)(((int)miny) & (~((int)BK_SIZE - 1)));
		minx += 0.5;
		miny += 0.5; // sampling at texel centers!
		maxx += 0.5;
		maxy += 0.5;
		// Half-edge constants
		float C1 = n1.x * (-v1.x) + n1.y * (-v1.y);
		float C2 = n2.x * (-v2.x) + n2.y * (-v2.y);
		float C3 = n3.x * (-v3.x) + n3.y * (-v3.y);
		// Loop through blocks
		for (float y0 = miny; y0 <= maxy; y0 += BK_SIZE) {
			for (float x0 = minx; x0 <= maxx; x0 += BK_SIZE) {
				// Corners of block
				float xc = (x0 + (BK_SIZE - 1) / 2.0f);
				float yc = (y0 + (BK_SIZE - 1) / 2.0f);
				// Evaluate half-space functions
				float aC = C1 + n1.x * xc + n1.y * yc;
				float bC = C2 + n2.x * xc + n2.y * yc;
				float cC = C3 + n3.x * xc + n3.y * yc;
				// Skip block when outside an edge
				if ( (aC <= BK_OUTSIDE) || (bC <= BK_OUTSIDE) || (cC <= BK_OUTSIDE) ) continue;
				// Accept whole block when totally covered
				if ( (aC >= BK_INSIDE) && (bC >= BK_INSIDE) && (cC >= BK_INSIDE) ) {
					Vector3 texRow = t1 + dy * (y0 - v1.y) + dx * (x0 - v1.x);
					for (float y = y0; y < y0 + BK_SIZE; y++) {
						Vector3 tex = texRow;
						for (float x = x0; x < x0 + BK_SIZE; x++) {
							if (!cb(param, (int)x, (int)y, tex, dx, dy, 1.0f)) {
								return false;
							}
							tex += dx;
						}
						texRow += dy;
					}
				} else { // Partially covered block
					float CY1 = C1 + n1.x * x0 + n1.y * y0;
					float CY2 = C2 + n2.x * x0 + n2.y * y0;
					float CY3 = C3 + n3.x * x0 + n3.y * y0;
					Vector3 texRow = t1 + dy * (y0 - v1.y) + dx * (x0 - v1.x);
					for (float y = y0; y < y0 + BK_SIZE; y++) { // @@ This is not clipping to scissor rectangle correctly.
						float CX1 = CY1;
						float CX2 = CY2;
						float CX3 = CY3;
						Vector3 tex = texRow;
						for (float x = x0; x < x0 + BK_SIZE; x++) { // @@ This is not clipping to scissor rectangle correctly.
							if (CX1 >= PX_INSIDE && CX2 >= PX_INSIDE && CX3 >= PX_INSIDE) {
								// pixel completely covered
								Vector3 tex2 = t1 + dx * (x - v1.x) + dy * (y - v1.y);
								if (!cb(param, (int)x, (int)y, tex2, dx, dy, 1.0f)) {
									return false;
								}
							} else if ((CX1 >= PX_OUTSIDE) && (CX2 >= PX_OUTSIDE) && (CX3 >= PX_OUTSIDE)) {
								// triangle partially covers pixel. do clipping.
								ClippedTriangle ct(v1 - Vector2(x, y), v2 - Vector2(x, y), v3 - Vector2(x, y));
								ct.clipAABox(-0.5, -0.5, 0.5, 0.5);
								Vector2 centroid = ct.centroid();
								float area = ct.area();
								if (area > 0.0f) {
									Vector3 texCent = tex - dx * centroid.x - dy * centroid.y;
									//XA_ASSERT(texCent.x >= -0.1f && texCent.x <= 1.1f); // @@ Centroid is not very exact...
									//XA_ASSERT(texCent.y >= -0.1f && texCent.y <= 1.1f);
									//XA_ASSERT(texCent.z >= -0.1f && texCent.z <= 1.1f);
									//Vector3 texCent2 = t1 + dx * (x - v1.x) + dy * (y - v1.y);
									if (!cb(param, (int)x, (int)y, texCent, dx, dy, area)) {
										return false;
									}
								}
							}
							CX1 += n1.x;
							CX2 += n2.x;
							CX3 += n3.x;
							tex += dx;
						}
						CY1 += n1.y;
						CY2 += n2.y;
						CY3 += n3.y;
						texRow += dy;
					}
				}
			}
		}
		return true;
	}

	Vector2 v1, v2, v3;
	Vector2 n1, n2, n3; // unit inward normals
	Vector3 t1, t2, t3;
	Vector3 dx, dy;
};

struct SetAtlasTexelArgs
{
	uint8_t *atlasData;
	uint32_t atlasWidth;
	Vector2 sourceUv[3];
	const TextureData *sourceTexture;
};

static bool setAtlasTexel(void *param, int x, int y, const Vector3 &bar, const Vector3 &, const Vector3 &, float)
{
	auto args = (SetAtlasTexelArgs *)param;
	uint8_t *dest = &args->atlasData[x * 3 + y * (args->atlasWidth * 3)];
	if (!args->sourceTexture) {
		dest[0] = 255;
		dest[1] = 0;
		dest[2] = 255;
	} else {
		// Interpolate source UVs using barycentrics.
		const Vector2 sourceUv = args->sourceUv[0] * bar.x + args->sourceUv[1] * bar.y + args->sourceUv[2] * bar.z;
		// Keep coordinates in range of texture dimensions.
		int sx = int(sourceUv.x * args->sourceTexture->width);
		while (sx < 0)
			sx += args->sourceTexture->width;
		if (sx >= args->sourceTexture->width)
			sx %= args->sourceTexture->width;
		int sy = int(sourceUv.y * args->sourceTexture->height);
		while (sy < 0)
			sy += args->sourceTexture->height;
		if (sy >= args->sourceTexture->height)
			sy %= args->sourceTexture->height;
		const uint8_t *source = &args->sourceTexture->data[sx * args->sourceTexture->numComponents + sy * (args->sourceTexture->width * args->sourceTexture->numComponents)];
		dest[0] = source[0];
		dest[1] = source[1];
		dest[2] = source[2];
	}
	return true;
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
	    printf("Usage: %s input_file.obj\n", argv[0]);
	    return EXIT_FAILURE;
	}
	// Load model file.
	printf("Loading '%s'...\n", argv[1]);
	objz_setIndexFormat(OBJZ_INDEX_FORMAT_U32);
	objz_setVertexFormat(sizeof(float) * 2, SIZE_MAX, 0, SIZE_MAX);
	objzModel *model = objz_load(argv[1]);
	if (!model) {
		fprintf(stderr, "%s\n", objz_getError());
		return EXIT_FAILURE;
	}
	if (objz_getError()) // Print warnings.
		printf("%s\n", objz_getError());
	// Load diffuse textures for each material.
	char basePath[256];
	STRCPY(basePath, sizeof(basePath), argv[1]);
	char *lastSlash = strrchr(basePath, '/');
	if (!lastSlash)
		lastSlash = strrchr(basePath, '\\');
	if (lastSlash) {
		lastSlash++;
		*lastSlash = 0;
	}
	printf("Base path is '%s'\n", basePath);
	std::vector<uint32_t> textures;
	textures.resize(model->numMaterials);
	for (uint32_t i = 0; i < model->numMaterials; i++) {
		const objzMaterial &mat = model->materials[i];
		textures[i] = mat.diffuseTexture[0] ? textureLoadCached(basePath, mat.diffuseTexture) : UINT32_MAX;
	}
	// Generate the atlas.
	xatlas::SetPrint(printf, true);
	xatlas::Atlas *atlas = xatlas::Create();
	auto modelUvs = (Vector2 *)model->vertices;
	std::vector<Vector2> uvs;
	uvs.resize(model->numVertices);
	for (uint32_t i = 0; i < model->numMeshes; i++) {
		const objzMesh &mesh = model->meshes[i];
		// Denormalize UVs by scaling them by texture dimensions.
		const TextureData *textureData = nullptr;
		if (mesh.materialIndex != -1 && textures[mesh.materialIndex] != UINT32_MAX)
			textureData = &s_textureCache[textures[mesh.materialIndex]].data;
		for (uint32_t j = 0; j < mesh.numIndices; j++) {
			const uint32_t index = ((const uint32_t *)model->indices)[mesh.firstIndex + j];
			uvs[index] = modelUvs[index];
			if (textureData) {
				uvs[index].x *= (float)textureData->width;
				uvs[index].y *= (float)textureData->height;
			}
		}
		xatlas::UvMeshDecl meshDecl;
		meshDecl.vertexCount = (uint32_t)uvs.size();
		meshDecl.vertexUvData = uvs.data();
		meshDecl.vertexStride = sizeof(float) * 2;
		meshDecl.indexCount = mesh.numIndices;
		meshDecl.indexData = &((const uint32_t *)model->indices)[mesh.firstIndex];
		meshDecl.indexFormat = xatlas::IndexFormat::UInt32;
		xatlas::AddMeshError::Enum error = xatlas::AddUvMesh(atlas, meshDecl);
		if (error != xatlas::AddMeshError::Success) {
			xatlas::Destroy(atlas);
			printf("Error adding mesh %d: %s\n", i, xatlas::StringForEnum(error));
			return EXIT_FAILURE;
		}
	}
	xatlas::PackOptions packOptions;
	packOptions.padding = 1;
	packOptions.texelsPerUnit = 1.0f;
	xatlas::PackCharts(atlas, packOptions);
	// Create a texture for the atlas.
	std::vector<uint8_t> atlasTexture;
	atlasTexture.resize(atlas->width * atlas->height * 3);
	// Rasterize chart triangles.
	for (uint32_t i = 0; i < atlas->meshCount; i++) {
		const xatlas::Mesh &atlasMesh = atlas->meshes[i];
		const objzMesh &sourceMesh = model->meshes[i];
		SetAtlasTexelArgs args;
		if (sourceMesh.materialIndex == -1 || textures[sourceMesh.materialIndex] == UINT32_MAX)
			args.sourceTexture = nullptr;
		else
			args.sourceTexture = &s_textureCache[textures[sourceMesh.materialIndex]].data;
		for (uint32_t j = 0; j < atlasMesh.chartCount; j++) {
			const xatlas::Chart &chart = atlasMesh.chartArray[j];
			for (uint32_t k = 0; k < chart.indexCount / 3; k++) {
				uint32_t indices[3];
				xatlas::Vertex vertices[3];
				Vector2 v[3];
				for (uint32_t l = 0; l < 3; l++) {
					indices[l] = chart.indexArray[k * 3 + l];
					vertices[l] = atlasMesh.vertexArray[indices[l]];
					v[l] = Vector2(vertices[l].uv[0], vertices[l].uv[1]);
					args.sourceUv[l] = modelUvs[vertices[l].xref];
					args.sourceUv[l].y = 1.0f - args.sourceUv[l].y;
				}
				Triangle tri(v[0], v[1], v[2], Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1));
				args.atlasData = atlasTexture.data();
				args.atlasWidth = atlas->width;
				tri.drawAA(setAtlasTexel, &args);
			}
		}
	}
	// Write the atlas texture.
	const char *outputFilename = "example_repack_output.tga";
	printf("Writing '%s'...\n", outputFilename);
	stbi_write_tga(outputFilename, atlas->width, atlas->height, 3, atlasTexture.data());
	// Cleanup.
	xatlas::Destroy(atlas);
	printf("Done\n");
	return EXIT_SUCCESS;
}
