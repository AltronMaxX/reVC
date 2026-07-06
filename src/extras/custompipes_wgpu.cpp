#include "common.h"

#ifdef RW_WGPU
#include "main.h"
#include "RwHelper.h"
#include "Lights.h"
#include "Timecycle.h"
#include "FileMgr.h"
#include "Clock.h"
#include "Weather.h"
#include "TxdStore.h"
#include "Renderer.h"
#include "World.h"
#include "custompipes.h"

#ifdef EXTENDED_PIPELINES

#ifndef LIBRW
#error "Need librw for EXTENDED_PIPELINES"
#endif

/*
 * WGPU port of the neo pipes (see custompipes_gl.cpp for the reference GL
 * implementation). The librw wgpu backend provides the custom object-shader
 * API (rw::wgpu::createObjShader & co): the game supplies a WGSL body with
 * vs_main/fs_main against the standard object binding contract, and
 * objShaderRender handles instancing, uniforms and the draw loop.
 *
 * Unlike the GL shaders there are no V-flips when sampling: the wgpu backend
 * uploads textures and renders camera textures with a top-left origin.
 */

namespace CustomPipes {

/*
 * Neo Vehicle pipe
 */

static rw::wgpu::ObjShader *neoVehicleShader;

// u_params layout: p[0]=eye, p[1]=reflProps (fresnel, lightStrength=speclight
// alpha, shininess, specularity), p[2..6]=specDir[5] (xyz + power in w),
// p[7..11]=specColor[5]
struct VehicleParams {
	float eye[4];
	float reflProps[4];
	float specDir[5][4];
	float specColor[5][4];
};
static VehicleParams vehParams;

static const char *neoVehicle_wgsl = R"(
struct VsIn {
	@location(0) pos    : vec3<f32>,
	@location(1) normal : vec3<f32>,
	@location(2) color  : vec4<f32>,
	@location(3) uv     : vec2<f32>,
	@location(4) uv1    : vec2<f32>,
};
struct VsOut {
	@builtin(position) position  : vec4<f32>,
	@location(0)       color     : vec4<f32>,
	@location(1)       reflcolor : vec4<f32>,
	@location(2)       uv        : vec2<f32>,
	@location(3)       uvEnv     : vec2<f32>,
	@location(4)       fog       : f32,
};

fn DoDirLightSpec(Ldir : vec3<f32>, Lcol : vec3<f32>, N : vec3<f32>, V : vec3<f32>, power : f32) -> vec3<f32> {
	return pow(clamp(dot(N, normalize(V - Ldir)), 0.0, 1.0), power) * Lcol;
}

@vertex
fn vs_main(v : VsIn) -> VsOut {
	var out : VsOut;
	let Vertex = uniforms.world * vec4<f32>(v.pos, 1.0);
	out.position = uniforms.viewProj * Vertex;
	let wm3 = mat3x3<f32>(uniforms.world[0].xyz, uniforms.world[1].xyz, uniforms.world[2].xyz);
	let Normal = wm3 * v.normal;

	let eye           = u_params.p[0].xyz;
	let fresnel       = u_params.p[1].x;
	let lightStrength = u_params.p[1].y;
	let shininess     = u_params.p[1].z;
	let specularity   = u_params.p[1].w;
	let viewVec = normalize(eye - Vertex.xyz);

	var col = v.color;
	col = vec4<f32>(col.rgb
	                + uniforms.ambLight.rgb * uniforms.surfProps.x
	                + DoDynamicLight(Vertex.xyz, Normal) * uniforms.surfProps.z * lightStrength,
	                col.a);
	col = clamp(col, vec4<f32>(0.0), vec4<f32>(1.0));
	out.color = col * uniforms.matColor;

	// reflect the view vector along the normal -> env map coords
	let refl = Normal * dot(viewVec, Normal) * 2.0 - viewVec;
	out.uvEnv = refl.xy * 0.5 + vec2<f32>(0.5);
	let b = 1.0 - clamp(dot(viewVec, Normal), 0.0, 1.0);
	var reflcolor = vec4<f32>(0.0, 0.0, 0.0, mix(b*b*b*b*b, 1.0, fresnel) * shininess);
	for(var i = 0; i < 5; i = i + 1){
		reflcolor = vec4<f32>(reflcolor.rgb
		            + DoDirLightSpec(u_params.p[2+i].xyz, u_params.p[7+i].rgb, Normal, viewVec, u_params.p[2+i].w)
		              * specularity * lightStrength,
		            reflcolor.a);
	}
	out.reflcolor = reflcolor;
	out.uv = v.uv;
	out.fog = DoFog(out.position.w);
	return out;
}

@fragment
fn fs_main(f : VsOut) -> @location(0) vec4<f32> {
	var pass1 = f.color * textureSample(t0, s0, f.uv);
	let envmap = textureSample(t1, s1, f.uvEnv).rgb;
	pass1 = vec4<f32>(mix(pass1.rgb, envmap, f.reflcolor.a), pass1.a);
	pass1 = vec4<f32>(mix(uniforms.fogColor.rgb, pass1.rgb, f.fog), pass1.a);

	let pass2 = f.reflcolor.rgb * f.fog;

	let color = vec4<f32>(pass1.rgb * pass1.a + pass2, pass1.a);
	if(!AlphaTestOK(color.a)) { discard; }
	return color;
}
)";

