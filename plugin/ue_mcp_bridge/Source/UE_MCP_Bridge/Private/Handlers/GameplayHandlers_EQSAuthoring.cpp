// Environment Query System authoring - split from GameplayHandlers.cpp.
// All functions below are still members of FGameplayHandlers; this file is a
// translation-unit partition. Registration stays in GameplayHandlers.cpp.
//
// Same strategy as behavior tree authoring: mutate the runtime asset
// (UEnvQuery::Options - each option pairs a generator with its tests) and null
// the editor-only EdGraph; the EQS editor rebuilds its graph from the runtime
// data on next open (UEnvironmentQueryGraph::SpawnMissingNodes).

#include "GameplayHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"
#include "HandlerJsonProperty.h"

#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/EnvQueryContext.h"
#include "EditorAssetLibrary.h"
#include "UObject/UObjectIterator.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	UEnvQuery* LoadEnvQuery(const TSharedPtr<FJsonObject>& Params, FString& OutPath, FString& OutError)
	{
		if (auto Err = RequireStringAlt(Params, TEXT("path"), TEXT("assetPath"), OutPath))
		{
			OutError = TEXT("Missing 'assetPath'");
			return nullptr;
		}
		UEnvQuery* Query = LoadObject<UEnvQuery>(nullptr, *OutPath);
		if (!Query)
		{
			OutError = FString::Printf(TEXT("EnvQuery not found: %s"), *OutPath);
		}
		return Query;
	}

	UClass* ResolveEQSClass(const FString& Token, UClass* BaseClass, const TCHAR* Prefix, FString& OutError)
	{
		UClass* Found = nullptr;
		if (Token.Contains(TEXT("/")))
		{
			Found = LoadClass<UObject>(nullptr, *Token);
			if (!Found && !Token.EndsWith(TEXT("_C")))
			{
				Found = LoadClass<UObject>(nullptr, *(Token + TEXT("_C")));
			}
		}
		if (!Found)
		{
			const FString Prefixed = Prefix + Token;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (!It->IsChildOf(BaseClass) || It->HasAnyClassFlags(CLASS_Abstract)) continue;
				if (It->GetName() == Token || It->GetName() == Prefixed)
				{
					Found = *It;
					break;
				}
			}
		}
		if (!Found || !Found->IsChildOf(BaseClass))
		{
			OutError = FString::Printf(TEXT("Class '%s' not found or not a %s subclass (gameplay list_eqs_classes to discover)"), *Token, *BaseClass->GetName());
			return nullptr;
		}
		return Found;
	}

	TArray<TSharedPtr<FJsonValue>> ApplyEQSProperties(UObject* Target, const TSharedPtr<FJsonObject>& Params)
	{
		TArray<TSharedPtr<FJsonValue>> Errors;
		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (!Params->TryGetObjectField(TEXT("properties"), Props) || !Props->IsValid())
		{
			return Errors;
		}
		for (const auto& Pair : (*Props)->Values)
		{
			FString Error;
			if (!MCPJsonProperty::SetDottedPropertyFromJson(Target, Pair.Key, Pair.Value, Error))
			{
				Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: %s"), *Pair.Key, *Error)));
			}
		}
		return Errors;
	}

	void FinalizeEQSEdit(UEnvQuery* Query)
	{
#if WITH_EDITORONLY_DATA
		Query->EdGraph = nullptr;
#endif
		Query->MarkPackageDirty();
		UEditorAssetLibrary::SaveAsset(Query->GetPathName());
	}
}

// gameplay(add_eqs_option): append an option (generator + empty test list).
TSharedPtr<FJsonValue> FGameplayHandlers::AddEQSOption(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Error;
	UEnvQuery* Query = LoadEnvQuery(Params, Path, Error);
	if (!Query) return MCPError(Error);

	FString GeneratorClass;
	if (auto Err = RequireString(Params, TEXT("generatorClass"), GeneratorClass)) return Err;
	UClass* GenClass = ResolveEQSClass(GeneratorClass, UEnvQueryGenerator::StaticClass(), TEXT("EnvQueryGenerator_"), Error);
	if (!GenClass) return MCPError(Error);

	UEnvQueryOption* Option = NewObject<UEnvQueryOption>(Query, UEnvQueryOption::StaticClass(), NAME_None, RF_Transactional);
	UEnvQueryGenerator* Generator = NewObject<UEnvQueryGenerator>(Option, GenClass, NAME_None, RF_Transactional);
	Option->Generator = Generator;
	Query->GetOptionsMutable().Add(Option);

	const TArray<TSharedPtr<FJsonValue>> PropErrors = ApplyEQSProperties(Generator, Params);
	FinalizeEQSEdit(Query);

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("assetPath"), Path);
	Result->SetNumberField(TEXT("optionIndex"), Query->GetOptions().Num() - 1);
	Result->SetStringField(TEXT("generatorClass"), GenClass->GetName());
	if (PropErrors.Num() > 0) Result->SetArrayField(TEXT("propertyErrors"), PropErrors);
	return MCPResult(Result);
}

