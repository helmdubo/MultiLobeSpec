#include "FogMS_CloudHost.h"
#include "FogMS_BoxVolume.h"

#include "Components/ActorComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MultiLobeSpec.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#if WITH_EDITORONLY_DATA
#include "Materials/MaterialExpressionVolumetricAdvancedMaterialOutput.h"
#endif

namespace
{
	const TCHAR* const FogMS_CloudMaterialPath = TEXT("/MultiLobeSpec/FogMS/M_FogMS_Cloud.M_FogMS_Cloud");
	const TCHAR* const FogMS_CloudInstancePath = TEXT("/MultiLobeSpec/FogMS/MI_FogMS_Cloud.MI_FogMS_Cloud");
	const TCHAR* const FogMS_CloudHostLabel = TEXT("FogMS Cloud Host");

	TAutoConsoleVariable<int32> CVarCloudHostStepSettings(TEXT("r.FogMS.CloudHost.StepSettings"), 1,
		TEXT("While a FogMS Box renders through a cloud host (Render Path = Cloud Host): 1 (default) sets r.VolumetricCloud.DistanceToSampleMaxCount ")
		TEXT("to the host's Tracing Max Distance (km) and r.VolumetricCloud.SampleMinCount to 8 at game-setting priority, so the host marches with ")
		TEXT("a uniform step = distance / samples (2 km / 768 = 2.6 m with the 'Create Cloud Host' settings) instead of the engine's 15 km / samples; ")
		TEXT("the previous values come back when no Box uses a host. A project/ini/console value has priority and is kept (the Box status reports it). ")
		TEXT("0 = leave both cvars to the project."), ECVF_Default);

	// P1 host settings (round 39, FogMS_Prod_Report.md 'Раунд 39'): trace 2 km from the camera, view samples x8 (768), sun march 0.25 km
	// with 32 samples, stop at transmittance 0.005, layer = the Box's density band +-10 m (at least 0.1 km).
	constexpr float FogMS_HostTraceKm = 2.0f;
	constexpr float FogMS_HostViewSampleScale = 8.0f;
	constexpr float FogMS_HostShadowSampleScale = 3.2f;
	constexpr float FogMS_HostShadowTraceKm = 0.25f;
	constexpr float FogMS_HostStopTransmittance = 0.005f;
	constexpr double FogMS_HostLayerMarginCm = 1000.0;
	constexpr int32 FogMS_HostSampleMinCount = 8;
	/** Layer check tolerance [cm]: a host fitted by SpawnHost keeps 10 m of margin; this only absorbs float rounding. */
	constexpr double FogMS_LayerToleranceCm = 50.0;

	/** The engine's condition for adding a cloud proxy to the scene (UVolumetricCloudComponent::CreateRenderState_Concurrent). */
	bool FogMS_CloudRenders(const UVolumetricCloudComponent* Component)
	{
		return Component && Component->IsRegistered() && Component->IsRenderStateCreated() && Component->GetVisibleFlag()
			&& Component->ShouldComponentAddToScene() && Component->ShouldRender();
	}

	UMaterial* FogMS_BaseMaterial(const UVolumetricCloudComponent* Component)
	{
		UMaterialInterface* Material = Component ? Component->Material.Get() : nullptr;
		return Material ? Material->GetMaterial() : nullptr;
	}

	FString FogMS_Label(const UVolumetricCloudComponent* Component)
	{
		return Component && Component->GetOwner() ? Component->GetOwner()->GetActorNameOrLabel() : FString(TEXT("?"));
	}

