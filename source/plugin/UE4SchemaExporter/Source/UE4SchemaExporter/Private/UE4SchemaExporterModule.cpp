#include "UE4SchemaExporterModule.h"

#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogUE4SchemaExporter, Log, All);

namespace
{
FString GetPropertyType(const FProperty* Property)
{
    FString ExtendedType;
    return Property ? Property->GetCPPType(&ExtendedType) + ExtendedType : TEXT("Unknown");
}
}

void FUE4SchemaExporterModule::StartupModule()
{
    ExportSchema();
}

void FUE4SchemaExporterModule::ShutdownModule()
{
}

void FUE4SchemaExporterModule::ExportSchema()
{
    const FString OutputDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SchemaExport"));
    IFileManager::Get().MakeDirectory(*OutputDirectory, true);
    const FString OutputPath = FPaths::Combine(OutputDirectory, TEXT("ue4_schema.json"));

    TArray<TSharedPtr<FJsonValue>> Classes;
    for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
    {
        UClass* Class = *ClassIt;
        if (!IsValid(Class))
        {
            continue;
        }

        TSharedRef<FJsonObject> ClassJson = MakeShared<FJsonObject>();
        ClassJson->SetStringField(TEXT("name"), Class->GetPathName());
        ClassJson->SetStringField(
            TEXT("super"),
            Class->GetSuperClass() ? Class->GetSuperClass()->GetPathName() : FString());
        ClassJson->SetNumberField(TEXT("size"), Class->GetStructureSize());

        TArray<TSharedPtr<FJsonValue>> Properties;
        for (TFieldIterator<FProperty> PropertyIt(Class, EFieldIteratorFlags::ExcludeSuper); PropertyIt; ++PropertyIt)
        {
            const FProperty* Property = *PropertyIt;
            TSharedRef<FJsonObject> PropertyJson = MakeShared<FJsonObject>();
            PropertyJson->SetStringField(TEXT("name"), Property->GetName());
            PropertyJson->SetStringField(TEXT("type"), GetPropertyType(Property));
            PropertyJson->SetNumberField(TEXT("offset"), Property->GetOffset_ForInternal());
            PropertyJson->SetNumberField(TEXT("element_size"), Property->ElementSize);
            PropertyJson->SetNumberField(TEXT("array_dim"), Property->ArrayDim);
            Properties.Add(MakeShared<FJsonValueObject>(PropertyJson));
        }
        ClassJson->SetArrayField(TEXT("properties"), Properties);

        TArray<TSharedPtr<FJsonValue>> Functions;
        for (TFieldIterator<UFunction> FunctionIt(Class, EFieldIteratorFlags::ExcludeSuper); FunctionIt; ++FunctionIt)
        {
            const UFunction* Function = *FunctionIt;
            TSharedRef<FJsonObject> FunctionJson = MakeShared<FJsonObject>();
            FunctionJson->SetStringField(TEXT("name"), Function->GetName());
            FunctionJson->SetNumberField(TEXT("flags"), static_cast<double>(Function->FunctionFlags));
            FunctionJson->SetNumberField(TEXT("parameter_size"), Function->ParmsSize);
            Functions.Add(MakeShared<FJsonValueObject>(FunctionJson));
        }
        ClassJson->SetArrayField(TEXT("functions"), Functions);
        Classes.Add(MakeShared<FJsonValueObject>(ClassJson));
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("format"), TEXT("ue4-reflection-schema-v1"));
    Root->SetStringField(TEXT("scope"), TEXT("type metadata only; no object values"));
    Root->SetArrayField(TEXT("classes"), Classes);

    FString JsonText;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
    if (!FJsonSerializer::Serialize(Root, Writer) || !FFileHelper::SaveStringToFile(JsonText, *OutputPath))
    {
        UE_LOG(LogUE4SchemaExporter, Error, TEXT("Could not write schema to %s"), *OutputPath);
        return;
    }

    UE_LOG(LogUE4SchemaExporter, Display, TEXT("Wrote reflected schema to %s"), *OutputPath);
}

IMPLEMENT_MODULE(FUE4SchemaExporterModule, UE4SchemaExporter)
