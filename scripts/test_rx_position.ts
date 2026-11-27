// test_rx_position.ts — b96-rangefeed (Stuart 11-27, the Lenovo: "i cant activate the range rings either"): WHERE A MAP'S
// RECEIVER POSITION COMES FROM (src/services/rxPosition.ts): lat/lon, else the locator square's centre (approximate), and on a
// front door the primary radio's. The layer's last fallback (the range outline's centre) is test-web-adsb-range.mjs's.
//   npx tsx scripts/test_rx_position.ts
import { rxFromLocation, rxApproxWords, doorRadioId } from '../src/services/rxPosition';

let passes = 0, fails = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  ok ? passes++ : fails++;
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${what}${ok ? '' : `\n     got  ${JSON.stringify(got)}\n     want ${JSON.stringify(want)}`}`);
};

// the Lenovo's radio, 11-27: lat/lon empty in its config, locator IO92nh — the server answers the square's centre
eq('the Lenovo: lat/lon = IO92nh\'s centre → approximate (locator)', rxFromLocation({ name: 'SDRplay RSP1A', lat: 52.3125, lon: -0.875, grid: 'IO92nh' }), { lat: 52.3125, lon: -0.875, from: 'locator' });
eq('an exact lat/lon in the same square → set', rxFromLocation({ lat: 52.2899, lon: -0.8512, grid: 'IO92nh' }), { lat: 52.2899, lon: -0.8512, from: 'set' });
eq('lat/lon only → set', rxFromLocation({ lat: 51.5, lon: -0.1 }), { lat: 51.5, lon: -0.1, from: 'set' });
eq('a grid only (an older server that sends no lat/lon) → its centre', rxFromLocation({ grid: 'IO92nh' }), { lat: 52.3125, lon: -0.875, from: 'locator' });
eq('"locator" spelled so', rxFromLocation({ locator: 'IO92' })?.from, 'locator');
eq('empty strings for lat/lon and a grid → the grid', rxFromLocation({ lat: '', lon: '', grid: 'IO92nh' })?.from, 'locator');
eq('Null Island is unknown', rxFromLocation({ lat: 0, lon: 0 }), null);
eq('nothing → null (the layer then uses its range outline\'s centre)', rxFromLocation({ name: 'x' }), null);
eq('RR73 is not a locator', rxFromLocation({ grid: 'RR73' }), null);
eq('the door\'s HTML page (not JSON) → null', rxFromLocation(null), null);
eq('the words: exact says nothing', rxApproxWords('set'), '');
eq('the words: the locator square, named', rxApproxWords('locator', 'IO92nh'), 'approximate — the centre of locator square IO92NH');
eq('the words: the outline\'s centre', rxApproxWords('range'), 'approximate — the centre of the server\'s ADS-B range outline');
eq('the door\'s primary radio', doorRadioId({ frontDoor: true, radios: [{ id: 'aa11' }, { id: '59a1a7d0', primary: true }] }), '59a1a7d0');
eq('…else the first', doorRadioId({ radios: [{ id: 'aa11' }, { id: 'bb22' }] }), 'aa11');
eq('…never a hostile id', doorRadioId({ radios: [{ id: '../x', primary: true }] }), '');
eq('…none', doorRadioId({}), '');

console.log(`\n${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