	/** Planet centre [cm] and ground radius [cm] the cloud layer is measured from (VolumetricCloudRendering.cpp: the SkyAtmosphere's
	 * planet when there is one, else the cloud's own Planet Radius with the planet top at the world origin; transform modes as
	 * SkyAtmosphereCommonData.cpp). FallbackRadiusKm = the cloud's Planet Radius. */
	void FogMS_CloudPlanet(const UWorld* World, double FallbackRadiusKm, FVector& OutCenter, double& OutRadius)
	{
		const USkyAtmosphereComponent* Sky = nullptr;
		ForEachObjectOfClass(USkyAtmosphereComponent::StaticClass(), [&Sky, World](UObject* Object)
		{
			const USkyAtmosphereComponent* Candidate = static_cast<const USkyAtmosphereComponent*>(Object);
			if (!Sky && Candidate->GetWorld() == World && Candidate->IsRegistered() && Candidate->ShouldRender()) Sky = Candidate;
		}, true, RF_ClassDefaultObject | RF_ArchetypeObject, EInternalObjectFlags::Garbage);
		if (Sky)
		{
			OutRadius = static_cast<double>(Sky->BottomRadius) * 1.0e5;
			const FVector Translation = Sky->GetComponentTransform().GetTranslation();
			switch (Sky->TransformMode)
			{
			case ESkyAtmosphereTransformMode::PlanetTopAtAbsoluteWorldOrigin: OutCenter = FVector(0.0, 0.0, -OutRadius); break;
			case ESkyAtmosphereTransformMode::PlanetTopAtComponentTransform: OutCenter = FVector(0.0, 0.0, -OutRadius) + Translation; break;
			default: OutCenter = Translation; break;
			}
			return;
		}
		OutRadius = FallbackRadiusKm * 1.0e5;
		OutCenter = FVector(0.0, 0.0, -OutRadius);
	}

	/** Altitude range [cm above the ground] of the Box's density band: the farthest point from the planet centre is a corner, the
	 * nearest is the centre clamped into the band box in cube space (the cube's axes are orthogonal: rotation x scale, no shear). */
	void FogMS_BandAltitudes(const FFogMSCloudBoxGeometry& Geometry, const FVector& Center, double Radius, double& OutMin, double& OutMax)
	{
		const FTransform& T = Geometry.CubeToWorld;
		OutMax = TNumericLimits<double>::Lowest();
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector Local((Corner & 1) ? 50.0 : -50.0, (Corner & 2) ? 50.0 : -50.0, (Corner & 4) ? Geometry.LocalZMax : Geometry.LocalZMin);
			OutMax = FMath::Max(OutMax, (T.TransformPosition(Local) - Center).Size() - Radius);
		}
		FVector Nearest = T.InverseTransformPosition(Center);
		Nearest.X = FMath::Clamp(Nearest.X, -50.0, 50.0);
		Nearest.Y = FMath::Clamp(Nearest.Y, -50.0, 50.0);
		Nearest.Z = FMath::Clamp(Nearest.Z, static_cast<double>(Geometry.LocalZMin), static_cast<double>(Geometry.LocalZMax));
		OutMin = (T.TransformPosition(Nearest) - Center).Size() - Radius;
	}

	IConsoleVariable* FogMS_FindCVar(const TCHAR* Name)
	{
		return IConsoleManager::Get().FindConsoleVariable(Name);
	}

	/** Game-setting priority write that never overrides an explicit (project, ini, device profile, command line, console) value. */
	bool FogMS_SetGameSetting(IConsoleVariable* Variable, float Value)
	{
		if (!Variable) return false;
		if (Variable->GetFloat() == Value) return true;
		const EConsoleVariableFlags SetBy = static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask);
		if (SetBy > ECVF_SetByGameSetting) return false;
		Variable->Set(*FString::SanitizeFloat(Value), ECVF_SetByGameSetting);
		return Variable->GetFloat() == Value;
	}

	FString FogMS_SetByName(const IConsoleVariable* Variable)
	{
		return Variable ? FString(GetConsoleVariableSetByName(static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask))) : FString();
	}

	/** Status note on the step settings the host actually runs with (after ApplyStepSettings). */
	FString FogMS_StepNote(const UVolumetricCloudComponent& Host)
	{
		IConsoleVariable* Distance = FogMS_FindCVar(TEXT("r.VolumetricCloud.DistanceToSampleMaxCount"));
		IConsoleVariable* MaxSamples = FogMS_FindCVar(TEXT("r.VolumetricCloud.ViewRaySampleMaxCount"));
		const float Samples = FMath::Min(96.0f * FMath::Max(Host.ViewSampleCountScale, 0.05f), MaxSamples ? MaxSamples->GetFloat() : 768.0f);
		const float DistanceKm = Distance ? Distance->GetFloat() : 15.0f;
		// VolumetricCloud.usf: StepCount = max(SampleMinCount, Samples * saturate(TraceLength / DistanceToSampleMaxCount)), so rays up to
		// DistanceToSampleMaxCount march with DistanceToSampleMaxCount / Samples and longer ones (up to Tracing Max Distance) coarser.
		const float StepM = FMath::Max(DistanceKm, Host.TracingMaxDistance) * 1000.0f / FMath::Max(Samples, 1.0f);
		FString Note = FString::Printf(TEXT("step ~%.1f m, trace %.1f km"), StepM, Host.TracingMaxDistance);
		if (CVarCloudHostStepSettings.GetValueOnGameThread() != 0 && Distance && FMath::Abs(DistanceKm - FMath::Max(Host.TracingMaxDistance, 0.1f)) > 1.0e-4f)
			Note += FString::Printf(TEXT(", r.VolumetricCloud.DistanceToSampleMaxCount %g kept (%s)"), DistanceKm, *FogMS_SetByName(Distance));
		return Note;
	}
}

