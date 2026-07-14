#include "LiveKitBlueprintLibrary.h"
#include "LiveKitSubsystem.h"
#include "LiveKitTypes.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "LiveKitBridgeModule.h"
#include "LiveKitPlatformBridgeFactory.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/UnrealType.h"

#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLiveKitWindowsDependencyDiscoveryTest,
    "LiveKitBridge.Windows.DependencyDiscovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLiveKitWindowsDependencyDiscoveryTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && WITH_LIVEKIT_WINDOWS
#if !IS_MONOLITHIC
    const FString ModuleFilename =
        FModuleManager::Get().GetModuleFilename(FName(TEXT("LiveKitBridge")));
    TestFalse(TEXT("LiveKitBridge module filename is available"), ModuleFilename.IsEmpty());
    const FString ModuleDirectory = FPaths::GetPath(ModuleFilename);
#else
    const FString ModuleDirectory = FPlatformProcess::BaseDir();
#endif
    TestTrue(
        TEXT("livekit_ffi.dll is staged beside the editor module"),
        IFileManager::Get().FileExists(
            *FPaths::Combine(ModuleDirectory, TEXT("livekit_ffi.dll"))));
    TestTrue(
        TEXT("livekit.dll is staged beside the editor module"),
        IFileManager::Get().FileExists(
            *FPaths::Combine(ModuleDirectory, TEXT("livekit.dll"))));
    TestTrue(
        TEXT("LiveKitUnrealWindowsAdapter.dll is staged beside the editor module"),
        IFileManager::Get().FileExists(
            *FPaths::Combine(
                ModuleDirectory,
                TEXT("LiveKitUnrealWindowsAdapter.dll"))));
    TestTrue(
        TEXT("Verified LiveKit C++ SDK initialized"),
        IsLiveKitWindowsSdkInitialized());
    IModuleInterface* LiveKitModule =
        FModuleManager::Get().GetModule(FName(TEXT("LiveKitBridge")));
    TestNotNull(TEXT("LiveKitBridge module interface is available"), LiveKitModule);
    if (LiveKitModule != nullptr)
    {
        TestFalse(
            TEXT("The initialized Win64 backend rejects dynamic module reload"),
            LiveKitModule->SupportsDynamicReloading());
    }
#else
    TestTrue(TEXT("No verified Win64 SDK is expected for this target"), true);
#endif
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLiveKitWindowsIdleLifecycleTest,
    "LiveKitBridge.Windows.IdleLifecycle",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLiveKitWindowsIdleLifecycleTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS
    std::atomic_bool bSawDisconnecting{false};
    std::atomic_bool bSawDisconnected{false};
    TSharedPtr<ILiveKitPlatformBridge> Bridge = CreateLiveKitPlatformBridge(
        [&bSawDisconnecting, &bSawDisconnected](ELiveKitConnectionState State)
        {
            if (State == ELiveKitConnectionState::Disconnecting)
            {
                bSawDisconnecting.store(true, std::memory_order_release);
            }
            else if (State == ELiveKitConnectionState::Disconnected)
            {
                bSawDisconnected.store(true, std::memory_order_release);
            }
        },
        [](const FLiveKitError&) {},
        [](const FLiveKitParticipantInfo&) {},
        [](const FLiveKitParticipantInfo&) {},
        [](const FLiveKitParticipantInfo&, bool) {},
        [](const FLiveKitDataMessage&) {},
        [](const FString&, bool, const FLiveKitError&) {},
        [](const FLiveKitByteStream&) {},
        [](const FString&, bool, const FLiveKitError&) {},
        [](const FLiveKitRpcInvocation&) {},
        [](const FString&, bool, const FString&, const FLiveKitError&) {},
        [](const FString&, bool, const FLiveKitError&) {});

    TestTrue(TEXT("Windows bridge factory returns a bridge"), Bridge.IsValid());
    if (!Bridge)
    {
        return false;
    }

    TestEqual(TEXT("Windows backend is selected"), Bridge->GetBackendName(), FString(TEXT("Windows")));
    TestEqual(
        TEXT("Bridge and module agree on SDK availability"),
        Bridge->IsSdkAvailable(),
        IsLiveKitWindowsSdkInitialized());
    TestFalse(
        TEXT("Empty byte-stream topics are rejected"),
        Bridge->RegisterByteStreamHandler(FString()));
    TestFalse(
        TEXT("Empty RPC methods are rejected"),
        Bridge->RegisterRpcMethod(FString()));

    if (Bridge->IsSdkAvailable())
    {
        TestTrue(
            TEXT("An idle byte-stream registration is retained for the next room"),
            Bridge->RegisterByteStreamHandler(TEXT("tests.lifecycle.stream")));
        TestFalse(
            TEXT("Duplicate idle byte-stream registrations are rejected"),
            Bridge->RegisterByteStreamHandler(TEXT("tests.lifecycle.stream")));
        Bridge->UnregisterByteStreamHandler(TEXT("tests.lifecycle.stream"));

        TestTrue(
            TEXT("An idle RPC registration is retained for the next room"),
            Bridge->RegisterRpcMethod(TEXT("tests.lifecycle.rpc")));
        TestFalse(
            TEXT("Duplicate idle RPC registrations are rejected"),
            Bridge->RegisterRpcMethod(TEXT("tests.lifecycle.rpc")));
        Bridge->UnregisterRpcMethod(TEXT("tests.lifecycle.rpc"));
    }

    // Unregistration and disconnect enqueue serialized control work even when
    // no room exists. Shutdown must cancel or join that work before returning,
    // and repeated shutdown/destruction must remain harmless.
    Bridge->Disconnect();
    if (Bridge->IsSdkAvailable())
    {
        TestTrue(
            TEXT("Idle disconnect enters the disconnecting state"),
            bSawDisconnecting.load(std::memory_order_acquire));
    }
    else
    {
        TestTrue(
            TEXT("The SDK-unavailable fallback completes idle disconnect synchronously"),
            bSawDisconnected.load(std::memory_order_acquire));
    }
    Bridge->Shutdown();
    Bridge->Shutdown();
    Bridge.Reset();
#else
    TestTrue(TEXT("Windows lifecycle test is not applicable to this target"), true);
#endif
    return true;
}

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
    TestTrue(TEXT("Microphone defaults enabled"), Options.bEnableMicrophone);
    TestTrue(TEXT("Apple voice processing defaults enabled"), Options.bEnableVoiceProcessing);
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
