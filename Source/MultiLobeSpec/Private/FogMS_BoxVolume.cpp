#include "FogMS_BoxVolume.h"
#include "FogMS_BoxRuntime.h"
#include "FogMS_CloudHost.h"
#include "FogMS_Weather.h"

#include "Components/BoxComponent.h"
#include "Components/ArrowComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "Engine/DirectionalLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTargetVolume.h"
#include "Engine/VolumeTexture.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "MultiLobeSpec.h"
#include "Serialization/CustomVersion.h"
#include "Serialization/Archive.h"
#include "ShaderPlatformConfig.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	const FGuid FogMS_MotionVersionGuid(0xD9F49C37, 0xBE184764, 0xA270B431, 0x3E0D958A);
	constexpr int32 FogMS_DirectionalMotionVersion = 1;
	FCustomVersionRegistration FogMS_MotionVersion(FogMS_MotionVersionGuid, FogMS_DirectionalMotionVersion, TEXT("FogMSDirectionalMotion"));
	// Must equal the transport grid (TransportGridSize in FogMS_Transport.cpp, WorldSize in
	// FogMS_WorldLighting.cpp); the producer rejects any other field size.
	constexpr int32 FogMS_TransportFieldSize = 32;

	// W41 Sun Detail Shadow map size (read on the game thread when the Box creates its map; a change re-creates it).
	TAutoConsoleVariable<int32> CVarSunMapResolution(TEXT("r.FogMS.SunMap.Resolution"), 256,
		TEXT("Sun Detail Shadow: texels of each Box's sun-space map across the sun rays (both lateral axes), rounded up to a multiple of 8, ")
		TEXT("clamped to [64, 512]. Default 256 (about 1 m per texel on a 200 m Box). Memory per Box: Resolution^2 x Steps x 4 bytes + 256 KB ")
		TEXT("(256 x 256 x 64: 16.25 MB). The map is rebuilt every frame; cost grows with Resolution^2 x Steps."), ECVF_Default);
	TAutoConsoleVariable<int32> CVarSunMapSteps(TEXT("r.FogMS.SunMap.Steps"), 64,
		TEXT("Sun Detail Shadow: density samples along each sun ray through the Box (the map's depth slices), rounded up to a multiple of 8, ")
		TEXT("clamped to [16, 256]. Default 64. The ray covers the Box (or its height-profile band) along the Box axis the sun crosses fastest."),
		ECVF_Default);

	struct FFogMSTransportTier
	{
		EFogMSAngularQuality AngularQuality;
		int32 Iterations;
		float Tolerance;

		bool Matches(EFogMSAngularQuality Quality, int32 InIterations, float InTolerance) const
		{
			return AngularQuality == Quality && Iterations == InIterations && Tolerance == InTolerance;
		}
	};

	/** Measured production tiers (frozen scene, warm start). False for Custom. */
	bool FogMS_GetTransportTier(EFogMSTransportPreset Preset, FFogMSTransportTier& OutTier)
	{
		switch (Preset)
		{
		case EFogMSTransportPreset::Production: OutTier = { EFogMSAngularQuality::Low16, 16, 1.0e-6f }; return true;
		case EFogMSTransportPreset::High: OutTier = { EFogMSAngularQuality::Balanced48, 16, 1.0e-8f }; return true;
		case EFogMSTransportPreset::Cinematic: OutTier = { EFogMSAngularQuality::High96, 64, 1.0e-14f }; return true;
		default: return false;
		}
	}

	bool FogMS_IsFinitePositiveVector(const FVector& Value)
	{
		return !Value.ContainsNaN() && Value.GetMin() > 0.0;
	}

	bool FogMS_IsFiniteColor(const FLinearColor& Value)
	{
		return FMath::IsFinite(Value.R) && FMath::IsFinite(Value.G)
			&& FMath::IsFinite(Value.B) && FMath::IsFinite(Value.A);
	}

	bool FogMS_MakeWorldPhase(const FVector& Center, float Frequency, FLinearColor& OutPhase)
	{
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			// Match the shader's float frequency, but retain the world center's double precision.
			const double Cycles = Center[Axis] * static_cast<double>(Frequency);
			if (!FMath::IsFinite(Cycles)) return false;
			const float Phase = static_cast<float>(Cycles - FMath::FloorToDouble(Cycles));
			// A double fraction just below one can round to 1.0f; wrap it back into [0,1).
			OutPhase.Component(Axis) = Phase == 1.0f ? 0.0f : Phase;
		}
		OutPhase.A = 0.0f;
		return FogMS_IsFiniteColor(OutPhase);
	}

	bool FogMS_DisplacedWorldPhase(const FVector& Center, const FVector& Displacement, const FVector& Offset,
		float Frequency, FLinearColor& OutPhase)
	{
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			// Reduce independently before subtracting: world position, user offset and
			// travelled distance remain doubles until the bounded final phase is stored.
			const double CenterCycles = Center[Axis] * static_cast<double>(Frequency);
			const double TravelCycles = Displacement[Axis] * static_cast<double>(Frequency);
			const double OffsetCycles = Offset[Axis] * static_cast<double>(Frequency);
			if (!FMath::IsFinite(CenterCycles) || !FMath::IsFinite(TravelCycles) || !FMath::IsFinite(OffsetCycles)) return false;
			const double Cycles = (CenterCycles - FMath::FloorToDouble(CenterCycles))
				- (TravelCycles - FMath::FloorToDouble(TravelCycles))
				- (OffsetCycles - FMath::FloorToDouble(OffsetCycles));
			const float Phase = static_cast<float>(Cycles - FMath::FloorToDouble(Cycles));
			OutPhase.Component(Axis) = Phase == 1.0f ? 0.0f : Phase;
		}
		OutPhase.A = 0.0f;
		return FogMS_IsFiniteColor(OutPhase);
	}

	bool FogMS_MakeWorldMapping(const FVector& Center, const FVector& Offset, float WorldTextureSize, float DetailScale,
		FLinearColor& OutFrequencies, FLinearColor& OutPhase0, FLinearColor& OutPhase1, FLinearColor& OutPhase2)
	{
		if (!FMath::IsFinite(WorldTextureSize) || WorldTextureSize < 1.0f || WorldTextureSize > 1.0e8f) return false;
		const float F0 = 1.0f / WorldTextureSize;
		const float F1 = F0 * DetailScale;
		const float F2 = F1 * 2.0f;
		OutFrequencies = FLinearColor(F0, F1, F2, 0.0f);
		if (!FogMS_IsFiniteColor(OutFrequencies) || FMath::Min3(F0, F1, F2) <= 0.0f || Offset.ContainsNaN()) return false;
		// Preserve the existing zero-offset static path exactly.
		if (Offset == FVector::ZeroVector)
			return FogMS_MakeWorldPhase(Center, F0, OutPhase0)
				&& FogMS_MakeWorldPhase(Center, F1, OutPhase1)
				&& FogMS_MakeWorldPhase(Center, F2, OutPhase2);
		return FogMS_DisplacedWorldPhase(Center, FVector::ZeroVector, Offset, F0, OutPhase0)
			&& FogMS_DisplacedWorldPhase(Center, FVector::ZeroVector, Offset, F1, OutPhase1)
			&& FogMS_DisplacedWorldPhase(Center, FVector::ZeroVector, Offset, F2, OutPhase2);
	}

	double FogMS_GetDensityFrameTime(const UWorld* World)
	{
		// Tick, editor property changes and multiple view families may all call UpdateDensity.
		// Snapshot on the game thread once per world/frame so every consumer sees one clock.
		struct FFrameTime { uint64 Frame = MAX_uint64; double Seconds = 0.0; };
		static TMap<TWeakObjectPtr<const UWorld>, FFrameTime> FrameTimes;
		for (auto It = FrameTimes.CreateIterator(); It; ++It)
		{
			if (!It.Key().IsValid()) It.RemoveCurrent();
		}
		if (!World) return 0.0;
		FFrameTime& Time = FrameTimes.FindOrAdd(World);
		if (Time.Frame != GFrameCounter)
		{
			Time.Frame = GFrameCounter;
			Time.Seconds = World->GetTimeSeconds();
		}
		return Time.Seconds;
	}

	bool FogMS_IsDensityActorVisible(const AActor& Actor, const UWorld* World)
	{
#if WITH_EDITOR
		if (World && !World->UsesGameHiddenFlags()) return !Actor.IsHiddenEd();
#endif
		return World && !Actor.IsHidden();
	}

	// Same predicate as BoxRuntime: the engine-shader overlay (r.FogMS.BoxMode 1) needs BindlessAll. Without it
	// only the injection-only runtime exists (Transport -> Emissive Injection volume -> this Box's Volume material).
	// The overlay is built by the editor-only shader patcher, so outside the editor (cooked game, -game) this is
	// always false and the Box is injection-only even when the RHI runs BindlessAll.
	bool FogMS_IsBindlessAllConfiguration()
	{
#if WITH_EDITOR
		return GIsEditor && FShaderPlatformConfig::IsValid(GMaxRHIShaderPlatform)
			&& FShaderPlatformConfig::GetBindlessConfiguration(GMaxRHIShaderPlatform) == ERHIBindlessConfiguration::All;
#else
		return false;
#endif
	}

	void FogMS_ApplyBoxMode(const int32 BoxMode)
	{
#if WITH_EDITOR
		if (GIsEditor)
		{
			IConsoleVariable* EnableVariable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.FogMS.Enable"));
			IConsoleVariable* BoxModeVariable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.FogMS.BoxMode"));
			if (!EnableVariable || !BoxModeVariable)
			{
				UE_LOG(LogMultiLobeSpec, Error, TEXT("FogMS Box Volume: r.FogMS.Enable or r.FogMS.BoxMode is unavailable; the mode was not applied."));
				return;
			}

			EnableVariable->Set(1, ECVF_SetByConsole);
			BoxModeVariable->Set(BoxMode, ECVF_SetByConsole);
			FMultiLobeSpecModule::Get().ApplyFromSettings();
			return;
		}
#endif
		UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS Box Volume: r.FogMS.BoxMode %d needs the editor-only engine-shader overlay; not applied outside the editor."), BoxMode);
	}

	// bApplyRequiredRenderSettings (game BeginPlay, and every automatic runtime start incl. the editor world and PIE/Simulate):
	// the two renderer requirements of the transport/world solver checked by FogMS_WorldViewProblem and
	// FogMS_BuildWorldLighting (engine defaults 3 and 1). ECVF_SetByGameSetting ranks above constructor defaults and
	// scalability, but below project settings, [SystemSettings]/ini, device profiles, ConsoleVariables.ini, the command line
	// and the console: an explicit value wins and is only reported (the Set is skipped so the engine logs no priority
	// warning). Editor: a value typed in the console, or restored by Restore Standard Lumen (FogMS_RestorePreviewSettings,
	// SetByConsole), is such an explicit value; Enable Indirect Preview itself sets 0, so it never conflicts. Not restored at
	// EndPlay or when the Box stops: a process (game) / session (editor) setting. One log line per call.
	void FogMS_ApplyRequiredRenderSettings(const AFogMSBoxVolume& Box, const TCHAR* Context)
	{
		struct FRequiredSetting
		{
			const TCHAR* Name;
			int32 Value;
		};
		static const FRequiredSetting RequiredSettings[] =
		{
			{ TEXT("r.RayTracing.Culling"), 0 },
			{ TEXT("r.Lumen.AsyncCompute"), 0 },
		};
		FString Summary;
		for (const FRequiredSetting& Setting : RequiredSettings)
		{
			IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Setting.Name);
			if (!Variable)
			{
				Summary += FString::Printf(TEXT(" %s unavailable;"), Setting.Name);
				continue;
			}
			const int32 Previous = Variable->GetInt();
			const EConsoleVariableFlags PreviousSetBy = static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask);
			if (Previous != Setting.Value && PreviousSetBy <= ECVF_SetByGameSetting) Variable->Set(Setting.Value, ECVF_SetByGameSetting);
			const int32 Current = Variable->GetInt();
			Summary += FString::Printf(TEXT(" %s %d -> %d (previous SetBy%s)%s;"), Setting.Name, Previous, Current,
				GetConsoleVariableSetByName(PreviousSetBy),
				Current == Setting.Value ? TEXT("") : TEXT(" kept: higher-priority project/ini/command-line/console value, Transport stays off"));
		}
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS %s: Apply Required Render Settings (%s, SetByGameSetting):%s"), *Box.GetName(), Context, *Summary);
	}

	float FogMS_SmoothStep(float A, float B, float X)
	{
		const float T = FMath::Clamp((X - A) / (B - A), 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}

	int32 FogMS_SunMapDimension(int32 Value, int32 Min, int32 Max)
	{
		return FMath::Clamp(FMath::DivideAndRoundUp(FMath::Max(Value, 1), 8) * 8, Min, Max);
	}

	// P2: the 'Create Cloud Host' button for game worlds and scripts (the -game smoke test uses it through -ExecCmds).
	FAutoConsoleCommandWithWorldAndArgs FogMS_CloudHostCreateCommand(TEXT("FogMS.CloudHost.Create"),
		TEXT("FogMS.CloudHost.Create [Box name or label]: what the Box button 'Create Cloud Host' does, for the first enabled FogMS Box of this ")
		TEXT("world (or the named one): spawns a Volumetric Cloud 'FogMS Cloud Host' with MI_FogMS_Cloud and the tested settings (unless a host ")
		TEXT("exists) and sets that Box's Render Path to Cloud Host. Only one Volumetric Cloud renders per scene: the host displaces the sky clouds. ")
		TEXT("Nothing is saved."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			AFogMSBoxVolume* Target = nullptr;
			for (TActorIterator<AFogMSBoxVolume> It(World); It && !Target; ++It)
			{
				if (It->IsActorBeingDestroyed()) continue;
				if (Args.Num() > 0 ? (It->GetName() == Args[0] || It->GetActorNameOrLabel() == Args[0]) : It->bEnabled) Target = *It;
			}
			if (!Target)
			{
				UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS.CloudHost.Create: no %s FogMS Box in this world; nothing spawned."),
					Args.Num() > 0 ? *FString::Printf(TEXT("'%s'"), *Args[0]) : TEXT("enabled"));
				return;
			}
			Target->CreateCloudHost();
		}));
}

