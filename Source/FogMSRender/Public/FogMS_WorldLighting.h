#pragma once

#include "FogMS_ShadowCache.h" // FogMSRender::BoxRowCount
#include "FogMS_Spatial.h"
#include "FogMS_WeatherLighting.h"

/** Game-thread snapshot of the scene's sky light for the public sky-boundary sources (r.FogMS.World.SkySource 0/2/3/4).
 * Values and RHI references only: no component, proxy or FTexture pointer reaches the render thread. The colour scale
 * itself stays View.SkyLightColor (View UB, read by FogMS_WorldSky); these fields select and gate the source.
 */
struct FFogMSWorldSky
{
	/** A registered, visible, world-affecting USkyLightComponent (ASkyLight actors) was found. */
	bool bValid = false;
	/** USkyLightComponent::IsRealTimeCaptureEnabled(): the value FSkyLightSceneProxy::bRealTimeCaptureEnabled is built from. */
	bool bRealTimeCapture = false;
	/** ULightComponentBase::VolumetricScatteringIntensity (the proxy's VolumetricScatteringIntensity). */
	float VolumetricScatteringIntensity = 0.f;
	/** ULightComponentBase::GetLightColor() (colour * intensity). Zero / invalid gate only, never multiplied. */
	FLinearColor LightColor = FLinearColor::Black;
	/** USkyLightComponent::bLowerHemisphereIsBlack ("Lower Hemisphere Is Solid Color") and LowerHemisphereColor. */
	bool bLowerHemisphereIsSolidColor = false;
	FLinearColor LowerHemisphereColor = FLinearColor::Black;
	/** Render thread: RHI references of USkyLightComponent::GetProcessedSkyTexture(), resolved inside the render command
	 * enqueued by the gather (the resource is alive then). Null when there is no processed cubemap yet. No blend
	 * destination: USkyLightComponent::BlendFraction / BlendDestinationProcessedSkyTexture are protected.
	 */
	FTextureRHIRef ProcessedTexture;
	FSamplerStateRHIRef ProcessedSampler;
	/** Candidate sky light components found; > 1 means the choice may differ from the renderer's (last registered). */
	int32 Count = 0;
	/** W49: a FogMS Weather sky dome (an Is Sky mesh with the weather clouds, AFogMSWeather::IsSkyDomeActive) shows in this world. With Real
	 * Time Capture the capture renders that dome instead of the atmosphere, so the Sky View LUT (atmosphere only) is no longer the sky: the
	 * auto sky source takes the capture's SH (FogMS_WorldSources.cpp). */
	bool bWeatherSky = false;
};

/** Current-frame isotropic lighting in the authored Box, independent of fog history.
 * Native Lumen surface-cache radiance remains subject to native scene coverage.
 */