static void
uploadSpecLights(void)
{
	memset(vehParams.specDir, 0, sizeof(vehParams.specDir));
	memset(vehParams.specColor, 0, sizeof(vehParams.specColor));
	for(int i = 0; i < 1+NUMEXTRADIRECTIONALS; i++)
		vehParams.specDir[i][3] = 1.0f;	// power
	float power = Power.Get();
	Color speccol = SpecColor.Get();
	vehParams.specColor[0][0] = speccol.r;
	vehParams.specColor[0][1] = speccol.g;
	vehParams.specColor[0][2] = speccol.b;
	rw::V3d dir = pDirect->getFrame()->getLTM()->at;
	vehParams.specDir[0][0] = dir.x;
	vehParams.specDir[0][1] = dir.y;
	vehParams.specDir[0][2] = dir.z;
	vehParams.specDir[0][3] = power;
	for(int i = 0; i < NUMEXTRADIRECTIONALS; i++){
		if(pExtraDirectionals[i]->getFlags() & rw::Light::LIGHTATOMICS){
			vehParams.specColor[1+i][0] = pExtraDirectionals[i]->color.red;
			vehParams.specColor[1+i][1] = pExtraDirectionals[i]->color.green;
			vehParams.specColor[1+i][2] = pExtraDirectionals[i]->color.blue;
			dir = pExtraDirectionals[i]->getFrame()->getLTM()->at;
			vehParams.specDir[1+i][0] = dir.x;
			vehParams.specDir[1+i][1] = dir.y;
			vehParams.specDir[1+i][2] = dir.z;
			vehParams.specDir[1+i][3] = power*2.0f;
		}
	}
}

static bool
vehicleMeshCB(rw::wgpu::ObjShader *sh, rw::Atomic *atomic, rw::Material *m, void *user)
{
	vehParams.reflProps[2] = m->surfaceProps.specular * VehicleShininess;
	vehParams.reflProps[3] = m->surfaceProps.specular == 0.0f ? 0.0f : VehicleSpecularity;
	rw::wgpu::setObjShaderParams(sh, &vehParams, sizeof(vehParams));
	return true;
}

static void
vehicleRender(rw::ObjPipeline *pipe, rw::Atomic *atomic)
{
	// TODO: make this less of a kludge (same as GL)
	if(VehiclePipeSwitch == VEHICLEPIPE_MATFX){
		rw::matFXGlobals.pipelines[rw::platform]->render(atomic);
		return;
	}

	rw::V3d eyePos = rw::engine->currentCamera->getFrame()->getLTM()->pos;
	vehParams.eye[0] = eyePos.x;
	vehParams.eye[1] = eyePos.y;
	vehParams.eye[2] = eyePos.z;
	vehParams.eye[3] = 0.0f;
	vehParams.reflProps[0] = Fresnel.Get();
	vehParams.reflProps[1] = SpecColor.Get().a;
	uploadSpecLights();

	rw::wgpu::setObjTexture2(EnvMapTex);
	rw::SetRenderState(rw::SRCBLEND, rw::BLENDONE);

	rw::wgpu::objShaderRender(neoVehicleShader, atomic, vehicleMeshCB, nil, 0);

	rw::wgpu::setObjTexture2(nil);
	rw::SetRenderState(rw::SRCBLEND, rw::BLENDSRCALPHA);
}

