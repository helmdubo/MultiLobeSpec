#include "MultiLobeSpec.h"
#include "MultiLobeSpecSettings.h"
#include "MultiLobeShaderPatcher.h"

#include "ShaderCore.h"          // AddShaderSourceDirectoryMapping / ResetAllShaderSourceDirectoryMappings /
                                 // AllShaderSourceDirectoryMappings / FlushShaderFileCache
#include "Misc/Paths.h"
#include "Misc/CoreDelegates.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"

DEFINE_LOG_CATEGORY(LogMultiLobeSpec);

IMPLEMENT_MODULE(FMultiLobeSpecModule, MultiLobeSpec)

// ---------------------------------------------------------------------------

static FString MLS_GetOverlayDir(const FMLSShaderConfig& Cfg)
{
	const FString BuildId = FMultiLobeShaderPatcher::GetOverlayBuildId(Cfg);
	return FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("MultiLobeSpec") / (TEXT("Shaders_") + BuildId.Left(24)));
}

static FString MLS_GetEngineShaderDir()
{
	// FPlatformProcess::ShaderDir() is the canonical engine shader source dir;
	// fall back to <Engine>/Shaders if unavailable.
	FString Dir = FPlatformProcess::ShaderDir();
	if (Dir.IsEmpty())
	{
		Dir = FPaths::Combine(FPaths::EngineDir(), TEXT("Shaders"));
	}
	return FPaths::ConvertRelativePathToFull(Dir);
}

static FMLSShaderConfig MLS_ConfigFromSettings()
{
	const UMultiLobeSpecSettings* S = GetDefault<UMultiLobeSpecSettings>();
	FMLSShaderConfig Cfg;
	S->FillConfig(Cfg);
	return Cfg;
}

static bool MLS_CheckRawMaterialVisibilityTransportPrerequisites(FString& OutError)
{
	struct FRequirement
	{
		const TCHAR* Name;
		int32 RequiredValue;
	};
	static const FRequirement Requirements[] =
	{
		{ TEXT("r.Substrate"), 0 },
		{ TEXT("r.AllowStaticLighting"), 0 },
		{ TEXT("r.GBufferDiffuseSampleOcclusion"), 0 }
	};
	for (const FRequirement& Requirement : Requirements)
	{
		const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Requirement.Name);
		if (Variable == nullptr)
		{
			OutError = FString::Printf(TEXT("Raw MaterialAO transport prerequisite unavailable: CVar '%s' was not registered."), Requirement.Name);
			return false;
		}
		const int32 ActualValue = Variable->GetInt();
		if (ActualValue != Requirement.RequiredValue)
		{
			OutError = FString::Printf(
				TEXT("Raw MaterialAO transport prerequisite failed: %s=%d, required %d. Generic VNDF direct and cone-aware capture/skylight IBL require the lossless legacy deferred transport. Configure the project and restart/rebuild shaders; Apply will not mutate restart-required CVars."),
				Requirement.Name,
				ActualValue,
				Requirement.RequiredValue);
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------

void FMultiLobeSpecModule::StartupModule()
{
	// MVP targets in-editor testing only. In cooked games we stay inert:
	// shipping patched shaders requires remapping inside the cook commandlet
	// (earlier loading phase) — deliberately out of MVP scope.
	if (!GIsEditor)
	{
		UE_LOG(LogMultiLobeSpec, Log, TEXT("Non-editor run — plugin inactive (MVP is editor-only)."));
		return;
	}

	OriginalEngineShaderDir = MLS_GetEngineShaderDir();

	// Console API for quick testing without opening Project Settings:
	static FAutoConsoleCommand CmdApply(
		TEXT("MLS.Apply"),
		TEXT("MultiLobeSpec: rebuild overlay from current settings and recompile changed shaders."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			// Route through settings so Off+Engine never maps the overlay (review v2 §9).
			FMultiLobeSpecModule::Get().ApplyFromSettings();
		}));

	static FAutoConsoleCommand CmdOff(
		TEXT("MLS.Disable"),
		TEXT("MultiLobeSpec: restore vanilla engine shaders and recompile."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			FMultiLobeSpecModule::Get().DisableAndRecompile();
		}));

	static FAutoConsoleCommand CmdPreset(
		TEXT("MLS.Preset"),
		TEXT("MultiLobeSpec: switch preset. Usage: MLS.Preset 0|1|2|3  (Off, Subtle, Cinematic, Custom)"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1) { return; }
			const int32 P = FMath::Clamp(FCString::Atoi(*Args[0]), 0, 3);
			UMultiLobeSpecSettings* S = GetMutableDefault<UMultiLobeSpecSettings>();
			S->Preset = static_cast<EMLSPreset>(P);
			S->SaveConfig();
			FMultiLobeSpecModule::Get().ApplyFromSettings();
		}));

	static FAutoConsoleCommand CmdDebugView(
		TEXT("MLS.DebugView"),
		TEXT("MultiLobeSpec: debug visualization. Usage: MLS.DebugView 0|1|2  (0 = None, 1 = Effective Lobe Weight, 2 = Second-Lobe Roughness). Requires Preset != Off (Off is a hard vanilla guarantee)."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1) { return; }
			const int32 D = FMath::Clamp(FCString::Atoi(*Args[0]), 0, 2);
			UMultiLobeSpecSettings* S = GetMutableDefault<UMultiLobeSpecSettings>();
			S->DebugView = static_cast<EMLSDebugView>(D);
			S->SaveConfig();
			if (S->Preset == EMLSPreset::Off && D != 0)
			{
				UE_LOG(LogMultiLobeSpec, Warning,
					TEXT("Preset is Off — debug views are hard-gated off with vanilla shading. Enable a preset (MLS.Preset 1..3) first."));
			}
			FMultiLobeSpecModule::Get().ApplyFromSettings();
		}));

	static FAutoConsoleCommand CmdTonemap(
		TEXT("MLS.Tonemap"),
		TEXT("MultiLobeSpec: switch tonemapper only. Usage: MLS.Tonemap 0|1|2|3|4  (0 = Engine ACES, 1 = AgX Base, 2 = AgX Punchy, 3 = GT Uchimura, 4 = GT High Contrast). BRDF preset is left untouched."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1) { return; }
			const int32 T = FMath::Clamp(FCString::Atoi(*Args[0]), 0, 4);
			UMultiLobeSpecSettings* S = GetMutableDefault<UMultiLobeSpecSettings>();
			S->Tonemapper = static_cast<EMLSTonemap>(T);
			S->SaveConfig();
			// Tonemapper is independent of the BRDF preset: works with vanilla BRDF too.
			FMultiLobeSpecModule::Get().ApplyFromSettings();
		}));

	static FAutoConsoleCommand CmdCapabilities(
		TEXT("MLS.Capabilities"),
		TEXT("MultiLobeSpec: print the fail-closed capability manifest for the active or selected overlay."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			FMultiLobeSpecModule::Get().LogCapabilities();
		}));

	// Apply on startup if a preset is active. LoadingPhase = PostEngineInit,
	// so the very first global/material shader load already happened against
	// vanilla sources (DDC hit, harmless); we then remap and issue
	// 'RecompileShaders Changed' — only shaders whose source hash actually
	// changed get rebuilt, and each preset's build is cached in the DDC.
	const FMLSShaderConfig Cfg = MLS_ConfigFromSettings();
	if (Cfg.bEnabled || Cfg.TonemapMode != 0)
	{
		ApplyAndRecompile();
	}
	else
	{
		UE_LOG(LogMultiLobeSpec, Log, TEXT("Preset = Off, Tonemapper = Engine — vanilla engine shaders, overlay not mapped."));
	}
}

