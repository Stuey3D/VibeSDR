package job

// ADVANCED (docs/v12/R36S-FLASHER.md §1.2) — for someone without the original card, with a damaged one, or who knows
// what they are doing:
//   • MAKE with the drivers from somewhere else: a saved backup or image file ("file"), the generic ROCKNIX drivers
//     already in VibeSDR ("generic"), or their own device tree files ("own") — onto ANY card, blank ones included, with
//     the card's backup optional (BackupKind "none").
//   • RESTORE: put a backup (this app's, or any card image) back on a card — the way back the backups' README promises.
// Everything about the drivers is checked BEFORE the card is backed up or touched (PrepareAdvanced, also what the
// window calls to say "Using …" when the choice is made).

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"vibesdr.net/r36s-flasher/internal/card"
	"vibesdr.net/r36s-flasher/internal/fdt"
)

// AdvancedBoot is the new card's boot partition built from the chosen drivers.
type AdvancedBoot struct {
	Boot       []byte
	ImgEnd     int64
	BootOffset int64
	Summary    string
}

func (a *AdvancedBoot) Close() {}

// PrepareAdvanced builds the new boot partition from j.Drivers. It reads only files — never a card. Errors are
// Friendly sentences (nothing has been touched yet).
func PrepareAdvanced(j *Job, tmp string) (*AdvancedBoot, error) {
	p1, imgParts, err := imageBoot(j.ImagePath, tmp)
	if err != nil {
		return nil, Friendly{"The VibeSDR image could not be read: " + err.Error() + ". Nothing has been changed."}
	}
	defer func() { p1.Close(); os.Remove(p1.Name()) }()
	newFS, err := card.OpenFAT(p1)
	if err != nil {
		return nil, fmt.Errorf("the image's boot partition: %w", err)
	}
	extB, err := card.ReadFile(newFS, "extlinux/extlinux.conf")
	if err != nil {
		return nil, errors.New("the image has no boot menu (extlinux.conf)")
	}
	ext := string(extB)
	var summary string
	switch j.Drivers {
	case "generic":
		model := "R36S"
		if b, err := card.ReadFile(newFS, card.ParseExt(ext).FDT); err == nil {
			if in, err := fdt.Parse(b); err == nil && in.Model != "" {
				model = in.Model
			}
		}
		summary = "the generic ROCKNIX drivers built into VibeSDR (" + model + ")"
	case "file":
		info, src, err := card.InspectFile(j.DriverFile, tmp)
		if err != nil {
			return nil, Friendly{filepath.Base(j.DriverFile) + " could not be read: " + err.Error() + ". Nothing has been changed."}
		}
		defer src.Close()
		if info.Kind == card.Unknown {
			return nil, Friendly{"There are no drivers VibeSDR can use in " + filepath.Base(j.DriverFile) + ": " + info.Why +
				". Try the generic ROCKNIX drivers, or your own device tree. Nothing has been changed."}
		}
		s, err := prepareBoot(p1, info, src, tmp)
		if err != nil {
			return nil, Friendly{"The drivers in " + filepath.Base(j.DriverFile) + " cannot be used: " + err.Error() + ". Nothing has been changed."}
		}
		summary = s + " from " + filepath.Base(j.DriverFile)
	case "own":
		o, err := card.ReadOwn(j.OwnDTB, j.OwnOverlays)
		if err != nil {
			return nil, Friendly{err.Error() + ". Nothing has been changed."}
		}
		pl, err := card.PlanOwn(ext, o)
		if err != nil {
			return nil, Friendly{err.Error() + ". Nothing has been changed."}
		}
		if err := card.ApplyPlan(newFS, ext, pl); err != nil {
			return nil, err
		}
		summary = "your own device tree: " + pl.Summary
	default:
		return nil, fmt.Errorf("unknown drivers %q", j.Drivers)
	}
	if err := p1.Sync(); err != nil {
		return nil, err
	}
	boot, err := os.ReadFile(p1.Name())
	if err != nil {
		return nil, err
	}
	return &AdvancedBoot{Boot: boot, ImgEnd: card.ImageEnd(imgParts), BootOffset: imgParts[0].Offset(), Summary: summary}, nil
}

// RestoreInfo is what checking a backup before putting it back found.
type RestoreInfo struct {
	Bytes   int64  `json:"bytes"`
	SHA256  string `json:"sha256"`
	Checked bool   `json:"checked"` // matched the .sha256 beside it
	Whole   bool   `json:"whole"`   // a whole card (not only its start)
}

// CheckRestore reads a backup or image all the way through: its size, its SHA-256, and that it matches the checksum
// beside it when there is one. prog gets the bytes read so far.
func CheckRestore(p string, prog func(int64)) (*RestoreInfo, error) {
	r, _, err := card.OpenStream(p)
	if err != nil {
		return nil, Friendly{filepath.Base(p) + " could not be opened: " + err.Error() + "."}
	}
	defer r.Close()
	h := sha256.New()
	buf := make([]byte, chunk)
	var n int64
	for {
		k, err := r.Read(buf)
		if k > 0 {
			if n == 0 && k >= 512 && (buf[510] != 0x55 || buf[511] != 0xaa) {
				return nil, Friendly{filepath.Base(p) + " is not an image of a card (it has no partition table)."}
			}
			h.Write(buf[:k])
			n += int64(k)
			if prog != nil {
				prog(n)
			}
		}
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return nil, Friendly{filepath.Base(p) + " is damaged — it could not be read to the end (" + err.Error() + ")."}
		}
	}
	if n < 1<<20 {
		return nil, Friendly{filepath.Base(p) + " is too small to be an image of a card."}
	}
	ri := &RestoreInfo{Bytes: n, SHA256: hex.EncodeToString(h.Sum(nil))}
	if want := card.SHA256Beside(p); want != "" {
		if want != ri.SHA256 {
			return nil, Friendly{filepath.Base(p) + " does not match the checksum saved beside it — it has been damaged since it was made, so it is not used."}
		}
		ri.Checked = true
	}
	// ★ a boot-only backup ends with the boot partition; a whole card goes on past its last partition's start
	ri.Whole = true
	if hb, err := firstSector(p); err == nil {
		if parts, err := card.ReadMBR(strings.NewReader(string(hb))); err == nil {
			last := int64(0)
			for _, pt := range parts {
				if pt.Offset() > last {
					last = pt.Offset()
				}
			}
			ri.Whole = n > last
		}
	}
	return ri, nil
}