UMaterial* UFogMSCloudHostSubsystem::GetCloudHostMaterial()
{
	static TWeakObjectPtr<UMaterial> Cached;
	static double LastAttempt = -1.0e9;
	if (UMaterial* Material = Cached.Get()) return Material;
	UMaterial* Material = FindObject<UMaterial>(nullptr, FogMS_CloudMaterialPath);
	if (!Material)
	{
		// A missing asset (not installed, or not cooked because no level references it) is retried at most once a second, quietly.
		const double Now = FPlatformTime::Seconds();
		if (Now - LastAttempt < 1.0) return nullptr;
		LastAttempt = Now;
		Material = LoadObject<UMaterial>(nullptr, FogMS_CloudMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	Cached = Material;
	return Material;
}

void UFogMSCloudHostSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	MarkDirtyHandle = UActorComponent::MarkRenderStateDirtyEvent.AddUObject(this, &UFogMSCloudHostSubsystem::OnMarkRenderStateDirty);
#if WITH_EDITOR
	PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(this, &UFogMSCloudHostSubsystem::OnObjectPropertyChanged);
#endif
}

void UFogMSCloudHostSubsystem::Deinitialize()
{
	UActorComponent::MarkRenderStateDirtyEvent.Remove(MarkDirtyHandle);
#if WITH_EDITOR
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);
#endif
	RestoreStepSettings();
	Hosts.Reset();
	Super::Deinitialize();
}

bool UFogMSCloudHostSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::Editor;
}

TStatId UFogMSCloudHostSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFogMSCloudHostSubsystem, STATGROUP_Tickables);
}

void UFogMSCloudHostSubsystem::Scan()
{
	if (ScanFrame == GFrameCounter) return;
	ScanFrame = GFrameCounter;
	ScanHosts.Reset();
	ScanOthers.Reset();
	const UWorld* World = GetWorld();
	UMaterial* HostMaterial = GetCloudHostMaterial();
	ForEachObjectOfClass(UVolumetricCloudComponent::StaticClass(), [this, World, HostMaterial](UObject* Object)
	{
		UVolumetricCloudComponent* Component = static_cast<UVolumetricCloudComponent*>(Object);
		if (Component->GetWorld() != World || !FogMS_CloudRenders(Component)) return;
		(HostMaterial && FogMS_BaseMaterial(Component) == HostMaterial ? ScanHosts : ScanOthers).Add(Component);
	}, true, RF_ClassDefaultObject | RF_ArchetypeObject, EInternalObjectFlags::Garbage);
	const auto ByPath = [](const TWeakObjectPtr<UVolumetricCloudComponent>& A, const TWeakObjectPtr<UVolumetricCloudComponent>& B)
	{
		return A.Get() && B.Get() && A->GetPathName() < B->GetPathName();
	};
	ScanHosts.Sort(ByPath);
	ScanOthers.Sort(ByPath);
}

