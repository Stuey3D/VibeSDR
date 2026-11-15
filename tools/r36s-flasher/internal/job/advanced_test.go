package job

// ADVANCED end to end on image FILES as cards (never a real disk): drivers from a backup (.img.gz, .img.xz, .img, .zip),
// the generic ROCKNIX drivers, the person's own device tree — onto a BLANK card with no backup — and RESTORE.

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"vibesdr.net/r36s-flasher/internal/card"

	"github.com/klauspost/pgzip"
	"github.com/ulikunitz/xz"
)

func emptyCard(t *testing.T, p string, size int64) {
	f, err := os.Create(p)
	if err != nil {
		t.Fatal(err)
	}
	f.Truncate(size)
	f.Close()
}

// compressTo writes the first n bytes of src to dst, compressed by dst's extension (.gz, .xz, .zip; else plain).
func compressTo(t *testing.T, src, dst string, n int64) {
	in, _ := os.Open(src)
	defer in.Close()
	out, err := os.Create(dst)
	if err != nil {
		t.Fatal(err)
	}
	defer out.Close()
	var w io.WriteCloser
	switch {
	case strings.HasSuffix(dst, ".gz"):
		w = pgzip.NewWriter(out)
	case strings.HasSuffix(dst, ".xz"):
		xw, err := xz.NewWriter(out)
		if err != nil {
			t.Fatal(err)
		}
		w = xw
	case strings.HasSuffix(dst, ".zip"):
		zw := zip.NewWriter(out)
		zw.Create("readme.txt")
		e, _ := zw.Create("r36s-card.img")
		if _, err := io.CopyN(e, in, n); err != nil {
			t.Fatal(err)
		}
		zw.Close()
		return
	default:
		if _, err := io.CopyN(out, in, n); err != nil {
			t.Fatal(err)
		}
		return
	}
	if _, err := io.CopyN(w, in, n); err != nil {
		t.Fatal(err)
	}
	w.Close()
}

func advJob(t *testing.T, cardPath string, size int64, drivers string, set func(j *Job)) (*Job, []Event, error) {
	img, sum := testImage(t)
	dir := t.TempDir()
	j := &Job{Target: cardPath, Size: size, BackupKind: "none", BackupPath: filepath.Join(dir, "backup", "card.img.gz"),
		ImagePath: img, ImageSHA256: sum, ImageVersion: "test", Tmp: dir, Drivers: drivers}
	if set != nil {
		set(j)
	}
	var evs []Event
	err := Run(j, func(e Event) { evs = append(evs, e) })
	return j, evs, err
}

