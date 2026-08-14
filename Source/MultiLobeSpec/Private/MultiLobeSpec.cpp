#include "MultiLobeSpec.h"
#include "MultiLobeSpecSettings.h"
#include "MultiLobeShaderPatcher.h"
#include "MultiLobeSpecViewExtension.h"
#include "MLSRawMaterialVisibilityOverlay.h"

#include "ShaderCore.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CoreDelegates.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "EditorSupportDelegates.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

DEFINE_LOG_CATEGORY(LogMultiLobeSpec);
IMPLEMENT_MODULE(FMultiLobeSpecModule, MultiLobeSpec)

static FString MLS_GetOverlayDir(const FMLSShaderConfig& Cfg)
{
	const FString BuildId = FMultiLobeShaderPatcher::GetOverlayBuildId(Cfg);
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MultiLobeSpec") / (TEXT("Shaders_") + BuildId.Left(24)));
}

static FString MLS_GetEngineShaderDir()
{
	FString Dir = FPlatformProcess::ShaderDir();
	if (Dir.IsEmpty()) Dir = FPaths::Combine(FPaths::EngineDir(), TEXT("Shaders"));
	return FPaths::ConvertRelativePathToFull(Dir);
}

static FMLSShaderConfig MLS_ConfigFromSettings()
{
	FMLSShaderConfig Config;
	GetDefault<UMultiLobeSpecSettings>()->FillConfig(Config);
	return Config;
}

static bool MLS_CheckRawMaterialVisibilityTransportPrerequisites(FString& OutError)
{
	struct FRequirement { const TCHAR* Name; int32 Value; };
	static const FRequirement Requirements[] =
	{
		{ TEXT("r.Substrate"), 0 },
		{ TEXT("r.AllowStaticLighting"), 0 }
	};
	for (const FRequirement& Requirement : Requirements)
	{
		const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Requirement.Name);
		if (!Variable || Variable->GetInt() != Requirement.Value)
		{
			OutError = FString::Printf(TEXT("Required renderer contract: %s=%d."), Requirement.Name, Requirement.Value);
			return false;
		}
	}

	const IConsoleVariable* SampleMask = IConsoleManager::Get().FindConsoleVariable(TEXT("r.GBufferDiffuseSampleOcclusion"));
	if (!SampleMask || (SampleMask->GetInt() != 0 && SampleMask->GetInt() != 1))
	{
		OutError = TEXT("r.GBufferDiffuseSampleOcclusion must exist and be either 0 or 1.");
		return false;
	}
	UE_LOG(LogMultiLobeSpec, Log, TEXT("Raw MaterialAO overlay transport supports r.GBufferDiffuseSampleOcclusion=%d."), SampleMask->GetInt());
	return true;
}

