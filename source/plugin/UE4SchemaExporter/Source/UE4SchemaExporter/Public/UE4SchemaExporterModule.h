#pragma once

#include "Modules/ModuleManager.h"

class FUE4SchemaExporterModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void ExportSchema();
};
