using UnrealBuildTool;

public class UE4SchemaExporter : ModuleRules
{
    public UE4SchemaExporter(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "Json" });
    }
}