func firstSector(p string) ([]byte, error) {
	r, _, err := card.OpenStream(p)
	if err != nil {
		return nil, err
	}
	defer r.Close()
	b := make([]byte, 512)
	_, err = io.ReadFull(r, b)
	return b, err
}

// runRestore puts j.RestorePath back on the card: checked all the way through first, the card backed up unless the
// person chose not to, then written, read back and ejected.
func runRestore(j *Job, rep Reporter, tmp string, stage func(string, float64) func(float64, string)) error {
	s := stage("restorecheck", 0.12)
	s(0, "Checking the backup…")
	ri, err := CheckRestore(j.RestorePath, func(d int64) { s(0.5, fmt.Sprintf("Checking the backup… %.1f GB", float64(d)/1e9)) })
	if err != nil {
		return err
	}
	if ri.Bytes > j.Size {
		return Friendly{fmt.Sprintf("This backup is of a bigger card (%.1f GB) than this one (%.1f GB). Use a card at least as big. Nothing on the card has changed.", float64(ri.Bytes)/1e9, float64(j.Size)/1e9)}
	}
	if j.Disk != "" {
		if out, err := exec.Command("/usr/sbin/diskutil", "unmountDisk", j.Disk).CombinedOutput(); err != nil {
			return Friendly{"The card is in use and could not be unmounted (" + strings.TrimSpace(string(out)) + "). Close anything using it and try again."}
		}
	}
	dev, err := openTarget(j)
	if err != nil {
		if j.Disk != "" {
			_ = exec.Command("/usr/sbin/diskutil", "mountDisk", j.Disk).Run()
		}
		return openFailure(err)
	}
	defer dev.Close()
	if j.BackupKind != "none" {
		if err := backupAndCheck(j, dev, stage, 0.22, 0.08); err != nil {
			return err
		}
	} else {
		stage("backup", 0.30)
	}
	// ★★★ A RAW CARD TAKES WHOLE SECTORS ONLY (card/rawread_test.go): an image that is not a whole number of sectors
	//     (made by hand) is written with its last sector padded with zeros, and checked over the same span.
	end := (ri.Bytes + 511) / 512 * 512
	s = stage("write", 0.30)
	r, _, err := card.OpenStream(j.RestorePath)
	if err != nil {
		return Friendly{"The backup could not be opened again: " + err.Error() + ". Nothing on the card has changed."}
	}
	defer r.Close()
	h := sha256.New()
	buf := make([]byte, chunk)
	for off := int64(0); off < end; {
		n := int64(len(buf))
		if end-off < n {
			n = end - off
		}
		real := n
		if off+n > ri.Bytes {
			real = ri.Bytes - off
		}
		if _, err := io.ReadFull(r, buf[:real]); err != nil {
			return Friendly{"The backup could not be read while writing (" + err.Error() + "). The card is now incomplete — start again." + keptNote(j)}
		}
		clear(buf[real:n])
		h.Write(buf[:n])
		if testCorrupt != nil {
			testCorrupt(off, buf[:n])
		}
		if sectorOnlyIO && (off%512 != 0 || n%512 != 0) {
			return Friendly{fmt.Sprintf("not a whole sector: %d bytes at %d", n, off)}
		}
		if _, err := dev.WriteAt(buf[:n], off); err != nil {
			return Friendly{fmt.Sprintf("Writing the card failed at %d MB (%v).", off>>20, err) + keptNote(j)}
		}
		off += n
		s(float64(off)/float64(end), fmt.Sprintf("Putting the backup back… %d%%", off*100/end))
	}
	wrote := hex.EncodeToString(h.Sum(nil))
	if err := syncTarget(dev); err != nil {
		return Friendly{"The card did not finish writing (" + err.Error() + ")." + keptNote(j)}
	}
	s = stage("verify", 0.28)
	got, err := hashSpan(dev, 0, end, func(d int64) { s(float64(d)/float64(end), fmt.Sprintf("Checking the card… %d%%", d*100/end)) })
	if err != nil || got != wrote || (end == ri.Bytes && got != ri.SHA256) {
		return Friendly{"The card did not read back correctly — it may be faulty. Try another card." + keptNote(j)}
	}
	dev.Close()
	if j.Disk != "" {
		_ = exec.Command("/usr/sbin/diskutil", "eject", j.Disk).Run()
	}
	text := "Done — the backup is back on the card. Put it in your R36S and switch it on."
	if !ri.Whole {
		text = "Done — the start of the card (its loader and boot partition) is back as it was in the backup."
	}
	rep(Event{Stage: "done", Frac: 1, Done: true, Text: text})
	return nil
}

// sectorOnlyIO — tests only: every write to the card must be whole sectors, as on a raw card.
var sectorOnlyIO bool

func keptNote(j *Job) string {
	if j.BackupKind == "none" {
		return ""
	}
	return " This card's own backup is safe: " + filepath.Base(j.BackupPath)
}
