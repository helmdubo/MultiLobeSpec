#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "Styling/AppStyle.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "MultiLobeSpec.h"
#include "MLSBakerTab.h"
#include "MLSConeEnvBRDFGenerator.h"
#include "MLSMicroShadowLUTGenerator.h"
#include "Widgets/Docking/SDockTab.h"

static const FName MLSBakerTabName("MLSBaker");

namespace
{
	enum class EMLSVNDFToolAction : uint8
	{
		Regenerate,
		Validate,
		ExportValidation
	};

	void RunVNDFTool(EMLSVNDFToolAction Action)
	{
		FMLSMicroShadowArtifactPaths Paths;
		FString Error;
		if (!FMLSMicroShadowLUTGenerator::GetCanonicalArtifactPaths(Paths, Error))
		{
			UE_LOG(LogTemp, Error, TEXT("MultiLobeSpec VNDF tool failed: %s"), *Error);
			return;
		}

		FMLSMicroShadowLUTSettings Settings;
		UE_LOG(LogTemp, Display,
			TEXT("MultiLobeSpec VNDF: generating %dx%dx%d RGBA8, %d samples/cell, layout=%s, roughnessInterpolation=%s"),
			Settings.VisibilitySize,
			Settings.NoLSize,
			Settings.NoVSize,
			Settings.Sequence.SampleCount,
			*FMLSMicroShadowLUTGenerator::AxisLayoutName(Settings.AxisLayout),
			*FMLSMicroShadowLUTGenerator::RoughnessInterpolationName(Settings.RoughnessInterpolation));

		FMLSMicroShadowLUTData Data;
		if (!FMLSMicroShadowLUTGenerator::Generate(Settings, Data, Error))
		{
			UE_LOG(LogTemp, Error, TEXT("MultiLobeSpec VNDF generation failed: %s"), *Error);
			return;
		}

		if (Action == EMLSVNDFToolAction::Regenerate)
		{
			if (!FMLSMicroShadowLUTGenerator::WriteArtifacts(Data, Paths, Error))
			{
				UE_LOG(LogTemp, Error, TEXT("MultiLobeSpec VNDF artifact commit failed: %s"), *Error);
				return;
			}
			UE_LOG(LogTemp, Display, TEXT("MultiLobeSpec VNDF LUT committed: %s | SHA256=%s"),
				*Paths.ShaderInclude, *Data.DataSHA256);
			return;
		}

		FMLSMicroShadowErrorMetrics Metrics;
		constexpr int32 ValidationPointCount = 10000;
		constexpr int32 ReferenceSamplesPerPoint = 16384;
		if (!FMLSMicroShadowLUTValidation::Validate(
			Data,
			ValidationPointCount,
			ReferenceSamplesPerPoint,
			0x71c35a9du,
			Metrics,
			Error))
		{
			UE_LOG(LogTemp, Error, TEXT("MultiLobeSpec VNDF validation failed to run: %s"), *Error);
			return;
		}

		UE_LOG(LogTemp, Display,
			TEXT("MultiLobeSpec VNDF validation %s: mean=%.6f p95=%.6f p99=%.6f max=%.6f boundaryP95=%.6f boundaryMax=%.6f quantMean=%.6f azimuthMean=%.6f azimuthMax=%.6f"),
			Metrics.bPassed ? TEXT("PASSED") : TEXT("FAILED"),
			Metrics.MeanAbsoluteError,
			Metrics.P95AbsoluteError,
			Metrics.P99AbsoluteError,
			Metrics.MaxAbsoluteError,
			Metrics.NearBoundaryP95AbsoluteError,
			Metrics.NearBoundaryMaxAbsoluteError,
			Metrics.QuantizationMeanAbsoluteError,
			Metrics.AzimuthMeanAbsoluteError,
			Metrics.AzimuthMaxAbsoluteError);

		if (Action == EMLSVNDFToolAction::ExportValidation)
		{
			if (!FMLSMicroShadowLUTValidation::WriteReport(Data, Metrics, Paths, Error))
			{
				UE_LOG(LogTemp, Error, TEXT("MultiLobeSpec VNDF validation export failed: %s"), *Error);
				return;
			}
			UE_LOG(LogTemp, Display, TEXT("MultiLobeSpec VNDF validation reports: %s | %s"),
				*Paths.ValidationJson, *Paths.ValidationCsv);
		}
	}

