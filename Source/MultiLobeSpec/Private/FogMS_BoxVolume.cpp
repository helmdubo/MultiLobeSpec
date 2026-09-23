#include "FogMS_BoxVolume.h"
#include "FogMS_BoxRuntime.h"

#include "Components/BoxComponent.h"
#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTargetVolume.h"
#include "Engine/VolumeTexture.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
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

	// Game worlds (bApplyRequiredRenderSettings): the two renderer requirements of the transport/world solver checked by
	// FogMS_WorldViewProblem and FogMS_BuildWorldLighting (engine defaults 3 and 1). ECVF_SetByGameSetting ranks above
	// constructor defaults and scalability, but below project settings, [SystemSettings]/ini, device profiles,
	// ConsoleVariables.ini, the command line and the console: an explicit project value wins and is only reported (the
	// Set is skipped so the engine logs no priority warning). Not restored at EndPlay: a game-process setting.
	void FogMS_ApplyRequiredRenderSettings(const AFogMSBoxVolume& Box)
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
				Current == Setting.Value ? TEXT("") : TEXT(" kept: higher-priority project/ini/command-line value, Transport stays off"));
		}
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS %s: Apply Required Render Settings (SetByGameSetting):%s"), *Box.GetName(), *Summary);
	}
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
	// Standalone game worlds only (packaged, -game): PIE and editor worlds keep the editor path (Enable Indirect Preview).
	// Only an enabled Box whose mode needs the world producer (Transport, World) changes these global settings.
	const UWorld* World = GetWorld();
	if (bApplyRequiredRenderSettings && bEnabled && World && World->WorldType == EWorldType::Game
		&& (FogMS_IsTransportMode(ScatteringMode) || ScatteringMode == EFogMSScatteringMode::WorldSpace))
		FogMS_ApplyRequiredRenderSettings(*this);
	// Game/PIE worlds have no Details button. An enabled Transport Box with Emissive Injection starts its runtime
	// itself: injection-only registers the view extension only; with BindlessAll (editor PIE) this is the button's path.
	if (bEnabled && UsesEmissiveInjection()) EnableLiveBox();
}

void AFogMSBoxVolume::AutoStartRuntime()
{
	// Injection-only configuration (no BindlessAll): starting the runtime changes no global state (no BoxMode cvar,
	// no engine-shader patch), so an enabled injection Box does it once on its first tick, also in the editor world.
	// With BindlessAll the editor keeps the explicit Enable Live Box step (it applies the overlay patch).
	if (bRuntimeAutoStarted || !bEnabled || !UsesEmissiveInjection() || FogMS_IsBindlessAllConfiguration()) return;
	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || !GetWorld() || GetWorld()->IsPreviewWorld()) return;
	bRuntimeAutoStarted = true;
	EnableLiveBox();
}

