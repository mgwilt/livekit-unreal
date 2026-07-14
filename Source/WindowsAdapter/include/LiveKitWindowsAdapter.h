#pragma once

/*
 * Allocator-safe Win64 facade for the LiveKit C++ SDK.
 *
 * This header is intentionally valid C. No C++ standard-library type,
 * exception, or object ownership may cross this ABI. String and byte views
 * passed into the adapter are borrowed for the duration of the call. Views
 * supplied to callbacks are borrowed for the duration of the callback.
 */

#include <stdint.h>

#if defined(_WIN32)
#  define LKUB_CALL __cdecl
#  if defined(LIVEKIT_UNREAL_WINDOWS_ADAPTER_EXPORTS)
#    define LKUB_API __declspec(dllexport)
#  else
#    define LKUB_API __declspec(dllimport)
#  endif
#else
#  define LKUB_CALL
#  define LKUB_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LKUB_ABI_VERSION 1u
#define LKUB_RESULT_MESSAGE_CAPACITY 1024u
#define LKUB_RESULT_DATA_CAPACITY 4096u

typedef struct LKUB_Room LKUB_Room;
typedef struct LKUB_ByteStreamReader LKUB_ByteStreamReader;

typedef struct LKUB_StringView
{
    const char* data;
    uint64_t size;
} LKUB_StringView;

typedef struct LKUB_BytesView
{
    const uint8_t* data;
    uint64_t size;
} LKUB_BytesView;

/* Buffers returned by the adapter must be released with lkub_buffer_release. */
typedef struct LKUB_OwnedBuffer
{
    uint8_t* data;
    uint64_t size;
} LKUB_OwnedBuffer;

typedef enum LKUB_Status
{
    LKUB_STATUS_OK = 0,
    LKUB_STATUS_INVALID_ARGUMENT = 1,
    LKUB_STATUS_INVALID_STATE = 2,
    LKUB_STATUS_SDK_ERROR = 3,
    LKUB_STATUS_RPC_ERROR = 4,
    LKUB_STATUS_OUT_OF_MEMORY = 5,
    LKUB_STATUS_INTERNAL_ERROR = 6
} LKUB_Status;

/* Caller-owned, fixed-size error storage. Text may be truncated to capacity. */
typedef struct LKUB_Result
{
    int32_t status;
    uint32_t rpc_error_code;
    uint32_t message_size;
    char message[LKUB_RESULT_MESSAGE_CAPACITY];
    uint32_t data_size;
    char data[LKUB_RESULT_DATA_CAPACITY];
} LKUB_Result;

typedef enum LKUB_LogLevel
{
    LKUB_LOG_TRACE = 0,
    LKUB_LOG_DEBUG = 1,
    LKUB_LOG_INFO = 2,
    LKUB_LOG_WARN = 3,
    LKUB_LOG_ERROR = 4,
    LKUB_LOG_CRITICAL = 5,
    LKUB_LOG_OFF = 6
} LKUB_LogLevel;

typedef enum LKUB_ConnectionState
{
    LKUB_CONNECTION_DISCONNECTED = 0,
    LKUB_CONNECTION_CONNECTED = 1,
    LKUB_CONNECTION_RECONNECTING = 2
} LKUB_ConnectionState;

typedef enum LKUB_DataReliability
{
    LKUB_DATA_LOSSY = 0,
    LKUB_DATA_RELIABLE = 1
} LKUB_DataReliability;

typedef struct LKUB_ParticipantInfo
{
    LKUB_StringView identity;
    LKUB_StringView sid;
    LKUB_StringView name;
    LKUB_StringView metadata;
    uint8_t is_agent;
    uint8_t is_speaking;
    uint8_t reserved[6];
} LKUB_ParticipantInfo;

typedef struct LKUB_UserData
{
    LKUB_StringView sender_identity;
    LKUB_StringView topic;
    LKUB_BytesView payload;
    int32_t reliability;
} LKUB_UserData;

typedef void (LKUB_CALL* LKUB_ParticipantCallback)(
    void* context,
    LKUB_Room* room,
    const LKUB_ParticipantInfo* participant);