FVector3f FogMS_FindAtmosphereSunDirection(UWorld* World)
{
	// Same atmosphere-light selection as the cloud host, weather map and renderer.
	if (const UDirectionalLightComponent* Light = UFogMSCloudHostSubsystem::FindAtmosphereSun(World))
		return FVector3f(-Light->GetForwardVector()).GetSafeNormal();
	return FVector3f::ZeroVector;
}

AFogMSBoxVolume::AFogMSBoxVolume()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	BoxComponent = CreateDefaultSubobject<UBoxComponent>(TEXT("BoxComponent"));
	SetRootComponent(BoxComponent);
	BoxComponent->SetMobility(EComponentMobility::Movable);
	BoxComponent->SetBoxExtent(FVector(1000.0f, 1000.0f, 1000.0f));
	BoxComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BoxComponent->SetGenerateOverlapEvents(false);
	BoxComponent->SetCanEverAffectNavigation(false);
	BoxComponent->bShouldCollideWhenPlacing = false;
	BoxComponent->bDrawOnlyIfSelected = false;
	BoxComponent->SetVisibility(true);
	BoxComponent->SetHiddenInGame(true);

	// The visible arrow is the runtime source itself: rotating it cannot rotate a
	// detached visualization while leaving the sampled wind direction unchanged.
	WindDirectionComponent = CreateDefaultSubobject<UArrowComponent>(TEXT("WindDirectionComponent"));
	WindDirectionComponent->SetupAttachment(BoxComponent);
	WindDirectionComponent->SetMobility(EComponentMobility::Movable);
	WindDirectionComponent->SetAbsolute(false, true, true);
	WindDirectionComponent->SetArrowColor(FLinearColor(0.2f, 0.8f, 1.0f));
	WindDirectionComponent->SetArrowLength(180.0f);
	WindDirectionComponent->SetHiddenInGame(true);
	WindDirectionComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	WindDirectionComponent->SetCanEverAffectNavigation(false);
	WindDirectionComponent->SetVisibleInRayTracing(false);
	WindDirectionComponent->SetCastShadow(false);

	DensityComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DensityComponent"));
	DensityComponent->SetupAttachment(BoxComponent);
	DensityComponent->SetMobility(EComponentMobility::Movable);
	DensityComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DensityComponent->SetGenerateOverlapEvents(false);
	DensityComponent->SetCanEverAffectNavigation(false);
	DensityComponent->bDisallowNanite = true;
	DensityComponent->SetCastShadow(false);
	DensityComponent->SetAffectDistanceFieldLighting(false);
	DensityComponent->SetAffectDynamicIndirectLighting(false);
	DensityComponent->SetVisibleInRayTracing(false);
	DensityComponent->SetHiddenInGame(false);
	DensityComponent->SetVisibility(false);
	DensityComponent->SetRelativeScale3D(BoxComponent->GetUnscaledBoxExtent() / 50.0);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(TEXT("/MultiLobeSpec/FogMS/M_FogMS_Density.M_FogMS_Density"));
	if (Cube.Succeeded()) DensityComponent->SetStaticMesh(Cube.Object);
	if (Material.Succeeded())
	{
		DensityMaterial = Material.Object;
		DensityComponent->SetMaterial(0, DensityMaterial);
	}
}

void AFogMSBoxVolume::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	UpdateDensity();
}

void AFogMSBoxVolume::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FogMS_MotionVersionGuid);
	Super::Serialize(Ar);
	// Missing version means an actor authored before Directional Wind existed.
	// Transactions and duplication must preserve their already chosen mode/state.
	if (Ar.IsLoading() && Ar.IsPersistent() && !Ar.IsTransacting()
		&& Ar.CustomVer(FogMS_MotionVersionGuid) < FogMS_DirectionalMotionVersion)
	{
		DensityMotionMode = EFogMSDensityMotionMode::LegacyVectors;
		DensityMotionReference = FFogMSDensityMotionReference{};
	}
}

void AFogMSBoxVolume::PostLoad()
{
	Super::PostLoad();
	// A preset never silently changes a loaded look. Actors saved before TransportPreset existed
	// load the default Production with their own saved fields (and TransportTolerance -1 = cvar,
	// which no preset uses): whenever the saved tuple differs from the preset's, keep the fields
	// and switch to Custom. A matching tuple (saved by a preset) keeps its preset.
	FFogMSTransportTier Tier;
	if (FogMS_GetTransportTier(TransportPreset, Tier) && !Tier.Matches(AngularQuality, TransportIterations, TransportTolerance))
		TransportPreset = EFogMSTransportPreset::Custom;
	UpdateDensity();
}

void AFogMSBoxVolume::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateDensity();
	AutoStartRuntime();
}

void AFogMSBoxVolume::BeginPlay()
{
	Super::BeginPlay();
	// Standalone game worlds (packaged, -game): every enabled Box whose mode needs the world producer (Transport, World)
	// applies the required settings, as before.
	const UWorld* World = GetWorld();
	if (bEnabled && World && World->WorldType == EWorldType::Game
		&& (FogMS_IsTransportMode(ScatteringMode) || ScatteringMode == EFogMSScatteringMode::WorldSpace))
		ApplyRequiredRenderSettingsOnce(TEXT("game BeginPlay"));
	// Game/PIE worlds have no Details button. An enabled Transport Box with Emissive Injection starts its runtime
	// itself: injection-only registers the view extension only; with BindlessAll (editor PIE) this is the button's path.
	// PIE/Simulate: this automatic start also applies the required settings (a game world did it above: once per actor).
	// Marks the start done, so the first tick's AutoStartRuntime does not repeat it.
	if (bEnabled && UsesEmissiveInjection())
		StartRuntimeAutomatically(World && World->WorldType == EWorldType::PIE ? TEXT("PIE/Simulate auto-start") : TEXT("BeginPlay auto-start"));
}

void AFogMSBoxVolume::AutoStartRuntime()
{
	// Injection-only configuration (no BindlessAll): starting the runtime changes no overlay state (no BoxMode cvar,
	// no engine-shader patch), so an enabled injection Box does it once on its first tick, also in the editor world.
	// The only global state it changes is Apply Required Render Settings (StartRuntimeAutomatically).
	// With BindlessAll the editor keeps the explicit Enable Live Box step (it applies the overlay patch).
	if (bRuntimeAutoStarted || !bEnabled || !UsesEmissiveInjection() || FogMS_IsBindlessAllConfiguration()) return;
	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || !GetWorld() || GetWorld()->IsPreviewWorld()) return;
	const EWorldType::Type WorldType = GetWorld()->WorldType;
	StartRuntimeAutomatically(WorldType == EWorldType::Editor ? TEXT("editor auto-start")
		: (WorldType == EWorldType::PIE ? TEXT("PIE/Simulate auto-start") : TEXT("game auto-start")));
}

void AFogMSBoxVolume::StartRuntimeAutomatically(const TCHAR* Context)
{
	bRuntimeAutoStarted = true;
	// Before the runtime starts, so its first frame already passes FogMS_WorldViewProblem: the solver's status then never
	// asks for Enable Indirect Preview for an automatically started Box (unless an explicit value was kept, see the helper).
	ApplyRequiredRenderSettingsOnce(Context);
	EnableLiveBox();
}

void AFogMSBoxVolume::ApplyRequiredRenderSettingsOnce(const TCHAR* Context)
{
	// Once per actor instance: a PIE/Simulate Box is a new instance and applies again (one log line each). Preview worlds
	// (asset editors, thumbnails) never change process-wide settings.
	if (!bApplyRequiredRenderSettings || bRequiredRenderSettingsApplied) return;
	const UWorld* World = GetWorld();
	if (!World || World->IsPreviewWorld() || HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)) return;
	bRequiredRenderSettingsApplied = true;
	FogMS_ApplyRequiredRenderSettings(*this, Context);
}

