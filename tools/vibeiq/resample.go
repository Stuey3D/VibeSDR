package main

import "math"

// ★★★ WHY THE BRIDGE RESAMPLES. The tunnel carries 48 kHz — the ceiling Stuart set for the public
//     path — but an rtl_tcp app is an RTL-SDR app and its rate menu starts at 250 kHz (SDR++:
//     250k, 1.024M, …). Fed 48k samples labelled 250k it plays them five times too fast: "a
//     garbled mess" (Stuart, 2026-09-09). So the bridge converts the stream to whatever the app
//     asks for with SET_SAMPLE_RATE, on this machine, where CPU is free. The 48 kHz of real signal
//     sits in the middle of the wider container; the app's own filters do the rest.
//  ★ Polyphase rational resampler, L up / M down, windowed-sinc prototype. 250k out at 32 taps
//    per phase is ~16 MMAC/s for both channels — nothing on a PC.

type rational struct {
	l, m  int
	taps  int       // per phase
	h     []float32 // prototype, length l*taps, scaled by l
	hist  []float32 // last `taps` inputs, ring
	hpos  int
	phase int // upsampled-domain position within the current input sample
}

func gcd(a, b int) int {
	for b != 0 {
		a, b = b, a%b
	}
	return a
}

func newRational(in, out, taps int) *rational {
	g := gcd(in, out)
	r := &rational{l: out / g, m: in / g, taps: taps}
	n := r.l * taps
	r.h = make([]float32, n)
	// Cut-off at half the narrower rate, in the upsampled domain (in*l).
	fc := 0.5 * math.Min(float64(in), float64(out)) / (float64(in) * float64(r.l))
	mid := float64(n-1) / 2
	for i := 0; i < n; i++ {
		x := float64(i) - mid
		var s float64
		if x == 0 {
			s = 2 * fc
		} else {
			s = math.Sin(2*math.Pi*fc*x) / (math.Pi * x)
		}
		w := 0.42 - 0.5*math.Cos(2*math.Pi*float64(i)/float64(n-1)) + 0.08*math.Cos(4*math.Pi*float64(i)/float64(n-1)) // Blackman
		r.h[i] = float32(s * w * float64(r.l))
	}
	r.hist = make([]float32, taps)
	return r
}

// process converts one channel; returns the number of outputs written to out (cap ≥ len(in)*l/m+2).
func (r *rational) process(in []float32, out []float32) int {
	k := 0
	for _, x := range in {
		r.hist[r.hpos] = x
		r.hpos = (r.hpos + 1) % r.taps
		// Emit every output whose upsampled index falls within this input sample's l slots.
		for r.phase < r.l {
			var acc float32
			idx := r.hpos - 1
			for j := 0; j < r.taps; j++ {
				if idx < 0 {
					idx += r.taps
				}
				acc += r.h[r.phase+j*r.l] * r.hist[idx]
				idx--
			}
			out[k] = acc
			k++
			r.phase += r.m
		}
		r.phase -= r.l
	}
	return k
}

// iqResampler converts interleaved u8 IQ from one rate to another.
type iqResampler struct {
	in, out int
	ri, rq  *rational
	fi, fq  []float32
	oi, oq  []float32
}

func newIqResampler(in, out int) *iqResampler {
	return &iqResampler{in: in, out: out, ri: newRational(in, out, 32), rq: newRational(in, out, 32)}
}

func (s *iqResampler) convert(u8 []byte) []byte {
	n := len(u8) / 2
	if cap(s.fi) < n {
		s.fi, s.fq = make([]float32, n), make([]float32, n)
	}
	s.fi, s.fq = s.fi[:n], s.fq[:n]
	for i := 0; i < n; i++ {
		s.fi[i] = (float32(u8[2*i]) - 127.5) / 127.5
		s.fq[i] = (float32(u8[2*i+1]) - 127.5) / 127.5
	}
	capOut := n*s.ri.l/s.ri.m + 4
	if cap(s.oi) < capOut {
		s.oi, s.oq = make([]float32, capOut), make([]float32, capOut)
	}
	ni := s.ri.process(s.fi, s.oi[:capOut])
	nq := s.rq.process(s.fq, s.oq[:capOut])
	if nq < ni {
		ni = nq
	}
	res := make([]byte, ni*2)
	for i := 0; i < ni; i++ {
		res[2*i] = clampU8(s.oi[i])
		res[2*i+1] = clampU8(s.oq[i])
	}
	return res
}

func clampU8(v float32) byte {
	f := v*127 + 127.5
	if f < 0 {
		return 0
	}
	if f > 255 {
		return 255
	}
	return byte(f)
}
