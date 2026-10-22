#include "LiveKitAndroidBridge.h"
#if PLATFORM_ANDROID
#include "Android/AndroidApplication.h"
#include "Android/AndroidJNI.h"
#include "Async/Async.h"
#include "Misc/ScopeLock.h"
#include "Dom/JsonObject.h"
#include "Misc/Base64.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
TMap<int64, TWeakPtr<FLiveKitAndroidBridge>> Bridges;
int64 NextHandle = 0;
struct FPendingVideo { int64 Epoch; FLiveKitVideoFrame Frame; };
FCriticalSection VideoLock;
TMap<int64, FPendingVideo> PendingVideos;
FLiveKitError Failure(const FString& Code)
{
    FLiveKitError Result;
    Result.Code = Code;
    Result.Message = Code;
    return Result;
}
FString StringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
{
    FString Value;
    Object->TryGetStringField(Name, Value);
    return Value;
}
}

extern "C" JNIEXPORT void JNICALL
Java_com_mgwilt_livekitunreal_LiveKitUnrealCallbacks_onEvent(JNIEnv* Env, jclass, jlong Handle,
    jlong Generation, jstring Event)
{
    const FString Payload = FJavaHelper::FStringFromParam(Env, Event);
    if (Payload.Len() > 128 * 1024) return;
    AsyncTask(ENamedThreads::GameThread, [Handle, Generation, Payload]()
    {
        if (const auto* Weak = Bridges.Find(Handle))
            if (auto Bridge = Weak->Pin()) Bridge->HandleEvent(Generation, Payload);
    });
}
extern "C" JNIEXPORT void JNICALL
Java_com_mgwilt_livekitunreal_LiveKitUnrealCallbacks_onVideo(JNIEnv* Env, jclass, jlong Handle,
    jlong Epoch, jobject Buffer, jint Width, jint Height, jint Rotation, jlong Timestamp)
{
    if (Width <= 0 || Height <= 0 || Width > 3840 || Height > 3840 || Rotation % 90 != 0) return;
    const int32 ChromaWidth = (Width + 1) / 2, ChromaHeight = (Height + 1) / 2;
    const int64 Size = int64(Width) * Height + 2ll * ChromaWidth * ChromaHeight;
    auto* Bytes = static_cast<uint8*>(Env->GetDirectBufferAddress(Buffer));
    if (!Bytes || Env->GetDirectBufferCapacity(Buffer) < Size) return;
    FLiveKitVideoFrame Frame;
    Frame.Width = Rotation % 180 ? Height : Width;
    Frame.Height = Rotation % 180 ? Width : Height;
    Frame.TimestampMicroseconds = Timestamp;
    Frame.Pixels.SetNumUninitialized(Width * Height * 4);
    const auto* U = Bytes + Width * Height;
    const auto* V = U + ChromaWidth * ChromaHeight;
    for (int32 Y = 0; Y < Height; ++Y) for (int32 X = 0; X < Width; ++X)
    {
        const int32 L = FMath::Max(0, int32(Bytes[Y * Width + X]) - 16);
        const int32 Cb = int32(U[(Y / 2) * ChromaWidth + X / 2]) - 128;
        const int32 Cr = int32(V[(Y / 2) * ChromaWidth + X / 2]) - 128;
        int32 Dx = X, Dy = Y;
        if (Rotation == 90) { Dx = Height - 1 - Y; Dy = X; }
        else if (Rotation == 180) { Dx = Width - 1 - X; Dy = Height - 1 - Y; }
        else if (Rotation == 270) { Dx = Y; Dy = Width - 1 - X; }
        uint8* Out = Frame.Pixels.GetData() + (Dy * Frame.Width + Dx) * 4;
        Out[0] = FMath::Clamp((298 * L + 516 * Cb + 128) >> 8, 0, 255);
        Out[1] = FMath::Clamp((298 * L - 100 * Cb - 208 * Cr + 128) >> 8, 0, 255);
        Out[2] = FMath::Clamp((298 * L + 409 * Cr + 128) >> 8, 0, 255); Out[3] = 255;
    }
    bool Schedule;
    {
        FScopeLock Lock(&VideoLock);
        Schedule = !PendingVideos.Contains(Handle);
        PendingVideos.Add(Handle, {Epoch, MoveTemp(Frame)});
    }
    if (Schedule) AsyncTask(ENamedThreads::GameThread, [Handle]() {
        FPendingVideo Latest;
        { FScopeLock Lock(&VideoLock); if (!PendingVideos.RemoveAndCopyValue(Handle, Latest)) return; }
        if (const auto* Weak = Bridges.Find(Handle))
            if (auto Bridge = Weak->Pin()) Bridge->HandleVideo(Latest.Epoch, MoveTemp(Latest.Frame));
    });
}
extern "C" JNIEXPORT void JNICALL
Java_com_mgwilt_livekitunreal_LiveKitUnrealCallbacks_onAudio(JNIEnv* Env, jclass, jlong Handle,
    jlong Generation, jshortArray Samples, jint Rate, jint Channels)
{
    const jsize Count = Env->GetArrayLength(Samples);
    if (Rate < 8000 || Rate > 192000 || Channels < 1 || Channels > 2 || Count <= 0 ||
        Count > 38400 || Count % Channels != 0) return;
    FLiveKitAudioFrame Frame;
    Frame.SampleRate = Rate;
    Frame.Channels = Channels;
    Frame.Samples.SetNumUninitialized(Count);
    Env->GetShortArrayRegion(Samples, 0, Count, Frame.Samples.GetData());
    AsyncTask(ENamedThreads::GameThread, [Handle, Generation, Frame = MoveTemp(Frame)]() mutable
    {
        if (const auto* Weak = Bridges.Find(Handle))
            if (auto Bridge = Weak->Pin()) Bridge->HandleAudio(Generation, MoveTemp(Frame));
    });
}

