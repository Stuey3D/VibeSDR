package job

// UPDATE, end to end against image FILES as the card. The card is made from the real image (as Make writes it), then
// given what a real R36S adds at first boot — a p4 VIBESDR drive in the partition table — and the owner's data in p3
// and p4, the debug log in the boot menu, and an OLD disk id (an earlier image's). Update must rewrite p1 + p2 only.

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"vibesdr.net/r36s-flasher/internal/card"

	"github.com/diskfs/go-diskfs/backend/file"
	"github.com/diskfs/go-diskfs/filesystem/fat32"
)

const oldDiskID = 0xdeadbeef

// firstBootCard: the image as Make writes it, then the R36S's first boot (p4 after p3), the owner's data, the debug log.
func firstBootCard(t *testing.T, size int64) (string, []card.Part) {
	img, _ := testImage(t)
	cardPath := filepath.Join(t.TempDir(), "card.img")
	gunzipTo(t, img, cardPath, size)
	f, err := os.OpenFile(cardPath, os.O_RDWR, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	parts, _ := card.ReadMBR(f)
	mbr := make([]byte, 512)
	f.ReadAt(mbr, 0)
	p4start := uint32(parts[2].End() / 512)
	e := mbr[446+16*3:]
	e[4] = 0x0c
	binary.LittleEndian.PutUint32(e[8:], p4start)
	binary.LittleEndian.PutUint32(e[12:], uint32(size/512)-p4start)
	binary.LittleEndian.PutUint32(mbr[440:], oldDiskID) // an earlier image's disk id
	f.WriteAt(mbr, 0)
	// the owner's settings and recordings
	f.WriteAt([]byte(strings.Repeat("SETTINGS", 512)), parts[2].Offset()+4096)
	f.WriteAt([]byte(strings.Repeat("RECORDING", 512)), int64(p4start)*512+8192)
	// the debug log, as on Stuart's card
	fsys, err := fat32.Read(file.New(f, false), parts[0].Size(), parts[0].Offset(), 0)
	if err != nil {
		t.Fatal(err)
	}
	ext, _ := card.ReadFile(fsys, "extlinux/extlinux.conf")
	pe := card.ParseExt(string(ext))
	w, _ := fsys.OpenFile("/extlinux/extlinux.conf", os.O_RDWR|os.O_TRUNC)
	w.Write([]byte(card.RewriteExt(string(ext), &card.Plan{FDT: pe.FDT, Overlays: pe.Overlays, AddArgs: []string{"vibe.debuglog"}})))
	w.Close()
	parts, _ = card.ReadMBR(f)
	return cardPath, parts
}

func span(t *testing.T, p string, off, n int64) string {
	f, _ := os.Open(p)
	defer f.Close()
	got, err := hashSpan(f, off, off+n, func(int64) {})
	if err != nil {
		t.Fatal(err)
	}
	return got
}

func runUpdateJob(t *testing.T, cardPath string, size int64) (*Job, []Event, error) {
	img, sum := testImage(t)
	dir := t.TempDir()
	j := &Job{Mode: "update", Target: cardPath, Size: size, BackupPath: filepath.Join(dir, "backup", "boot.img.gz"),
		ImagePath: img, ImageSHA256: sum, ImageVersion: "new-test", Tmp: dir}
	var evs []Event
	err := Run(j, func(e Event) { evs = append(evs, e) })
	return j, evs, err
}

// ★★★ The whole point: system rewritten, settings and recordings and the loader untouched, the card still boots.
func TestUpdateKeepsSettingsAndRecordings(t *testing.T) {
	const size = int64(5) << 30
	cardPath, parts := firstBootCard(t, size)
	if len(parts) != 4 {
		t.Fatalf("first-boot card should have 4 partitions, has %d", len(parts))
	}
	// what must survive
	loader := span(t, cardPath, 512, parts[0].Offset()-512)
	p3 := span(t, cardPath, parts[2].Offset(), parts[2].Size())
	p4 := span(t, cardPath, parts[3].Offset(), parts[3].Size())
	bootBefore := span(t, cardPath, 0, parts[0].End())
	// damage the system, so the update visibly puts it back
	{
		f, _ := os.OpenFile(cardPath, os.O_RDWR, 0)
		f.WriteAt([]byte(strings.Repeat("OLDSYSTEM", 4096)), parts[1].Offset()+1<<20)
		f.Close()
	}
	j, evs, err := runUpdateJob(t, cardPath, size)
	if err != nil {
		t.Fatalf("update: %v", err)
	}
	last := evs[len(evs)-1]
	if !last.Done || !strings.Contains(last.Text, "Updated from an earlier VibeSDR to new-test") {
		t.Fatalf("done text: %+v", last)
	}
	if span(t, cardPath, 512, parts[0].Offset()-512) != loader {
		t.Fatal("the loader changed")
	}
	if span(t, cardPath, parts[2].Offset(), parts[2].Size()) != p3 {
		t.Fatal("★ the settings partition changed")
	}
	if span(t, cardPath, parts[3].Offset(), parts[3].Size()) != p4 {
		t.Fatal("★ the VIBESDR drive (recordings) changed")
	}
	f, _ := os.Open(cardPath)
	after, _ := card.ReadMBR(f)
	id, _ := card.DiskID(f)
	f.Close()
	if len(after) != 4 || after[3] != parts[3] || id != "deadbeef" {
		t.Fatalf("the partition table changed: %+v id %s", after, id)
	}
	// the system is the image's again
	img, _ := testImage(t)
	imgCard := filepath.Join(t.TempDir(), "image.img")
	gunzipTo(t, img, imgCard, parts[2].Offset())
	if span(t, cardPath, parts[1].Offset(), parts[1].Size()) != span(t, imgCard, parts[1].Offset(), parts[1].Size()) {
		t.Fatal("★ p2 is not the image's system")
	}
	ext := cardBoot(t, cardPath)
	if !strings.Contains(ext, "root=PARTUUID=deadbeef-02") {
		t.Fatalf("★★ the boot menu does not point at THIS card's system partition — it would not boot:\n%s", ext)
	}
	if !strings.Contains(ext, "vibe.debuglog") || !strings.Contains(ext, "FDT /rk3326-gameconsole-eeclone.dtb") {
		t.Fatalf("drivers / debug log not carried:\n%s", ext)
	}
	// the backup is the boot area as it was
	if got, n, err := gunzipHash(j.BackupPath, func(int64) {}); err != nil || got != bootBefore || n != parts[0].End() {
		t.Fatalf("boot backup does not round-trip: %v %d", err, n)
	}
	for _, e := range evs {
		if e.Frac < 0 || e.Frac > 1.0001 {
			t.Fatalf("progress out of range: %+v", e)
		}
	}
}

// A card from before the fixed system partition: refused before ANY write, with the way forward.
func TestUpdateRefusesOldLayout(t *testing.T) {
	const size = int64(5) << 30
	cardPath, _ := firstBootCard(t, size)
	{
		f, _ := os.OpenFile(cardPath, os.O_RDWR, 0)
		mbr := make([]byte, 512)
		f.ReadAt(mbr, 0)
		e := mbr[446+16*1:]
		binary.LittleEndian.PutUint32(e[12:], binary.LittleEndian.Uint32(e[12:])-2048) // p2 1 MiB smaller
		f.WriteAt(mbr, 0)
		f.Close()
	}
	before := fileHash(t, cardPath, size)
	_, _, err := runUpdateJob(t, cardPath, size)
	if err == nil || !strings.Contains(err.Error(), "before VibeSDR's system partition had a fixed size") ||
		!strings.Contains(err.Error(), "Make a VibeSDR card once") {
		t.Fatalf("want the fixed-size refusal, got %v", err)
	}
	if fileHash(t, cardPath, size) != before {
		t.Fatal("★ a refused update changed the card")
	}
}

// A stock (or any non-VibeSDR) card is not updated — Make is the way.
func TestUpdateRefusesStockCard(t *testing.T) {
	dir := t.TempDir()
	cardPath := filepath.Join(dir, "stock.img")
	const size = int64(4) << 30
	stockCard(t, cardPath, size, []byte("not a device tree"))
	before := fileHash(t, cardPath, size)
	_, _, err := runUpdateJob(t, cardPath, size)
	if err == nil || !strings.Contains(err.Error(), "not a VibeSDR card") {
		t.Fatalf("want the not-VibeSDR refusal, got %v", err)
	}
	if fileHash(t, cardPath, size) != before {
		t.Fatal("★ a refused update changed the card")
	}
}
