using UnrealBuildTool;

public class MultiLobeSpec : ModuleRules
{
	public MultiLobeSpec(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"FogMSRender",
			"RenderCore",      // FogMS Box RDG passes; editor overlay: AddShaderSourceDirectoryMapping / FlushShaderFileCache
			"RHI",             // Plugin-owned live FogMS Box texture/SRV
			"DeveloperSettings"
		});

		// The engine-shader overlay (MultiLobeShaderPatcher, MLSRawMaterialVisibilityOverlay, the MLS/FogMS console
		// tools and the apply-failure toast) is compiled only with WITH_EDITOR, so its modules are editor-target only.
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"Json",        // Overlay capability manifest / LUT admission
				"Projects",    // IPluginManager: plugin Shaders/ and LUT artifact paths
				"Slate",       // FSlateNotificationManager: apply-failure toast
				"SlateCore"
			});
		}
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("D3D12RHI");
			AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");
		}
	}
}