FLiveKitAndroidBridge::FLiveKitAndroidBridge(FStateHandler S, FErrorHandler E, FParticipantHandler J,
    FParticipantHandler L, FDataHandler D, FPublishResultHandler P, FRpcResultHandler R)
    : State(MoveTemp(S)), Error(MoveTemp(E)), Joined(MoveTemp(J)), Left(MoveTemp(L)),
      Data(MoveTemp(D)), Published(MoveTemp(P)), RpcResult(MoveTemp(R)) {}
FLiveKitAndroidBridge::~FLiveKitAndroidBridge() { Shutdown(); }
void FLiveKitAndroidBridge::ActivateLifetimeGate()
{
    Handle = ++NextHandle;
    Bridges.Add(Handle, AsShared());
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    Class = FAndroidApplication::FindJavaClassGlobalRef("com/mgwilt/livekitunreal/LiveKitUnrealBridge");
    if (CheckException(Env) || !Class) return;
    const jmethodID Constructor = Method(Env, "<init>", "(Landroid/app/Activity;J)V");
    if (!Constructor) return;
    jobject Local = Env->NewObject(Class, Constructor, FJavaWrapper::GameActivityThis, static_cast<jlong>(Handle));
    if (!CheckException(Env) && Local) { Object = Env->NewGlobalRef(Local); Env->DeleteLocalRef(Local); }
}
bool FLiveKitAndroidBridge::CheckException(JNIEnv* Env)
{
    if (!Env->ExceptionCheck()) return false;
    Env->ExceptionClear();
    Error(Failure(TEXT("android_sdk_error")));
    return true;
}
jmethodID FLiveKitAndroidBridge::Method(JNIEnv* Env, const char* Name, const char* Signature)
{
    if (!Class) return nullptr;
    jmethodID Id = Env->GetMethodID(Class, Name, Signature);
    return CheckException(Env) ? nullptr : Id;
}
void FLiveKitAndroidBridge::Shutdown()
{
    Bridges.Remove(Handle);
    bMicrophoneCapturing = false;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (Object)
    {
        if (auto Id = Method(Env, "shutdown", "()V")) Env->CallVoidMethod(Object, Id);
        CheckException(Env);
        Env->DeleteGlobalRef(Object);
        Object = nullptr;
    }
    if (Class) { Env->DeleteGlobalRef(Class); Class = nullptr; }
}
bool FLiveKitAndroidBridge::ReadConnectionConfiguration(FString& Url, FString& Token, FString& Identity)
{
    if (!Object) return false;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    auto Id = Method(Env, "configuration", "()Ljava/lang/String;");
    if (!Id) return false;
    jstring Value = static_cast<jstring>(Env->CallObjectMethod(Object, Id));
    if (CheckException(Env) || !Value) return false;
    const FString Json = FJavaHelper::FStringFromParam(Env, Value);
    Env->DeleteLocalRef(Value);
    TSharedPtr<FJsonObject> Config;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Config) || !Config) return false;
    Url = StringField(Config, TEXT("url")); Token = StringField(Config, TEXT("token"));
    Identity = StringField(Config, TEXT("participant"));
    return !Url.IsEmpty() && !Token.IsEmpty() && !Identity.IsEmpty();
}
bool FLiveKitAndroidBridge::ConfigureVideoReceiver(const FString& Identity, const FString& Name, FVideoHandler Handler)
{
    Video = MoveTemp(Handler);
    if (!Object) return false;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    const auto Id = Method(Env, "configureVideo", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (!Id) return false;
    auto P = FJavaHelper::ToJavaString(Env, Identity); auto N = FJavaHelper::ToJavaString(Env, Name);
    Env->CallVoidMethod(Object, Id, *P, *N);
    return !CheckException(Env);
}
void FLiveKitAndroidBridge::HandleVideo(int64 Epoch, FLiveKitVideoFrame Frame)
{
    if (Epoch == Generation && Video && Frame.IsValid()) Video(Frame);
}
bool FLiveKitAndroidBridge::ConfigureAudioReceiver(const FString& Identity, const FString& Name, FAudioHandler Handler)
{
    Participant = Identity; Track = Name; Audio = MoveTemp(Handler);
    return Object && !Participant.IsEmpty();
}
void FLiveKitAndroidBridge::Connect(const FString& Url, const FString& Token, bool Mic, bool)
{
    if (!Object) { Error(Failure(TEXT("android_sdk_unavailable"))); return; }
    ++Generation; SampleIndex = 0;
    State(ELiveKitConnectionState::Connecting);
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    auto Id = Method(Env, "connect", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JZ)V");
    if (!Id) return;
    auto U = FJavaHelper::ToJavaString(Env, Url); auto T = FJavaHelper::ToJavaString(Env, Token);
    auto P = FJavaHelper::ToJavaString(Env, Participant); auto N = FJavaHelper::ToJavaString(Env, Track);
    Env->CallVoidMethod(Object, Id, *U, *T, *P, *N, static_cast<jlong>(Generation), static_cast<jboolean>(Mic));
    CheckException(Env);
}
void FLiveKitAndroidBridge::Disconnect()
{
    ++Generation; bMicrophoneCapturing = false;
    State(ELiveKitConnectionState::Disconnecting);
    if (!Object) { State(ELiveKitConnectionState::Disconnected); return; }
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (auto Id = Method(Env, "disconnect", "(J)V")) Env->CallVoidMethod(Object, Id, static_cast<jlong>(Generation));
    CheckException(Env);
}
void FLiveKitAndroidBridge::SetMicrophoneEnabled(bool Enabled)
{
    if (!Enabled) bMicrophoneCapturing = false;
    if (!Object) return;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (auto Id = Method(Env, "microphone", "(Z)V")) Env->CallVoidMethod(Object, Id, static_cast<jboolean>(Enabled));
    CheckException(Env);
}
void FLiveKitAndroidBridge::PublishData(const FString& Operation, const TArray<uint8>& Bytes,
    const FString& Topic, ELiveKitDataReliability Reliability, const TArray<FString>& Destinations)
{
    if (!Object) { Published(Operation, false, Failure(TEXT("android_sdk_unavailable"))); return; }
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const auto& Destination : Destinations) Values.Add(MakeShared<FJsonValueString>(Destination));
    FString DestinationsJson;
    FJsonSerializer::Serialize(Values, TJsonWriterFactory<>::Create(&DestinationsJson));
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    auto Id = Method(Env, "publish", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ZLjava/lang/String;)V");
    if (!Id) return;
    auto O = FJavaHelper::ToJavaString(Env, Operation); auto B = FJavaHelper::ToJavaString(Env, FBase64::Encode(Bytes));
    auto T = FJavaHelper::ToJavaString(Env, Topic); auto D = FJavaHelper::ToJavaString(Env, DestinationsJson);
    Env->CallVoidMethod(Object, Id, *O, *B, *T, static_cast<jboolean>(Reliability == ELiveKitDataReliability::Reliable), *D);
    CheckException(Env);
}
void FLiveKitAndroidBridge::PerformRpc(const FString& Request, const FString&, const FString&,
    const FString&, float, float)
{
    RpcResult(Request, false, FString(), Failure(TEXT("android_rpc_unsupported")));
}
void FLiveKitAndroidBridge::HandleEvent(int64 Epoch, const FString& Json)
{
    if (Epoch != Generation) return;
    TSharedPtr<FJsonObject> Event;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Event) || !Event) return;
    const FString Type = StringField(Event, TEXT("type"));
    if (Type == TEXT("state"))
    {
        const FString Value = StringField(Event, TEXT("state"));
        ELiveKitConnectionState Next;
        if (Value == TEXT("connected")) Next = ELiveKitConnectionState::Connected;
        else if (Value == TEXT("reconnecting")) Next = ELiveKitConnectionState::Reconnecting;
        else if (Value == TEXT("disconnected")) Next = ELiveKitConnectionState::Disconnected;
        else if (Value == TEXT("failed")) Next = ELiveKitConnectionState::Failed;
        else return;
        if (Next != ELiveKitConnectionState::Connected) bMicrophoneCapturing = false;
        UE_LOG(LogTemp, Log, TEXT("LiveKit Android state: %s"), *Value);
        State(Next);
    }
    else if (Type == TEXT("error")) Error(Failure(StringField(Event, TEXT("code"))));
    else if (Type == TEXT("microphone"))
    {
        Event->TryGetBoolField(TEXT("enabled"), bMicrophoneCapturing);
        UE_LOG(LogTemp, Log, TEXT("LiveKit Android microphone: %s"), bMicrophoneCapturing ? TEXT("on") : TEXT("off"));
    }
    else if (Type == TEXT("participant_connected") || Type == TEXT("participant_disconnected"))
    {
        FLiveKitParticipantInfo Person;
        Person.Identity = StringField(Event, TEXT("identity")); Person.Sid = StringField(Event, TEXT("sid"));
        Person.Name = StringField(Event, TEXT("name"));
        if (Type == TEXT("participant_connected")) Joined(Person); else Left(Person);
    }
    else if (Type == TEXT("data"))
    {
        FLiveKitDataMessage Message;
        Message.SenderIdentity = StringField(Event, TEXT("sender")); Message.Topic = StringField(Event, TEXT("topic"));
        if (FBase64::Decode(StringField(Event, TEXT("data")), Message.Data)) Data(Message);
    }
    else if (Type == TEXT("publish"))
    {
        bool Success = false; Event->TryGetBoolField(TEXT("success"), Success);
        Published(StringField(Event, TEXT("id")), Success, Success ? FLiveKitError() : Failure(TEXT("publish_failed")));
    }
    else if (Type == TEXT("audio_subscribed"))
        UE_LOG(LogTemp, Log, TEXT("LiveKit Android selected audio subscribed"));
}
void FLiveKitAndroidBridge::HandleAudio(int64 Epoch, FLiveKitAudioFrame Frame)
{
    if (Epoch != Generation || !Audio) return;
    Frame.SampleIndex = SampleIndex; Frame.bDiscontinuity = SampleIndex == 0;
    SampleIndex += Frame.Samples.Num() / Frame.Channels;
    Audio(Frame);
}
#endif
