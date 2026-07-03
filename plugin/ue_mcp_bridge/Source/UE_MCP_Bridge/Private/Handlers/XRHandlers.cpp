#include "XRHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "IXRTrackingSystem.h"
#include "IHeadMountedDisplay.h"
#include "ISpectatorScreenController.h"
#include "HeadMountedDisplayTypes.h"
#include "MotionControllerComponent.h"
#include "StereoRendering.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

void FXRHandlers::RegisterHandlers(FMCPHandlerRegistry& Registry)
{
	Registry.RegisterHandler(TEXT("get_xr_status"), &GetXRStatus);
	Registry.RegisterHandler(TEXT("get_xr_poses"), &GetXRPoses);
	Registry.RegisterHandler(TEXT("xr_enable_hmd"), &EnableHMD);
	Registry.RegisterHandler(TEXT("xr_set_tracking_origin"), &SetTrackingOrigin);
	Registry.RegisterHandler(TEXT("xr_set_world_to_meters"), &SetWorldToMeters);
	Registry.RegisterHandler(TEXT("xr_set_spectator_mode"), &SetSpectatorScreenMode);
	Registry.RegisterHandler(TEXT("list_unreal_plugins"), &ListUnrealPlugins);
}

namespace
{
	IXRTrackingSystem* GetXRSystem()
	{
		return GEngine ? GEngine->XRSystem.Get() : nullptr;
	}

	/** PIE world when running, else the editor world - XR state during a VR
	 *  Preview session lives on the play world. */
	UWorld* GetActiveXRWorld()
	{
		if (GEditor && GEditor->PlayWorld)
		{
			return GEditor->PlayWorld;
		}
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	TSharedPtr<FJsonObject> VectorToJson(const FVector& V)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}

	TSharedPtr<FJsonObject> RotatorToJson(const FRotator& R)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("pitch"), R.Pitch);
		O->SetNumberField(TEXT("yaw"), R.Yaw);
		O->SetNumberField(TEXT("roll"), R.Roll);
		return O;
	}

	const TCHAR* TrackingOriginToString(EHMDTrackingOrigin::Type Origin)
	{
		switch (Origin)
		{
		case EHMDTrackingOrigin::View: return TEXT("View");
		case EHMDTrackingOrigin::Local: return TEXT("Local");
		case EHMDTrackingOrigin::LocalFloor: return TEXT("LocalFloor");
		case EHMDTrackingOrigin::Stage: return TEXT("Stage");
		default: return TEXT("CustomOpenXR");
		}
	}
}

