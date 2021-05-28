// Copyright Gunfire Games, LLC. All Rights Reserved.

using UnrealBuildTool;

public class GunfireSaveSystem : ModuleRules
{
	public GunfireSaveSystem(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
		);
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"DeveloperSettings",
				"Engine",
			}
		);

		if (Target.bBuildEditor == true)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}

		// Define a macro for debug game compiles
		if (Target.Configuration == UnrealTargetConfiguration.DebugGame)
		{
			PrivateDefinitions.Add("UE_BUILD_DEBUGGAME=1");
		}
		else
		{
			PrivateDefinitions.Add("UE_BUILD_DEBUGGAME=0");
		}
	}
}