	void RunVNDFCandidateTool(const TArray<FString>& Args)
	{
		if (Args.Num() != 6 && Args.Num() != 10 && Args.Num() != 13)
		{
			UE_LOG(LogTemp, Error,
				TEXT("Usage: MLS.GenerateVNDFCandidate Name VisibilitySize NoLSize NoVSize linear|warped|boundary roughness|alpha [R0 R1 R2 R3] or [R0..R6]"));
			return;
		}

		const FString Name = Args[0];
		for (TCHAR Character : Name)
		{
			if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-'))
			{
				UE_LOG(LogTemp, Error, TEXT("VNDF candidate name may contain only letters, digits, '_' and '-'."));
				return;
			}
		}

		FMLSMicroShadowLUTSettings Settings;
		Settings.VisibilitySize = FCString::Atoi(*Args[1]);
		Settings.NoLSize = FCString::Atoi(*Args[2]);
		Settings.NoVSize = FCString::Atoi(*Args[3]);
		if (Args[4].Equals(TEXT("linear"), ESearchCase::IgnoreCase))
		{
			Settings.AxisLayout = EMLSMicroShadowAxisLayout::Linear;
		}
		else if (Args[4].Equals(TEXT("warped"), ESearchCase::IgnoreCase))
		{
			Settings.AxisLayout = EMLSMicroShadowAxisLayout::Warped;
		}
		else if (Args[4].Equals(TEXT("boundary"), ESearchCase::IgnoreCase))
		{
			Settings.AxisLayout = EMLSMicroShadowAxisLayout::BoundaryAware;
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("VNDF candidate layout must be 'linear', 'warped' or 'boundary'."));
			return;
		}
		if (Args[5].Equals(TEXT("roughness"), ESearchCase::IgnoreCase))
		{
			Settings.RoughnessInterpolation = EMLSMicroShadowRoughnessInterpolation::PerceptualRoughness;
		}
		else if (Args[5].Equals(TEXT("alpha"), ESearchCase::IgnoreCase))
		{
			Settings.RoughnessInterpolation = EMLSMicroShadowRoughnessInterpolation::Alpha;
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("VNDF candidate interpolation must be 'roughness' or 'alpha'."));
			return;
		}
		if (Args.Num() == 10 || Args.Num() == 13)
		{
			Settings.RoughnessNodes.Reset(Args.Num() - 6);
			for (int32 Index = 6; Index < Args.Num(); ++Index)
			{
				Settings.RoughnessNodes.Add(FCString::Atod(*Args[Index]));
			}
		}

		FMLSMicroShadowArtifactPaths CanonicalPaths;
		FString Error;
		if (!FMLSMicroShadowLUTGenerator::GetCanonicalArtifactPaths(CanonicalPaths, Error))
		{
			UE_LOG(LogTemp, Error, TEXT("MultiLobeSpec candidate path failed: %s"), *Error);
			return;
		}
		const FString CandidateDir = FPaths::Combine(FPaths::GetPath(CanonicalPaths.ShaderInclude), TEXT("Candidates"), Name);
		FMLSMicroShadowArtifactPaths CandidatePaths;
		CandidatePaths.ShaderInclude = FPaths::Combine(CandidateDir, TEXT("MLS_MicroShadowLUT.ush"));
		CandidatePaths.Manifest = FPaths::Combine(CandidateDir, TEXT("MLS_MicroShadowLUT.manifest.json"));
		CandidatePaths.ValidationJson = FPaths::Combine(CandidateDir, TEXT("MLS_MicroShadowLUT.validation.json"));
		CandidatePaths.ValidationCsv = FPaths::Combine(CandidateDir, TEXT("MLS_MicroShadowLUT.validation.csv"));

		FMLSMicroShadowLUTData Data;
		if (!FMLSMicroShadowLUTGenerator::Generate(Settings, Data, Error)
			|| !FMLSMicroShadowLUTGenerator::WriteArtifacts(Data, CandidatePaths, Error))
		{
			UE_LOG(LogTemp, Error, TEXT("MultiLobeSpec VNDF candidate '%s' failed: %s"), *Name, *Error);
			return;
		}
		UE_LOG(LogTemp, Display,
			TEXT("MultiLobeSpec VNDF candidate '%s' committed: %dx%dx%d %s/%s SHA256=%s"),
			*Name,
			Settings.VisibilitySize,
			Settings.NoLSize,
			Settings.NoVSize,
			*FMLSMicroShadowLUTGenerator::AxisLayoutName(Settings.AxisLayout),
			*FMLSMicroShadowLUTGenerator::RoughnessInterpolationName(Settings.RoughnessInterpolation),
			*Data.DataSHA256);
	}

	void RegenerateConeEnvBRDF()
	{
		FMLSConeEnvBRDFArtifactPaths Paths;
		FString Error;
		if (!FMLSConeEnvBRDFGenerator::GetCanonicalArtifactPaths(Paths, Error))
		{
			UE_LOG(LogTemp, Error, TEXT("MultiLobeSpec Cone EnvBRDF path failed: %s"), *Error);
			return;
		}

		FMLSConeEnvBRDFSettings Settings;
		UE_LOG(LogTemp, Display,
			TEXT("MultiLobeSpec Cone EnvBRDF: generating %dx%dx%d packed RG16, %d samples/cell"),
			Settings.NoVSize,
			Settings.RoughnessSize,
			Settings.AdjustedCosConeSize,
			Settings.SampleCount);
		FMLSConeEnvBRDFData Data;
		if (!FMLSConeEnvBRDFGenerator::Generate(Settings, Data, Error)
			|| !FMLSConeEnvBRDFGenerator::WriteArtifacts(Data, Paths, Error))
		{
			UE_LOG(LogTemp, Error, TEXT("MultiLobeSpec Cone EnvBRDF generation failed: %s"), *Error);
			return;
		}
		UE_LOG(LogTemp, Display, TEXT("MultiLobeSpec Cone EnvBRDF committed: %s | SHA256=%s"),
			*Paths.ShaderInclude, *Data.DataSHA256);
	}

	FAutoConsoleCommand MLSRegenerateVNDFLUTCommand(
		TEXT("MLS.RegenerateVNDFLUT"),
		TEXT("Regenerate the canonical Generic VNDF packed LUT and manifest. This is an explicit offline operation."),
		FConsoleCommandDelegate::CreateLambda([]() { RunVNDFTool(EMLSVNDFToolAction::Regenerate); }));

	FAutoConsoleCommand MLSValidateVNDFLUTCommand(
		TEXT("MLS.ValidateVNDFLUT"),
		TEXT("Run the full Generic VNDF LUT acceptance sweep without modifying artifacts."),
		FConsoleCommandDelegate::CreateLambda([]() { RunVNDFTool(EMLSVNDFToolAction::Validate); }));

	FAutoConsoleCommand MLSExportVNDFValidationCommand(
		TEXT("MLS.ExportVNDFValidation"),
		TEXT("Run full validation and atomically export JSON and CSV reports."),
		FConsoleCommandDelegate::CreateLambda([]() { RunVNDFTool(EMLSVNDFToolAction::ExportValidation); }));

	FAutoConsoleCommand MLSGenerateVNDFCandidateCommand(
		TEXT("MLS.GenerateVNDFCandidate"),
		TEXT("Generate an explicit candidate LUT. Args: Name VisibilitySize NoLSize NoVSize linear|warped|boundary roughness|alpha [R0 R1 R2 R3] or [R0..R6]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunVNDFCandidateTool));

	FAutoConsoleCommand MLSRegenerateConeEnvBRDFCommand(
		TEXT("MLS.RegenerateConeEnvBRDF"),
		TEXT("Regenerate the canonical 32x32x8 cone-aware EnvBRDF packed RG16 LUT and manifest."),
		FConsoleCommandDelegate::CreateStatic(&RegenerateConeEnvBRDF));
}

/**
 * Editor-only UI for MultiLobeSpec: a main-toolbar "MLS Apply" button
 * (the CallInEditor button on UDeveloperSettings is not reliably invoked
 * by the Project Settings details view in 5.7).
 * First step of the Editor/Runtime module split (review v2 §13.9).
 */
class FMultiLobeSpecEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FMultiLobeSpecEditorModule::RegisterMenus));

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(MLSBakerTabName,
			FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
			{
				return SNew(SDockTab).TabRole(ETabRole::NomadTab)
				[
					SNew(SMLSBakerTab)
				];
			}))
			.SetDisplayName(NSLOCTEXT("MLS", "BakerTab", "MLS Baker"));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(MLSBakerTabName);
	}