// gameplay(add_eqs_test): append a test to an option's test list.
TSharedPtr<FJsonValue> FGameplayHandlers::AddEQSTest(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Error;
	UEnvQuery* Query = LoadEnvQuery(Params, Path, Error);
	if (!Query) return MCPError(Error);

	FString TestClass;
	if (auto Err = RequireString(Params, TEXT("testClass"), TestClass)) return Err;
	UClass* Class = ResolveEQSClass(TestClass, UEnvQueryTest::StaticClass(), TEXT("EnvQueryTest_"), Error);
	if (!Class) return MCPError(Error);

	const int32 OptionIndex = OptionalInt(Params, TEXT("optionIndex"), 0);
	TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
	if (!Options.IsValidIndex(OptionIndex))
	{
		return MCPError(FString::Printf(TEXT("optionIndex %d out of range (options=%d; add_eqs_option first)"), OptionIndex, Options.Num()));
	}

	UEnvQueryTest* Test = NewObject<UEnvQueryTest>(Options[OptionIndex], Class, NAME_None, RF_Transactional);
	Options[OptionIndex]->Tests.Add(Test);

	const TArray<TSharedPtr<FJsonValue>> PropErrors = ApplyEQSProperties(Test, Params);
	FinalizeEQSEdit(Query);

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("assetPath"), Path);
	Result->SetNumberField(TEXT("optionIndex"), OptionIndex);
	Result->SetNumberField(TEXT("testIndex"), Options[OptionIndex]->Tests.Num() - 1);
	Result->SetStringField(TEXT("testClass"), Class->GetName());
	if (PropErrors.Num() > 0) Result->SetArrayField(TEXT("propertyErrors"), PropErrors);
	return MCPResult(Result);
}

// gameplay(set_eqs_property): dotted-path property write on a generator
// (testIndex omitted) or a test. FAIDataProvider values live one level down,
// e.g. "SearchRadius.DefaultValue" or "FloatValueMin.DefaultValue".
TSharedPtr<FJsonValue> FGameplayHandlers::SetEQSProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Error;
	UEnvQuery* Query = LoadEnvQuery(Params, Path, Error);
	if (!Query) return MCPError(Error);

	FString PropertyName;
	if (auto Err = RequireString(Params, TEXT("propertyName"), PropertyName)) return Err;
	TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
	if (!Value.IsValid()) return MCPError(TEXT("Missing 'value'"));

	const int32 OptionIndex = OptionalInt(Params, TEXT("optionIndex"), 0);
	TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
	if (!Options.IsValidIndex(OptionIndex))
	{
		return MCPError(FString::Printf(TEXT("optionIndex %d out of range (options=%d)"), OptionIndex, Options.Num()));
	}

	UObject* Target = nullptr;
	const int32 TestIndex = OptionalInt(Params, TEXT("testIndex"), -1);
	if (TestIndex < 0)
	{
		Target = Options[OptionIndex]->Generator;
		if (!Target) return MCPError(TEXT("Option has no generator"));
	}
	else
	{
		if (!Options[OptionIndex]->Tests.IsValidIndex(TestIndex))
		{
			return MCPError(FString::Printf(TEXT("testIndex %d out of range (tests=%d)"), TestIndex, Options[OptionIndex]->Tests.Num()));
		}
		Target = Options[OptionIndex]->Tests[TestIndex];
	}

	if (!MCPJsonProperty::SetDottedPropertyFromJson(Target, PropertyName, Value, Error))
	{
		return MCPError(FString::Printf(TEXT("Set '%s' failed: %s"), *PropertyName, *Error));
	}

	FinalizeEQSEdit(Query);

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetStringField(TEXT("assetPath"), Path);
	Result->SetStringField(TEXT("target"), Target->GetClass()->GetName());
	Result->SetStringField(TEXT("propertyName"), PropertyName);
	return MCPResult(Result);
}

