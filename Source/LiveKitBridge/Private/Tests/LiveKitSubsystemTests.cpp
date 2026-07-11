#include "LiveKitBlueprintLibrary.h"
#include "LiveKitSubsystem.h"
#include "LiveKitTypes.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLiveKitByteStreamBlueprintSurfaceTest,
    "LiveKitBridge.ByteStream.BlueprintSurface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLiveKitByteStreamBlueprintSurfaceTest::RunTest(const FString& Parameters)
{
    UScriptStruct* StreamStruct = FLiveKitByteStream::StaticStruct();
    TestNotNull(TEXT("Byte stream is a reflected struct"), StreamStruct);

    const FName FieldNames[] = {
        GET_MEMBER_NAME_CHECKED(FLiveKitByteStream, SenderIdentity),
        GET_MEMBER_NAME_CHECKED(FLiveKitByteStream, StreamId),
        GET_MEMBER_NAME_CHECKED(FLiveKitByteStream, Topic),
        GET_MEMBER_NAME_CHECKED(FLiveKitByteStream, Name),
        GET_MEMBER_NAME_CHECKED(FLiveKitByteStream, MimeType),
        GET_MEMBER_NAME_CHECKED(FLiveKitByteStream, Attributes),
        GET_MEMBER_NAME_CHECKED(FLiveKitByteStream, Data),
    };
    for (const FName FieldName : FieldNames)
    {
        FProperty* Property = StreamStruct->FindPropertyByName(FieldName);
        TestNotNull(*FString::Printf(TEXT("%s is reflected"), *FieldName.ToString()), Property);
        if (Property)
        {
            TestTrue(
                *FString::Printf(TEXT("%s is Blueprint visible"), *FieldName.ToString()),
                Property->HasAnyPropertyFlags(CPF_BlueprintVisible));
        }
    }

    UClass* SubsystemClass = ULiveKitSubsystem::StaticClass();
    TestNotNull(
        TEXT("RegisterByteStreamHandler is Blueprint callable"),
        SubsystemClass->FindFunctionByName(
            GET_FUNCTION_NAME_CHECKED(ULiveKitSubsystem, RegisterByteStreamHandler)));
    TestNotNull(
        TEXT("UnregisterByteStreamHandler is Blueprint callable"),
        SubsystemClass->FindFunctionByName(
            GET_FUNCTION_NAME_CHECKED(ULiveKitSubsystem, UnregisterByteStreamHandler)));
    TestNotNull(
        TEXT("OnByteStreamReceived is reflected"),
        SubsystemClass->FindPropertyByName(
            GET_MEMBER_NAME_CHECKED(ULiveKitSubsystem, OnByteStreamReceived)));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLiveKitByteStreamValueTest,
    "LiveKitBridge.ByteStream.ValueContainer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLiveKitByteStreamValueTest::RunTest(const FString& Parameters)
{
    TestEqual(
        TEXT("Incoming stream hard cap"),
        LiveKitLimits::MaxIncomingByteStreamBytes,
        8 * 1024 * 1024);

    FLiveKitByteStream Stream;
    Stream.SenderIdentity = TEXT("sender-1");
    Stream.StreamId = TEXT("stream-42");
    Stream.Topic = TEXT("models.preview");
    Stream.Name = TEXT("shape.ply");
    Stream.MimeType = TEXT("application/octet-stream");
    Stream.Attributes.Add(TEXT("lod"), TEXT("preview"));
    Stream.Data = {0x00, 0x7f, 0xff};

    TestEqual(TEXT("Sender identity"), Stream.SenderIdentity, FString(TEXT("sender-1")));
    TestEqual(TEXT("Stream ID"), Stream.StreamId, FString(TEXT("stream-42")));
    TestEqual(TEXT("Topic"), Stream.Topic, FString(TEXT("models.preview")));
    TestEqual(TEXT("Name"), Stream.Name, FString(TEXT("shape.ply")));
    TestEqual(
        TEXT("MIME type"),
        Stream.MimeType,
        FString(TEXT("application/octet-stream")));
    TestEqual(TEXT("Attribute"), Stream.Attributes.FindRef(TEXT("lod")), FString(TEXT("preview")));
    TestEqual(TEXT("Byte count"), Stream.Data.Num(), 3);
    TestEqual(TEXT("Final byte"), Stream.Data[2], static_cast<uint8>(0xff));
    return true;
}
#endif