void FMultiLobeSpecModule::ApplyFromSettings()
{
	const FMLSShaderConfig Cfg = MLS_ConfigFromSettings();
	if (Cfg.bEnabled || Cfg.TonemapMode != 0)
	{
		ApplyAndRecompile();
	}
	else
	{
		DisableAndRecompile();
	}
}

void FMultiLobeSpecModule::ShutdownModule()
{
	// Editor is going down; nothing to restore (mappings die with the process).
}

// ---------------------------------------------------------------------------

bool FMultiLobeSpecModule::ApplyInternal(FString& OutError)
{
	const FMLSShaderConfig Cfg = MLS_ConfigFromSettings();
	const FString OverlayDir = MLS_GetOverlayDir(Cfg);

	const bool bRawMaterialVisibilityTransportRequested =
		Cfg.MicroShadowMode == static_cast<int32>(EMLSMicroShadowMode::GenericVNDFIsotropicLUT)
		|| Cfg.bConeAwareIndirectSpecular;
	if (bRawMaterialVisibilityTransportRequested)
	{
		if (!MLS_CheckRawMaterialVisibilityTransportPrerequisites(OutError))
		{
			return false;
		}
	}

	if (bOverlayActive && ActiveOverlayDir == OverlayDir)
	{
		return true;
	}

	if (!FMultiLobeShaderPatcher::BuildOverlay(OriginalEngineShaderDir, OverlayDir, Cfg, OutError))
	{
		// Overlay directories are immutable and content-addressed. A failed target
		// was never mapped, so removing it cannot damage the previous active build.
		if (OverlayDir != ActiveOverlayDir)
		{
			IFileManager::Get().DeleteDirectory(*OverlayDir, /*RequireExists*/false, /*Tree*/true);
		}
		return false;
	}

	if (!RemapEngineShaders(OverlayDir))
	{
		OutError = TEXT("Shader directory remap failed — overlay built but NOT active.");
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
		UE_LOG(LogMultiLobeSpec, Error, TEXT("Apply FAILED: %s | Current state: %s"), *Error,
			bOverlayActive ? TEXT("PREVIOUS overlay remains active") : TEXT("vanilla engine shaders"));
		return;
	}

	const FMLSShaderConfig Cfg = MLS_ConfigFromSettings();
	static const TCHAR* TonemapNames[] = { TEXT("Engine ACES"), TEXT("AgX Base"), TEXT("AgX Punchy"), TEXT("GT Uchimura"), TEXT("GT High Contrast") };
	UE_LOG(LogMultiLobeSpec, Log,
		TEXT("Overlay active: %s | Tonemap=%s | DualLobe W=%.2f EnvW=%.2f R2=r*%.2f+%.2f CoreFade=%.1f | Diffuse=%s"),
		*ActiveOverlayDir,
		TonemapNames[FMath::Clamp(Cfg.TonemapMode, 0, 4)],
		Cfg.Lobe2Weight, Cfg.EnvLobe2Weight, Cfg.Lobe2Scale, Cfg.Lobe2Offset, Cfg.CoreFade,
		Cfg.DiffuseModel == 1 ? TEXT("Chan") : TEXT("Lambert"));
	TriggerRecompile();
}

