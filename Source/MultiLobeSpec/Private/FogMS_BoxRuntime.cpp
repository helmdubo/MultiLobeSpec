#include "FogMS_BoxRuntime.h"
#include "FogMS_BoxVolume.h"
#include "FogMS_DensityAtlas.h"
#include "FogMS_ShadowCache.h"
#include "FogMS_Spatial.h"
#include "FogMS_WorldLighting.h"
#include "FogMS_ScreenScattering.h"
#include "MultiLobeSpec.h"

#include "Components/BoxComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/TextureRenderTargetVolume.h"
#include "DynamicRHI.h"
#include "Engine/World.h"
#include "Engine/Scene.h"
#include "EngineUtils.h"
#include "HAL/CriticalSection.h"
#include "HAL/IConsoleManager.h"
#if PLATFORM_WINDOWS
#include "ID3D12DynamicRHI.h"
#endif
#include "Misc/ScopeLock.h"
#include "MultiGPU.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "ShaderPlatformConfig.h"
#include "ShaderCompiler.h"
#include "TextureResource.h"

namespace
{
	TAutoConsoleVariable<int32> FogMS_BoxMode(TEXT("r.FogMS.BoxMode"), 0,
		TEXT("0 global A1, 1 live Box. Initial change requires FogMS.Apply and D3D12 -BindlessAll. Actor edits then update live. Without -BindlessAll use Enable Live Box on the actor instead (injection-only runtime; this stays 0)."));
	TAutoConsoleVariable<int32> FogMS_ScreenScatteringSun(TEXT("r.FogMS.ScreenScatteringSun"), 1,
		TEXT("Include the later procedural SkyAtmosphere sun disk in native fog screen-space scattering. Requires native FSSS; 0 restores UE source ordering."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<int32> FogMS_ViewIntegration(TEXT("r.FogMS.ViewIntegration"), 0,
		TEXT("Transport view reconstruction: 0 previous temporal froxel path; 1 rejected mean-coefficient experiment; 2 coherent froxel segments; 3 Box-anchored ray intervals. Diagnostic, not a transport/lighting quality setting."), ECVF_RenderThreadSafe);

	FAutoConsoleCommand FogMS_DumpSpatialCommand(TEXT("FogMS.DumpSpatial"),
		TEXT("Read back the last spatial HDR atlas for verification. Usage: FogMS.DumpSpatial absolute_path_prefix"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() != 1) return;
			ENQUEUE_RENDER_COMMAND(FogMS_DumpSpatial)([Path = Args[0]](FRHICommandListImmediate& RHICmdList)
			{
				if (!FogMS_DumpWorldLighting_RenderThread(RHICmdList, Path))
					FogMS_DumpSpatial_RenderThread(RHICmdList, Path);
			});
		}));

	constexpr uint32 FogMS_BoxPacketRowCount = 24;

	struct FBoxPacket
	{
		// Rows 0..4: center high/active, center low/feather, three unit axes/extent.
		// Row 5: history reset, scattering mode, extra octaves, density animation active.
		// Row 6: contribution, occlusion, eccentricity, authored sun shadow.
		// Rows 7..10: density atlas and shape; row 11: size Z and three detail controls.
		// Row 12: world frequencies f0/f1/f2, world-aligned mode.
		// Rows 13..15: world phases 0/1/2, with surface shadow enabled/strength/steps in W.
		// Row 16: filtered sun shadow enabled/sigma/inside volume; W = Transport convergence
		// tolerance (CPU only: no shader reads it, and it is excluded from the history key;
		// negative = r.FogMS.Transport.Tolerance). Rows 17..20: filtered shadow cache axes/extent, descriptor/grid.
		// Row 21: spatial strength/range/steps/directions; B2 uses 1/diagonal/iterations/6.
		// Row 22: descriptor/grid/valid/mode. Row 23: density albedo RGB, B2 density marker:
		// 4 = overlay injects density + source; 5 = Emissive Injection (native Volume MID owns
		// sigma_t and receives sigma_s*J; every overlay density/source reader requires exactly 4).
		FVector4f Rows[FogMS_BoxPacketRowCount];
		FBoxPacket() { FMemory::Memzero(Rows, sizeof(Rows)); }
	};
	static_assert(sizeof(FBoxPacket) == 384);

	struct FBoxPhases
	{
		FVector3f Values[3];
		FBoxPhases() { FMemory::Memzero(Values, sizeof(Values)); }
		explicit FBoxPhases(const FBoxPacket& Packet)
		{
			for (uint32 Octave = 0; Octave < 3; ++Octave) Values[Octave] = FVector3f(Packet.Rows[13 + Octave]);
		}
	};

	struct FBoxPhaseHistory
	{
		uint64 Frame = MAX_uint64;
		FBoxPhases Current;
		FBoxPhases Previous;

		bool Advance(uint64 NewFrame, const FBoxPhases& Actual)
		{
			const bool bContinuous = Frame == NewFrame || (Frame != MAX_uint64 && NewFrame == Frame + 1);
			if (Frame != NewFrame)
			{
				Previous = bContinuous ? Current : Actual;
				Frame = NewFrame;
			}
			// Multiple families may publish in one game frame. Keep its prior-frame
			// phase fixed, and retain the final actual phase for the following frame.
			Current = Actual;
			return bContinuous;
		}
	};

	struct FBoxRenderSnapshot
	{
		FBoxPacket Packet;
		FBoxPhases PreviousPhases;
		bool bProceduralSunForFSSS = false;
		int32 ViewIntegrationMode = 0;
	};

	struct FPreviewSetting { const TCHAR* Name; float Value; bool bRequired = true; };
	const FPreviewSetting PreviewSettings[] =
	{
		{ TEXT("r.Lumen.HardwareRayTracing"), 1 },
		{ TEXT("r.Lumen.TranslucencyVolume.HardwareRayTracing"), 1 },
		{ TEXT("r.Lumen.TranslucencyVolume.Enable"), 1 },
		{ TEXT("r.Lumen.TranslucencyVolume.TraceFromVolume"), 1 },
		{ TEXT("r.Lumen.TranslucencyVolume.RadianceCache"), 0 },
		{ TEXT("r.Lumen.TranslucencyVolume.ShareRadianceCacheWithOpaque"), 0 },
		{ TEXT("r.Lumen.TranslucencyVolume.SpatialFilter"), 0 },
		{ TEXT("r.Lumen.TranslucencyVolume.GridCenterOffsetFromDepthBuffer"), -1 },
		// Fog consumes raw TLV SH before TLV's own temporal filter. Use a fixed
		// quadrature to avoid pumping the medium attenuation through moving rays.
		// These are quality defaults, not validity requirements: allow live A/B.
		{ TEXT("r.Lumen.TranslucencyVolume.Temporal.Jitter"), 0, false },
		{ TEXT("r.Lumen.TranslucencyVolume.TracingOctahedronResolution"), 8, false },
		// The resident bindless payload is not an RDG parameter. Keep its producers and
		// readers on the graphics queue until an explicit cross-queue dependency exists.
		{ TEXT("r.Lumen.AsyncCompute"), 0 },
		// World transport traces outside the camera. This is not a requirement of A1c.
		{ TEXT("r.RayTracing.Culling"), 0, false },
	};
	TMap<FString, FString> PreviewPreviousValues;
	bool bIndirectPreviewEnabled = false;