void FMultiLobeSpecModule::StartupModule()
{
	if (!GIsEditor)
	{
		UE_LOG(LogMultiLobeSpec, Log, TEXT("Non-editor run — plugin inactive."));
		return;
	}

	OriginalEngineShaderDir = MLS_GetEngineShaderDir();
	RuntimeViewExtension = FSceneViewExtensions::NewExtension<FMLSViewExtension>();

	static FAutoConsoleCommand CmdApply(TEXT("MLS.Apply"), TEXT("Apply MLS settings."), FConsoleCommandDelegate::CreateLambda([] { FMultiLobeSpecModule::Get().ApplyFromSettings(); }));
	static FAutoConsoleCommand CmdOff(TEXT("MLS.Disable"), TEXT("Restore previous shader mapping."), FConsoleCommandDelegate::CreateLambda([] { FMultiLobeSpecModule::Get().DisableAndRecompile(); }));
	static FAutoConsoleCommand CmdCapabilities(TEXT("MLS.Capabilities"), TEXT("Print MLS capability manifest."), FConsoleCommandDelegate::CreateLambda([] { FMultiLobeSpecModule::Get().LogCapabilities(); }));
	static FAutoConsoleCommand CmdStatus(TEXT("MLS.Status"), TEXT("Print overlay/mapping/config/marker health report."), FConsoleCommandDelegate::CreateLambda([] { FMultiLobeSpecModule::Get().LogStatus(); }));

	static FAutoConsoleCommand CmdPreset(TEXT("MLS.Preset"), TEXT("MLS.Preset 0|1|2|3"), FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.IsEmpty()) return;
		UMultiLobeSpecSettings* S = GetMutableDefault<UMultiLobeSpecSettings>();
		S->Preset = static_cast<EMLSPreset>(FMath::Clamp(FCString::Atoi(*Args[0]), 0, 3));
		S->SaveConfig();
		FMultiLobeSpecModule::Get().ApplyFromSettings();
	}));

	static FAutoConsoleCommand CmdDebug(TEXT("MLS.DebugView"), TEXT("Instant debug view 0..5; no compile."), FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.IsEmpty()) return;
		FMultiLobeSpecModule::Get().SetRuntimeDebugView(FMath::Clamp(FCString::Atoi(*Args[0]), 0, 5));
	}));

	static FAutoConsoleCommand CmdMicro(TEXT("MLS.MicroShadow"), TEXT("0 Legacy, 1 Step, 2 CoD WWII, 3 PZ, 4 VNDF experimental, 5 overlap experimental."), FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.IsEmpty()) return;
		UMultiLobeSpecSettings* S = GetMutableDefault<UMultiLobeSpecSettings>();
		S->MicroShadowMode = static_cast<EMLSMicroShadowMode>(FMath::Clamp(FCString::Atoi(*Args[0]), 0, 5));
		S->SaveConfig();
		FMultiLobeSpecModule::Get().ApplyFromSettings();
	}));

	static FAutoConsoleCommand CmdIndirect(TEXT("MLS.IndirectVisibility"), TEXT("Indirect material-visibility policy: 0 Direct Only, 1 UE Legacy, 2 Activision RGB diffuse."), FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.IsEmpty()) return;
		UMultiLobeSpecSettings* S = GetMutableDefault<UMultiLobeSpecSettings>();
		S->IndirectMaterialVisibility = static_cast<EMLSIndirectMaterialVisibility>(FMath::Clamp(FCString::Atoi(*Args[0]), 0, 2));
		S->SaveConfig();
		FMultiLobeSpecModule::Get().ApplyFromSettings();
	}));

	static FAutoConsoleCommand CmdCavity(TEXT("MLS.CavityDepth"), TEXT("Direct-only cavity deepening: MLS.CavityDepth <0..1> [power 0.25..4]. Rebuilds the overlay."), FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.IsEmpty()) return;
		UMultiLobeSpecSettings* S = GetMutableDefault<UMultiLobeSpecSettings>();
		S->MicroShadowCavityDepth = FMath::Clamp(FCString::Atof(*Args[0]), 0.0f, 1.0f);
		if (Args.Num() > 1) S->MicroShadowCavityPower = FMath::Clamp(FCString::Atof(*Args[1]), 0.25f, 4.0f);
		S->SaveConfig();
		FMultiLobeSpecModule::Get().ApplyFromSettings();
	}));

	static FAutoConsoleCommand CmdTonemap(TEXT("MLS.Tonemap"), TEXT("MLS.Tonemap 0..4"), FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.IsEmpty()) return;
		UMultiLobeSpecSettings* S = GetMutableDefault<UMultiLobeSpecSettings>();
		S->Tonemapper = static_cast<EMLSTonemap>(FMath::Clamp(FCString::Atoi(*Args[0]), 0, 4));
		S->SaveConfig();
		FMultiLobeSpecModule::Get().ApplyFromSettings();
	}));

	const FMLSShaderConfig Config = MLS_ConfigFromSettings();
	if (Config.bEnabled || Config.TonemapMode != 0) ApplyAndRecompile();
	else SetRuntimeDebugView(0, false);
}

void FMultiLobeSpecModule::ApplyFromSettings()
{
	const FMLSShaderConfig Config = MLS_ConfigFromSettings();
	if (Config.bEnabled || Config.TonemapMode != 0) ApplyAndRecompile();
	else DisableAndRecompile();
}

void FMultiLobeSpecModule::ShutdownModule()
{
	if (RuntimeViewExtension.IsValid()) RuntimeViewExtension->SetDebugView(0);
	RuntimeViewExtension.Reset();
}

