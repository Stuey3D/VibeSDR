/**
 * ★★★ THE SDRplay CARD on the Android server screen (ServerModeScreen; the main app's Server mode and VibeServer Lite).
 * The Setup page's card in its order and words: broadcast notch, DAB notch, AM notch (RSPduo), HDR (RSPdx family),
 * 24 MHz reference (RSP2, RSPduo), DAB decimation, automatic RF gain, VibeAGC, automatic notching, listeners toggling the
 * notches, the DAB AGC target — then the IF AGC target and its lock (the Setup page's gain section, RSP only) and the
 * aerial socket by band with its lock (the Setup page's aerial-socket card, only on a model with more than one socket).
 * ★ Drawn only for an RSP (the screen decides), and each row only on the models that have it (rspCaps).
 * ★ D-pad: every control is a TouchableOpacity, Switch or TvTextInput — Android's own focus search reaches them; the two
 *   targets are − / + keys, not a slider a remote cannot drag.
 */
import React from 'react';
import { View, Text, TouchableOpacity, Switch, StyleSheet } from 'react-native';
import TextInput from './TvTextInput';
import type { RspCard, RspCaps } from '../services/rspCard';

type Props = {
  C: any; F: string | undefined;
  card: RspCard; onChange: (c: RspCard) => void;
  caps: RspCaps;
  vibeAgc: number; onVibeAgc: (v: number) => void;
};

