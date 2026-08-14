#include "MultiLobeSpecSettings.h"
#include "MultiLobeSpec.h"

UMultiLobeSpecSettings::UMultiLobeSpecSettings()
{
	CategoryName = TEXT("Plugins");
}

void UMultiLobeSpecSettings::FillConfig(FMLSShaderConfig& Out) const
{
	Out.bBRDFEnabled = (Preset != EMLSPreset::Off);

	switch (Preset)
	{
	case EMLSPreset::Off:
		Out.Lobe2Weight = 0.f; Out.EnvLobe2Weight = 0.f;
		Out.Lobe2Scale = 1.f;  Out.Lobe2Offset = 0.f;
		Out.CoreFade = 6.f;
		break;
	case EMLSPreset::Subtle:
		Out.Lobe2Weight = 0.12f; Out.EnvLobe2Weight = 0.12f;
		Out.Lobe2Scale = 1.6f;   Out.Lobe2Offset = 0.05f;
		Out.CoreFade = 6.f;
		break;
	case EMLSPreset::Cinematic:
		Out.Lobe2Weight = 0.22f; Out.EnvLobe2Weight = 0.22f;
		Out.Lobe2Scale = 2.0f;   Out.Lobe2Offset = 0.10f;
		Out.CoreFade = 6.f;
		break;
	case EMLSPreset::Custom:
	default:
		Out.Lobe2Weight = Lobe2Weight; Out.EnvLobe2Weight = EnvLobe2Weight;
		Out.Lobe2Scale = Lobe2RoughnessScale; Out.Lobe2Offset = Lobe2RoughnessOffset;
		Out.CoreFade = CoreFade;
		break;
	}

	Out.bPatchEnvBRDF = bPatchEnvBRDF;
	Out.MicroShadowMode = static_cast<int32>(MicroShadowMode);
	Out.bMicroShadow = (MicroShadowMode != EMLSMicroShadowMode::Off);
	Out.MicroShadowDiffuseStrength = MicroShadowDiffuseStrength;
	Out.MicroShadowSpecularStrength = MicroShadowSpecularStrength;

	Out.bLumenDualBlur = bLumenDualBlur && Out.bBRDFEnabled;
	Out.LumenBlurWeight = Out.EnvLobe2Weight;
	Out.bTwoSampleIBL = bTwoSampleIBL && Out.bBRDFEnabled;
	Out.IndirectMaterialVisibilityMode = static_cast<int32>(IndirectMaterialVisibility);
	Out.bConeAwareIndirectSpecular = bConeAwareIndirectSpecular
		&& Out.bTwoSampleIBL
		&& Out.bPatchEnvBRDF
		&& IndirectMaterialVisibility == EMLSIndirectMaterialVisibility::RGBInterreflection;

	Out.bEnabled = Out.bBRDFEnabled
		|| Out.bMicroShadow
		|| IndirectMaterialVisibility == EMLSIndirectMaterialVisibility::RGBInterreflection;

	// Debug is session-only and excluded from the content-addressed overlay identity.
	Out.DebugView = Out.bEnabled ? static_cast<int32>(DebugView) : 0;
	Out.bPatchEnvBRDFApprox = bPatchEnvBRDF;
	Out.DiffuseModel = (DiffuseModel == EMLSDiffuse::Chan) ? 1 : 0;

	switch (Tonemapper)
	{
	default:
	case EMLSTonemap::EngineACES:   Out.TonemapMode = 0; break;
	case EMLSTonemap::AgX:          Out.TonemapMode = 1; break;
	case EMLSTonemap::AgXPunchy:    Out.TonemapMode = 2; break;
	case EMLSTonemap::GTUchimura:   Out.TonemapMode = 3; break;
	case EMLSTonemap::GTUchimuraHC: Out.TonemapMode = 4; break;
	}
}

void UMultiLobeSpecSettings::ApplyChanges()
{
	if (FModuleManager::Get().IsModuleLoaded("MultiLobeSpec"))
	{
		FMultiLobeSpecModule::Get().ApplyFromSettings();
	}
}

#if WITH_EDITOR
void UMultiLobeSpecSettings::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	if (Event.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UMultiLobeSpecSettings, DebugView)
		&& FModuleManager::Get().IsModuleLoaded("MultiLobeSpec"))
	{
		FMultiLobeSpecModule::Get().SetRuntimeDebugView(static_cast<int32>(DebugView), false);
		return;
	}

	UE_LOG(LogMultiLobeSpec, Log,
		TEXT("Settings changed (pending). Press 'Apply Changes' in Project Settings or run MLS.Apply."));
}
#endif
