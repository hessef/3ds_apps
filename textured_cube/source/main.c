//devkitPro 3ds graphics example with comments added to help with future referencing

#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>
#include <string.h>
#include "vshader_shbin.h"	// compiled binary of vshader_v.pica, generated at build time by picasso
#include "kitten_t3x.h"		// pre-converted Morton/tiled texture, generated at build time by tex3ds

#define CLEAR_COLOR 0x68B0D8FF	// RGBA sky-blue background color

// Flags that control how the GPU transfers the rendered framebuffer to the screen.
// These tell the hardware: no vertical flip, no tiling on output, no raw copy mode,
// input is RGBA8, output is RGB8 (drops alpha since the screen doesn't need it),
// and no scaling.
#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// One vertex: all data the shader needs for a single corner of a triangle.
// position  = XYZ in 3D space
// texcoord  = UV coordinates (where on the texture this corner maps to)
// normal    = the direction the surface faces at this corner (used for lighting)
// These map to v0, v1, v2 in the shader via AttrInfo_AddLoader below.
typedef struct { float position[3]; float texcoord[2]; float normal[3]; } vertex;

// The full geometry of the cube, defined as a flat list of triangle corners.
// A cube has 6 faces × 2 triangles × 3 corners = 36 entries.
// Each face uses its own copies of the shared geometric corners because the
// normal is different per face — the same geometric point on the PZ face has
// normal {0,0,+1} but on the PX face it's {+1,0,0}. Sharing corners would
// give wrong normals and break the flat-shading look.
// In a real project you'd load this from a file using a library like tinyobjloader
// rather than hard-coding it.
static const vertex vertex_list[] =
{
	// First face (PZ) — the front face, normal points toward +Z
	// First triangle
	{ {-0.5f, -0.5f, +0.5f}, {0.0f, 0.0f}, {0.0f, 0.0f, +1.0f} },
	{ {+0.5f, -0.5f, +0.5f}, {1.0f, 0.0f}, {0.0f, 0.0f, +1.0f} },
	{ {+0.5f, +0.5f, +0.5f}, {1.0f, 1.0f}, {0.0f, 0.0f, +1.0f} },
	// Second triangle
	{ {+0.5f, +0.5f, +0.5f}, {1.0f, 1.0f}, {0.0f, 0.0f, +1.0f} },
	{ {-0.5f, +0.5f, +0.5f}, {0.0f, 1.0f}, {0.0f, 0.0f, +1.0f} },
	{ {-0.5f, -0.5f, +0.5f}, {0.0f, 0.0f}, {0.0f, 0.0f, +1.0f} },

	// Second face (MZ) — the back face, normal points toward -Z
	// First triangle
	{ {-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f}, {0.0f, 0.0f, -1.0f} },
	{ {-0.5f, +0.5f, -0.5f}, {1.0f, 0.0f}, {0.0f, 0.0f, -1.0f} },
	{ {+0.5f, +0.5f, -0.5f}, {1.0f, 1.0f}, {0.0f, 0.0f, -1.0f} },
	// Second triangle
	{ {+0.5f, +0.5f, -0.5f}, {1.0f, 1.0f}, {0.0f, 0.0f, -1.0f} },
	{ {+0.5f, -0.5f, -0.5f}, {0.0f, 1.0f}, {0.0f, 0.0f, -1.0f} },
	{ {-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f}, {0.0f, 0.0f, -1.0f} },

	// Third face (PX) — right face, normal points toward +X
	// First triangle
	{ {+0.5f, -0.5f, -0.5f}, {0.0f, 0.0f}, {+1.0f, 0.0f, 0.0f} },
	{ {+0.5f, +0.5f, -0.5f}, {1.0f, 0.0f}, {+1.0f, 0.0f, 0.0f} },
	{ {+0.5f, +0.5f, +0.5f}, {1.0f, 1.0f}, {+1.0f, 0.0f, 0.0f} },
	// Second triangle
	{ {+0.5f, +0.5f, +0.5f}, {1.0f, 1.0f}, {+1.0f, 0.0f, 0.0f} },
	{ {+0.5f, -0.5f, +0.5f}, {0.0f, 1.0f}, {+1.0f, 0.0f, 0.0f} },
	{ {+0.5f, -0.5f, -0.5f}, {0.0f, 0.0f}, {+1.0f, 0.0f, 0.0f} },

	// Fourth face (MX) — left face, normal points toward -X
	// First triangle
	{ {-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f} },
	{ {-0.5f, -0.5f, +0.5f}, {1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f} },
	{ {-0.5f, +0.5f, +0.5f}, {1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f} },
	// Second triangle
	{ {-0.5f, +0.5f, +0.5f}, {1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f} },
	{ {-0.5f, +0.5f, -0.5f}, {0.0f, 1.0f}, {-1.0f, 0.0f, 0.0f} },
	{ {-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f} },

	// Fifth face (PY) — top face, normal points toward +Y
	// First triangle
	{ {-0.5f, +0.5f, -0.5f}, {0.0f, 0.0f}, {0.0f, +1.0f, 0.0f} },
	{ {-0.5f, +0.5f, +0.5f}, {1.0f, 0.0f}, {0.0f, +1.0f, 0.0f} },
	{ {+0.5f, +0.5f, +0.5f}, {1.0f, 1.0f}, {0.0f, +1.0f, 0.0f} },
	// Second triangle
	{ {+0.5f, +0.5f, +0.5f}, {1.0f, 1.0f}, {0.0f, +1.0f, 0.0f} },
	{ {+0.5f, +0.5f, -0.5f}, {0.0f, 1.0f}, {0.0f, +1.0f, 0.0f} },
	{ {-0.5f, +0.5f, -0.5f}, {0.0f, 0.0f}, {0.0f, +1.0f, 0.0f} },

	// Sixth face (MY) — bottom face, normal points toward -Y
	// First triangle
	{ {-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f}, {0.0f, -1.0f, 0.0f} },
	{ {+0.5f, -0.5f, -0.5f}, {1.0f, 0.0f}, {0.0f, -1.0f, 0.0f} },
	{ {+0.5f, -0.5f, +0.5f}, {1.0f, 1.0f}, {0.0f, -1.0f, 0.0f} },
	// Second triangle
	{ {+0.5f, -0.5f, +0.5f}, {1.0f, 1.0f}, {0.0f, -1.0f, 0.0f} },
	{ {-0.5f, -0.5f, +0.5f}, {0.0f, 1.0f}, {0.0f, -1.0f, 0.0f} },
	{ {-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f}, {0.0f, -1.0f, 0.0f} },
};

