package release

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestFindLocal(t *testing.T) {
	dir := t.TempDir()
	if _, err := FindLocal(dir); err == nil || !strings.Contains(err.Error(), "no VibeSDR image") {
		t.Fatalf("empty folder: %v", err)
	}
	old := filepath.Join(dir, "vibesdr-r36s-12.0.0-beta74+handheld71-20261113.img.gz")
	nw := filepath.Join(dir, "vibesdr-r36s-12.0.0-beta75+handheld72-20261114.img.gz")
	os.WriteFile(old, []byte("a"), 0o644)
	os.WriteFile(old+".sha256", []byte(strings.Repeat("a", 64)+"  x\n"), 0o644)
	os.WriteFile(nw, []byte("b"), 0o644)
	os.WriteFile(filepath.Join(dir, "unrelated.img.gz"), []byte("c"), 0o644)
	now := time.Now()
	os.Chtimes(old, now.Add(-time.Hour), now.Add(-time.Hour))
	if _, err := FindLocal(dir); err == nil || !strings.Contains(err.Error(), "checksum") {
		t.Fatalf("the newest has no .sha256 — must be refused, not fall back: %v", err)
	}
	os.WriteFile(nw+".sha256", []byte(strings.Repeat("B", 64)+"  "+filepath.Base(nw)+"\n"), 0o644)
	img, err := FindLocal(dir)
	if err != nil || img.Version != "12.0.0-beta75+handheld72-20261114" || img.SHA256 != strings.Repeat("b", 64) || img.Source != "Desktop" {
		t.Fatalf("got %+v %v", img, err)
	}
}
