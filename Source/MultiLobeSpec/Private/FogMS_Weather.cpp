#include "FogMS_Weather.h"
#include "FogMS_CloudHost.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "MultiLobeSpec.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	/** Plugin assets of W48 (matedit_weather.py; textures from Tools/FogMSEnergyValidation/ProdProbe/texgen/gen_weather_textures.py). */
	const TCHAR* const FogMS_WeatherAssetDir = TEXT("/MultiLobeSpec/FogMS/Weather/");
	const TCHAR* const FogMS_ComposeName = TEXT("M_FogMS_WeatherCompose");
	const TCHAR* const FogMS_SunName = TEXT("M_FogMS_WeatherSun");
	const TCHAR* const FogMS_PatternName = TEXT("T_FogMS_WeatherPattern");
	const TCHAR* const FogMS_CurlName = TEXT("T_FogMS_Curl2D");
	const TCHAR* const FogMS_LUTName = TEXT("T_FogMS_CloudTypeLUT");
	/** RT_FogMS_WeatherMap / RT_FogMS_WeatherSun size (design 3.6: 512^2 over 16-32 km). */
	constexpr int32 FogMS_WeatherMapSize = 512;
	constexpr double FogMS_KmToCm = 1.0e5;
	/** The altitude envelope handed to the host is rounded outward to 100 m x Weather Scale (a transition refits the layer at its ends). */
	constexpr double FogMS_EnvelopeStepCm = 1.0e4;
	/** The thin-layer sun map is redrawn when the sun turns by more than this. */
	const double FogMS_SunRedrawCos = FMath::Cos(FMath::DegreesToRadians(0.05));
	/** Calibration draw of the RGBA write check (M_FogMS_WeatherCompose, P1.y = 1): the expected texel. */
	const FLinearColor FogMS_CalibrationTexel(0.25f, 0.5f, 0.75f, 0.125f);
	/** W48 FogMS.Weather.SetupShadows / the button: resolution x2 (1024 texels); shadow ray samples x1 for the Thin layer (the engine's 16
	 * samples through the hero band are enough) and x4 for the Extended layer (16 x 4 = 64 through a layer up to the weather top). The
	 * shadow pass costs in proportion to texels x samples (round 48: Extended at x4 under a 4 deg sun, 128 samples with the engine's
	 * horizon boost: 10 ms with Overcast). */
	constexpr float FogMS_WeatherShadowResolutionScale = 2.0f;
	float FogMS_WeatherShadowRaySampleScale(const AFogMSWeather* Weather)
	{
		return Weather && Weather->ShadowLayer == EFogMSWeatherShadowLayer::Extended ? 4.0f : 1.0f;
	}

	template <typename T>
	T* FogMS_LoadWeatherAsset(const TCHAR* Name)
	{
		const FString Path = FString::Printf(TEXT("%s%s.%s"), FogMS_WeatherAssetDir, Name, Name);
		if (T* Found = FindObject<T>(nullptr, *Path)) return Found;
		return LoadObject<T>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	const TCHAR* FogMS_PresetAssetName(EFogMSWeatherPreset Preset)
	{
		switch (Preset)
		{
		case EFogMSWeatherPreset::Clear: return TEXT("DA_FogMS_Weather_Clear");
		case EFogMSWeatherPreset::Scattered: return TEXT("DA_FogMS_Weather_Scattered");
		case EFogMSWeatherPreset::Broken: return TEXT("DA_FogMS_Weather_Broken");
		case EFogMSWeatherPreset::Overcast: return TEXT("DA_FogMS_Weather_Overcast");
		default: return nullptr;
		}
	}

	bool FogMS_ParsePreset(const FString& Text, EFogMSWeatherPreset& Out)
	{
		static const TPair<const TCHAR*, EFogMSWeatherPreset> Names[] =
		{
			{ TEXT("Clear"), EFogMSWeatherPreset::Clear }, { TEXT("SKC"), EFogMSWeatherPreset::Clear },
			{ TEXT("Scattered"), EFogMSWeatherPreset::Scattered }, { TEXT("SCT"), EFogMSWeatherPreset::Scattered },
			{ TEXT("Broken"), EFogMSWeatherPreset::Broken }, { TEXT("BKN"), EFogMSWeatherPreset::Broken },
			{ TEXT("Overcast"), EFogMSWeatherPreset::Overcast }, { TEXT("OVC"), EFogMSWeatherPreset::Overcast },
		};
		for (const TPair<const TCHAR*, EFogMSWeatherPreset>& Name : Names)
			if (Text.Equals(Name.Key, ESearchCase::IgnoreCase)) { Out = Name.Value; return true; }
		return false;
	}

	AFogMSWeather* FogMS_FindWeatherActor(UWorld* World)
	{
		for (TActorIterator<AFogMSWeather> It(World); It; ++It)
			if (!It->IsActorBeingDestroyed() && It->bEnabled) return *It;
		return nullptr;
	}

	/** Wraps a length into [0, Period). */
	double FogMS_Wrap(double Value, double Period)
	{
		const double Wrapped = FMath::Fmod(Value, Period);
		return Wrapped < 0.0 ? Wrapped + Period : Wrapped;
	}

	FString FogMS_ValuesText(const FFogMSWeatherValues& V)
	{
		const FString Low = V.HasLowLayer()
			? FString::Printf(TEXT("L0 coverage %.2f type %.2f %.2f-%.2f km sigma %.3f/m"), V.Coverage, V.CloudType, V.BaseKm, V.TopKm, V.Extinction)
			: FString(TEXT("L0 off"));
		const FString Deck = V.HasDeck()
			? FString::Printf(TEXT("deck %.2f %.2f-%.2f km sigma %.3f/m"), V.DeckCoverage, V.DeckBaseKm, V.DeckTopKm, V.DeckExtinction)
			: FString(TEXT("deck off"));
		return FString::Printf(TEXT("%s; %s; wind %.1f m/s toward %.0f deg"), *Low, *Deck, V.WindSpeed, V.WindDirectionDeg);
	}

	// Console: FogMS.Weather.Set <Clear|Scattered|Broken|Overcast|asset path> [seconds]
	FAutoConsoleCommandWithWorldAndArgs FogMS_WeatherSetCommand(TEXT("FogMS.Weather.Set"),
		TEXT("FogMS.Weather.Set <Clear|Scattered|Broken|Overcast|/Game/Path/To/WeatherStateAsset> [Seconds, default 0]: switches the FogMS Weather actor of ")
		TEXT("this world to that state over the given time (one log line 'weather: A -> B (N s)'). Presets use DA_FogMS_Weather_<name> from ")
		TEXT("/MultiLobeSpec/FogMS/Weather (built-in values when the asset is missing). Nothing is saved."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			AFogMSWeather* Weather = FogMS_FindWeatherActor(World);
			if (!Weather)
			{
				UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS.Weather.Set: no enabled FogMS Weather actor in this world (place one: Place Actors > FogMS Weather)."));
				return;
			}
			if (Args.Num() < 1)
			{
				UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS.Weather.Set <Clear|Scattered|Broken|Overcast|asset path> [seconds]"));
				return;
			}
			UFogMSWeatherState* State = nullptr;
			EFogMSWeatherPreset Preset = EFogMSWeatherPreset::Custom;
			if (FogMS_ParsePreset(Args[0], Preset))
				State = AFogMSWeather::FindPresetState(Preset);
			else
				State = LoadObject<UFogMSWeatherState>(nullptr, *Args[0], nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!State)
			{
				UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS.Weather.Set: '%s' is neither a preset (Clear, Scattered, Broken, Overcast) nor a FogMS Weather State asset."), *Args[0]);
				return;
			}
			const float Seconds = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 0.0f;
			Weather->SetWeather(State, FMath::IsFinite(Seconds) ? Seconds : 0.0f);
		}));

	FAutoConsoleCommandWithWorldAndArgs FogMS_WeatherSetupShadowsCommand(TEXT("FogMS.Weather.SetupShadows"),
		TEXT("FogMS.Weather.SetupShadows [ExtentKm, default: the actor's Shadow Extent Km (10)] [ResolutionScale, default 2] [RaySampleScale, default ")
		TEXT("1 with the actor's Thin layer, 4 with Extended]: the atmosphere sun of this world gets Cast Cloud Shadows on, Cloud Shadow Extent, Cloud ")
		TEXT("Shadow Map Resolution Scale and Cloud Shadow Ray Sample Count Scale for the FogMS Weather shadows (W48; 10 km x2 = 1024 texels of ")
		TEXT("19.5 m). One log line with the previous values, one undo step in the editor. Nothing is saved."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			const AFogMSWeather* Weather = FogMS_FindWeatherActor(World);
			const float ExtentKm = Args.Num() > 0 ? FCString::Atof(*Args[0]) : (Weather ? Weather->ShadowExtentKm : 10.0f);
			const float Scale = Args.Num() > 1 ? FCString::Atof(*Args[1]) : FogMS_WeatherShadowResolutionScale;
			const float RayScale = Args.Num() > 2 ? FCString::Atof(*Args[2]) : FogMS_WeatherShadowRaySampleScale(Weather);
			if (!(FMath::IsFinite(ExtentKm) && ExtentKm >= 1.0f && ExtentKm <= 10000.0f) || !(FMath::IsFinite(Scale) && Scale >= 0.25f && Scale <= 16.0f)
				|| !(FMath::IsFinite(RayScale) && RayScale >= 0.25f && RayScale <= 16.0f))
			{
				UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS.Weather.SetupShadows: extent %g km / resolution scale %g / ray sample scale %g out of range (1..10000 km, 0.25..16, 0.25..16): nothing changed."),
					ExtentKm, Scale, RayScale);
				return;
			}
			FString Message;
			const bool bOk = UFogMSCloudHostSubsystem::SetupSunShadows(World, ExtentKm, Scale, Message, RayScale);
			if (bOk) UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS.Weather.SetupShadows: %s"), *Message);
			if (!bOk) UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS.Weather.SetupShadows: %s"), *Message);
		}));

	FAutoConsoleCommandWithWorldAndArgs FogMS_WeatherStatusCommand(TEXT("FogMS.Weather.Status"),
		TEXT("FogMS.Weather.Status: logs the status line of every FogMS Weather actor of this world."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			int32 Count = 0;
			for (TActorIterator<AFogMSWeather> It(World); It; ++It, ++Count)
				UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS Weather '%s': %s"), *It->GetActorNameOrLabel(), *It->WeatherStatus);
			if (Count == 0) UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS.Weather.Status: no FogMS Weather actor in this world."));
		}));
}