// Status estimate "tau core" (FogMS_BoxRuntime.cpp, AddBoxUpdate). CPU only, from the Box properties validated by the last
// UpdateDensity; no texture read, no GPU readback. Definition: optical depth of the Box's added medium along a full chord
// through the Box centre, parallel to one Box axis (face to face), minimum over the three axes (the thinnest direction):
//   tau_a = integral over the chord of sigma(s) ds,  sigma = Density[1/m] * 0.01 * mask(n_max(h)) * fade(s)  [1/cm, s in cm]
// with the FogMS_Extinction factors (matedit_density.py EXTINCTION_CODE_V2 / FogMS_IndirectLocalDensity):
//   fade = smoothstep(0, min(Density Edge Feather, min extent), distance to the nearest face) (step for width 0);
//   n_max(h) = P(h) * 1 + Detail Strength, the largest possible noise value: texture noise <= 1 (UNORM), detail <= 1,
//   erosion only subtracts; P = height profile at normalised height h (1 when off); h = 0.5 on the X/Y chords;
//   mask = smoothstep(Threshold -+ Softness/2, n_max) (step(Threshold) for Softness 0).
// Accuracy: the analytic factors (Density, feather, height profile, threshold band) are exact up to the 64-point midpoint
// rule (for a smooth fade well under 1 %; a step edge, Softness or Feather 0, up to 1/64 of the chord). The noise is not on
// the CPU (a cooked game has no texture source), so tau is an UPPER BOUND: the real optical depth along the same chord is
// lower by how much of it the noise leaves below the threshold band (coverage) and by erosion. Only this Box's density:
// height fog and other Boxes are not included. A value well below 1 means a thin medium with little multiple scattering.
// A trustworthy value needs the solver's per-cell sigma (pass 0): a column-sum reduction plus an async readback.
float AFogMSBoxVolume::GetCoreOpticalDepthEstimate() const
{
	const double Now = FPlatformTime::Seconds();
	if (CoreOpticalDepthTime > 0.0 && Now - CoreOpticalDepthTime < 1.0) return CoreOpticalDepth;
	CoreOpticalDepthTime = Now;
	CoreOpticalDepth = -1.0f;
	if (!IsDensitySourceActive() || !BoxComponent) return CoreOpticalDepth;
	// Same inputs as the MID (FogMS_WorldExtent, FogMS_DensityFeather, ...); UpdateDensity validated them this frame.
	const FVector Extent = BoxComponent->GetScaledBoxExtent().GetAbs();
	const float MinExtent = static_cast<float>(Extent.GetMin());
	const float Width = FMath::Min(DensityEdgeFeather, MinExtent);
	const float SigmaPerCm = FMath::Max(Density, 0.0f) * 0.01f;
	const auto Mask = [this](float N)
	{
		return Softness > 0.0f ? FogMS_SmoothStep(Threshold - Softness * 0.5f, Threshold + Softness * 0.5f, N)
			: (N >= Threshold ? 1.0f : 0.0f);
	};
	const auto SS = [](float A, float W, float X) { return FogMS_SmoothStep(A, A + FMath::Max(W, 1.0e-4f), X); };
	const auto Profile = [this, &SS](float H)
	{
		if (!bHeightProfile) return 1.0f;
		const float M = HeightBottom + 0.5f * (HeightTop - HeightBottom);
		return SS(HeightBottom, BottomSoftness, H) * (1.0f - SS(HeightTop - TopSoftness, TopSoftness, H))
			* (1.0f + AnvilStrength * SS(M, FMath::Max(HeightTop - TopSoftness - M, 1.0e-4f), H));
	};
	constexpr int32 Samples = 64;
	float Result = TNumericLimits<float>::Max();
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const float AxisExtent = static_cast<float>(Extent[Axis]);
		float Tau = 0.0f;
		for (int32 Index = 0; Index < Samples; ++Index)
		{
			const float S = (Index + 0.5f) / Samples * 2.0f - 1.0f; // Local coordinate along this axis, -1..1.
			float Inside = (1.0f - FMath::Abs(S)) * AxisExtent;
			for (int32 Other = 0; Other < 3; ++Other)
				if (Other != Axis) Inside = FMath::Min(Inside, static_cast<float>(Extent[Other]));
			const float Fade = Width > 0.0f ? FogMS_SmoothStep(0.0f, Width, Inside) : (Inside >= 0.0f ? 1.0f : 0.0f);
			const float H = Axis == 2 ? S * 0.5f + 0.5f : 0.5f;
			Tau += SigmaPerCm * Mask(Profile(H) + DetailStrength) * Fade;
		}
		Result = FMath::Min(Result, Tau * 2.0f * AxisExtent / Samples);
	}
	CoreOpticalDepth = FMath::IsFinite(Result) ? Result : -1.0f;
	return CoreOpticalDepth;
}

void AFogMSBoxVolume::Destroyed()
{
	// P2: a host this Box rendered through goes empty now (its tick would notice one frame later).
	if (bCloudHostBound)
	{
		if (UFogMSCloudHostSubsystem* Hosts = GetWorld() ? GetWorld()->GetSubsystem<UFogMSCloudHostSubsystem>() : nullptr) Hosts->ReleaseBox(*this);
		bCloudHostBound = false;
	}
	ReleaseTransportField();
	ReleaseSunDetailMaps();
	Super::Destroyed();
}

bool AFogMSBoxVolume::ShouldTickIfViewportsOnly() const
{
	return true;
}

bool AFogMSBoxVolume::EnsureTransportField()
{
	if (!IsValid(TransportField))
	{
		UTextureRenderTargetVolume* Field = NewObject<UTextureRenderTargetVolume>(this,
			MakeUniqueObjectName(this, UTextureRenderTargetVolume::StaticClass(), TEXT("FogMS_TransportField")), RF_Transient);
		// bSupportsUAV makes CreateResource set bCanCreateUAV, i.e. TexCreate_UAV on the RHI texture.
		Field->bSupportsUAV = true;
		Field->bHDR = true;
		Field->bForceLinearGamma = true;
		// Alpha 0 marks "no current field": the material falls back to native albedo lighting.
		Field->ClearColor = FLinearColor::Transparent;
		Field->Filter = TF_Bilinear;
		Field->Init(FogMS_TransportFieldSize, FogMS_TransportFieldSize, FogMS_TransportFieldSize, PF_FloatRGBA);
		Field->UpdateResourceImmediate(true);
		TransportField = Field;
		bHasMaterialState = false;
	}
	return TransportField->GetResource() != nullptr;
}

void AFogMSBoxVolume::ReleaseTransportField()
{
	if (!TransportField) return;
	TransportField = nullptr;
	if (IsValid(DensityMID))
	{
		// A null texture value cannot clear an existing MID override. Clear all overrides;
		// the next active UpdateDensity re-applies every parameter (bHasMaterialState=false).
		DensityMID->ClearParameterValues();
	}
	bHasMaterialState = false;
	// The UObject is released by GC (UTexture::BeginDestroy fences the resource). The render
	// thread holds its own FTextureRHIRef until the next Box packet replaces it.
}

bool AFogMSBoxVolume::EnsureSunDetailMaps()
{
	const int32 Resolution = FogMS_SunMapDimension(CVarSunMapResolution.GetValueOnGameThread(), 64, 512);
	const int32 Steps = FogMS_SunMapDimension(CVarSunMapSteps.GetValueOnGameThread(), 16, 256);
	const auto Make = [this](const TCHAR* Name, int32 X, int32 Y, int32 Z, EPixelFormat Format)
	{
		UTextureRenderTargetVolume* Target = NewObject<UTextureRenderTargetVolume>(this,
			MakeUniqueObjectName(this, UTextureRenderTargetVolume::StaticClass(), Name), RF_Transient);
		Target->bSupportsUAV = true;   // TexCreate_UAV: the render thread writes it with compute passes
		Target->bHDR = true;
		Target->bForceLinearGamma = true;
		Target->ClearColor = FLinearColor::Transparent; // cell G = 0: invalid until the first build
		Target->Filter = TF_Bilinear;                    // trilinear lookups in the material (no mips)
		Target->Init(X, Y, Z, Format);
		Target->UpdateResourceImmediate(true);
		return Target;
	};
	if (!IsValid(SunDetailMap) || SunDetailMap->SizeX != Resolution || SunDetailMap->SizeY != Resolution || SunDetailMap->SizeZ != Steps)
	{
		SunDetailMap = Make(TEXT("FogMS_SunDetailMap"), Resolution, Resolution, Steps, PF_G16R16F);
		bHasMaterialState = false;
	}
	if (!IsValid(SunDetailCell))
	{
		SunDetailCell = Make(TEXT("FogMS_SunDetailCell"), FogMS_TransportFieldSize, FogMS_TransportFieldSize, FogMS_TransportFieldSize, PF_FloatRGBA);
		bHasMaterialState = false;
	}
	return SunDetailMap->GetResource() != nullptr && SunDetailCell->GetResource() != nullptr;
}

void AFogMSBoxVolume::ReleaseSunDetailMaps()
{
	if (!SunDetailMap && !SunDetailCell) return;
	SunDetailMap = nullptr;
	SunDetailCell = nullptr;
	// As ReleaseTransportField: a null texture cannot clear a MID override, so clear all; the next update re-applies every one.
	if (IsValid(DensityMID)) DensityMID->ClearParameterValues();
	bHasMaterialState = false;
}

bool AFogMSBoxVolume::GetSunDetailRenderData(FFogMSSunDetailBasis& OutBasis, UTextureRenderTargetVolume*& OutMap, UTextureRenderTargetVolume*& OutCell) const
{
	if (!IsHybridInjectionActive() || LastDensityState.SunDetailValue <= 0.0f || !LastDensityState.SunBasis.bValid
		|| !IsValid(SunDetailMap) || !IsValid(SunDetailCell) || LastDensityState.SunMap.Get() != SunDetailMap
		|| LastDensityState.SunCell.Get() != SunDetailCell)
		return false;
	OutBasis = LastDensityState.SunBasis;
	OutMap = SunDetailMap;
	OutCell = SunDetailCell;
	return true;
}

void AFogMSBoxVolume::ApplyTransportPreset()
{
	// BoxRuntime packs these three fields; a non-Custom preset owns them. Idempotent.
	FFogMSTransportTier Tier;
	if (!FogMS_GetTransportTier(TransportPreset, Tier)) return;
	AngularQuality = Tier.AngularQuality;
	TransportIterations = Tier.Iterations;
	TransportTolerance = Tier.Tolerance;
}

void AFogMSBoxVolume::ApplyHeightProfilePreset()
{
	// Design table (FogMS_DensityAuthoring_Design.md section 3): B, T, SB, ST, A.
	float Values[5];
	switch (HeightProfilePreset)
	{
	case EFogMSHeightProfilePreset::Stratus: Values[0] = 0.40f; Values[1] = 0.60f; Values[2] = 0.05f; Values[3] = 0.10f; Values[4] = 0.0f; break;
	case EFogMSHeightProfilePreset::Cumulus: Values[0] = 0.10f; Values[1] = 0.80f; Values[2] = 0.05f; Values[3] = 0.20f; Values[4] = 0.0f; break;
	case EFogMSHeightProfilePreset::Cumulonimbus: Values[0] = 0.05f; Values[1] = 0.98f; Values[2] = 0.02f; Values[3] = 0.15f; Values[4] = 1.0f; break;
	case EFogMSHeightProfilePreset::ValleyFog: Values[0] = 0.0f; Values[1] = 0.35f; Values[2] = 0.0f; Values[3] = 0.30f; Values[4] = 0.0f; break;
	default: return;
	}
	HeightBottom = Values[0];
	HeightTop = Values[1];
	BottomSoftness = Values[2];
	TopSoftness = Values[3];
	AnvilStrength = Values[4];
	bHeightProfile = true;
}