struct FFogMSWorldRequest : FFogMSSpatialRequest
{
	FVector4f BoxRows[FogMSRender::BoxRowCount];
	float Strength = 0.35f;
	bool bTransport = false;
	int32 Iterations = 24;
	/** Transport convergence tolerance of this Box (AFogMSBoxVolume::TransportTolerance), clamped to [0,1]
	 * by the solver. Negative: use r.FogMS.Transport.Tolerance. Must be finite. Does not affect warm start.
	 */
	float Tolerance = -1.f;
	/** World-space unit vector toward the atmosphere sun light; zero when there is none.
	 * B3 transport rotates its angular quadrature so one ordinate points exactly at the sun.
	 */
	FVector3f DirectionToSun = FVector3f::ZeroVector;
	/** Sky light snapshot for the public sky-boundary sources; unused by the Sky View LUT and SH paths. */
	FFogMSWorldSky Sky;
	/** W50: only the atmosphere sun's incident radiance is attenuated; sky and Lumen already see weather. */
	FFogMSWeatherLighting Weather;
	/** Render thread only. Transport + Emissive Injection: 32^3 UAV-capable Texture3D (FloatRGBA or
	 * RGBA32F) that receives total incident J per Box cell (alpha 1), then is left in SRV state for
	 * the Box Volume material. Null: no field write. An invalid texture fails the request.
	 */
	FTextureRHIRef InjectionTexture;
	/** Hybrid injection (packet row 23.w == 6): native fog keeps all single scattering, so InjectionTexture receives
	 * RGB = J_ms = max(total - uncollided incident, 0) and A = 0.5 + 0.5 * T_sun, the per-cell transmittance toward
	 * the atmosphere sun (medium x RT visibility, 1 without that sun). Requires InjectionTexture; false = full field.
	 */
	bool bHybridInjection = false;
	/** Render thread only; required. The Box's resident BGRA8 density atlas (FFogMSDensityAtlas, X by Y+Z*SizeY,
	 * left in SRV state after its upload). Producers bind it as an ordinary SRV, so it works without -BindlessAll;
	 * packet row 7.z (its heap index) is for overlay consumers only. Null or non-2D fails the request.
	 */
	FTextureRHIRef DensityAtlas;
	/** Render thread only; Transport only. Set when FogMS_TransportPublishesLate(GraphBuilder): the async solver
	 * writes transient graph textures only, and FogMS_PublishWorldLightingLate copies them into the resident atlas
	 * and InjectionTexture from PrePostProcessPass of the same graph. Consumers see the field one frame late.
	 */
	bool bLatePublish = false;
	/** Transport only; caller veto for an r.FogMS.Transport.SolveInterval hold. A hold re-offers what the last
	 * publication left in the resident atlas and InjectionTexture, so the caller clears this whenever that content
	 * is no longer there (e.g. the injection volume was cleared, replaced, or last written for another view).
	 */
	bool bAllowHold = true;
	/** Transport only; AFogMSBoxVolume::LumenBounce (packet row 5.z: 0 Auto -> true, 1 Off -> false). True: boundary rays
	 * that hit geometry read the Lumen surface cache when FogMS_GetLumenSource succeeds this frame (5.8.2 only), else the
	 * public fallback FallbackGroundAlbedo * (sun * visibility * Box-medium transmittance [r.FogMS.World.FallbackMedium]
	 * + SH sky irradiance) / pi. False: always the fallback. Neither fails the request. World (non-transport) ignores it
	 * and still requires the Lumen source.
	 */
	bool bLumenBounce = true;
	/** Transport only; AFogMSBoxVolume::FallbackGroundAlbedo (packet row 6.xyz in the Transport modes). Diffuse albedo of the
	 * surfaces hit by boundary rays when the public fallback lights them (not the Lumen branch). Each channel is clamped to
	 * [0,1] by the solver; a non-finite channel uses 0.3. Part of the hold key (with the packet revision).
	 */
	FLinearColor FallbackGroundAlbedo = FLinearColor(0.3f, 0.3f, 0.3f, 1.0f);
	/** Stable runtime id of the requesting Box (never reused while the process runs; 0 is reserved, see
	 * FogMS_InvalidateWorldLighting_RenderThread). Per-view state (warm start, SolveInterval hold chain, resident atlas,
	 * late publication) is keyed by (view, BoxId, transport), so several Boxes can be requested in one view and graph.
	 * Must stay below 2^31.
	 */
	uint32 BoxId = 0;
	/** True only for the one Box whose field feeds the overlay consumers (the packet Box): with BindlessAll it gets the
	 * resident atlas + bindless descriptor, and FogMS.DumpSpatial dumps it. False (every other Box): Transport with
	 * InjectionTexture only; no resident atlas or descriptor is created and DescriptorIndex stays MAX_uint32.
	 */
	bool bResidentAtlas = true;
	/** Transport only; scheduler (r.FogMS.MaxBoxesPerFrame): this Box is over this frame's solve budget. Only an
	 * r.FogMS.Transport.SolveInterval hold may be returned; otherwise the call adds no pass, writes nothing and returns
	 * bQueued (the caller keeps the injection field as is). Validation failures still fail as usual.
	 */
	bool bHoldOnly = false;
	FFogMSWorldRequest() { FMemory::Memzero(BoxRows, sizeof(BoxRows)); }
};

/** FogMS_BuildWorldLighting result. bHeld (Transport, r.FogMS.Transport.SolveInterval > 1): no solve and no pass
 * this frame; Valid with the descriptor pair of the last publication, whose resident atlas and InjectionTexture keep
 * their content. The caller republishes that pair and must not clear the field. HoldPhase is 1..SolveInterval-1.
 */
struct FFogMSWorldResult : FFogMSSpatialResult
{
	bool bHeld = false;
	/** bHoldOnly request that could not hold: no pass, nothing written, Valid false. Not a failure: the caller keeps
	 * the injection field it last published and must not clear it. */
	bool bQueued = false;
	int32 HoldPhase = 0;
	int32 SolveInterval = 1;
	/** Sky boundary source of the published solve (r.FogMS.World.SkySource), for status text. Holds repeat the last. */
	FString SkySource;
	/** Transport only, for status text: "Lumen" or "fallback (<reason>)" for the surface radiance at boundary-ray hits of
	 * the published solve. Holds repeat the last. Empty for World (non-transport). */
	FString LumenBounce;
};