UFogMSCloudHostSubsystem::FHost* UFogMSCloudHostSubsystem::FindBinding(const UVolumetricCloudComponent* Component)
{
	for (FHost& Host : Hosts)
		if (Host.Component.Get() == Component) return &Host;
	return nullptr;
}

FString UFogMSCloudHostSubsystem::ValidateHostMaterial(const UVolumetricCloudComponent& Component) const
{
	const UMaterial* Base = FogMS_BaseMaterial(&Component);
	if (!Base) return TEXT("the host has no material");
	if (Base->MaterialDomain != MD_Volume) return TEXT("host material is not in the Volume domain");
	// VolumetricCloud.usf: an Unlit cloud material gets no albedo and no extinction.
	if (Base->GetShadingModels().IsUnlit()) return TEXT("host material is Unlit (the cloud would have no albedo and no extinction)");
	if (!Base->GetUsageByFlag(MATUSAGE_VolumetricCloud)) return TEXT("host material lacks 'Used with Volumetric Cloud'");
#if WITH_EDITORONLY_DATA
	// VolumetricCloud.usf adds Emissive once per multiple-scattering octave: with octaves the solver's field would be counted N + 1
	// times. The cooked game has no expressions; the editor check covers the authored asset.
	for (const TObjectPtr<UMaterialExpression>& Expression : Base->GetExpressions())
	{
		const UMaterialExpressionVolumetricAdvancedMaterialOutput* Output = Cast<UMaterialExpressionVolumetricAdvancedMaterialOutput>(Expression.Get());
		if (Output && Output->MultiScatteringApproximationOctaveCount != 0)
			return FString::Printf(TEXT("host material has %u multiple-scattering octaves (must be 0, else the field's light is added once per octave)"),
				Output->MultiScatteringApproximationOctaveCount);
	}
#endif
	return FString();
}

FString UFogMSCloudHostSubsystem::CheckLayer(const UVolumetricCloudComponent& Component, const FFogMSCloudBoxGeometry& Geometry) const
{
	FVector Center;
	double Radius = 0.0;
	FogMS_CloudPlanet(GetWorld(), Component.PlanetRadius, Center, Radius);
	double AltMin = 0.0, AltMax = 0.0;
	FogMS_BandAltitudes(Geometry, Center, Radius, AltMin, AltMax);
	const double Bottom = static_cast<double>(Component.LayerBottomAltitude) * 1.0e5;
	const double Top = Bottom + static_cast<double>(FMath::Max(Component.LayerHeight, 0.1f)) * 1.0e5;
	if (AltMin < Bottom - FogMS_LayerToleranceCm || AltMax > Top + FogMS_LayerToleranceCm)
		return FString::Printf(TEXT("layer %.3f-%.3f km does not cover the Box's density band %.3f-%.3f km above the ground"),
			Bottom * 1.0e-5, Top * 1.0e-5, AltMin * 1.0e-5, AltMax * 1.0e-5);
	return FString();
}

