// Movie Render Queue driving - split from SequencerHandlers.cpp.
// All functions below are still members of FSequencerHandlers; this file is a
// translation-unit partition. Registration stays in SequencerHandlers.cpp.
//
// The MovieRenderPipeline plugin may not be enabled in a given project, so
// every type here is reached through reflection: classes via FindObject,
// methods via ProcessEvent on their BlueprintCallable UFUNCTIONs, and
// properties via the shared JSON property setter. No Build.cs dependency.

#include "SequencerHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"
#include "HandlerJsonProperty.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorSubsystem.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/StructOnScope.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	/** One reflection call: fills at most one object/class param and one bool
	 *  param positionally (the MRQ scripting API only has such signatures),
	 *  runs ProcessEvent, and reads an object/bool/float/string return. */
	struct FSimpleCall
	{
		UObject* ObjectArg = nullptr;
		bool bBoolArg = false;
		bool bHasBoolArg = false;

		UObject* ReturnObject = nullptr;
		bool bReturnBool = false;
		double ReturnNumber = 0.0;
		FString ReturnString;
	};

	bool CallSimple(UObject* Target, const TCHAR* FuncName, FSimpleCall& InOut, FString& OutError)
	{
		if (!Target)
		{
			OutError = FString::Printf(TEXT("%s: null target"), FuncName);
			return false;
		}
		UFunction* Func = Target->GetClass()->FindFunctionByName(FuncName);
		if (!Func)
		{
			OutError = FString::Printf(TEXT("Function %s not found on %s"), FuncName, *Target->GetClass()->GetName());
			return false;
		}

		FStructOnScope FuncParams(Func);
		uint8* Mem = FuncParams.GetStructMemory();

		for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm)) continue;
			if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(*It))
			{
				ObjProp->SetObjectPropertyValue_InContainer(Mem, InOut.ObjectArg);
			}
			else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(*It))
			{
				if (InOut.bHasBoolArg)
				{
					BoolProp->SetPropertyValue_InContainer(Mem, InOut.bBoolArg);
				}
			}
		}

		Target->ProcessEvent(Func, Mem);

		if (FProperty* ReturnProp = Func->GetReturnProperty())
		{
			if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(ReturnProp))
			{
				InOut.ReturnObject = ObjProp->GetObjectPropertyValue_InContainer(Mem);
			}
			else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(ReturnProp))
			{
				InOut.bReturnBool = BoolProp->GetPropertyValue_InContainer(Mem);
			}
			else if (FNumericProperty* NumProp = CastField<FNumericProperty>(ReturnProp))
			{
				InOut.ReturnNumber = NumProp->GetFloatingPointPropertyValue(NumProp->ContainerPtrToValuePtr<void>(Mem));
			}
			else if (FStrProperty* StrProp = CastField<FStrProperty>(ReturnProp))
			{
				InOut.ReturnString = StrProp->GetPropertyValue_InContainer(Mem);
			}
		}
		return true;
	}

	UObject* GetQueueSubsystem(FString& OutError)
	{
		UClass* SubsystemClass = FindObject<UClass>(nullptr, TEXT("/Script/MovieRenderPipelineEditor.MoviePipelineQueueSubsystem"));
		if (!SubsystemClass)
		{
			OutError = TEXT("Movie Render Pipeline plugin is not enabled in this project (enable 'Movie Render Queue' and restart the editor)");
			return nullptr;
		}
		if (!GEditor)
		{
			OutError = TEXT("Editor not available");
			return nullptr;
		}
		UObject* Subsystem = GEditor->GetEditorSubsystemBase(SubsystemClass);
		if (!Subsystem)
		{
			OutError = TEXT("MoviePipelineQueueSubsystem unavailable");
		}
		return Subsystem;
	}

	UObject* GetQueue(UObject* Subsystem, FString& OutError)
	{
		FSimpleCall Call;
		if (!CallSimple(Subsystem, TEXT("GetQueue"), Call, OutError)) return nullptr;
		if (!Call.ReturnObject) OutError = TEXT("GetQueue returned null");
		return Call.ReturnObject;
	}

	/** Read the queue's Jobs array through reflection. */
	TArray<UObject*> GetQueueJobs(UObject* Queue)
	{
		TArray<UObject*> Jobs;
		if (!Queue) return Jobs;
		if (FArrayProperty* JobsProp = CastField<FArrayProperty>(Queue->GetClass()->FindPropertyByName(TEXT("Jobs"))))
		{
			FScriptArrayHelper Helper(JobsProp, JobsProp->ContainerPtrToValuePtr<void>(Queue));
			if (FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(JobsProp->Inner))
			{
				for (int32 i = 0; i < Helper.Num(); ++i)
				{
					if (UObject* Job = Inner->GetObjectPropertyValue(Helper.GetRawPtr(i)))
					{
						Jobs.Add(Job);
					}
				}
			}
		}
		return Jobs;
	}

	/** Set a struct/string property on an object by importing text (used for
	 *  FSoftObjectPath job fields, which the JSON setter can't synthesize). */
	bool ImportPropertyText(UObject* Target, const TCHAR* PropName, const FString& Text, FString& OutError)
	{
		FProperty* Prop = Target->GetClass()->FindPropertyByName(PropName);
		if (!Prop)
		{
			OutError = FString::Printf(TEXT("Property %s not found on %s"), PropName, *Target->GetClass()->GetName());
			return false;
		}
		void* ValueAddr = Prop->ContainerPtrToValuePtr<void>(Target);
		if (!Prop->ImportText_Direct(*Text, ValueAddr, Target, PPF_None))
		{
			OutError = FString::Printf(TEXT("Failed to set %s = %s"), PropName, *Text);
			return false;
		}
		return true;
	}

	UClass* FindMrqClass(const TCHAR* Path, FString& OutError)
	{
		UClass* Class = FindObject<UClass>(nullptr, Path);
		if (!Class)
		{
			Class = LoadObject<UClass>(nullptr, Path);
		}
		if (!Class)
		{
			OutError = FString::Printf(TEXT("Class not found: %s (Movie Render Pipeline plugin enabled?)"), Path);
		}
		return Class;
	}

	UObject* FindOrAddSetting(UObject* Config, UClass* SettingClass, FString& OutError)
	{
		FSimpleCall Call;
		Call.ObjectArg = SettingClass;
		Call.bHasBoolArg = true;
		Call.bBoolArg = false; // bIncludeDisabledSettings
		if (!CallSimple(Config, TEXT("FindOrAddSettingByClass"), Call, OutError)) return nullptr;
		if (!Call.ReturnObject) OutError = FString::Printf(TEXT("FindOrAddSettingByClass(%s) returned null"), *SettingClass->GetName());
		return Call.ReturnObject;
	}
}