bool FMultiLobeSpecModule::ApplyInternal(FString& OutError)
{
	const FMLSShaderConfig Config = MLS_ConfigFromSettings();
	const FString OverlayDir = MLS_GetOverlayDir(Config);
	const bool bRawTransport = Config.NeedsRawMaterialVisibilityTransport();
	if (bRawTransport && !MLS_CheckRawMaterialVisibilityTransportPrerequisites(OutError)) return false;
	if (bOverlayActive && ActiveOverlayDir == OverlayDir) return true;

	if (!FMultiLobeShaderPatcher::BuildOverlay(OriginalEngineShaderDir, OverlayDir, Config, OutError))
	{
		if (OverlayDir != ActiveOverlayDir) IFileManager::Get().DeleteDirectory(*OverlayDir, false, true);
		return false;
	}
	if (bRawTransport && !FMLSRawMaterialVisibilityOverlay::Patch(OverlayDir, OutError))
	{
		if (OverlayDir != ActiveOverlayDir) IFileManager::Get().DeleteDirectory(*OverlayDir, false, true);
		return false;
	}
	if (!RemapEngineShaders(OverlayDir))
	{
		OutError = TEXT("Shader directory remap failed — overlay built but not active.");
		return false;
	}
	bOverlayActive = true;
	ActiveOverlayDir = OverlayDir;
	return true;
}

void FMultiLobeSpecModule::ApplyAndRecompile()
{
	FString Error;
	if (!ApplyInternal(Error))
	{
		UE_LOG(LogMultiLobeSpec, Error, TEXT("Apply FAILED: %s"), *Error);
		// A failed apply leaves the previous mapping intact, so the viewport keeps
		// rendering stock shaders and every MLS feature silently looks dead. Surface
		// the failure in the editor UI instead of only the output log.
		FNotificationInfo Notification(FText::Format(
			NSLOCTEXT("MLS", "ApplyFailed", "MLS Apply failed — overlay NOT active:\n{0}\nRun MLS.Status for details."),
			FText::FromString(Error)));
		Notification.ExpireDuration = 10.0f;
		Notification.bFireAndForget = true;
		if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Notification))
		{
			Item->SetCompletionState(SNotificationItem::CS_Fail);
		}
		return;
	}
	const FMLSShaderConfig Config = MLS_ConfigFromSettings();
	UE_LOG(LogMultiLobeSpec, Log, TEXT("Overlay active: %s | MicroShadow=%d | IndirectVisibility=%d"), *ActiveOverlayDir, Config.MicroShadowMode, Config.IndirectMaterialVisibilityMode);
	SetRuntimeDebugView(Config.DebugView, false);
	TriggerRecompile();
}

void FMultiLobeSpecModule::DisableAndRecompile()
{
	SetRuntimeDebugView(0, false);
	if (!bOverlayActive) return;
	const FString RestoreDir = PreviousEngineMapping.IsEmpty() ? OriginalEngineShaderDir : PreviousEngineMapping;
	if (!RemapEngineShaders(RestoreDir)) return;
	bOverlayActive = false;
	ActiveOverlayDir.Reset();
	PreviousEngineMapping.Reset();
	TriggerRecompile();
}

bool FMultiLobeSpecModule::SetRuntimeDebugView(int32 DebugView, bool bPersistSetting)
{
	const int32 Requested = FMath::Clamp(DebugView, 0, 5);
	UMultiLobeSpecSettings* Settings = GetMutableDefault<UMultiLobeSpecSettings>();
	if (bPersistSetting) Settings->DebugView = static_cast<EMLSDebugView>(Requested);
	FMLSShaderConfig Config;
	Settings->FillConfig(Config);
	const int32 Effective = (Config.bEnabled && bOverlayActive) ? Requested : 0;
	if (!RuntimeViewExtension.IsValid()) return false;
	RuntimeViewExtension->SetDebugView(Effective);
	FEditorSupportDelegates::RedrawAllViewports.Broadcast();
	if (Effective != Requested)
	{
		UE_LOG(LogMultiLobeSpec, Warning,
			TEXT("Debug view %d requested but forced to 0: %s."),
			Requested,
			!Config.bEnabled
				? TEXT("all MLS shading features are disabled")
				: TEXT("overlay is not active — run MLS.Apply and check MLS.Status"));
		return true;
	}
	UE_LOG(LogMultiLobeSpec, Display, TEXT("Runtime debug view = %d; no overlay rebuild or shader compile."), Effective);
	if (Effective >= 1)
	{
		UE_LOG(LogMultiLobeSpec, Display,
			TEXT("Debug views 1..5 are lighting-weighted per-light masks inside DefaultLitBxDF: pixels with no direct light stay black. For a readable mask use a strong directional light and ShowFlag.GlobalIllumination 0, ShowFlag.SkyLighting 0, ShowFlag.ReflectionEnvironment 0."));
	}
	return true;
}