void
CreateVehiclePipe(void)
{
	if(CFileMgr::LoadFile("neo/carTweakingTable.dat", work_buff, sizeof(work_buff), "r") <= 0)
		printf("Error: couldn't open 'neo/carTweakingTable.dat'\n");
	else{
		char *fp = (char*)work_buff;
		fp = ReadTweakValueTable(fp, Fresnel);
		fp = ReadTweakValueTable(fp, Power);
		fp = ReadTweakValueTable(fp, DiffColor);
		fp = ReadTweakValueTable(fp, SpecColor);
	}

	neoVehicleShader = rw::wgpu::createObjShader(neoVehicle_wgsl);
	assert(neoVehicleShader);

	rw::ObjPipeline *pipe = rw::ObjPipeline::create();
	pipe->impl.instance = rw::wgpu::objDefaultInstance;
	pipe->impl.uninstance = nil;
	pipe->impl.render = vehicleRender;
	vehiclePipe = pipe;
}

void
DestroyVehiclePipe(void)
{
	rw::wgpu::destroyObjShader(neoVehicleShader);
	neoVehicleShader = nil;

	((rw::ObjPipeline*)vehiclePipe)->destroy();
	vehiclePipe = nil;
}



/*
 * Neo World pipe
 */

static rw::wgpu::ObjShader *neoWorldShader;

// u_params: p[0].rgb = lightmap blend factor
struct WorldParams {
	float lightMap[4];
};
static WorldParams worldParams;

static const char *neoWorld_wgsl = R"(
struct VsIn {
	@location(0) pos    : vec3<f32>,
	@location(1) normal : vec3<f32>,
	@location(2) color  : vec4<f32>,
	@location(3) uv     : vec2<f32>,
	@location(4) uv1    : vec2<f32>,
};
struct VsOut {
	@builtin(position) position : vec4<f32>,
	@location(0)       color    : vec4<f32>,
	@location(1)       uv       : vec2<f32>,
	@location(2)       uv1      : vec2<f32>,
	@location(3)       fog      : f32,
};

@vertex
fn vs_main(v : VsIn) -> VsOut {
	var out : VsOut;
	let Vertex = uniforms.world * vec4<f32>(v.pos, 1.0);
	out.position = uniforms.viewProj * Vertex;
	let wm3 = mat3x3<f32>(uniforms.world[0].xyz, uniforms.world[1].xyz, uniforms.world[2].xyz);
	let Normal = wm3 * v.normal;

	var col = v.color;
	col = vec4<f32>(col.rgb
	                + uniforms.ambLight.rgb * uniforms.surfProps.x
	                + DoDynamicLight(Vertex.xyz, Normal) * uniforms.surfProps.z,
	                col.a);
	col = clamp(col, vec4<f32>(0.0), vec4<f32>(1.0));
	// the GL pipe forces a white material colour, keeping only its alpha
	out.color = col * vec4<f32>(1.0, 1.0, 1.0, uniforms.matColor.a);

	out.uv  = v.uv;
	out.uv1 = v.uv1;
	out.fog = DoFog(out.position.w);
	return out;
}

@fragment
fn fs_main(f : VsOut) -> @location(0) vec4<f32> {
	let tc0 = textureSample(t0, s0, f.uv);
	let tc1 = textureSample(t1, s1, f.uv1);

	let lightMap = vec4<f32>(u_params.p[0].rgb, uniforms.matColor.a);
	var color = tc0 * f.color * (vec4<f32>(1.0) + lightMap * (tc1 - vec4<f32>(1.0)));
	color.a = f.color.a * tc0.a * lightMap.a;

	color = vec4<f32>(mix(uniforms.fogColor.rgb, color.rgb, f.fog), color.a);
	if(!AlphaTestOK(color.a)) { discard; }
	return color;
}
)";

static bool
worldMeshCB(rw::wgpu::ObjShader *sh, rw::Atomic *atomic, rw::Material *m, void *user)
{
	using namespace rw;

	float blend = 0.0f;
	Texture *dualtex = nil;
	if(MatFX::getEffects(m) == MatFX::DUAL){
		MatFX *matfx = MatFX::get(m);
		dualtex = matfx->getDualTexture();
		if(dualtex)
			blend = WorldLightmapBlend.Get()*LightmapMult;
	}
	rw::wgpu::setObjTexture2(dualtex);	// nil -> white (no lightmap)
	worldParams.lightMap[0] = worldParams.lightMap[1] = worldParams.lightMap[2] = blend;
	worldParams.lightMap[3] = 0.0f;
	rw::wgpu::setObjShaderParams(sh, &worldParams, sizeof(worldParams));
	return true;
}