FFogMSCloudHostBinding UFogMSCloudHostSubsystem::AcquireHost(const AFogMSBoxVolume& Box, const FFogMSCloudBoxGeometry& Geometry)
{
	FFogMSCloudHostBinding Result;
	const auto Fallback = [&Result](const FString& Problem)
	{
		Result.MID = nullptr;
		Result.Status = FString::Printf(TEXT(" [cloud host: %s, froxel fallback]"), *Problem);
		return Result;
	};
	if (!GetCloudHostMaterial()) return Fallback(TEXT("none (M_FogMS_Cloud is missing: run matedit_cloud.py)"));
	Scan();
	if (ScanHosts.IsEmpty()) return Fallback(TEXT("none"));
	// The host this Box already uses, else the first host no other Box fed in the last frame (P2: one Box per host).
	UVolumetricCloudComponent* Chosen = nullptr;
	const AFogMSBoxVolume* Busy = nullptr;
	for (const TWeakObjectPtr<UVolumetricCloudComponent>& Weak : ScanHosts)
	{
		UVolumetricCloudComponent* Component = Weak.Get();
		if (!Component) continue;
		const FHost* Binding = FindBinding(Component);
		const AFogMSBoxVolume* Owner = Binding ? Binding->Owner.Get() : nullptr;
		if (Owner == &Box) { Chosen = Component; break; }
		const bool bOwnerActive = Owner && Binding->FedFrame + 1 >= GFrameCounter;
		if (!bOwnerActive) { if (!Chosen) Chosen = Component; }
		else if (!Busy) Busy = Owner;
	}
	if (!Chosen)
		return Fallback(Busy ? FString::Printf(TEXT("busy with Box '%s' (one Box per host in this version)"), *Busy->GetActorNameOrLabel())
			: FString(TEXT("none")));
	const FString MaterialProblem = ValidateHostMaterial(*Chosen);
	if (!MaterialProblem.IsEmpty()) return Fallback(FString::Printf(TEXT("'%s': %s"), *FogMS_Label(Chosen), *MaterialProblem));
	const FString LayerProblem = CheckLayer(*Chosen, Geometry);
	if (!LayerProblem.IsEmpty()) return Fallback(FString::Printf(TEXT("'%s': %s"), *FogMS_Label(Chosen), *LayerProblem));

	// The host's MID: its material when that is already a MID (ours, also one saved with the level), else a new MID of it with the
	// cloud component as outer. SetMaterial re-creates the component's render state, which puts the host on top of the scene's
	// cloud stack.
	UMaterialInterface* Current = Chosen->Material.Get();
	UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Current);
	if (!MID)
	{
		MID = UMaterialInstanceDynamic::Create(Current, Chosen,
			MakeUniqueObjectName(Chosen, UMaterialInstanceDynamic::StaticClass(), TEXT("MID_FogMS_CloudHost")));
		if (MID) Chosen->SetMaterial(MID);
		if (!MID || Chosen->Material.Get() != MID)
			return Fallback(FString::Printf(TEXT("'%s': cannot assign a dynamic material (a Static-mobility host in a game world?)"), *FogMS_Label(Chosen)));
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s': material %s wrapped in %s for Box '%s'."), *FogMS_Label(Chosen),
			*GetNameSafe(Current), *MID->GetName(), *Box.GetActorNameOrLabel());
	}
	FHost* Host = FindBinding(Chosen);
	if (!Host)
	{
		Host = &Hosts.AddDefaulted_GetRef();
		Host->Component = Chosen;
	}
	if (!Host->bBound || Host->Owner.Get() != &Box || Host->MID.Get() != MID)
	{
		// New binding: claim the render from any other rendering cloud on the next tick; one log line.
		if (!ScanOthers.IsEmpty()) bOthersDirty = true;
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s' renders Box '%s' (Render Path = Cloud Host); the Box's froxel copy is off (FogMS_FroxelWeight 0)."),
			*FogMS_Label(Chosen), *Box.GetActorNameOrLabel());
	}
	Host->MID = MID;
	Host->Owner = &Box;
	Host->FedFrame = GFrameCounter;
	Host->bBound = true;
	Result.MID = MID;
	FString Displaced;
	for (const TWeakObjectPtr<UVolumetricCloudComponent>& Other : ScanOthers)
		if (Other.IsValid()) Displaced += (Displaced.IsEmpty() ? TEXT("'") : TEXT(", '")) + FogMS_Label(Other.Get()) + TEXT("'");
	Result.Status = FString::Printf(TEXT(" [render: cloud host] [cloud host '%s': %s%s]"), *FogMS_Label(Chosen), *FogMS_StepNote(*Chosen),
		Displaced.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; displaces Volumetric Cloud %s (one cloud renders per scene)"), *Displaced));
	return Result;
}

void UFogMSCloudHostSubsystem::EmptyHost(FHost& Host, const TCHAR* Reason)
{
	// FogMS_Density 0: the extinction is 0 and the conservative density 0, so the cloud has nothing to march (M_FogMS_Cloud).
	if (UMaterialInstanceDynamic* MID = Host.MID.Get())
	{
		MID->SetScalarParameterValue(TEXT("FogMS_Density"), 0.0f);
		MID->SetScalarParameterValue(TEXT("FogMS_InjectionMode"), 0.0f);
	}
	if (Host.bBound)
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s' is empty: %s."), *FogMS_Label(Host.Component.Get()), Reason);
	Host.Owner.Reset();
	Host.bBound = false;
}