typedef void (LKUB_CALL* LKUB_SpeakingCallback)(
    void* context,
    LKUB_Room* room,
    const LKUB_ParticipantInfo* participant,
    uint8_t is_speaking);

typedef void (LKUB_CALL* LKUB_ConnectionStateCallback)(
    void* context,
    LKUB_Room* room,
    int32_t state);

/* A terminal callback can occur for both disconnected and EOS; consumers must de-duplicate. */
typedef void (LKUB_CALL* LKUB_TerminalCallback)(
    void* context,
    LKUB_Room* room,
    int32_t disconnect_reason);

typedef void (LKUB_CALL* LKUB_UserDataCallback)(
    void* context,
    LKUB_Room* room,
    const LKUB_UserData* data);

typedef struct LKUB_RoomCallbacks
{
    uint32_t struct_size;
    uint32_t abi_version;
    LKUB_ParticipantCallback participant_connected;
    LKUB_ParticipantCallback participant_disconnected;
    LKUB_SpeakingCallback speaking_changed;
    LKUB_ConnectionStateCallback connection_state_changed;
    LKUB_TerminalCallback terminal;
    LKUB_UserDataCallback user_data_received;
} LKUB_RoomCallbacks;

typedef void (LKUB_CALL* LKUB_ParticipantVisitor)(
    void* context,
    const LKUB_ParticipantInfo* participant);

typedef struct LKUB_ByteStreamInfo
{
    LKUB_StringView sender_identity;
    LKUB_StringView stream_id;
    LKUB_StringView topic;
    LKUB_StringView name;
    LKUB_StringView mime_type;
    uint8_t has_total_size;
    uint8_t reserved[7];
    uint64_t total_size;
} LKUB_ByteStreamInfo;

/* Ownership of reader transfers to the callback and must be destroyed by the receiver. */
typedef void (LKUB_CALL* LKUB_ByteStreamCallback)(
    void* context,
    LKUB_Room* room,
    LKUB_ByteStreamReader* reader);

typedef void (LKUB_CALL* LKUB_AttributeVisitor)(
    void* context,
    LKUB_StringView key,
    LKUB_StringView value);

typedef struct LKUB_RpcInvocation
{
    LKUB_StringView request_id;
    LKUB_StringView caller_identity;
    LKUB_StringView method;
    LKUB_StringView payload;
    double response_timeout_seconds;
} LKUB_RpcInvocation;

typedef enum LKUB_RpcResponseKind
{
    LKUB_RPC_RESPONSE_SUCCESS = 0,
    LKUB_RPC_RESPONSE_ERROR = 1
} LKUB_RpcResponseKind;

/*
 * The handler fills this structure synchronously. Its views are borrowed and
 * must remain valid only until the handler returns; the adapter copies them.
 */
typedef struct LKUB_RpcResponse
{
    int32_t kind;
    uint8_t has_payload;
    uint8_t reserved[3];
    uint32_t error_code;
    LKUB_StringView payload;
    LKUB_StringView error_message;
    LKUB_StringView error_data;
} LKUB_RpcResponse;

typedef void (LKUB_CALL* LKUB_RpcHandler)(
    void* context,
    LKUB_Room* room,
    const LKUB_RpcInvocation* invocation,
    LKUB_RpcResponse* response);

LKUB_API uint32_t LKUB_CALL lkub_get_abi_version(void);

LKUB_API int32_t LKUB_CALL lkub_initialize(
    int32_t log_level,
    uint8_t* out_initialization_owned,
    LKUB_Result* out_result);

LKUB_API void LKUB_CALL lkub_shutdown(void);

LKUB_API void LKUB_CALL lkub_result_reset(LKUB_Result* result);

LKUB_API void LKUB_CALL lkub_buffer_release(LKUB_OwnedBuffer* buffer);

LKUB_API int32_t LKUB_CALL lkub_room_create(
    const LKUB_RoomCallbacks* callbacks,
    void* callback_context,
    LKUB_Room** out_room,
    LKUB_Result* out_result);

/* Prevent new callback dispatch; an already in-flight callback may still complete. */
LKUB_API void LKUB_CALL lkub_room_detach_callbacks(LKUB_Room* room);

