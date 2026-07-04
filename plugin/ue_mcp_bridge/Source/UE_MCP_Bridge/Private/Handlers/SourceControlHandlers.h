#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"

// Revision control bridge (Perforce/Git/Plastic via ISourceControlModule).
// Exists because Perforce-managed projects reject saves on files that were
// not checked out first: agents mutating .uasset packages need status +
// checkout + add + revert without leaving the bridge. All operations run
// against whichever provider the editor has active; when none is enabled
// the handlers fail with a clear pointer at the Revision Control settings.
class FSourceControlHandlers
{
public:
	static void RegisterHandlers(class FMCPHandlerRegistry& Registry);

private:
	static TSharedPtr<FJsonValue> Status(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> CheckOut(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> MarkForAdd(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> Revert(const TSharedPtr<FJsonObject>& Params);
};
