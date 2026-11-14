package job

// MAKE ON A NEW CARD, end to end against image FILES: the original card is only read (it is made read-only on disk, so
// a single write would fail the test), and VibeSDR goes on a second card.

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func prepareJob(t *testing.T, src string, size int64, kind string) (*Job, *Prepared, error) {
	img, sum := testImage(t)
	dir := t.TempDir()
	j := &Job{Target: src, Size: size, BackupKind: kind, BackupPath: filepath.Join(dir, "backup", "card.img.gz"),
		ImagePath: img, ImageSHA256: sum, ImageVersion: "test", Tmp: dir}
	p, err := Prepare(j, func(Event) {})
	return j, p, err
}

func blankCard(t *testing.T, size int64, junk string) string {
	p := filepath.Join(t.TempDir(), "new.img")
	f, err := os.Create(p)
	if err != nil {
		t.Fatal(err)
	}
	f.Truncate(size)
	if junk != "" {
		f.WriteAt([]byte(strings.Repeat(junk, 4096)), 0) // whatever was on it before
	}
	f.Close()
	return p
}

// ★★★ The original is never written; the new card gets VibeSDR with THIS R36S's drivers (and the debug log).
func TestNewCardLeavesTheOriginal(t *testing.T) {
	const size = int64(5) << 30
	src, _ := firstBootCard(t, size) // a VibeSDR card with the debug log, settings and recordings
	before := fileHash(t, src, size)
	os.Chmod(src, 0o444) // ★ any write to the original now fails
	defer os.Chmod(src, 0o644)
	_, p, err := prepareJob(t, src, size, "boot")
	if err != nil {
		t.Fatalf("prepare: %v", err)
	}
	if fileHash(t, src, size) != before {
		t.Fatal("★ the original card changed")
	}
	dst := blankCard(t, int64(8)<<30, "OLDGAMES")
	img, sum := testImage(t)
	var evs []Event
	j := &Job{Target: dst, Size: int64(8) << 30, ImagePath: img, ImageSHA256: sum, Tmp: t.TempDir()}
	if err := WriteNew(j, p, func(e Event) { evs = append(evs, e) }); err != nil {
		t.Fatalf("write new: %v", err)
	}
	if !evs[len(evs)-1].Done || !strings.Contains(evs[len(evs)-1].Text, "original card is as it was") {
		t.Fatalf("done: %+v", evs[len(evs)-1])
	}
	ext := cardBoot(t, dst)
	if !strings.Contains(ext, "vibe.debuglog") || !strings.Contains(ext, "FDT /rk3326-gameconsole-eeclone.dtb") {
		t.Fatalf("drivers not carried to the new card:\n%s", ext)
	}
	if fileHash(t, src, size) != before {
		t.Fatal("★ the original card changed")
	}
	if _, err := os.Stat(p.BootFile); err == nil {
		t.Fatal("the prepared boot partition was left behind")
	}
}

// ★★ The original put back where the new card should be is refused, untouched.
func TestNewCardRefusesTheOriginal(t *testing.T) {
	const size = int64(5) << 30
	src, _ := firstBootCard(t, size)
	_, p, err := prepareJob(t, src, size, "boot")
	if err != nil {
		t.Fatalf("prepare: %v", err)
	}
	defer p.Discard()
	before := fileHash(t, src, size)
	img, sum := testImage(t)
	j := &Job{Target: src, Size: size, ImagePath: img, ImageSHA256: sum, Tmp: t.TempDir()}
	err = WriteNew(j, p, func(Event) {})
	if err == nil || !strings.Contains(err.Error(), "the card you started with") {
		t.Fatalf("want the original refused, got %v", err)
	}
	if fileHash(t, src, size) != before {
		t.Fatal("★ a refused write changed the original")
	}
}

// A new card too small for VibeSDR is refused before anything is written.
func TestNewCardTooSmall(t *testing.T) {
	const size = int64(5) << 30
	src, _ := firstBootCard(t, size)
	_, p, err := prepareJob(t, src, size, "boot")
	if err != nil {
		t.Fatalf("prepare: %v", err)
	}
	defer p.Discard()
	small := int64(2) << 30
	dst := blankCard(t, small, "KEEPME")
	before := fileHash(t, dst, small)
	img, sum := testImage(t)
	j := &Job{Target: dst, Size: small, ImagePath: img, ImageSHA256: sum, Tmp: t.TempDir()}
	if err := WriteNew(j, p, func(Event) {}); err == nil || !strings.Contains(err.Error(), "too small") {
		t.Fatalf("want too small, got %v", err)
	}
	if fileHash(t, dst, small) != before {
		t.Fatal("a refused card changed")
	}
}