// ------------------------------------------------------------------------------------------------------------------------ values

FFogMSWeatherValues FFogMSWeatherValues::Lerp(const FFogMSWeatherValues& A, const FFogMSWeatherValues& B, float T)
{
	FFogMSWeatherValues Out;
	Out.Coverage = FMath::Lerp(A.Coverage, B.Coverage, T);
	Out.CloudType = FMath::Lerp(A.CloudType, B.CloudType, T);
	Out.BaseKm = FMath::Lerp(A.BaseKm, B.BaseKm, T);
	Out.TopKm = FMath::Lerp(A.TopKm, B.TopKm, T);
	Out.Extinction = FMath::Lerp(A.Extinction, B.Extinction, T);
	Out.DetailStrength = FMath::Lerp(A.DetailStrength, B.DetailStrength, T);
	Out.DeckCoverage = FMath::Lerp(A.DeckCoverage, B.DeckCoverage, T);
	Out.DeckBaseKm = FMath::Lerp(A.DeckBaseKm, B.DeckBaseKm, T);
	Out.DeckTopKm = FMath::Lerp(A.DeckTopKm, B.DeckTopKm, T);
	Out.DeckExtinction = FMath::Lerp(A.DeckExtinction, B.DeckExtinction, T);
	// Wind as a vector: a turn through the short way, the speed blends with it.
	const float RadA = FMath::DegreesToRadians(A.WindDirectionDeg), RadB = FMath::DegreesToRadians(B.WindDirectionDeg);
	const FVector2D Wind = FMath::Lerp(FVector2D(FMath::Cos(RadA), FMath::Sin(RadA)) * A.WindSpeed, FVector2D(FMath::Cos(RadB), FMath::Sin(RadB)) * B.WindSpeed, T);
	Out.WindSpeed = static_cast<float>(Wind.Size());
	Out.WindDirectionDeg = Out.WindSpeed > 1.0e-4f ? FMath::RadiansToDegrees(static_cast<float>(FMath::Atan2(Wind.Y, Wind.X))) : FMath::Lerp(A.WindDirectionDeg, B.WindDirectionDeg, T);
	return Out;
}