/** PostTLAS, graphics queue only. One call per Box and view; several Boxes may be requested in one graph, each with its
 * own (view, Request.BoxId) state. Resident atlas (packet Box only, Request.bResidentAtlas): N x (2*N*N).
 * Without BindlessAll only Transport + InjectionTexture is accepted: no resident atlas or descriptor is created
 * (Result.Texture null, DescriptorIndex MAX_uint32); the field reaches fog through InjectionTexture alone.
 * Lower half: additional incident radiance, including per-order damping.
 * Upper half: primary sky + surface/emissive incident radiance.
 * Both are scene-linear, g=0, no receiver sigma_s or camera pre-exposure.
 */
FOGMSRENDER_API FFogMSWorldResult FogMS_BuildWorldLighting(
	FRDGBuilder& GraphBuilder, const FSceneView& View, const FFogMSWorldRequest& Request);
/** True when this graph runs the Transport solver on async compute (r.FogMS.Transport.AsyncCompute resolved on;
 * the solver's own predicate). The caller then sets bLatePublish and defers every graphics consumer of the solve.
 */
FOGMSRENDER_API bool FogMS_TransportPublishesLate(const FRDGBuilder& GraphBuilder);
/** Render thread, before this graph's late copy. The resident Transport atlas of this view and Box when the previous
 * graph completed its late copy (FogMS_PublishWorldLightingLate) or held that copy (SolveInterval hold) for the
 * same Box bounds (BoxRows 0..4). Only the packet Box (bResidentAtlas) has one; any other Box gets false.
 * False otherwise: the caller must publish no field (fail closed).
 */
FOGMSRENDER_API bool FogMS_GetLateTransportField(const FSceneView& View, uint32 BoxId, const FVector4f* BoxRows, uint32& OutDescriptorIndex, int32& OutGridSize);
/** PrePostProcessPass of the graph whose PostTLAS build set bLatePublish for this Box. Graphics copies: transient
 * atlas -> resident atlas (only when one exists, i.e. BindlessAll packet Box) and, when requested, transient field ->
 * InjectionTexture, each left in external SRV access. The graph's blackboard holds one pending entry per (view, Box);
 * this call consumes the one of BoxId. False when there is none for this view and Box or nothing was copied.
 */
FOGMSRENDER_API bool FogMS_PublishWorldLightingLate(FRDGBuilder& GraphBuilder, const FSceneView& View, uint32 BoxId, bool& bOutInjectionCopied);
FOGMSRENDER_API void FogMS_ShutdownWorldLighting_RenderThread(FRHICommandListImmediate& RHICmdList);
/** Breaks the SolveInterval hold chain of every view state of BoxId (0: of every Box, the former behaviour). */
FOGMSRENDER_API void FogMS_InvalidateWorldLighting_RenderThread(uint32 BoxId = 0);
/** Render thread, between graphs. The Box is gone (destroyed, disabled, hidden or no longer active): retire every
 * per-view state of BoxId (resident atlas behind a GPU fence, warm-start atlas back to the pool). */
FOGMSRENDER_API void FogMS_ReleaseWorldLightingBox_RenderThread(FRHICommandListImmediate& RHICmdList, uint32 BoxId);
/** Returns false when there is no current world-mode producer; caller may dump legacy.
 * Dumps the packet Box only (the request with bResidentAtlas): the only Box with a resident atlas. */
FOGMSRENDER_API bool FogMS_DumpWorldLighting_RenderThread(FRHICommandListImmediate& RHICmdList, const FString& PathPrefix);

/** W41 Sun Detail Shadow: the sun-space map of one Box (FogMS_SunDetail.cpp / FogMS_SunDetail.usf).
 * Coordinates: "Local" = Box-axis centimetres (packet rows 2..4, origin at the Box centre; FogMS_IndirectLocalPosition).
 * The map is a sheared box grid: the depth axis A is the Box axis whose slab (the Box, or the height-profile band along Z when
 * the density is provably zero outside it) gives the sun rays the shortest chord; slices are planes of constant Local[A]
 * (w = 0 on the sun-facing face, 1 on the far face); a texel column (u, v) is one sun ray, u along Box axis (A+1)%3 and v along
 * (A+2)%3 measured where the ray enters the slab. Every sun ray through the region is one column, and the steps of all columns
 * share the slice planes. Computed once per game frame on the game thread (AFogMSBoxVolume::UpdateDensity, FogMS_MakeSunDetailBasis)
 * and used unchanged by the MID (FogMS_SunMapU/V/W rows, in the material's cube space) and by the render-thread map build. */
