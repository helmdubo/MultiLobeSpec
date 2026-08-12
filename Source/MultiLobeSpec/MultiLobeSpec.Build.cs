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
			"Json",
			"RenderCore",      // AddShaderSourceDirectoryMapping / FlushShaderFileCache
			"Projects",
			"DeveloperSettings"
		});
	}
}