#if WITH_EDITOR
void AFogMSBoxVolume::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	// Before Super: its construction rerun reaches UpdateDensity, which would overwrite a direct
	// edit (Details, Python set_editor_property) of a preset-owned field with the preset's value.
	const FName Name = PropertyChangedEvent.GetPropertyName();
	if (TransportPreset != EFogMSTransportPreset::Custom
		&& (Name == GET_MEMBER_NAME_CHECKED(AFogMSBoxVolume, AngularQuality)
			|| Name == GET_MEMBER_NAME_CHECKED(AFogMSBoxVolume, TransportIterations)
			|| Name == GET_MEMBER_NAME_CHECKED(AFogMSBoxVolume, TransportTolerance)))
		TransportPreset = EFogMSTransportPreset::Custom;
	// Height profile preset: editor-only apply on its own change; a direct edit of one of its values selects None.
	if (Name == GET_MEMBER_NAME_CHECKED(AFogMSBoxVolume, HeightProfilePreset))
		ApplyHeightProfilePreset();
	else if (Name == GET_MEMBER_NAME_CHECKED(AFogMSBoxVolume, HeightBottom)
		|| Name == GET_MEMBER_NAME_CHECKED(AFogMSBoxVolume, HeightTop)
		|| Name == GET_MEMBER_NAME_CHECKED(AFogMSBoxVolume, BottomSoftness)
		|| Name == GET_MEMBER_NAME_CHECKED(AFogMSBoxVolume, TopSoftness)
		|| Name == GET_MEMBER_NAME_CHECKED(AFogMSBoxVolume, AnvilStrength))
		HeightProfilePreset = EFogMSHeightProfilePreset::None;
	// Also here, not only in UpdateDensity: Blueprint defaults (archetypes) skip UpdateDensity.
	ApplyTransportPreset();
	Super::PostEditChangeProperty(PropertyChangedEvent);
	UpdateDensity();
}

void AFogMSBoxVolume::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	UpdateDensity();
}
#endif

bool FFogMSDensityMotionReference::IsFinite() const
{
	return FMath::IsFinite(Time) && !Displacement0.ContainsNaN() && !Displacement1.ContainsNaN()
		&& !Displacement2.ContainsNaN() && !Velocity0.ContainsNaN() && !Velocity1.ContainsNaN() && !Velocity2.ContainsNaN();
}

bool FFogMSDensityMotionReference::Equals(const FFogMSDensityMotionReference& Other) const
{
	return bInitialized == Other.bInitialized && Time == Other.Time
		&& Displacement0 == Other.Displacement0 && Displacement1 == Other.Displacement1 && Displacement2 == Other.Displacement2
		&& Velocity0 == Other.Velocity0 && Velocity1 == Other.Velocity1 && Velocity2 == Other.Velocity2;
}

bool AFogMSBoxVolume::GetDensityMotionVelocities(FVector& Out0, FVector& Out1, FVector& Out2, bool* bOutWeatherWind) const
{
	if (bOutWeatherWind) *bOutWeatherWind = false;
	bool bWeatherWind = false;
	if (DensityMotionMode == EFogMSDensityMotionMode::LegacyVectors)
		Out0 = DensityWindVelocity;
	else if (DensityMotionMode == EFogMSDensityMotionMode::Directional)
	{
		if (!WindDirectionComponent || !WindDirectionComponent->GetComponentTransform().IsValid()
			|| !FMath::IsFinite(WindSpeed) || WindSpeed < 0.0
			|| !FMath::IsFinite(EdgeFlowSpeed) || EdgeFlowSpeed < 0.0) return false;
		Out0 = WindDirectionComponent->GetForwardVector() * WindSpeed;
		if (bUseWeatherWind)
		{
			FVector WeatherVelocity;
			if (AFogMSWeather::GetWindVelocity(GetWorld(), WeatherVelocity) && !WeatherVelocity.ContainsNaN())
			{
				Out0 = WeatherVelocity;
				bWeatherWind = true;
			}
		}
	}
	else return false;
	Out1 = Out0 + DensityDetailVelocity;
	Out2 = Out1 + DensityEvolutionVelocity;
	// Preserve the original operations exactly for zero flow and Legacy mode.
	// The automatic offsets are relative to the common wind, not cumulative:
	// octave two uses half the world speed at twice the frequency.
	if (DensityMotionMode == EFogMSDensityMotionMode::Directional && EdgeFlowSpeed > 0.0)
	{
		FVector EdgeRight = WindDirectionComponent->GetRightVector();
		FVector EdgeUp = WindDirectionComponent->GetUpVector();
		if (bWeatherWind && !Out0.IsNearlyZero())
		{
			// Weather wind is horizontal in world space. Rotate the existing contour flow with it, without changing its authored speed.
			EdgeRight = FVector::CrossProduct(FVector::UpVector, Out0.GetSafeNormal()).GetSafeNormal();
			EdgeUp = FVector::UpVector;
		}
		Out1 += EdgeRight * EdgeFlowSpeed;
		Out2 -= EdgeUp * (0.5 * EdgeFlowSpeed);
	}
	if (bOutWeatherWind) *bOutWeatherWind = bWeatherWind;
	return !Out0.ContainsNaN() && !Out1.ContainsNaN() && !Out2.ContainsNaN();
}

bool AFogMSBoxVolume::EvaluateDirectionalMotion(double Time, const FVector& Velocity0, const FVector& Velocity1,
	const FVector& Velocity2, FVector& Out0, FVector& Out1, FVector& Out2)
{
	if (!FMath::IsFinite(Time)) return false;
	FFogMSDensityMotionReference Reference = DensityMotionReference;
	if (!Reference.bInitialized)
	{
		Reference = FFogMSDensityMotionReference{};
		Reference.bInitialized = true;
		// New live motion begins here; manual-first motion has a reproducible t=0 origin.
		Reference.Time = bUseManualAnimationTime ? 0.0 : Time;
		Reference.Velocity0 = Velocity0; Reference.Velocity1 = Velocity1; Reference.Velocity2 = Velocity2;
	}
	if (!Reference.IsFinite()) return false;
	const double Delta = Time - Reference.Time;
	Out0 = Reference.Displacement0 + Reference.Velocity0 * Delta;
	Out1 = Reference.Displacement1 + Reference.Velocity1 * Delta;
	Out2 = Reference.Displacement2 + Reference.Velocity2 * Delta;
	if (Out0.ContainsNaN() || Out1.ContainsNaN() || Out2.ContainsNaN()) return false;
	if (Reference.Velocity0 != Velocity0 || Reference.Velocity1 != Velocity1 || Reference.Velocity2 != Velocity2)
	{
		// Resolve the old segment at this exact CPU snapshot before changing slope.
		// Repeated edits within one frame/frozen time therefore cannot move the phase.
		Reference.Time = Time;
		Reference.Displacement0 = Out0; Reference.Displacement1 = Out1; Reference.Displacement2 = Out2;
		Reference.Velocity0 = Velocity0; Reference.Velocity1 = Velocity1; Reference.Velocity2 = Velocity2;
	}
	DensityMotionReference = Reference;
	return true;
}

bool AFogMSBoxVolume::FDensityState::HasSameDensityParameters(const FDensityState& Other) const
{
	return bUseNativeDensity == Other.bUseNativeDensity
		&& bEmissiveInjection == Other.bEmissiveInjection && bHybridInjection == Other.bHybridInjection
		&& bFieldOnlyInjection == Other.bFieldOnlyInjection
		&& InjectionField == Other.InjectionField
		&& Texture == Other.Texture && TextureResource == Other.TextureResource
		&& ChannelMask == Other.ChannelMask && TileScaleValue == Other.TileScaleValue
		&& bWorldAligned == Other.bWorldAligned && WorldFrequencies == Other.WorldFrequencies
		&& ThresholdValue == Other.ThresholdValue && SoftnessValue == Other.SoftnessValue
		&& DetailStrengthValue == Other.DetailStrengthValue && DetailScaleValue == Other.DetailScaleValue
		&& DetailSecondOctaveValue == Other.DetailSecondOctaveValue
		&& ErosionStrengthValue == Other.ErosionStrengthValue && ErosionDepthValue == Other.ErosionDepthValue
		&& ErosionMask == Other.ErosionMask
		&& bHeightProfile == Other.bHeightProfile && HeightBottomValue == Other.HeightBottomValue
		&& HeightTopValue == Other.HeightTopValue && BottomSoftnessValue == Other.BottomSoftnessValue
		&& TopSoftnessValue == Other.TopSoftnessValue && AnvilStrengthValue == Other.AnvilStrengthValue
		&& DensityValue == Other.DensityValue && Albedo == Other.Albedo
		&& WorldExtent == Other.WorldExtent && Feather == Other.Feather && TextureOffset == Other.TextureOffset;
}

bool AFogMSBoxVolume::FDensityState::HasSameMaterialParameters(const FDensityState& Other) const
{
	return HasSameDensityParameters(Other)
		&& WorldPhase0 == Other.WorldPhase0 && WorldPhase1 == Other.WorldPhase1 && WorldPhase2 == Other.WorldPhase2
		&& DepthPrefilterValue == Other.DepthPrefilterValue && PrefilterWavelengths == Other.PrefilterWavelengths
		&& ForwardStrengthValue == Other.ForwardStrengthValue && ForwardGValue == Other.ForwardGValue
		&& ForwardDepthValue == Other.ForwardDepthValue && ForwardEccValue == Other.ForwardEccValue
		&& ForwardFloorValue == Other.ForwardFloorValue
		&& SunDetailValue == Other.SunDetailValue && SunMapRowU == Other.SunMapRowU && SunMapRowV == Other.SunMapRowV
		&& SunMapRowW == Other.SunMapRowW && SunMap == Other.SunMap && SunCell == Other.SunCell
		&& FroxelWeightValue == Other.FroxelWeightValue;
}

bool AFogMSBoxVolume::FDensityState::HasSameEffect(const FDensityState& Other) const
{
	if (bActive != Other.bActive) return false;
	if (!bActive) return true;
	if (!HasSameDensityParameters(Other) || !WorldTransform.Equals(Other.WorldTransform, 0.0)
		|| bAnimationActive != Other.bAnimationActive) return false;
	// The W36 depth prefilter, the W37 forward lobe and the W41 sun detail map change only the native fog material (MID), never
	// the solver's density: no revision bump (no cold solve, no fog-history reset). The wavelengths derive from fields compared above.
	if (!bAnimationActive)
		return WorldPhase0 == Other.WorldPhase0 && WorldPhase1 == Other.WorldPhase1 && WorldPhase2 == Other.WorldPhase2;
	// Continuous phase progression updates the MID and shadow cache, not the global
	// fog-history revision. Authored edits, explicit seeks and backwards world time do.
	return bManualAnimationTime == Other.bManualAnimationTime
		&& MotionMode == Other.MotionMode
		&& bWeatherWindDriven == Other.bWeatherWindDriven
		&& MotionResetRevision == Other.MotionResetRevision
		&& DetailVelocity == Other.DetailVelocity && EvolutionVelocity == Other.EvolutionVelocity
		// The integrated weather trajectory changes slope during a blend, without a density discontinuity. Keep history through that
		// motion; source changes, actual density edits, explicit seeks and backwards time still invalidate it.
		&& ((bWeatherWindDriven && Other.bWeatherWindDriven)
			|| ((MotionMode != EFogMSDensityMotionMode::Directional || MotionReference.Equals(Other.MotionReference))
				&& WindVelocity == Other.WindVelocity))
		&& TimeOffset == Other.TimeOffset
		&& (!bManualAnimationTime || ManualTime == Other.ManualTime)
		&& SampleTime >= Other.SampleTime;
}

