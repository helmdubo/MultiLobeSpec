#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MultiLobeShaderPatcher.h"
#include "MultiLobeSpecSettings.h"
#include "MultiLobeSpecViewExtension.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMLSRawMaterialVisibilityPredicateTest,
	"MultiLobeSpec.Runtime.RawMaterialVisibility.Predicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMLSRawMaterialVisibilityPredicateTest::RunTest(const FString& Parameters)
{
	FMLSShaderConfig Config;
	Config.bEnabled = true;
	Config.bMicroShadow = false;
	Config.IndirectMaterialVisibilityMode = 1;
	Config.bConeAwareIndirectSpecular = false;
	TestFalse(TEXT("Legacy mode preserves stock transport"), Config.NeedsRawMaterialVisibilityTransport());

	Config.bMicroShadow = true;
	TestTrue(TEXT("Direct micro-shadowing requires raw visibility"), Config.NeedsRawMaterialVisibilityTransport());

	Config.bMicroShadow = false;
	Config.IndirectMaterialVisibilityMode = 0;
	TestFalse(TEXT("Indirect direct-only policy alone does not repurpose the channel"), Config.NeedsRawMaterialVisibilityTransport());

	Config.IndirectMaterialVisibilityMode = 2;
	TestTrue(TEXT("RGB indirect mode requires continuous visibility"), Config.NeedsRawMaterialVisibilityTransport());

	Config.IndirectMaterialVisibilityMode = 1;
	Config.bConeAwareIndirectSpecular = true;
	TestTrue(TEXT("Cone-aware IBL requires continuous visibility"), Config.NeedsRawMaterialVisibilityTransport());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMLSFeatureActivationIndependenceTest,
	"MultiLobeSpec.Runtime.FeatureActivation.IndependentGates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMLSFeatureActivationIndependenceTest::RunTest(const FString& Parameters)
{
	UMultiLobeSpecSettings* Settings = NewObject<UMultiLobeSpecSettings>();
	Settings->Preset = EMLSPreset::Off;
	Settings->MicroShadowMode = EMLSMicroShadowMode::ActivisionWWII;
	Settings->IndirectMaterialVisibility = EMLSIndirectMaterialVisibility::DirectOnly;

	FMLSShaderConfig Config;
	Settings->FillConfig(Config);
	TestFalse(TEXT("Preset Off disables only the authored BRDF"), Config.bBRDFEnabled);
	TestTrue(TEXT("Preset Off does not disable an explicit direct micro-shadow mode"), Config.bMicroShadow);
	TestTrue(TEXT("Micro-shadow alone activates the shading overlay"), Config.bEnabled);
	TestEqual(TEXT("DirectOnly remains an independent selected policy"), Config.IndirectMaterialVisibilityMode, 0);

	Settings->MicroShadowMode = EMLSMicroShadowMode::Off;
	Settings->FillConfig(Config);
	TestFalse(TEXT("No BRDF, micro-shadow, or full-indirect feature leaves the overlay inactive"), Config.bEnabled);

	Settings->IndirectMaterialVisibility = EMLSIndirectMaterialVisibility::RGBInterreflection;
	Settings->FillConfig(Config);
	TestTrue(TEXT("Full indirect diffuse alone activates the overlay"), Config.bEnabled);
	TestTrue(TEXT("Full indirect diffuse requests continuous material visibility"), Config.NeedsRawMaterialVisibilityTransport());

	Settings->MicroShadowMode = EMLSMicroShadowMode::ActivisionWWII;
	Settings->FillConfig(Config);
	TestEqual(TEXT("Full indirect selection is not silently forced back to DirectOnly"), Config.IndirectMaterialVisibilityMode, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMLSRuntimeDebugBuildIdentityTest,
	"MultiLobeSpec.Runtime.DebugView.BuildIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMLSRuntimeDebugBuildIdentityTest::RunTest(const FString& Parameters)
{
	FMLSShaderConfig Config;
	Config.bEnabled = true;
	Config.bMicroShadow = true;
	Config.MicroShadowMode = 2;
	Config.DebugView = 0;
	const FString NormalBuildId = FMultiLobeShaderPatcher::GetOverlayBuildId(Config);
	Config.DebugView = 5;
	const FString DebugBuildId = FMultiLobeShaderPatcher::GetOverlayBuildId(Config);
	TestEqual(TEXT("Runtime debug selection does not change overlay build identity"), DebugBuildId, NormalBuildId);

	// Cavity knobs are compile-time defines, so they MUST participate in the
	// content-addressed identity — otherwise a value change reuses a stale overlay.
	// Values here must differ from the FMLSShaderConfig defaults (0.5 / 1.0).
	Config.DebugView = 0;
	Config.MicroShadowCavityDepth = 0.25f;
	const FString CavityDepthBuildId = FMultiLobeShaderPatcher::GetOverlayBuildId(Config);
	TestNotEqual(TEXT("Cavity depth changes overlay build identity"), CavityDepthBuildId, NormalBuildId);
	Config.MicroShadowCavityPower = 2.0f;
	const FString CavityPowerBuildId = FMultiLobeShaderPatcher::GetOverlayBuildId(Config);
	TestNotEqual(TEXT("Cavity power changes overlay build identity"), CavityPowerBuildId, CavityDepthBuildId);

	const int32 OriginalFlags = static_cast<int32>(0x81234567u);
	for (int32 Mode = 0; Mode <= 5; ++Mode)
	{
		const int32 EncodedFlags = FMLSViewExtension::EncodeDebugView(OriginalFlags, Mode);
		TestEqual(FString::Printf(TEXT("Mode %d round-trips"), Mode), FMLSViewExtension::DecodeDebugView(EncodedFlags), Mode);
		TestEqual(
			FString::Printf(TEXT("Mode %d preserves unreserved bits"), Mode),
			static_cast<uint32>(EncodedFlags) & ~FMLSViewExtension::DebugViewMask,
			static_cast<uint32>(OriginalFlags) & ~FMLSViewExtension::DebugViewMask);
	}
	return true;
}

#endif
