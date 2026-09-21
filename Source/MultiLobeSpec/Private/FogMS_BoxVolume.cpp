#include "FogMS_BoxVolume.h"
#include "FogMS_BoxRuntime.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/VolumeTexture.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "MultiLobeSpec.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
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

	bool FogMS_MakeWorldMapping(const FVector& Center, float WorldTextureSize, float DetailScale,
		FLinearColor& OutFrequencies, FLinearColor& OutPhase0, FLinearColor& OutPhase1, FLinearColor& OutPhase2)
	{
		if (!FMath::IsFinite(WorldTextureSize) || WorldTextureSize < 1.0f || WorldTextureSize > 1.0e8f) return false;
		const float F0 = 1.0f / WorldTextureSize;
		const float F1 = F0 * DetailScale;
		const float F2 = F1 * 2.0f;
		OutFrequencies = FLinearColor(F0, F1, F2, 0.0f);
		return FogMS_IsFiniteColor(OutFrequencies) && FMath::Min3(F0, F1, F2) > 0.0f
			&& FogMS_MakeWorldPhase(Center, F0, OutPhase0)
			&& FogMS_MakeWorldPhase(Center, F1, OutPhase1)
			&& FogMS_MakeWorldPhase(Center, F2, OutPhase2);
	}

	bool FogMS_IsDensityActorVisible(const AActor& Actor, const UWorld* World)
	{
#if WITH_EDITOR
		if (World && !World->UsesGameHiddenFlags()) return !Actor.IsHiddenEd();
#endif
		return World && !Actor.IsHidden();
	}

	void FogMS_ApplyBoxMode(const int32 BoxMode)
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

void AFogMSBoxVolume::PostLoad()
{
	Super::PostLoad();
	UpdateDensity();
}

void AFogMSBoxVolume::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateDensity();
}

bool AFogMSBoxVolume::ShouldTickIfViewportsOnly() const
{
	return true;
}

#if WITH_EDITOR
void AFogMSBoxVolume::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	UpdateDensity();
}

void AFogMSBoxVolume::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	UpdateDensity();
}
#endif

bool AFogMSBoxVolume::FDensityState::HasSameMaterialParameters(const FDensityState& Other) const
{
	return Texture == Other.Texture && TextureResource == Other.TextureResource
		&& ChannelMask == Other.ChannelMask && TileScaleValue == Other.TileScaleValue
		&& bWorldAligned == Other.bWorldAligned && WorldFrequencies == Other.WorldFrequencies
		&& WorldPhase0 == Other.WorldPhase0 && WorldPhase1 == Other.WorldPhase1 && WorldPhase2 == Other.WorldPhase2
		&& ThresholdValue == Other.ThresholdValue && SoftnessValue == Other.SoftnessValue
		&& DetailStrengthValue == Other.DetailStrengthValue && DetailScaleValue == Other.DetailScaleValue
		&& DetailSecondOctaveValue == Other.DetailSecondOctaveValue
		&& DensityValue == Other.DensityValue && Albedo == Other.Albedo
		&& WorldExtent == Other.WorldExtent && Feather == Other.Feather;
}

bool AFogMSBoxVolume::FDensityState::HasSameEffect(const FDensityState& Other) const
{
	return bActive == Other.bActive && (!bActive
		|| (HasSameMaterialParameters(Other) && WorldTransform.Equals(Other.WorldTransform, 0.0)));
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
		else if (bWorldAlignedTexture && !FogMS_MakeWorldMapping(BoxComponent->GetComponentLocation(), WorldTextureSize, DetailScale,
			WorldFrequencies, WorldPhase0, WorldPhase1, WorldPhase2))
		{
			Problem = TEXT("World Texture Size must be finite in [1,100000000] cm, with finite positive frequencies and world phases.");
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
				State.bActive = true;
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
					DensityMID->SetScalarParameterValue(TEXT("FogMS_Density"), State.DensityValue);
					DensityMID->SetVectorParameterValue(TEXT("FogMS_Albedo"), State.Albedo);
					DensityMID->SetVectorParameterValue(TEXT("FogMS_WorldExtent"), State.WorldExtent);
					DensityMID->SetScalarParameterValue(TEXT("FogMS_DensityFeather"), State.Feather);
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
		LastDensityState = State;
	}
	if (Problem != LastDensityProblem)
	{
		if (!Problem.IsEmpty())
		{
			UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS density disabled on %s: %s"), *GetPathName(), *Problem);
		}
		LastDensityProblem = MoveTemp(Problem);
	}
}

void AFogMSBoxVolume::EnableLiveBox()
{
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