export default function RspSetupCard({ C, F, card, onChange, caps, vibeAgc, onVibeAgc }: Props) {
  const set = <K extends keyof RspCard>(k: K, v: RspCard[K]) => onChange({ ...card, [k]: v });
  const hint = [st.hint, { color: C.textDim, fontFamily: F }];
  const sw = (label: string, k: keyof RspCard, disabled = false) => (
    <View style={[st.row, { opacity: disabled ? 0.5 : 1 }]}>
      <Text style={{ color: C.text, fontFamily: F, fontSize: 14, flex: 1, paddingRight: 10 }}>{label}</Text>
      <Switch value={!!card[k]} disabled={disabled} onValueChange={(v) => set(k, v as any)}
        trackColor={{ false: C.trackOff, true: C.trackOn }} thumbColor={C.thumb} />
    </View>
  );
  const key = (label: string, onPress: () => void, active = false, disabled = false) => (
    <TouchableOpacity onPress={disabled ? undefined : onPress} disabled={disabled}
      style={[st.key, { borderColor: active ? C.keyBorderAct : C.keyBorder, opacity: disabled ? 0.45 : 1 }]}>
      <Text style={{ color: active ? C.keyAct : C.key, fontFamily: F, fontSize: 13 }}>{label}</Text>
    </TouchableOpacity>
  );
  const stepper = (k: 'dabAgcTarget' | 'agcSet', lo: number, hi: number, dflt: number) => {
    const v = card[k];
    return (
      <View style={[st.row, { justifyContent: 'flex-start', gap: 6, flexWrap: 'wrap' }]}>
        {key('−5', () => set(k, Math.max(lo, v - 5)), false, v <= lo)}
        {key('−1', () => set(k, Math.max(lo, v - 1)), false, v <= lo)}
        <Text style={{ color: C.text, fontFamily: F, fontSize: 14, minWidth: 92, textAlign: 'center' }}>
          {`${v} dBFS${v === dflt ? ' · default' : ''}`}
        </Text>
        {key('+1', () => set(k, Math.min(hi, v + 1)), false, v >= hi)}
        {key('+5', () => set(k, Math.min(hi, v + 5)), false, v >= hi)}
        {v !== dflt && key('Default', () => set(k, dflt))}
      </View>
    );
  };
  return (
    <View style={{ gap: 6 }}>
      {caps.rfNotch && (<>
        {sw('Broadcast notch', 'rfNotch', card.autoNotch)}
        <Text style={hint}>One switch, two filters, both ahead of the tuner: medium wave (522{'–'}1710 kHz, &gt;14 dB;
          660{'–'}1550 kHz, &gt;30 dB) and FM broadcast (77{'–'}115 MHz, &gt;30 dB). Use it on HF if a local transmitter
          is overloading the front end. Never use it to listen to FM {'—'} it removes exactly that. Two traps worth knowing:
          the FM notch is far wider than the broadcast band, so it also takes the bottom of airband, 108{'–'}115 MHz; and on
          160 m the medium-wave notch{'’'}s skirt has not finished rolling off, so top band is inside the attenuation.
          Automatic notching below knows both of these.</Text>
      </>)}
      {caps.dabNotch && (<>
        {sw('DAB notch', 'dabNotch', card.autoNotch)}
        <Text style={hint}>Same idea, for Band III: 160{'–'}235 MHz at better than 20 dB (165{'–'}230 MHz at better than
          30 dB). Note it starts at 160 MHz, so it also covers marine VHF.</Text>
      </>)}
      {card.autoNotch && (caps.rfNotch || caps.dabNotch) && (
        <Text style={hint}>Automatic notch filtering is on, so the settings above are only the starting state {'—'} the
          receiver sets them from the frequency being received from the first retune onward. Switch it off below to
          control them by hand.</Text>
      )}
      {caps.amNotch && (<>
        {sw('AM broadcast notch (RSPduo)', 'rspAmNotch')}
        <Text style={hint}>A separate filter from the broadcast notch above, on the RSPduo{'’'}s Hi-Z port (tuner 1): it
          cuts the medium-wave broadcast band ahead of the tuner. Useful on a wire antenna after dark, when a local MW
          transmitter is what is overloading the front end. Never on to listen to MW {'—'} it removes exactly that.</Text>
      </>)}
      {caps.hdr && (<>
        {sw('HDR mode below 2 MHz (RSPdx family)', 'rspHdr')}
        <Text style={hint}>The high dynamic range path, which the hardware handles itself. It is for exactly the case a long
          wire creates on medium wave and below: several strong broadcast signals at once, where the front end runs out of
          headroom before any one of them is loud.</Text>
      </>)}
      {caps.extRef && (<>
        {sw('24 MHz reference output (RSP2, RSPduo)', 'rspExtRef')}
        <Text style={hint}>Puts this radio{'’'}s clock out on the reference connector, for locking a second receiver to it.
          Leave it off unless something is plugged into that socket.</Text>
      </>)}
      {sw('DAB through the API’s decimation (flat ensemble)', 'rspDabDecim')}
      <Text style={hint}>The RSP has no 2.048 MHz IF filter: for DAB it is given the 1.536 MHz one, whose {'−'}3 dB points
        sit exactly on the multiplex{'’'}s outer carriers, so the edge subchannels arrive 3 dB down. On, the converter runs
        at 4.096 MS/s through the 5 MHz filter and the API decimates by two with its own sharp filter: the whole ensemble
        passes flat, the neighbouring blocks are removed digitally, and this server still receives 2.048 MS/s. 14-bit
        throughout. Takes effect on the next DAB entry. Off by default until it has been compared on air.</Text>
      {sw('Automatic RF gain', 'rfAgc')}
      <Text style={hint}>The RSP{'’'}s own IF AGC runs the IF stage; this moves the LNA to suit it. It acts only when the
        IF gain reduction is clearly outside its 30{'–'}50 dB working range, judged on an average, faster the further out
        it is, one step at a time with a dwell afterwards, and it respects any gain cap you have set per band. With VibeAGC
        running (below) it only says whether VibeAGC may move the LNA {'—'} off, it keeps to the IF and the RF gain stays
        where you put it.</Text>
      <Text style={[st.sub, { color: C.text, fontFamily: F }]}>VibeAGC (SDRplay)</Text>
      {([[0, 'Automatic — on with the OpenRSP driver'], [1, 'On'], [2, 'Off — the RSP’s own IF AGC']] as const).map(([v, label]) => (
        <TouchableOpacity key={v} onPress={() => onVibeAgc(v)}
          style={[st.opt, { borderColor: vibeAgc === v ? C.keyBorderAct : C.border }]}>
          <Text style={{ color: vibeAgc === v ? C.keyAct : C.key, fontFamily: F, fontSize: 14, flex: 1 }}>{label}</Text>
          {vibeAgc === v && <Text style={{ color: C.keyAct, fontFamily: F }}>✓</Text>}
        </TouchableOpacity>
      ))}
      <Text style={hint}>Our own gain control for both stages of the RSP, in place of the API{'’'}s IF AGC: it measures the
        converter level from the samples itself, moves the IF in small steps that are smoothed out of the audio, and takes
        RF gain out first whenever the front end is being driven hard. Automatic runs it only with the OpenRSP driver (the
        one this app uses). If the driver ever rejects a gain write, VibeAGC hands back to the RSP{'’'}s own AGC and says
        why in the log. You can also switch it live from the radio menu (the AGC row in the hardware panel, once the
        protected settings are unlocked); the choice here is what the server starts with.</Text>
      {(caps.rfNotch || caps.dabNotch) && (<>
        {sw('Automatic notch filtering', 'autoNotch')}
        <Text style={hint}>Sets both notches from the frequency being received, so they protect the front end without ever
          removing the band you are on. Below 2.5 MHz the broadcast notch comes off (medium wave and the whole of 160 m);
          75{'–'}117 MHz it comes off (FM, and the bottom of airband stays clear); everywhere else both go on. The DAB notch
          comes off from 158 MHz to the top of Band III, and throughout DAB mode. The receiver shows which it has chosen.</Text>
        {sw('Let listeners toggle the notches', 'userNotch')}
        <Text style={hint}>On a shared receiver the notches are part of the front end, so one listener{'’'}s change is
          everyone{'’'}s. Switch this off if you would rather own them {'—'} automatic notching owns them anyway while it is
          on. Either way listeners are told why, rather than finding a control that silently does nothing.</Text>
      </>)}
      {sw('Use a separate IF AGC target for DAB', 'dabAgcOverride')}
      {card.dabAgcOverride && (<>
        <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12 }}>DAB AGC target</Text>
        {stepper('dabAgcTarget', -60, -10, -40)}
      </>)}
      <Text style={hint}>The target applies while DAB is on, and the normal one comes back the moment DAB is left. Lower gives
        OFDM its peak headroom (an ensemble{'’'}s peaks run about 10 dB above the average the AGC levels); higher keeps the
        IF AGC off its end stop when strong blocks dominate the band. This steers the RSP{'’'}s own IF AGC, and the automatic
        RF gain above works from where that AGC settles.</Text>
      <Text style={[st.sub, { color: C.text, fontFamily: F }]}>IF AGC target</Text>
      {stepper('agcSet', -72, -10, -30)}
      <Text style={hint}>Where the AGC aims. A higher (less negative) figure asks for a louder output and so applies MORE gain,
        which on a strong band is how a front end is overloaded; a lower one runs quieter with more headroom. {'−'}30 dBFS is
        the default and suits most aerials. DAB takes this lower while it runs (the DAB target above) and gives it back on
        the way out.</Text>
      {sw('Lock the AGC target', 'agcSetLock')}
      <Text style={hint}>Listeners see it but cannot move it. Worth setting on a shared receiver: the target decides how hard
        everyone{'’'}s front end is driven.</Text>
      {caps.antennas.length > 1 && (<>
        <Text style={[st.sub, { color: C.text, fontFamily: F }]}>Aerial socket</Text>
        <Text style={hint}>{`This radio has ${caps.antennas.length} sockets: ${caps.antennas.join(', ')}.`}</Text>
        <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12 }}>Switch aerial by band</Text>
        <TextInput value={card.antennaMap} maxLength={300}
          onChangeText={(t) => set('antennaMap', t)}
          placeholder="0-30MHz Antenna C, 30-150MHz Antenna A, 150MHz+ antenna B" placeholderTextColor={C.placeholderDim}
          autoCapitalize="none" autoCorrect={false}
          style={[st.input, { color: C.text, borderColor: C.border, fontFamily: F }]} />
        <Text style={hint}>Leave empty to choose the aerial by hand. Ranges work the way the allowed bands do {'—'} a unit at
          either end covers both, and {'“'}150MHz+{'”'} means that frequency and upwards. The first rule that matches
          wins.</Text>
        {sw('Only I may change the aerial', 'antennaPortLocked')}
        <Text style={hint}>The aerial is shared hardware: changing it changes what every listener hears. On, only the admin
          password can move it {'—'} listeners still see which socket is in use, greyed, with the reason.</Text>
      </>)}
    </View>
  );
}

const st = StyleSheet.create({
  row: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginTop: 8 },
  hint: { fontSize: 11.5, lineHeight: 16 },
  sub: { fontSize: 14, marginTop: 12 },
  key: { borderWidth: 1, borderRadius: 8, paddingHorizontal: 12, paddingVertical: 8 },
  opt: { borderWidth: 1, borderRadius: 8, paddingHorizontal: 14, paddingVertical: 12, flexDirection: 'row', alignItems: 'center' },
  input: { borderWidth: 1, borderRadius: 8, paddingHorizontal: 12, paddingVertical: 10, fontSize: 15 },
});
