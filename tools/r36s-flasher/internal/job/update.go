package job

// UPDATE A VIBESDR CARD (docs/v12/R36S-FLASHER.md §1.3): rewrite ONLY the system — the boot partition (keeping this
// R36S's drivers already on it) and the read-only system partition. The settings partition (p3, vibestate) and the
// VIBESDR drive (p4: recordings, gallery, EiBi, exports, logs) are never touched, nor is the loader or the partition
// table. The boot partition is backed up first, and read back, so a failed update can be put back.

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"vibesdr.net/r36s-flasher/internal/card"
)

// Shares of the bar after the image check (pImgCheck).
const (
	uBackup  = 0.06
	uBackChk = 0.03
	uPrepare = 0.04
	uWrite   = 0.53
	uVerify  = 0.30
)

func runUpdate(j *Job, rep Reporter, tmp string, stage func(string, float64) func(float64, string)) error {
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
	remount := func() {
		if j.Disk != "" {
			dev.Close()
			_ = exec.Command("/usr/sbin/diskutil", "mountDisk", j.Disk).Run()
		}
	}

	// 1. is this a VibeSDR card this image can update in place? — decided before anything is written or backed up
	s := stage("prepare", uPrepare)
	s(0, "Reading your VibeSDR card…")
	info, err := card.Inspect(dev, tmp)
	if err != nil {
		remount()
		return Friendly{"Could not read the card: " + err.Error() + ". Nothing on the card has changed."}
	}
	if info.Kind != card.VibeSDR {
		remount()
		why := "it is not a VibeSDR card"
		if info.Why != "" {
			why += " (" + info.Why + ")"
		}
		return Friendly{"This card cannot be updated: " + why + ". To put VibeSDR on it, use Make a VibeSDR card. Nothing on the card has changed."}
	}
	p1, imgParts, err := imageBoot(j.ImagePath, tmp)
	if err != nil {
		remount()
		return Friendly{"The VibeSDR image could not be read: " + err.Error() + ". Nothing on the card has changed."}
	}
	defer p1.Close()
	if why := card.SameLayout(info.Parts, imgParts); why != "" {
		remount()
		return Friendly{"This card cannot be updated in place: " + why + ". Use Make a VibeSDR card once (it backs the whole card up first, recordings and all); every update after that keeps your settings and recordings. Nothing on the card has changed."}
	}
	diskID, err := card.DiskID(dev)
	if err != nil {
		remount()
		return Friendly{err.Error() + ". Nothing on the card has changed."}
	}
	from := card.ReadVersionFromCard(dev, info, tmp)
	summary, err := prepareBoot(p1, info, dev, tmp)
	if err != nil {
		remount()
		return Friendly{"Stopped before writing: " + err.Error() + ". Nothing on the card has changed."}
	}
	if err := setRootDiskID(p1, diskID); err != nil {
		remount()
		return Friendly{"Stopped before writing: " + err.Error() + ". Nothing on the card has changed."}
	}
	s(1, "Using "+summary)
	newBoot, err := os.ReadFile(p1.Name())
	if err != nil {
		return err
	}

	// 2. back up the boot partition (and the loader before it), then check it reads back
	span := imgParts[0].End()
	s = stage("backup", uBackup)
	rawSum, err := backup(dev, span, j.BackupPath, func(d int64) { s(float64(d)/float64(span), fmt.Sprintf("Backing up the boot partition… %d%%", d*100/span)) })
	if err != nil {
		return Friendly{"The backup could not be made (" + err.Error() + "). Nothing on the card has changed."}
	}
	s = stage("backupcheck", uBackChk)
	s(0, "Checking the backup…")
	if got, n, err := gunzipHash(j.BackupPath, func(d int64) { s(float64(d)/float64(span), "Checking the backup…") }); err != nil || got != rawSum || n != span {
		return Friendly{"The backup did not read back correctly, so the card was not touched. Your card is exactly as it was."}
	}
	j.BackupKind = "boot"
	writeBackupNotes(j, span, rawSum)

	// 3. write the boot and system partitions only
	start, end := imgParts[0].Offset(), imgParts[1].End()
	total := end - start
	s = stage("write", uWrite)
	wroteSum, err := writeImage(dev, j.ImagePath, start, end, imgParts[0].Offset(), newBoot, func(d int64) {
		s(float64(d)/float64(total), fmt.Sprintf("Updating VibeSDR… %d%%", d*100/total))
	})
	if err != nil {
		return Friendly{"Updating the card failed (" + err.Error() + "). Your settings and recordings were not touched; the boot partition's backup is safe: " + filepath.Base(j.BackupPath)}
	}
	if err := syncTarget(dev); err != nil {
		return Friendly{"The card did not finish writing (" + err.Error() + "). Your settings and recordings were not touched."}
	}

	// 4. read back what was written
	s = stage("verify", uVerify)
	got, err := hashSpan(dev, start, end, func(d int64) { s(float64(d)/float64(total), fmt.Sprintf("Checking the card… %d%%", d*100/total)) })
	if err != nil || got != wroteSum {
		return Friendly{"The card did not read back correctly — it may be faulty. Your settings and recordings were not touched; the boot partition's backup is safe."}
	}
	dev.Close()
	if j.Disk != "" {
		_ = exec.Command("/usr/sbin/diskutil", "eject", j.Disk).Run()
	}
	what := "an earlier VibeSDR"
	if from != "" {
		what = from
	}
	rep(Event{Stage: "done", Frac: 1, Done: true, Text: fmt.Sprintf("Updated from %s to %s — your settings and recordings are as they were. Put the card in your R36S and switch it on.", what, j.ImageVersion)})
	return nil
}

func setRootDiskID(p1 *os.File, id string) error {
	fsys, err := card.OpenFAT(p1)
	if err != nil {
		return fmt.Errorf("the new boot partition: %w", err)
	}
	if err := card.SetRootDiskID(fsys, id); err != nil {
		return err
	}
	return p1.Sync()
}