void AFogMSBoxVolume::GetDensityWorldMapping(FVector4f& OutFrequenciesAndMode, FVector3f& OutPhase0, FVector3f& OutPhase1, FVector3f& OutPhase2) const
{
	OutFrequenciesAndMode = FVector4f(LastDensityState.WorldFrequencies.R, LastDensityState.WorldFrequencies.G,
		LastDensityState.WorldFrequencies.B, LastDensityState.bWorldAligned ? 1.0f : 0.0f);
	OutPhase0 = FVector3f(LastDensityState.WorldPhase0.R, LastDensityState.WorldPhase0.G, LastDensityState.WorldPhase0.B);
	OutPhase1 = FVector3f(LastDensityState.WorldPhase1.R, LastDensityState.WorldPhase1.G, LastDensityState.WorldPhase1.B);
	OutPhase2 = FVector3f(LastDensityState.WorldPhase2.R, LastDensityState.WorldPhase2.G, LastDensityState.WorldPhase2.B);
}

void AFogMSBoxVolume::UpdateDensity()
{
	if (bUpdatingDensity || HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || IsActorBeingDestroyed()) return;
	TGuardValue<bool> UpdateGuard(bUpdatingDensity, true);
	SunDetailProblem.Reset(); // W41 status note, re-derived below
	RenderPathStatus.Reset(); // P2 status suffix, re-derived below
	// P2: the cloud host's MID when a host renders this Box this update (resolved below, after the injection state).
	UMaterialInstanceDynamic* HostMID = nullptr;
	// OnConstruction, PostLoad, PostEditChangeProperty, Tick and BoxRuntime (right before it
	// packs the transport controls) all pass here: the packet always sees the preset's values,
	// also after a Blueprint/runtime write that bypassed PostEditChangeProperty.
	ApplyTransportPreset();
	// Mode or switch change away from Emissive Injection: drop the field and its MID binding.
	if (TransportField && !UsesEmissiveInjection()) ReleaseTransportField();
	// W41: Sun Detail Shadow no longer requested (switch, strength, hybrid, debug view or Box off): drop its maps (16 MB) and bindings.
	if ((SunDetailMap || SunDetailCell) && !(bEnabled && bSunDetailShadow && SunDetailStrength > 0.0f && UsesEmissiveInjection()
		&& bHybridSingleScattering && !bDebugFieldOnly))
		ReleaseSunDetailMaps();
	if (WindDirectionComponent) WindDirectionComponent->SetVisibility(DensityMotionMode == EFogMSDensityMotionMode::Directional);

	FDensityState State;
	FString Problem;
	const UWorld* World = GetWorld();
	const FVector LocalExtent = BoxComponent ? BoxComponent->GetUnscaledBoxExtent() : FVector::ZeroVector;
	const FVector WorldExtent = BoxComponent ? BoxComponent->GetScaledBoxExtent().GetAbs() : FVector::ZeroVector;
	const FLinearColor TileValue(bWorldAlignedTexture ? FVector::OneVector : TileScale);
	const FLinearColor ExtentValue(WorldExtent);
	FLinearColor WorldFrequencies(0, 0, 0, 0);
	FLinearColor WorldPhase0(0, 0, 0, 0);
	FLinearColor WorldPhase1(0, 0, 0, 0);
	FLinearColor WorldPhase2(0, 0, 0, 0);
	bool bAnimationActive = false;
	bool bWeatherWindDriven = false;
	double AnimationSampleTime = 0.0;
	FVector MotionVelocity0 = FVector::ZeroVector, MotionVelocity1 = FVector::ZeroVector, MotionVelocity2 = FVector::ZeroVector;
	FString AnimationStatus = bAnimateDensity ? TEXT("Waiting for a valid density source") : TEXT("Off");
	const bool bValidBounds = BoxComponent && DensityComponent && BoxComponent->GetComponentTransform().IsValid()
		&& FogMS_IsFinitePositiveVector(LocalExtent) && FogMS_IsFinitePositiveVector(WorldExtent)
		&& FogMS_IsFiniteColor(ExtentValue) && FMath::Min3(ExtentValue.R, ExtentValue.G, ExtentValue.B) > 0.0f;

	if (bValidBounds)
	{
		// The engine cube spans -50..50 cm. Negative world scale mirrors local-mode texture with the box.
		const FTransform RelativeTransform(FQuat::Identity, FVector::ZeroVector, LocalExtent / 50.0);
		if (!DensityComponent->GetRelativeTransform().Equals(RelativeTransform, 0.0))
		{
			DensityComponent->SetRelativeTransform(RelativeTransform);
		}
	}

	if (bDensityEnabled)
	{
		if (!bValidBounds)
		{
			Problem = TEXT("Box transform and extents must be finite with positive extents.");
		}
		else if (!bWorldAlignedTexture && (!FogMS_IsFinitePositiveVector(TileScale) || !FogMS_IsFiniteColor(TileValue)
			|| FMath::Min3(TileValue.R, TileValue.G, TileValue.B) <= 0.0f))
		{
			Problem = TEXT("Tile Scale must contain positive finite values representable by the material.");
		}
		else if (!FMath::IsFinite(Threshold) || Threshold < 0.0f || Threshold > 1.0f
			|| !FMath::IsFinite(Softness) || Softness < 0.0f || Softness > 1.0f)
		{
			Problem = TEXT("Threshold and Softness must be finite and between zero and one.");
		}
		else if (!FMath::IsFinite(DetailStrength) || DetailStrength < 0.0f || DetailStrength > 1.0f
			|| !FMath::IsFinite(DetailScale) || DetailScale < 0.1f || DetailScale > 32.0f
			|| !FMath::IsFinite(DetailSecondOctave) || DetailSecondOctave < 0.0f || DetailSecondOctave > 1.0f)
		{
			Problem = TEXT("Detail Strength and Second Octave must be finite in [0,1], and Detail Scale in [0.1,32].");
		}
		else if (!FMath::IsFinite(ErosionStrength) || ErosionStrength < 0.0f || ErosionStrength > 1.0f
			|| !FMath::IsFinite(ErosionDepth) || ErosionDepth < 0.01f || ErosionDepth > 1.0f
			|| static_cast<uint8>(ErosionChannel) > static_cast<uint8>(EFogMSErosionChannel::A))
		{
			Problem = TEXT("Erosion Strength must be finite in [0,1], Erosion Depth in [0.01,1], and Erosion Channel G, B or A.");
		}
		else if (bHeightProfile && (!FMath::IsFinite(HeightBottom) || !FMath::IsFinite(HeightTop)
			|| HeightBottom < 0.0f || HeightTop > 1.0f || HeightBottom >= HeightTop
			|| !FMath::IsFinite(BottomSoftness) || BottomSoftness < 0.0f || BottomSoftness > 1.0f
			|| !FMath::IsFinite(TopSoftness) || TopSoftness < 0.0f || TopSoftness > 1.0f
			|| !FMath::IsFinite(AnvilStrength) || AnvilStrength < 0.0f || AnvilStrength > 1.0f))
		{
			Problem = TEXT("Height Profile requires finite 0 <= Height Bottom < Height Top <= 1, and Bottom/Top Softness and Anvil Strength in [0,1].");
		}
		else if (bWorldAlignedTexture && !FogMS_MakeWorldMapping(BoxComponent->GetComponentLocation(), TextureOffsetWorld, WorldTextureSize, DetailScale,
			WorldFrequencies, WorldPhase0, WorldPhase1, WorldPhase2))
		{
			Problem = TEXT("World Texture Size must be finite in [1,100000000] cm, with finite Texture Offset, positive frequencies and world phases.");
		}
		else if (!FMath::IsFinite(Density) || Density < 0.0f
			|| !FMath::IsFinite(DensityEdgeFeather) || DensityEdgeFeather < 0.0f)
		{
			Problem = TEXT("Density and Density Edge Feather must be finite and non-negative.");
		}
		else if (!FogMS_IsFiniteColor(DensityAlbedo)
			|| DensityAlbedo.R < 0.0f || DensityAlbedo.R > 1.0f
			|| DensityAlbedo.G < 0.0f || DensityAlbedo.G > 1.0f
			|| DensityAlbedo.B < 0.0f || DensityAlbedo.B > 1.0f)
		{
			Problem = TEXT("Density Albedo must be finite with RGB channels between zero and one.");
		}
		else if (static_cast<uint8>(DensityChannel) > static_cast<uint8>(EFogMSDensityChannel::A))
		{
			Problem = TEXT("Density Channel must be R, G, B or A.");
		}
		else if (!IsValid(DensityTexture))
		{
			Problem = TEXT("Assign a valid Volume Texture to Density Texture.");
		}
#if WITH_EDITOR
		else if (DensityTexture->IsCompiling())
		{
			Problem = TEXT("Density Texture is still compiling; the source will resume when it is ready.");
		}
#endif
		else if (DensityTexture->GetSizeX() <= 0 || DensityTexture->GetSizeY() <= 0 || DensityTexture->GetSizeZ() <= 0
			|| DensityTexture->GetNumMips() <= 0 || !DensityTexture->GetResource())
		{
			Problem = TEXT("Density Texture has no usable 3D texture resource.");
		}
		else if (!IsValid(DensityMaterial) || !DensityMaterial->GetMaterial()
			|| DensityMaterial->GetMaterial()->MaterialDomain != MD_Volume || DensityMaterial->GetBlendMode() != BLEND_Additive
			|| !DensityComponent->GetStaticMesh())
		{
			Problem = TEXT("FogMS density cube or additive Volume material is unavailable.");
		}
		else if (World && Density > 0.0f && FogMS_IsDensityActorVisible(*this, World))
		{
			if (!IsValid(DensityMID))
			{
				DensityMID = UMaterialInstanceDynamic::Create(DensityMaterial, this);
				bHasMaterialState = false;
			}
			if (!DensityMID)
			{
				Problem = TEXT("Could not create the FogMS density material instance.");
			}
			else
			{
				if (bAnimateDensity)
				{
					if (!bEnabled)
						AnimationStatus = TEXT("Static: Box Enabled is false");
					else if (!bWorldAlignedTexture)
						AnimationStatus = TEXT("Static: animation requires World Aligned Texture");
					else if (ScatteringMode != EFogMSScatteringMode::WorldSpace && !FogMS_IsTransportMode(ScatteringMode))
						AnimationStatus = TEXT("Static: animation requires World or Transport scattering; Spatial history is unsupported");
					else if (!GetDensityMotionVelocities(MotionVelocity0, MotionVelocity1, MotionVelocity2, &bWeatherWindDriven)
						|| !FMath::IsFinite(AnimationTimeOffset) || (bUseManualAnimationTime && !FMath::IsFinite(ManualAnimationTime)))
						AnimationStatus = TEXT("Static: wind direction, nonnegative wind/edge-flow speeds, relative velocities and time must be valid");
					else
					{
						AnimationSampleTime = (bUseManualAnimationTime ? ManualAnimationTime : FogMS_GetDensityFrameTime(World)) + AnimationTimeOffset;
						FLinearColor Phase0, Phase1, Phase2;
						const FVector Center = BoxComponent->GetComponentLocation();
						FVector Displacement0, Displacement1, Displacement2;
						bool bMotionValid = FMath::IsFinite(AnimationSampleTime);
						if (bMotionValid && DensityMotionMode == EFogMSDensityMotionMode::Directional)
							bMotionValid = EvaluateDirectionalMotion(AnimationSampleTime, MotionVelocity0, MotionVelocity1, MotionVelocity2,
								Displacement0, Displacement1, Displacement2);
						else if (bMotionValid)
						{
							// Original absolute-time semantics remain intact until explicit conversion.
							Displacement0 = MotionVelocity0 * AnimationSampleTime;
							Displacement1 = MotionVelocity1 * AnimationSampleTime;
							Displacement2 = MotionVelocity2 * AnimationSampleTime;
						}
						bAnimationActive = bMotionValid
							&& FogMS_DisplacedWorldPhase(Center, Displacement0, TextureOffsetWorld, WorldFrequencies.R, Phase0)
							&& FogMS_DisplacedWorldPhase(Center, Displacement1, TextureOffsetWorld, WorldFrequencies.G, Phase1)
							&& FogMS_DisplacedWorldPhase(Center, Displacement2, TextureOffsetWorld, WorldFrequencies.B, Phase2);
						if (bAnimationActive)
						{
							WorldPhase0 = Phase0; WorldPhase1 = Phase1; WorldPhase2 = Phase2;
							AnimationStatus = bUseManualAnimationTime ? TEXT("Frozen at manual time") : TEXT("Active: shared world-time density phases");
							AnimationStatus += bWeatherWindDriven ? TEXT("; wind: FogMS Weather")
								: bUseWeatherWind && DensityMotionMode == EFogMSDensityMotionMode::LegacyVectors
									? TEXT("; wind: legacy (Use Directional Motion to follow weather)")
									: bUseWeatherWind ? TEXT("; wind: local fallback (no active FogMS Weather)") : TEXT("; wind: local");
						}
						else AnimationStatus = TEXT("Static: animation phase is not finite");
					}
				}
				State.bActive = true;
				// Emissive Injection: native voxelization owns sigma_t and receives sigma_s*J as
				// emissive; the Box packet marks row 23.w = 5 (hybrid: 6) so the overlay adds neither.
				// Falls back to the overlay path (MID density 0) if the field has no resource.
				const bool bInjection = bEnabled && UsesEmissiveInjection() && EnsureTransportField();
				// Keep authored density valid for the B2 atlas while avoiding duplicate
				// native voxelization. Leaving Transport restores the authored value.
				// Without BindlessAll no overlay injects density (row 23.w 4), so Transport without injection keeps
				// the native MID density: the Box stays visible, lit natively (fail closed).
				State.bUseNativeDensity = !bEnabled || !FogMS_IsTransportMode(ScatteringMode) || bInjection
					|| !FogMS_IsBindlessAllConfiguration();
				State.bEmissiveInjection = bInjection;
				// Debug field only (MID mode 3): the material shows sigma_s * J alone, so the solver must publish the full
				// field (row 23.w = 5, as mode 1): the hybrid field lacks the uncollided sun term. It overrides the hybrid.
				State.bFieldOnlyInjection = bInjection && bDebugFieldOnly;
				// Hybrid: native fog keeps single scattering; the field carries J_ms and T_sun (packet row 23.w = 6).
				State.bHybridInjection = bInjection && bHybridSingleScattering && !State.bFieldOnlyInjection;
				State.InjectionField = bInjection ? TransportField.Get() : nullptr;
				// P2 Render Path = Cloud Host (FogMS_CloudHost.h): the host renders from this Box's transport field, so it needs the
				// injection state above. With a usable host the solver publishes the hybrid field (the cloud marches the sun itself on
				// every ray step; Field Only (Debug) keeps the full field, mode 3), the froxel copy gets FogMS_FroxelWeight 0 and the
				// host's MID gets this Box's parameters below. Otherwise the Box stays in the froxels; the status says why.
				if (RenderPath == EFogMSRenderPath::CloudHost && bEnabled)
				{
					if (!bInjection)
						RenderPathStatus = TEXT(" [cloud host: needs Transport with Emissive Injection, froxel fallback]");
					else if (UFogMSCloudHostSubsystem* Hosts = World->GetSubsystem<UFogMSCloudHostSubsystem>())
					{
						FFogMSCloudBoxGeometry Geometry;
						GetCloudHostGeometry(Geometry);
						FFogMSCloudHostBinding Binding = Hosts->AcquireHost(*this, Geometry);
						HostMID = Binding.MID;
						RenderPathStatus = MoveTemp(Binding.Status);
					}
					else RenderPathStatus = TEXT(" [cloud host: no cloud host subsystem in this world, froxel fallback]");
				}
				if (HostMID)
				{
					State.bHybridInjection = !State.bFieldOnlyInjection;
					State.FroxelWeightValue = 0.0f;
					// The cloud shadows itself per step: no sun map (16 MB) for this Box while the host renders it.
					if (SunDetailMap || SunDetailCell) ReleaseSunDetailMaps();
					if (bSunDetailShadow) SunDetailProblem = TEXT("the cloud host marches the sun per step");
				}
				State.Texture = DensityTexture.Get();
				State.TextureResource = DensityTexture->GetResource();
				State.ChannelMask = FLinearColor(0, 0, 0, 0);
				State.ChannelMask.Component(static_cast<int32>(DensityChannel)) = 1.0f;
				State.TileScaleValue = TileValue;
				State.bWorldAligned = bWorldAlignedTexture;
				State.WorldFrequencies = WorldFrequencies;
				State.WorldPhase0 = WorldPhase0;
				State.WorldPhase1 = WorldPhase1;
				State.WorldPhase2 = WorldPhase2;
				State.ThresholdValue = Threshold;
				State.SoftnessValue = Softness;
				State.DetailStrengthValue = DetailStrength;
				State.DetailScaleValue = DetailScale;
				State.DetailSecondOctaveValue = DetailSecondOctave;
				State.ErosionStrengthValue = ErosionStrength;
				State.ErosionDepthValue = ErosionDepth;
				State.ErosionMask = FLinearColor(0, 0, 0, 0);
				State.ErosionMask.Component(1 + static_cast<int32>(ErosionChannel)) = 1.0f;
				// Profile off keeps the neutral defaults, so editing a disabled profile changes nothing.
				State.bHeightProfile = bHeightProfile;
				if (bHeightProfile)
				{
					State.HeightBottomValue = HeightBottom;
					State.HeightTopValue = HeightTop;
					State.BottomSoftnessValue = BottomSoftness;
					State.TopSoftnessValue = TopSoftness;
					State.AnvilStrengthValue = AnvilStrength;
				}
				// W36 depth prefilter (material FogMS_Extinction v3, matedit_density.py). Non-finite = 0 (off); the Details
				// clamp [0,2] is enforced here too for Blueprint/Python writes.
				State.DepthPrefilterValue = FMath::IsFinite(DepthPrefilter) ? FMath::Clamp(DepthPrefilter, 0.0f, 2.0f) : 0.0f;
				{
					// lambda_i = world feature size of noise band i = its texture tile period / 4 (the bundled Perlin-Worley has a
					// base lattice of 4 cells per tile: gen_perlin_worley.py perlin_fbm(4, 5), worley_fbm(4)). World aligned: tile =
					// 1 / frequency (exact, isotropic). Local: tile per Box axis = 2 * extent / Tile Scale; one scalar per band,
					// the geometric mean of the three axes (exact for isotropic tiling). Detail octaves as the material's detail
					// UVs: tile / Detail Scale and tile / (2 * Detail Scale). All values validated positive above.
					constexpr double FeaturesPerTile = 4.0;
					double Tile0, Tile1, Tile2;
					if (bWorldAlignedTexture)
					{
						Tile0 = 1.0 / WorldFrequencies.R; Tile1 = 1.0 / WorldFrequencies.G; Tile2 = 1.0 / WorldFrequencies.B;
					}
					else
					{
						Tile0 = FMath::Pow(2.0 * WorldExtent.X / TileValue.R * 2.0 * WorldExtent.Y / TileValue.G
							* 2.0 * WorldExtent.Z / TileValue.B, 1.0 / 3.0);
						Tile1 = Tile0 / DetailScale; Tile2 = Tile1 * 0.5;
					}
					const FLinearColor Wavelengths(static_cast<float>(Tile0 / FeaturesPerTile), static_cast<float>(Tile1 / FeaturesPerTile),
						static_cast<float>(Tile2 / FeaturesPerTile), 0.0f);
					// 0 = unknown: the material leaves that band unfiltered.
					State.PrefilterWavelengths = FogMS_IsFiniteColor(Wavelengths) && FMath::Min3(Wavelengths.R, Wavelengths.G, Wavelengths.B) > 0.0f
						? Wavelengths : FLinearColor(0, 0, 0, 0);
				}
				// W37/W38 forward lobe (material node FogMS_ForwardLobe v2, matedit_density.py) from the Multiple Scattering Look
				// set. Details clamps enforced here too for Blueprint/Python writes; non-finite MS Contribution = 0 (off), other
				// non-finite values = their defaults. MS Occlusion 0 is allowed (the node clamps b to >= 0.001, S = 0 still selects
				// Lobe 1). The material applies the lobe only in hybrid mode 2 (FogMS_InjectionMode), so no extra gate here.
				const auto Finite = [](float Value, float Min, float Max, float Fallback)
				{
					return FMath::IsFinite(Value) ? FMath::Clamp(Value, Min, Max) : Fallback;
				};
				State.ForwardStrengthValue = Finite(MSContribution, 0.0f, 1.0f, 0.0f);
				State.ForwardGValue = Finite(PhaseG, 0.0f, 0.9f, 0.6f);
				State.ForwardDepthValue = Finite(MSOcclusion, 0.0f, 1.0f, 0.5f);
				State.ForwardEccValue = Finite(MSEccentricity, 0.0f, 1.0f, 0.5f);
				State.ForwardFloorValue = Finite(MSBackFloor, 0.0f, 1.0f, 0.25f);
				// W46 host prefilter strength (host MID only, written every update; the Details clamp [0,4] here too).
				State.HostPrefilterValue = Finite(HostPrefilter, 0.0f, 4.0f, 1.0f);
				// W41 Sun Detail Shadow (material node FogMS_SunDetail v1, matedit_density.py; map FogMS_SunDetail.usf). Only with the hybrid
				// split: it redistributes the native sun single scattering that mode 2 scales by the field's per-cell sun share. The basis
				// uses this frame's atmosphere sun (the one the runtime hands the solver); the render thread builds the map with it.
				if (State.bHybridInjection && bSunDetailShadow && FMath::IsFinite(SunDetailStrength) && SunDetailStrength > 0.0f && !HostMID)
				{
					float MaterialDefault = 0.0f;
					if (!DensityMaterial->GetScalarParameterDefaultValue(FHashedMaterialParameterInfo(TEXT("FogMS_SunDetail")), MaterialDefault))
						SunDetailProblem = TEXT("material has no FogMS_SunDetail node (run matedit_density.py)");
					else if (!EnsureSunDetailMaps())
						SunDetailProblem = TEXT("map textures unavailable");
					else
					{
						// Band: with the height profile on and Threshold - Softness/2 > Detail Strength the density is exactly 0 outside
						// Height Bottom..Top (FogMS_IndirectLocalDensity exits on P = 0 there), so the map covers that band only.
						float BandMin = -static_cast<float>(WorldExtent.Z), BandMax = static_cast<float>(WorldExtent.Z);
						if (bHeightProfile && Threshold - 0.5f * Softness - DetailStrength > 1.0e-6f)
						{
							const float SignZ = BoxComponent->GetComponentScale().Z < 0.0 ? -1.0f : 1.0f; // UVSign.z, packet row 10.x
							const float Z0 = (HeightBottom - 0.5f) * 2.0f * static_cast<float>(WorldExtent.Z) * SignZ;
							const float Z1 = (HeightTop - 0.5f) * 2.0f * static_cast<float>(WorldExtent.Z) * SignZ;
							BandMin = FMath::Min(Z0, Z1);
							BandMax = FMath::Max(Z0, Z1);
						}
						// The packet's Box axes (FillBoxPacket: rows 2..4 from the Box component's rotation).
						const FQuat Rotation = BoxComponent->GetComponentQuat();
						const FVector Axes[3] = { Rotation.GetAxisX(), Rotation.GetAxisY(), Rotation.GetAxisZ() };
						const FFogMSSunDetailBasis Basis = FogMS_MakeSunDetailBasis(FogMS_FindAtmosphereSunDirection(GetWorld()),
							FVector3f(Axes[0]), FVector3f(Axes[1]), FVector3f(Axes[2]), FVector3f(WorldExtent), BandMin, BandMax, SunDetailMap->SizeZ);
						State.SunMap = SunDetailMap.Get();
						State.SunCell = SunDetailCell.Get();
						if (!Basis.bValid) SunDetailProblem = TEXT("no atmosphere sun above the horizon");
						else
						{
							// The basis rows take Box-axis centimetres (Local = dot(World - Center, Axis_r)); the material has the cube's
							// object space P (-50..50, TransformPosition_0). World = M P (the density component's LocalToWorld), so
							// Local_r = sum_i P_i dot(M axis i, Axis_r) + dot(M origin - Center, Axis_r) and each material row is
							// Row . (K P + Off): exact for any scale sign (no assumption on how the cube maps to the Box).
							const FMatrix M = DensityComponent->GetComponentTransform().ToMatrixWithScale();
							const FVector Center = BoxComponent->GetComponentLocation();
							const EAxis::Type ObjectAxes[3] = { EAxis::X, EAxis::Y, EAxis::Z };
							double K[3][3] = {}, Off[3] = {};
							for (int32 R = 0; R < 3; ++R)
							{
								for (int32 I = 0; I < 3; ++I) K[R][I] = FVector::DotProduct(M.GetScaledAxis(ObjectAxes[I]), Axes[R]);
								Off[R] = FVector::DotProduct(M.GetOrigin() - Center, Axes[R]);
							}
							const FVector4f Rows[3] = { Basis.RowU, Basis.RowV, Basis.RowW };
							FLinearColor Material[3] = { FLinearColor(0, 0, 0, 0), FLinearColor(0, 0, 0, 0), FLinearColor(0, 0, 0, 0) };
							for (int32 Q = 0; Q < 3; ++Q)
							{
								double W = Rows[Q].W;
								for (int32 R = 0; R < 3; ++R) W += Rows[Q][R] * Off[R];
								for (int32 I = 0; I < 3; ++I)
								{
									double Value = 0.0;
									for (int32 R = 0; R < 3; ++R) Value += Rows[Q][R] * K[R][I];
									Material[Q].Component(I) = static_cast<float>(Value);
								}
								Material[Q].A = static_cast<float>(W);
							}
							if (FogMS_IsFiniteColor(Material[0]) && FogMS_IsFiniteColor(Material[1]) && FogMS_IsFiniteColor(Material[2]))
							{
								State.SunDetailValue = FMath::Min(SunDetailStrength, 1.0f);
								State.SunMapRowU = Material[0];
								State.SunMapRowV = Material[1];
								State.SunMapRowW = Material[2];
								State.SunBasis = Basis;
							}
							else SunDetailProblem = TEXT("non-finite map transform");
						}
					}
				}
				State.DensityValue = Density;
				State.Albedo = FLinearColor(DensityAlbedo.R, DensityAlbedo.G, DensityAlbedo.B, 1.0f);
				State.WorldExtent = ExtentValue;
				State.Feather = DensityEdgeFeather;
				State.WorldTransform = DensityComponent->GetComponentTransform();
				State.TextureOffset = bWorldAlignedTexture ? TextureOffsetWorld : FVector::ZeroVector;
				State.bAnimationActive = bAnimationActive;
				if (bAnimationActive)
				{
					State.bManualAnimationTime = bUseManualAnimationTime;
					State.bWeatherWindDriven = bWeatherWindDriven;
					State.MotionResetRevision = MotionResetRevision;
					State.MotionMode = DensityMotionMode;
					State.MotionReference = DensityMotionReference;
					State.WindVelocity = MotionVelocity0;
					State.DetailVelocity = DensityDetailVelocity;
					State.EvolutionVelocity = DensityEvolutionVelocity;
					State.ManualTime = ManualAnimationTime;
					State.TimeOffset = AnimationTimeOffset;
					State.SampleTime = AnimationSampleTime;
				}

				if (!bHasMaterialState || !State.HasSameMaterialParameters(LastMaterialState))
				{
					WriteDensityParameters(*DensityMID, State, false);
					LastMaterialState = State;
					bHasMaterialState = true;
				}
				// P2: the host's MID gets the same parameters every update (a MID skips unchanged values), so a new or emptied host and
				// a moved Box are always current; the host's own FogMS_Cloud* look parameters stay the instance's.
				if (HostMID) WriteDensityParameters(*HostMID, State, true);
				if (DensityComponent->GetMaterial(0) != DensityMID) DensityComponent->SetMaterial(0, DensityMID);
			}
		}
	}

	if (DensityComponent && DensityComponent->GetVisibleFlag() != State.bActive)
	{
		DensityComponent->SetVisibility(State.bActive);
	}
	if (!State.HasSameEffect(LastDensityState))
	{
		++DensityRevision;
	}
	// P2: a host this Box rendered through last update but not now (Render Path, fallback, density off) goes empty.
	if (bCloudHostBound && !HostMID)
	{
		if (UFogMSCloudHostSubsystem* Hosts = World ? World->GetSubsystem<UFogMSCloudHostSubsystem>() : nullptr) Hosts->ReleaseBox(*this);
	}
	bCloudHostBound = HostMID != nullptr;
	// Refresh even when only animated phases changed. The packet must match the MID.
	LastDensityState = State;
	DensityAnimationTime = State.bAnimationActive ? State.SampleTime : 0.0;
	DensityAnimationStatus = MoveTemp(AnimationStatus);
	if (Problem != LastDensityProblem)
	{
		if (!Problem.IsEmpty())
		{
			UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS density disabled on %s: %s"), *GetPathName(), *Problem);
		}
		LastDensityProblem = MoveTemp(Problem);
	}
}