func bootFile(t *testing.T, cardPath, p string) []byte {
	f, _ := os.Open(cardPath)
	defer f.Close()
	parts, _ := card.ReadMBR(f)
	p1, err := card.ExtractPart(f, parts[0], t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer p1.Close()
	fsys, _ := card.OpenFAT(p1)
	b, err := card.ReadFile(fsys, p)
	if err != nil {
		t.Fatalf("%s not on the card's boot partition", p)
	}
	return b
}

// imageBootFile: a file from the test image's own boot partition.
func imageBootFile(t *testing.T, p string) []byte {
	img, _ := testImage(t)
	p1, _, err := imageBoot(img, t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer p1.Close()
	fsys, _ := card.OpenFAT(p1)
	b, err := card.ReadFile(fsys, p)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// GENERIC drivers onto a BLANK card, no backup: the image's own boot menu, untouched.
func TestAdvancedGenericBlankCard(t *testing.T) {
	dir := t.TempDir()
	cardPath := filepath.Join(dir, "blank.img")
	const size = int64(4) << 30
	emptyCard(t, cardPath, size)
	j, evs, err := advJob(t, cardPath, size, "generic", nil)
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	if !evs[len(evs)-1].Done {
		t.Fatal("no done event")
	}
	if _, err := os.Stat(j.BackupPath); err == nil {
		t.Fatal("a backup was made though none was asked for")
	}
	ext := cardBoot(t, cardPath)
	if !strings.Contains(ext, "FDT /rk3326-gameconsole-eeclone.dtb") || !strings.Contains(ext, "/overlays/mipi-panel.dtbo /overlays/vibe-r36s.dtbo") {
		t.Fatalf("generic boot menu changed:\n%s", ext)
	}
	used := ""
	for _, e := range evs {
		if strings.HasPrefix(e.Text, "Using ") {
			used = e.Text
		}
		if e.Frac < 0 || e.Frac > 1.0001 {
			t.Fatalf("progress out of range: %+v", e)
		}
	}
	if !strings.Contains(used, "generic ROCKNIX") {
		t.Fatalf("summary: %q", used)
	}
	t.Log(used)
}

// DRIVERS FROM A BACKUP of a stock card of the board we know — gzip, xz, zip and plain — onto a blank card.
func TestAdvancedFromStockBackup(t *testing.T) {
	dtb, _ := os.ReadFile("../card/testdata/rf3536k3ka.dtb")
	dir := t.TempDir()
	stock := filepath.Join(dir, "stock.img")
	stockCard(t, stock, 3<<30, dtb)
	span := int64(2048+131072) * 512 // the boot-only backup: 1 MiB + the boot partition
	for _, name := range []string{"stock-boot.img.gz", "stock-boot.img.xz", "stock-boot.zip", "stock-boot.img"} {
		t.Run(name, func(t *testing.T) {
			src := filepath.Join(dir, name)
			compressTo(t, stock, src, span)
			cardPath := filepath.Join(t.TempDir(), "blank.img")
			const size = int64(4) << 30
			emptyCard(t, cardPath, size)
			_, evs, err := advJob(t, cardPath, size, "file", func(j *Job) { j.DriverFile = src })
			if err != nil {
				t.Fatalf("run: %v", err)
			}
			if ext := cardBoot(t, cardPath); !strings.Contains(ext, "FDT /rk3326-gameconsole-eeclone.dtb") {
				t.Fatalf("boot menu: %s", ext)
			}
			for _, e := range evs {
				if strings.HasPrefix(e.Text, "Using ") && !strings.Contains(e.Text, "EE clone") {
					t.Fatalf("summary: %q", e.Text)
				}
			}
		})
	}
}

// DRIVERS FROM A VIBESDR CARD'S BACKUP: its boot menu's choice and the debug log come across, as Simple mode does.
func TestAdvancedFromVibeSDRBackup(t *testing.T) {
	img, _ := testImage(t)
	dir := t.TempDir()
	old := filepath.Join(dir, "old.img")
	gunzipTo(t, img, old, 4<<30)
	addDebugLog(t, old)
	parts := imageParts(t)
	src := filepath.Join(dir, "vibesdr-boot.img.gz")
	compressTo(t, old, src, parts[0].End())
	cardPath := filepath.Join(dir, "blank.img")
	const size = int64(4) << 30
	emptyCard(t, cardPath, size)
	if _, _, err := advJob(t, cardPath, size, "file", func(j *Job) { j.DriverFile = src }); err != nil {
		t.Fatalf("run: %v", err)
	}
	if ext := cardBoot(t, cardPath); !strings.Contains(ext, "vibe.debuglog") {
		t.Fatalf("debug log not carried from the backup:\n%s", ext)
	}
}

// A backup with no drivers VibeSDR knows: stopped BEFORE anything — the blank card stays blank.
func TestAdvancedFromUnknownRefused(t *testing.T) {
	dtb, _ := os.ReadFile("../card/testdata/rf3536k3ka.dtb")
	dtb[len(dtb)-1] ^= 0xff
	dir := t.TempDir()
	stock := filepath.Join(dir, "other.img")
	stockCard(t, stock, 3<<30, dtb)
	src := filepath.Join(dir, "other.img.gz")
	compressTo(t, stock, src, int64(2048+131072)*512)
	cardPath := filepath.Join(dir, "blank.img")
	const size = int64(4) << 30
	emptyCard(t, cardPath, size)
	before := fileHash(t, cardPath, 64<<20)
	_, _, err := advJob(t, cardPath, size, "file", func(j *Job) { j.DriverFile = src })
	var fe Friendly
	if !errors.As(err, &fe) || !strings.Contains(fe.Msg, "no drivers VibeSDR can use") || !strings.Contains(fe.Msg, "generic ROCKNIX") {
		t.Fatalf("want a plain refusal, got %v", err)
	}
	if fileHash(t, cardPath, 64<<20) != before {
		t.Fatal("the card was changed")
	}
	t.Log(fe.Msg)
}

// THE PERSON'S OWN DEVICE TREE (+ a panel overlay): copied to /own, the boot menu points at it, VibeSDR's own overlay kept.
func TestAdvancedOwnDTB(t *testing.T) {
	dir := t.TempDir()
	dtbPath := filepath.Join(dir, "my board.dtb")
	os.WriteFile(dtbPath, imageBootFile(t, "rk3326-gameconsole-eeclone.dtb"), 0o644)
	ovPath := filepath.Join(dir, "mipi-panel.dtbo")
	panel := imageBootFile(t, "overlays/mipi-panel.dtbo")
	os.WriteFile(ovPath, panel, 0o644)
	cardPath := filepath.Join(dir, "blank.img")
	const size = int64(4) << 30
	emptyCard(t, cardPath, size)
	if _, _, err := advJob(t, cardPath, size, "own", func(j *Job) { j.OwnDTB = dtbPath; j.OwnOverlays = []string{ovPath} }); err != nil {
		t.Fatalf("run: %v", err)
	}
	ext := cardBoot(t, cardPath)
	if !strings.Contains(ext, "FDT /own/my-board.dtb") || !strings.Contains(ext, "FDTOVERLAYS /own/overlays/mipi-panel.dtbo /overlays/vibe-r36s.dtbo") {
		t.Fatalf("boot menu:\n%s", ext)
	}
	if !bytes.Equal(bootFile(t, cardPath, "own/overlays/mipi-panel.dtbo"), panel) {
		t.Fatal("the overlay on the card is not the person's")
	}
}

// fakeDTB: a minimal device tree whose root says model and compatible.
func fakeDTB(model string, compat ...string) []byte {
	var st, strs bytes.Buffer
	u32 := func(b *bytes.Buffer, v uint32) { binary.Write(b, binary.BigEndian, v) }
	pad := func(b *bytes.Buffer) {
		for b.Len()%4 != 0 {
			b.WriteByte(0)
		}
	}
	prop := func(name string, val []byte) {
		off := strs.Len()
		strs.WriteString(name)
		strs.WriteByte(0)
		u32(&st, 3)
		u32(&st, uint32(len(val)))
		u32(&st, uint32(off))
		st.Write(val)
		pad(&st)
	}
	u32(&st, 1)
	st.Write([]byte{0, 0, 0, 0})
	prop("model", append([]byte(model), 0))
	prop("compatible", []byte(strings.Join(compat, "\x00")+"\x00"))
	u32(&st, 2)
	u32(&st, 9)
	const hdr = 40
	rsv := 16
	offStruct := hdr + rsv
	offStrings := offStruct + st.Len()
	total := offStrings + strs.Len()
	out := make([]byte, total)
	be := binary.BigEndian
	be.PutUint32(out[0:], 0xd00dfeed)
	be.PutUint32(out[4:], uint32(total))
	be.PutUint32(out[8:], uint32(offStruct))
	be.PutUint32(out[12:], uint32(offStrings))
	be.PutUint32(out[16:], uint32(hdr))
	be.PutUint32(out[20:], 17)
	be.PutUint32(out[24:], 16)
	be.PutUint32(out[32:], uint32(strs.Len()))
	be.PutUint32(out[36:], uint32(st.Len()))
	copy(out[offStruct:], st.Bytes())
	copy(out[offStrings:], strs.Bytes())
	return out
}

// OWN DEVICE TREES that cannot be used are refused in one sentence before anything is touched; the known stock DTB
// uses VibeSDR's own drivers for that board.
func TestAdvancedOwnChecks(t *testing.T) {
	dir := t.TempDir()
	cardPath := filepath.Join(dir, "blank.img")
	const size = int64(4) << 30
	emptyCard(t, cardPath, size)
	cases := []struct {
		name, want string
		dtb        []byte
	}{
		{"fake.dtb", "not for an RK3326", fakeDTB("GA36", "allwinner,sun8i-a33")},
		{"stock-other.dtb", "original device tree", fakeDTB("Rockchip rk3326 evb lpddr3 v10 board for linux", "rockchip,rk3326-evb-lp3-v10-linux", "rockchip,rk3326")},
		{"notes.dtb", "is not a device tree", []byte("hello")},
	}
	for _, c := range cases {
		p := filepath.Join(dir, c.name)
		os.WriteFile(p, c.dtb, 0o644)
		_, _, err := advJob(t, cardPath, size, "own", func(j *Job) { j.OwnDTB = p })
		var fe Friendly
		if !errors.As(err, &fe) || !strings.Contains(fe.Msg, c.want) {
			t.Fatalf("%s: want %q, got %v", c.name, c.want, err)
		}
		t.Log(fe.Msg)
	}
	// the known stock board's DTB → VibeSDR's own device tree for it
	p := filepath.Join(dir, "rf3536k3ka.dtb")
	b, _ := os.ReadFile("../card/testdata/rf3536k3ka.dtb")
	os.WriteFile(p, b, 0o644)
	if _, _, err := advJob(t, cardPath, size, "own", func(j *Job) { j.OwnDTB = p }); err != nil {
		t.Fatalf("known stock dtb: %v", err)
	}
	if ext := cardBoot(t, cardPath); !strings.Contains(ext, "FDT /rk3326-gameconsole-eeclone.dtb") || strings.Contains(ext, "/own/") {
		t.Fatalf("known stock dtb should use our device tree:\n%s", ext)
	}
}

// RESTORE: a whole-card backup put back on another card byte for byte (checked against its .sha256); a damaged backup
// and a too-small card are refused before anything is written.
func TestRestore(t *testing.T) {
	dtb, _ := os.ReadFile("../card/testdata/rf3536k3ka.dtb")
	dir := t.TempDir()
	orig := filepath.Join(dir, "orig.img")
	const size = int64(512) << 20
	stockCard(t, orig, size, dtb)
	{ // something in the "system" area, so the card is not all zeros
		f, _ := os.OpenFile(orig, os.O_RDWR, 0)
		f.WriteAt(bytes.Repeat([]byte("R36S"), 1<<18), 200<<20)
		f.Close()
	}
	bk := filepath.Join(dir, "bk", "orig.img.gz")
	f, _ := os.Open(orig)
	sum, err := backup(f, size, bk, func(int64) {})
	f.Close()
	if err != nil {
		t.Fatal(err)
	}
	writeBackupNotes(&Job{BackupPath: bk, BackupKind: "whole"}, size, sum)

	restore := func(cardPath string, csize int64, src string) error {
		j := &Job{Mode: "restore", Target: cardPath, Size: csize, BackupKind: "none", RestorePath: src, Tmp: t.TempDir()}
		return Run(j, func(Event) {})
	}
	// onto a bigger blank card
	cardPath := filepath.Join(dir, "card.img")
	emptyCard(t, cardPath, 1<<30)
	if err := restore(cardPath, 1<<30, bk); err != nil {
		t.Fatalf("restore: %v", err)
	}
	if fileHash(t, cardPath, size) != sum {
		t.Fatal("the card is not the backup")
	}
	// a too-small card
	small := filepath.Join(dir, "small.img")
	emptyCard(t, small, 256<<20)
	var fe Friendly
	if err := restore(small, 256<<20, bk); !errors.As(err, &fe) || !strings.Contains(fe.Msg, "bigger card") {
		t.Fatalf("too small: %v", err)
	}
	// a damaged backup (its .sha256 no longer matches)
	os.WriteFile(bk+".sha256", []byte(strings.Repeat("0", 64)+"  orig.img\n"), 0o644)
	if err := restore(cardPath, 1<<30, bk); !errors.As(err, &fe) || !strings.Contains(fe.Msg, "does not match the checksum") {
		t.Fatalf("damaged: %v", err)
	}
	// ★ a hand-made image that is not a whole number of sectors, onto a card that reads and writes ONLY whole sectors
	odd := filepath.Join(dir, "odd.img")
	{
		b, _ := os.ReadFile(orig)
		os.WriteFile(odd, b[:len(b)-100], 0o644)
	}
	sc := filepath.Join(dir, "sector.img")
	emptyCard(t, sc, 1<<30)
	j := &Job{Mode: "restore", Target: sc, Size: 1 << 30, BackupKind: "none", RestorePath: odd, Tmp: t.TempDir(),
		Open: func(p string, flag int) (*os.File, error) { return os.OpenFile(p, flag, 0) }}
	sectorOnlyIO = true
	err = Run(j, func(Event) {})
	sectorOnlyIO = false
	if err != nil {
		t.Fatalf("odd-sized image onto a sector-only card: %v", err)
	}
	if fileHash(t, sc, size-100) != fileHash(t, odd, size-100) {
		t.Fatal("the odd-sized image is not on the card")
	}
	// the backups list finds it
	if l := card.ListBackups(dir); len(l) != 1 || l[0].Kind != "whole" || l[0].Bytes != size {
		t.Fatalf("ListBackups: %+v", l)
	}
}

func addDebugLog(t *testing.T, cardPath string) {
	f, _ := os.OpenFile(cardPath, os.O_RDWR, 0)
	defer f.Close()
	parts, _ := card.ReadMBR(f)
	p1, err := card.ExtractPart(f, parts[0], t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer p1.Close()
	fsys, _ := card.OpenFAT(p1)
	ext, _ := card.ReadFile(fsys, "extlinux/extlinux.conf")
	pe := card.ParseExt(string(ext))
	pl := &card.Plan{FDT: pe.FDT, Overlays: pe.Overlays, AddArgs: []string{"vibe.debuglog"}, Changed: true, CopyIn: map[string][]byte{}}
	if err := card.ApplyPlan(fsys, string(ext), pl); err != nil {
		t.Fatal(err)
	}
	p1.Sync()
	b, _ := os.ReadFile(p1.Name())
	f.WriteAt(b, parts[0].Offset())
}
