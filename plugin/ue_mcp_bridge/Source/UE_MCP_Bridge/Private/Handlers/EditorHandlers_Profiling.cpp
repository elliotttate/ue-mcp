// Unreal Insights trace capture + analysis - split from EditorHandlers.cpp.
// All functions below are still members of FEditorHandlers; this file is a
// translation-unit partition. Registration stays in EditorHandlers.cpp.
//
// start_trace/stop_trace wrap FTraceAuxiliary (write a .utrace to disk);
// analyze_trace loads the file through TraceServices and returns frame
// statistics as JSON, so an agent gets quantitative performance data with no
// external tooling. The TS server composes these into editor(profile_pie).

#include "EditorHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"

#include "ProfilingDebugging/TraceAuxiliary.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Bookmarks.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	TSharedPtr<FJsonObject> FrameStatsToJson(const TArray<double>& DurationsMs)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("frames"), DurationsMs.Num());
		if (DurationsMs.Num() == 0)
		{
			return Obj;
		}
		TArray<double> Sorted = DurationsMs;
		Sorted.Sort();
		double Sum = 0.0;
		int32 Hitches = 0;
		for (double D : Sorted)
		{
			Sum += D;
			if (D > 33.34) Hitches++;
		}
		auto Percentile = [&Sorted](double P) -> double
		{
			const int32 Index = FMath::Clamp(static_cast<int32>(P * (Sorted.Num() - 1)), 0, Sorted.Num() - 1);
			return Sorted[Index];
		};
		const double AvgMs = Sum / Sorted.Num();
		Obj->SetNumberField(TEXT("avgMs"), AvgMs);
		Obj->SetNumberField(TEXT("minMs"), Sorted[0]);
		Obj->SetNumberField(TEXT("maxMs"), Sorted.Last());
		Obj->SetNumberField(TEXT("p50Ms"), Percentile(0.50));
		Obj->SetNumberField(TEXT("p90Ms"), Percentile(0.90));
		Obj->SetNumberField(TEXT("p99Ms"), Percentile(0.99));
		Obj->SetNumberField(TEXT("avgFps"), AvgMs > 0.0 ? 1000.0 / AvgMs : 0.0);
		Obj->SetNumberField(TEXT("hitchesOver33ms"), Hitches);
		return Obj;
	}
}

// editor(start_trace): begin writing an Insights .utrace to disk.
TSharedPtr<FJsonValue> FEditorHandlers::StartTrace(const TSharedPtr<FJsonObject>& Params)
{
	if (FTraceAuxiliary::IsConnected())
	{
		return MCPError(FString::Printf(TEXT("A trace is already active (%s). Call stop_trace first."),
			*FTraceAuxiliary::GetTraceDestinationString()));
	}

	const FString Channels = OptionalString(Params, TEXT("channels"), TEXT("default,frame,bookmark"));
	FString OutputPath = OptionalString(Params, TEXT("outputPath"), TEXT(""));
	if (OutputPath.IsEmpty())
	{
		OutputPath = FPaths::ProfilingDir() / FString::Printf(TEXT("MCP_%s.utrace"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}
	if (FPaths::IsRelative(OutputPath))
	{
		OutputPath = FPaths::ProjectSavedDir() / OutputPath;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);

	if (!FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::File, *OutputPath, *Channels))
	{
		return MCPError(TEXT("FTraceAuxiliary::Start failed - see the Output Log (channel string invalid, or tracing disabled in this build?)"));
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("tracePath"), FTraceAuxiliary::GetTraceDestinationString());
	Result->SetStringField(TEXT("channels"), Channels);
	return MCPResult(Result);
}

// editor(stop_trace): stop the active trace and return the file path.
TSharedPtr<FJsonValue> FEditorHandlers::StopTrace(const TSharedPtr<FJsonObject>& Params)
{
	if (!FTraceAuxiliary::IsConnected())
	{
		return MCPError(TEXT("No trace is active"));
	}
	const FString TracePath = FTraceAuxiliary::GetTraceDestinationString();
	FTraceAuxiliary::Stop();

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("tracePath"), TracePath);
	return MCPResult(Result);
}

