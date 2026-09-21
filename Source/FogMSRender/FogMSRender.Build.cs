using System.IO;
using UnrealBuildTool;

public class FogMSRender : ModuleRules
{
	public FogMSRender(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "RenderCore", "RHI" });
		PrivateDependencyModuleNames.AddRange(new[] { "CoreUObject", "Engine", "Projects", "Renderer" });
		// The separate spatial-lighting implementation reads the version-guarded Renderer interface.
		// The shadow-cache implementation itself uses only public Engine/RenderCore/RHI APIs.
		PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source", "Runtime", "Renderer", "Private"));
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("D3D12RHI");
			AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");
		}
	}
}
