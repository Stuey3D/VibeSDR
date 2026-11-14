package card

import (
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"regexp"
	"strings"

	"github.com/diskfs/go-diskfs/filesystem"
)

// VersionFile is where an image (from beta76) records its version on the boot partition, so Update can say
// "beta75 → beta76". An older card has none: it is then "an earlier VibeSDR".
const VersionFile = "vibesdr-version.txt"

// ReadVersion is the image version a VibeSDR boot partition records, or "" when it records none.
func ReadVersion(fsys filesystem.FileSystem) string {
	b, err := ReadFile(fsys, VersionFile)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}

// DiskID is the card's MBR disk signature as the kernel spells it in PARTUUID ("4bd2a6f1" → PARTUUID=4bd2a6f1-02).
func DiskID(r io.ReaderAt) (string, error) {
	b := make([]byte, 4)
	if _, err := r.ReadAt(b, 440); err != nil {
		return "", fmt.Errorf("could not read the card's disk id: %w", err)
	}
	return fmt.Sprintf("%08x", binary.LittleEndian.Uint32(b)), nil
}

var rootPartUUID = regexp.MustCompile(`root=PARTUUID=[0-9A-Fa-f]{8}-02\b`)

// SetRootDiskID points the boot menu's root= at THIS card's disk id.
//
// ★★★ WHY. Every image has its own disk id (a hash of its version) and its boot menu finds the system partition by it
// (root=PARTUUID=<id>-02). Update keeps the card's partition table — p4, the VIBESDR drive, was added to it at first boot
// and is not in the image's — so the card keeps its OLD id, and a new boot menu left as built would look for a disk that
// is not there: the R36S would stop at boot. The settings partition is found by its label, so nothing else needs it.
func SetRootDiskID(fsys filesystem.FileSystem, id string) error {
	b, err := ReadFile(fsys, "extlinux/extlinux.conf")
	if err != nil {
		return fmt.Errorf("the new boot partition has no boot menu: %w", err)
	}
	s := string(b)
	if len(rootPartUUID.FindAllString(s, -1)) == 0 {
		return fmt.Errorf("the new boot menu does not name its system partition (root=PARTUUID=…-02)")
	}
	s = rootPartUUID.ReplaceAllString(s, "root=PARTUUID="+id+"-02")
	return writeFile(fsys, "extlinux/extlinux.conf", []byte(s))
}

// SameLayout: Update rewrites p1 and p2 in place, so both must be exactly where the image has them, and the settings
// partition must start where the image's does. Returns why not, in plain words, or "".
func SameLayout(cardParts, imgParts []Part) string {
	if len(imgParts) < 3 || len(cardParts) < 3 {
		return "its partitions are not the ones VibeSDR makes"
	}
	for i := 0; i < 2; i++ {
		if cardParts[i].Start != imgParts[i].Start || cardParts[i].Sects != imgParts[i].Sects {
			if i == 1 {
				return "it was made before VibeSDR's system partition had a fixed size, so the new system cannot be put in its place"
			}
			return "its boot partition is not where this VibeSDR puts it"
		}
	}
	if cardParts[2].Start != imgParts[2].Start {
		return "its settings partition is not where this VibeSDR expects it"
	}
	return ""
}

// ReadVersionFromCard: the version a VibeSDR card's boot partition records, or "" (an older card, or unreadable).
func ReadVersionFromCard(r io.ReaderAt, info *Info, tmp string) string {
	if info == nil || len(info.Parts) == 0 {
		return ""
	}
	f, err := ExtractPart(r, info.Parts[0], tmp)
	if err != nil {
		return ""
	}
	defer func() { f.Close(); os.Remove(f.Name()) }()
	fsys, err := OpenFAT(f)
	if err != nil {
		return ""
	}
	return ReadVersion(fsys)
}