// editor(get_trace_status): whether a trace is running and where it writes.
TSharedPtr<FJsonValue> FEditorHandlers::GetTraceStatus(const TSharedPtr<FJsonObject>& Params)
{
	auto Result = MCPSuccess();
	const bool bConnected = FTraceAuxiliary::IsConnected();
	Result->SetBoolField(TEXT("tracing"), bConnected);
	if (bConnected)
	{
		Result->SetStringField(TEXT("tracePath"), FTraceAuxiliary::GetTraceDestinationString());
	}
	return MCPResult(Result);
}

// editor(analyze_trace): load a .utrace through TraceServices and return
// game/render frame statistics plus bookmarks. Analysis of a short capture
// completes in seconds; pass a generous call timeout for long traces.
TSharedPtr<FJsonValue> FEditorHandlers::AnalyzeTrace(const TSharedPtr<FJsonObject>& Params)
{
	FString TracePath;
	if (auto Err = RequireString(Params, TEXT("tracePath"), TracePath)) return Err;
	if (FPaths::IsRelative(TracePath))
	{
		TracePath = FPaths::ProjectSavedDir() / TracePath;
	}
	if (!IFileManager::Get().FileExists(*TracePath))
	{
		return MCPError(FString::Printf(TEXT("Trace file not found: %s"), *TracePath));
	}
	if (FTraceAuxiliary::IsConnected() && FTraceAuxiliary::GetTraceDestinationString() == TracePath)
	{
		return MCPError(TEXT("That trace is still being written - call stop_trace first"));
	}

	ITraceServicesModule& TraceServicesModule = FModuleManager::LoadModuleChecked<ITraceServicesModule>(TEXT("TraceServices"));
	TSharedPtr<TraceServices::IAnalysisService> AnalysisService = TraceServicesModule.GetAnalysisService();
	if (!AnalysisService.IsValid())
	{
		return MCPError(TEXT("TraceServices analysis service unavailable"));
	}

	TSharedPtr<const TraceServices::IAnalysisSession> Session = AnalysisService->StartAnalysis(*TracePath);
	if (!Session.IsValid())
	{
		return MCPError(FString::Printf(TEXT("Failed to open trace: %s"), *TracePath));
	}
	Session->Wait();

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("tracePath"), TracePath);
	{
		TraceServices::FAnalysisSessionReadScope ReadScope(*Session);
		Result->SetNumberField(TEXT("durationSeconds"), Session->GetDurationSeconds());

		const TraceServices::IFrameProvider& Frames = TraceServices::ReadFrameProvider(*Session);
		auto CollectFrames = [&Frames](ETraceFrameType FrameType)
		{
			TArray<double> DurationsMs;
			const uint64 Count = Frames.GetFrameCount(FrameType);
			Frames.EnumerateFrames(FrameType, 0, Count, [&DurationsMs](const TraceServices::FFrame& Frame)
			{
				DurationsMs.Add((Frame.EndTime - Frame.StartTime) * 1000.0);
			});
			return DurationsMs;
		};
		Result->SetObjectField(TEXT("gameFrames"), FrameStatsToJson(CollectFrames(TraceFrameType_Game)));
		Result->SetObjectField(TEXT("renderFrames"), FrameStatsToJson(CollectFrames(TraceFrameType_Rendering)));

		const TraceServices::IBookmarkProvider& Bookmarks = TraceServices::ReadBookmarkProvider(*Session);
		TArray<TSharedPtr<FJsonValue>> BookmarkArr;
		Bookmarks.EnumerateBookmarks(0.0, Session->GetDurationSeconds(), [&BookmarkArr](const TraceServices::FBookmark& Bookmark)
		{
			if (BookmarkArr.Num() >= 100) return;
			TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
			B->SetNumberField(TEXT("time"), Bookmark.Time);
			B->SetStringField(TEXT("text"), Bookmark.Text ? Bookmark.Text : TEXT(""));
			BookmarkArr.Add(MakeShared<FJsonValueObject>(B));
		});
		Result->SetArrayField(TEXT("bookmarks"), BookmarkArr);
	}

	// Release our reference; the analysis session tears down its workers.
	Session->Stop(true);

	return MCPResult(Result);
}
