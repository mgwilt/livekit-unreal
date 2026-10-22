package com.mgwilt.livekitunreal;

public final class LiveKitUnrealCallbacks {
    private LiveKitUnrealCallbacks() {}
    public static native void onEvent(long handle, long generation, String event);
    public static native void onVideo(long handle, long generation, java.nio.ByteBuffer i420,
                                      int width, int height, int rotation, long timestamp);
    public static native void onAudio(long handle, long generation, short[] samples,
                                      int sampleRate, int channels);
}
