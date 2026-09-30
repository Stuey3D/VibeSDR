/**
 * The ONE controls-haptics switch (menu ✦ HAPTICS) — read by the drums' ratchet, the tuner keys and
 * every dome key (faceplates §5: "Gate every key haptic on the existing controls-haptics setting").
 *
 * ★ Its own module, not DrumWheel's: DomeKey needs it, and DrumWheel now draws its well face with
 *   DomeKey's shared texture (§6.1) — the switch living in DrumWheel made that an import cycle.
 *   DrumWheel re-exports both functions, so existing callers are unchanged.
 * Module-level so SDRScreen can flip it without threading a prop through ControlsBar's layouts
 * into every drum and key.
 */
let _hapticsOn = true;
export function setDrumHaptics(on: boolean) { _hapticsOn = on; }
/** Same switch, read by every control that shares the setting. */
export function getControlHaptics() { return _hapticsOn; }