void FMultiLobeSpecModule::DisableAndRecompile()
{
	if (!bOverlayActive)
	{
		UE_LOG(LogMultiLobeSpec, Log, TEXT("Overlay already inactive."));
		return;
	}

	const FString RestoreDir = PreviousEngineMapping.IsEmpty() ? OriginalEngineShaderDir : PreviousEngineMapping;
	if (!RemapEngineShaders(RestoreDir))
	{
		UE_LOG(LogMultiLobeSpec, Error, TEXT("Restore remap failed — overlay may still be active."));
		return;
	}
	bOverlayActive = false;
	ActiveOverlayDir.Reset();
	// Forget the captured mapping: on the next activation we must capture whatever
	// is CURRENT (another tool may remap /Engine while we are inactive) — review v2 §12.
	PreviousEngineMapping.Reset();

	UE_LOG(LogMultiLobeSpec, Log, TEXT("Restored pre-plugin engine shader mapping: %s"), *RestoreDir);
	TriggerRecompile();
}

void FMultiLobeSpecModule::LogCapabilities() const
{
	const FString OverlayDir = bOverlayActive ? ActiveOverlayDir : MLS_GetOverlayDir(MLS_ConfigFromSettings());
	const FString ManifestPath = OverlayDir / TEXT("MLS_CAPABILITIES.json");
	FString Manifest;
	if (!FFileHelper::LoadFileToString(Manifest, *ManifestPath))
	{
		UE_LOG(LogMultiLobeSpec, Warning,
			TEXT("Capability manifest is not available for '%s'. Apply/build the selected overlay first."),
			*OverlayDir);
		return;
	}
	UE_LOG(LogMultiLobeSpec, Display, TEXT("Capability manifest: %s\n%s"), *ManifestPath, *Manifest);
}

bool FMultiLobeSpecModule::RemapEngineShaders(const FString& NewDir)
{
	// The virtual include root "/Engine" -> disk directory lives in the shader
	// source directory mapping table. There is no public "replace" API, so we
	// snapshot the table, reset it and re-register everything with our
	// substitution. Plugin "/Plugin/X" mappings registered before us are
	// preserved by the snapshot; ones registered after are unaffected.
	TMap<FString, FString> Mappings = AllShaderSourceDirectoryMappings();

	if (!Mappings.Contains(TEXT("/Engine")))
	{
		// Extremely early load order edge case — bail out rather than risk a
		// duplicate-mapping assert when the engine registers "/Engine" later.
		UE_LOG(LogMultiLobeSpec, Error,
			TEXT("'/Engine' mapping not registered yet — cannot remap safely. ")
			TEXT("Run 'MLS.Apply' from the console once the editor is fully loaded."));
		return false;
	}

	// Remember whatever was mapped before OUR first remap (could be another
	// tool's overlay, not necessarily the physical engine dir) — restore THAT.
	if (PreviousEngineMapping.IsEmpty() && !bOverlayActive)
	{
		PreviousEngineMapping = Mappings[TEXT("/Engine")];
	}

	Mappings[TEXT("/Engine")] = NewDir;

	ResetAllShaderSourceDirectoryMappings();
	for (const TPair<FString, FString>& It : Mappings)
	{
		AddShaderSourceDirectoryMapping(It.Key, It.Value);
	}
	return true;
}

void FMultiLobeSpecModule::TriggerRecompile()
{
	// Drop cached shader-source hashes so the DDC keying sees the overlay files…
	FlushShaderFileCache();

	// …and rebuild everything whose source hash changed. First build of a
	// preset: minutes (base pass is the big permutation bush). Every
	// subsequent switch to an already-built preset: seconds (DDC hit).
	if (GEngine)
	{
		GEngine->Exec(nullptr, TEXT("RECOMPILESHADERS CHANGED"));
	}
	else
	{
		UE_LOG(LogMultiLobeSpec, Warning,
			TEXT("GEngine not available — run 'RecompileShaders Changed' manually (Ctrl+Shift+.)"));
	}
}