#define vertex_list_count (sizeof(vertex_list)/sizeof(vertex_list[0]))	// = 36

// Handles to GPU-side objects, kept as globals so sceneRender() and sceneExit() can access them.
static DVLB_s* vshader_dvlb;       // parsed shader binary
static shaderProgram_s program;    // shader program object binding the vertex shader
static int uLoc_projection;        // uniform index for the projection matrix
static int uLoc_modelView;         // uniform index for the modelView matrix
static int uLoc_lightVec;          // uniform index for the light direction vector
static int uLoc_lightHalfVec;      // uniform index for the Blinn-Phong half vector
static int uLoc_lightClr;          // uniform index for the light color
static int uLoc_material;          // uniform index for the 4-row material matrix
static C3D_Mtx projection;         // the projection matrix (computed once in sceneInit)

// Material matrix: defines how this surface responds to each lighting component.
// Each row is a 4-component vector; the shader aliases these as mat_amb/dif/spe/emi.
// Layout is {w, r, g, b} — w is unused by the lighting math, rgb are the color weights.
// Ambient  {0.0, 0.2, 0.2, 0.2} — dim blue-gray base light, always present
// Diffuse  {0.0, 0.4, 0.4, 0.4} — mid gray, the main directional shading
// Specular {0.0, 0.8, 0.8, 0.8} — bright highlight on the shiny side
// Emission {1.0, 0.0, 0.0, 0.0} — no self-glow (rgb all zero)
static C3D_Mtx material =
{
	{
	{ { 0.0f, 0.2f, 0.2f, 0.2f } }, // Ambient
	{ { 0.0f, 0.4f, 0.4f, 0.4f } }, // Diffuse
	{ { 0.0f, 0.8f, 0.8f, 0.8f } }, // Specular
	{ { 1.0f, 0.0f, 0.0f, 0.0f } }, // Emission
	}
};