	void FogMS_RestorePreviewSettings()
	{
		for (const FPreviewSetting& Setting : PreviewSettings)
		{
			const FString* Previous = PreviewPreviousValues.Find(Setting.Name);
			IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Setting.Name);
			if (Previous && Variable && Variable->GetFloat() == Setting.Value) Variable->Set(**Previous, ECVF_SetByConsole);
		}
		PreviewPreviousValues.Empty();
	}

	float FogMS_ConsoleFloat(const TCHAR* Name, float DefaultValue = 0.0f)
	{
		// All callers run on the game thread; native CVars outlive this extension.
		// Cache successful lookups, but retry a missing variable after module startup.
		static TMap<FString, IConsoleVariable*> Variables;
		IConsoleVariable*& Variable = Variables.FindOrAdd(Name);
		if (!Variable) Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		return Variable ? Variable->GetFloat() : DefaultValue;
	}

	// The engine-shader overlay (packet texture, heap descriptors, A1d/A1e, shadow cache, SSFS sky disk,
	// ViewIntegration, debug views) needs BindlessAll. Without it the runtime is injection-only: the Transport
	// solver writes the Emissive Injection volume and the Box Volume material lights native fog.
	bool FogMS_IsBindlessAll()
	{
		return FShaderPlatformConfig::IsValid(GMaxRHIShaderPlatform)
			&& FShaderPlatformConfig::GetBindlessConfiguration(GMaxRHIShaderPlatform) == ERHIBindlessConfiguration::All;
	}

	bool FogMS_IsLocalOverlayEnabled()
	{
		static const IConsoleVariable* const EnableVariable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.FogMS.Enable"));
		// Never true without BindlessAll, even if r.FogMS.BoxMode was set by hand: no overlay can exist then.
		return FogMS_IsBindlessAll() && FogMS_BoxMode.GetValueOnGameThread() == 1 && EnableVariable && EnableVariable->GetInt() != 0;
	}

	// Status text for an overlay-only feature that is not active.
	const TCHAR* FogMS_OverlayUnavailable()
	{
		return FogMS_IsBindlessAll() ? TEXT("use Enable Live Box to activate the local overlay.")
			: TEXT("requires -BindlessAll (injection-only: no BindlessAll; overlay features off).");
	}

	bool FogMS_IsWorldScatteringMode(EFogMSScatteringMode Mode)
	{
		return Mode == EFogMSScatteringMode::WorldSpace || FogMS_IsTransportMode(Mode);
	}

	bool FogMS_UsesWorldProducer(float Mode)
	{
		return Mode == static_cast<float>(EFogMSScatteringMode::WorldSpace)
			|| FogMS_IsTransportMode(static_cast<EFogMSScatteringMode>(Mode));
	}

	FString FogMS_IndirectViewProblem(const FSceneViewFamily& Family, float BoxDiagonal)
	{
		if (!bIndirectPreviewEnabled) return TEXT("Use Enable Indirect Preview for this editor session.");
		if (FogMS_ConsoleFloat(TEXT("r.RayTracing")) == 0)
			return TEXT("Requires Hardware Ray Tracing (r.RayTracing=1).");
		if (FogMS_ConsoleFloat(TEXT("r.RDG.AsyncCompute"), 1) > 1)
			return TEXT("Forced RDG async compute is unsupported by this preview.");
		for (const FPreviewSetting& Setting : PreviewSettings)
		{
			if (Setting.bRequired && FogMS_ConsoleFloat(Setting.Name, 123456.0f) != Setting.Value)
				return FString::Printf(TEXT("Requires %s=%g. Use Enable Indirect Preview."), Setting.Name, Setting.Value);
		}
		if (Family.Views.IsEmpty()) return TEXT("No rendered view.");
		for (const FSceneView* View : Family.Views)
		{
			if (!View->IsRayTracingAllowedForView()
				|| (View->bIsSceneCapture && FogMS_ConsoleFloat(TEXT("r.RayTracing.SceneCaptures"), -1) == 0))
				return TEXT("Ray tracing is disabled for this view or SceneCapture.");
			if (View->FinalPostProcessSettings.DynamicGlobalIlluminationMethod != EDynamicGlobalIlluminationMethod::Lumen)
				return TEXT("Requires Lumen global illumination for this view.");
			if (View->FinalPostProcessSettings.LumenSkylightLeaking != 0.0f)
				return TEXT("Requires Post Process Skylight Leaking=0 for meaningful ray endpoints.");
			const float MaxTrace = FMath::Min(65000.0f,
				View->FinalPostProcessSettings.LumenMaxTraceDistance * FogMS_ConsoleFloat(TEXT("r.Lumen.TraceDistanceScale"), 1.0f));
			if (!FMath::IsFinite(MaxTrace) || BoxDiagonal >= MaxTrace)
				return FString::Printf(TEXT("Box diagonal (%g cm) must be smaller than this view's trace range (%g cm, capped for R16F)."), BoxDiagonal, MaxTrace);
		}
		return FString();
	}

	FString FogMS_WorldViewProblem(const FSceneViewFamily& Family)
	{
		// World lighting reads the current surface cache directly. It does not depend
		// on TLV radiance-cache/filter settings or native volumetric-fog history.
		if (Family.Views.Num() != 1 || !Family.bRealtimeUpdate)
			return TEXT("World requires one realtime view.");
		const FSceneView* View = Family.Views[0];
		if (View->bIsSceneCapture || View->bIsReflectionCapture)
			return TEXT("World is unavailable in scene or reflection captures.");
		if (FogMS_ConsoleFloat(TEXT("r.RayTracing")) == 0 || !View->IsRayTracingAllowedForView())
			return TEXT("World requires Hardware Ray Tracing for this view.");
		if (View->FinalPostProcessSettings.DynamicGlobalIlluminationMethod != EDynamicGlobalIlluminationMethod::Lumen)
			return TEXT("World requires Lumen global illumination for this view.");
		if (FogMS_ConsoleFloat(TEXT("r.RayTracing.Culling"), -1) != 0)
			return TEXT("World requires r.RayTracing.Culling=0. Enable Indirect Preview sets it for this session.");
		if (FogMS_ConsoleFloat(TEXT("r.LumenScene.GPUDrivenUpdate"), 0) != 0)
			return TEXT("World requires r.LumenScene.GPUDrivenUpdate=0 so Lumen retains card coverage around the Box.");
		if (FogMS_ConsoleFloat(TEXT("r.Lumen.AsyncCompute"), -1) != 0)
			return TEXT("World requires r.Lumen.AsyncCompute=0 (graphics queue).");
		if (FogMS_ConsoleFloat(TEXT("r.RDG.AsyncCompute"), 1) > 1)
			return TEXT("Forced RDG async compute is unsupported by World.");
		return FString();
	}

	void FogMS_WriteScatteringControls(const AFogMSBoxVolume& Actor, FBoxPacket& Packet, FString& OutProblem)
	{
		// The zero-initialized MS rows are canonical for Off, zero contribution and invalid controls.
		if (Actor.ScatteringMode == EFogMSScatteringMode::Off) return;
		if (FogMS_IsTransportMode(Actor.ScatteringMode))
		{
			const float BoxDiagonal = FVector3f(Packet.Rows[2].W, Packet.Rows[3].W, Packet.Rows[4].W).Size() * 2.0f;
			if (static_cast<uint8>(Actor.AngularQuality) > static_cast<uint8>(EFogMSAngularQuality::Medium24)
				|| Actor.TransportIterations < 1 || Actor.TransportIterations > 64
				|| !FMath::IsFinite(Actor.TransportTolerance) || Actor.TransportTolerance < -1.0f || Actor.TransportTolerance > 1.0f
				|| !FMath::IsFinite(BoxDiagonal) || BoxDiagonal <= 0.0f)
			{
				OutProblem = TEXT("Transport requires valid Angular Quality, Iterations in [1,64], finite Tolerance in [-1,1] and a finite positive Box diagonal.");
				return;
			}
			Packet.Rows[5].Y = static_cast<float>(Actor.ScatteringMode);
			Packet.Rows[16].W = Actor.TransportTolerance;
			Packet.Rows[21] = FVector4f(1.0f, BoxDiagonal, static_cast<float>(Actor.TransportIterations),
				Actor.ScatteringMode == EFogMSScatteringMode::Transport ? 6.0f : (Actor.AngularQuality == EFogMSAngularQuality::High96 ? 96.0f : (Actor.AngularQuality == EFogMSAngularQuality::Low16 ? 16.0f : (Actor.AngularQuality == EFogMSAngularQuality::Medium24 ? 24.0f : 48.0f))));
			return;
		}
		if (Actor.ScatteringMode == EFogMSScatteringMode::SpatialPreview || Actor.ScatteringMode == EFogMSScatteringMode::WorldSpace)
		{
			if (!FMath::IsFinite(Actor.SpatialStrength) || Actor.SpatialStrength < 0 || Actor.SpatialStrength > 0.5f
				|| !FMath::IsFinite(Actor.SpatialDistance) || Actor.SpatialDistance < 10 || Actor.SpatialDistance > 2000)
			{
				OutProblem = TEXT("Spatial scattering requires finite Strength in [0,0.5] and Distance in [10,2000] cm.");
				return;
			}
			Packet.Rows[5].Y = static_cast<float>(Actor.ScatteringMode);
			Packet.Rows[21] = FVector4f(Actor.SpatialStrength, Actor.SpatialDistance, 12.0f, 12.0f);
			return;
		}
		if (Actor.ScatteringMode != EFogMSScatteringMode::Octaves)
		{
			OutProblem = FString::Printf(TEXT("FogMS octaves disabled on %s: unsupported Scattering Mode %u. A1 remains active."),
				*Actor.GetPathName(), static_cast<uint32>(Actor.ScatteringMode));
			return;
		}
		if (Actor.ExtraOctaves < 1 || Actor.ExtraOctaves > 2
			|| !FMath::IsFinite(Actor.MSContribution) || Actor.MSContribution < 0.0f || Actor.MSContribution > 1.0f
			|| !FMath::IsFinite(Actor.MSOcclusion) || Actor.MSOcclusion < 0.0f || Actor.MSOcclusion > 1.0f
			|| !FMath::IsFinite(Actor.MSEccentricity) || Actor.MSEccentricity < 0.0f || Actor.MSEccentricity > 1.0f)
		{
			OutProblem = FString::Printf(TEXT("FogMS octaves disabled on %s: require Extra Octaves 1..2 and finite Contribution, Occlusion and Eccentricity in [0,1]. A1 remains active."),
				*Actor.GetPathName());
			return;
		}
		if (Actor.MSContribution == 0.0f) return;
		Packet.Rows[5].Y = static_cast<float>(Actor.ScatteringMode);
		Packet.Rows[5].Z = static_cast<float>(Actor.ExtraOctaves);
		Packet.Rows[6] = FVector4f(Actor.MSContribution, Actor.MSOcclusion, Actor.MSEccentricity, 0.0f);
	}

	struct FBoxGPUState
	{
		FBoxRenderSnapshot RenderSnapshot;
		int32 LastViewIntegrationMode = 0;
		FVector3f DirectionToSun = FVector3f::ZeroVector;
		uint64 Revision = 0;
		uint64 DensityAtlasRevision = 0;
		bool bWorldFieldPublished = false;
		TSharedPtr<FFogMSShadowCacheState, ESPMode::ThreadSafe> ShadowCache;
		FCriticalSection FieldStatusMutex;
		uint64 FieldStatusRevision = 0;
		FString ShadowFieldStatus = TEXT("Waiting for shadow cache");
		FString SpatialFieldStatus = TEXT("Waiting for spatial field");
		FFogMSDensityAtlas DensityAtlas;
		FString LastAtlasProblem;
		FCriticalSection AtlasStatusMutex;
		TMap<uint64, FString> AtlasStatus;
#if PLATFORM_WINDOWS
		TRefCountPtr<ID3D12Resource> NativeTexture;
#endif
		FTextureRHIRef Texture;
		FShaderResourceViewRHIRef SRV;
		uint32 DescriptorIndex = MAX_uint32;
		// Emissive Injection target (Box-owned UTextureRenderTargetVolume RHI texture). Render thread only.
		FTextureRHIRef InjectionTexture;
		// True after a graph wrote a current J into InjectionTexture; a non-publishing frame clears it.
		bool bInjectionFieldWritten = false;
		// r.FogMS.Transport.AsyncCompute one-frame-late publication (render thread only). PostTLAS queues the async
		// solve and records the row 22 it will publish; PrePostProcessPass of the same graph copies and uploads it.
		FVector4f LateField = FVector4f(0, 0, 0, 0);
		bool bLatePublishQueued = false;
		// Last late copy into InjectionTexture: GFrameNumberRenderThread and view. A late PostTLAS more than one frame
		// later (copy skipped without PrePostProcessPass, or a gap) clears the older J before fog samples it. Only
		// that view's fallback clears it: a capture/other family between two of its frames would erase valid J.
		uint32 LateInjectionRenderFrame = 0;
		uint32 LateInjectionViewKey = 0;
		// Uploaded density atlas of the current packet (render thread). Transport/World producers bind it directly
		// (FFogMSWorldRequest::DensityAtlas); the packet's row 7.z heap index serves only overlay consumers.
		FTextureRHIRef DensityAtlasTexture;
		// Injection-only runtime (no packet): the injection volume that this frame's native fog samples holds a
		// current J. Set by PostTLAS; replaces the row 22 test of the SSFS trigger in that configuration.
		bool bInjectionFieldConsumed = false;

		// False in the injection-only runtime (no BindlessAll): no packet texture, no descriptor, no hidden readers.
		// With BindlessAll Prepare fails unless the packet exists, so every existing path sees true.
		bool HasPacket() const { return Texture.IsValid(); }

		bool GetAtlasStatus(uint64 AtlasRevision, FString& OutProblem)
		{
			FScopeLock Lock(&AtlasStatusMutex);
			const FString* Status = AtlasStatus.Find(AtlasRevision);
			if (!Status) return false;
			OutProblem = *Status;
			return true;
		}

		void RecordAtlasStatus(uint64 AtlasRevision, const FString& Problem)
		{
			// Report resource errors to the game thread without touching actors on the render thread.
			FScopeLock Lock(&AtlasStatusMutex);
			AtlasStatus.FindOrAdd(AtlasRevision) = Problem;
		}

		void Upload(FRHICommandListBase& RHICmdList, const FBoxRenderSnapshot& Snapshot)
		{
			// Injection-only: no packet texture and no reader of one; the producers take BoxRows as uniforms.
			if (!HasPacket()) return;
			// Row y=0 preserves the producer's 24-float4 ABI. Only phase XYZ is
			// resident in row y=1; no prior atlas descriptor or resource is retained.
			FBoxPacket TextureRows[2];
			TextureRows[0] = Snapshot.Packet;
			// Previously unused second-row texel. No descriptor, allocation or packet
			// ABI change; this view flag shares the existing resident publication.
			TextureRows[1].Rows[0].W = Snapshot.bProceduralSunForFSSS ? 1.0f : 0.0f;
			TextureRows[1].Rows[0].X = static_cast<float>(Snapshot.ViewIntegrationMode);
			for (uint32 Octave = 0; Octave < 3; ++Octave)
				TextureRows[1].Rows[13 + Octave] = FVector4f(Snapshot.PreviousPhases.Values[Octave], 0.0f);
			// The external resident allocation is stable; UpdateTexture2D restores its SRV state.
			RHICmdList.UpdateTexture2D(Texture, 0, FUpdateTextureRegion2D(0, 0, 0, 0, FogMS_BoxPacketRowCount, 2),
				sizeof(FBoxPacket), reinterpret_cast<const uint8*>(TextureRows));
		}
	};

	struct FWorldPacketState
	{
		FBoxPacket Last;
		FBoxPhaseHistory PhaseHistory;
		FVector3f LastSunDirection = FVector3f::ZeroVector;
		uint64 Revision = 1;
		uint64 LastChangedFrame = 0;
		uint64 LastViewResetFrame = 0;
		uint64 LastAtlasRevision = 0;
		struct FViewRevision { uint64 Revision = 0; uint64 LastSeenFrame = 0; };
		TMap<uint32, FViewRevision> Views;
		TMap<TWeakObjectPtr<AFogMSBoxVolume>, uint64> DensityRevisions;
		FString LastProblem;
	};

	class FBoxViewExtension final : public FSceneViewExtensionBase
	{
	public:
		FBoxViewExtension(const FAutoRegister& AutoRegister, TSharedRef<FBoxGPUState, ESPMode::ThreadSafe> InGPU)
			: FSceneViewExtensionBase(AutoRegister), GPU(InGPU) {}

		virtual void BeginRenderViewFamily(FSceneViewFamily& Family) override
		{
			// Enqueued before this family's rendering commands. No private Renderer state or View UB edits.
			if (!Family.Scene) return;
			UWorld* World = Family.Scene->GetWorld();
			FBoxPacket Packet;
			FVector3f DirectionToSun = FVector3f::ZeroVector;
			if (World)
			{
				for (TActorIterator<ADirectionalLight> It(World); It; ++It)
				{
					const UDirectionalLightComponent* Light = Cast<UDirectionalLightComponent>(It->GetLightComponent());
					if (Light && Light->IsVisible() && Light->bAffectsWorld && Light->bAtmosphereSunLight && Light->AtmosphereSunLightIndex == 0)
					{
						DirectionToSun = FVector3f(-Light->GetForwardVector()).GetSafeNormal();
						break;
					}
				}
			}
			FString Problem;
			int32 Count = 0;
			AFogMSBoxVolume* Selected = nullptr;
			FFogMSDensityAtlas::FUploadPtr DensityUpload;
			FWorldPacketState& Previous = Worlds.FindOrAdd(World);
			TMap<TWeakObjectPtr<AFogMSBoxVolume>, uint64> DensityRevisions;
			if (World && (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game))
			{
				for (TActorIterator<AFogMSBoxVolume> It(World); It; ++It)
				{
					if (!It->IsActorBeingDestroyed())
					{
						It->IndirectShadowStatus = It->bIndirectShadowing ? TEXT("Waiting for one valid, visible Box") : TEXT("Off");
						It->SunShadowStatus = It->bAuthoredSunShadow ? TEXT("Bypassed: requires one valid, visible Box") : TEXT("Off");
						It->SurfaceShadowStatus = It->bCastSunShadow ? TEXT("Bypassed: requires one valid, visible Box") : TEXT("Off");
						It->SpatialStatus = (It->ScatteringMode == EFogMSScatteringMode::SpatialPreview || FogMS_IsWorldScatteringMode(It->ScatteringMode))
							? TEXT("Waiting for valid visible Box") : TEXT("Off");
						// Authored density stays valid independently; enabled B2 suppresses only native voxelization.
						It->UpdateDensity();
						DensityRevisions.Add(*It, It->GetDensityRevision());
					}
					if (!It->bEnabled || It->IsActorBeingDestroyed())
					{
						It->IndirectShadowStatus = TEXT("Off (Box Enabled is false)");
						It->SunShadowStatus = TEXT("Off (Box Enabled is false)");
						It->SurfaceShadowStatus = TEXT("Off (Box Enabled is false)");
						It->SpatialStatus = TEXT("Off (Box Enabled is false)");
						continue;
					}
#if WITH_EDITOR
					// Match PPV editor visibility: Outliner eye, layers and hidden levels.
					// The component's HiddenInGame only hides the bounds, not this effect.
					const bool bHidden = World->UsesGameHiddenFlags() ? It->IsHidden() : It->IsHiddenEd();
#else
					const bool bHidden = It->IsHidden();
#endif
					if (!bHidden) { Selected = *It; ++Count; }
					else
					{
						It->IndirectShadowStatus = TEXT("Off (Box hidden)");
						It->SunShadowStatus = TEXT("Off (Box hidden)");
						It->SurfaceShadowStatus = TEXT("Off (Box hidden)");
						It->SpatialStatus = TEXT("Off (Box hidden)");
					}
				}
			}
			if (Count > 1)
			{
				Problem = TEXT("More than one enabled, visible FogMS Box in this world; local FogMS bypassed until only one is active.");
			}
			else if (Count == 1)
			{
				const UBoxComponent* Box = Selected->GetBoxComponent();
				const FVector Center = Box->GetComponentLocation();
				const FVector Extent = Box->GetScaledBoxExtent().GetAbs();
				const FQuat Rotation = Box->GetComponentQuat();
				const float Feather = Selected->FeatherDistance;
				if (Center.ContainsNaN() || Extent.ContainsNaN() || Rotation.ContainsNaN()
					|| Extent.GetMin() < 0.01 || !FMath::IsFinite(Feather) || Feather < 0.0f)
				{
					Problem = TEXT("FogMS Box has an invalid transform, extent or feather; local FogMS bypassed.");
					if (Selected->bAuthoredSunShadow) Selected->SunShadowStatus = TEXT("Bypassed: invalid Box transform, extent or feather.");
					if (Selected->bCastSunShadow) Selected->SurfaceShadowStatus = TEXT("Bypassed: invalid Box transform, extent or feather.");
				}
				else
				{
					const FVector3f High(Center);
					const FVector3f Low(Center - FVector(High));
					Packet.Rows[0] = FVector4f(High, 1.0f);
					Packet.Rows[1] = FVector4f(Low, FMath::Min(Feather, static_cast<float>(Extent.GetMin())));
					Packet.Rows[2] = FVector4f(FVector3f(Rotation.GetAxisX()), static_cast<float>(Extent.X));
					Packet.Rows[3] = FVector4f(FVector3f(Rotation.GetAxisY()), static_cast<float>(Extent.Y));
					Packet.Rows[4] = FVector4f(FVector3f(Rotation.GetAxisZ()), static_cast<float>(Extent.Z));
					FogMS_WriteScatteringControls(*Selected, Packet, Problem);
					// The authored sun path is independent of scattering octaves and indirect preview.
					Packet.Rows[6].W = Selected->bAuthoredSunShadow ? 1.0f : 0.0f;
					Selected->IndirectShadowStatus = TEXT("Off");
					Selected->SunShadowStatus = TEXT("Off");
					Selected->SurfaceShadowStatus = TEXT("Off");
					FString DensityProblem;
					bool bAtlasStatusKnown = false;
					if (Selected->bAuthoredSunShadow || Selected->bIndirectShadowing || Selected->bCastSunShadow
						|| Selected->ScatteringMode == EFogMSScatteringMode::SpatialPreview || FogMS_IsWorldScatteringMode(Selected->ScatteringMode))
					{
						if (!Selected->IsDensitySourceActive())
						{
							DensityProblem = Selected->GetDensityProblem();
							if (DensityProblem.IsEmpty())
								DensityProblem = Selected->bDensityEnabled ? TEXT("Requires a visible Box density source with Density > 0.") : TEXT("Density Enabled is off.");
						}
						else
						{
							DensityUpload = GPU->DensityAtlas.Prepare(Selected->DensityTexture.Get(), DensityProblem);
							if (DensityUpload.IsValid())
							{
								// The three shadow paths share the same validated authored density.
								const FVector Scale = Box->GetComponentScale();
								const float DensityRaySteps = FogMS_IsTransportMode(Selected->ScatteringMode)
									? 16.0f : static_cast<float>(FMath::Clamp(Selected->IndirectShadowSteps, 1, 64));
								Packet.Rows[7] = FVector4f(0, DensityRaySteps, 0, Selected->Density * 0.01f);
								Packet.Rows[8] = FVector4f(FVector3f(Selected->bWorldAlignedTexture ? FVector::OneVector : Selected->TileScale), Selected->Threshold);
								Packet.Rows[9] = FVector4f(Selected->Softness, Selected->DensityEdgeFeather,
									Scale.X < 0 ? -1.0f : 1.0f, Scale.Y < 0 ? -1.0f : 1.0f);
								Packet.Rows[10] = FVector4f(Scale.Z < 0 ? -1.0f : 1.0f, static_cast<float>(Selected->DensityChannel),
									static_cast<float>(DensityUpload->SizeX), static_cast<float>(DensityUpload->SizeY));
								Packet.Rows[11] = FVector4f(static_cast<float>(DensityUpload->SizeZ), Selected->DetailStrength,
									Selected->DetailScale, Selected->DetailSecondOctave);
								FVector3f Phase0, Phase1, Phase2;
								Selected->GetDensityWorldMapping(Packet.Rows[12], Phase0, Phase1, Phase2);
								// Current phases are shared with the MID. The animation marker enables
								// local density-reactive rejection in the native fog history consumer.
								Packet.Rows[5].W = Selected->IsDensityAnimationActive() ? 1.0f : 0.0f;
								Packet.Rows[13] = FVector4f(Phase0, 0.0f);
								Packet.Rows[14] = FVector4f(Phase1, 0.0f);
								Packet.Rows[15] = FVector4f(Phase2, 0.0f);
								if (FogMS_IsTransportMode(Selected->ScatteringMode))
								{
									// Density injection must survive a lighting-producer fallback:
									// the native MID is zero while this authored Transport box is enabled.
									// Emissive Injection (5 full field, 6 hybrid) instead keeps the native MID density
									// on and the overlay adds neither density nor source for this Box.
									const FLinearColor Albedo = Selected->DensityAlbedo;
									Packet.Rows[23] = FVector4f(Albedo.R, Albedo.G, Albedo.B, Selected->IsEmissiveInjectionActive()
										? (Selected->IsHybridInjectionActive() ? 6.0f : 5.0f) : 4.0f);
								}
								bAtlasStatusKnown = GPU->GetAtlasStatus(DensityUpload->Revision, DensityProblem);
							}
						}
					}
					if (Selected->bAuthoredSunShadow)
					{
						if (!FogMS_IsLocalOverlayEnabled())
						{
							Packet.Rows[6].W = 0.0f;
							Selected->SunShadowStatus = FString(TEXT("Bypassed: ")) + FogMS_OverlayUnavailable();
						}
						else if (!DensityUpload.IsValid() || !DensityProblem.IsEmpty())
						{
							Packet.Rows[6].W = 0.0f;
							if (DensityProblem.IsEmpty()) DensityProblem = TEXT("Density atlas is unavailable.");
							Selected->SunShadowStatus = TEXT("Fallback to A1: ") + DensityProblem;
							Problem = TEXT("FogMS authored sun shadow bypassed: ") + DensityProblem;
						}
						else
						{
							Selected->SunShadowStatus = bAtlasStatusKnown
								? TEXT("Active authored density + height fog") : TEXT("Waiting for density atlas GPU upload");
						}
					}
					if (Selected->bCastSunShadow)
					{
						FString SurfaceProblem;
						if (!FogMS_IsLocalOverlayEnabled())
							SurfaceProblem = FogMS_IsBindlessAll() ? TEXT("Use Enable Live Box to activate the local overlay.") : FogMS_OverlayUnavailable();
						else if (!FMath::IsFinite(Selected->SurfaceShadowStrength)
							|| Selected->SurfaceShadowStrength < 0.0f || Selected->SurfaceShadowStrength > 1.0f
							|| Selected->SurfaceShadowSteps < 1 || Selected->SurfaceShadowSteps > 64)
							SurfaceProblem = TEXT("Require finite Strength in [0,1] and Steps in [1,64].");
						else if (!DensityUpload.IsValid() || !DensityProblem.IsEmpty())
							SurfaceProblem = DensityProblem.IsEmpty() ? TEXT("Density atlas is unavailable.") : DensityProblem;
						if (SurfaceProblem.IsEmpty())
						{
							Packet.Rows[13].W = 1.0f;
							Packet.Rows[14].W = Selected->SurfaceShadowStrength;
							Packet.Rows[15].W = static_cast<float>(Selected->SurfaceShadowSteps);
							if (Selected->bFilteredSunShadow && FMath::IsFinite(Selected->ShadowFilterSigma)
								&& Selected->ShadowFilterSigma >= 0 && Selected->ShadowFilterSigma <= 1000 && !DirectionToSun.IsNearlyZero())
								Packet.Rows[16] = FVector4f(1, Selected->ShadowFilterSigma, Selected->bFilterSunInsideVolume ? 1.0f : 0.0f,
									Packet.Rows[16].W); // W: Transport tolerance, written with the scattering controls.
							Selected->SurfaceShadowStatus = !bAtlasStatusKnown ? TEXT("Waiting for density atlas GPU upload")
								: (Selected->SurfaceShadowStrength > 0.0f ? TEXT("Active authored-density surface sun shadow")
									: TEXT("Ready (Strength=0: native surface sunlight)"));
						}
						else
						{
							Selected->SurfaceShadowStatus = TEXT("Bypassed: ") + SurfaceProblem;
							if (!Problem.IsEmpty()) Problem += TEXT(" ");
							Problem += TEXT("FogMS surface sun shadow bypassed: ") + SurfaceProblem;
						}
					}
					if (FogMS_IsTransportMode(Selected->ScatteringMode))
					{
						// B2 attenuation follows authored density, without the A1c artistic blend
						// or its legacy TLV ray overlay. Preserve the actor's saved controls.
						Packet.Rows[7].X = 0.0f;
						Selected->IndirectShadowStatus = TEXT("Not used by Transport; attenuation follows authored density.");
					}
					else if (Selected->bIndirectShadowing)
					{
						FString IndirectProblem = DensityProblem;
						if (IndirectProblem.IsEmpty() && (!FMath::IsFinite(Selected->IndirectShadowStrength)
							|| Selected->IndirectShadowStrength < 0 || Selected->IndirectShadowStrength > 1
							|| Selected->IndirectShadowSteps < 1 || Selected->IndirectShadowSteps > 64))
							IndirectProblem = TEXT("Require finite Strength in [0,1] and Steps in [1,64].");
						if (IndirectProblem.IsEmpty())
							IndirectProblem = Selected->ScatteringMode == EFogMSScatteringMode::WorldSpace
								? FogMS_WorldViewProblem(Family)
								: FogMS_IndirectViewProblem(Family, static_cast<float>(Extent.Size() * 2.0));
						if (IndirectProblem.IsEmpty()) Packet.Rows[7].X = Selected->IndirectShadowStrength;
						Selected->IndirectShadowStatus = IndirectProblem.IsEmpty()
							? (Selected->IndirectShadowStrength > 0
								? (Selected->ScatteringMode == EFogMSScatteringMode::WorldSpace ? TEXT("Active World ray attenuation (independent of TLV cache)") : TEXT("Active experimental ray attenuation"))
								: TEXT("Ready (Strength=0: unattenuated indirect lighting)"))
							: IndirectProblem;
						if (!IndirectProblem.IsEmpty())
						{
							if (!Problem.IsEmpty()) Problem += TEXT(" ");
							Problem += TEXT("FogMS indirect bypassed: ") + IndirectProblem;
						}
					}
				}
			}
			if (Selected && Count == 1 && (Packet.Rows[5].Y == 2.0f || FogMS_UsesWorldProducer(Packet.Rows[5].Y)))
			{
				const bool bTransport = FogMS_IsTransportMode(static_cast<EFogMSScatteringMode>(Packet.Rows[5].Y));
				const bool bWorldLighting = FogMS_UsesWorldProducer(Packet.Rows[5].Y);
				FString SpatialProblem;
				// Without BindlessAll only Transport + Emissive Injection runs (solver -> injection volume -> Box Volume
				// material -> native fog); every other mode needs the overlay consumers. Fail closed: native fog lighting.
				const bool bInjectionOnly = !FogMS_IsBindlessAll();
				if (bInjectionOnly && !bTransport)
					SpatialProblem = TEXT("Spatial/World scattering requires -BindlessAll (injection-only: no BindlessAll; overlay features off). Native fog lighting.");
				else if (bInjectionOnly && !Selected->IsEmissiveInjectionActive())
					SpatialProblem = TEXT("Transport needs Emissive Injection or -BindlessAll (injection-only: no BindlessAll). Native fog lighting with authored density.");
				else if (!bInjectionOnly && !FogMS_IsLocalOverlayEnabled()) SpatialProblem = TEXT("Use Enable Live Box first.");
				else if (!DensityUpload.IsValid() || !Selected->IsDensitySourceActive())
					SpatialProblem = Selected->GetDensityProblem().IsEmpty() ? TEXT("Requires a valid authored density source.") : Selected->GetDensityProblem();
				else if (bWorldLighting) SpatialProblem = FogMS_WorldViewProblem(Family);
				else if (FogMS_ConsoleFloat(TEXT("r.VolumetricFog.TemporalReprojection"), 1) == 0)
					SpatialProblem = TEXT("Requires native fog history (TemporalReprojection=1).");
				else if (FogMS_ConsoleFloat(TEXT("r.Lumen.AsyncCompute")) != 0)
					SpatialProblem = TEXT("Use Enable Indirect Preview to keep the shared payload on the graphics queue.");
				else if (FogMS_ConsoleFloat(TEXT("r.RDG.AsyncCompute"), 1) > 1)
					SpatialProblem = TEXT("Forced RDG async compute is unsupported by Spatial Preview.");
				for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
				{
					const UExponentialHeightFogComponent* Fog = It->GetComponent();
					const float PhaseTolerance = bWorldLighting ? 0.000001f : 0.00001f;
					if (Fog && Fog->IsVisible() && Fog->bEnableVolumetricFog
						&& (!FMath::IsFinite(Fog->VolumetricFogScatteringDistribution) || FMath::Abs(Fog->VolumetricFogScatteringDistribution) > PhaseTolerance))
						SpatialProblem = TEXT("Spatial scattering requires fog Scattering Distribution=0.");
				}
				if (bWorldLighting && SpatialProblem.IsEmpty())
				{
					const FLinearColor Albedo = Selected->DensityAlbedo;
					if (!FMath::IsFinite(Albedo.R) || !FMath::IsFinite(Albedo.G) || !FMath::IsFinite(Albedo.B)
						|| FMath::Min3(Albedo.R, Albedo.G, Albedo.B) < 0 || FMath::Max3(Albedo.R, Albedo.G, Albedo.B) > 1)
						SpatialProblem = TEXT("World requires finite Density Albedo RGB in [0,1].");
					else
					{
						// Transport delivery marker: 4 overlay, 5 full-field injection, 6 hybrid injection.
						Packet.Rows[23] = FVector4f(Albedo.R, Albedo.G, Albedo.B,
							bTransport ? (Selected->IsEmissiveInjectionActive() ? (Selected->IsHybridInjectionActive() ? 6.0f : 5.0f) : 4.0f) : 0.0f);
						// Public additional streaming origin: keep native Lumen cards
						// around the medium when the real camera leaves it. Preserve
						// existing origins; native Lumen admits at most one extra origin.
						Family.StreamingViewOrigins.Insert(FVector(Packet.Rows[0]) + FVector(Packet.Rows[1]), 0);
					}
				}
				if (!SpatialProblem.IsEmpty())
				{
					Packet.Rows[21].X = 0;
					if (bWorldLighting) Packet.Rows[5].Y = 0;
				}
				Selected->SpatialStatus = SpatialProblem.IsEmpty()
					? (bTransport ? TEXT("Waiting for current-frame isotropic transport")
						: (bWorldLighting ? TEXT("Waiting for current-frame World primary + three scattering orders")
							: (Packet.Rows[21].X > 0 ? TEXT("Waiting for current spatial field") : TEXT("Off (Spatial Strength=0)")))) : SpatialProblem;
			}
			else if (Selected && Count == 1 && FogMS_IsWorldScatteringMode(Selected->ScatteringMode) && !Problem.IsEmpty())
			{
				Selected->SpatialStatus = Problem;
			}
			if (!FogMS_IsBindlessAll())
			{
				// Injection-only: A1d (6.W), A1e (13.W/14.W) and the sun-shadow cache (16.X) are overlay features.
				// Already zero through FogMS_IsLocalOverlayEnabled(); forced here so BuildShadow can never run.
				Packet.Rows[6].W = 0.0f;
				Packet.Rows[13].W = Packet.Rows[14].W = 0.0f;
				Packet.Rows[16].X = 0.0f;
			}
			const uint64 AtlasRevision = DensityUpload.IsValid() ? DensityUpload->Revision : 0;
			bool bDensityChanged = DensityRevisions.Num() != Previous.DensityRevisions.Num() || AtlasRevision != Previous.LastAtlasRevision;
			Previous.LastAtlasRevision = AtlasRevision;
			for (const auto& Pair : DensityRevisions)
			{
				const uint64* OldRevision = Previous.DensityRevisions.Find(Pair.Key);
				bDensityChanged |= !OldRevision || *OldRevision != Pair.Value;
			}
			Previous.DensityRevisions = MoveTemp(DensityRevisions);
			// Only continuous animation phases are absent from the global history key.
			// DensityRevision still covers authored velocity edits, seeks, mode switches,
			// bounds changes and backwards time. The actual packet retains all phases:
			// the MID, sun cache and current-frame lighting must see their current values.
			FBoxPacket HistoryPacket = Packet;
			if (HistoryPacket.Rows[5].W > 0.5f)
			{
				for (int32 Row = 13; Row <= 15; ++Row)
				{
					HistoryPacket.Rows[Row].X = HistoryPacket.Rows[Row].Y = HistoryPacket.Rows[Row].Z = 0.0f;
				}
			}
			// The transport tolerance only stops the per-frame solve earlier or later; the warm-start
			// continuation and native fog history stay valid, so it must not bump the revision.
			HistoryPacket.Rows[16].W = 0.0f;
			// Previous.Last retains zero in row 5.x, so reset itself cannot change revision.
			if (bDensityChanged || !DirectionToSun.Equals(Previous.LastSunDirection, 1.0e-6f)
				|| FMemory::Memcmp(HistoryPacket.Rows, Previous.Last.Rows, sizeof(Packet.Rows)) != 0)
			{
				Previous.Last = HistoryPacket;
				Previous.LastSunDirection = DirectionToSun;
				++Previous.Revision;
				Previous.LastChangedFrame = GFrameCounter;
			}
			// A persistent capture may skip the two changed frames. Reject its old revision
			// when it next renders. OR the flag over this frame so families batched together
			// for one Scene all receive a consistent packet, including stereo views.
			for (const FSceneView* View : Family.Views)
			{
				const uint32 Key = View->GetViewKey();
				if (!Key) continue;
				FWorldPacketState::FViewRevision& Seen = Previous.Views.FindOrAdd(Key);
				if (Seen.Revision != Previous.Revision) Previous.LastViewResetFrame = GFrameCounter;
				Seen.Revision = Previous.Revision;
				Seen.LastSeenFrame = GFrameCounter;
			}
			for (auto It = Previous.Views.CreateIterator(); It; ++It)
			{
				if (GFrameCounter > It.Value().LastSeenFrame + 600) It.RemoveCurrent();
			}
			Packet.Rows[5].X = (GFrameCounter <= Previous.LastChangedFrame + 1
				|| Previous.LastViewResetFrame == GFrameCounter) ? 1.0f : 0.0f;
			// Unlike Last/HistoryPacket, this state keeps the actual animation phases.
			// They can reconstruct previous density using the current atlas only for
			// continuous animation; structural edits already reset native history.
			const bool bContinuousPhase = Previous.PhaseHistory.Advance(GFrameCounter, FBoxPhases(Packet));
			if (!bContinuousPhase && Packet.Rows[5].W > 0.5f)
			{
				Previous.LastViewResetFrame = GFrameCounter;
				Packet.Rows[5].X = 1.0f; // No reliable previous phase on first use or after a skipped frame.
			}
			FBoxRenderSnapshot Snapshot;
			Snapshot.Packet = Packet;
			Snapshot.PreviousPhases = Previous.PhaseHistory.Previous;
			if (Problem != Previous.LastProblem)
			{
				if (!Problem.IsEmpty()) UE_LOG(LogMultiLobeSpec, Warning, TEXT("%s"), *Problem);
				Previous.LastProblem = Problem;
			}
			for (auto It = Worlds.CreateIterator(); It; ++It)
			{
				if (!It.Key().IsValid()) It.RemoveCurrent();
			}
			if (Selected && Count == 1)
			{
				FScopeLock Lock(&GPU->FieldStatusMutex);
				if (GPU->FieldStatusRevision == Previous.Revision)
				{
					if (Packet.Rows[16].X > 0) Selected->SurfaceShadowStatus = GPU->ShadowFieldStatus;
					if (FogMS_UsesWorldProducer(Packet.Rows[5].Y) || (Packet.Rows[5].Y == 2 && Packet.Rows[21].X > 0))
						Selected->SpatialStatus = GPU->SpatialFieldStatus;
				}
			}
			// Emissive Injection: hand the Box-owned field to the render thread. The resource is
			// created and released only through render commands enqueued in game-thread order
			// (UpdateResource / UTexture::BeginDestroy), so it is alive when this command runs
			// (same hand-off as NiagaraDataInterfaceRenderTargetVolume). Only its RHI ref is kept.
			FTextureRenderTargetResource* InjectionResource = nullptr;
			if (Selected && Selected->IsEmissiveInjectionActive() && IsValid(Selected->TransportField))
				InjectionResource = Selected->TransportField->GameThread_GetRenderTargetResource();
			ENQUEUE_RENDER_COMMAND(FogMS_UpdateBox)([Resource = GPU, Snapshot, DensityUpload, DirectionToSun, Revision = Previous.Revision, InjectionResource](FRHICommandListImmediate& RHICmdList) mutable
			{
				FBoxPacket& Packet = Snapshot.Packet;
				{
					FTextureRHIRef Injection = InjectionResource ? InjectionResource->GetTextureRHI() : FTextureRHIRef();
					if (Injection.GetReference() != Resource->InjectionTexture.GetReference())
					{
						// A new field starts cleared (ClearColor alpha 0); a dropped one is no longer sampled.
						Resource->InjectionTexture = Injection;
						Resource->bInjectionFieldWritten = false;
					}
				}
				// Hidden bindless reads have no RDG dependency. Fence prior async readers
				// before overwriting the packet or retiring an atlas, including manual CVar changes.
				// Injection-only: no packet and no hidden reader (the atlas is bound to graphics pass 0 only),
				// so there is nothing to fence; skip both cross-pipe transitions.
				const bool bFence = Resource->HasPacket();
				if (bFence) RHICmdList.Transition(TArrayView<const FRHITransitionInfo>(), ERHIPipeline::AsyncCompute, ERHIPipeline::Graphics);
				if (DensityUpload.IsValid())
				{
					FString AtlasProblem;
					FTextureRHIRef AtlasTexture;
					const uint32 Descriptor = Resource->DensityAtlas.EnsureAndGetDescriptor(RHICmdList, DensityUpload, AtlasProblem, &AtlasTexture);
					const bool bDescriptor = Descriptor != MAX_uint32 && Descriptor < (1u << 24);
					// Overlay consumers need the heap index (row 7.z); producers need only the uploaded texture.
					// With a packet (BindlessAll) both are required, exactly as before; injection-only needs the texture.
					if (AtlasTexture.IsValid() && (bDescriptor || !Resource->HasPacket()))
					{
						if (bDescriptor) Packet.Rows[7].Z = static_cast<float>(Descriptor);
						Resource->DensityAtlasTexture = AtlasTexture;
					}
					else
					{
						Resource->DensityAtlasTexture.SafeRelease();
						if (AtlasProblem.IsEmpty() && AtlasTexture.IsValid() && Descriptor == MAX_uint32)
							AtlasProblem = TEXT("Density atlas has no bindless descriptor; the Box overlay requires -BindlessAll.");
						Packet.Rows[6].W = 0.0f;
						Packet.Rows[13].W = 0.0f;
						Packet.Rows[7] = FVector4f(0, 0, 0, 0);
						Packet.Rows[10].Z = Packet.Rows[10].W = Packet.Rows[11].X = 0;
						if (FogMS_UsesWorldProducer(Packet.Rows[5].Y))
						{
							const bool bTransport = FogMS_IsTransportMode(static_cast<EFogMSScatteringMode>(Packet.Rows[5].Y));
							Packet.Rows[5].Y = 0;
							Packet.Rows[22] = FVector4f(0, 0, 0, 0);
							FScopeLock Lock(&Resource->FieldStatusMutex);
							Resource->FieldStatusRevision = Revision;
							Resource->SpatialFieldStatus = bTransport ? TEXT("Transport unavailable: density atlas GPU upload failed.")
								: TEXT("World unavailable: density atlas GPU upload failed.");
						}
						Packet.Rows[5].X = 1; // Never retain stale attenuation after a failed resource update.
						if (AtlasProblem.IsEmpty()) AtlasProblem = TEXT("Atlas descriptor cannot be represented exactly by the packet.");
					}
					Resource->RecordAtlasStatus(DensityUpload->Revision, AtlasProblem);
					if (AtlasProblem != Resource->LastAtlasProblem)
					{
						if (!AtlasProblem.IsEmpty()) UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS density atlas: %s"), *AtlasProblem);
						Resource->LastAtlasProblem = AtlasProblem;
					}
				}
				else Resource->DensityAtlasTexture.SafeRelease(); // No density source: producers are not requested.
				Resource->Upload(RHICmdList, Snapshot);
				Resource->RenderSnapshot = Snapshot;
				if (!FogMS_UsesWorldProducer(Packet.Rows[5].Y)) Resource->bWorldFieldPublished = false;
				Resource->DirectionToSun = DirectionToSun;
				Resource->Revision = Revision;
				Resource->DensityAtlasRevision = DensityUpload.IsValid() ? DensityUpload->Revision : 0;
				if (bFence) RHICmdList.Transition(TArrayView<const FRHITransitionInfo>(), ERHIPipeline::Graphics, ERHIPipeline::AsyncCompute);
			});
		}
		virtual ESceneViewExtensionFlags GetFlags() const override
		{
			return ESceneViewExtensionFlags::SubscribesToPostTLASBuild | ESceneViewExtensionFlags::RequiresHardwareInlineRayTracing;
		}

		virtual void PostTLASBuild_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& View) override
		{
			FBoxRenderSnapshot& Snapshot = GPU->RenderSnapshot;
			FBoxPacket& Packet = Snapshot.Packet;
			Packet.Rows[22] = FVector4f(0, 0, 0, 0);
			const bool bTransport = FogMS_IsTransportMode(static_cast<EFogMSScatteringMode>(Packet.Rows[5].Y));
			const bool bWorldLighting = FogMS_UsesWorldProducer(Packet.Rows[5].Y);
			// Row 23.w == 5 (full field) or 6 (hybrid): the Box Volume MID reads the field from InjectionTexture; the
			// overlay reads nothing (its consumers require exactly 4). Hybrid publishes J_ms and T_sun instead of J.
			const bool bInjection = bTransport && (Packet.Rows[23].W == 5.0f || Packet.Rows[23].W == 6.0f) && GPU->InjectionTexture.IsValid();
			const bool bHybrid = bInjection && Packet.Rows[23].W == 6.0f;
			// Async solver (r.FogMS.Transport.AsyncCompute): publish one frame late. Every graphics consumer of the solve
			// (atlas/field copies, row 22 upload) moves to PrePostProcessPass, after ComputeVolumetricFog, so RDG joins the
			// async queue there and the solve overlaps lights/Lumen/fog. This frame's fog reads last frame's publication.
			const bool bLate = bTransport && Packet.Rows[0].W >= 0.5f && FogMS_TransportPublishesLate(GraphBuilder);
			GPU->bLatePublishQueued = false;
			GPU->bInjectionFieldConsumed = false; // Set at the end only when this frame's fog samples a current J.
			// Same-frame: every fallback clears J. Late: only the view that owns the late J (LateInjectionViewKey);
			// a capture/planar/other family rendered between two of its frames must not erase J before its next fog.
			const bool bMayClearInjection = !FogMS_TransportPublishesLate(GraphBuilder) || View.GetViewKey() == GPU->LateInjectionViewKey;
			if (!bWorldLighting || Packet.Rows[0].W < 0.5f) FogMS_InvalidateWorldLighting_RenderThread();
			if (Packet.Rows[0].W < 0.5f || (!bWorldLighting && (Packet.Rows[5].Y != 2 || Packet.Rows[21].X <= 0)))
			{
				if (bMayClearInjection) ClearInjectionField(GraphBuilder);
				return;
			}
			if (bLate && GFrameNumberRenderThread - GPU->LateInjectionRenderFrame > 1u)
			{
				// No late copy in the previous frame (PrePostProcessPass skipped, or a gap): the volume holds older J.
				// Clear on graphics before the solver forks and before this frame's fog samples it (fail closed).
				ClearInjectionField(GraphBuilder);
			}
			FFogMSSpatialRequest Request;
			Request.CenterWS = FVector(Packet.Rows[0]) + FVector(Packet.Rows[1]);
			Request.AxisX = FVector3f(Packet.Rows[2]); Request.AxisY = FVector3f(Packet.Rows[3]); Request.AxisZ = FVector3f(Packet.Rows[4]);
			Request.Extent = FVector3f(Packet.Rows[2].W, Packet.Rows[3].W, Packet.Rows[4].W);
			Request.Revision = GPU->Revision;
			Request.RangeCm = Packet.Rows[21].Y;
			Request.Steps = 12;
			Request.Directions = bTransport ? static_cast<int32>(Packet.Rows[21].W) : 12;
			Request.ResetHistory = Packet.Rows[5].X > 0.5f;
			Request.PhaseG = 0;
			FFogMSSpatialResult Result;
			if (bWorldLighting)
			{
				FFogMSWorldRequest WorldRequest;
				static_cast<FFogMSSpatialRequest&>(WorldRequest) = Request;
				FMemory::Memcpy(WorldRequest.BoxRows, Packet.Rows, sizeof(Packet.Rows));
				WorldRequest.Strength = Packet.Rows[21].X;
				WorldRequest.bTransport = bTransport;
				WorldRequest.DirectionToSun = GPU->DirectionToSun;
				if (bTransport)
				{
					WorldRequest.Iterations = static_cast<int32>(Packet.Rows[21].Z);
					WorldRequest.Tolerance = Packet.Rows[16].W;
				}
				if (bInjection) WorldRequest.InjectionTexture = GPU->InjectionTexture;
				WorldRequest.bHybridInjection = bHybrid; // Implies bInjection: the request validation needs the field.
				// Uploaded by this packet's FogMS_UpdateBox command (same family, earlier on the render thread);
				// pass 0 of the producer binds it directly. Null fails the request (native fallback below).
				WorldRequest.DensityAtlas = GPU->DensityAtlasTexture;
				WorldRequest.bLatePublish = bLate;
				if (bLate)
				{
					// This frame's fog runs before this graph's late copy: upload last frame's pair now, on graphics BEFORE
					// the solver forks, so this packet rewrite (and its manual fences) never waits on the async solve.
					// No pair (first late frame, skipped copy, gap, Box moved): the packet of a failed same-frame solve.
					uint32 LateDescriptor = MAX_uint32;
					int32 LateGrid = 0;
					const bool bConsumed = FogMS_GetLateTransportField(View, Packet.Rows, LateDescriptor, LateGrid)
						&& LateDescriptor < (1u << 24) && LateGrid > 1;
					// Stays in the snapshot: PrePostProcessPass (SSFS) sees what this frame's fog consumed.
					Packet.Rows[22] = bConsumed ? FVector4f(static_cast<float>(LateDescriptor), static_cast<float>(LateGrid), 1, 4.0f) : FVector4f(0, 0, 0, 0);
					FBoxRenderSnapshot Consumer = Snapshot;
					if (!bConsumed) Consumer.Packet.Rows[5].Y = 0;
					// Native fog history: reset once when what fog consumes switches (the same-frame rule, moved to the consumer).
					if (GPU->bWorldFieldPublished != bConsumed) Consumer.Packet.Rows[5].X = 1;
					GPU->bWorldFieldPublished = bConsumed;
					PublishFields(GraphBuilder, Consumer);
				}
				// Strength=0 still produces current primary indirect lighting. World
				// applies per-order strength in its atlas; the consumer must not repeat it.
				Result = FogMS_BuildWorldLighting(GraphBuilder, View, WorldRequest);
			}
			else
				Result = FogMS_BuildSpatial(GraphBuilder, View, Request);
			// With a packet (BindlessAll) publication means a resident descriptor for the overlay, as before. Injection-only
			// has no descriptor: a valid Transport build that wrote (or queued) the injection field is the publication.
			const bool bDescriptorPublished = Result.DescriptorIndex < (1u << 24);
			const bool bPublished = Result.Valid && Result.GridSize > 1 && (bDescriptorPublished || (!GPU->HasPacket() && bInjection));
			if (bWorldLighting && !bLate)
			{
				// Native fog still filters the final image temporally. Invalidate it once
				// when switching between current World lighting and native fallback.
				if (GPU->bWorldFieldPublished != bPublished) Packet.Rows[5].X = 1;
				GPU->bWorldFieldPublished = bPublished;
			}
			if (bPublished && bLate)
			{
				// Late: row 22 keeps the consumed pair until PrePostProcessPass has added the copy it describes.
				// Injection-only: no descriptor, so no row 22 pair; the late copy still publishes the injection field.
				GPU->LateField = bDescriptorPublished ? FVector4f(static_cast<float>(Result.DescriptorIndex), static_cast<float>(Result.GridSize), 1, 4.0f)
					: FVector4f(0, 0, 0, 0);
				GPU->bLatePublishQueued = true;
			}
			else if (bPublished)
			{
				// Row 22 describes a resident descriptor; the injection-only runtime has none (and no packet reader).
				if (bDescriptorPublished)
					Packet.Rows[22] = FVector4f(static_cast<float>(Result.DescriptorIndex), static_cast<float>(Result.GridSize), 1,
						bTransport ? 4.0f : (bWorldLighting ? 3.0f : 0.0f));
			}
			else if (bWorldLighting)
				Packet.Rows[5].Y = 0; // No replacement GI may consume an invalid World atlas.
			// The producer wrote the field in this graph only for a published injection request.
			if (bInjection && bPublished)
			{
				// Late: nothing is written yet; PrePostProcessPass sets it when its copy is actually added.
				if (!bLate) GPU->bInjectionFieldWritten = true;
			}
			else if (bMayClearInjection) ClearInjectionField(GraphBuilder);
			FString ToleranceText;
			if (bTransport && bPublished)
			{
				// Report what the solver ran, resolved as in FogMS_RenderTransport: row 16.w >= 0 is the
				// Box tolerance, negative falls back to r.FogMS.Transport.Tolerance; both clamped to [0,1].
				static TConsoleVariableData<float>* const ToleranceCVar = IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.FogMS.Transport.Tolerance"));
				if (Packet.Rows[16].W >= 0.0f) ToleranceText = FString::Printf(TEXT("tol %.2e"), FMath::Clamp(Packet.Rows[16].W, 0.0f, 1.0f));
				else if (ToleranceCVar) ToleranceText = FString::Printf(TEXT("tol %.2e from cvar"), FMath::Clamp(ToleranceCVar->GetValueOnRenderThread(), 0.0f, 1.0f));
				else ToleranceText = TEXT("tol from cvar");
			}
			{
				FScopeLock Lock(&GPU->FieldStatusMutex);
				GPU->FieldStatusRevision = GPU->Revision;
				if (bPublished)
					GPU->SpatialFieldStatus = bTransport
						? FString::Printf(TEXT("Active %s isotropic transport (%d directions, %s%s%s%s; see convergence diagnostics)%s"), Request.Directions == 6 ? TEXT("B2") : TEXT("B3"), Request.Directions,
							*ToleranceText, bInjection ? TEXT("; emissive injection via material") : TEXT(""),
							bHybrid ? TEXT("; hybrid: native single scattering") : TEXT(""), bLate ? TEXT("; one frame late") : TEXT(""),
							GPU->HasPacket() ? TEXT("") : TEXT(" (injection-only: no BindlessAll; overlay features off)"))
						: (bWorldLighting
							? (Packet.Rows[21].X > 0 ? TEXT("Active World primary + three current-frame scattering orders (no fog history)")
								: TEXT("Active World primary (Strength=0: extra orders disabled; no fog history)"))
							: TEXT("Active experimental spatial transfer (history sources, HWRT visibility)"));
				else
				{
					const FString Reason = Result.Error.IsEmpty() ? TEXT("Invalid output atlas or descriptor.") : Result.Error;
					GPU->SpatialFieldStatus = bTransport ? (bInjection
							? TEXT("Transport unavailable; native lighting with authored density (emissive injection field cleared): ")
							: TEXT("Transport unavailable; native lighting with authored density: ")) + Reason
						: (bWorldLighting ? TEXT("World unavailable; native lighting: ") + Reason : Reason);
				}
			}
			// UE 5.8 calls PostTLAS after the base-pass extension, before deferred
			// lighting and volumetric fog. Publish the new spatial descriptor here.
			// Late: already uploaded before the solver; the new pair goes up with its copy in PrePostProcessPass.
			if (!bLate) PublishFields(GraphBuilder, Snapshot);
			// Injection-only SSFS trigger: the volume this frame's fog samples holds a current J (same-frame: written
			// above; late: last frame's copy, not cleared by the gap/failure paths above).
			GPU->bInjectionFieldConsumed = !GPU->HasPacket() && bInjection && GPU->bInjectionFieldWritten;
		}

		virtual void PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& View,
			const FRenderTargetBindingSlots& RenderTargets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override
		{
			FBoxRenderSnapshot Snapshot = GPU->RenderSnapshot;
			FBoxPacket& Packet = Snapshot.Packet;
			Snapshot.bProceduralSunForFSSS = FogMS_ScreenScatteringSun.GetValueOnRenderThread() != 0
				&& View.Family->EngineShowFlags.Atmosphere && View.Family->EngineShowFlags.DeferredAtmospherePass
				&& !View.bIsSceneCapture && !View.bIsReflectionCapture && !View.bIsPlanarReflection
				&& View.Family->Views.Num() == 1;
			Snapshot.ViewIntegrationMode = FMath::Clamp(FogMS_ViewIntegration.GetValueOnRenderThread(), 0, 3);
			if (GPU->LastViewIntegrationMode != Snapshot.ViewIntegrationMode) Packet.Rows[5].X = 1;
			GPU->LastViewIntegrationMode = Snapshot.ViewIntegrationMode;
			Packet.Rows[20] = FVector4f(0, 0, 0, 0);
			// The shadow cache is an overlay feature (heap-read density atlas, BindlessAll-only shaders). Row 16.X is
			// forced to 0 without BindlessAll; HasPacket() keeps BuildShadow unreachable in that runtime regardless.
			if (GPU->HasPacket() && Packet.Rows[0].W > 0.5f && Packet.Rows[16].X > 0 && Packet.Rows[7].W > 0)
			{
				if (!GPU->ShadowCache.IsValid()) GPU->ShadowCache = FogMSRender::CreateShadowCacheState();
				FogMSRender::FShadowCacheResult Result;
				const bool bReady = FogMSRender::BuildShadow(GraphBuilder, View, GPU->ShadowCache, Packet.Rows,
					GPU->DirectionToSun, Packet.Rows[16].Y, Result, GPU->DensityAtlasRevision);
				if (bReady && Result.DescriptorIndex < (1u << 24))
				{
					Packet.Rows[17] = FVector4f(Result.AxisX, Result.HalfExtent.X);
					Packet.Rows[18] = FVector4f(Result.AxisY, Result.HalfExtent.Y);
					Packet.Rows[19] = FVector4f(Result.AxisZ, Result.HalfExtent.Z);
					Packet.Rows[20] = FVector4f(static_cast<float>(Result.DescriptorIndex), static_cast<float>(Result.GridSize.X), static_cast<float>(Result.GridSize.Z), 1);
				}
				FScopeLock Lock(&GPU->FieldStatusMutex);
				GPU->FieldStatusRevision = GPU->Revision;
				GPU->ShadowFieldStatus = bReady ? TEXT("Active filtered light-space transmittance") : TEXT("Reference march fallback: ") + Result.Error;
			}
			if (View.bIsSceneCapture || View.bIsReflectionCapture || View.Family->Views.Num() != 1)
			{
				Packet.Rows[22] = FVector4f(0, 0, 0, 0);
				if (FogMS_UsesWorldProducer(Packet.Rows[5].Y)) Packet.Rows[5].Y = 0;
			}
			// Retain shadow metadata for the later PostTLAS spatial publication.
			GPU->RenderSnapshot = Snapshot;
			PublishFields(GraphBuilder, Snapshot);
		}
		virtual void PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
			const FPostProcessingInputs& Inputs) override
		{
			const FBoxPacket& Packet = GPU->RenderSnapshot.Packet;
			// Screen scattering is a post filter, not density/source injection: keep it for both
			// Transport deliveries (4 overlay, 5 emissive injection, 6 hybrid injection) with a published field.
			// BindlessAll: the field this frame's fog consumed is row 22 (unchanged). Injection-only has no row 22:
			// PostTLAS records whether the injection volume sampled by this frame's fog holds a current J.
			const bool bFieldConsumed = GPU->HasPacket() ? (Packet.Rows[22].Z >= .5f && Packet.Rows[22].W == 4.f)
				: GPU->bInjectionFieldConsumed;
			if (Packet.Rows[0].W > .5f && (Packet.Rows[23].W == 4.f || Packet.Rows[23].W == 5.f || Packet.Rows[23].W == 6.f)
				&& bFieldConsumed
				&& !View.bIsSceneCapture && !View.bIsReflectionCapture && !View.bIsPlanarReflection
				&& View.Family && View.Family->Views.Num() == 1)
			{
				FogMS_AddScreenScattering(GraphBuilder, View, Inputs);
			}
			if (GPU->bLatePublishQueued)
			{
				// One-frame-late publication, graphics queue, after ComputeVolumetricFog (and SSFS): the copies are the
				// first graphics consumers of the async solve, so RDG joins here. Row 22 and the volume change only
				// together with their copies; the fences in PublishFields bracket the packet rewrite on this list.
				// The blackboard entry proves the solve belongs to THIS graph; otherwise nothing is written.
				GPU->bLatePublishQueued = false;
				bool bInjectionCopied = false;
				if (FogMS_PublishWorldLightingLate(GraphBuilder, View, bInjectionCopied))
				{
					if (bInjectionCopied)
					{
						GPU->bInjectionFieldWritten = true;
						GPU->LateInjectionRenderFrame = GFrameNumberRenderThread;
						GPU->LateInjectionViewKey = View.GetViewKey();
					}
					GPU->RenderSnapshot.Packet.Rows[22] = GPU->LateField;
					PublishFields(GraphBuilder, GPU->RenderSnapshot);
				}
			}
		}
	private:
		void ClearInjectionField(FRDGBuilder& GraphBuilder)
		{
			// Fail closed: a frame that does not publish J must not leave the previous J lit.
			// Alpha 0 makes the material fall back to native albedo lighting, matching the
			// overlay's Transport fallback (authored density, native lighting).
			FRHITexture* Texture = GPU->InjectionTexture.GetReference();
			if (!Texture || !GPU->bInjectionFieldWritten || !EnumHasAnyFlags(Texture->GetDesc().Flags, TexCreate_UAV)) return;
			FRDGTextureRef Field = RegisterExternalTexture(GraphBuilder, Texture, TEXT("FogMS.TransportField"));
			GraphBuilder.UseInternalAccessMode(Field);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Field), FLinearColor::Transparent);
			// The Volume material binding is invisible to RDG; hand the field back as SRV.
			GraphBuilder.UseExternalAccessMode(Field, ERHIAccess::SRVMask, ERHIPipeline::Graphics);
			GPU->bInjectionFieldWritten = false;
		}

		void PublishFields(FRDGBuilder& GraphBuilder, const FBoxRenderSnapshot& Snapshot) const
		{
			// Consumers use a stable resident descriptor. Each publication follows its
			// producer's external-SRV transition in the graphics graph. Capture both
			// rows together: a later family/world must not replace only the prior phase.
			// Injection-only: no packet texture and no hidden reader, so neither the upload nor its async<->graphics
			// fences are needed; the injection volume's hand-off is RDG's UseExternalAccessMode in the producer.
			if (!GPU->HasPacket()) return;
			GraphBuilder.AddPass(RDG_EVENT_NAME("FogMS PublishFields"), ERDGPassFlags::NeverCull,
				[Resource = GPU, Snapshot](FRHICommandList& RHICmdList)
				{
					// Bindless readers are invisible to RDG, including forced async passes.
					RHICmdList.Transition(TArrayView<const FRHITransitionInfo>(), ERHIPipeline::AsyncCompute, ERHIPipeline::Graphics);
					Resource->Upload(RHICmdList, Snapshot);
					RHICmdList.Transition(TArrayView<const FRHITransitionInfo>(), ERHIPipeline::Graphics, ERHIPipeline::AsyncCompute);
				});
		}
		TSharedRef<FBoxGPUState, ESPMode::ThreadSafe> GPU;
		TMap<TWeakObjectPtr<UWorld>, FWorldPacketState> Worlds;
	};

	TSharedPtr<FBoxGPUState, ESPMode::ThreadSafe> GPUState;
	TSharedPtr<FBoxViewExtension, ESPMode::ThreadSafe> Extension;
}