private:
	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);

		// "User" toolbar lives on the main level-editor toolbar, right of Play/Simulate.
		UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.User");
		FToolMenuSection& Section = Toolbar->FindOrAddSection("MultiLobeSpec");

		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			"MLSApply",
			FUIAction(FExecuteAction::CreateStatic(&FMultiLobeSpecEditorModule::OnApplyClicked)),
			INVTEXT("MLS Apply"),
			INVTEXT("MultiLobeSpec: rebuild the shader overlay from current Project Settings and recompile changed shaders (same as the MLS.Apply console command)."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh")));

		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			"MLSBaker",
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				FGlobalTabmanager::Get()->TryInvokeTab(MLSBakerTabName);
			})),
			INVTEXT("MLS Baker"),
			INVTEXT("MultiLobeSpec: окно бейка AO из нормалей (WWII pipeline) и авто-назначения в материалы."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Adjust")));

		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			"MLSRegenerateVNDFLUT",
			FUIAction(FExecuteAction::CreateLambda([]() { RunVNDFTool(EMLSVNDFToolAction::Regenerate); })),
			INVTEXT("MLS Regenerate VNDF LUT"),
			INVTEXT("Explicit offline 8192-sample/cell bake of the canonical Generic VNDF packed LUT and manifest."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Save")));

		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			"MLSValidateVNDFLUT",
			FUIAction(FExecuteAction::CreateLambda([]() { RunVNDFTool(EMLSVNDFToolAction::Validate); })),
			INVTEXT("MLS Validate VNDF LUT"),
			INVTEXT("Run the full 10k-point acceptance sweep against a fresh VNDF reference."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Check")));

		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			"MLSExportVNDFValidation",
			FUIAction(FExecuteAction::CreateLambda([]() { RunVNDFTool(EMLSVNDFToolAction::ExportValidation); })),
			INVTEXT("MLS Export VNDF CSV"),
			INVTEXT("Run acceptance and export machine-readable JSON and CSV validation reports."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Download")));

		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			"MLSRegenerateConeEnvBRDF",
			FUIAction(FExecuteAction::CreateStatic(&RegenerateConeEnvBRDF)),
			INVTEXT("MLS Regenerate Cone EnvBRDF"),
			INVTEXT("Explicit offline 8192-sample/cell bake of the 32x32x8 cone-aware EnvBRDF LUT."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Save")));
	}

	static void OnApplyClicked()
	{
		if (FModuleManager::Get().IsModuleLoaded("MultiLobeSpec"))
		{
			FMultiLobeSpecModule::Get().ApplyFromSettings();
		}
	}
};

IMPLEMENT_MODULE(FMultiLobeSpecEditorModule, MultiLobeSpecEditor)
