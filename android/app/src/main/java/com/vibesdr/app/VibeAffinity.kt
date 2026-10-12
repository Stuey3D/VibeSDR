package com.vibesdr.app

/** The app's threads on the right cores at the right priority (cpp/vibe_affinity.cpp — the why lives there). */
object VibeAffinity {
    init { System.loadLibrary("vibelocalsdr") }   // no-op when VibeLocalSDR already loaded it
    /** Arm (SDR screen up, in front) or disarm the sweep. Returns the plan. */
    @JvmStatic external fun arm(on: Boolean): String
    /** The last sweep's counts. */
    @JvmStatic external fun lastSweep(): String
}
