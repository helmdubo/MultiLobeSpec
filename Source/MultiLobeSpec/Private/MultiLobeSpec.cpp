#include "MultiLobeSpec.h"
#include "MultiLobeSpecSettings.h"
#include "MultiLobeShaderPatcher.h"
#include "MultiLobeSpecViewExtension.h"
#include "MLSRawMaterialVisibilityOverlay.h"

#include "ShaderCore.h"
#include "Misc/Paths.h"
#include "Misc/CoreDelegates.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "EditorSupportDelegates.h"

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

	static FAutoConsoleCommand CmdIndirect(TEXT("MLS.IndirectVisibility"), TEXT("Legacy-only indirect AO policy 0..2."), FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.IsEmpty()) return;
		UMultiLobeSpecSettings* S = GetMutableDefault<UMultiLobeSpecSettings>();
		S->IndirectMaterialVisibility = static_cast<EMLSIndirectMaterialVisibility>(FMath::Clamp(FCString::Atoi(*Args[0]), 0, 2));
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
	const int32 Effective = (Settings->Preset != EMLSPreset::Off && bOverlayActive) ? Requested : 0;
	if (!RuntimeViewExtension.IsValid()) return false;
	RuntimeViewExtension->SetDebugView(Effective);
	FEditorSupportDelegates::RedrawAllViewports.Broadcast();
	UE_LOG(LogMultiLobeSpec, Display, TEXT("Runtime debug view = %d; no overlay rebuild or shader compile."), Effective);
	return true;
}

void FMultiLobeSpecModule::LogCapabilities() const
{
	const FString OverlayDir = bOverlayActive ? ActiveOverlayDir : MLS_GetOverlayDir(MLS_ConfigFromSettings());
	const FString Path = OverlayDir / TEXT("MLS_CAPABILITIES.json");
	FString Manifest;
	if (FFileHelper::LoadFileToString(Manifest, *Path)) UE_LOG(LogMultiLobeSpec, Display, TEXT("%s\n%s"), *Path, *Manifest);
	else UE_LOG(LogMultiLobeSpec, Warning, TEXT("Capability manifest not available: %s"), *Path);
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