/*
 * Destroy immediately only if no connection attempt was made. Once connect has
 * been called, retain the room until global SDK shutdown because the pinned SDK
 * exposes no listener-drain barrier, including on a failed connect attempt.
 */
LKUB_API void LKUB_CALL lkub_room_destroy(LKUB_Room* room);

/* Construct platform audio before connect so subscribed audio is played. */
LKUB_API int32_t LKUB_CALL lkub_room_prepare_audio(
    LKUB_Room* room,
    LKUB_Result* out_result);

LKUB_API int32_t LKUB_CALL lkub_room_connect(
    LKUB_Room* room,
    LKUB_StringView server_url,
    LKUB_StringView token,
    uint32_t connect_timeout_milliseconds,
    LKUB_Result* out_result);

/*
 * The pinned SDK exposes graceful-disconnect success, not a listener-drain
 * barrier. out_listener_drained is therefore conservatively always false;
 * any room that attempted connect must remain alive until lkub_shutdown.
 */
LKUB_API int32_t LKUB_CALL lkub_room_disconnect(
    LKUB_Room* room,
    uint8_t* out_listener_drained,
    LKUB_Result* out_result);

LKUB_API int32_t LKUB_CALL lkub_room_set_microphone_enabled(
    LKUB_Room* room,
    uint8_t enabled,
    LKUB_Result* out_result);

LKUB_API int32_t LKUB_CALL lkub_room_publish_data(
    LKUB_Room* room,
    LKUB_BytesView payload,
    int32_t reliability,
    LKUB_StringView topic,
    const LKUB_StringView* destination_identities,
    uint32_t destination_count,
    LKUB_Result* out_result);

LKUB_API int32_t LKUB_CALL lkub_room_visit_remote_participants(
    LKUB_Room* room,
    LKUB_ParticipantVisitor visitor,
    void* visitor_context,
    LKUB_Result* out_result);

LKUB_API int32_t LKUB_CALL lkub_room_register_byte_stream_handler(
    LKUB_Room* room,
    LKUB_StringView topic,
    LKUB_ByteStreamCallback callback,
    void* callback_context,
    LKUB_Result* out_result);

LKUB_API int32_t LKUB_CALL lkub_room_unregister_byte_stream_handler(
    LKUB_Room* room,
    LKUB_StringView topic,
    LKUB_Result* out_result);

LKUB_API int32_t LKUB_CALL lkub_byte_stream_get_info(
    LKUB_ByteStreamReader* reader,
    LKUB_ByteStreamInfo* out_info,
    LKUB_Result* out_result);

LKUB_API int32_t LKUB_CALL lkub_byte_stream_visit_attributes(
    LKUB_ByteStreamReader* reader,
    LKUB_AttributeVisitor visitor,
    void* visitor_context,
    LKUB_Result* out_result);

/* A successful EOS read sets out_end_of_stream and returns an empty buffer. */
LKUB_API int32_t LKUB_CALL lkub_byte_stream_read_next(
    LKUB_ByteStreamReader* reader,
    LKUB_OwnedBuffer* out_chunk,
    uint8_t* out_end_of_stream,
    LKUB_Result* out_result);

LKUB_API void LKUB_CALL lkub_byte_stream_cancel(LKUB_ByteStreamReader* reader);
LKUB_API void LKUB_CALL lkub_byte_stream_destroy(LKUB_ByteStreamReader* reader);

LKUB_API int32_t LKUB_CALL lkub_room_register_rpc_method(
    LKUB_Room* room,
    LKUB_StringView method,
    LKUB_RpcHandler handler,
    void* handler_context,
    LKUB_Result* out_result);

LKUB_API int32_t LKUB_CALL lkub_room_unregister_rpc_method(
    LKUB_Room* room,
    LKUB_StringView method,
    LKUB_Result* out_result);

LKUB_API int32_t LKUB_CALL lkub_room_perform_rpc(
    LKUB_Room* room,
    LKUB_StringView destination_identity,
    LKUB_StringView method,
    LKUB_StringView payload,
    double response_timeout_seconds,
    LKUB_OwnedBuffer* out_response,
    LKUB_Result* out_result);

#ifdef __cplusplus
} /* extern "C" */
#endif
