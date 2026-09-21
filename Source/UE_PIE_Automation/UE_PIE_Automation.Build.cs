using UnrealBuildTool;

public class UE_PIE_Automation : ModuleRules
{
	public UE_PIE_Automation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Json",
				"JsonUtilities",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"ApplicationCore",
				"AssetRegistry",
				"EditorScriptingUtilities",
				"EditorStyle",
				"EnhancedInput",
				"ImageWrapper",
				"InputCore",
				"Kismet",
				"LevelEditor",
				"ModelContextProtocol",
				"RenderCore",
				"RHI",
				"Projects",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"UnrealEd",
				"WorkspaceMenuStructure",
			}
		);
	}
}
