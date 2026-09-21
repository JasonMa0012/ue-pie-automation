using UnrealBuildTool;

public class UEPIEAutomation : ModuleRules
{
	public UEPIEAutomation(ReadOnlyTargetRules Target) : base(Target)
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
