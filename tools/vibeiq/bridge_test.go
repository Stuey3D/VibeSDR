package main

import "testing"

func TestCheckTarget(t *testing.T) {
	good := []struct{ in, host, path string }{
		{"abc.vibeserver.vibesdr.net", "abc.vibeserver.vibesdr.net", "/"},
		{"192.168.1.5:8073", "192.168.1.5:8073", "/"},
		{"[::1]:8073", "[::1]:8073", "/"},
		{"https://box.local/r/0001/", "box.local", "/r/0001/"},
		{"http://box.local/r/0001", "box.local", "/r/0001/"},
	}
	for _, g := range good {
		p, err := resolve("abcdef", g.in)
		if err != nil || p.host != g.host || p.path != g.path {
			t.Errorf("%q: got %+v, %v; want host %q path %q", g.in, p, err, g.host, g.path)
		}
	}
	for _, b := range []string{
		"evil.example@box.local", "box.local?x=1", "box.local#x", "box.local/../admin",
		"box.local/r/0001/../../x", "box.local:99999", "box.local:abc", "/r/0001/", "box local",
	} {
		if p, err := resolve("abcdef", b); err == nil {
			t.Errorf("%q: accepted as %+v", b, p)
		}
	}
	// The directory's answer goes through the same check.
	if _, err := checkTarget(pairing{host: "abc.vibeserver.vibesdr.net", path: "/r/x y/"}); err == nil {
		t.Error("a path with a space was accepted")
	}
}
