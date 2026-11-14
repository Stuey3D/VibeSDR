package job

// End-to-end: the whole Simple-mode run against image FILES as the "card" — never a real disk.
// Needs a real VibeSDR image: VIBE_TEST_IMAGE, else ~/R36S-images/vibesdr-r36s-12.0.0-beta74+handheld71-20261113.img.gz.

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"vibesdr.net/r36s-flasher/internal/card"

	"github.com/diskfs/go-diskfs/backend/file"
	"github.com/diskfs/go-diskfs/filesystem/fat32"
	"github.com/klauspost/pgzip"
)

func testImage(t *testing.T) (string, string) {
	p := os.Getenv("VIBE_TEST_IMAGE")
	if p == "" {
		h, _ := os.UserHomeDir()
		p = filepath.Join(h, "R36S-images", "vibesdr-r36s-12.0.0-beta74+handheld71-20261113.img.gz")
	}
	b, err := os.ReadFile(p + ".sha256")
	if err != nil {
		t.Skip("no test image: " + p)
	}
	return p, strings.Fields(string(b))[0]
}

func fileHash(t *testing.T, p string, n int64) string {
	f, err := os.Open(p)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.CopyN(h, f, n); err != nil {
		t.Fatal(err)
	}
	return hex.EncodeToString(h.Sum(nil))
}

func gunzipTo(t *testing.T, src, dst string, size int64) {
	in, _ := os.Open(src)
	defer in.Close()
	zr, err := pgzip.NewReader(in)
	if err != nil {
		t.Fatal(err)
	}
	out, err := os.Create(dst)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := io.Copy(out, zr); err != nil {
		t.Fatal(err)
	}
	out.Truncate(size)
	out.Close()
}

// stockCard: a 3 GB card with an MBR, a 64 MiB FAT32 boot partition holding dtb, and a Linux partition after it.
func stockCard(t *testing.T, p string, size int64, dtb []byte) {
	f, err := os.Create(p)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	f.Truncate(size)
	mbr := make([]byte, 512)
	put := func(i int, typ byte, start, n uint32) {
		e := mbr[446+16*i:]
		e[4] = typ
		binary.LittleEndian.PutUint32(e[8:], start)
		binary.LittleEndian.PutUint32(e[12:], n)
	}
	put(0, 0x0c, 2048, 131072)        // 64 MiB at 1 MiB
	put(1, 0x83, 133120, 2*1024*2048) // 2 GiB
	mbr[510], mbr[511] = 0x55, 0xaa
	f.WriteAt(mbr, 0)
	fsys, err := fat32.Create(file.New(f, false), 64<<20, 1<<20, 512, "BOOT", false)
	if err != nil {
		t.Fatal(err)
	}
	w, err := fsys.OpenFile("/rk3326-r35s-linux.dtb", os.O_RDWR|os.O_CREATE)
	if err != nil {
		t.Fatal(err)
	}
	w.Write(dtb)
	w.Close()
}

func runJob(t *testing.T, cardPath string, size int64, kind string) (*Job, []Event, error) {
	img, sum := testImage(t)
	dir := t.TempDir()
	j := &Job{Target: cardPath, Size: size, BackupKind: kind, BackupPath: filepath.Join(dir, "backup", "card.img.gz"),
		ImagePath: img, ImageSHA256: sum, ImageVersion: "test", Tmp: dir}
	var evs []Event
	err := Run(j, func(e Event) { evs = append(evs, e) })
	return j, evs, err
}

func imageParts(t *testing.T) []card.Part {
	img, _ := testImage(t)
	f, _ := os.Open(img)
	defer f.Close()
	zr, _ := pgzip.NewReader(f)
	head := make([]byte, 512)
	io.ReadFull(zr, head)
	parts, err := card.ReadMBR(bytes.NewReader(head))
	if err != nil {
		t.Fatal(err)
	}
	return parts
}

