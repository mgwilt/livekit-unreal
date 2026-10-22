package com.mgwilt.livekitunreal

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.util.Base64
import com.twilio.audioswitch.AudioDevice
import io.livekit.android.AudioOptions
import io.livekit.android.ConnectOptions
import io.livekit.android.LiveKit
import io.livekit.android.LiveKitOverrides
import io.livekit.android.events.RoomEvent
import io.livekit.android.events.collect
import io.livekit.android.room.Room
import io.livekit.android.room.participant.Participant
import io.livekit.android.room.participant.RemoteParticipant
import io.livekit.android.room.track.DataPublishReliability
import io.livekit.android.room.track.RemoteAudioTrack
import io.livekit.android.room.track.RemoteVideoTrack
import com.mgwilt.livekitunreal.media.AvatarVideoSink
import io.livekit.android.room.track.RemoteTrackPublication
import io.livekit.android.room.track.Track
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import livekit.org.webrtc.AudioTrackSink
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.nio.ByteOrder
import java.util.concurrent.CopyOnWriteArraySet

/** Unreal plays the PCM with the animation lookahead delay. */
class LiveKitUnrealBridge(private val activity: Activity, private val handle: Long) {
    companion object {
        private const val MIC_REQUEST = 7713
        private val instances = CopyOnWriteArraySet<LiveKitUnrealBridge>()
        @JvmStatic fun onPermissionsResult(request: Int, results: IntArray) {
            if (request == MIC_REQUEST) instances.forEach { it.permissionResult(results) }
        }
        @JvmStatic fun onStop() { instances.forEach { it.disconnect(it.generation) } }
    }

    @Volatile private var generation = 0L
    private var room: Room? = null
    private var scope: CoroutineScope? = null
    private var pending: (() -> Unit)? = null
    private var permissionGeneration: Long? = null
    private var selectedParticipant = ""
    private var selectedTrack = ""
    private var videoParticipant = ""
    private var videoName = ""
    private var videoTrack: RemoteVideoTrack? = null
    private var videoSink: AvatarVideoSink? = null
    fun configureVideo(participant: String, track: String) {
        videoParticipant = participant
        videoName = track
    }
    private fun detachVideo() {
        videoSink?.active = false
        val sink = videoSink
        if (sink != null) videoTrack?.removeRenderer(sink)
        videoSink = null
        videoTrack = null
    }
    private var audioTrack: RemoteAudioTrack? = null
    private var audioSink: AudioTrackSink? = null

    init { instances.add(this) }

    fun configuration(): String = try {
        val file = File(activity.filesDir, "LiveKit.local.json")
        if (file.length() in 1..32768) file.readText() else ""
    } catch (_: Exception) { "" }

    private fun event(epoch: Long, type: String, data: JSONObject = JSONObject()) {
        data.put("type", type)
        LiveKitUnrealCallbacks.onEvent(handle, epoch, data.toString())
    }
    private fun state(epoch: Long, value: String) {
        event(epoch, "state", JSONObject().put("state", value))
    }
    private fun error(epoch: Long, code: String) {
        event(epoch, "error", JSONObject().put("code", code))
    }

