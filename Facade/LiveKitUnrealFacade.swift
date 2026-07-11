import Foundation
import LiveKit

private actor LiveKitUnrealPendingRpcStore {
    private var pending: [String: CheckedContinuation<String, Error>] = [:]
    private var closeError: Error?

    func wait(
        requestId: String,
        timeout: TimeInterval,
        dispatch: @escaping @Sendable () -> Void
    ) async throws -> String {
        try await withCheckedThrowingContinuation { continuation in
            if let closeError {
                continuation.resume(throwing: closeError)
                return
            }

            guard pending[requestId] == nil else {
                continuation.resume(throwing: NSError(
                    domain: "LiveKitForUnreal.RPC",
                    code: 4,
                    userInfo: [NSLocalizedDescriptionKey: "An RPC invocation with this request ID is already pending."]
                ))
                return
            }

            pending[requestId] = continuation
            dispatch()

            Task { [weak self] in
                let nanoseconds = UInt64(max(0.25, timeout) * 1_000_000_000)
                try? await Task.sleep(nanoseconds: nanoseconds)
                await self?.fail(
                    requestId: requestId,
                    error: NSError(
                        domain: "LiveKitForUnreal.RPC",
                        code: 1500,
                        userInfo: [NSLocalizedDescriptionKey: "The Unreal RPC handler timed out."]
                    )
                )
            }
        }
    }

    func complete(requestId: String, payload: String) {
        pending.removeValue(forKey: requestId)?.resume(returning: payload)
    }

    func fail(requestId: String, error: Error) {
        pending.removeValue(forKey: requestId)?.resume(throwing: error)
    }

    func close(error: Error) {
        guard closeError == nil else { return }
        closeError = error
        let continuations = pending.values
        pending.removeAll()
        for continuation in continuations {
            continuation.resume(throwing: error)
        }
    }
}

@objc(LiveKitUnrealRpcError)
@objcMembers
public final class LiveKitUnrealRpcError: NSObject, @unchecked Sendable {
    public let domain: String
    public let code: Int
    public let message: String
    public let data: String

    public init(domain: String, code: Int, message: String, data: String) {
        self.domain = domain
        self.code = code
        self.message = message
        self.data = data
        super.init()
    }

    fileprivate convenience init(_ error: Error) {
        if let rpcError = error as? RpcError {
            self.init(
                domain: "LiveKit.RpcError",
                code: rpcError.code,
                message: rpcError.message,
                data: rpcError.data
            )
            return
        }

        let nsError = error as NSError
        self.init(
            domain: nsError.domain,
            code: nsError.code,
            message: nsError.localizedDescription,
            data: ""
        )
    }
}

@objc(LiveKitUnrealSwiftFacade)
@objcMembers
public final class LiveKitUnrealSwiftFacade: NSObject, @unchecked Sendable {
    public typealias InvocationHandler = @Sendable (
        _ requestId: String,
        _ callerIdentity: String,
        _ method: String,
        _ payload: String,
        _ responseTimeout: TimeInterval
    ) -> Void

    public typealias ByteStreamCompletionHandler = @Sendable (
        _ senderIdentity: String,
        _ streamId: String,
        _ topic: String,
        _ name: String,
        _ mimeType: String,
        _ attributes: [String: String],
        _ data: Data?,
        _ error: NSError?
    ) -> Void

    private weak var room: Room?
    private let pending = LiveKitUnrealPendingRpcStore()
    private let methodsLock = NSLock()
    private var methods: Set<String> = []
    private var registrationsInFlight: Set<String> = []
    private var cancelledRegistrations: Set<String> = []
    private var byteStreamTopics: Set<String> = []
    private var byteStreamRegistrationsInFlight: Set<String> = []
    private var cancelledByteStreamRegistrations: Set<String> = []
    private var isShutDown = false

    @objc(initWithRoom:)
    public init(room: Room) {
        self.room = room
        super.init()
    }