func cardBoot(t *testing.T, cardPath string) string {
	f, _ := os.Open(cardPath)
	defer f.Close()
	parts, err := card.ReadMBR(f)
	if err != nil {
		t.Fatal(err)
	}
	p1, err := card.ExtractPart(f, parts[0], t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer p1.Close()
	fsys, err := card.OpenFAT(p1)
	if err != nil {
		t.Fatal(err)
	}
	b, err := card.ReadFile(fsys, "extlinux/extlinux.conf")
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

// ★ Stuart's case: a VibeSDR card with vibe.debuglog added — a whole-card backup that round-trips, the boot menu's
//   choice carried over, the debug log kept, the image's own partitions written.
func TestVibeSDRCardWholeBackup(t *testing.T) {
	img, _ := testImage(t)
	dir := t.TempDir()
	cardPath := filepath.Join(dir, "card.img")
	const size = int64(4) << 30
	gunzipTo(t, img, cardPath, size)
	// the owner's debug log, as on Stuart's card
	{
		f, _ := os.OpenFile(cardPath, os.O_RDWR, 0)
		parts, _ := card.ReadMBR(f)
		fsys, err := fat32.Read(file.New(f, false), parts[0].Size(), parts[0].Offset(), 0)
		if err != nil {
			t.Fatal(err)
		}
		ext, _ := card.ReadFile(fsys, "extlinux/extlinux.conf")
		pl := &card.Plan{FDT: card.ParseExt(string(ext)).FDT, Overlays: card.ParseExt(string(ext)).Overlays, AddArgs: []string{"vibe.debuglog"}}
		w, _ := fsys.OpenFile("/extlinux/extlinux.conf", os.O_RDWR|os.O_TRUNC)
		w.Write([]byte(card.RewriteExt(string(ext), pl)))
		w.Close()
		f.Close()
	}
	before := fileHash(t, cardPath, size)
	j, evs, err := runJob(t, cardPath, size, "whole")
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	if !evs[len(evs)-1].Done {
		t.Fatal("no done event")
	}
	// the backup is the card as it was, byte for byte
	if got, n, err := gunzipHash(j.BackupPath, func(int64) {}); err != nil || got != before || n != size {
		t.Fatalf("backup does not round-trip: %v %d", err, n)
	}
	ext := cardBoot(t, cardPath)
	if !strings.Contains(ext, "vibe.debuglog") || !strings.Contains(ext, "FDT /rk3326-gameconsole-eeclone.dtb") {
		t.Fatalf("drivers not carried over:\n%s", ext)
	}
	f, _ := os.Open(cardPath)
	parts, _ := card.ReadMBR(f)
	f.Close()
	want := imageParts(t)
	if len(parts) != len(want) || parts[1].Size() != want[1].Size() {
		t.Fatalf("partition table: %+v want %+v", parts, want)
	}
	t.Logf("p2 = %d MiB (3072 from beta75 on)", parts[1].Size()>>20)
	for _, e := range evs {
		if e.Frac < 0 || e.Frac > 1.0001 {
			t.Fatalf("progress out of range: %+v", e)
		}
	}
}

// A stock card of the board we know: boot-only backup, nothing changed in the image's boot menu.
func TestStockKnownBoard(t *testing.T) {
	dtb, _ := os.ReadFile("../card/testdata/rf3536k3ka.dtb")
	dir := t.TempDir()
	cardPath := filepath.Join(dir, "card.img")
	const size = int64(4) << 30 // ★ room for a 3 GB-root image (beta75+, ~3.5 GB)
	stockCard(t, cardPath, size, dtb)
	j, _, err := runJob(t, cardPath, size, "boot")
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	if _, n, err := gunzipHash(j.BackupPath, func(int64) {}); err != nil || n != int64(2048+131072)*512 {
		t.Fatalf("boot backup should be 1 MiB + the 64 MiB boot partition, got %d (%v)", n, err)
	}
	if ext := cardBoot(t, cardPath); !strings.Contains(ext, "FDT /rk3326-gameconsole-eeclone.dtb") {
		t.Fatalf("stock card's boot menu: %s", ext)
	}
}

// A stock card of a board we do NOT know: stopped before writing, card untouched, backup made.
func TestStockUnknownRefused(t *testing.T) {
	dtb, _ := os.ReadFile("../card/testdata/rf3536k3ka.dtb")
	dtb[len(dtb)-1] ^= 0xff // one byte different: not the board we know
	dir := t.TempDir()
	cardPath := filepath.Join(dir, "card.img")
	const size = int64(3) << 30
	stockCard(t, cardPath, size, dtb)
	before := fileHash(t, cardPath, size)
	j, _, err := runJob(t, cardPath, size, "boot")
	var fe Friendly
	if !errors.As(err, &fe) || !strings.Contains(fe.Msg, "isn't one VibeSDR knows") || !strings.Contains(fe.Msg, "backup is safe") {
		t.Fatalf("want a plain refusal, got %v", err)
	}
	if fileHash(t, cardPath, size) != before {
		t.Fatal("the card was changed")
	}
	if _, err := os.Stat(j.BackupPath); err != nil {
		t.Fatal("no backup")
	}
	t.Log(fe.Msg)
}

// A card that does not keep what it is given: the read-back catches it.
func TestCorruptWriteCaught(t *testing.T) {
	dtb, _ := os.ReadFile("../card/testdata/rf3536k3ka.dtb")
	dir := t.TempDir()
	cardPath := filepath.Join(dir, "card.img")
	const size = int64(4) << 30 // ★ room for a 3 GB-root image (beta75+)
	stockCard(t, cardPath, size, dtb)
	testCorrupt = func(off int64, b []byte) {
		if off == 200<<20 {
			b[1234] ^= 0x01
		}
	}
	defer func() { testCorrupt = nil }()
	_, _, err := runJob(t, cardPath, size, "boot")
	if err == nil || !strings.Contains(err.Error(), "did not read back correctly") {
		t.Fatalf("want a read-back failure, got %v", err)
	}
}

// Too small a card: refused before writing.
func TestTooSmall(t *testing.T) {
	dtb, _ := os.ReadFile("../card/testdata/rf3536k3ka.dtb")
	dir := t.TempDir()
	cardPath := filepath.Join(dir, "card.img")
	const size = int64(1) << 30
	f, _ := os.Create(cardPath)
	f.Close()
	stockCard(t, cardPath, int64(3)<<30, dtb)
	os.Truncate(cardPath, size)
	before := fileHash(t, cardPath, size)
	_, _, err := runJob(t, cardPath, size, "boot")
	if err == nil || !strings.Contains(err.Error(), "too small") {
		t.Fatalf("want too small, got %v", err)
	}
	if fileHash(t, cardPath, size) != before {
		t.Fatal("the card was changed")
	}
}

// A wrong image checksum: nothing done at all.
func TestBadImageChecksum(t *testing.T) {
	img, _ := testImage(t)
	dir := t.TempDir()
	cardPath := filepath.Join(dir, "card.img")
	os.WriteFile(cardPath, make([]byte, 1<<20), 0o644)
	j := &Job{Target: cardPath, Size: 1 << 20, BackupKind: "boot", BackupPath: filepath.Join(dir, "b", "x.img.gz"),
		ImagePath: img, ImageSHA256: strings.Repeat("0", 64), Tmp: dir}
	err := Run(j, func(Event) {})
	if err == nil || !strings.Contains(err.Error(), "does not match its checksum") {
		t.Fatalf("got %v", err)
	}
	if _, err := os.Stat(j.BackupPath); err == nil {
		t.Fatal("a backup was made before the image was checked")
	}
}