// editor(mrq_create_job): allocate a queue job for a Level Sequence with
// output settings, an image-sequence output format, and the deferred render
// pass - one call from sequence to render-ready job.
TSharedPtr<FJsonValue> FSequencerHandlers::MrqCreateJob(const TSharedPtr<FJsonObject>& Params)
{
	FString SequencePath;
	if (auto Err = RequireString(Params, TEXT("sequencePath"), SequencePath)) return Err;

	FString Error;
	UObject* Subsystem = GetQueueSubsystem(Error);
	if (!Subsystem) return MCPError(Error);
	UObject* Queue = GetQueue(Subsystem, Error);
	if (!Queue) return MCPError(Error);

	if (OptionalBool(Params, TEXT("clearQueue"), false))
	{
		for (UObject* Job : GetQueueJobs(Queue))
		{
			FSimpleCall Delete;
			Delete.ObjectArg = Job;
			CallSimple(Queue, TEXT("DeleteJob"), Delete, Error);
		}
	}

	UClass* JobClass = FindMrqClass(TEXT("/Script/MovieRenderPipeline.MoviePipelineExecutorJob"), Error);
	if (!JobClass) return MCPError(Error);

	FSimpleCall Allocate;
	Allocate.ObjectArg = JobClass;
	if (!CallSimple(Queue, TEXT("AllocateNewJob"), Allocate, Error)) return MCPError(Error);
	UObject* Job = Allocate.ReturnObject;
	if (!Job) return MCPError(TEXT("AllocateNewJob returned null"));

	// Bind the sequence and map. Map defaults to the currently loaded level.
	if (!ImportPropertyText(Job, TEXT("Sequence"), SequencePath, Error)) return MCPError(Error);
	FString MapPath = OptionalString(Params, TEXT("mapPath"), TEXT(""));
	if (MapPath.IsEmpty())
	{
		if (UWorld* World = GetEditorWorld())
		{
			MapPath = World->GetOutermost()->GetName();
		}
	}
	if (!MapPath.IsEmpty())
	{
		ImportPropertyText(Job, TEXT("Map"), MapPath, Error);
	}
	const FString JobName = OptionalString(Params, TEXT("jobName"), FPackageName::GetShortName(SequencePath));
	ImportPropertyText(Job, TEXT("JobName"), JobName, Error);

	FSimpleCall GetConfig;
	if (!CallSimple(Job, TEXT("GetConfiguration"), GetConfig, Error)) return MCPError(Error);
	UObject* Config = GetConfig.ReturnObject;
	if (!Config) return MCPError(TEXT("Job has no configuration"));

	// Output settings: directory, resolution, file name format.
	UClass* OutputSettingClass = FindMrqClass(TEXT("/Script/MovieRenderPipeline.MoviePipelineOutputSetting"), Error);
	if (!OutputSettingClass) return MCPError(Error);
	UObject* OutputSetting = FindOrAddSetting(Config, OutputSettingClass, Error);
	if (!OutputSetting) return MCPError(Error);

	const FString OutputDirectory = OptionalString(Params, TEXT("outputDirectory"), TEXT(""));
	if (!OutputDirectory.IsEmpty())
	{
		FString PropError;
		if (!MCPJsonProperty::SetDottedPropertyFromJson(OutputSetting, TEXT("OutputDirectory.Path"),
			MakeShared<FJsonValueString>(OutputDirectory), PropError))
		{
			return MCPError(FString::Printf(TEXT("OutputDirectory: %s"), *PropError));
		}
	}
	const int32 ResX = OptionalInt(Params, TEXT("resolutionX"), 0);
	const int32 ResY = OptionalInt(Params, TEXT("resolutionY"), 0);
	if (ResX > 0 && ResY > 0)
	{
		FString PropError;
		MCPJsonProperty::SetDottedPropertyFromJson(OutputSetting, TEXT("OutputResolution.X"), MakeShared<FJsonValueNumber>(ResX), PropError);
		MCPJsonProperty::SetDottedPropertyFromJson(OutputSetting, TEXT("OutputResolution.Y"), MakeShared<FJsonValueNumber>(ResY), PropError);
	}
	const FString FileNameFormat = OptionalString(Params, TEXT("fileNameFormat"), TEXT(""));
	if (!FileNameFormat.IsEmpty())
	{
		FString PropError;
		MCPJsonProperty::SetDottedPropertyFromJson(OutputSetting, TEXT("FileNameFormat"), MakeShared<FJsonValueString>(FileNameFormat), PropError);
	}

	// Image output format (default PNG) + the deferred render pass.
	const FString Format = OptionalString(Params, TEXT("imageFormat"), TEXT("png")).ToLower();
	const TCHAR* FormatClassPath =
		Format == TEXT("jpg") || Format == TEXT("jpeg") ? TEXT("/Script/MovieRenderPipeline.MoviePipelineImageSequenceOutput_JPG") :
		Format == TEXT("bmp") ? TEXT("/Script/MovieRenderPipeline.MoviePipelineImageSequenceOutput_BMP") :
		Format == TEXT("exr") ? TEXT("/Script/MovieRenderPipeline.MoviePipelineImageSequenceOutput_EXR") :
		TEXT("/Script/MovieRenderPipeline.MoviePipelineImageSequenceOutput_PNG");
	if (UClass* FormatClass = FindMrqClass(FormatClassPath, Error))
	{
		if (!FindOrAddSetting(Config, FormatClass, Error)) return MCPError(Error);
	}
	else
	{
		return MCPError(Error);
	}
	if (OptionalBool(Params, TEXT("addDeferredPass"), true))
	{
		if (UClass* PassClass = FindMrqClass(TEXT("/Script/MovieRenderPipeline.MoviePipelineDeferredPassBase"), Error))
		{
			if (!FindOrAddSetting(Config, PassClass, Error)) return MCPError(Error);
		}
	}

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("jobName"), JobName);
	Result->SetStringField(TEXT("sequencePath"), SequencePath);
	Result->SetStringField(TEXT("mapPath"), MapPath);
	Result->SetStringField(TEXT("imageFormat"), Format);
	if (!OutputDirectory.IsEmpty()) Result->SetStringField(TEXT("outputDirectory"), OutputDirectory);
	Result->SetNumberField(TEXT("jobsInQueue"), GetQueueJobs(Queue).Num());
	return MCPResult(Result);
}