static void* vbo_data;						// pointer to vertex data in linear (GPU-accessible) memory
static C3D_Tex kitten_tex;					// GPU texture object
static float angleX = 0.0, angleY = 0.0;	// current rotation angles, incremented each frame

// Loads a texture from an in-memory .t3x blob (already in Morton/tiled format for PICA200).
// The t3x object itself is freed immediately — we only need the C3D_Tex handle going forward.
static bool loadTextureFromMem(C3D_Tex* tex, C3D_TexCube* cube, const void* data, size_t size)
{
	Tex3DS_Texture t3x = Tex3DS_TextureImport(data, size, tex, cube, false);
	if (!t3x)
		return false;

	// Delete the t3x object since we don't need it
	Tex3DS_TextureFree(t3x);
	return true;
}

static void sceneInit(void)
{
	// --- Shader setup ---
    // Parse the compiled shader binary (vshader_shbin) into a DVLB — the PICA200's
    // shader binary container format. Then wrap it in a shader program and bind it,
    // making it the active vertex shader for all subsequent draw calls.
	vshader_dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &vshader_dvlb->DVLE[0]);
	C3D_BindProgram(&program);

	// Look up the index of each uniform by name so we can set them in sceneRender().
    // Uniforms are values constant across all vertices in a draw call — matrices, light data, etc.
	uLoc_projection   = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
	uLoc_modelView    = shaderInstanceGetUniformLocation(program.vertexShader, "modelView");
	uLoc_lightVec     = shaderInstanceGetUniformLocation(program.vertexShader, "lightVec");
	uLoc_lightHalfVec = shaderInstanceGetUniformLocation(program.vertexShader, "lightHalfVec");
	uLoc_lightClr     = shaderInstanceGetUniformLocation(program.vertexShader, "lightClr");
	uLoc_material     = shaderInstanceGetUniformLocation(program.vertexShader, "material");

	// --- Vertex attribute layout ---
    // Tell the GPU how to interpret the raw bytes in the vertex buffer.
    // Each call maps one attribute slot to a run of floats within the vertex struct:
    //   slot 0 = 3 floats = position  → v0 in shader (inpos)
    //   slot 1 = 2 floats = texcoord  → v1 in shader (intex)
    //   slot 2 = 3 floats = normal    → v2 in shader (innrm)
    // The GPU advances by sizeof(vertex) bytes between vertices (set in BufInfo_Add below).
	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3); // v0=position
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2); // v1=texcoord
	AttrInfo_AddLoader(attrInfo, 2, GPU_FLOAT, 3); // v2=normal

	 // --- Projection matrix ---
    // PerspTilt builds a perspective matrix that accounts for the 3DS top screen being
    // stored sideways in memory — using Mtx_Persp instead would render everything rotated 90°.
    // 80° field of view, top screen aspect ratio, near plane 0.01, far plane 1000.
	Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(80.0f), C3D_AspectRatioTop, 0.01f, 1000.0f, false);

	// --- Vertex buffer ---
    // linearAlloc allocates from the linear heap, which the PICA200 can access via DMA.
    // Regular malloc/new memory is NOT visible to the GPU — this is mandatory for VBO data.
    // BufInfo_Add registers the buffer: stride=sizeof(vertex), 3 attributes, 0x210 encodes
    // which attribute slots are active (slots 0, 1, 2 in this case).
	vbo_data = linearAlloc(sizeof(vertex_list));
	memcpy(vbo_data, vertex_list, sizeof(vertex_list));
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo_data, sizeof(vertex), 3, 0x210);

	// --- Texture ---
    // The .t3x file was pre-converted to Morton/tiled format at build time by tex3ds.
    // GPU_LINEAR filtering when magnifying (smoother), GPU_NEAREST when minifying (sharper/faster).
    // TexBind(0, ...) binds to texture unit 0, referenced as GPU_TEXTURE0 in the TexEnv below.
    // svcBreak(USERBREAK_PANIC) crashes with a visible error if the texture fails to load —
    // better than silently rendering garbage.
	if (!loadTextureFromMem(&kitten_tex, NULL, kitten_t3x, kitten_t3x_size))
		svcBreak(USERBREAK_PANIC);
	C3D_TexSetFilter(&kitten_tex, GPU_LINEAR, GPU_NEAREST);
	C3D_TexBind(0, &kitten_tex);

	// --- Fragment stage (TexEnv) ---
    // The TexEnv runs after the vertex shader, once per pixel.
    // GPU_MODULATE multiplies the texture color (from kitten_tex) by the vertex color
    // (the lighting result output as outclr by the shader).
    // Result: pixels on the lit side are bright and textured; dark side is dim and textured.
    // C3D_Both means this applies to both RGB and Alpha channels.
	// See https://www.opengl.org/sdk/docs/man2/xhtml/glTexEnv.xml for more insight
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
}

