package com.vibesdr.app

/** The native spectrum engine (cpp/spectrum — docs/NATIVE-SPECTRUM-ENGINE.md): installs
 *  global.__vibeSpectrumCreate() into the JS runtime. Call on the JS thread. */
object VibeSpectrum {
    init { System.loadLibrary("vibespectrum") }
    @JvmStatic external fun nativeInstall(runtimePtr: Long): Boolean
    // The native spectrum socket's sinks (cpp/spectrum/spec_socket.cpp) — see VibeSpecSocketModule.
    @JvmStatic external fun nativeSinkCreate(id: Int, vibeServer: Boolean)
    @JvmStatic external fun nativeSinkDestroy(id: Int)
    /** TRUE = the queue was empty: tell JS to drain. */
    @JvmStatic external fun nativeSinkText(id: Int, utf8: ByteArray): Boolean
    @JvmStatic external fun nativeSinkBinary(id: Int, data: ByteArray): Boolean
}
