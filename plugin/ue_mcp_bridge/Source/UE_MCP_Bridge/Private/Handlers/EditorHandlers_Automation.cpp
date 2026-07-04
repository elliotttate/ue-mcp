// UE Automation Framework runner: start_automation_tests + get_automation_test_status.
//
// The automation controller is asynchronous (worker discovery and test
// execution progress over MessageBus + latent commands ticked per frame), so
// handlers never block. start_automation_tests kicks off a state machine
// driven by a core ticker; get_automation_test_status polls it. The TS server
// orchestrates the wait (mirrors the wait_for_pie_event pattern).

#include "EditorHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"

#include "IAutomationControllerModule.h"
#include "IAutomationControllerManager.h"
#include "IAutomationReport.h"
#include "AutomationState.h"
#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"
#include "Misc/App.h"
#include "HAL/PlatformTime.h"

namespace
{
	IAutomationControllerManagerPtr GetAutomationManager()
	{
		IAutomationControllerModule& Module =
			FModuleManager::LoadModuleChecked<IAutomationControllerModule>(TEXT("AutomationController"));
		return Module.GetAutomationController();
	}

	const TCHAR* AutomationStateLabel(EAutomationState State)
	{
		switch (State)
		{
		case EAutomationState::NotRun:    return TEXT("NotRun");
		case EAutomationState::InProcess: return TEXT("InProcess");
		case EAutomationState::Fail:      return TEXT("Fail");
		case EAutomationState::Success:   return TEXT("Success");
		case EAutomationState::Skipped:   return TEXT("Skipped");
		default:                          return TEXT("Unknown");
		}
	}

	const TCHAR* AutomationEventTypeLabel(EAutomationEventType Type)
	{
		switch (Type)
		{
		case EAutomationEventType::Error:   return TEXT("Error");
		case EAutomationEventType::Warning: return TEXT("Warning");
		default:                            return TEXT("Info");
		}
	}

	// Single in-flight run. Handlers and the ticker both execute on the game
	// thread, so plain members are safe.
	struct FMCPAutomationRun
	{
		bool bActive = false;
		bool bListOnly = false;
		FString Phase = TEXT("idle"); // finding_workers | running | complete | failed
		FString Error;
		TArray<FString> Filters;
		TArray<FString> DiscoveredTests;
		TArray<FString> EnabledTests;
		double StartTimeSeconds = 0.0;
		double TimeoutSeconds = 600.0;
		double WorkerRetrySeconds = 0.0;
		int32 FindWorkerAttempts = 0;
		FTSTicker::FDelegateHandle TickerHandle;
		FDelegateHandle RefreshedHandle;
		TSharedPtr<FJsonObject> Results;

		void Cleanup()
		{
			if (TickerHandle.IsValid())
			{
				FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
			if (RefreshedHandle.IsValid())
			{
				if (IAutomationControllerManagerPtr Manager = GetAutomationManager())
				{
					Manager->OnTestsRefreshed().Remove(RefreshedHandle);
				}
				RefreshedHandle.Reset();
			}
		}

		void Finish(const FString& InPhase, const FString& InError = FString())
		{
			Phase = InPhase;
			Error = InError;
			bActive = false;
			Cleanup();
		}
	};

	FMCPAutomationRun GAutomationRun;

	bool MatchesAnyFilter(const FString& TestName, const TArray<FString>& Filters)
	{
		if (Filters.Num() == 0) return true;
		for (const FString& Filter : Filters)
		{
			if (TestName.Contains(Filter, ESearchCase::IgnoreCase)) return true;
		}
		return false;
	}

