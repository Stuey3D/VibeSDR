package com.vibesdr.app

import com.facebook.react.ReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.uimanager.ViewManager

class VibeStreamPackage : ReactPackage {
    override fun createNativeModules(ctx: ReactApplicationContext): List<NativeModule> =
        listOf(VibeStreamModule(ctx), VibeLocalSdrModule(ctx), VibeMdnsModule(ctx), VibeSpecSocketModule(ctx),
               VibeTranscriberModule(ctx))
    override fun createViewManagers(ctx: ReactApplicationContext): List<ViewManager<*, *>> =
        emptyList()
}
