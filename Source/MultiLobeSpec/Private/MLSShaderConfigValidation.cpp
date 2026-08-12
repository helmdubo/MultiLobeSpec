#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MultiLobeShaderPatcher.h"

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
	TestFalse(TEXT("Direct-only configuration needs no raw transport"),
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

#endif