static void
worldRender(rw::ObjPipeline *pipe, rw::Atomic *atomic)
{
	if(!LightmapEnable){
		rw::wgpu::objDefaultRender(pipe, atomic);
		return;
	}
	rw::wgpu::objShaderRender(neoWorldShader, atomic, worldMeshCB, nil, 0);
	rw::wgpu::setObjTexture2(nil);
}

void
CreateWorldPipe(void)
{
	if(CFileMgr::LoadFile("neo/worldTweakingTable.dat", work_buff, sizeof(work_buff), "r") <= 0)
		printf("Error: couldn't open 'neo/worldTweakingTable.dat'\n");
	else
		ReadTweakValueTable((char*)work_buff, WorldLightmapBlend);

	neoWorldShader = rw::wgpu::createObjShader(neoWorld_wgsl);
	assert(neoWorldShader);

	rw::ObjPipeline *pipe = rw::ObjPipeline::create();
	pipe->impl.instance = rw::wgpu::objDefaultInstance;
	pipe->impl.uninstance = nil;
	pipe->impl.render = worldRender;
	worldPipe = pipe;
}

void
DestroyWorldPipe(void)
{
	rw::wgpu::destroyObjShader(neoWorldShader);
	neoWorldShader = nil;

	((rw::ObjPipeline*)worldPipe)->destroy();
	worldPipe = nil;
}




/*
 * Neo Gloss pipe
 */

static rw::wgpu::ObjShader *neoGlossShader;

// u_params: p[0]=eye, p[1].x=glossMult
struct GlossParams {
	float eye[4];
	float reflProps[4];
};
static GlossParams glossParams;

static const char *neoGloss_wgsl = R"(
struct VsIn {
	@location(0) pos    : vec3<f32>,
	@location(1) normal : vec3<f32>,
	@location(2) color  : vec4<f32>,
	@location(3) uv     : vec2<f32>,
	@location(4) uv1    : vec2<f32>,
};
struct VsOut {
	@builtin(position) position : vec4<f32>,
	@location(0)       light    : vec3<f32>,
	@location(1)       uv       : vec2<f32>,
	@location(2)       fog      : f32,
};

@vertex
fn vs_main(v : VsIn) -> VsOut {
	var out : VsOut;
	let Vertex = uniforms.world * vec4<f32>(v.pos, 1.0);
	out.position = uniforms.viewProj * Vertex;

	let viewVec = normalize(u_params.p[0].xyz - Vertex.xyz);
	out.light = normalize(viewVec - uniforms.lightDirection[0].xyz);
	out.uv = v.uv;
	out.fog = DoFog(out.position.w);
	return out;
}

@fragment
fn fs_main(f : VsOut) -> @location(0) vec4<f32> {
	// specular against the constant "compressed" normal (0,0,1) — see the
	// GL neoGloss shaders
	var s = f.light.z;
	s = s * s;
	s = s * s;
	s = s * s;
	let color = textureSample(t0, s0, f.uv) * s * f.fog * u_params.p[1].x;
	if(!AlphaTestOK(color.a)) { discard; }
	return color;
}
)";

static bool
glossMeshCB(rw::wgpu::ObjShader *sh, rw::Atomic *atomic, rw::Material *m, void *user)
{
	if(m->texture == nil)
		return false;
	rw::Texture *tex = GetGlossTex(m);
	if(tex == nil)
		return false;
	rw::wgpu::setObjTexture0(tex);
	return true;
}

static void
glossRender(rw::ObjPipeline *pipe, rw::Atomic *atomic)
{
	using namespace rw;

	worldRender(pipe, atomic);
	if(!GlossEnable)
		return;

	rw::V3d eyePos = rw::engine->currentCamera->getFrame()->getLTM()->pos;
	glossParams.eye[0] = eyePos.x;
	glossParams.eye[1] = eyePos.y;
	glossParams.eye[2] = eyePos.z;
	glossParams.eye[3] = 0.0f;
	glossParams.reflProps[0] = GlossMult;
	glossParams.reflProps[1] = 0.0f;
	glossParams.reflProps[2] = 0.0f;
	glossParams.reflProps[3] = 0.0f;
	rw::wgpu::setObjShaderParams(neoGlossShader, &glossParams, sizeof(glossParams));

	SetRenderState(SRCBLEND, BLENDONE);
	SetRenderState(DESTBLEND, BLENDONE);
	SetRenderState(ZWRITEENABLE, FALSE);
	SetRenderState(ALPHATESTFUNC, ALPHAALWAYS);

	rw::wgpu::objShaderRender(neoGlossShader, atomic, glossMeshCB, nil,
	                          rw::wgpu::OBJRENDER_FORCEBLEND);

	SetRenderState(ZWRITEENABLE, TRUE);
	SetRenderState(ALPHATESTFUNC, ALPHAGREATEREQUAL);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
}