void UFogMSCloudHostSubsystem::ReleaseBox(const AFogMSBoxVolume& Box)
{
	for (FHost& Host : Hosts)
		if (Host.bBound && Host.Owner.Get() == &Box) EmptyHost(Host, TEXT("its Box no longer renders through it"));
}

void UFogMSCloudHostSubsystem::Tick(float DeltaTime)
{
	const UVolumetricCloudComponent* StepHost = nullptr;
	for (int32 Index = Hosts.Num() - 1; Index >= 0; --Index)
	{
		FHost& Host = Hosts[Index];
		if (!Host.Component.IsValid())
		{
			Hosts.RemoveAtSwap(Index);
			continue;
		}
		// A Box that stopped feeding without ReleaseBox (destroyed, hidden, density invalid): one frame of slack for tick order.
		if (Host.bBound && (!Host.Owner.IsValid() || Host.FedFrame + 1 < GFrameCounter))
			EmptyHost(Host, Host.Owner.IsValid() ? TEXT("its Box stopped using it") : TEXT("its Box is gone"));
		if (Host.bBound) StepHost = Host.Component.Get();
	}
	if (!StepHost)
	{
		ClaimedOver.Reset();
		bOthersDirty = false;
		RestoreStepSettings();
		return;
	}
	// Displacement: FScene renders the most recently added cloud. When the set of other rendering clouds changes, or one of them was
	// re-created (MarkRenderStateDirty, an editor property edit), re-create the bound hosts' render state now, so they are on top again
	// from this frame's end-of-frame update on.
	Scan();
	bool bChanged = ScanOthers.Num() != ClaimedOver.Num();
	for (int32 Index = 0; !bChanged && Index < ScanOthers.Num(); ++Index) bChanged = ScanOthers[Index] != ClaimedOver[Index];
	if (bOthersDirty.exchange(false) || bChanged)
	{
		if (!ScanOthers.IsEmpty())
		{
			for (FHost& Host : Hosts)
			{
				if (!Host.bBound) continue;
				Host.Component->MarkRenderStateDirty();
				UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host '%s' takes the render over %d other Volumetric Cloud(s), e.g. '%s' (one cloud renders per scene)."),
					*FogMS_Label(Host.Component.Get()), ScanOthers.Num(), *FogMS_Label(ScanOthers[0].Get()));
			}
		}
		ClaimedOver = ScanOthers;
	}
	if (CVarCloudHostStepSettings.GetValueOnGameThread() != 0) ApplyStepSettings(StepHost);
	else RestoreStepSettings();
}

void UFogMSCloudHostSubsystem::ApplyStepSettings(const UVolumetricCloudComponent* Host)
{
	IConsoleVariable* Distance = FogMS_FindCVar(TEXT("r.VolumetricCloud.DistanceToSampleMaxCount"));
	IConsoleVariable* MinCount = FogMS_FindCVar(TEXT("r.VolumetricCloud.SampleMinCount"));
	if (!Host || !Distance || !MinCount) return;
	const float WantKm = FMath::Max(Host->TracingMaxDistance, 0.1f);
	if (!bStepSettingsApplied)
	{
		PreviousDistanceToSampleMaxCount = Distance->GetFloat();
		PreviousSampleMinCount = MinCount->GetInt();
		bStepSettingsApplied = true;
		const bool bDistance = FogMS_SetGameSetting(Distance, WantKm);
		const bool bMin = FogMS_SetGameSetting(MinCount, static_cast<float>(FogMS_HostSampleMinCount));
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host step settings (SetByGameSetting, while a Box renders through a host): r.VolumetricCloud.DistanceToSampleMaxCount %g -> %g%s; r.VolumetricCloud.SampleMinCount %d -> %d%s."),
			PreviousDistanceToSampleMaxCount, Distance->GetFloat(), bDistance ? TEXT("") : *FString::Printf(TEXT(" (kept: %s)"), *FogMS_SetByName(Distance)),
			PreviousSampleMinCount, MinCount->GetInt(), bMin ? TEXT("") : *FString::Printf(TEXT(" (kept: %s)"), *FogMS_SetByName(MinCount)));
		return;
	}
	FogMS_SetGameSetting(Distance, WantKm);   // follows edits of the host's Tracing Max Distance
}

