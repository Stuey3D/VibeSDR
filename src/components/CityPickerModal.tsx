import React, { useMemo, useState } from 'react';
import { Modal, View, Text, TextInput, FlatList, TouchableOpacity, StyleSheet } from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import {
  usePopupStyles, usePopupTheme, usePopupFrame, onMetal, engraveText, windowStyle,
  PopupKey, PopupPlate, PopupHandle, PopupWindow, type PopupTokens, scrollLane,
} from './PopupShell';
import { useSurface } from '../contexts/FaceplateContext';

// Rough receiver location for the FT8 map when device GPS is unavailable/denied
// (local hardware). The user picks a nearby city → "rough distances based on the
// selected city", per the brief. Curated list of major world cities; no geocoder.

export interface City { name: string; country: string; lat: number; lon: number; }

const CITIES: City[] = [
  { name: 'London', country: 'UK', lat: 51.51, lon: -0.13 },
  { name: 'Manchester', country: 'UK', lat: 53.48, lon: -2.24 },
  { name: 'Edinburgh', country: 'UK', lat: 55.95, lon: -3.19 },
  { name: 'Dublin', country: 'IE', lat: 53.35, lon: -6.26 },
  { name: 'Paris', country: 'FR', lat: 48.86, lon: 2.35 },
  { name: 'Madrid', country: 'ES', lat: 40.42, lon: -3.70 },
  { name: 'Lisbon', country: 'PT', lat: 38.72, lon: -9.14 },
  { name: 'Berlin', country: 'DE', lat: 52.52, lon: 13.40 },
  { name: 'Amsterdam', country: 'NL', lat: 52.37, lon: 4.90 },
  { name: 'Brussels', country: 'BE', lat: 50.85, lon: 4.35 },
  { name: 'Rome', country: 'IT', lat: 41.90, lon: 12.50 },
  { name: 'Zurich', country: 'CH', lat: 47.38, lon: 8.54 },
  { name: 'Vienna', country: 'AT', lat: 48.21, lon: 16.37 },
  { name: 'Prague', country: 'CZ', lat: 50.08, lon: 14.44 },
  { name: 'Warsaw', country: 'PL', lat: 52.23, lon: 21.01 },
  { name: 'Stockholm', country: 'SE', lat: 59.33, lon: 18.07 },
  { name: 'Oslo', country: 'NO', lat: 59.91, lon: 10.75 },
  { name: 'Copenhagen', country: 'DK', lat: 55.68, lon: 12.57 },
  { name: 'Helsinki', country: 'FI', lat: 60.17, lon: 24.94 },
  { name: 'Athens', country: 'GR', lat: 37.98, lon: 23.73 },
  { name: 'Moscow', country: 'RU', lat: 55.76, lon: 37.62 },
  { name: 'Istanbul', country: 'TR', lat: 41.01, lon: 28.98 },
  { name: 'New York', country: 'US', lat: 40.71, lon: -74.01 },
  { name: 'Chicago', country: 'US', lat: 41.88, lon: -87.63 },
  { name: 'Denver', country: 'US', lat: 39.74, lon: -104.99 },
  { name: 'Los Angeles', country: 'US', lat: 34.05, lon: -118.24 },
  { name: 'Seattle', country: 'US', lat: 47.61, lon: -122.33 },
  { name: 'Miami', country: 'US', lat: 25.76, lon: -80.19 },
  { name: 'Toronto', country: 'CA', lat: 43.65, lon: -79.38 },
  { name: 'Vancouver', country: 'CA', lat: 49.28, lon: -123.12 },
  { name: 'Mexico City', country: 'MX', lat: 19.43, lon: -99.13 },
  { name: 'São Paulo', country: 'BR', lat: -23.55, lon: -46.63 },
  { name: 'Buenos Aires', country: 'AR', lat: -34.60, lon: -58.38 },
  { name: 'Johannesburg', country: 'ZA', lat: -26.20, lon: 28.05 },
  { name: 'Cairo', country: 'EG', lat: 30.04, lon: 31.24 },
  { name: 'Dubai', country: 'AE', lat: 25.20, lon: 55.27 },
  { name: 'Mumbai', country: 'IN', lat: 19.08, lon: 72.88 },
  { name: 'Delhi', country: 'IN', lat: 28.61, lon: 77.21 },
  { name: 'Singapore', country: 'SG', lat: 1.35, lon: 103.82 },
  { name: 'Bangkok', country: 'TH', lat: 13.76, lon: 100.50 },
  { name: 'Hong Kong', country: 'HK', lat: 22.32, lon: 114.17 },
  { name: 'Beijing', country: 'CN', lat: 39.90, lon: 116.41 },
  { name: 'Tokyo', country: 'JP', lat: 35.68, lon: 139.69 },
  { name: 'Seoul', country: 'KR', lat: 37.57, lon: 126.98 },
  { name: 'Sydney', country: 'AU', lat: -33.87, lon: 151.21 },
  { name: 'Melbourne', country: 'AU', lat: -37.81, lon: 144.96 },
  { name: 'Perth', country: 'AU', lat: -31.95, lon: 115.86 },
  { name: 'Auckland', country: 'NZ', lat: -36.85, lon: 174.76 },
];