// editor(mrq_render): render the queue with the PIE executor (in-process).
// Fire-and-forget: poll mrq_status for completion.
TSharedPtr<FJsonValue> FSequencerHandlers::MrqRender(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UObject* Subsystem = GetQueueSubsystem(Error);
	if (!Subsystem) return MCPError(Error);

	FSimpleCall Rendering;
	if (!CallSimple(Subsystem, TEXT("IsRendering"), Rendering, Error)) return MCPError(Error);
	if (Rendering.bReturnBool)
	{
		return MCPError(TEXT("A render is already in progress (mrq_status to watch it)"));
	}

	UObject* Queue = GetQueue(Subsystem, Error);
	if (!Queue) return MCPError(Error);
	if (GetQueueJobs(Queue).Num() == 0)
	{
		return MCPError(TEXT("Queue is empty - mrq_create_job first"));
	}

	UClass* ExecutorClass = FindMrqClass(TEXT("/Script/MovieRenderPipelineEditor.MoviePipelinePIEExecutor"), Error);
	if (!ExecutorClass) return MCPError(Error);

	FSimpleCall Render;
	Render.ObjectArg = ExecutorClass;
	if (!CallSimple(Subsystem, TEXT("RenderQueueWithExecutor"), Render, Error)) return MCPError(Error);
	if (!Render.ReturnObject)
	{
		return MCPError(TEXT("RenderQueueWithExecutor returned null (a modal dialog may be open, or another PIE session is running)"));
	}

	auto Result = MCPSuccess();
	Result->SetBoolField(TEXT("started"), true);
	Result->SetStringField(TEXT("executor"), Render.ReturnObject->GetClass()->GetName());
	Result->SetStringField(TEXT("note"), TEXT("Rendering runs via a PIE session; poll editor(mrq_status) until rendering=false"));
	return MCPResult(Result);
}