bool FFogMSWeatherValues::Equals(const FFogMSWeatherValues& O) const
{
	return Coverage == O.Coverage && CloudType == O.CloudType && BaseKm == O.BaseKm && TopKm == O.TopKm && Extinction == O.Extinction
		&& DetailStrength == O.DetailStrength && DeckCoverage == O.DeckCoverage && DeckBaseKm == O.DeckBaseKm && DeckTopKm == O.DeckTopKm
		&& DeckExtinction == O.DeckExtinction && WindSpeed == O.WindSpeed && WindDirectionDeg == O.WindDirectionDeg;
}

FFogMSWeatherValues UFogMSWeatherState::GetPresetValues(EFogMSWeatherPreset InPreset)
{
	// FogMS_Weather_Design.md 1.2 / 1.6 (middle latitudes, physical scale); coverage = oktas / 8, extinction from the LWC table (1.3).
	FFogMSWeatherValues V;
	const auto Set = [&V](float Coverage, float Type, float Base, float Top, float Sigma, float Detail, float Deck, float DeckBase, float DeckTop,
		float DeckSigma, float Wind)
	{
		V.Coverage = Coverage; V.CloudType = Type; V.BaseKm = Base; V.TopKm = Top; V.Extinction = Sigma; V.DetailStrength = Detail;
		V.DeckCoverage = Deck; V.DeckBaseKm = DeckBase; V.DeckTopKm = DeckTop; V.DeckExtinction = DeckSigma; V.WindSpeed = Wind; V.WindDirectionDeg = 30.0f;
	};
	switch (InPreset)
	{
	case EFogMSWeatherPreset::Clear:     Set(0.00f, 0.50f, 1.0f, 2.0f, 0.05f, 1.0f, 0.00f, 2.5f, 3.5f, 0.03f, 5.0f); break;
	case EFogMSWeatherPreset::Scattered: Set(0.40f, 0.50f, 1.0f, 2.5f, 0.05f, 1.0f, 0.00f, 2.5f, 3.5f, 0.03f, 8.0f); break;
	case EFogMSWeatherPreset::Broken:    Set(0.75f, 0.60f, 0.8f, 3.0f, 0.07f, 1.0f, 0.30f, 2.5f, 3.5f, 0.03f, 10.0f); break;
	case EFogMSWeatherPreset::Overcast:  Set(1.00f, 0.15f, 0.5f, 1.2f, 0.07f, 0.6f, 0.80f, 2.0f, 3.5f, 0.03f, 10.0f); break;
	default: break;
	}
	return V;
}

void UFogMSWeatherState::ApplyPreset(EFogMSWeatherPreset InPreset)
{
	Modify();
	Preset = InPreset;
	if (InPreset != EFogMSWeatherPreset::Custom) Values = GetPresetValues(InPreset);
}

#if WITH_EDITOR
void UFogMSWeatherState::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FName Name = PropertyChangedEvent.GetMemberPropertyName();
	if (Name == GET_MEMBER_NAME_CHECKED(UFogMSWeatherState, Preset))
	{
		if (Preset != EFogMSWeatherPreset::Custom) Values = GetPresetValues(Preset);
	}
	else if (Name == GET_MEMBER_NAME_CHECKED(UFogMSWeatherState, Values))
	{
		if (Preset != EFogMSWeatherPreset::Custom && !Values.Equals(GetPresetValues(Preset))) Preset = EFogMSWeatherPreset::Custom;
	}
}
#endif

// ------------------------------------------------------------------------------------------------------------------------ actor

AFogMSWeather::AFogMSWeather()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	SetRootComponent(Root);
}

