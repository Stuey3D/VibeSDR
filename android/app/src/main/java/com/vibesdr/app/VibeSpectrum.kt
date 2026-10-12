package com.vibesdr.app

/** The native spectrum engine (cpp/spectrum — docs/NATIVE-SPECTRUM-ENGINE.md): installs
 *  global.__vibeSpectrumCreate() into the JS runtime. Call on the JS thread. */
object VibeSpectrum {
    init { System.loadLibrary("vibespectrum") }
    @JvmStatic external fun nativeInstall(runtimePtr: Long): Boolean
}