    public func registerRpcMethod(
        _ method: String,
        invocationHandler: @escaping InvocationHandler,
        completion: @escaping @Sendable (NSError?) -> Void
    ) {
        let canRegister = methodsLock.withLock {
            guard !isShutDown,
                  !methods.contains(method),
                  !registrationsInFlight.contains(method)
            else {
                return false
            }
            registrationsInFlight.insert(method)
            return true
        }

        guard canRegister else {
            completion(NSError(
                domain: "LiveKitForUnreal.RPC",
                code: 5,
                userInfo: [NSLocalizedDescriptionKey: "The RPC method is already registered or the facade is shutting down."]
            ))
            return
        }

        guard let room else {
            _ = methodsLock.withLock {
                registrationsInFlight.remove(method)
            }
            completion(NSError(
                domain: "LiveKitForUnreal.RPC",
                code: 1,
                userInfo: [NSLocalizedDescriptionKey: "The LiveKit room is unavailable."]
            ))
            return
        }

        Task { [weak self] in
            do {
                try await room.registerRpcMethod(method) { [weak self] invocation in
                    guard let self else {
                        throw NSError(
                            domain: "LiveKitForUnreal.RPC",
                            code: 2,
                            userInfo: [NSLocalizedDescriptionKey: "The Unreal RPC bridge was released."]
                        )
                    }

                    let requestId = invocation.requestId
                    let callerIdentity = invocation.callerIdentity.stringValue
                    let payload = invocation.payload
                    let timeout = min(10.0, max(0.25, invocation.responseTimeout))
                    return try await self.pending.wait(
                        requestId: requestId,
                        timeout: timeout
                    ) {
                        invocationHandler(
                            requestId,
                            callerIdentity,
                            method,
                            payload,
                            timeout
                        )
                    }
                }

                let shouldKeepRegistration = self?.methodsLock.withLock {
                    self?.registrationsInFlight.remove(method)
                    let wasCancelled = self?.cancelledRegistrations.remove(method) != nil
                    guard self?.isShutDown == false, !wasCancelled else { return false }
                    self?.methods.insert(method)
                    return true
                } ?? false

                guard shouldKeepRegistration else {
                    await room.unregisterRpcMethod(method)
                    completion(NSError(
                        domain: "LiveKitForUnreal.RPC",
                        code: 6,
                        userInfo: [NSLocalizedDescriptionKey: "The RPC facade shut down while the method was being registered."]
                    ))
                    return
                }
                completion(nil)
            } catch {
                if let self {
                    self.methodsLock.withLock {
                        self.registrationsInFlight.remove(method)
                        self.cancelledRegistrations.remove(method)
                    }
                }
                completion(error as NSError)
            }
        }
    }

    public func unregisterRpcMethod(
        _ method: String,
        completion: @escaping @Sendable () -> Void
    ) {
        let shouldUnregister = methodsLock.withLock {
            if registrationsInFlight.contains(method) {
                cancelledRegistrations.insert(method)
                return false
            }
            return methods.remove(method) != nil
        }

        guard shouldUnregister else {
            completion()
            return
        }

        guard let room else {
            completion()
            return
        }

        Task {
            await room.unregisterRpcMethod(method)
            completion()
        }
    }

    public func registerByteStreamHandler(
        _ topic: String,
        maximumBytes: Int,
        streamHandler: @escaping ByteStreamCompletionHandler,
        completion: @escaping @Sendable (NSError?) -> Void
    ) {
        let canRegister = methodsLock.withLock {
            guard !isShutDown,
                  !byteStreamTopics.contains(topic),
                  !byteStreamRegistrationsInFlight.contains(topic)
            else {
                return false
            }
            byteStreamRegistrationsInFlight.insert(topic)
            return true
        }

        guard canRegister else {
            completion(NSError(
                domain: "LiveKitForUnreal.ByteStream",
                code: 5,
                userInfo: [NSLocalizedDescriptionKey: "The byte-stream topic is already registered or the facade is shutting down."]
            ))
            return
        }

        guard let room else {
            _ = methodsLock.withLock {
                byteStreamRegistrationsInFlight.remove(topic)
            }
            completion(NSError(
                domain: "LiveKitForUnreal.ByteStream",
                code: 1,
                userInfo: [NSLocalizedDescriptionKey: "The LiveKit room is unavailable."]
            ))
            return
        }

        Task { [weak self] in
            do {
                try await room.registerByteStreamHandler(for: topic) { [weak self] reader, senderIdentity in
                    guard let self, self.shouldDeliverByteStream(for: topic) else { return }

                    let info = reader.info
                    let streamId = info.id
                    let streamTopic = info.topic
                    let name = info.name ?? ""
                    let mimeType = info.mimeType
                    let attributes = info.attributes

                    do {
                        if let totalLength = info.totalLength, totalLength > maximumBytes {
                            throw Self.byteStreamTooLargeError(
                                topic: topic,
                                receivedBytes: totalLength,
                                maximumBytes: maximumBytes
                            )
                        }

                        var data = Data()
                        if let totalLength = info.totalLength, totalLength > 0 {
                            data.reserveCapacity(min(totalLength, maximumBytes))
                        }
                        for try await chunk in reader {
                            guard chunk.count <= maximumBytes - data.count else {
                                throw Self.byteStreamTooLargeError(
                                    topic: topic,
                                    receivedBytes: data.count + chunk.count,
                                    maximumBytes: maximumBytes
                                )
                            }
                            data.append(chunk)
                        }
                        guard self.shouldDeliverByteStream(for: topic) else { return }
                        streamHandler(
                            senderIdentity.stringValue,
                            streamId,
                            streamTopic,
                            name,
                            mimeType,
                            attributes,
                            data,
                            nil
                        )
                    } catch {
                        guard self.shouldDeliverByteStream(for: topic) else { return }
                        streamHandler(
                            senderIdentity.stringValue,
                            streamId,
                            streamTopic,
                            name,
                            mimeType,
                            attributes,
                            nil,
                            error as NSError
                        )
                    }
                }

                let shouldKeepRegistration = self?.methodsLock.withLock {
                    self?.byteStreamRegistrationsInFlight.remove(topic)
                    let wasCancelled = self?.cancelledByteStreamRegistrations.remove(topic) != nil
                    guard self?.isShutDown == false, !wasCancelled else { return false }
                    self?.byteStreamTopics.insert(topic)
                    return true
                } ?? false

                guard shouldKeepRegistration else {
                    await room.unregisterByteStreamHandler(for: topic)
                    completion(NSError(
                        domain: "LiveKitForUnreal.ByteStream",
                        code: 6,
                        userInfo: [NSLocalizedDescriptionKey: "The byte-stream registration was cancelled before it completed."]
                    ))
                    return
                }
                completion(nil)
            } catch {
                if let self {
                    self.methodsLock.withLock {
                        self.byteStreamRegistrationsInFlight.remove(topic)
                        self.cancelledByteStreamRegistrations.remove(topic)
                    }
                }
                completion(error as NSError)
            }
        }
    }