interface Props {
  visible: boolean;
  onClose: () => void;
  onPick:  (c: City) => void;
}

export default function CityPickerModal({ visible, onClose, onPick }: Props) {
  const insets = useSafeAreaInsets();
  const s = usePopupStyles(makeS);
  const pt = usePopupTheme();
  // ★★★ Transparency OFF: no full-screen dim over the live waterfall (§10.2).
  const dim = useSurface().scrimOpacity > 0;
  const metalFrame = usePopupFrame(14, true);
  const [q, setQ] = useState('');
  const list = useMemo(() => {
    const s = q.trim().toLowerCase();
    if (!s) return CITIES;
    return CITIES.filter(c => c.name.toLowerCase().includes(s) || c.country.toLowerCase().includes(s));
  }, [q]);

  return (
    <Modal visible={visible} transparent animationType="slide" onRequestClose={onClose}>
      <View style={[s.backdrop, dim && s.backdropDim]}>
        <View style={[s.sheet, { paddingBottom: insets.bottom + 10 }, metalFrame, metalFrame && { paddingTop: 0 }]}>
          <PopupPlate radius={14} />
          <PopupHandle />
          <Text style={s.title}>SET LOCATION</Text>
          <Text style={s.sub}>Pick a nearby city for rough spot distances (no GPS).</Text>
          <TextInput
            style={s.search}
            placeholder="Search city or country…"
            placeholderTextColor={pt.metal ? pt.winDim : 'rgba(120,240,120,0.4)'}
            value={q}
            onChangeText={setQ}
            autoCorrect={false}
          />
          {/* ★ §10.3: the list is data — a recessed window on silver / black. */}
          <PopupWindow style={s.listWrap} metalStyle={s.listWin}>
          <FlatList
            data={list}
            keyExtractor={(c) => c.name}
            keyboardShouldPersistTaps="handled"
            style={s.list}
            // ★ The indicator's lane: the country code is right-aligned to the row's edge.
            contentContainerStyle={scrollLane}
            renderItem={({ item }) => (
              <TouchableOpacity style={s.row} onPress={() => onPick(item)} activeOpacity={0.6}>
                <Text style={s.city}>{item.name}</Text>
                <Text style={s.cc}>{item.country}</Text>
              </TouchableOpacity>
            )}
          />
          </PopupWindow>
          {pt.metal ? (
            <PopupKey label="CANCEL" onPress={onClose} height={32} style={{ alignSelf: 'center', width: 110, marginTop: 10 }} />
          ) : (
          <TouchableOpacity style={s.cancel} onPress={onClose} activeOpacity={0.7}>
            <Text style={s.cancelTxt}>CANCEL</Text>
          </TouchableOpacity>
          )}
        </View>
      </View>
    </Modal>
  );
}

const makeS = (pt: PopupTokens) => StyleSheet.create({
  backdrop: { flex: 1, justifyContent: 'flex-end' },
  backdropDim: { backgroundColor: 'rgba(0,0,0,0.6)' },
  sheet:    { backgroundColor: '#060906', borderTopWidth: 1, borderColor: 'rgba(80,200,80,0.4)',
              borderTopLeftRadius: 14, borderTopRightRadius: 14, paddingHorizontal: 16, paddingTop: 14, maxHeight: '80%' },
  title:    onMetal(pt, { color: 'rgba(120,240,120,0.95)', fontSize: 14, letterSpacing: 2, fontWeight: '600' }, engraveText(pt)),
  sub:      onMetal(pt, { color: 'rgba(80,200,80,0.6)', fontSize: 11, marginTop: 4, marginBottom: 10 }, engraveText(pt, pt.note)),
  search:   onMetal(pt, { backgroundColor: 'rgba(80,200,80,0.08)', borderWidth: 1, borderColor: 'rgba(80,200,80,0.3)',
              borderRadius: 8, color: '#bfe', paddingHorizontal: 12, paddingVertical: 9, fontSize: 14 },
              { ...windowStyle(pt), color: pt.readout }),
  listWrap: { flexShrink: 1 },
  listWin:  { marginTop: 8, paddingHorizontal: 10 },
  list:     onMetal(pt, { marginTop: 8 }, { marginTop: 0 }),
  row:      { flexDirection: 'row', justifyContent: 'space-between', paddingVertical: 11,
              borderBottomWidth: StyleSheet.hairlineWidth, borderColor: 'rgba(80,200,80,0.15)' },
  city:     onMetal(pt, { color: 'rgba(180,255,180,0.95)', fontSize: 15 }, { color: pt.winText }),
  cc:       onMetal(pt, { color: 'rgba(80,200,80,0.55)', fontSize: 12, marginLeft: 8 }, { color: pt.winDim }),
  cancel:   { marginTop: 10, alignSelf: 'center', paddingVertical: 8, paddingHorizontal: 24 },
  cancelTxt:{ color: 'rgba(80,200,80,0.7)', fontSize: 13, letterSpacing: 1 },
});