int32 FFogMSBoxRuntime::GetMode()
{
	return FogMS_BoxMode.GetValueOnGameThread();
}

bool FFogMSBoxRuntime::Prepare(uint32& OutDescriptorIndex, FString& OutError)
{
	if (!GDynamicRHI || FCString::Strcmp(GDynamicRHI->GetName(), TEXT("D3D12")) != 0
		|| GMaxRHIShaderPlatform != SP_PCD3D_SM6 || GNumExplicitGPUsForRendering != 1
		|| !FShaderPlatformConfig::IsValid(GMaxRHIShaderPlatform))
	{
		OutError = TEXT("Live FogMS Box requires single-GPU D3D12/SM6. Global A1 remains available without it.");
		return false;
	}
	if (!FogMS_IsBindlessAll())
	{
		// Injection-only runtime: register the view extension without a packet texture or descriptor. Its only
		// output path is Transport -> Emissive Injection volume -> Box Volume material -> native fog (no patch).
		// GRHISupportsInlineRayTracing already implies bindless is not Disabled (D3D12Adapter).
		if (!GRHISupportsInlineRayTracing)
		{
			OutError = TEXT("FogMS Box transport requires inline hardware ray tracing (D3D12 SM6, ray tracing enabled). Global A1 remains available.");
			return false;
		}
		if (!GPUState.IsValid()) GPUState = MakeShared<FBoxGPUState, ESPMode::ThreadSafe>(); // HasPacket() == false
		if (!Extension.IsValid()) Extension = FSceneViewExtensions::NewExtension<FBoxViewExtension>(GPUState.ToSharedRef());
		// Returns true (runtime ready) but still reports an error for the overlay: FFogMSShaderPatcher::ReadConfig
		// checks only OutError, so a BoxMode 1 Apply fails closed and no Box overlay is ever built with this index.
		OutDescriptorIndex = MAX_uint32;
		OutError = TEXT("The live Box overlay (BoxMode 1) requires -BindlessAll. Injection-only runtime active: Transport + Emissive Injection through the Box Volume material; overlay features off.");
		return true;
	}
	if (!GPUState.IsValid())
	{
		GPUState = MakeShared<FBoxGPUState, ESPMode::ThreadSafe>();
		ENQUEUE_RENDER_COMMAND(FogMS_CreateBoxTexture)([Resource = GPUState](FRHICommandListImmediate& RHICmdList)
		{
#if PLATFORM_WINDOWS
			// A literal heap descriptor is invisible to native Fog FParameters/residency gathering.
			// Create a resident committed allocation (no CREATE_NOT_RESIDENT); the public external
			// wrapper opts out of Engine eviction. Keep it alive for the entire overlay lifetime.
			ID3D12DynamicRHI* D3D12 = GetID3D12DynamicRHI();
			D3D12_HEAP_PROPERTIES Heap{};
			Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			Heap.CreationNodeMask = Heap.VisibleNodeMask = D3D12->RHIGetDeviceNodeMask(0);
			D3D12_RESOURCE_DESC Desc{};
			Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			Desc.Width = FogMS_BoxPacketRowCount;
			Desc.Height = 2;
			Desc.DepthOrArraySize = Desc.MipLevels = Desc.SampleDesc.Count = 1;
			Desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			const HRESULT Result = D3D12->RHIGetDevice(0)->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				nullptr, IID_PPV_ARGS(Resource->NativeTexture.GetInitReference()));
			if (FAILED(Result))
			{
				UE_LOG(LogMultiLobeSpec, Error, TEXT("FogMS Box resident texture allocation failed: 0x%08x"), static_cast<uint32>(Result));
				return;
			}
			Resource->Texture = D3D12->RHICreateTexture2DFromResource(PF_A32B32G32R32F,
				ETextureCreateFlags::ShaderResource | ETextureCreateFlags::External, FClearValueBinding::None, Resource->NativeTexture);
			Resource->SRV = RHICmdList.CreateShaderResourceView(Resource->Texture,
				FRHIViewDesc::CreateTextureSRV().SetDimensionFromTexture(Resource->Texture));
			Resource->Upload(RHICmdList, FBoxRenderSnapshot{});
			const FRHIDescriptorHandle Handle = Resource->SRV->GetBindlessHandle();
			if (Handle.IsValid()) Resource->DescriptorIndex = Handle.GetIndex();
#endif
		});
		FlushRenderingCommands();
	}
	if (GPUState->DescriptorIndex == MAX_uint32)
	{
		OutError = TEXT("FogMS Box could not allocate a valid bindless SRV. Previous overlay remains active.");
		return false;
	}
	if (!Extension.IsValid()) Extension = FSceneViewExtensions::NewExtension<FBoxViewExtension>(GPUState.ToSharedRef());
	OutDescriptorIndex = GPUState->DescriptorIndex;
	return true;
}