UFogMSWeatherState* AFogMSWeather::FindPresetState(EFogMSWeatherPreset Preset)
{
	const TCHAR* Name = FogMS_PresetAssetName(Preset);
	if (!Name) return nullptr;
	if (UFogMSWeatherState* Asset = FogMS_LoadWeatherAsset<UFogMSWeatherState>(Name)) return Asset;
	// The asset is missing (matedit_weather.py not run): a transient state with the built-in values (not saved with the level).
	UFogMSWeatherState* State = NewObject<UFogMSWeatherState>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UFogMSWeatherState::StaticClass(),
		FName(*FString::Printf(TEXT("%s_Builtin"), Name))), RF_Transient);
	State->ApplyPreset(Preset);
	return State;
}

FString AFogMSWeather::StateName(const UFogMSWeatherState* State) const
{
	if (!State) return TEXT("Clear (no Weather State)");
	FString Name = State->GetName();
	Name.RemoveFromStart(TEXT("DA_FogMS_Weather_"));
	return Name;
}

void AFogMSWeather::SetWeather(UFogMSWeatherState* NewState, float Seconds)
{
	const FString From = bHaveValues ? (TransitionDuration > 0.0f && TransitionElapsed < TransitionDuration
		? FString::Printf(TEXT("%s->%s %.0f %%"), *FromName, *ToName, 100.0f * TransitionElapsed / TransitionDuration) : ToName) : StateName(WeatherState);
	// The transition starts from the blend in effect now (a transition may interrupt another one).
	FromValues = bHaveValues ? CurrentValues : (WeatherState ? WeatherState->Values : UFogMSWeatherState::GetPresetValues(EFogMSWeatherPreset::Clear));
	FromName = From;
	Modify();
	WeatherState = NewState;
	ToName = StateName(NewState);
	TransitionElapsed = 0.0f;
	TransitionDuration = FMath::IsFinite(Seconds) ? FMath::Max(Seconds, 0.0f) : 0.0f;
	bHaveValues = true;
	if (TransitionDuration <= 0.0f)
	{
		CurrentValues = NewState ? NewState->Values : UFogMSWeatherState::GetPresetValues(EFogMSWeatherPreset::Clear);
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS Weather '%s': weather: %s -> %s (immediate): %s."), *GetActorNameOrLabel(), *From, *ToName, *FogMS_ValuesText(CurrentValues));
	}
	else
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS Weather '%s': weather: %s -> %s (%g s)."), *GetActorNameOrLabel(), *From, *ToName, TransitionDuration);
}

void AFogMSWeather::SetupSunShadows()
{
	FString Message;
	const bool bOk = UFogMSCloudHostSubsystem::SetupSunShadows(GetWorld(), FMath::Clamp(ShadowExtentKm, 1.0f, 200.0f), FogMS_WeatherShadowResolutionScale, Message,
		FogMS_WeatherShadowRaySampleScale(this));
	if (bOk) UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS Weather '%s' Setup Sun Shadows: %s"), *GetActorNameOrLabel(), *Message);
	if (!bOk) UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS Weather '%s' Setup Sun Shadows: %s"), *GetActorNameOrLabel(), *Message);
}

#if WITH_EDITOR
void AFogMSWeather::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(AFogMSWeather, WeatherState) && bHaveValues)
	{
		// The Details panel wrote the new state already: start the transition from the blend in effect.
		UFogMSWeatherState* NewState = WeatherState;
		SetWeather(NewState, EditorTransitionSeconds);
	}
}
#endif

void AFogMSWeather::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateWeather(DeltaSeconds);
}

void AFogMSWeather::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Game / PIE end or the level going away: the host (saved or not) is the level's business; only a Destroyed() deletes it.
	StopFeeding(TEXT("end of play"), false);
	Super::EndPlay(EndPlayReason);
}

void AFogMSWeather::Destroyed()
{
	StopFeeding(TEXT("the actor was deleted"), true);
	Super::Destroyed();
}

void AFogMSWeather::StopFeeding(const TCHAR* Reason, bool bDeleteCreatedHost)
{
	UWorld* World = GetWorld();
	UFogMSCloudHostSubsystem* Hosts = World ? World->GetSubsystem<UFogMSCloudHostSubsystem>() : nullptr;
	if (Hosts && bFeeding) Hosts->StopWeather(*this, Reason);
	bFeeding = false;
	if (bDeleteCreatedHost && IsValid(CreatedCloudHost) && Hosts)
	{
		if (Hosts->IsHostBoundToBox(CreatedCloudHost))
		{
			UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS Weather '%s': keeps cloud host '%s' it created (a Box renders through it)."), *GetActorNameOrLabel(),
				*CreatedCloudHost->GetActorNameOrLabel());
		}
		else
		{
			UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS Weather '%s': deletes cloud host '%s' it created (no Box renders through it; the sky clouds render again)."),
				*GetActorNameOrLabel(), *CreatedCloudHost->GetActorNameOrLabel());
			CreatedCloudHost->Destroy();
		}
		CreatedCloudHost = nullptr;
	}
}

bool AFogMSWeather::EnsureResources(FString& OutProblem)
{
	// A missing asset is looked up on disk at most every 2 s (FindObject is cheap, a failing LoadObject is not).
	const double Now = FPlatformTime::Seconds();
	if (!LastResourceProblem.IsEmpty() && Now - LastResourceAttempt < 2.0)
	{
		OutProblem = LastResourceProblem;
		return false;
	}
	LastResourceAttempt = Now;
	LastResourceProblem.Reset();
	const bool bOk = EnsureResourcesInner(OutProblem);
	if (!bOk) LastResourceProblem = OutProblem;
	return bOk;
}