void FMultiLobeSpecModule::LogCapabilities() const
{
	const FString OverlayDir = bOverlayActive ? ActiveOverlayDir : MLS_GetOverlayDir(MLS_ConfigFromSettings());
	const FString Path = OverlayDir / TEXT("MLS_CAPABILITIES.json");
	FString Manifest;
	// UE_LOG expands to a brace-enclosed block, so the ';' terminating the
	// single-statement 'if' closes the statement and orphans 'else' (C2181).
	// Any if/else around a log macro must be braced.
	if (FFileHelper::LoadFileToString(Manifest, *Path))
	{
		UE_LOG(LogMultiLobeSpec, Display, TEXT("%s\n%s"), *Path, *Manifest);
	}
	else
	{
		UE_LOG(LogMultiLobeSpec, Warning, TEXT("Capability manifest not available: %s"), *Path);
	}
}

void FMultiLobeSpecModule::LogStatus() const
{
	const FMLSShaderConfig Config = MLS_ConfigFromSettings();
	const FString TargetDir = MLS_GetOverlayDir(Config);

	UE_LOG(LogMultiLobeSpec, Display, TEXT("=== MLS.Status ==="));
	if (bOverlayActive)
	{
		UE_LOG(LogMultiLobeSpec, Display, TEXT("Overlay active: YES  (%s)"), *ActiveOverlayDir);
	}
	else
	{
		UE_LOG(LogMultiLobeSpec, Warning,
			TEXT("Overlay active: NO — the engine is rendering stock shaders. Micro-shadow, debug views and indirect modes are all inert. Run MLS.Apply and read the error above it, if any."));
	}
	UE_LOG(LogMultiLobeSpec, Display, TEXT("Target overlay for current settings: %s%s"),
		*TargetDir,
		(bOverlayActive && TargetDir == ActiveOverlayDir) ? TEXT("  (matches active)") : TEXT("  (NOT the active overlay — settings changed since last apply)"));

	const TMap<FString, FString> Mappings = AllShaderSourceDirectoryMappings();
	const FString* EngineMapping = Mappings.Find(TEXT("/Engine"));
	UE_LOG(LogMultiLobeSpec, Display, TEXT("/Engine shader mapping: %s"),
		EngineMapping ? **EngineMapping : TEXT("<missing>"));

	UE_LOG(LogMultiLobeSpec, Display,
		TEXT("Effective config: overlayEnabled=%d brdfEnabled=%d microShadowMode=%d microShadowEnabled=%d cavityDepth=%.2f cavityPower=%.2f indirectVisibility=%d rawVisibilityTransport=%d tonemap=%d debugViewRuntime=%d"),
		Config.bEnabled ? 1 : 0,
		Config.bBRDFEnabled ? 1 : 0,
		Config.MicroShadowMode,
		Config.bMicroShadow ? 1 : 0,
		Config.MicroShadowCavityDepth,
		Config.MicroShadowCavityPower,
		Config.IndirectMaterialVisibilityMode,
		Config.NeedsRawMaterialVisibilityTransport() ? 1 : 0,
		Config.TonemapMode,
		RuntimeViewExtension.IsValid() ? RuntimeViewExtension->GetDebugView() : -1);

	static const TCHAR* const ContractCVars[] = { TEXT("r.Substrate"), TEXT("r.AllowStaticLighting"), TEXT("r.GBufferDiffuseSampleOcclusion") };
	for (const TCHAR* CVarName : ContractCVars)
	{
		const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(CVarName);
		UE_LOG(LogMultiLobeSpec, Display, TEXT("CVar %s = %s"),
			CVarName, Variable ? *FString::FromInt(Variable->GetInt()) : TEXT("<missing>"));
	}

	if (!bOverlayActive)
	{
		return;
	}

	// Ground truth of what the compiled shaders actually saw: the generated config
	// defines and the patch markers inside the active overlay files.
	FString ConfigText;
	if (FFileHelper::LoadFileToString(ConfigText, *(ActiveOverlayDir / TEXT("Private/MultiLobeSpecConfig.ush"))))
	{
		TArray<FString> Lines;
		ConfigText.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("#define MLS_ENABLED"))
				|| Line.StartsWith(TEXT("#define MLS_BRDF_ENABLED"))
				|| Line.StartsWith(TEXT("#define MLS_MICROSHADOW"))
				|| Line.StartsWith(TEXT("#define MLS_INDIRECT_VISIBILITY_MODE"))
				|| Line.StartsWith(TEXT("#define MLS_RAW_MATERIAL_VISIBILITY_TRANSPORT")))
			{
				UE_LOG(LogMultiLobeSpec, Display, TEXT("  %s"), *Line);
			}
		}
	}
	else
	{
		UE_LOG(LogMultiLobeSpec, Warning, TEXT("  Generated MultiLobeSpecConfig.ush is missing from the active overlay."));
	}

	struct FMarkerCheck { const TCHAR* RelPath; const TCHAR* Marker; bool bExpected; };
	const FMarkerCheck Checks[] =
	{
		{ TEXT("Private/BasePassPixelShader.usf"),            TEXT("MLS_RAW_MATERIAL_VISIBILITY_TRANSPORT"),      Config.NeedsRawMaterialVisibilityTransport() },
		{ TEXT("Private/BasePassPixelShader.usf"),            TEXT("MLS_SAMPLE_OCCLUSION_TRANSPORT_SUPPORTED"),   Config.NeedsRawMaterialVisibilityTransport() },
		{ TEXT("Private/GBufferHelpers.ush"),                 TEXT("MLS_GBUFFER_RAW_VISIBILITY_TRANSPORT"),       Config.NeedsRawMaterialVisibilityTransport() },
		{ TEXT("Private/DiffuseIndirectComposite.usf"),       TEXT("MLS_NONLUMEN_MATERIAL_VISIBILITY_POLICY"),     Config.bEnabled },
		{ TEXT("Private/Lumen/LumenScreenProbeGather.usf"),   TEXT("MLS_LUMEN_MATERIAL_VISIBILITY_POLICY"),        Config.bEnabled },
		{ TEXT("Private/SkyLightingDiffuseShared.ush"),       TEXT("MLS_SKYLIGHT_MATERIAL_VISIBILITY_POLICY"),     Config.bEnabled },
		{ TEXT("Private/ReflectionEnvironmentPixelShader.usf"), TEXT("MLS_REFLECTION_ENV_MATERIAL_VISIBILITY_POLICY"), Config.bEnabled },
	};
	for (const FMarkerCheck& Check : Checks)
	{
		FString Text;
		const bool bLoaded = FFileHelper::LoadFileToString(Text, *(ActiveOverlayDir / Check.RelPath));
		const bool bFound = bLoaded && Text.Contains(Check.Marker);
		const bool bHealthy = bLoaded && (bFound == Check.bExpected);
		if (bHealthy)
		{
			UE_LOG(LogMultiLobeSpec, Display, TEXT("  [OK]   %s: %s"), Check.RelPath, bFound ? TEXT("patched") : TEXT("stock (as expected)"));
		}
		else
		{
			UE_LOG(LogMultiLobeSpec, Warning, TEXT("  [FAIL] %s: %s (expected %s)"),
				Check.RelPath,
				!bLoaded ? TEXT("file missing") : bFound ? TEXT("patched") : TEXT("NOT patched"),
				Check.bExpected ? TEXT("patched") : TEXT("stock"));
		}
	}
}

bool FMultiLobeSpecModule::RemapEngineShaders(const FString& NewDir)
{
	TMap<FString, FString> Mappings = AllShaderSourceDirectoryMappings();
	if (!Mappings.Contains(TEXT("/Engine"))) return false;
	if (PreviousEngineMapping.IsEmpty() && !bOverlayActive) PreviousEngineMapping = Mappings[TEXT("/Engine")];
	Mappings[TEXT("/Engine")] = NewDir;
	ResetAllShaderSourceDirectoryMappings();
	for (const TPair<FString, FString>& Mapping : Mappings) AddShaderSourceDirectoryMapping(Mapping.Key, Mapping.Value);
	return true;
}

void FMultiLobeSpecModule::TriggerRecompile()
{
	FlushShaderFileCache();
	if (GEngine) GEngine->Exec(nullptr, TEXT("RECOMPILESHADERS CHANGED"));
}