void AFogMSBoxVolume::WriteDensityParameters(UMaterialInstanceDynamic& MID, const FDensityState& State, bool bCloudHost) const
{
	MID.SetTextureParameterValue(TEXT("FogMS_Noise"), DensityTexture);
	MID.SetVectorParameterValue(TEXT("FogMS_ChannelMask"), State.ChannelMask);
	MID.SetVectorParameterValue(TEXT("FogMS_TileScale"), State.TileScaleValue);
	MID.SetScalarParameterValue(TEXT("FogMS_WorldAligned"), State.bWorldAligned ? 1.0f : 0.0f);
	MID.SetVectorParameterValue(TEXT("FogMS_WorldFrequencies"), State.WorldFrequencies);
	MID.SetVectorParameterValue(TEXT("FogMS_WorldPhase0"), State.WorldPhase0);
	MID.SetVectorParameterValue(TEXT("FogMS_WorldPhase1"), State.WorldPhase1);
	MID.SetVectorParameterValue(TEXT("FogMS_WorldPhase2"), State.WorldPhase2);
	MID.SetScalarParameterValue(TEXT("FogMS_Threshold"), State.ThresholdValue);
	MID.SetScalarParameterValue(TEXT("FogMS_Softness"), State.SoftnessValue);
	MID.SetScalarParameterValue(TEXT("FogMS_DetailStrength"), State.DetailStrengthValue);
	MID.SetScalarParameterValue(TEXT("FogMS_DetailScale"), State.DetailScaleValue);
	MID.SetScalarParameterValue(TEXT("FogMS_DetailSecondOctave"), State.DetailSecondOctaveValue);
	// S1/S2 (material node FogMS_Extinction, Tools/FogMSEnergyValidation/ProdProbe/matedit_density.py).
	MID.SetScalarParameterValue(TEXT("FogMS_ErosionStrength"), State.ErosionStrengthValue);
	MID.SetScalarParameterValue(TEXT("FogMS_ErosionDepth"), State.ErosionDepthValue);
	MID.SetVectorParameterValue(TEXT("FogMS_ErosionMask"), State.ErosionMask);
	MID.SetScalarParameterValue(TEXT("FogMS_HeightProfile"), State.bHeightProfile ? 1.0f : 0.0f);
	MID.SetScalarParameterValue(TEXT("FogMS_HeightBottom"), State.HeightBottomValue);
	MID.SetScalarParameterValue(TEXT("FogMS_HeightTop"), State.HeightTopValue);
	MID.SetScalarParameterValue(TEXT("FogMS_HeightBottomSoftness"), State.BottomSoftnessValue);
	MID.SetScalarParameterValue(TEXT("FogMS_HeightTopSoftness"), State.TopSoftnessValue);
	MID.SetScalarParameterValue(TEXT("FogMS_HeightAnvilStrength"), State.AnvilStrengthValue);
	// W36 (FogMS_DepthFootprint / FogMS_Extinction v3). A material without them ignores both (material defaults
	// 0 = v2 math). Depth Prefilter is froxel-only; the wavelengths also feed the W46 host prefilter (M_FogMS_Cloud v2 node
	// FogMS_CloudFootprint: w = FogMS_CloudPrefilter * max(FogMS_CloudStep, traced pixel width); strength 0 = w 0, the solver's formula).
	MID.SetVectorParameterValue(TEXT("FogMS_PrefilterWavelengths"), State.PrefilterWavelengths);
	if (bCloudHost)
		MID.SetScalarParameterValue(TEXT("FogMS_CloudPrefilter"), State.HostPrefilterValue);
	else
		MID.SetScalarParameterValue(TEXT("FogMS_DepthPrefilter"), State.DepthPrefilterValue);
	// W37/W38 (FogMS_ForwardLobe). A material without the node ignores them; Strength 0 returns the injection
	// Emissive unchanged. FogMS_ForwardEcc exists from node v2 on; a v1 node ignores it (fixed g/2 = Ecc 0.5).
	MID.SetScalarParameterValue(TEXT("FogMS_ForwardStrength"), State.ForwardStrengthValue);
	MID.SetScalarParameterValue(TEXT("FogMS_ForwardG"), State.ForwardGValue);
	MID.SetScalarParameterValue(TEXT("FogMS_ForwardDepth"), State.ForwardDepthValue);
	MID.SetScalarParameterValue(TEXT("FogMS_ForwardEcc"), State.ForwardEccValue);
	MID.SetScalarParameterValue(TEXT("FogMS_ForwardFloor"), State.ForwardFloorValue);
	if (!bCloudHost)
	{
		// W41 (FogMS_SunDetail v1). Strength 0 (off, night, material default) returns the field alpha unchanged; the textures
		// stay bound while the maps exist (ReleaseSunDetailMaps clears every override when they go).
		MID.SetScalarParameterValue(TEXT("FogMS_SunDetail"), State.SunDetailValue);
		MID.SetVectorParameterValue(TEXT("FogMS_SunMapU"), State.SunMapRowU);
		MID.SetVectorParameterValue(TEXT("FogMS_SunMapV"), State.SunMapRowV);
		MID.SetVectorParameterValue(TEXT("FogMS_SunMapW"), State.SunMapRowW);
		if (State.SunMap.IsValid()) MID.SetTextureParameterValue(TEXT("FogMS_SunMap"), State.SunMap.Get());
		if (State.SunCell.IsValid()) MID.SetTextureParameterValue(TEXT("FogMS_SunMapCell"), State.SunCell.Get());
		// P2 (M_FogMS_Density FogMS_FroxelWeight, matedit_density.py W39): 1 = the Box renders in the froxels, 0 = a cloud host
		// renders it (the froxel copy then adds no extinction, scattering or emissive to the volumetric fog).
		MID.SetScalarParameterValue(TEXT("FogMS_FroxelWeight"), State.FroxelWeightValue);
	}
	// The host always gets the true density (it has no native/overlay split).
	MID.SetScalarParameterValue(TEXT("FogMS_Density"), (bCloudHost || State.bUseNativeDensity) ? State.DensityValue : 0.0f);
	MID.SetVectorParameterValue(TEXT("FogMS_Albedo"), State.Albedo);
	MID.SetVectorParameterValue(TEXT("FogMS_WorldExtent"), State.WorldExtent);
	MID.SetScalarParameterValue(TEXT("FogMS_DensityFeather"), State.Feather);
	// Material contract: valid = Field.a >= 0.5 (cleared field: 0), Emissive = valid*Field.rgb*Albedo*sigma_t.
	// Mode 1 (full field, J, a = 1): BaseColor = Albedo*(1 - valid). Mode 2 (hybrid, J_ms, a = 0.5 + 0.5*T_sun):
	// BaseColor = Albedo*lerp(1, saturate(2*Field.a - 1), valid), native single scattering darkened by T_sun.
	// Mode 3 (debug field only, full field J, a = 1; field contract v4): BaseColor = 0 even without a valid
	// field, Emissive as mode 1. A material older than v4 treats 3 like 2 (BaseColor = Albedo with a = 1).
	MID.SetScalarParameterValue(TEXT("FogMS_InjectionMode"), State.bEmissiveInjection
		? (State.bFieldOnlyInjection ? 3.0f : (State.bHybridInjection ? 2.0f : 1.0f)) : 0.0f);
	if (State.bEmissiveInjection)
		MID.SetTextureParameterValue(TEXT("FogMS_TransportField"), TransportField);
	if (bCloudHost && DensityComponent)
	{
		// M_FogMS_Cloud FogMS_CloudBoxLocal: cube space -50..50 of this sample = rows . (sample - centre). Row i = world axis i of the
		// density cube divided by its world scale on that axis (a negative scale mirrors like TransformPosition World -> Local).
		const FTransform CubeToWorld = DensityComponent->GetComponentTransform();
		const FVector Center = CubeToWorld.GetLocation();
		const FVector Scale = CubeToWorld.GetScale3D();
		MID.SetVectorParameterValue(TEXT("FogMS_CloudBoxCenter"), FLinearColor(static_cast<float>(Center.X), static_cast<float>(Center.Y),
			static_cast<float>(Center.Z), 0.0f));
		static const FName RowNames[3] = { TEXT("FogMS_CloudWorldToLocal0"), TEXT("FogMS_CloudWorldToLocal1"), TEXT("FogMS_CloudWorldToLocal2") };
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const FVector Row = CubeToWorld.GetRotation().RotateVector(FVector(Axis == 0 ? 1.0 : 0.0, Axis == 1 ? 1.0 : 0.0, Axis == 2 ? 1.0 : 0.0)) / Scale[Axis];
			MID.SetVectorParameterValue(RowNames[Axis], FLinearColor(static_cast<float>(Row.X), static_cast<float>(Row.Y), static_cast<float>(Row.Z), 0.0f));
		}
	}
}