void UFogMSCloudHostSubsystem::RestoreStepSettings()
{
	if (!bStepSettingsApplied) return;
	bStepSettingsApplied = false;
	IConsoleVariable* Distance = FogMS_FindCVar(TEXT("r.VolumetricCloud.DistanceToSampleMaxCount"));
	IConsoleVariable* MinCount = FogMS_FindCVar(TEXT("r.VolumetricCloud.SampleMinCount"));
	// Only values still at game-setting priority are ours to put back; an explicit value set meanwhile stays.
	const auto Ours = [](IConsoleVariable* Variable)
	{
		return Variable && static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask) == ECVF_SetByGameSetting;
	};
	if (Ours(Distance)) Distance->Set(*FString::SanitizeFloat(PreviousDistanceToSampleMaxCount), ECVF_SetByGameSetting);
	if (Ours(MinCount)) MinCount->Set(PreviousSampleMinCount, ECVF_SetByGameSetting);
	UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS cloud host step settings restored: r.VolumetricCloud.DistanceToSampleMaxCount %g, r.VolumetricCloud.SampleMinCount %d."),
		Distance ? Distance->GetFloat() : -1.0f, MinCount ? MinCount->GetInt() : -1);
}

void UFogMSCloudHostSubsystem::OnMarkRenderStateDirty(UActorComponent& Component)
{
	// Called for every component of every world: cheapest test first. Our own hosts are skipped (their re-creation is the claim).
	if (!Component.IsA<UVolumetricCloudComponent>() || Component.GetWorld() != GetWorld()) return;
	for (const FHost& Host : Hosts)
		if (Host.Component.Get() == &Component) return;
	bOthersDirty = true;
}

#if WITH_EDITOR
void UFogMSCloudHostSubsystem::OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
	// Details edits re-register the component (FComponentReregisterContext): no MarkRenderStateDirty, but the edited cloud goes on top.
	UVolumetricCloudComponent* Component = Cast<UVolumetricCloudComponent>(Object);
	if (!Component)
		if (const AVolumetricCloud* Actor = Cast<AVolumetricCloud>(Object)) Component = Actor->FindComponentByClass<UVolumetricCloudComponent>();
	if (!Component || Component->GetWorld() != GetWorld()) return;
	for (const FHost& Host : Hosts)
		if (Host.Component.Get() == Component) return;
	bOthersDirty = true;
}
#endif