    public func unregisterByteStreamHandler(
        _ topic: String,
        completion: @escaping @Sendable () -> Void
    ) {
        let shouldUnregister = methodsLock.withLock {
            if byteStreamRegistrationsInFlight.contains(topic) {
                cancelledByteStreamRegistrations.insert(topic)
                return false
            }
            return byteStreamTopics.remove(topic) != nil
        }

        guard shouldUnregister else {
            completion()
            return
        }

        guard let room else {
            completion()
            return
        }

        Task {
            await room.unregisterByteStreamHandler(for: topic)
            completion()
        }
    }

    public func performRpc(
        destinationIdentity: String,
        method: String,
        payload: String,
        responseTimeout: TimeInterval,
        maxRoundTripLatency: TimeInterval,
        completion: @escaping @Sendable (String?, LiveKitUnrealRpcError?) -> Void
    ) {
        guard let room else {
            completion(nil, LiveKitUnrealRpcError(
                domain: "LiveKitForUnreal.RPC",
                code: 1,
                message: "The LiveKit room is unavailable.",
                data: ""
            ))
            return
        }

        Task {
            do {
                let response = try await room.localParticipant.performRpc(
                    destinationIdentity: Participant.Identity(from: destinationIdentity),
                    method: method,
                    payload: payload,
                    responseTimeout: responseTimeout,
                    maxRoundTripLatency: maxRoundTripLatency
                )
                completion(response, nil)
            } catch {
                completion(nil, LiveKitUnrealRpcError(error))
            }
        }
    }

    public func completeRpcInvocation(_ requestId: String, payload: String) {
        Task {
            await pending.complete(requestId: requestId, payload: payload)
        }
    }

    public func failRpcInvocation(
        _ requestId: String,
        code: Int,
        message: String,
        data: String
    ) {
        // LiveKit 2.15.1 does not expose a public RpcError initializer. Throwing
        // this NSError therefore reaches the caller as built-in 1500
        // (Application Error); code/message/data remain local diagnostic intent.
        Task {
            await pending.fail(
                requestId: requestId,
                error: NSError(
                    domain: "LiveKitForUnreal.RPC",
                    code: code,
                    userInfo: [
                        NSLocalizedDescriptionKey: message,
                        "data": data,
                    ]
                )
            )
        }
    }

    public func shutdown() {
        let registrationsToRemove = methodsLock.withLock { () -> ([String], [String]) in
            guard !isShutDown else { return ([], []) }
            isShutDown = true
            let rpcMethods = Array(methods)
            let streamTopics = Array(byteStreamTopics)
            methods.removeAll()
            byteStreamTopics.removeAll()
            return (rpcMethods, streamTopics)
        }
        if let room {
            Task {
                for method in registrationsToRemove.0 {
                    await room.unregisterRpcMethod(method)
                }
                for topic in registrationsToRemove.1 {
                    await room.unregisterByteStreamHandler(for: topic)
                }
            }
        }

        Task {
            await pending.close(error: NSError(
                domain: "LiveKitForUnreal.RPC",
                code: 3,
                userInfo: [NSLocalizedDescriptionKey: "The LiveKit room disconnected."]
            ))
        }
    }

    private func shouldDeliverByteStream(for topic: String) -> Bool {
        methodsLock.withLock {
            !isShutDown &&
                !cancelledByteStreamRegistrations.contains(topic) &&
                (byteStreamTopics.contains(topic) || byteStreamRegistrationsInFlight.contains(topic))
        }
    }

    private static func byteStreamTooLargeError(
        topic: String,
        receivedBytes: Int,
        maximumBytes: Int
    ) -> NSError {
        NSError(
            domain: "LiveKitForUnreal.ByteStream",
            code: 7,
            userInfo: [
                NSLocalizedDescriptionKey: "Byte stream on topic '\(topic)' exceeds the \(maximumBytes)-byte safety limit (received or declared \(receivedBytes) bytes)."
            ]
        )
    }
}