	TSharedPtr<FJsonObject> CollectAutomationResults(IAutomationControllerManagerPtr Manager)
	{
		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		int32 Passed = 0, Failed = 0, Skipped = 0, NotRun = 0;
		double TotalDuration = 0.0;

		TArray<TSharedPtr<FJsonValue>> Rows;
		TArray<TSharedPtr<IAutomationReport>> Reports = Manager->GetEnabledReports();
		for (const TSharedPtr<IAutomationReport>& Report : Reports)
		{
			if (!Report.IsValid()) continue;

			const EAutomationState State = Report->GetState(0, 0);
			switch (State)
			{
			case EAutomationState::Success: Passed++; break;
			case EAutomationState::Fail:    Failed++; break;
			case EAutomationState::Skipped: Skipped++; break;
			default:                        NotRun++; break;
			}

			const FAutomationTestResults& TestResults = Report->GetResults(0, 0);
			TotalDuration += TestResults.Duration;

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Report->GetDisplayName());
			Row->SetStringField(TEXT("fullPath"), Report->GetFullTestPath());
			Row->SetStringField(TEXT("state"), AutomationStateLabel(State));
			Row->SetNumberField(TEXT("durationSeconds"), TestResults.Duration);
			Row->SetNumberField(TEXT("errors"), TestResults.GetErrorTotal());
			Row->SetNumberField(TEXT("warnings"), TestResults.GetWarningTotal());

			// Only ship event entries for problem tests; a green suite's info
			// spam would dwarf the payload.
			if (State == EAutomationState::Fail || TestResults.GetErrorTotal() > 0 || TestResults.GetWarningTotal() > 0)
			{
				constexpr int32 MaxEntries = 25;
				const TArray<FAutomationExecutionEntry>& Entries = TestResults.GetEntries();
				TArray<TSharedPtr<FJsonValue>> EntryRows;
				for (const FAutomationExecutionEntry& Entry : Entries)
				{
					if (Entry.Event.Type == EAutomationEventType::Info) continue;
					TSharedPtr<FJsonObject> EntryRow = MakeShared<FJsonObject>();
					EntryRow->SetStringField(TEXT("type"), AutomationEventTypeLabel(Entry.Event.Type));
					EntryRow->SetStringField(TEXT("message"), Entry.Event.Message);
					if (!Entry.Filename.IsEmpty())
					{
						EntryRow->SetStringField(TEXT("file"), Entry.Filename);
						EntryRow->SetNumberField(TEXT("line"), Entry.LineNumber);
					}
					EntryRows.Add(MakeShared<FJsonValueObject>(EntryRow));
					if (EntryRows.Num() >= MaxEntries)
					{
						break;
					}
				}
				if (EntryRows.Num() > 0)
				{
					Row->SetArrayField(TEXT("events"), EntryRows);
					if (Entries.Num() > MaxEntries) Row->SetBoolField(TEXT("eventsTruncated"), true);
				}
			}
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}

		Summary->SetNumberField(TEXT("total"), Rows.Num());
		Summary->SetNumberField(TEXT("passed"), Passed);
		Summary->SetNumberField(TEXT("failed"), Failed);
		Summary->SetNumberField(TEXT("skipped"), Skipped);
		Summary->SetNumberField(TEXT("notRun"), NotRun);
		Summary->SetNumberField(TEXT("totalDurationSeconds"), TotalDuration);
		Summary->SetBoolField(TEXT("allPassed"), Failed == 0 && NotRun == 0 && Rows.Num() > 0);

		TSharedPtr<FJsonObject> Results = MakeShared<FJsonObject>();
		Results->SetObjectField(TEXT("summary"), Summary);
		Results->SetArrayField(TEXT("tests"), Rows);
		return Results;
	}

	// OnTestsRefreshed: workers responded with the test list. Select and run.
	void HandleTestsRefreshed()
	{
		FMCPAutomationRun& Run = GAutomationRun;
		if (!Run.bActive || Run.Phase != TEXT("finding_workers")) return;

		IAutomationControllerManagerPtr Manager = GetAutomationManager();
		if (!Manager.IsValid() || Manager->GetNumDeviceClusters() == 0) return;

		// An empty filter collection still has to be applied, otherwise the
		// manager reports no visible tests (same dance as the Automation exec).
		TSharedPtr<AutomationFilterCollection> Filters = MakeShareable(new AutomationFilterCollection());
		Manager->SetFilter(Filters);
		Manager->SetVisibleTestsEnabled(true);

		Run.DiscoveredTests.Reset();
		Manager->GetEnabledTestNames(Run.DiscoveredTests);

		if (Run.bListOnly)
		{
			TArray<TSharedPtr<FJsonValue>> Names;
			for (const FString& Name : Run.DiscoveredTests)
			{
				if (MatchesAnyFilter(Name, Run.Filters))
				{
					Names.Add(MakeShared<FJsonValueString>(Name));
				}
			}
			TSharedPtr<FJsonObject> Results = MakeShared<FJsonObject>();
			Results->SetArrayField(TEXT("testNames"), Names);
			Results->SetNumberField(TEXT("count"), Names.Num());
			Run.Results = Results;
			Run.Finish(TEXT("complete"));
			return;
		}

		Run.EnabledTests.Reset();
		for (const FString& Name : Run.DiscoveredTests)
		{
			if (MatchesAnyFilter(Name, Run.Filters))
			{
				Run.EnabledTests.Add(Name);
			}
		}
		if (Run.EnabledTests.Num() == 0)
		{
			Run.Finish(TEXT("failed"), FString::Printf(
				TEXT("No automation tests matched the filter (%d discovered)"), Run.DiscoveredTests.Num()));
			return;
		}

		Manager->StopTests();
		Manager->SetEnabledTests(Run.EnabledTests);

		// Drop the refresh delegate before running so a mid-run refresh can't
		// restart selection (mirrors the engine's commandline runner).
		if (Run.RefreshedHandle.IsValid())
		{
			Manager->OnTestsRefreshed().Remove(Run.RefreshedHandle);
			Run.RefreshedHandle.Reset();
		}

		Manager->RunTests();
		Run.Phase = TEXT("running");
	}

