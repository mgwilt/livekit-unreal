#include "LiveKitBlueprintLibrary.h"
#include "LiveKitSubsystem.h"
#include "LiveKitTypes.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLiveKitConnectionValidationTest,
    "LiveKitBridge.Connection.Validation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLiveKitConnectionValidationTest::RunTest(const FString& Parameters)
{
    UGameInstance* GameInstance = NewObject<UGameInstance>();
    GameInstance->AddToRoot();
    ULiveKitSubsystem* Subsystem = NewObject<ULiveKitSubsystem>(GameInstance);

    FLiveKitConnectOptions Options;
    AddExpectedError(
        TEXT("invalid_connection_details"),
        EAutomationExpectedErrorFlags::Contains,
        1);
    Subsystem->Connect(FString(), FString(), Options);
    TestEqual(
        TEXT("Missing URL and token fail before touching the SDK"),
        Subsystem->GetConnectionState(),
        ELiveKitConnectionState::Failed);

    GameInstance->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLiveKitStateTransitionTest,
    "LiveKitBridge.Connection.StateTransitions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLiveKitStateTransitionTest::RunTest(const FString& Parameters)
{
    UGameInstance* GameInstance = NewObject<UGameInstance>();
    GameInstance->AddToRoot();
    ULiveKitSubsystem* Subsystem = NewObject<ULiveKitSubsystem>(GameInstance);

    Subsystem->SetConnectionStateForTesting(ELiveKitConnectionState::Connecting);
    TestEqual(TEXT("Connecting"), Subsystem->GetConnectionState(), ELiveKitConnectionState::Connecting);
    Subsystem->SetConnectionStateForTesting(ELiveKitConnectionState::Reconnecting);
    TestEqual(TEXT("Reconnecting"), Subsystem->GetConnectionState(), ELiveKitConnectionState::Reconnecting);
    Subsystem->SetConnectionStateForTesting(ELiveKitConnectionState::Disconnecting);
    TestEqual(TEXT("Disconnecting"), Subsystem->GetConnectionState(), ELiveKitConnectionState::Disconnecting);
    Subsystem->SetConnectionStateForTesting(ELiveKitConnectionState::Disconnected);
    TestEqual(TEXT("Disconnected"), Subsystem->GetConnectionState(), ELiveKitConnectionState::Disconnected);

    GameInstance->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLiveKitUtf8DataTest,
    "LiveKitBridge.Data.Utf8RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLiveKitUtf8DataTest::RunTest(const FString& Parameters)
{
    const FString Expected = TEXT("Hello, LiveKit — 你好");
    FTCHARToUTF8 Converted(*Expected);
    FLiveKitDataMessage Message;
    Message.Data.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
    TestEqual(TEXT("UTF-8 payload round-trips"), Message.AsString(), Expected);
    TestEqual(
        TEXT("Blueprint text helper uses the same UTF-8 conversion"),
        ULiveKitBlueprintLibrary::DataMessageAsText(Message),
        Expected);
    return true;
}
#endif