void AFogMSBoxVolume::Destroyed()
{
	ReleaseTransportField();
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

void AFogMSBoxVolume::ApplyTransportPreset()
{
	// BoxRuntime packs these three fields; a non-Custom preset owns them. Idempotent.
	FFogMSTransportTier Tier;
	if (!FogMS_GetTransportTier(TransportPreset, Tier)) return;
	AngularQuality = Tier.AngularQuality;
	TransportIterations = Tier.Iterations;
	TransportTolerance = Tier.Tolerance;
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

bool AFogMSBoxVolume::GetDensityMotionVelocities(FVector& Out0, FVector& Out1, FVector& Out2) const
{
	if (DensityMotionMode == EFogMSDensityMotionMode::LegacyVectors)
		Out0 = DensityWindVelocity;
	else if (DensityMotionMode == EFogMSDensityMotionMode::Directional)
	{
		if (!WindDirectionComponent || !WindDirectionComponent->GetComponentTransform().IsValid()
			|| !FMath::IsFinite(WindSpeed) || WindSpeed < 0.0
			|| !FMath::IsFinite(EdgeFlowSpeed) || EdgeFlowSpeed < 0.0) return false;
		Out0 = WindDirectionComponent->GetForwardVector() * WindSpeed;
	}
	else return false;
	Out1 = Out0 + DensityDetailVelocity;
	Out2 = Out1 + DensityEvolutionVelocity;
	// Preserve the original operations exactly for zero flow and Legacy mode.
	// The automatic offsets are relative to the common wind, not cumulative:
	// octave two uses half the world speed at twice the frequency.
	if (DensityMotionMode == EFogMSDensityMotionMode::Directional && EdgeFlowSpeed > 0.0)
	{
		Out1 += WindDirectionComponent->GetRightVector() * EdgeFlowSpeed;
		Out2 -= WindDirectionComponent->GetUpVector() * (0.5 * EdgeFlowSpeed);
	}
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
		&& InjectionField == Other.InjectionField
		&& Texture == Other.Texture && TextureResource == Other.TextureResource
		&& ChannelMask == Other.ChannelMask && TileScaleValue == Other.TileScaleValue
		&& bWorldAligned == Other.bWorldAligned && WorldFrequencies == Other.WorldFrequencies
		&& ThresholdValue == Other.ThresholdValue && SoftnessValue == Other.SoftnessValue
		&& DetailStrengthValue == Other.DetailStrengthValue && DetailScaleValue == Other.DetailScaleValue
		&& DetailSecondOctaveValue == Other.DetailSecondOctaveValue
		&& DensityValue == Other.DensityValue && Albedo == Other.Albedo
		&& WorldExtent == Other.WorldExtent && Feather == Other.Feather && TextureOffset == Other.TextureOffset;
}

bool AFogMSBoxVolume::FDensityState::HasSameMaterialParameters(const FDensityState& Other) const
{
	return HasSameDensityParameters(Other)
		&& WorldPhase0 == Other.WorldPhase0 && WorldPhase1 == Other.WorldPhase1 && WorldPhase2 == Other.WorldPhase2;
}

bool AFogMSBoxVolume::FDensityState::HasSameEffect(const FDensityState& Other) const
{
	if (bActive != Other.bActive) return false;
	if (!bActive) return true;
	if (!HasSameDensityParameters(Other) || !WorldTransform.Equals(Other.WorldTransform, 0.0)
		|| bAnimationActive != Other.bAnimationActive) return false;
	if (!bAnimationActive) return HasSameMaterialParameters(Other);
	// Continuous phase progression updates the MID and shadow cache, not the global
	// fog-history revision. Authored edits, explicit seeks and backwards world time do.
	return bManualAnimationTime == Other.bManualAnimationTime
		&& MotionMode == Other.MotionMode
		&& (MotionMode != EFogMSDensityMotionMode::Directional || MotionReference.Equals(Other.MotionReference))
		&& WindVelocity == Other.WindVelocity && DetailVelocity == Other.DetailVelocity
		&& EvolutionVelocity == Other.EvolutionVelocity && TimeOffset == Other.TimeOffset
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
	// OnConstruction, PostLoad, PostEditChangeProperty, Tick and BoxRuntime (right before it
	// packs the transport controls) all pass here: the packet always sees the preset's values,
	// also after a Blueprint/runtime write that bypassed PostEditChangeProperty.
	ApplyTransportPreset();
	// Mode or switch change away from Emissive Injection: drop the field and its MID binding.
	if (TransportField && !UsesEmissiveInjection()) ReleaseTransportField();
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
					else if (!GetDensityMotionVelocities(MotionVelocity0, MotionVelocity1, MotionVelocity2)
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
				// Hybrid: native fog keeps single scattering; the field carries J_ms and T_sun (packet row 23.w = 6).
				State.bHybridInjection = bInjection && bHybridSingleScattering;
				State.InjectionField = bInjection ? TransportField.Get() : nullptr;
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
					DensityMID->SetTextureParameterValue(TEXT("FogMS_Noise"), DensityTexture);
					DensityMID->SetVectorParameterValue(TEXT("FogMS_ChannelMask"), State.ChannelMask);
					DensityMID->SetVectorParameterValue(TEXT("FogMS_TileScale"), State.TileScaleValue);
					DensityMID->SetScalarParameterValue(TEXT("FogMS_WorldAligned"), State.bWorldAligned ? 1.0f : 0.0f);
					DensityMID->SetVectorParameterValue(TEXT("FogMS_WorldFrequencies"), State.WorldFrequencies);
					DensityMID->SetVectorParameterValue(TEXT("FogMS_WorldPhase0"), State.WorldPhase0);
					DensityMID->SetVectorParameterValue(TEXT("FogMS_WorldPhase1"), State.WorldPhase1);
					DensityMID->SetVectorParameterValue(TEXT("FogMS_WorldPhase2"), State.WorldPhase2);
					DensityMID->SetScalarParameterValue(TEXT("FogMS_Threshold"), State.ThresholdValue);
					DensityMID->SetScalarParameterValue(TEXT("FogMS_Softness"), State.SoftnessValue);
					DensityMID->SetScalarParameterValue(TEXT("FogMS_DetailStrength"), State.DetailStrengthValue);
					DensityMID->SetScalarParameterValue(TEXT("FogMS_DetailScale"), State.DetailScaleValue);
					DensityMID->SetScalarParameterValue(TEXT("FogMS_DetailSecondOctave"), State.DetailSecondOctaveValue);
					DensityMID->SetScalarParameterValue(TEXT("FogMS_Density"), State.bUseNativeDensity ? State.DensityValue : 0.0f);
					DensityMID->SetVectorParameterValue(TEXT("FogMS_Albedo"), State.Albedo);
					DensityMID->SetVectorParameterValue(TEXT("FogMS_WorldExtent"), State.WorldExtent);
					DensityMID->SetScalarParameterValue(TEXT("FogMS_DensityFeather"), State.Feather);
					// Material contract: valid = Field.a >= 0.5 (cleared field: 0), Emissive = valid*Field.rgb*Albedo*sigma_t.
					// Mode 1 (full field, J, a = 1): BaseColor = Albedo*(1 - valid). Mode 2 (hybrid, J_ms, a = 0.5 + 0.5*T_sun):
					// BaseColor = Albedo*lerp(1, saturate(2*Field.a - 1), valid), native single scattering darkened by T_sun.
					DensityMID->SetScalarParameterValue(TEXT("FogMS_InjectionMode"),
						State.bEmissiveInjection ? (State.bHybridInjection ? 2.0f : 1.0f) : 0.0f);
					if (State.bEmissiveInjection)
						DensityMID->SetTextureParameterValue(TEXT("FogMS_TransportField"), TransportField);
					LastMaterialState = State;
					bHasMaterialState = true;
				}
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

bool AFogMSBoxVolume::RestoreDensityMotionReference(const FFogMSDensityMotionReference& Reference)
{
	if (!Reference.IsFinite()) return false;
	Modify();
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
	FFogMSBoxRuntime::ConfigureIndirectPreview(true);
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