	bool TickAutomationRun(float DeltaTime)
	{
		FMCPAutomationRun& Run = GAutomationRun;
		if (!Run.bActive) return false;

		IAutomationControllerManagerPtr Manager = GetAutomationManager();
		if (!Manager.IsValid())
		{
			Run.Finish(TEXT("failed"), TEXT("AutomationController module unavailable"));
			return false;
		}

		Manager->Tick();

		const double Elapsed = FPlatformTime::Seconds() - Run.StartTimeSeconds;
		if (Elapsed > Run.TimeoutSeconds)
		{
			Manager->StopTests();
			// Salvage whatever finished before the deadline.
			Run.Results = CollectAutomationResults(Manager);
			Run.Finish(TEXT("failed"), FString::Printf(TEXT("Timed out after %.0fs"), Run.TimeoutSeconds));
			return false;
		}

		if (Run.Phase == TEXT("finding_workers"))
		{
			Run.WorkerRetrySeconds -= DeltaTime;
			if (Run.WorkerRetrySeconds <= 0.0)
			{
				if (Run.FindWorkerAttempts >= 6)
				{
					Run.Finish(TEXT("failed"), TEXT("No automation worker responded (is the editor fully initialized?)"));
					return false;
				}
				Manager->RequestAvailableWorkers(FApp::GetSessionId());
				Run.FindWorkerAttempts++;
				Run.WorkerRetrySeconds = 10.0;
			}
		}
		else if (Run.Phase == TEXT("running"))
		{
			if (Manager->GetTestState() != EAutomationControllerModuleState::Running)
			{
				Run.Results = CollectAutomationResults(Manager);
				Run.Finish(TEXT("complete"));
				return false;
			}
		}

		return true;
	}
}

TSharedPtr<FJsonValue> FEditorHandlers::StartAutomationTests(const TSharedPtr<FJsonObject>& Params)
{
	FMCPAutomationRun& Run = GAutomationRun;
	if (Run.bActive)
	{
		return MCPError(FString::Printf(
			TEXT("An automation run is already active (phase: %s). Poll get_automation_test_status or wait for it to finish."),
			*Run.Phase));
	}

	TArray<FString> Filters;
	const FString SingleFilter = OptionalString(Params, TEXT("filter"));
	if (!SingleFilter.IsEmpty()) Filters.Add(SingleFilter);
	const TArray<TSharedPtr<FJsonValue>>* FiltersArr = nullptr;
	if (Params->TryGetArrayField(TEXT("filters"), FiltersArr))
	{
		Filters.Append(JsonArrayToStringList(FiltersArr));
	}
	const bool bListOnly = OptionalBool(Params, TEXT("listOnly"), false);
	const bool bRunAll = OptionalBool(Params, TEXT("runAll"), false);
	if (!bListOnly && Filters.Num() == 0 && !bRunAll)
	{
		return MCPError(TEXT("Supply 'filter'/'filters' to select tests, or runAll=true to run every test (may take a long time)"));
	}

	IAutomationControllerManagerPtr Manager = GetAutomationManager();
	if (!Manager.IsValid())
	{
		return MCPError(TEXT("AutomationController module unavailable"));
	}

	Run = FMCPAutomationRun();
	Run.bActive = true;
	Run.bListOnly = bListOnly;
	Run.Phase = TEXT("finding_workers");
	Run.Filters = MoveTemp(Filters);
	Run.StartTimeSeconds = FPlatformTime::Seconds();
	Run.TimeoutSeconds = FMath::Clamp(OptionalNumber(Params, TEXT("timeoutSeconds"), 600.0), 10.0, 7200.0);

	Run.RefreshedHandle = Manager->OnTestsRefreshed().AddStatic(&HandleTestsRefreshed);
	Run.TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateStatic(&TickAutomationRun));

	auto Result = MCPSuccess();
	Result->SetBoolField(TEXT("started"), true);
	Result->SetBoolField(TEXT("listOnly"), bListOnly);
	Result->SetNumberField(TEXT("timeoutSeconds"), Run.TimeoutSeconds);
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FEditorHandlers::GetAutomationTestStatus(const TSharedPtr<FJsonObject>& Params)
{
	FMCPAutomationRun& Run = GAutomationRun;

	auto Result = MCPSuccess();
	Result->SetBoolField(TEXT("active"), Run.bActive);
	Result->SetStringField(TEXT("phase"), Run.Phase);
	if (!Run.Error.IsEmpty()) Result->SetStringField(TEXT("error"), Run.Error);
	if (Run.bActive)
	{
		Result->SetNumberField(TEXT("elapsedSeconds"), FPlatformTime::Seconds() - Run.StartTimeSeconds);
	}
	if (Run.DiscoveredTests.Num() > 0)
	{
		Result->SetNumberField(TEXT("discoveredCount"), Run.DiscoveredTests.Num());
	}
	if (Run.EnabledTests.Num() > 0)
	{
		Result->SetNumberField(TEXT("enabledCount"), Run.EnabledTests.Num());
	}
	if (Run.Results.IsValid())
	{
		Result->SetObjectField(TEXT("results"), Run.Results);
	}
	return MCPResult(Result);
}