void
CreateGlossPipe(void)
{
	neoGlossShader = rw::wgpu::createObjShader(neoGloss_wgsl);
	assert(neoGlossShader);

	rw::ObjPipeline *pipe = rw::ObjPipeline::create();
	pipe->impl.instance = rw::wgpu::objDefaultInstance;
	pipe->impl.uninstance = nil;
	pipe->impl.render = glossRender;
	glossPipe = pipe;
}

void
DestroyGlossPipe(void)
{
	rw::wgpu::destroyObjShader(neoGlossShader);
	neoGlossShader = nil;

	((rw::ObjPipeline*)glossPipe)->destroy();
	glossPipe = nil;
}



/*
 * Neo Rim pipes
 */

static rw::wgpu::ObjShader *neoRimShader;
static rw::wgpu::ObjShader *neoRimSkinShader;

// u_params: p[0]=viewVec, p[1]=rimData (offset, scale, scaling, 0),
// p[2]=rampStart, p[3]=rampEnd
struct RimParams {
	float viewVec[4];
	float rimData[4];
	float rampStart[4];
	float rampEnd[4];
};
static RimParams rimParams;

// shared vertex-stage rim logic; the skinned variant differs only in the
// skinning prologue
#define RIM_VS_BODY \
	"	var col = inColor;\n" \
	"	col = vec4<f32>(col.rgb\n" \
	"	                + uniforms.ambLight.rgb * uniforms.surfProps.x\n" \
	"	                + DoDynamicLight(Vertex.xyz, Normal) * uniforms.surfProps.z,\n" \
	"	                col.a);\n" \
	"	// rim light\n" \
	"	let rf = u_params.p[1].x - u_params.p[1].y * dot(Normal, u_params.p[0].xyz);\n" \
	"	let rimlight = clamp(mix(u_params.p[3], u_params.p[2], rf) * u_params.p[1].z,\n" \
	"	                     vec4<f32>(0.0), vec4<f32>(1.0));\n" \
	"	col = vec4<f32>(col.rgb + rimlight.rgb, col.a);\n" \
	"	col = clamp(col, vec4<f32>(0.0), vec4<f32>(1.0));\n" \
	"	out.color = col * uniforms.matColor;\n" \
	"	out.uv = inUV;\n" \
	"	out.fog = DoFog(out.position.w);\n"

static const char *neoRim_wgsl =
R"(
struct VsIn {
	@location(0) pos    : vec3<f32>,
	@location(1) normal : vec3<f32>,
	@location(2) color  : vec4<f32>,
	@location(3) uv     : vec2<f32>,
	@location(4) uv1    : vec2<f32>,
};
struct VsOut {
	@builtin(position) position : vec4<f32>,
	@location(0)       color    : vec4<f32>,
	@location(1)       uv       : vec2<f32>,
	@location(2)       fog      : f32,
};

@vertex
fn vs_main(v : VsIn) -> VsOut {
	var out : VsOut;
	let Vertex = uniforms.world * vec4<f32>(v.pos, 1.0);
	out.position = uniforms.viewProj * Vertex;
	let wm3 = mat3x3<f32>(uniforms.world[0].xyz, uniforms.world[1].xyz, uniforms.world[2].xyz);
	let Normal = wm3 * v.normal;
	let inColor = v.color;
	let inUV = v.uv;
)"
RIM_VS_BODY
R"(
	return out;
}

@fragment
fn fs_main(f : VsOut) -> @location(0) vec4<f32> {
	var color = f.color * textureSample(t0, s0, f.uv);
	color = vec4<f32>(mix(uniforms.fogColor.rgb, color.rgb, f.fog), color.a);
	if(!AlphaTestOK(color.a)) { discard; }
	return color;
}
)";