static void sceneRender(void)
{
	// Build the modelView matrix fresh each frame.
    // Transforms are applied in reverse order of the Mtx_ calls because each one
    // left-multiplies the matrix — so the actual order of operations on vertices is:
    //   1. RotateY  (spin around vertical axis)
    //   2. RotateX  (tilt)
    //   3. Translate (move back from camera so the cube is visible)
	C3D_Mtx modelView;
	Mtx_Identity(&modelView);
	Mtx_Translate(&modelView, 0.0, 0.0, -2.0 + 0.5*sinf(angleX), true);
	Mtx_RotateX(&modelView, angleX, true);
	Mtx_RotateY(&modelView, angleY, true);

	// Rotate the cube each frame
	angleX += M_PI / 180;
	angleY += M_PI / 360;

	// Upload uniforms to the GPU — values that stay constant for all 36 vertices this frame.
    // These correspond to the .fvec declarations at the top of the shader.
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_modelView,  &modelView);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_material,   &material);
	 // Light direction: pointing straight toward the camera (-Z). w=0 because it's a direction.
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc_lightVec,     0.0f, 0.0f, -1.0f, 0.0f);
	// Half vector for Blinn-Phong specular: halfway between light dir and view dir.
    // Since both are -Z here, the half vector is also -Z.
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc_lightHalfVec, 0.0f, 0.0f, -1.0f, 0.0f);
	// Light color: pure white (r=1, g=1, b=1). Change this to tint the lighting.
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc_lightClr,     1.0f, 1.0f,  1.0f, 1.0f);	

	// Issue the draw call. The GPU reads vertex_list_count (36) vertices from the VBO,
    // runs the vertex shader once per vertex, rasterizes the resulting triangles,
    // and runs the TexEnv fragment stage once per covered pixel.
	C3D_DrawArrays(GPU_TRIANGLES, 0, vertex_list_count);
}

static void sceneExit(void)
{
	// Free resources in reverse order of creation.
	C3D_TexDelete(&kitten_tex);		// release GPU texture memory
	linearFree(vbo_data);			// release linear heap VBO memory
	shaderProgramFree(&program);	// release shader program
	DVLB_Free(vshader_dvlb);		// release parsed shader binary
}

int main()
{
	// Initialize the 3DS graphics subsystem and citro3d.
    // C3D_DEFAULT_CMDBUF_SIZE is the size of the GPU command buffer —
    // the ring buffer the CPU writes draw commands into for the GPU to consume.
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

	// Create a render target for the top-left screen (240×400 in memory —
    // the screen is physically sideways, hence width and height are swapped).
    // GPU_RB_RGBA8 = 32-bit color buffer, GPU_RB_DEPTH24_STENCIL8 = depth+stencil.
	C3D_RenderTarget* target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	// Initialize the scene
	sceneInit();

	// Main loop — aptMainLoop() returns false when the home button is pressed
    // or the system needs to suspend the app.
	while (aptMainLoop())
	{
		hidScanInput();

		// Respond to user input
		u32 kDown = hidKeysDown();
		if (kDown & KEY_START)
			break; // break in order to return to hbmenu

		// Frame rendering: C3D_FRAME_SYNCDRAW blocks until the previous frame's
        // GPU commands have been consumed, preventing tearing and buffer overruns.
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(target);	// direct subsequent draw calls to this render target
			sceneRender();
		C3D_FrameEnd(0);
	}

	// Deinitialize the scene
	sceneExit();

	// Deinitialize graphics
	C3D_Fini();
	gfxExit();
	return 0;
}
