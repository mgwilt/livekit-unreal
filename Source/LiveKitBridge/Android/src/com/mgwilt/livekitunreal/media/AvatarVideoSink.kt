package com.mgwilt.livekitunreal.media

import com.mgwilt.livekitunreal.LiveKitUnrealCallbacks
import livekit.org.webrtc.VideoFrame
import livekit.org.webrtc.VideoSink
import livekit.org.webrtc.YuvHelper
import java.nio.ByteBuffer

/** Conversion buffers are reused. Native delivery retains only the latest frame. */
class AvatarVideoSink(private val handle: Long, private val epoch: Long) : VideoSink {
    @Volatile var active = true
    private var pixels: ByteBuffer? = null
    @Synchronized override fun onFrame(frame: VideoFrame) {
        if (!active) return
        val planar = frame.buffer.toI420() ?: return
        try {
            val width = planar.width
            val height = planar.height
            if (width !in 1..3840 || height !in 1..3840) return
            val bytes = width * height + 2 * ((width + 1) / 2) * ((height + 1) / 2)
            val target = pixels?.takeIf { it.capacity() == bytes } ?: ByteBuffer.allocateDirect(bytes).also { pixels = it }
            target.clear()
            YuvHelper.I420Copy(planar.dataY, planar.strideY, planar.dataU, planar.strideU,
                planar.dataV, planar.strideV, target, width, height)
            if (active) LiveKitUnrealCallbacks.onVideo(handle, epoch, target, width, height,
                frame.rotation, frame.timestampNs / 1000)
        } finally { planar.release() }
    }
}
