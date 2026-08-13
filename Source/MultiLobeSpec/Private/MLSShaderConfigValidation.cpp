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
	Config.IndirectMaterialVisibilityMode = 0;
	Config.bConeAwareIndirectSpecular = false;
	TestTrue(TEXT("Every enabled overlay preserves raw transport for runtime Debug View 3"),
		Config.NeedsRawMaterialVisibilityTransport());

	Config.bMicroShadow = true;
	TestTrue(TEXT("Every direct micro-shadow mode needs raw transport"),
		Config.NeedsRawMaterialVisibilityTransport());

	Config.bMicroShadow = false;
	Config.IndirectMaterialVisibilityMode = 1;
	TestTrue(TEXT("Raw scalar indirect visibility needs raw transport"),
		Config.NeedsRawMaterialVisibilityTransport());

	Config.IndirectMaterialVisibilityMode = 2;
	TestTrue(TEXT("RGB interreflection needs raw transport"),
		Config.NeedsRawMaterialVisibilityTransport());

	Config.IndirectMaterialVisibilityMode = 0;
	Config.bConeAwareIndirectSpecular = true;
	TestTrue(TEXT("Cone-aware indirect specular needs raw transport"),
		Config.NeedsRawMaterialVisibilityTransport());

	Config.bEnabled = false;
	Config.bMicroShadow = true;
	Config.IndirectMaterialVisibilityMode = 2;
	TestFalse(TEXT("Vanilla Off mode never requests transport"),
		Config.NeedsRawMaterialVisibilityTransport());
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
	Config.MicroShadowMode = 4;
	Config.DebugView = 0;
	const FString NormalBuildId = FMultiLobeShaderPatcher::GetOverlayBuildId(Config);
	Config.DebugView = 5;
	const FString DebugBuildId = FMultiLobeShaderPatcher::GetOverlayBuildId(Config);
	TestEqual(TEXT("Runtime debug selection does not change overlay build identity"),
		DebugBuildId, NormalBuildId);
	const int32 OriginalFlags = static_cast<int32>(0x81234567u);
	for (int32 Mode = 0; Mode <= 5; ++Mode)
	{
		const int32 EncodedFlags = FMLSViewExtension::EncodeDebugView(OriginalFlags, Mode);
		TestEqual(FString::Printf(TEXT("Mode %d round-trips through PostVolumeUserFlags"), Mode),
			FMLSViewExtension::DecodeDebugView(EncodedFlags), Mode);
		TestEqual(FString::Printf(TEXT("Mode %d preserves all unreserved user-flag bits"), Mode),
			static_cast<uint32>(EncodedFlags) & ~FMLSViewExtension::DebugViewMask,
			static_cast<uint32>(OriginalFlags) & ~FMLSViewExtension::DebugViewMask);
	}
	return true;
}

#endif
