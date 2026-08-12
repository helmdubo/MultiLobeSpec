using UnrealBuildTool;

public class MultiLobeSpecEditor : ModuleRules
{
	public MultiLobeSpecEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// EKeys/FKey symbols used by Slate input code live in InputCore.
		// Keep this explicit/public so modular editor DLL links cannot rely on
		// transitive Slate dependencies.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"InputCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"UnrealEd",
			"Blutility",
			"EditorScriptingUtilities",
			"AssetRegistry",
			"Json",
			"Projects",
			"RHI",
			"MultiLobeSpec"
		});
	}
}