struct FFogMSSunDetailBasis
{
	/** False: no map (no atmosphere sun, sun more than ~6 deg below the horizon, invalid Box). The MID strength is then 0. */
	bool bValid = false;
	/** Box axis (0 X, 1 Y, 2 Z) of the depth slices; U runs along (DepthAxis+1)%3, V along (DepthAxis+2)%3. */
	int32 DepthAxis = 2;
	/** Forward map Local -> map uvw: uvw.x = dot(Local, RowU.xyz) + RowU.w, likewise V and W; the region maps into [0,1]^3. */
	FVector4f RowU = FVector4f(0, 0, 0, 0);
	FVector4f RowV = FVector4f(0, 0, 0, 0);
	FVector4f RowW = FVector4f(0, 0, 0, 0);
	/** Inverse used by the march: column (u, v) enters at Origin + u * AxisU + v * AxisV (Local, on the w = 0 plane); its slice k
	 * samples at that point + (k + 0.5) * Step; StepLength = |Step| [cm]; Slices = number of steps (the map's depth). */
	FVector3f Origin = FVector3f::ZeroVector;
	FVector3f AxisU = FVector3f::ZeroVector;
	FVector3f AxisV = FVector3f::ZeroVector;
	FVector3f Step = FVector3f::ZeroVector;
	float StepLength = 0.f;
	int32 Slices = 0;

	bool Equals(const FFogMSSunDetailBasis& Other) const
	{
		return bValid == Other.bValid && DepthAxis == Other.DepthAxis && RowU == Other.RowU && RowV == Other.RowV && RowW == Other.RowW
			&& Origin == Other.Origin && AxisU == Other.AxisU && AxisV == Other.AxisV && Step == Other.Step
			&& StepLength == Other.StepLength && Slices == Other.Slices;
	}
};

/** Game thread (pure math). DirectionToSun: world unit vector toward the atmosphere sun (zero = none). Axis*: the Box's world unit
 * axes (packet rows 2..4); Extent: positive half extents [cm]. BandMinZ..BandMaxZ: Local Z range that can hold density (the whole
 * Box: -Extent.Z..Extent.Z). Slices: depth steps of the map (r.FogMS.SunMap.Steps). Invalid input or a sun more than ~6 deg below
 * the Box's horizon (DirectionToSun.Z < -0.1) returns bValid false. */
FOGMSRENDER_API FFogMSSunDetailBasis FogMS_MakeSunDetailBasis(const FVector3f& DirectionToSun, const FVector3f& AxisX, const FVector3f& AxisY,
	const FVector3f& AxisZ, const FVector3f& Extent, float BandMinZ, float BandMaxZ, int32 Slices);

/** One Box's map build (render thread, PostTLAS, graphics queue). */
struct FFogMSSunDetailRequest
{
	/** The Box packet of this frame (density rows, current animation phases), as FFogMSWorldRequest::BoxRows. */
	FVector4f BoxRows[FogMSRender::BoxRowCount];
	FFogMSSunDetailBasis Basis;
	/** The Box's density atlas (FFogMSWorldRequest::DensityAtlas): the solver's density function, the same noise and phases. */
	FTextureRHIRef DensityAtlas;
	/** Box-owned volume, Resolution x Resolution x Basis.Slices, PF_G16R16F, UAV: R = mean sun transmittance over the step
	 * (T_hi, read by the material with trilinear filtering), G = extinction sigma of the step [1/m] (for the cell average). */
	FTextureRHIRef MapTexture;
	/** Box-owned volume, 32^3 (the transport grid, texel = cell as the injection field), PF_FloatRGBA, UAV: R = sigma-weighted mean
	 * of T_hi over the cell (Tbar), G = 1 valid (0 = cleared: the material then ignores the map), B = mean sigma [1/m], A = mean T. */
	FTextureRHIRef CellTexture;
	FFogMSSunDetailRequest() { FMemory::Memzero(BoxRows, sizeof(BoxRows)); }
};

/** Adds the map passes (march + cell average) and leaves both textures in external SRV access for the Box material. False (no
 * pass added, OutError set) on invalid input; the caller then clears the cell texture so the material falls back to the W40 math. */
FOGMSRENDER_API bool FogMS_BuildSunDetailMap(FRDGBuilder& GraphBuilder, const FSceneView& View, const FFogMSSunDetailRequest& Request,
	FString& OutError);
/** Clears the cell texture (valid flag 0) so the Box material ignores its map; leaves it in external SRV access. */
FOGMSRENDER_API void FogMS_ClearSunDetailMap(FRDGBuilder& GraphBuilder, const FTextureRHIRef& CellTexture);