static const char *neoRimSkin_wgsl =
R"(
struct VsIn {
	@location(0) pos     : vec3<f32>,
	@location(1) normal  : vec3<f32>,
	@location(2) color   : vec4<f32>,
	@location(3) uv      : vec2<f32>,
	@location(4) weights : vec4<f32>,
	@location(5) indices : vec4<u32>,
};
struct VsOut {
	@builtin(position) position : vec4<f32>,
	@location(0)       color    : vec4<f32>,
	@location(1)       uv       : vec2<f32>,
	@location(2)       fog      : f32,
};

@vertex
fn vs_main(v : VsIn) -> VsOut {
	var out : VsOut;

	var skinPos = vec3<f32>(0.0);
	var skinNorm = vec3<f32>(0.0);
	for(var i = 0; i < 4; i = i + 1){
		let m = boneMatrices[v.indices[i]];
		let w = v.weights[i];
		skinPos  = skinPos  + (m * vec4<f32>(v.pos, 1.0)).xyz * w;
		let m3 = mat3x3<f32>(m[0].xyz, m[1].xyz, m[2].xyz);
		skinNorm = skinNorm + (m3 * v.normal) * w;
	}

	let Vertex = uniforms.world * vec4<f32>(skinPos, 1.0);
	out.position = uniforms.viewProj * Vertex;
	let wm3 = mat3x3<f32>(uniforms.world[0].xyz, uniforms.world[1].xyz, uniforms.world[2].xyz);
	let Normal = wm3 * skinNorm;
	let inColor = v.color;
	let inUV = v.uv;
)"
RIM_VS_BODY
R"(
	return out;
}

@fragment
fn fs_main(f : VsOut) -> @location(0) vec4<f32> {
	var color = f.color * textureSample(t0, s0, f.uv);
	color = vec4<f32>(mix(uniforms.fogColor.rgb, color.rgb, f.fog), color.a);
	if(!AlphaTestOK(color.a)) { discard; }
	return color;
}
)";

static void
uploadRimData(rw::wgpu::ObjShader *sh, bool enable)
{
	rw::V3d viewVec = rw::engine->currentCamera->getFrame()->getLTM()->at;
	rimParams.viewVec[0] = viewVec.x;
	rimParams.viewVec[1] = viewVec.y;
	rimParams.viewVec[2] = viewVec.z;
	rimParams.viewVec[3] = 0.0f;
	rimParams.rimData[0] = Offset.Get();
	rimParams.rimData[1] = Scale.Get();
	if(enable)
		rimParams.rimData[2] = Scaling.Get()*RimlightMult;
	else
		rimParams.rimData[2] = 0.0f;
	rimParams.rimData[3] = 0.0f;
	Color col = RampStart.Get();
	memcpy(rimParams.rampStart, &col, sizeof(col));
	col = RampEnd.Get();
	memcpy(rimParams.rampEnd, &col, sizeof(col));
	rw::wgpu::setObjShaderParams(sh, &rimParams, sizeof(rimParams));
}

static void
rimRender(rw::ObjPipeline *pipe, rw::Atomic *atomic)
{
	if(!RimlightEnable){
		rw::wgpu::objDefaultRender(pipe, atomic);
		return;
	}
	uploadRimData(neoRimShader, atomic->geometry->flags & rw::Geometry::LIGHT);
	rw::wgpu::objShaderRender(neoRimShader, atomic, nil, nil, 0);
}

static void
rimSkinRender(rw::ObjPipeline *pipe, rw::Atomic *atomic)
{
	if(!RimlightEnable){
		rw::skinGlobals.pipelines[rw::platform]->render(atomic);
		return;
	}
	uploadRimData(neoRimSkinShader, atomic->geometry->flags & rw::Geometry::LIGHT);
	rw::wgpu::objShaderRender(neoRimSkinShader, atomic, nil, nil, 0);
}

