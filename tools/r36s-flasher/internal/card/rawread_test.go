package card

// ★★★ A RAW CARD READS ONLY WHOLE SECTORS. macOS's /dev/rdiskN refuses any read that is not a multiple of 512 bytes
// at a multiple of 512 ("invalid argument") — the plain files every other test uses accept anything, which is how
// DiskID's 4-byte read at 440 reached Stuart's card (2026-11-14). sectorOnly behaves like the raw device.

import (
	"errors"
	"io"
	"os"
	"testing"
)

type sectorOnly struct{ r io.ReaderAt }

func (s sectorOnly) ReadAt(b []byte, off int64) (int, error) {
	if off%512 != 0 || len(b)%512 != 0 {
		return 0, errors.New("invalid argument (not a whole sector)")
	}
	return s.r.ReadAt(b, off)
}

func TestRawCardReads(t *testing.T) {
	f, err := os.CreateTemp(t.TempDir(), "card-*.img")
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	mbr := make([]byte, 512)
	mbr[440], mbr[441], mbr[442], mbr[443] = 0xac, 0xfe, 0xae, 0x30 // 30aefeac, Stuart's card
	mbr[446+4], mbr[446+8], mbr[446+12] = 0x0c, 0x80, 0x00
	mbr[446+13] = 0x04 // p1: 1024 sectors at 128
	mbr[510], mbr[511] = 0x55, 0xaa
	f.WriteAt(mbr, 0)
	f.Truncate(1 << 20)
	raw := sectorOnly{f}
	id, err := DiskID(raw)
	if err != nil || id != "30aefeac" {
		t.Fatalf("DiskID on a raw card: %q %v", id, err)
	}
	if _, err := ReadMBR(raw); err != nil {
		t.Fatalf("ReadMBR on a raw card: %v", err)
	}
}
