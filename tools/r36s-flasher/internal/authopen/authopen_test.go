package authopen

import (
	"os"
	"path/filepath"
	"testing"

	"golang.org/x/sys/unix"
)

// The hand-over itself: authopen opens a file of our own (no prompt needed) and the open file arrives over the socket,
// readable and writable, with the exclusive lock the card is opened with.
func TestReceiveHandsBackAnOpenFile(t *testing.T) {
	p := filepath.Join(t.TempDir(), "card.img")
	if err := os.WriteFile(p, []byte("VIBESDR-0123456789"), 0o600); err != nil {
		t.Fatal(err)
	}
	f, err := receive(p, unix.O_RDWR|unix.O_EXLOCK, nil)
	if err != nil {
		t.Fatalf("authopen: %v", err)
	}
	defer f.Close()
	b := make([]byte, 7)
	if _, err := f.ReadAt(b, 0); err != nil || string(b) != "VIBESDR" {
		t.Fatalf("read %q %v", b, err)
	}
	if _, err := f.WriteAt([]byte("vibe"), 0); err != nil {
		t.Fatalf("write: %v", err)
	}
	got, _ := os.ReadFile(p)
	if string(got[:4]) != "vibe" {
		t.Fatalf("the write did not land: %q", got)
	}
	// the exclusive lock is held: a second exclusive open must fail at once
	if g, err := os.OpenFile(p, os.O_RDWR|unix.O_EXLOCK|unix.O_NONBLOCK, 0); err == nil {
		g.Close()
		t.Fatal("the exclusive lock was not held")
	}
}

func TestReceiveReportsAFailure(t *testing.T) {
	if _, err := receive(filepath.Join(t.TempDir(), "missing"), unix.O_RDWR, nil); err == nil {
		t.Fatal("a missing file opened")
	}
}