    fun connect(url: String, token: String, participant: String, track: String, epoch: Long,
                microphone: Boolean) {
        activity.runOnUiThread {
            closeRoom()
            generation = epoch
            selectedParticipant = participant
            selectedTrack = track
            val start = { if (generation == epoch) openRoom(url, token, epoch, microphone) }
            if (microphone && activity.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                pending = start
                val requested = permissionGeneration != null
                permissionGeneration = epoch
                if (!requested) {
                    activity.requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), MIC_REQUEST)
                }
            } else start()
        }
    }

    private fun permissionResult(results: IntArray) {
        val epoch = permissionGeneration
        permissionGeneration = null
        val start = pending
        pending = null
        if (epoch == null || epoch != generation) return
        if (results.isNotEmpty() && results[0] == PackageManager.PERMISSION_GRANTED) start?.invoke()
        else { error(epoch, "microphone_permission_denied"); state(epoch, "disconnected") }
    }

    private fun openRoom(url: String, token: String, epoch: Long, microphone: Boolean) {
        val jobs = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
        scope = jobs
        try {
            val current = LiveKit.create(activity.applicationContext,
                overrides = LiveKitOverrides(audioOptions = AudioOptions(disableAudioPrewarming = true)))
            room = current
            current.audioSwitchHandler?.apply {
                loggingEnabled = false
                preferredDeviceList = listOf(AudioDevice.BluetoothHeadset::class.java,
                    AudioDevice.WiredHeadset::class.java, AudioDevice.Speakerphone::class.java,
                    AudioDevice.Earpiece::class.java)
            }
            jobs.launch(start = CoroutineStart.UNDISPATCHED) {
                current.events.collect { incoming ->
                    if (generation != epoch || room !== current) return@collect
                    when (incoming) {
                        is RoomEvent.Connected -> {
                            subscribe(current)
                            state(epoch, "connected")
                        }
                        is RoomEvent.Reconnecting -> state(epoch, "reconnecting")
                        is RoomEvent.Reconnected -> { subscribe(current); state(epoch, "connected") }
                        is RoomEvent.Disconnected -> { closeRoom(); state(epoch, "disconnected") }
                        is RoomEvent.FailedToConnect -> { error(epoch, "connection_failed"); closeRoom(); state(epoch, "failed") }
                        is RoomEvent.ParticipantConnected -> {
                            participant(epoch, "participant_connected", incoming.participant)
                            subscribe(current)
                        }
                        is RoomEvent.ParticipantDisconnected -> participant(epoch, "participant_disconnected", incoming.participant)
                        is RoomEvent.TrackPublished -> subscribe(current)
                        is RoomEvent.TrackSubscribed -> {
                            val video = incoming.track as? RemoteVideoTrack
                            if (video != null && incoming.participant.identity?.value == videoParticipant &&
                                (videoName.isEmpty() || incoming.publication.name == videoName)) {
                                detachVideo()
                                val sink = AvatarVideoSink(handle, epoch)
                                videoTrack = video
                                videoSink = sink
                                video.addRenderer(sink)
                                event(epoch, "video_subscribed")
                            }
                            val remoteAudio = incoming.track as? RemoteAudioTrack
                            if (incoming.participant.identity?.value == selectedParticipant &&
                                (selectedTrack.isEmpty() || incoming.publication.name == selectedTrack) &&
                                remoteAudio != null) attachAudio(epoch, remoteAudio)
                        }
                        is RoomEvent.TrackUnsubscribed -> {
                            if (incoming.track === audioTrack) detachAudio()
                            if (incoming.track === videoTrack) detachVideo()
                        }
                        is RoomEvent.DataReceived -> event(epoch, "data", JSONObject()
                            .put("sender", incoming.participant?.identity?.value ?: "")
                            .put("topic", incoming.topic ?: "")
                            .put("data", Base64.encodeToString(incoming.data, Base64.NO_WRAP)))
                        else -> Unit
                    }
                }
            }
            jobs.launch {
                try {
                    current.connect(url, token, ConnectOptions(autoSubscribe = false, audio = false, video = false))
                    if (generation != epoch || room !== current) { current.disconnect(); return@launch }
                    current.remoteParticipants.values.forEach { participant(epoch, "participant_connected", it) }
                    subscribe(current)
                    if (microphone) microphone(true)
                } catch (cancelled: CancellationException) { throw cancelled }
                catch (_: Exception) { error(epoch, "connection_failed"); closeRoom(); state(epoch, "failed") }
            }
        } catch (_: Exception) { error(epoch, "sdk_initialization_failed"); closeRoom(); state(epoch, "failed") }
    }

    private fun participant(epoch: Long, type: String, person: RemoteParticipant) {
        event(epoch, type, JSONObject().put("identity", person.identity?.value ?: "")
            .put("sid", person.sid.value).put("name", person.name ?: ""))
    }
    private fun subscribe(current: Room) {
        current.remoteParticipants.values.forEach { person ->
            person.trackPublications.values.filterIsInstance<RemoteTrackPublication>().forEach { publication ->
                publication.setSubscribed(person.identity?.value == selectedParticipant &&
                    publication.kind == Track.Kind.AUDIO && (selectedTrack.isEmpty() || publication.name == selectedTrack) ||
                    person.identity?.value == videoParticipant && publication.kind == Track.Kind.VIDEO &&
                    (videoName.isEmpty() || publication.name == videoName))
            }
        }
    }
    private fun attachAudio(epoch: Long, track: RemoteAudioTrack) {
        if (audioTrack === track) return
        detachAudio()
        val sink = AudioTrackSink { buffer, bits, rate, channels, frames, _ ->
            if (buffer != null && bits == 16 && rate in 8000..192000 && channels in 1..2 &&
                frames in 1..19200 && buffer.remaining() >= frames * channels * 2 && generation == epoch) {
                val samples = ShortArray(frames * channels)
                buffer.duplicate().order(ByteOrder.nativeOrder()).asShortBuffer().get(samples)
                LiveKitUnrealCallbacks.onAudio(handle, epoch, samples, rate, channels)
            }
        }
        audioTrack = track
        audioSink = sink
        track.setVolume(0.0)
        event(epoch, "audio_subscribed")
        track.addSink(sink)
    }
    private fun detachAudio() {
        val track = audioTrack
        val sink = audioSink
        if (track != null && sink != null) track.removeSink(sink)
        audioTrack = null
        audioSink = null
    }
    fun microphone(enabled: Boolean) {
        activity.runOnUiThread {
            val epoch = generation
            val current = room ?: return@runOnUiThread
            if (enabled && activity.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                pending = { if (generation == epoch && room === current) microphone(true) }
                val requested = permissionGeneration != null
                permissionGeneration = epoch
                if (!requested) activity.requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), MIC_REQUEST)
                return@runOnUiThread
            }
            scope?.launch {
                try {
                    val success = current.localParticipant.setMicrophoneEnabled(enabled)
                    if (generation == epoch && room === current) {
                        if (success) event(epoch, "microphone", JSONObject().put("enabled", enabled))
                        else error(epoch, "microphone_publish_failed")
                    }
                } catch (cancelled: CancellationException) { throw cancelled }
                catch (_: Exception) { if (generation == epoch) error(epoch, "microphone_publish_failed") }
            }
        }
    }
    fun publish(id: String, data: String, topic: String, reliable: Boolean, destinations: String) {
        activity.runOnUiThread {
            val epoch = generation
            val current = room
            if (current == null) {
                event(epoch, "publish", JSONObject().put("id", id).put("success", false)); return@runOnUiThread
            }
            scope?.launch {
                try {
                    val list = JSONArray(destinations)
                    val identities = (0 until list.length()).map { Participant.Identity(list.getString(it)) }
                    val result = current.localParticipant.publishData(Base64.decode(data, Base64.DEFAULT),
                        if (reliable) DataPublishReliability.RELIABLE else DataPublishReliability.LOSSY,
                        topic, identities.ifEmpty { null })
                    if (generation == epoch) event(epoch, "publish", JSONObject().put("id", id).put("success", result.isSuccess))
                } catch (cancelled: CancellationException) { throw cancelled }
                catch (_: Exception) { if (generation == epoch) event(epoch, "publish", JSONObject().put("id", id).put("success", false)) }
            }
        }
    }
    fun disconnect(epoch: Long) {
        activity.runOnUiThread {
            generation = epoch
            closeRoom()
            state(epoch, "disconnected")
        }
    }
    private fun closeRoom() {
        pending = null
        detachVideo()
        detachAudio()
        val previous = room
        room = null
        scope?.cancel()
        scope = null
        previous?.disconnect()
        previous?.release()
        event(generation, "microphone", JSONObject().put("enabled", false))
    }
    fun shutdown() { activity.runOnUiThread { closeRoom(); instances.remove(this) } }
}