bool AFogMSWeather::EnsureResourcesInner(FString& OutProblem)
{
	UMaterialInterface* Compose = ComposeMaterial ? ComposeMaterial.Get() : FogMS_LoadWeatherAsset<UMaterialInterface>(FogMS_ComposeName);
	UMaterialInterface* Sun = SunMaterial ? SunMaterial.Get() : FogMS_LoadWeatherAsset<UMaterialInterface>(FogMS_SunName);
	UTexture2D* Pattern = PatternTexture ? PatternTexture.Get() : FogMS_LoadWeatherAsset<UTexture2D>(FogMS_PatternName);
	UTexture2D* Curl = CurlTexture ? CurlTexture.Get() : FogMS_LoadWeatherAsset<UTexture2D>(FogMS_CurlName);
	UTexture2D* LUT = TypeLUT ? TypeLUT.Get() : FogMS_LoadWeatherAsset<UTexture2D>(FogMS_LUTName);
	FString Missing;
	if (!Compose) Missing += TEXT(" M_FogMS_WeatherCompose");
	if (!Sun) Missing += TEXT(" M_FogMS_WeatherSun");
	if (!Pattern) Missing += TEXT(" T_FogMS_WeatherPattern");
	if (!Curl) Missing += TEXT(" T_FogMS_Curl2D");
	if (!LUT) Missing += TEXT(" T_FogMS_CloudTypeLUT");
	if (!Missing.IsEmpty())
	{
		OutProblem = FString::Printf(TEXT("weather assets missing in /MultiLobeSpec/FogMS/Weather:%s (run texgen/gen_weather_textures.py, then matedit_weather.py in the editor)"), *Missing);
		return false;
	}
	FLinearColor Probe;
	if (!Compose->GetVectorParameterDefaultValue(FHashedMaterialParameterInfo(TEXT("FogMS_WeatherComposeP0")), Probe))
	{
		OutProblem = FString::Printf(TEXT("%s is not the W48 compose material (no FogMS_WeatherComposeP0): run matedit_weather.py"), *Compose->GetName());
		return false;
	}
	if (!ComposeMID || ComposeMID->Parent != Compose)
	{
		ComposeMID = UMaterialInstanceDynamic::Create(Compose, this,
			MakeUniqueObjectName(this, UMaterialInstanceDynamic::StaticClass(), TEXT("MID_FogMS_WeatherCompose")));
		MapCheck = 0;
		bMapValid = false;
	}
	if (!SunMID || SunMID->Parent != Sun)
	{
		SunMID = UMaterialInstanceDynamic::Create(Sun, this, MakeUniqueObjectName(this, UMaterialInstanceDynamic::StaticClass(), TEXT("MID_FogMS_WeatherSun")));
		bSunMapValid = false;
	}
	if (!ComposeMID || !SunMID)
	{
		OutProblem = TEXT("cannot create the weather material instances");
		return false;
	}
	const auto MakeTarget = [this](TObjectPtr<UTextureRenderTarget2D>& Target, const TCHAR* Name, ETextureRenderTargetFormat Format, EPixelFormat Pixel)
	{
		if (IsValid(Target) && Target->GetResource()) return false;
		Target = NewObject<UTextureRenderTarget2D>(this, MakeUniqueObjectName(this, UTextureRenderTarget2D::StaticClass(), Name), RF_Transient);
		Target->RenderTargetFormat = Format;
		Target->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);
		Target->AddressX = TA_Wrap;
		Target->AddressY = TA_Wrap;
		Target->bAutoGenerateMips = false;
		Target->InitCustomFormat(FogMS_WeatherMapSize, FogMS_WeatherMapSize, Pixel, true);
		Target->UpdateResourceImmediate(true);
		return true;
	};
	if (MakeTarget(WeatherMap, TEXT("RT_FogMS_WeatherMap"), RTF_RGBA16f, PF_FloatRGBA))
	{
		MapCheck = 0;
		bMapValid = false;
		bSunMapValid = false;
	}
	if (MakeTarget(WeatherSunMap, TEXT("RT_FogMS_WeatherSun"), RTF_R16f, PF_R16F)) bSunMapValid = false;
	if (!WeatherMap || !WeatherMap->GetResource() || !WeatherSunMap || !WeatherSunMap->GetResource())
	{
		OutProblem = TEXT("cannot create RT_FogMS_WeatherMap / RT_FogMS_WeatherSun (no rendering?)");
		return false;
	}
	ComposeMID->SetTextureParameterValue(TEXT("FogMS_WeatherPattern"), Pattern);
	SunMID->SetTextureParameterValue(TEXT("FogMS_WeatherMap"), WeatherMap);
	SunMID->SetTextureParameterValue(TEXT("FogMS_WeatherTypeLUT"), LUT);
	SunMID->SetTextureParameterValue(TEXT("FogMS_WeatherPattern"), Pattern);
	SunMID->SetTextureParameterValue(TEXT("FogMS_WeatherCurl"), Curl);
	if (MapCheck == 0)
	{
		// One-time RGBA write check (a GPU readback, once per render target): the compose material is Alpha Composite, the map is cleared
		// to (0,0,0,1), so RGB = emissive and A = 1 - opacity. A renderer that writes the channels differently (e.g. Substrate converts the
		// legacy material) is caught here and the weather stays off instead of casting wrong shadows.
		UKismetRenderingLibrary::ClearRenderTarget2D(this, WeatherMap, FLinearColor(0.0f, 0.0f, 0.0f, 1.0f));
		ComposeMID->SetVectorParameterValue(TEXT("FogMS_WeatherComposeP1"), FLinearColor(0.0f, 1.0f, 0.0f, 0.0f));
		UKismetRenderingLibrary::DrawMaterialToRenderTarget(this, WeatherMap, ComposeMID);
		const FLinearColor Got = UKismetRenderingLibrary::ReadRenderTargetRawPixel(this, WeatherMap, 7, 7, false);
		ComposeMID->SetVectorParameterValue(TEXT("FogMS_WeatherComposeP1"), FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
		const bool bOk = FMath::Abs(Got.R - FogMS_CalibrationTexel.R) < 2.0e-3f && FMath::Abs(Got.G - FogMS_CalibrationTexel.G) < 2.0e-3f
			&& FMath::Abs(Got.B - FogMS_CalibrationTexel.B) < 2.0e-3f && FMath::Abs(Got.A - FogMS_CalibrationTexel.A) < 2.0e-3f;
		MapCheck = bOk ? 1 : -1;
		bMapValid = false;
		MapCheckProblem = bOk ? FString() : FString::Printf(TEXT("RT_FogMS_WeatherMap RGBA write check failed: wrote (%.4f, %.4f, %.4f, %.4f), expected (0.25, 0.5, 0.75, 0.125): this renderer does not write the Alpha Composite compose material as expected (Substrate?); the weather stays off"),
			Got.R, Got.G, Got.B, Got.A);
		UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS Weather '%s': RT_FogMS_WeatherMap RGBA write check %s (read back %.4f, %.4f, %.4f, %.4f)."), *GetActorNameOrLabel(),
			bOk ? TEXT("passed") : TEXT("FAILED"), Got.R, Got.G, Got.B, Got.A);
	}
	if (MapCheck < 0)
	{
		OutProblem = MapCheckProblem;
		return false;
	}
	return true;
}

