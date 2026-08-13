#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MultiLobeShaderPatcher.h"
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
	TestFalse(TEXT("RGB indirect mode is not a v0.15 raw-transport trigger"), Config.NeedsRawMaterialVisibilityTransport());

	Config.bConeAwareIndirectSpecular = true;
	TestFalse(TEXT("Staged cone IBL does not silently activate transport"), Config.NeedsRawMaterialVisibilityTransport());
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
