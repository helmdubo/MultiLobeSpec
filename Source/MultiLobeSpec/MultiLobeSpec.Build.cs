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
			"Json",
			"RenderCore",      // AddShaderSourceDirectoryMapping / FlushShaderFileCache
			"RHI",             // Plugin-owned live FogMS Box texture/SRV
			"Projects",
			"DeveloperSettings",
			"Slate",           // FSlateNotificationManager: apply-failure toast
			"SlateCore"
		});
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("D3D12RHI");
			AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");
		}
	}
}