// editor(mrq_status): whether a render is running + per-job progress.
TSharedPtr<FJsonValue> FSequencerHandlers::MrqStatus(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UObject* Subsystem = GetQueueSubsystem(Error);
	if (!Subsystem) return MCPError(Error);

	FSimpleCall Rendering;
	if (!CallSimple(Subsystem, TEXT("IsRendering"), Rendering, Error)) return MCPError(Error);

	auto Result = MCPSuccess();
	Result->SetBoolField(TEXT("rendering"), Rendering.bReturnBool);

	UObject* Queue = GetQueue(Subsystem, Error);
	TArray<TSharedPtr<FJsonValue>> JobsArr;
	for (UObject* Job : GetQueueJobs(Queue))
	{
		TSharedPtr<FJsonObject> JobObj = MakeShared<FJsonObject>();
		if (FStrProperty* NameProp = CastField<FStrProperty>(Job->GetClass()->FindPropertyByName(TEXT("JobName"))))
		{
			JobObj->SetStringField(TEXT("jobName"), NameProp->GetPropertyValue_InContainer(Job));
		}
		FSimpleCall Progress;
		if (CallSimple(Job, TEXT("GetStatusProgress"), Progress, Error))
		{
			JobObj->SetNumberField(TEXT("progress"), Progress.ReturnNumber);
		}
		FSimpleCall Message;
		if (CallSimple(Job, TEXT("GetStatusMessage"), Message, Error))
		{
			JobObj->SetStringField(TEXT("statusMessage"), Message.ReturnString);
		}
		JobsArr.Add(MakeShared<FJsonValueObject>(JobObj));
	}
	Result->SetArrayField(TEXT("jobs"), JobsArr);
	return MCPResult(Result);
}

// editor(mrq_clear): delete every job in the queue.
TSharedPtr<FJsonValue> FSequencerHandlers::MrqClear(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UObject* Subsystem = GetQueueSubsystem(Error);
	if (!Subsystem) return MCPError(Error);
	UObject* Queue = GetQueue(Subsystem, Error);
	if (!Queue) return MCPError(Error);

	int32 Deleted = 0;
	for (UObject* Job : GetQueueJobs(Queue))
	{
		FSimpleCall Delete;
		Delete.ObjectArg = Job;
		if (CallSimple(Queue, TEXT("DeleteJob"), Delete, Error))
		{
			Deleted++;
		}
	}

	auto Result = MCPSuccess();
	Result->SetNumberField(TEXT("deleted"), Deleted);
	return MCPResult(Result);
}