void AFogMSWeather::UpdateWeather(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	UFogMSCloudHostSubsystem* Hosts = World ? World->GetSubsystem<UFogMSCloudHostSubsystem>() : nullptr;
	if (!Hosts)
	{
		WeatherStatus = TEXT("Inactive: no FogMS cloud host subsystem in this world (editor, PIE and game worlds only)");
		return;
	}
	if (!bEnabled)
	{
		if (bFeeding) StopFeeding(TEXT("Enabled unticked"), false);
		WeatherStatus = TEXT("Off (Enabled unticked): the cloud host is as without weather");
		return;
	}
	FString Other;
	if (!Hosts->ClaimWeather(*this, Other))
	{
		bFeeding = false;
		WeatherStatus = FString::Printf(TEXT("Inactive: FogMS Weather '%s' drives this world (one weather actor per level)"), *Other);
		return;
	}

	// 1. The blended state (FogMS_Weather_Design.md 3.7: coverage by threshold, type along the LUT; one smoothstep curve in W48).
	const FFogMSWeatherValues Target = WeatherState ? WeatherState->Values : UFogMSWeatherState::GetPresetValues(EFogMSWeatherPreset::Clear);
	if (!bHaveValues)
	{
		CurrentValues = Target;
		FromValues = Target;
		FromName = ToName = StateName(WeatherState);
		TransitionDuration = 0.0f;
		bHaveValues = true;
	}
	float Progress = 1.0f;
	const bool bTransition = TransitionDuration > 0.0f && TransitionElapsed < TransitionDuration;
	if (bTransition)
	{
		TransitionElapsed = FMath::Min(TransitionElapsed + FMath::Max(DeltaSeconds, 0.0f), TransitionDuration);
		Progress = TransitionElapsed / TransitionDuration;
		CurrentValues = FFogMSWeatherValues::Lerp(FromValues, Target, FMath::SmoothStep(0.0f, 1.0f, Progress));
		if (TransitionElapsed >= TransitionDuration)
			UE_LOG(LogMultiLobeSpec, Display, TEXT("FogMS Weather '%s': weather: %s reached (%g s): %s."), *GetActorNameOrLabel(), *ToName, TransitionDuration,
				*FogMS_ValuesText(Target));
	}
	else
		CurrentValues = Target;

	// 2. Assets, render targets, the RGBA write check.
	FString Problem;
	if (!EnsureResources(Problem))
	{
		if (bFeeding) StopFeeding(TEXT("its resources are missing"), false);
		if (Problem != LastLoggedProblem)
		{
			LastLoggedProblem = Problem;
			UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS Weather '%s': %s."), *GetActorNameOrLabel(), *Problem);
		}
		WeatherStatus = FString::Printf(TEXT("Inactive: %s"), *Problem);
		return;
	}
	LastLoggedProblem.Reset();

	// 3. Physical numbers x Weather Scale (lengths x scale, extinction / scale: the same optical depths).
	const double Scale = FMath::Clamp(static_cast<double>(WeatherScale), 0.01, 10.0);
	const double DomainCm = FMath::Clamp(static_cast<double>(DomainSizeKm), 2.0, 64.0) * FogMS_KmToCm * Scale;
	// The detail tiling must divide the domain: the curl repeats every 2 detail tiles and the deck's low noise every 4 (FogMSWeatherFn),
	// so the tile count is a multiple of 4 and every field stays seamless where the domain (and the wind displacement) wraps.
	const int32 DetailTiles = 4 * FMath::Max(1, FMath::RoundToInt(DomainCm / (4.0 * FMath::Clamp(static_cast<double>(DetailTileKm), 0.2, 20.0) * FogMS_KmToCm * Scale)));
	const double DetailCm = DomainCm / DetailTiles;
	const double WindCmPerS = FMath::Max(0.0, static_cast<double>(CurrentValues.WindSpeed)) * 100.0 * Scale;
	const double WindRad = FMath::DegreesToRadians(static_cast<double>(CurrentValues.WindDirectionDeg));
	WindOffset.X = FogMS_Wrap(WindOffset.X + FMath::Cos(WindRad) * WindCmPerS * FMath::Max(DeltaSeconds, 0.0f), DomainCm);
	WindOffset.Y = FogMS_Wrap(WindOffset.Y + FMath::Sin(WindRad) * WindCmPerS * FMath::Max(DeltaSeconds, 0.0f), DomainCm);
	const FFogMSWeatherValues& V = CurrentValues;
	const bool bLow = V.HasLowLayer(), bDeck = V.HasDeck();
	const FLinearColor L0 = bLow ? FLinearColor(static_cast<float>(V.BaseKm * FogMS_KmToCm * Scale), static_cast<float>(V.TopKm * FogMS_KmToCm * Scale),
		static_cast<float>(V.Extinction / Scale), FMath::Clamp(V.DetailStrength, 0.0f, 1.0f)) : FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
	const FLinearColor L1 = bDeck ? FLinearColor(static_cast<float>(V.DeckBaseKm * FogMS_KmToCm * Scale), static_cast<float>(V.DeckTopKm * FogMS_KmToCm * Scale),
		static_cast<float>(V.DeckExtinction / Scale), 0.0f) : FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
	// Detail mip for the shadow pass: the pattern texel of the detail tiling vs the sun's cloud shadow map texel (the shadow is that
	// coarse anyway; design 3.5 'coarse mip in the shadow pass').
	const UDirectionalLightComponent* Sun = UFogMSCloudHostSubsystem::FindAtmosphereSun(World);
	const FVector ToSun = Sun ? FVector(-Sun->GetForwardVector()).GetSafeNormal() : FVector::UpVector;
	double ShadowTexelCm = 2000.0;
	if (Sun)
		ShadowTexelCm = 2.0 * Sun->CloudShadowExtent * FogMS_KmToCm / FMath::Clamp(512.0 * Sun->CloudShadowMapResolutionScale, 1.0, 2048.0);
	const float DetailMip = static_cast<float>(FMath::Clamp(FMath::Log2(FMath::Max(ShadowTexelCm / (DetailCm / FogMS_WeatherMapSize), 1.0)), 0.0, 5.0));
	const FVector Location = GetActorLocation();
	const FLinearColor Origin(static_cast<float>(Location.X - 0.5 * DomainCm), static_cast<float>(Location.Y - 0.5 * DomainCm), 0.0f, 0.0f);
	const FLinearColor Domain(static_cast<float>(1.0 / DomainCm), static_cast<float>(1.0 / DetailCm), FMath::Clamp(CurlStrength, 0.0f, 0.5f), DetailMip);
	const FLinearColor Wind(static_cast<float>(WindOffset.X), static_cast<float>(WindOffset.Y), 0.0f, 0.0f);

	// 4. RT_FogMS_WeatherMap: redrawn only when the map inputs change (the wind moves it at lookup).
	const FVector4f Compose(V.Coverage, V.CloudType, FMath::Clamp(CoverageEdge, 0.005f, 0.2f), bDeck ? V.DeckCoverage : 0.0f);
	if (!bMapValid || Compose != DrawnCompose)
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(this, WeatherMap, FLinearColor(0.0f, 0.0f, 0.0f, 1.0f));
		ComposeMID->SetVectorParameterValue(TEXT("FogMS_WeatherComposeP0"), FLinearColor(Compose.X, Compose.Y, Compose.Z, Compose.W));
		ComposeMID->SetVectorParameterValue(TEXT("FogMS_WeatherComposeP1"), FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
		UKismetRenderingLibrary::DrawMaterialToRenderTarget(this, WeatherMap, ComposeMID);
		DrawnCompose = Compose;
		bMapValid = true;
		bSunMapValid = false;
		++MapDrawCount;
	}
	const bool bActive = bLow || bDeck;
	const bool bThin = ShadowLayer == EFogMSWeatherShadowLayer::Thin;

	// 5. Thin layer: RT_FogMS_WeatherSun (the column optical depth along the sun) when the map, the layers or the sun changed.
	if (bThin && bActive)
	{
		if (!bSunMapValid || L0 != DrawnSunL0 || L1 != DrawnSunL1 || Domain != DrawnSunDomain || FVector::DotProduct(ToSun, DrawnSunDirection) < FogMS_SunRedrawCos)
		{
			SunMID->SetVectorParameterValue(TEXT("FogMS_WeatherDomain"), Domain);
			SunMID->SetVectorParameterValue(TEXT("FogMS_WeatherL0"), L0);
			SunMID->SetVectorParameterValue(TEXT("FogMS_WeatherL1"), L1);
			SunMID->SetVectorParameterValue(TEXT("FogMS_WeatherSunDir"), FLinearColor(static_cast<float>(ToSun.X), static_cast<float>(ToSun.Y), static_cast<float>(ToSun.Z), 0.0f));
			UKismetRenderingLibrary::DrawMaterialToRenderTarget(this, WeatherSunMap, SunMID);
			DrawnSunL0 = L0;
			DrawnSunL1 = L1;
			DrawnSunDomain = Domain;
			DrawnSunDirection = ToSun;
			bSunMapValid = true;
		}
	}

	// 6. The host feed. Envelope: the active layers of the blend and of both ends of a running transition, rounded outward.
	double Bottom = TNumericLimits<double>::Max(), Top = TNumericLimits<double>::Lowest(), Base = TNumericLimits<double>::Max();
	const auto Envelope = [&Bottom, &Top, Scale](const FFogMSWeatherValues& E)
	{
		if (E.HasLowLayer()) { Bottom = FMath::Min(Bottom, E.BaseKm * FogMS_KmToCm * Scale); Top = FMath::Max(Top, E.TopKm * FogMS_KmToCm * Scale); }
		if (E.HasDeck()) { Bottom = FMath::Min(Bottom, E.DeckBaseKm * FogMS_KmToCm * Scale); Top = FMath::Max(Top, E.DeckTopKm * FogMS_KmToCm * Scale); }
	};
	Envelope(V);
	if (bTransition) { Envelope(FromValues); Envelope(Target); }
	if (bLow) Base = FMath::Min(Base, V.BaseKm * FogMS_KmToCm * Scale);
	if (bDeck) Base = FMath::Min(Base, V.DeckBaseKm * FogMS_KmToCm * Scale);
	const double Step = FogMS_EnvelopeStepCm * Scale;
	FFogMSWeatherFeed Feed;
	Feed.Owner = this;
	Feed.bActive = bActive;
	Feed.bThinLayer = bThin;
	Feed.BottomCm = bActive ? FMath::Max(0.0, FMath::FloorToDouble(Bottom / Step) * Step) : 0.0;
	Feed.TopCm = bActive ? FMath::CeilToDouble(Top / Step) * Step : 0.0;
	Feed.BaseCm = bActive ? Base : 0.0;
	Feed.Map = WeatherMap.Get();
	Feed.SunMap = WeatherSunMap.Get();
	Feed.TypeLUT = TypeLUT ? TypeLUT.Get() : FogMS_LoadWeatherAsset<UTexture2D>(FogMS_LUTName);
	Feed.Pattern = PatternTexture ? PatternTexture.Get() : FogMS_LoadWeatherAsset<UTexture2D>(FogMS_PatternName);
	Feed.Curl = CurlTexture ? CurlTexture.Get() : FogMS_LoadWeatherAsset<UTexture2D>(FogMS_CurlName);
	Feed.Origin = Origin;
	Feed.Domain = Domain;
	Feed.Wind = Wind;
	Feed.L0 = L0;
	Feed.L1 = L1;
	Hosts->FeedWeather(Feed);
	bFeeding = true;

	// 7. No cloud host at all: create one (the FogMS.CloudHost.Create path), once per actor session.
	if (Hosts->WeatherNeedsHost() && bCreateCloudHost && !IsValid(CreatedCloudHost) && !bTriedSpawn)
	{
		bTriedSpawn = true;
		FString Message;
		AActor* Host = UFogMSCloudHostSubsystem::SpawnHost(World, nullptr, Message);
		UE_LOG(LogMultiLobeSpec, Warning, TEXT("FogMS Weather '%s': %s"), *GetActorNameOrLabel(), *Message);
		if (Host) CreatedCloudHost = Host;
	}

	// 8. Status.
	const FString StateText = bTransition
		? FString::Printf(TEXT("'%s' -> '%s' %.0f %% (%.1f of %.1f s)"), *FromName, *ToName, 100.0f * Progress, TransitionElapsed, TransitionDuration)
		: FString::Printf(TEXT("'%s'"), *ToName);
	FString SunNote;
	if (!Sun) SunNote = TEXT("no atmosphere sun: no cloud shadow map");
	else if (!Sun->bCastCloudShadows || !(Sun->CloudShadowStrength > 0.0f))
		SunNote = FString::Printf(TEXT("the sun '%s' casts no cloud shadows: click Setup Sun Shadows (or FogMS.Weather.SetupShadows)"),
			Sun->GetOwner() ? *Sun->GetOwner()->GetActorNameOrLabel() : *Sun->GetName());
	else
	{
		SunNote = FString::Printf(TEXT("cloud shadow extent %g km, texel %.1f m"), Sun->CloudShadowExtent, ShadowTexelCm * 0.01);
		if (Sun->CloudShadowExtent + 1.0e-3f < ShadowExtentKm)
			SunNote += FString::Printf(TEXT(": weather shadows only within %g km of the camera; Setup Sun Shadows gives %g km"), Sun->CloudShadowExtent, ShadowExtentKm);
	}
	const FString HostNote = Hosts->GetWeatherNote().IsEmpty() ? FString(TEXT("waiting for the cloud host subsystem tick")) : Hosts->GetWeatherNote();
	WeatherStatus = FString::Printf(TEXT("%s: %s: %s | map %d^2 over %.1f km (%.0f m/texel), drawn %d x | %s | %s | %s"),
		bActive ? TEXT("Active") : TEXT("Clear"), *StateText, *FogMS_ValuesText(V), FogMS_WeatherMapSize, DomainCm * 1.0e-5,
		DomainCm * 0.01 / FogMS_WeatherMapSize, MapDrawCount,
		bThin ? TEXT("shadow layer: thin (the weather column spread over the host layer)") : TEXT("shadow layer: extended host layer"),
		*HostNote, *SunNote);
}