AActor* UFogMSCloudHostSubsystem::SpawnHost(UWorld* World, const AFogMSBoxVolume* Box, FString& OutMessage)
{
	if (!World)
	{
		OutMessage = TEXT("no world: nothing spawned");
		return nullptr;
	}
	UMaterial* HostMaterial = GetCloudHostMaterial();
	UMaterialInterface* Instance = LoadObject<UMaterialInterface>(nullptr, FogMS_CloudInstancePath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!HostMaterial || !Instance || Instance->GetMaterial() != HostMaterial)
	{
		OutMessage = TEXT("M_FogMS_Cloud / MI_FogMS_Cloud are missing or MI_FogMS_Cloud is not an instance of M_FogMS_Cloud (run matedit_cloud.py in the editor): nothing spawned");
		return nullptr;
	}
	// One host per level is enough for P2: an existing one (visible or not) is reused.
	AActor* Existing = nullptr;
	FString Others;
	ForEachObjectOfClass(UVolumetricCloudComponent::StaticClass(), [&](UObject* Object)
	{
		const UVolumetricCloudComponent* Component = static_cast<const UVolumetricCloudComponent*>(Object);
		if (Component->GetWorld() != World || !Component->IsRegistered()) return;
		if (FogMS_BaseMaterial(Component) == HostMaterial) { if (!Existing) Existing = Component->GetOwner(); }
		else if (FogMS_CloudRenders(Component)) Others += (Others.IsEmpty() ? TEXT("'") : TEXT(", '")) + FogMS_Label(Component) + TEXT("'");
	}, true, RF_ClassDefaultObject | RF_ArchetypeObject, EInternalObjectFlags::Garbage);
	if (Existing)
	{
		OutMessage = FString::Printf(TEXT("cloud host '%s' already exists: nothing spawned"), *Existing->GetActorNameOrLabel());
		return Existing;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
#if WITH_EDITOR
	Params.ObjectFlags |= RF_Transactional;
#endif
	AVolumetricCloud* Host = World->SpawnActor<AVolumetricCloud>(AVolumetricCloud::StaticClass(), FTransform::Identity, Params);
	UVolumetricCloudComponent* Component = Host ? Host->FindComponentByClass<UVolumetricCloudComponent>() : nullptr;
	if (!Component)
	{
		OutMessage = TEXT("spawning the Volumetric Cloud actor failed");
		return Host;
	}
#if WITH_EDITOR
	Host->SetActorLabel(FogMS_CloudHostLabel);
#endif
	// Layer = the Box's density band +-10 m (at least 0.1 km), altitude above the SkyAtmosphere ground (FogMS_CloudPlanet).
	double BottomKm = 0.0, HeightKm = 0.1;
	if (Box)
	{
		FFogMSCloudBoxGeometry Geometry;
		Box->GetCloudHostGeometry(Geometry);
		FVector Center;
		double Radius = 0.0;
		FogMS_CloudPlanet(World, Component->PlanetRadius, Center, Radius);
		double AltMin = 0.0, AltMax = 0.0;
		FogMS_BandAltitudes(Geometry, Center, Radius, AltMin, AltMax);
		BottomKm = FMath::Max(0.0, AltMin - FogMS_HostLayerMarginCm) * 1.0e-5;
		HeightKm = FMath::Max(0.1, (AltMax + FogMS_HostLayerMarginCm) * 1.0e-5 - BottomKm);
	}
	Component->SetMobility(EComponentMobility::Movable);
	Component->LayerBottomAltitude = static_cast<float>(BottomKm);
	Component->LayerHeight = static_cast<float>(HeightKm);
	Component->TracingMaxDistanceMode = EVolumetricCloudTracingMaxDistanceMode::DistanceFromPointOfView;
	Component->TracingMaxDistance = FogMS_HostTraceKm;
	Component->TracingStartDistanceFromCamera = 0.0f;
	Component->ViewSampleCountScale = FogMS_HostViewSampleScale;
	Component->ShadowViewSampleCountScale = FogMS_HostShadowSampleScale;
	Component->ShadowTracingDistance = FogMS_HostShadowTraceKm;
	Component->StopTracingTransmittanceThreshold = FogMS_HostStopTransmittance;
	Component->bUsePerSampleAtmosphericLightTransmittance = false;
	Component->bVisibleInRealTimeSkyCaptures = false;
	Component->SetMaterial(Instance);
	Component->MarkRenderStateDirty();
	// The per-frame scan cache may predate the new host: the next AcquireHost of this frame (the Box button calls UpdateDensity) sees it.
	if (UFogMSCloudHostSubsystem* Self = World->GetSubsystem<UFogMSCloudHostSubsystem>()) Self->ScanFrame = MAX_uint64;
	OutMessage = FString::Printf(TEXT("created cloud host '%s' (MI_FogMS_Cloud; layer %.3f-%.3f km above the ground, trace %.1f km from the camera, ")
		TEXT("view samples x%.0f, sun march %.2f km). Only one Volumetric Cloud renders per scene: while this host is visible it displaces %s. ")
		TEXT("Nothing was saved: save the level to keep the host, delete it to get the sky clouds back."),
		*Host->GetActorNameOrLabel(), BottomKm, BottomKm + HeightKm, FogMS_HostTraceKm, FogMS_HostViewSampleScale, FogMS_HostShadowTraceKm,
		Others.IsEmpty() ? TEXT("no other cloud (none is visible now)") : *FString::Printf(TEXT("the sky cloud %s"), *Others));
	return Host;
}