void AFogMSBoxVolume::GetCloudHostGeometry(FFogMSCloudBoxGeometry& Out) const
{
	Out.CubeToWorld = DensityComponent ? DensityComponent->GetComponentTransform() : FTransform::Identity;
	Out.LocalZMin = -50.0f;
	Out.LocalZMax = 50.0f;
	// Same band as the W41 sun map: with the height profile on and Threshold - Softness/2 above Detail Strength the density is exactly 0
	// outside Height Bottom..Top (the material's h = cube z * 0.01 + 0.5).
	if (bHeightProfile && HeightBottom < HeightTop && Threshold - 0.5f * Softness - DetailStrength > 1.0e-6f)
	{
		Out.LocalZMin = (FMath::Clamp(HeightBottom, 0.0f, 1.0f) - 0.5f) * 100.0f;
		Out.LocalZMax = (FMath::Clamp(HeightTop, 0.0f, 1.0f) - 0.5f) * 100.0f;
	}
}

void AFogMSBoxVolume::CreateCloudHost()
{
	FString Message;
	AActor* Host = UFogMSCloudHostSubsystem::SpawnHost(GetWorld(), this, Message);
	if (Host && RenderPath != EFogMSRenderPath::CloudHost)
	{
		Modify();
		RenderPath = EFogMSRenderPath::CloudHost;
		Message += TEXT(" Render Path of this Box set to Cloud Host.");
	}
	UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS %s: %s"), *GetActorNameOrLabel(), *Message);
	UpdateDensity();
}

