#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"

/**
 * VR / XR support. Everything goes through the engine's IXRTrackingSystem
 * interface (HeadMountedDisplay module), so the bridge works with any XR
 * runtime (OpenXR, OculusVR, SteamVR) and builds without the XRBase plugin.
 * Handlers report a clear "no XR system" error when no runtime is active.
 */
class FXRHandlers
{
public:
	static void RegisterHandlers(class FMCPHandlerRegistry& Registry);

private:
	/** XR runtime, HMD, stereo, tracking origin, world-to-meters, tracked
	 *  devices, enabled XR plugins, and key VR CVars in one snapshot. */
	static TSharedPtr<FJsonValue> GetXRStatus(const TSharedPtr<FJsonObject>& Params);
	/** Live HMD + motion controller poses (PIE world preferred). */
	static TSharedPtr<FJsonValue> GetXRPoses(const TSharedPtr<FJsonObject>& Params);
	/** Toggle the HMD + stereo rendering (e.g. drop out of VR mid-PIE). */
	static TSharedPtr<FJsonValue> EnableHMD(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SetTrackingOrigin(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SetWorldToMeters(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SetSpectatorScreenMode(const TSharedPtr<FJsonObject>& Params);
	/** Enumerate discovered Unreal plugins with enabled state (filterable) -
	 *  VR setup always starts with "is OpenXR enabled?". */
	static TSharedPtr<FJsonValue> ListUnrealPlugins(const TSharedPtr<FJsonObject>& Params);
};