void FFogMSBoxRuntime::Shutdown()
{
	Extension.Reset();
	ENQUEUE_RENDER_COMMAND(FogMS_ReleaseSpatial)([](FRHICommandListImmediate& RHICmdList)
	{
		FogMS_ShutdownWorldLighting_RenderThread(RHICmdList);
		FogMS_ShutdownSpatial_RenderThread(RHICmdList);
	});
	if (GPUState.IsValid())
	{
		FlushRenderingCommands();
		GPUState.Reset();
	}
	bIndirectPreviewEnabled = false;
	PreviewPreviousValues.Empty(); // Session ends; never rebuild shaders during module shutdown.
}

bool FFogMSBoxRuntime::IsIndirectPreviewEnabled()
{
	return bIndirectPreviewEnabled;
}

void FFogMSBoxRuntime::ConfigureIndirectPreview(bool bEnable)
{
	if (bEnable)
	{
		for (const FPreviewSetting& Setting : PreviewSettings)
		{
			IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Setting.Name);
			if (!Variable) continue;
			if (!PreviewPreviousValues.Contains(Setting.Name)) PreviewPreviousValues.Add(Setting.Name, Variable->GetString());
			Variable->Set(Setting.Value, ECVF_SetByConsole);
		}
		bIndirectPreviewEnabled = true;
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS indirect preview: Lumen ray tracing enabled with fixed 8x8 angular sampling; radiance cache, spatial filter, depth offset, Lumen async compute and ray-tracing instance culling disabled for this session. Use Restore Standard Lumen to restore previous values."));
	}
	else if (!PreviewPreviousValues.IsEmpty())
	{
		bIndirectPreviewEnabled = false;
		if (!FMultiLobeSpecModule::Get().ApplyFromSettings())
		{
			bIndirectPreviewEnabled = true;
			UE_LOG(LogMultiLobeSpec, Error, TEXT("FogMS restore failed: previous overlay and preview renderer settings retained. Fix the Apply error and retry Restore Standard Lumen."));
			return;
		}
		if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
		ENQUEUE_RENDER_COMMAND(FogMS_PreviewRestoreDrain)([](FRHICommandListImmediate& RHICmdList)
		{
			RHICmdList.SubmitAndBlockUntilGPUIdle();
		});
		FlushRenderingCommands();
		FogMS_RestorePreviewSettings();
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS indirect preview renderer settings restored (subsequent manual CVar edits preserved)."));
	}
}
