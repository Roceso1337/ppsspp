// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <stdio.h>
#include <locale.h>

#include "gfx_es2/gpu_features.h"

#if defined(_WIN32) && defined(_DEBUG)
#include "Common/CommonWindows.h"
#endif

#include "base/stringutil.h"
#include "Core/Config.h"
#include "GPU/ge_constants.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/High/GLES/VertShaderGenGLES.h"
#include "GPU/High/GLES/ShaderManagerHighGLES.h"
#include "GPU/High/Command.h"

// SDL 1.2 on Apple does not have support for OpenGL 3 and hence needs
// special treatment in the shader generator.
#ifdef __APPLE__
#define FORCE_OPENGL_2_0
#endif

#undef WRITE

#define WRITE p+=sprintf

namespace HighGpu {

bool CanUseHardwareTransform(int prim, bool isModeThrough) {
	if (!g_Config.bHardwareTransform)
		return false;
	return isModeThrough && prim != GE_PRIM_RECTANGLES;
}

// These bits are internal to this file, although the resulting IDs will be externally visible.
// TODO: We will cut away many of these, turning them into uniforms. This should reduce the number
// of generated shaders drastically.
enum {
	BIT_LMODE = 0,
	BIT_IS_THROUGH = 1,
	BIT_ENABLE_FOG = 2,
	BIT_HAS_COLOR = 3,
	BIT_DO_TEXTURE = 4,
	BIT_FLIP_TEXTURE = 5,
	BIT_DO_TEXTURE_PROJ = 6,
	BIT_USE_HW_TRANSFORM = 8,
	BIT_HAS_NORMAL = 9,  // conditioned on hw transform
	BIT_UVGEN_MODE = 16,
	BIT_UVPROJ_MODE = 18,
	BIT_LS0 = 18,
	BIT_LS1 = 20,
	BIT_BONES = 22,
	BIT_ENABLE_BONES = 30,
	BIT_LIGHT0_COMP = 32,
	BIT_LIGHT0_TYPE = 34,
	BIT_LIGHT1_COMP = 36,
	BIT_LIGHT1_TYPE = 38,
	BIT_LIGHT2_COMP = 40,
	BIT_LIGHT2_TYPE = 42,
	BIT_LIGHT3_COMP = 44,
	BIT_LIGHT3_TYPE = 46,
	BIT_MATERIAL_UPDATE = 48,
	BIT_LIGHT0_ENABLE = 52,
	BIT_LIGHT1_ENABLE = 53,
	BIT_LIGHT2_ENABLE = 54,
	BIT_LIGHT3_ENABLE = 55,
	BIT_LIGHTING_ENABLE = 56,
	BIT_WEIGHT_FMTSCALE = 57,
	BIT_TEXCOORD_FMTSCALE = 60,
	BIT_HAS_TEXCOORD = 60,
	BIT_FLATSHADE = 62,
	BIT_NORM_REVERSE = 32 + 27,
};

void ComputeVertexShaderID(ShaderID *id_out, u32 vertType, u32 enabled,
		const HighGpu::RasterState *raster, const HighGpu::TexScaleState *ts,
		const HighGpu::LightGlobalState *lgs, const HighGpu::LightState **ls, bool flipTexture, bool useHWTransform) {
	using namespace HighGpu;

	bool isModeThrough = (vertType & GE_VTYPE_THROUGH_MASK) != 0;
	bool isModeClear = raster->clearMode;

	bool doTexture = (enabled & ENABLE_TEXTURE) && !isModeClear;
	bool doTextureProjection = ts->getUVGenMode() == GE_TEXMAP_TEXTURE_MATRIX;
	bool doShadeMapping = ts->getUVGenMode() == GE_TEXMAP_ENVIRONMENT_MAP;
	bool doFlatShading = raster->shadeMode == GE_SHADE_FLAT && !isModeClear;

	bool hasColor = (vertType & GE_VTYPE_COL_MASK) != 0;
	bool hasNormal = (vertType & GE_VTYPE_NRM_MASK) != 0;
	bool hasTexcoord = (vertType & GE_VTYPE_TC_MASK) != 0 || !useHWTransform;

	bool enableFog = (enabled & ENABLE_FOG) && !isModeThrough && !isModeClear;
	bool enableBones = (enabled & ENABLE_BONES);
	bool lmode = lgs->lmode && (enabled & ENABLE_LIGHTS);

	ShaderID id;

	id.SetBit(BIT_LMODE, lmode);
	id.SetBit(BIT_IS_THROUGH, isModeThrough);
	id.SetBit(BIT_ENABLE_FOG, enableFog);
	id.SetBit(BIT_HAS_COLOR, hasColor);
	if (doTexture) {
		id.SetBit(BIT_DO_TEXTURE);
		id.SetBit(BIT_FLIP_TEXTURE, flipTexture);
		id.SetBit(BIT_DO_TEXTURE_PROJ, doTextureProjection);
	}

	if (useHWTransform) {
		id.SetBit(BIT_ENABLE_BONES, enableBones);
		id.SetBit(BIT_USE_HW_TRANSFORM);
		id.SetBit(BIT_HAS_NORMAL, hasNormal);

		// UV generation mode. TODO: Make this conditional on the presence of a texture
		id.SetBits(BIT_UVGEN_MODE, 2, ts->getUVGenMode());

		// The next bits are used differently depending on UVgen mode
		if (doTextureProjection) {
			id.SetBits(BIT_UVPROJ_MODE, 2, ts->getUVProjMode());
		} else if (doShadeMapping) {
			id.SetBits(BIT_LS0, 2, ts->getUVLS0());
			id.SetBits(BIT_LS1, 2, ts->getUVLS1());
		}

		// Bones - TODO: In the new plan, these will be handled entirely differently.
		if (vertTypeIsSkinningEnabled(vertType)) {
			id.SetBits(BIT_BONES, 3, TranslateNumBones(vertTypeGetNumBoneWeights(vertType)) - 1);
		}

		// Okay, d[1] coming up. ==============
		if ((enabled && ENABLE_LIGHTS) || doShadeMapping) {
			// Light bits
			for (int i = 0; i < 4; i++) {
				if (enabled & (ENABLE_LIGHT0 << i)) {
					id.SetBits(BIT_LIGHT0_COMP + 4 * i, 2, ls[i]->getLightComputation());
					id.SetBits(BIT_LIGHT0_TYPE + 4 * i, 2, ls[i]->getLightType());
				}
			}
			id.SetBits(BIT_MATERIAL_UPDATE, 3, lgs->materialUpdate & 7);
			// TODO: Optimize by shifting in all the light bits together.
			for (int i = 0; i < 4; i++) {
				id.SetBit(BIT_LIGHT0_ENABLE + i, (enabled & (ENABLE_LIGHT0 << i)) != 0);
			}
			// doShadeMapping is stored as UVGenMode, so this is enough for isLightingEnabled.
			id.SetBit(BIT_LIGHTING_ENABLE);
		}

		// 2 bits. We should probably send in the weight scalefactor as a uniform instead,
		// or simply preconvert all weights to floats.
		id.SetBits(BIT_WEIGHT_FMTSCALE, 2, vertTypeGetWeightMask(vertType));
		id.SetBit(BIT_NORM_REVERSE, lgs->areNormalsReversed());
		if (doTextureProjection && ts->getUVProjMode() == GE_PROJMAP_UV) {
			id.SetBit(BIT_TEXCOORD_FMTSCALE, (vertType & GE_VTYPE_TC_MASK) >> GE_VTYPE_TC_SHIFT);  // two bits
		} else {
			id.SetBit(BIT_HAS_TEXCOORD, hasTexcoord);
		}
	}

	id.SetBit(BIT_FLATSHADE, doFlatShading);

	*id_out = id;
}

static const char * const boneWeightAttrDecl[9] = {
	"#ERROR#",
	"attribute mediump float w1;\n",
	"attribute mediump vec2 w1;\n",
	"attribute mediump vec3 w1;\n",
	"attribute mediump vec4 w1;\n",
	"attribute mediump vec4 w1;\nattribute mediump float w2;\n",
	"attribute mediump vec4 w1;\nattribute mediump vec2 w2;\n",
	"attribute mediump vec4 w1;\nattribute mediump vec3 w2;\n",
	"attribute mediump vec4 w1, w2;\n",
};

static const char * const boneWeightInDecl[9] = {
	"#ERROR#",
	"in mediump float w1;\n",
	"in mediump vec2 w1;\n",
	"in mediump vec3 w1;\n",
	"in mediump vec4 w1;\n",
	"in mediump vec4 w1;\nin mediump float w2;\n",
	"in mediump vec4 w1;\nin mediump vec2 w2;\n",
	"in mediump vec4 w1;\nin mediump vec3 w2;\n",
	"in mediump vec4 w1, w2;\n",
};

enum DoLightComputation {
	LIGHT_OFF,
	LIGHT_SHADE,
	LIGHT_FULL,
};

void GenerateVertexShader(const ShaderID &id, char *buffer) {
	char *p = buffer;

// #define USE_FOR_LOOP

	// In GLSL ES 3.0, you use "out" variables instead.
	bool glslES30 = false;
	const char *varying = "varying";
	const char *attribute = "attribute";
	const char * const * boneWeightDecl = boneWeightAttrDecl;
	bool highpFog = false;
	bool highpTexcoord = false;

#if defined(USING_GLES2)
	// Let's wait until we have a real use for this.
	// ES doesn't support dual source alpha :(
	if (gl_extensions.GLES3) {
		WRITE(p, "#version 300 es\n");
		glslES30 = true;
	} else {
		WRITE(p, "#version 100\n");  // GLSL ES 1.0
	}
	WRITE(p, "precision highp float;\n");

	// PowerVR needs highp to do the fog in MHU correctly.
	// Others don't, and some can't handle highp in the fragment shader.
	highpFog = (gl_extensions.bugs & BUG_PVR_SHADER_PRECISION_BAD) ? true : false;
	highpTexcoord = highpFog;

#elif !defined(FORCE_OPENGL_2_0)
	if (gl_extensions.VersionGEThan(3, 3, 0)) {
		glslES30 = true;
		WRITE(p, "#version 330\n");
		WRITE(p, "#define lowp\n");
		WRITE(p, "#define mediump\n");
		WRITE(p, "#define highp\n");
	} else if (gl_extensions.VersionGEThan(3, 0, 0)) {
		WRITE(p, "#version 130\n");
		// Remove lowp/mediump in non-mobile non-glsl 3 implementations
		WRITE(p, "#define lowp\n");
		WRITE(p, "#define mediump\n");
		WRITE(p, "#define highp\n");
	} else {
		WRITE(p, "#version 110\n");
		// Remove lowp/mediump in non-mobile non-glsl 3 implementations
		WRITE(p, "#define lowp\n");
		WRITE(p, "#define mediump\n");
		WRITE(p, "#define highp\n");
	}
#else
	// Need to remove lowp/mediump for Mac
	WRITE(p, "#define lowp\n");
	WRITE(p, "#define mediump\n");
	WRITE(p, "#define highp\n");
#endif

	if (glslES30) {
		attribute = "in";
		varying = "out";
		boneWeightDecl = boneWeightInDecl;
	}

	bool lmode = id.Bit(BIT_LMODE);
	bool doTexture = id.Bit(BIT_DO_TEXTURE);
	bool doTextureProjection = id.Bit(BIT_DO_TEXTURE_PROJ);

	GETexMapMode uvGenMode = static_cast<GETexMapMode>(id.Bits(BIT_UVGEN_MODE, 2));

	// this is only valid for some settings of uvGenMode
	GETexProjMapMode uvProjMode = static_cast<GETexProjMapMode>(id.Bits(BIT_UVPROJ_MODE, 2));
	bool doShadeMapping = uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP;
	bool doFlatShading = id.Bit(BIT_FLATSHADE);

	bool isModeThrough = id.Bit(BIT_IS_THROUGH);
	bool useHWTransform = id.Bit(BIT_USE_HW_TRANSFORM);
	bool hasColor = (id.d[0] >> BIT_HAS_COLOR) & 1;
	bool hasNormal = id.Bit(BIT_HAS_NORMAL);
	bool hasTexcoord = id.Bit(BIT_HAS_TEXCOORD);
	bool enableFog = id.Bit(BIT_ENABLE_FOG);
	bool throughmode = id.Bit(BIT_IS_THROUGH);
	bool flipV = id.Bit(BIT_FLIP_TEXTURE);  // This also means that we are texturing from a render target
	bool flipNormal = id.Bit(BIT_NORM_REVERSE);
	bool enableBones = id.Bit(BIT_ENABLE_BONES);
	bool enableLighting = id.Bit(BIT_LIGHTING_ENABLE);
	int ls0 = id.Bits(BIT_LS0, 2);
	int ls1 = id.Bits(BIT_LS1, 2);
	int matUpdate = id.Bits(BIT_MATERIAL_UPDATE, 3);

	const char *shading = "";
	if (glslES30)
		shading = doFlatShading ? "flat" : "";

	DoLightComputation doLight[4] = {LIGHT_OFF, LIGHT_OFF, LIGHT_OFF, LIGHT_OFF};
	if (useHWTransform) {
		int shadeLight0 = doShadeMapping ? ls0 : -1;
		int shadeLight1 = doShadeMapping ? ls1 : -1;
		for (int i = 0; i < 4; i++) {
			if (i == shadeLight0 || i == shadeLight1)
				doLight[i] = LIGHT_SHADE;
			if (id.Bit(BIT_LIGHTING_ENABLE) && id.Bit(BIT_LIGHT0_ENABLE + i))
				doLight[i] = LIGHT_FULL;
		}
	}

	int numBoneWeights = 0;
	int boneWeightScale = id.Bits(BIT_WEIGHT_FMTSCALE, 2);
	if (id.Bit(BIT_ENABLE_BONES)) {
		numBoneWeights = 1 + id.Bits(BIT_BONES, 3);
		WRITE(p, "%s", boneWeightDecl[numBoneWeights]);
	}
	int texFmtScale = id.Bits(BIT_TEXCOORD_FMTSCALE, 2);

	if (useHWTransform)
		WRITE(p, "%s vec3 position;\n", attribute);
	else
		WRITE(p, "%s vec4 position;\n", attribute);  // need to pass the fog coord in w

	if (useHWTransform && hasNormal)
		WRITE(p, "%s mediump vec3 normal;\n", attribute);

	if (doTexture && hasTexcoord) {
		if (!useHWTransform && doTextureProjection && !throughmode)
			WRITE(p, "%s vec3 texcoord;\n", attribute);
		else
			WRITE(p, "%s vec2 texcoord;\n", attribute);
	}
	if (hasColor) {
		WRITE(p, "%s lowp vec4 color0;\n", attribute);
		if (lmode && !useHWTransform)  // only software transform supplies color1 as vertex data
			WRITE(p, "%s lowp vec3 color1;\n", attribute);
	}

	if (isModeThrough)	{
		WRITE(p, "uniform mat4 u_proj_through;\n");
	} else {
		WRITE(p, "uniform mat4 u_proj;\n");
		// Add all the uniforms we'll need to transform properly.
	}

	bool prescale = g_Config.bPrescaleUV && !throughmode && (uvGenMode == GE_TEXMAP_TEXTURE_COORDS || uvGenMode == GE_TEXMAP_UNKNOWN);

	if (useHWTransform) {
		// When transforming by hardware, we need a great deal more uniforms...
		WRITE(p, "uniform mat4 u_world;\n");
		WRITE(p, "uniform mat4 u_view;\n");
		if (doTextureProjection)
			WRITE(p, "uniform mediump mat4 u_texmtx;\n");
		if (enableBones) {
#ifdef USE_BONE_ARRAY
			WRITE(p, "uniform mediump mat4 u_bone[%i];\n", numBoneWeights);
#else
			for (int i = 0; i < numBoneWeights; i++) {
				WRITE(p, "uniform mat4 u_bone%i;\n", i);
			}
#endif
		}
		if (doTexture && (flipV || !prescale || uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP || uvGenMode == GE_TEXMAP_TEXTURE_MATRIX)) {
			WRITE(p, "uniform vec4 u_uvscaleoffset;\n");
		}
		for (int i = 0; i < 4; i++) {
			if (doLight[i] != LIGHT_OFF) {
				// This is needed for shade mapping
				WRITE(p, "uniform vec3 u_lightpos%i;\n", i);
			}
			if (doLight[i] == LIGHT_FULL) {
				GELightType type = static_cast<GELightType>(id.Bits(BIT_LIGHT0_TYPE + 4*i, 2));
				GELightComputation comp = static_cast<GELightComputation>(id.Bits(BIT_LIGHT0_COMP + 4*i, 2));

				if (type != GE_LIGHTTYPE_DIRECTIONAL)
					WRITE(p, "uniform mediump vec3 u_lightatt%i;\n", i);

				if (type == GE_LIGHTTYPE_SPOT || type == GE_LIGHTTYPE_UNKNOWN) {
					WRITE(p, "uniform mediump vec3 u_lightdir%i;\n", i);
					WRITE(p, "uniform mediump float u_lightangle%i;\n", i);
					WRITE(p, "uniform mediump float u_lightspotCoef%i;\n", i);
				}
				WRITE(p, "uniform lowp vec3 u_lightambient%i;\n", i);
				WRITE(p, "uniform lowp vec3 u_lightdiffuse%i;\n", i);

				if (comp != GE_LIGHTCOMP_ONLYDIFFUSE) {
					WRITE(p, "uniform lowp vec3 u_lightspecular%i;\n", i);
				}
			}
		}
		if (enableLighting) {
			WRITE(p, "uniform lowp vec4 u_ambient;\n");
			if ((matUpdate & 2) == 0 || !hasColor)
				WRITE(p, "uniform lowp vec3 u_matdiffuse;\n");
			// if ((gstate.materialupdate & 4) == 0)
			WRITE(p, "uniform lowp vec4 u_matspecular;\n");  // Specular coef is contained in alpha
			WRITE(p, "uniform lowp vec3 u_matemissive;\n");
		}
	}

	if (useHWTransform || !hasColor)
		WRITE(p, "uniform lowp vec4 u_matambientalpha;\n");  // matambient + matalpha

	if (enableFog) {
		WRITE(p, "uniform highp vec2 u_fogcoef;\n");
	}

	WRITE(p, "%s %s lowp vec4 v_color0;\n", shading, varying);
	if (lmode) {
		WRITE(p, "%s %s lowp vec3 v_color1;\n", shading, varying);
	}

	if (doTexture) {
		if (doTextureProjection) {
			WRITE(p, "%s %s vec3 v_texcoord;\n", varying, highpTexcoord ? "highp" : "mediump");
		} else {
			WRITE(p, "%s %s vec2 v_texcoord;\n", varying, highpTexcoord ? "highp" : "mediump");
		}
	}

	if (enableFog) {
		// See the fragment shader generator
		if (highpFog) {
			WRITE(p, "%s highp float v_fogdepth;\n", varying);
		} else {
			WRITE(p, "%s mediump float v_fogdepth;\n", varying);
		}
	}

	WRITE(p, "void main() {\n");

	if (!useHWTransform) {
		// Simple pass-through of vertex data to fragment shader
		if (doTexture) {
			if (throughmode && doTextureProjection) {
				WRITE(p, "  v_texcoord = vec3(texcoord, 1.0);\n");
			} else {
				WRITE(p, "  v_texcoord = texcoord;\n");
			}
		}
		if (hasColor) {
			WRITE(p, "  v_color0 = color0;\n");
			if (lmode)
				WRITE(p, "  v_color1 = color1;\n");
		} else {
			WRITE(p, "  v_color0 = u_matambientalpha;\n");
			if (lmode)
				WRITE(p, "  v_color1 = vec3(0.0);\n");
		}
		if (enableFog) {
			WRITE(p, "  v_fogdepth = position.w;\n");
		}
		if (isModeThrough)	{
			WRITE(p, "  gl_Position = u_proj_through * vec4(position.xyz, 1.0);\n");
		} else {
			WRITE(p, "  gl_Position = u_proj * vec4(position.xyz, 1.0);\n");
		}
	} else {
		// Step 1: World Transform / Skinning
		if (enableBones) {
			// No skinning, just standard T&L.
			WRITE(p, "  vec3 worldpos = (u_world * vec4(position.xyz, 1.0)).xyz;\n");
			if (hasNormal)
				WRITE(p, "  mediump vec3 worldnormal = normalize((u_world * vec4(%snormal, 0.0)).xyz);\n", flipNormal ? "-" : "");
			else
				WRITE(p, "  mediump vec3 worldnormal = vec3(0.0, 0.0, 1.0);\n");
		} else {
			static const char *rescale[4] = {"", " * 1.9921875", " * 1.999969482421875", ""}; // 2*127.5f/128.f, 2*32767.5f/32768.f, 1.0f};
			const char *factor = rescale[texFmtScale];

			static const char * const boneWeightAttr[8] = {
				"w1.x", "w1.y", "w1.z", "w1.w",
				"w2.x", "w2.y", "w2.z", "w2.w",
			};

#if defined(USE_FOR_LOOP) && defined(USE_BONE_ARRAY)

			// To loop through the weights, we unfortunately need to put them in a float array.
			// GLSL ES sucks - no way to directly initialize an array!
			switch (numBoneWeights) {
			case 1: WRITE(p, "  float w[1]; w[0] = w1;\n"); break;
			case 2: WRITE(p, "  float w[2]; w[0] = w1.x; w[1] = w1.y;\n"); break;
			case 3: WRITE(p, "  float w[3]; w[0] = w1.x; w[1] = w1.y; w[2] = w1.z;\n"); break;
			case 4: WRITE(p, "  float w[4]; w[0] = w1.x; w[1] = w1.y; w[2] = w1.z; w[3] = w1.w;\n"); break;
			case 5: WRITE(p, "  float w[5]; w[0] = w1.x; w[1] = w1.y; w[2] = w1.z; w[3] = w1.w; w[4] = w2;\n"); break;
			case 6: WRITE(p, "  float w[6]; w[0] = w1.x; w[1] = w1.y; w[2] = w1.z; w[3] = w1.w; w[4] = w2.x; w[5] = w2.y;\n"); break;
			case 7: WRITE(p, "  float w[7]; w[0] = w1.x; w[1] = w1.y; w[2] = w1.z; w[3] = w1.w; w[4] = w2.x; w[5] = w2.y; w[6] = w2.z;\n"); break;
			case 8: WRITE(p, "  float w[8]; w[0] = w1.x; w[1] = w1.y; w[2] = w1.z; w[3] = w1.w; w[4] = w2.x; w[5] = w2.y; w[6] = w2.z; w[7] = w2.w;\n"); break;
			}

			WRITE(p, "  mat4 skinMatrix = w[0] * u_bone[0];\n");
			if (numBoneWeights > 1) {
				WRITE(p, "  for (int i = 1; i < %i; i++) {\n", numBoneWeights);
				WRITE(p, "    skinMatrix += w[i] * u_bone[i];\n");
				WRITE(p, "  }\n");
			}

#else

#ifdef USE_BONE_ARRAY
			if (numBoneWeights == 1)
				WRITE(p, "  mat4 skinMatrix = w1 * u_bone[0]");
			else
				WRITE(p, "  mat4 skinMatrix = w1.x * u_bone[0]");
			for (int i = 1; i < numBoneWeights; i++) {
				const char *weightAttr = boneWeightAttr[i];
				// workaround for "cant do .x of scalar" issue
				if (numBoneWeights == 1 && i == 0) weightAttr = "w1";
				if (numBoneWeights == 5 && i == 4) weightAttr = "w2";
				WRITE(p, " + %s * u_bone[%i]", weightAttr, i);
			}
#else
			// Uncomment this to screw up bone shaders to check the vertex shader software fallback
			// WRITE(p, "THIS SHOULD ERROR! #error");
			if (numBoneWeights == 1)
				WRITE(p, "  mat4 skinMatrix = w1 * u_bone0");
			else
				WRITE(p, "  mat4 skinMatrix = w1.x * u_bone0");
			for (int i = 1; i < numBoneWeights; i++) {
				const char *weightAttr = boneWeightAttr[i];
				// workaround for "cant do .x of scalar" issue
				if (numBoneWeights == 1 && i == 0) weightAttr = "w1";
				if (numBoneWeights == 5 && i == 4) weightAttr = "w2";
				WRITE(p, " + %s * u_bone%i", weightAttr, i);
			}
#endif

#endif

			WRITE(p, ";\n");

			// Trying to simplify this results in bugs in LBP...
			WRITE(p, "  vec3 skinnedpos = (skinMatrix * vec4(position, 1.0)).xyz %s;\n", factor);
			WRITE(p, "  vec3 worldpos = (u_world * vec4(skinnedpos, 1.0)).xyz;\n");

			if (hasNormal) {
				WRITE(p, "  mediump vec3 skinnednormal = (skinMatrix * vec4(%snormal, 0.0)).xyz %s;\n", flipNormal ? "-" : "", factor);
			} else {
				WRITE(p, "  mediump vec3 skinnednormal = (skinMatrix * vec4(0.0, 0.0, %s1.0, 0.0)).xyz %s;\n", flipNormal ? "-" : "", factor);
			}
			WRITE(p, "  mediump vec3 worldnormal = normalize((u_world * vec4(skinnednormal, 0.0)).xyz);\n");
		}

		WRITE(p, "  vec4 viewPos = u_view * vec4(worldpos, 1.0);\n");

		// Final view and projection transforms.
		WRITE(p, "  gl_Position = u_proj * viewPos;\n");

		// TODO: Declare variables for dots for shade mapping if needed.

		const char *ambientStr = (matUpdate & 1) && hasColor ? "color0" : "u_matambientalpha";
		const char *diffuseStr = (matUpdate & 2) && hasColor ? "color0.rgb" : "u_matdiffuse";
		const char *specularStr = (matUpdate & 4) && hasColor ? "color0.rgb" : "u_matspecular.rgb";

		bool diffuseIsZero = true;
		bool specularIsZero = true;
		bool distanceNeeded = false;

		if (enableLighting) {
			WRITE(p, "  lowp vec4 lightSum0 = u_ambient * %s + vec4(u_matemissive, 0.0);\n", ambientStr);

			for (int i = 0; i < 4; i++) {
				GELightType type = static_cast<GELightType>(id.Bits(BIT_LIGHT0_TYPE + 4*i, 2));
				GELightComputation comp = static_cast<GELightComputation>(id.Bits(BIT_LIGHT0_COMP + 4*i, 2));
				if (doLight[i] != LIGHT_FULL)
					continue;
				diffuseIsZero = false;
				if (comp != GE_LIGHTCOMP_ONLYDIFFUSE)
					specularIsZero = false;
				if (type != GE_LIGHTTYPE_DIRECTIONAL)
					distanceNeeded = true;
			}

			if (!specularIsZero) {
				WRITE(p, "  lowp vec3 lightSum1 = vec3(0.0);\n");
			}
			if (!diffuseIsZero) {
				WRITE(p, "  vec3 toLight;\n");
				WRITE(p, "  lowp vec3 diffuse;\n");
			}
			if (distanceNeeded) {
				WRITE(p, "  float distance;\n");
				WRITE(p, "  lowp float lightScale;\n");
			}
		}

		// Calculate lights if needed. If shade mapping is enabled, lights may need to be
		// at least partially calculated.
		for (int i = 0; i < 4; i++) {
			if (doLight[i] != LIGHT_FULL)
				continue;

			GELightType type = static_cast<GELightType>(id.Bits(BIT_LIGHT0_TYPE + 4*i, 2));
			GELightComputation comp = static_cast<GELightComputation>(id.Bits(BIT_LIGHT0_COMP + 4*i, 2));

			if (type == GE_LIGHTTYPE_DIRECTIONAL) {
				// We prenormalize light positions for directional lights.
				WRITE(p, "  toLight = u_lightpos%i;\n", i);
			} else {
				WRITE(p, "  toLight = u_lightpos%i - worldpos;\n", i);
				WRITE(p, "  distance = length(toLight);\n");
				WRITE(p, "  toLight /= distance;\n");
			}

			bool doSpecular = comp != GE_LIGHTCOMP_ONLYDIFFUSE;
			bool poweredDiffuse = comp == GE_LIGHTCOMP_BOTHWITHPOWDIFFUSE;

			WRITE(p, "  mediump float dot%i = max(dot(toLight, worldnormal), 0.0);\n", i);
			if (poweredDiffuse) {
				// pow(0.0, 0.0) may be undefined, but the PSP seems to treat it as 1.0.
				// Seen in Tales of the World: Radiant Mythology (#2424.)
				WRITE(p, "  if (dot%i == 0.0 && u_matspecular.a == 0.0) {\n", i);
				WRITE(p, "    dot%i = 1.0;\n", i);
				WRITE(p, "  } else {\n");
				WRITE(p, "    dot%i = pow(dot%i, u_matspecular.a);\n", i, i);
				WRITE(p, "  }\n");
			}

			const char *timesLightScale = " * lightScale";

			// Attenuation
			switch (type) {
			case GE_LIGHTTYPE_DIRECTIONAL:
				timesLightScale = "";
				break;
			case GE_LIGHTTYPE_POINT:
				WRITE(p, "  lightScale = clamp(1.0 / dot(u_lightatt%i, vec3(1.0, distance, distance*distance)), 0.0, 1.0);\n", i);
				break;
			case GE_LIGHTTYPE_SPOT:
			case GE_LIGHTTYPE_UNKNOWN:
				WRITE(p, "  lowp float angle%i = dot(normalize(u_lightdir%i), toLight);\n", i, i);
				WRITE(p, "  if (angle%i >= u_lightangle%i) {\n", i, i);
				WRITE(p, "    lightScale = clamp(1.0 / dot(u_lightatt%i, vec3(1.0, distance, distance*distance)), 0.0, 1.0) * pow(angle%i, u_lightspotCoef%i);\n", i, i, i);
				WRITE(p, "  } else {\n");
				WRITE(p, "    lightScale = 0.0;\n");
				WRITE(p, "  }\n");
				break;
			default:
				// ILLEGAL
				break;
			}

			WRITE(p, "  diffuse = (u_lightdiffuse%i * %s) * dot%i;\n", i, diffuseStr, i);
			if (doSpecular) {
				WRITE(p, "  dot%i = dot(normalize(toLight + vec3(0.0, 0.0, 1.0)), worldnormal);\n", i);
				WRITE(p, "  if (dot%i > 0.0)\n", i);
				WRITE(p, "    lightSum1 += u_lightspecular%i * %s * (pow(dot%i, u_matspecular.a) %s);\n", i, specularStr, i, timesLightScale);
			}
			WRITE(p, "  lightSum0.rgb += (u_lightambient%i * %s.rgb + diffuse)%s;\n", i, ambientStr, timesLightScale);
		}

		if (id.Bit(BIT_LIGHTING_ENABLE)) {
			// Sum up ambient, emissive here.
			if (lmode) {
				WRITE(p, "  v_color0 = clamp(lightSum0, 0.0, 1.0);\n");
				// v_color1 only exists when lmode = 1.
				if (specularIsZero) {
					WRITE(p, "  v_color1 = vec3(0.0);\n");
				} else {
					WRITE(p, "  v_color1 = clamp(lightSum1, 0.0, 1.0);\n");
				}
			} else {
				if (specularIsZero) {
					WRITE(p, "  v_color0 = clamp(lightSum0, 0.0, 1.0);\n");
				} else {
					WRITE(p, "  v_color0 = clamp(clamp(lightSum0, 0.0, 1.0) + vec4(lightSum1, 0.0), 0.0, 1.0);\n");
				}
			}
		} else {
			// Lighting doesn't affect color.
			if (hasColor) {
				WRITE(p, "  v_color0 = color0;\n");
			} else {
				WRITE(p, "  v_color0 = u_matambientalpha;\n");
			}
			if (lmode)
				WRITE(p, "  v_color1 = vec3(0.0);\n");
		}

		// Step 3: UV generation
		if (doTexture) {
			switch (uvGenMode) {
			case GE_TEXMAP_TEXTURE_COORDS:  // Scale-offset. Easy.
			case GE_TEXMAP_UNKNOWN: // Not sure what this is, but Riviera uses it.  Treating as coords works.
				if (prescale && !flipV) {
					if (hasTexcoord) {
						WRITE(p, "  v_texcoord = texcoord;\n");
					} else {
						WRITE(p, "  v_texcoord = vec2(0.0);\n");
					}
				} else {
					if (hasTexcoord) {
						WRITE(p, "  v_texcoord = texcoord * u_uvscaleoffset.xy + u_uvscaleoffset.zw;\n");
					} else {
						WRITE(p, "  v_texcoord = u_uvscaleoffset.zw;\n");
					}
				}
				break;

			case GE_TEXMAP_TEXTURE_MATRIX:  // Projection mapping.
				{
					std::string temp_tc;
					switch (uvProjMode) {
					case GE_PROJMAP_POSITION:  // Use model space XYZ as source
						temp_tc = "vec4(position.xyz, 1.0)";
						break;
					case GE_PROJMAP_UV:  // Use unscaled UV as source
						{
							// prescale is false here.
							if (hasTexcoord) {
								static const char *rescaleuv[4] = {"", " * 1.9921875", " * 1.999969482421875", ""}; // 2*127.5f/128.f, 2*32767.5f/32768.f, 1.0f};
								const char *factor = rescaleuv[texFmtScale];
								temp_tc = StringFromFormat("vec4(texcoord.xy %s, 0.0, 1.0)", factor);
							} else {
								temp_tc = "vec4(0.0, 0.0, 0.0, 1.0)";
							}
						}
						break;
					case GE_PROJMAP_NORMALIZED_NORMAL:  // Use normalized transformed normal as source
						if (hasNormal)
							temp_tc = flipNormal ? "vec4(normalize(-normal), 1.0)" : "vec4(normalize(normal), 1.0)";
						else
							temp_tc = "vec4(0.0, 0.0, 1.0, 1.0)";
						break;
					case GE_PROJMAP_NORMAL:  // Use non-normalized transformed normal as source
						if (hasNormal)
							temp_tc = flipNormal ? "vec4(-normal, 1.0)" : "vec4(normal, 1.0)";
						else
							temp_tc = "vec4(0.0, 0.0, 1.0, 1.0)";
						break;
					}
					// Transform by texture matrix. XYZ as we are doing projection mapping.
					WRITE(p, "  v_texcoord = (u_texmtx * %s).xyz * vec3(u_uvscaleoffset.xy, 1.0);\n", temp_tc.c_str());
				}
				break;

			case GE_TEXMAP_ENVIRONMENT_MAP:  // Shade mapping - use dots from light sources.
				WRITE(p, "  v_texcoord = u_uvscaleoffset.xy * vec2(1.0 + dot(normalize(u_lightpos%i), worldnormal), 1.0 + dot(normalize(u_lightpos%i), worldnormal)) * 0.5;\n", ls0, ls1);
				break;

			default:
				// ILLEGAL
				break;
			}

			// Will flip in the fragment for GE_TEXMAP_TEXTURE_MATRIX.
			if (flipV && uvGenMode != GE_TEXMAP_TEXTURE_MATRIX)
				WRITE(p, "  v_texcoord.y = 1.0 - v_texcoord.y;\n");
		}

		// Compute fogdepth
		if (enableFog)
			WRITE(p, "  v_fogdepth = (viewPos.z + u_fogcoef.x) * u_fogcoef.y;\n");
	}
	WRITE(p, "}\n");
}

}  // namespace HighGpu