bool AFogMSBoxVolume::RestoreDensityMotionReference(const FFogMSDensityMotionReference& Reference)
{
	if (!Reference.IsFinite()) return false;
	Modify();
	++MotionResetRevision;
	DensityMotionReference = Reference;
	UpdateDensity();
	return true;
}

void AFogMSBoxVolume::UseDirectionalMotion()
{
	if (DensityMotionMode == EFogMSDensityMotionMode::Directional) return;
	UpdateDensity();
	FVector Velocity0, Velocity1, Velocity2;
	const double Time = (bUseManualAnimationTime ? ManualAnimationTime : FogMS_GetDensityFrameTime(GetWorld())) + AnimationTimeOffset;
	if (!WindDirectionComponent || !GetDensityMotionVelocities(Velocity0, Velocity1, Velocity2) || !FMath::IsFinite(Time))
	{
		UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS: cannot convert invalid legacy motion on %s."), *GetPathName());
		return;
	}
	FFogMSDensityMotionReference Reference;
	Reference.bInitialized = true;
	Reference.Time = Time;
	// A disabled/unsupported legacy animation currently displays the static mapping.
	// Its explicit conversion must preserve that phase too, without enabling motion.
	if (LastDensityState.bAnimationActive)
	{
		Reference.Displacement0 = Velocity0 * Time;
		Reference.Displacement1 = Velocity1 * Time;
		Reference.Displacement2 = Velocity2 * Time;
	}
	Reference.Velocity0 = Velocity0; Reference.Velocity1 = Velocity1; Reference.Velocity2 = Velocity2;
	const double Speed = Velocity0.Size();
	if (!Reference.IsFinite() || !FMath::IsFinite(Speed))
	{
		UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS: legacy motion conversion overflow on %s."), *GetPathName());
		return;
	}
	Modify();
	WindDirectionComponent->Modify();
	if (Speed > 0.0) WindDirectionComponent->SetWorldRotation(Velocity0.Rotation());
	WindSpeed = Speed;
	DensityMotionReference = Reference;
	DensityMotionMode = EFogMSDensityMotionMode::Directional;
	UpdateDensity();
}

void AFogMSBoxVolume::UseLegacyMotion()
{
	if (DensityMotionMode == EFogMSDensityMotionMode::LegacyVectors) return;
	Modify();
	DensityMotionMode = EFogMSDensityMotionMode::LegacyVectors;
	UpdateDensity();
}

void AFogMSBoxVolume::ResetMotionOrigin()
{
	if (DensityMotionMode != EFogMSDensityMotionMode::Directional) return;
	FVector Velocity0, Velocity1, Velocity2;
	const double Time = (bUseManualAnimationTime ? ManualAnimationTime : FogMS_GetDensityFrameTime(GetWorld())) + AnimationTimeOffset;
	if (!FMath::IsFinite(Time) || !GetDensityMotionVelocities(Velocity0, Velocity1, Velocity2)) return;
	Modify();
	++MotionResetRevision;
	DensityMotionReference = FFogMSDensityMotionReference{};
	DensityMotionReference.bInitialized = true;
	DensityMotionReference.Time = Time;
	DensityMotionReference.Velocity0 = Velocity0;
	DensityMotionReference.Velocity1 = Velocity1;
	DensityMotionReference.Velocity2 = Velocity2;
	UpdateDensity();
}

void AFogMSBoxVolume::FreezeDensityAnimation()
{
	UpdateDensity();
	if (bUseManualAnimationTime) return;
	Modify();
	ManualAnimationTime = LastDensityState.bAnimationActive
		? LastDensityState.SampleTime - AnimationTimeOffset : FogMS_GetDensityFrameTime(GetWorld());
	bUseManualAnimationTime = true;
	UpdateDensity();
}

void AFogMSBoxVolume::ResumeDensityAnimation()
{
	if (!bUseManualAnimationTime) return;
	Modify();
	AnimationTimeOffset = ManualAnimationTime + AnimationTimeOffset - FogMS_GetDensityFrameTime(GetWorld());
	bUseManualAnimationTime = false;
	UpdateDensity();
}

void AFogMSBoxVolume::EnableLiveBox()
{
	if (!FogMS_IsBindlessAllConfiguration())
	{
		// No BindlessAll: never set r.FogMS.BoxMode 1 or apply the engine-shader patch (its consumers need the heap).
		// Register only the injection-only runtime; Prepare reports the overlay as unavailable in OutError by design.
		uint32 UnusedDescriptor = MAX_uint32;
		FString OverlayNote;
		if (!FFogMSBoxRuntime::Prepare(UnusedDescriptor, OverlayNote))
		{
			SpatialStatus = OverlayNote;
			UE_LOG(LogMultiLobeSpec, Error, TEXT("FogMS Box: %s"), *OverlayNote);
			return;
		}
		// The runtime overwrites this every frame; this is the immediate feedback of the button.
		SpatialStatus = UsesEmissiveInjection()
			? TEXT("Waiting for current-frame isotropic transport (injection-only: no BindlessAll; overlay features off)")
			: (FogMS_IsTransportMode(ScatteringMode)
				? TEXT("Transport needs Emissive Injection or -BindlessAll (injection-only: no BindlessAll; overlay features off). Native fog lighting.")
				: TEXT("This Scattering Mode requires -BindlessAll (injection-only: no BindlessAll; overlay features off). Native fog lighting."));
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS Box: injection-only runtime (no BindlessAll): Transport + Emissive Injection via the Box Volume material; overlay features (A1d/A1e, shadow cache, ViewIntegration, SSFS sky disk, debug views) off."));
		return;
	}
	FogMS_ApplyBoxMode(1);
}

void AFogMSBoxVolume::EnableIndirectPreview()
{
	bIndirectShadowing = true;
	// Transport modes never enable the A1c TLV attenuation (packet row 7.x = 0) and do not read the translucency volume, so
	// their preview leaves r.Lumen.TranslucencyVolume.SpatialFilter / .Temporal.Jitter at the project values (see PreviewSettings).
	FFogMSBoxRuntime::ConfigureIndirectPreview(true, !FogMS_IsTransportMode(ScatteringMode));
	EnableLiveBox();
}

void AFogMSBoxVolume::RestoreStandardLumen()
{
	bIndirectShadowing = false;
	FFogMSBoxRuntime::ConfigureIndirectPreview(false);
}

void AFogMSBoxVolume::UseGlobalA1()
{
	FogMS_ApplyBoxMode(0);
}
