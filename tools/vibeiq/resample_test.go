package main

import (
	"math"
	"testing"
)

// A 5 kHz tone at 48k must come out as a 5 kHz tone at 250k with the same amplitude.
func TestResampleTone(t *testing.T) {
	rs := newIqResampler(48000, 250000)
	n := 4800
	in := make([]byte, n*2)
	for i := 0; i < n; i++ {
		ph := 2 * math.Pi * 5000 * float64(i) / 48000
		in[2*i] = clampU8(float32(0.5 * math.Cos(ph)))
		in[2*i+1] = clampU8(float32(0.5 * math.Sin(ph)))
	}
	out := rs.convert(in)
	m := len(out) / 2
	if m < 24000 || m > 25100 {
		t.Fatalf("got %d outputs for %d inputs", m, n)
	}
	// Measure the tone: correlate against 5 kHz at 250k over the settled tail.
	var re, im, pw float64
	for i := m / 2; i < m; i++ {
		x := (float64(out[2*i]) - 127.5) / 127.5
		y := (float64(out[2*i+1]) - 127.5) / 127.5
		ph := 2 * math.Pi * 5000 * float64(i) / 250000
		re += x*math.Cos(ph) + y*math.Sin(ph)
		im += y*math.Cos(ph) - x*math.Sin(ph)
		pw += x*x + y*y
	}
	cnt := float64(m - m/2)
	amp := math.Hypot(re, im) / cnt
	rms := math.Sqrt(pw / cnt)
	t.Logf("tone amplitude %.3f (want ~0.5), rms %.3f (want ~0.5)", amp, rms)
	if amp < 0.45 || amp > 0.55 || rms > 0.55 {
		t.Fatalf("tone not preserved: amp %.3f rms %.3f", amp, rms)
	}
}
