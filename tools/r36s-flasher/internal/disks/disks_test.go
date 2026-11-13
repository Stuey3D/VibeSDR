package disks

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The Mac's own disks never appear, and a disk image attached as a raw disk is refused (it is virtual).
func TestListNeverOffersOwnOrVirtualDisks(t *testing.T) {
	img := filepath.Join(t.TempDir(), "fake.img")
	f, _ := os.Create(img)
	f.Truncate(64 << 20)
	f.Close()
	out, err := exec.Command("/usr/bin/hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage", "-nomount", img).Output()
	if err != nil {
		t.Skip("hdiutil attach unavailable")
	}
	dev := strings.Fields(string(out))[0]
	defer exec.Command("/usr/bin/hdiutil", "detach", dev).Run()
	list, err := List()
	if err != nil {
		t.Fatal(err)
	}
	boot := bootWhole()
	for _, d := range list {
		if d.Device == dev {
			t.Fatalf("an attached disk image was offered: %+v", d)
		}
		if boot[d.ID] {
			t.Fatalf("the startup disk was offered: %+v", d)
		}
		t.Logf("offered: %+v", d)
	}
	t.Logf("startup disks refused: %v", boot)
}