// gameplay(remove_eqs_node): remove a test (optionIndex + testIndex) or a
// whole option with its generator and tests (testIndex omitted).
TSharedPtr<FJsonValue> FGameplayHandlers::RemoveEQSNode(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Error;
	UEnvQuery* Query = LoadEnvQuery(Params, Path, Error);
	if (!Query) return MCPError(Error);

	const int32 OptionIndex = OptionalInt(Params, TEXT("optionIndex"), 0);
	TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
	if (!Options.IsValidIndex(OptionIndex))
	{
		return MCPError(FString::Printf(TEXT("optionIndex %d out of range (options=%d)"), OptionIndex, Options.Num()));
	}

	const int32 TestIndex = OptionalInt(Params, TEXT("testIndex"), -1);
	FString Removed;
	if (TestIndex < 0)
	{
		Removed = Options[OptionIndex]->Generator ? Options[OptionIndex]->Generator->GetClass()->GetName() : TEXT("(option)");
		Options.RemoveAt(OptionIndex);
	}
	else
	{
		if (!Options[OptionIndex]->Tests.IsValidIndex(TestIndex))
		{
			return MCPError(FString::Printf(TEXT("testIndex %d out of range (tests=%d)"), TestIndex, Options[OptionIndex]->Tests.Num()));
		}
		Removed = Options[OptionIndex]->Tests[TestIndex]->GetClass()->GetName();
		Options[OptionIndex]->Tests.RemoveAt(TestIndex);
	}

	FinalizeEQSEdit(Query);

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("assetPath"), Path);
	Result->SetStringField(TEXT("removed"), Removed);
	return MCPResult(Result);
}

// gameplay(read_eqs_query): options -> generator + tests with key settings.
TSharedPtr<FJsonValue> FGameplayHandlers::ReadEQSQuery(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Error;
	UEnvQuery* Query = LoadEnvQuery(Params, Path, Error);
	if (!Query) return MCPError(Error);

	TArray<TSharedPtr<FJsonValue>> OptionsArr;
	for (const UEnvQueryOption* Option : Query->GetOptions())
	{
		if (!Option) continue;
		TSharedPtr<FJsonObject> OptObj = MakeShared<FJsonObject>();
		if (Option->Generator)
		{
			OptObj->SetStringField(TEXT("generatorClass"), Option->Generator->GetClass()->GetName());
		}
		TArray<TSharedPtr<FJsonValue>> TestsArr;
		for (const UEnvQueryTest* Test : Option->Tests)
		{
			if (!Test) continue;
			TSharedPtr<FJsonObject> TestObj = MakeShared<FJsonObject>();
			TestObj->SetStringField(TEXT("testClass"), Test->GetClass()->GetName());
			TestObj->SetStringField(TEXT("purpose"),
				Test->TestPurpose == EEnvTestPurpose::Filter ? TEXT("Filter") :
				Test->TestPurpose == EEnvTestPurpose::Score ? TEXT("Score") : TEXT("FilterAndScore"));
			TestsArr.Add(MakeShared<FJsonValueObject>(TestObj));
		}
		OptObj->SetArrayField(TEXT("tests"), TestsArr);
		OptionsArr.Add(MakeShared<FJsonValueObject>(OptObj));
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("assetPath"), Path);
	Result->SetArrayField(TEXT("options"), OptionsArr);
	return MCPResult(Result);
}

// gameplay(list_eqs_classes): concrete generator/test/context classes.
TSharedPtr<FJsonValue> FGameplayHandlers::ListEQSClasses(const TSharedPtr<FJsonObject>& Params)
{
	const FString Kind = OptionalString(Params, TEXT("kind"), TEXT(""));
	auto Collect = [](UClass* Base)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(Base) && !It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
			{
				Arr.Add(MakeShared<FJsonValueString>(It->GetName()));
			}
		}
		return Arr;
	};

	auto Result = MCPSuccess();
	if (Kind.IsEmpty() || Kind == TEXT("generator"))
	{
		Result->SetArrayField(TEXT("generators"), Collect(UEnvQueryGenerator::StaticClass()));
	}
	if (Kind.IsEmpty() || Kind == TEXT("test"))
	{
		Result->SetArrayField(TEXT("tests"), Collect(UEnvQueryTest::StaticClass()));
	}
	if (Kind.IsEmpty() || Kind == TEXT("context"))
	{
		Result->SetArrayField(TEXT("contexts"), Collect(UEnvQueryContext::StaticClass()));
	}
	return MCPResult(Result);
}