void
CreateRimLightPipes(void)
{
	if(CFileMgr::LoadFile("neo/rimTweakingTable.dat", work_buff, sizeof(work_buff), "r") <= 0)
		printf("Error: couldn't open 'neo/rimTweakingTable.dat'\n");
	else{
		char *fp = (char*)work_buff;
		fp = ReadTweakValueTable(fp, RampStart);
		fp = ReadTweakValueTable(fp, RampEnd);
		fp = ReadTweakValueTable(fp, Offset);
		fp = ReadTweakValueTable(fp, Scale);
		fp = ReadTweakValueTable(fp, Scaling);
	}

	neoRimShader = rw::wgpu::createObjShader(neoRim_wgsl);
	assert(neoRimShader);
	neoRimSkinShader = rw::wgpu::createObjSkinShader(neoRimSkin_wgsl);
	assert(neoRimSkinShader);

	rw::ObjPipeline *pipe = rw::ObjPipeline::create();
	pipe->impl.instance = rw::wgpu::objDefaultInstance;
	pipe->impl.uninstance = nil;
	pipe->impl.render = rimRender;
	rimPipe = pipe;

	pipe = rw::ObjPipeline::create();
	pipe->impl.instance = rw::wgpu::objSkinInstance;
	pipe->impl.uninstance = nil;
	pipe->impl.render = rimSkinRender;
	rimSkinPipe = pipe;
}

void
DestroyRimLightPipes(void)
{
	rw::wgpu::destroyObjShader(neoRimShader);
	neoRimShader = nil;

	rw::wgpu::destroyObjShader(neoRimSkinShader);
	neoRimSkinShader = nil;

	((rw::ObjPipeline*)rimPipe)->destroy();
	rimPipe = nil;

	((rw::ObjPipeline*)rimSkinPipe)->destroy();
	rimSkinPipe = nil;
}

}

#endif

#ifdef NEW_RENDERER
#ifndef LIBRW
#error "Need librw for NEW_PIPELINES"
#endif

namespace WorldRender
{

/*
 * WGPU port of the gl3 deferred blend pass. Instead of gl3's low-level
 * setupVertexInput/drawInst, the wgpu backend exposes per-mesh control over
 * the stock object shader (objMeshesNeedingBlend/objRenderMeshes), so we
 * store the atomic + its capture-time matrix and the blend-mesh mask.
 */

struct BuildingInst
{
	rw::Matrix matrix;
	rw::Atomic *atomic;
	uint32 blendMask;	// meshes needing blending (deferred)
	uint8 fadeAlpha;
	bool lighting;
};
static BuildingInst blendInsts[3][2000];
int numBlendInsts[3];

static rw::RGBAf black;

// Render all opaque meshes and put atomics that need blending
// into the deferred list.
void
AtomicFirstPass(RpAtomic *atomic, int pass)
{
	using namespace rw;

	BuildingInst *building = &blendInsts[pass][numBlendInsts[pass]];

	uint32 blendMask = rw::wgpu::objMeshesNeedingBlend(atomic);
	building->atomic = atomic;
	building->blendMask = blendMask;
	building->fadeAlpha = 255;
	building->lighting = !!(atomic->geometry->flags & rw::Geometry::LIGHT);
	building->matrix = *atomic->getFrame()->getLTM();

	const RGBAf *amb = building->lighting ? &pAmbient->color : &black;
	rw::wgpu::objRenderMeshes(atomic, &building->matrix, ~blendMask, 255, amb);

	if(blendMask)
		numBlendInsts[pass]++;
}

void
AtomicFullyTransparent(RpAtomic *atomic, int pass, int fadeAlpha)
{
	BuildingInst *building = &blendInsts[pass][numBlendInsts[pass]];

	rw::wgpu::objMeshesNeedingBlend(atomic);	// just instance
	building->atomic = atomic;
	building->blendMask = 0xFFFFFFFFu;
	building->fadeAlpha = fadeAlpha;
	building->lighting = !!(atomic->geometry->flags & rw::Geometry::LIGHT);
	building->matrix = *atomic->getFrame()->getLTM();
	numBlendInsts[pass]++;
}

void
RenderBlendPass(int pass)
{
	using namespace rw;

	int i;
	for(i = 0; i < numBlendInsts[pass]; i++){
		BuildingInst *building = &blendInsts[pass][i];

		const RGBAf *amb = building->lighting ? &pAmbient->color : &black;
		// fading atomics redraw every mesh (opaque ones were already drawn
		// at full alpha, but the fade needs them blended); otherwise only
		// the deferred blend meshes
		uint32 mask = building->fadeAlpha != 255 ? 0xFFFFFFFFu : building->blendMask;
		rw::wgpu::objRenderMeshes(building->atomic, &building->matrix, mask,
		                          building->fadeAlpha, amb);
	}
}
}
#endif

#endif