TSharedPtr<FJsonValue> FXRHandlers::GetXRStatus(const TSharedPtr<FJsonObject>& Params)
{
	auto Result = MCPSuccess();

	IXRTrackingSystem* XR = GetXRSystem();
	Result->SetBoolField(TEXT("xrSystemPresent"), XR != nullptr);
	if (XR)
	{
		Result->SetStringField(TEXT("systemName"), XR->GetSystemName().ToString());
		Result->SetBoolField(TEXT("headTrackingAllowed"), XR->IsHeadTrackingAllowed());
		Result->SetStringField(TEXT("trackingOrigin"), TrackingOriginToString(XR->GetTrackingOrigin()));

		if (IHeadMountedDisplay* HMD = XR->GetHMDDevice())
		{
			Result->SetBoolField(TEXT("hmdConnected"), HMD->IsHMDConnected());
			Result->SetBoolField(TEXT("hmdEnabled"), HMD->IsHMDEnabled());
		}
		else
		{
			Result->SetBoolField(TEXT("hmdConnected"), false);
			Result->SetBoolField(TEXT("hmdEnabled"), false);
		}
		Result->SetBoolField(TEXT("spectatorScreenAvailable"), XR->GetSpectatorScreenController() != nullptr);

		TArray<int32> DeviceIds;
		XR->EnumerateTrackedDevices(DeviceIds);
		Result->SetNumberField(TEXT("trackedDeviceCount"), DeviceIds.Num());
	}

	Result->SetBoolField(TEXT("stereoEnabled"),
		GEngine && GEngine->StereoRenderingDevice.IsValid() && GEngine->StereoRenderingDevice->IsStereoEnabled());

	if (UWorld* World = GetActiveXRWorld())
	{
		if (AWorldSettings* Settings = World->GetWorldSettings())
		{
			Result->SetNumberField(TEXT("worldToMeters"), Settings->WorldToMeters);
		}
	}
	Result->SetBoolField(TEXT("pieRunning"), GEditor && GEditor->PlayWorld != nullptr);

	// XR-relevant plugins - the usual "why doesn't VR preview show up" answer.
	TArray<TSharedPtr<FJsonValue>> XRPlugins;
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
	{
		const FString& PluginName = Plugin->GetName();
		const FString Category = Plugin->GetDescriptor().Category;
		if (PluginName.Contains(TEXT("XR")) || PluginName.Contains(TEXT("VR")) ||
			Category.Contains(TEXT("Virtual Reality")) || Category.Contains(TEXT("XR")))
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("name"), PluginName);
			P->SetBoolField(TEXT("enabled"), Plugin->IsEnabled());
			XRPlugins.Add(MakeShared<FJsonValueObject>(P));
		}
	}
	Result->SetArrayField(TEXT("xrPlugins"), XRPlugins);

	// Key VR rendering CVars.
	TSharedPtr<FJsonObject> CVars = MakeShared<FJsonObject>();
	const TCHAR* CVarNames[] = { TEXT("vr.PixelDensity"), TEXT("r.ForwardShading"), TEXT("r.InstancedStereo"), TEXT("r.MobileHDR"), TEXT("r.MSAACount") };
	for (const TCHAR* Name : CVarNames)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			CVars->SetStringField(Name, Var->GetString());
		}
	}
	Result->SetObjectField(TEXT("cvars"), CVars);

	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FXRHandlers::GetXRPoses(const TSharedPtr<FJsonObject>& Params)
{
	IXRTrackingSystem* XR = GetXRSystem();
	if (!XR)
	{
		return MCPError(TEXT("No XR system active (no XR runtime plugin enabled, or no HMD connected)"));
	}
	UWorld* World = GetActiveXRWorld();

	auto Result = MCPSuccess();

	FQuat HmdOrientation;
	FVector HmdPosition;
	if (XR->GetCurrentPose(IXRTrackingSystem::HMDDeviceId, HmdOrientation, HmdPosition))
	{
		TSharedPtr<FJsonObject> Hmd = MakeShared<FJsonObject>();
		Hmd->SetObjectField(TEXT("position"), VectorToJson(HmdPosition));
		Hmd->SetObjectField(TEXT("rotation"), RotatorToJson(HmdOrientation.Rotator()));
		Result->SetObjectField(TEXT("hmd"), Hmd);
	}

	// Motion controllers: every UMotionControllerComponent in the active world,
	// with its motion source, tracking state, and world transform.
	TArray<TSharedPtr<FJsonValue>> Controllers;
	for (TObjectIterator<UMotionControllerComponent> It; It; ++It)
	{
		UMotionControllerComponent* Controller = *It;
		if (!IsValid(Controller) || Controller->GetWorld() != World) continue;
		TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetStringField(TEXT("motionSource"), Controller->GetTrackingMotionSource().ToString());
		C->SetBoolField(TEXT("tracked"), Controller->IsTracked());
		C->SetObjectField(TEXT("position"), VectorToJson(Controller->GetComponentLocation()));
		C->SetObjectField(TEXT("rotation"), RotatorToJson(Controller->GetComponentRotation()));
		if (AActor* Owner = Controller->GetOwner())
		{
			C->SetStringField(TEXT("owner"), Owner->GetActorLabel());
		}
		Controllers.Add(MakeShared<FJsonValueObject>(C));
	}
	Result->SetArrayField(TEXT("motionControllers"), Controllers);
	Result->SetStringField(TEXT("world"), (GEditor && GEditor->PlayWorld) ? TEXT("pie") : TEXT("editor"));
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FXRHandlers::EnableHMD(const TSharedPtr<FJsonObject>& Params)
{
	IXRTrackingSystem* XR = GetXRSystem();
	if (!XR)
	{
		return MCPError(TEXT("No XR system active"));
	}
	IHeadMountedDisplay* HMD = XR->GetHMDDevice();
	if (!HMD)
	{
		return MCPError(TEXT("XR system has no HMD device"));
	}

	const bool bEnable = OptionalBool(Params, TEXT("enabled"), true);
	HMD->EnableHMD(bEnable);
	if (GEngine && GEngine->StereoRenderingDevice.IsValid())
	{
		GEngine->StereoRenderingDevice->EnableStereo(bEnable);
	}

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetBoolField(TEXT("hmdEnabled"), HMD->IsHMDEnabled());
	Result->SetBoolField(TEXT("stereoEnabled"),
		GEngine && GEngine->StereoRenderingDevice.IsValid() && GEngine->StereoRenderingDevice->IsStereoEnabled());
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FXRHandlers::SetTrackingOrigin(const TSharedPtr<FJsonObject>& Params)
{
	IXRTrackingSystem* XR = GetXRSystem();
	if (!XR)
	{
		return MCPError(TEXT("No XR system active"));
	}
	FString Origin;
	if (auto Err = RequireString(Params, TEXT("origin"), Origin)) return Err;

	EHMDTrackingOrigin::Type Type;
	if (Origin.Equals(TEXT("Local"), ESearchCase::IgnoreCase) || Origin.Equals(TEXT("Eye"), ESearchCase::IgnoreCase) || Origin.Equals(TEXT("Seated"), ESearchCase::IgnoreCase))
	{
		Type = EHMDTrackingOrigin::Local;
	}
	else if (Origin.Equals(TEXT("LocalFloor"), ESearchCase::IgnoreCase) || Origin.Equals(TEXT("Floor"), ESearchCase::IgnoreCase) || Origin.Equals(TEXT("Standing"), ESearchCase::IgnoreCase))
	{
		Type = EHMDTrackingOrigin::LocalFloor;
	}
	else if (Origin.Equals(TEXT("Stage"), ESearchCase::IgnoreCase) || Origin.Equals(TEXT("RoomScale"), ESearchCase::IgnoreCase))
	{
		Type = EHMDTrackingOrigin::Stage;
	}
	else if (Origin.Equals(TEXT("View"), ESearchCase::IgnoreCase))
	{
		Type = EHMDTrackingOrigin::View;
	}
	else
	{
		return MCPError(FString::Printf(TEXT("Unknown origin '%s'. Expected Local (seated), LocalFloor (standing), Stage (room-scale), or View."), *Origin));
	}

	XR->SetTrackingOrigin(Type);

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetStringField(TEXT("trackingOrigin"), TrackingOriginToString(XR->GetTrackingOrigin()));
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FXRHandlers::SetWorldToMeters(const TSharedPtr<FJsonObject>& Params)
{
	double WorldToMeters = 0.0;
	if (!Params->TryGetNumberField(TEXT("worldToMeters"), WorldToMeters) || WorldToMeters <= 0.0)
	{
		return MCPError(TEXT("Missing 'worldToMeters' (positive number; UE default is 100)"));
	}
	UWorld* World = GetActiveXRWorld();
	if (!World)
	{
		return MCPError(TEXT("No active world"));
	}
	AWorldSettings* Settings = World->GetWorldSettings();
	if (!Settings)
	{
		return MCPError(TEXT("World has no settings actor"));
	}
	Settings->WorldToMeters = static_cast<float>(WorldToMeters);

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetNumberField(TEXT("worldToMeters"), Settings->WorldToMeters);
	Result->SetStringField(TEXT("world"), (GEditor && GEditor->PlayWorld) ? TEXT("pie") : TEXT("editor"));
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FXRHandlers::SetSpectatorScreenMode(const TSharedPtr<FJsonObject>& Params)
{
	IXRTrackingSystem* XR = GetXRSystem();
	if (!XR)
	{
		return MCPError(TEXT("No XR system active"));
	}
	ISpectatorScreenController* Spectator = XR->GetSpectatorScreenController();
	if (!Spectator)
	{
		return MCPError(TEXT("XR system has no spectator screen controller"));
	}
	FString Mode;
	if (auto Err = RequireString(Params, TEXT("mode"), Mode)) return Err;

	static const TMap<FString, ESpectatorScreenMode> Modes = {
		{ TEXT("disabled"), ESpectatorScreenMode::Disabled },
		{ TEXT("singleeyeletterboxed"), ESpectatorScreenMode::SingleEyeLetterboxed },
		{ TEXT("undistorted"), ESpectatorScreenMode::Undistorted },
		{ TEXT("distorted"), ESpectatorScreenMode::Distorted },
		{ TEXT("singleeye"), ESpectatorScreenMode::SingleEye },
		{ TEXT("singleeyecroppedtofill"), ESpectatorScreenMode::SingleEyeCroppedToFill },
		{ TEXT("texture"), ESpectatorScreenMode::Texture },
		{ TEXT("texturepluseye"), ESpectatorScreenMode::TexturePlusEye },
	};
	const ESpectatorScreenMode* Found = Modes.Find(Mode.ToLower());
	if (!Found)
	{
		return MCPError(FString::Printf(TEXT("Unknown mode '%s'. Expected Disabled, SingleEyeLetterboxed, Undistorted, Distorted, SingleEye, SingleEyeCroppedToFill, Texture, or TexturePlusEye."), *Mode));
	}
	Spectator->SetSpectatorScreenMode(*Found);

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetStringField(TEXT("mode"), Mode);
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FXRHandlers::ListUnrealPlugins(const TSharedPtr<FJsonObject>& Params)
{
	const FString Filter = OptionalString(Params, TEXT("filter"), TEXT(""));
	const bool bEnabledOnly = OptionalBool(Params, TEXT("enabledOnly"), false);

	TArray<TSharedPtr<FJsonValue>> Plugins;
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
	{
		if (bEnabledOnly && !Plugin->IsEnabled()) continue;
		const FPluginDescriptor& Descriptor = Plugin->GetDescriptor();
		if (!Filter.IsEmpty() &&
			!Plugin->GetName().Contains(Filter) &&
			!Descriptor.Category.Contains(Filter) &&
			!Descriptor.FriendlyName.Contains(Filter))
		{
			continue;
		}
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("name"), Plugin->GetName());
		P->SetStringField(TEXT("friendlyName"), Descriptor.FriendlyName);
		P->SetStringField(TEXT("category"), Descriptor.Category);
		P->SetBoolField(TEXT("enabled"), Plugin->IsEnabled());
		P->SetBoolField(TEXT("enabledByDefault"), Descriptor.bEnabledByDefault);
		P->SetStringField(TEXT("location"),
			Plugin->GetType() == EPluginType::Engine ? TEXT("engine") :
			Plugin->GetType() == EPluginType::Project ? TEXT("project") : TEXT("other"));
		Plugins.Add(MakeShared<FJsonValueObject>(P));
		if (Plugins.Num() >= 500) break;
	}

	auto Result = MCPSuccess();
	Result->SetNumberField(TEXT("count"), Plugins.Num());
	Result->SetArrayField(TEXT("plugins"), Plugins);
	return MCPResult(Result);
}
