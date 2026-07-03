#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"

class FSequencerHandlers
{
public:
	// Register all sequencer handlers
	static void RegisterHandlers(class FMCPHandlerRegistry& Registry);

private:
	// Handler implementations
	static TSharedPtr<FJsonValue> CreateLevelSequence(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> ReadSequenceInfo(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> AddTrack(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SequenceControl(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SetPlaybackRange(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> AddSection(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SetKeyframes(const TSharedPtr<FJsonObject>& Params);

	// Movie Render Queue (SequencerHandlers_MRQ.cpp). The MovieRenderPipeline
	// plugin is reached entirely through reflection (FindObject + ProcessEvent)
	// so the bridge builds and loads without it; handlers fail with a clear
	// "plugin not enabled" error instead of a link-time break.
	static TSharedPtr<FJsonValue> MrqCreateJob(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> MrqRender(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> MrqStatus(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> MrqClear(const TSharedPtr<FJsonObject>& Params);
};
