package job

// MAKE ON A NEW CARD (Stuart, 2026-11-14): the card with the person's current R36S system is only READ — backed up and
// its drivers read — and VibeSDR is written to a DIFFERENT card they insert afterwards. Two steps, because the person
// swaps cards in between:
//   Prepare   — the original card, opened READ-ONLY: image check, backup + read-back check, drivers → the new card's
//               boot partition, ready in a file; then the original is ejected.
//   WriteNew  — the new card: refused if it is the original put back, or too small; written, read back, ejected.
// The new card is not backed up: the person chose it for VibeSDR, and the window warns, naming what is on it, when it
// is not empty (Stuart's choice: "Warn, then write").

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"vibesdr.net/r36s-flasher/internal/card"

	"golang.org/x/sys/unix"
)

// Prepared is what reading the original card produced. The boot partition file lives in the Job's Tmp folder until
// WriteNew (or Discard) is done with it.
type Prepared struct {
	BootFile   string // the new card's boot partition, this R36S's drivers in it
	ImgEnd     int64  // bytes of the image to write
	BootOffset int64  // where the boot partition goes
	Summary    string // which drivers, in words
	SourceSize int64  // the original card's size and …
	SourceHead string // … the SHA-256 of its first MiB — so it cannot be written to by mistake if it is put back
}

// Discard removes what Prepare left behind.
func (p *Prepared) Discard() {
	if p != nil && p.BootFile != "" {
		os.RemoveAll(filepath.Dir(p.BootFile))
	}
}

const headBytes = 1 << 20

func headHash(r io.ReaderAt) (string, error) {
	b := make([]byte, headBytes)
	if _, err := r.ReadAt(b, 0); err != nil && err != io.EOF {
		return "", err
	}
	s := sha256.Sum256(b)
	return hex.EncodeToString(s[:]), nil
}

// Prepare reads the original card (never writes to it): checks the image, backs the card up, checks the backup,
// reads its drivers and builds the new card's boot partition. Then it ejects the original.
func Prepare(j *Job, rep Reporter) (p *Prepared, err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("unexpected problem: %v", r)
		}
	}()
	if j.Tmp == "" {
		j.Tmp = os.TempDir()
	}
	work, err := os.MkdirTemp(j.Tmp, "vibesdr-newcard-")
	if err != nil {
		return nil, err
	}
	keep := false
	defer func() {
		if !keep {
			os.RemoveAll(work)
		}
	}()
	at := 0.0
	stage := func(name string, share float64) func(float64, string) {
		base := at
		at += share
		return func(frac float64, text string) {
			if frac > 1 {
				frac = 1
			}
			rep(Event{Stage: name, Frac: base + share*frac, Text: text})
		}
	}

	s := stage("image", 0.08)
	s(0, "Checking the VibeSDR image…")
	if got, err := hashFile(j.ImagePath, func(d, t int64) { s(float64(d)/float64(t), "Checking the VibeSDR image…") }); err != nil {
		return nil, fmt.Errorf("could not read the VibeSDR image: %w", err)
	} else if got != strings.ToLower(j.ImageSHA256) {
		return nil, Friendly{"The VibeSDR image does not match its checksum, so it was not used. Nothing on your card has changed."}
	}

	if j.Disk != "" {
		if out, err := exec.Command("/usr/sbin/diskutil", "unmountDisk", j.Disk).CombinedOutput(); err != nil {
			return nil, Friendly{"The card is in use and could not be unmounted (" + strings.TrimSpace(string(out)) + "). Close anything using it and try again."}
		}
	}
	dev, err := openRead(j)
	if err != nil {
		if j.Disk != "" {
			_ = exec.Command("/usr/sbin/diskutil", "mountDisk", j.Disk).Run()
		}
		return nil, openFailure(err)
	}
	defer dev.Close()
	head, err := headHash(dev)
	if err != nil {
		return nil, Friendly{"Could not read the card: " + err.Error() + ". Nothing on it has changed."}
	}

	span := j.Size
	if j.BackupKind == "boot" {
		span = bootSpan(dev, j.Size)
	}
	s = stage("backup", 0.55)
	rawSum, err := backup(dev, span, j.BackupPath, func(d int64) { s(float64(d)/float64(span), backupText(j.BackupKind, d, span)) })
	if err != nil {
		return nil, Friendly{"The backup could not be made (" + err.Error() + "). Nothing on your card has changed."}
	}
	s = stage("backupcheck", 0.2)
	s(0, "Checking the backup…")
	if got, n, err := gunzipHash(j.BackupPath, func(d int64) { s(float64(d)/float64(span), "Checking the backup…") }); err != nil || got != rawSum || n != span {
		return nil, Friendly{"The backup did not read back correctly. Your card has not changed."}
	}
	writeBackupNotes(j, span, rawSum)

	s = stage("prepare", 0.17)
	s(0, "Reading this R36S's drivers…")
	info, err := card.Inspect(dev, work)
	if err != nil {
		return nil, Friendly{"Could not read the card: " + err.Error() + ". Your backup is safe and the card has not changed."}
	}
	if info.Kind == card.Unknown {
		return nil, Friendly{"Stopped: " + info.Why + ". Your backup is safe and the card has not changed."}
	}
	p1, imgParts, err := imageBoot(j.ImagePath, work)
	if err != nil {
		return nil, Friendly{"The VibeSDR image could not be read: " + err.Error() + ". Your card has not changed."}
	}
	defer p1.Close()
	summary, err := prepareBoot(p1, info, dev, work)
	if err != nil {
		return nil, Friendly{"Stopped: " + err.Error() + ". Your backup is safe and the card has not changed."}
	}
	s(1, "Using "+summary)
	dev.Close()
	if j.Disk != "" {
		_ = exec.Command("/usr/sbin/diskutil", "eject", j.Disk).Run()
	}
	keep = true
	return &Prepared{BootFile: p1.Name(), ImgEnd: card.ImageEnd(imgParts), BootOffset: imgParts[0].Offset(),
		Summary: summary, SourceSize: j.Size, SourceHead: head}, nil
}

// WriteNew writes VibeSDR to the new card (j.Target/j.Disk/j.Size), with the boot partition Prepare built.
func WriteNew(j *Job, p *Prepared, rep Reporter) (err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("unexpected problem: %v", r)
		}
	}()
	if p == nil {
		return Friendly{"The original card has not been read yet — start again."}
	}
	if p.ImgEnd > j.Size {
		return Friendly{fmt.Sprintf("This card is too small: VibeSDR needs %.1f GB and the card holds %.1f GB. Nothing on it has changed.", float64(p.ImgEnd)/1e9, float64(j.Size)/1e9)}
	}
	newBoot, err := os.ReadFile(p.BootFile)
	if err != nil {
		return Friendly{"The prepared boot partition is missing — start again. Nothing on this card has changed."}
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
	// ★★ THE ORIGINAL PUT BACK IS REFUSED — same size and the same first MiB (its partition table and loader).
	if head, err := headHash(dev); err == nil && j.Size == p.SourceSize && head == p.SourceHead {
		dev.Close()
		if j.Disk != "" {
			_ = exec.Command("/usr/sbin/diskutil", "mountDisk", j.Disk).Run()
		}
		return Friendly{"This is the card you started with — it is kept as it is. Take it out and insert the card you want VibeSDR written to."}
	}

	at := 0.0
	stage := func(name string, share float64) func(float64, string) {
		base := at
		at += share
		return func(frac float64, text string) {
			if frac > 1 {
				frac = 1
			}
			rep(Event{Stage: name, Frac: base + share*frac, Text: text})
		}
	}
	end := p.ImgEnd
	s := stage("write", 0.6)
	wroteSum, err := writeImage(dev, j.ImagePath, 0, end, p.BootOffset, newBoot, func(d int64) {
		s(float64(d)/float64(end), fmt.Sprintf("Writing VibeSDR… %d%%", d*100/end))
	})
	if err != nil {
		return Friendly{"Writing the card failed (" + err.Error() + "). Your original card was not touched."}
	}
	if err := syncTarget(dev); err != nil {
		return Friendly{"The card did not finish writing (" + err.Error() + "). Your original card was not touched."}
	}
	s = stage("verify", 0.4)
	got, err := hashSpan(dev, 0, end, func(d int64) { s(float64(d)/float64(end), fmt.Sprintf("Checking the card… %d%%", d*100/end)) })
	if err != nil || got != wroteSum {
		return Friendly{"The new card did not read back correctly — it may be faulty. Try another card. Your original card was not touched."}
	}
	dev.Close()
	if j.Disk != "" {
		_ = exec.Command("/usr/sbin/diskutil", "eject", j.Disk).Run()
	}
	p.Discard()
	rep(Event{Stage: "done", Frac: 1, Done: true, Text: "Done — put the new card in your R36S and switch it on. Your original card is as it was."})
	return nil
}

// openRead opens the original card READ-ONLY (exclusively, so nothing mounts it while it is read).
func openRead(j *Job) (*os.File, error) {
	flag := os.O_RDONLY
	if strings.HasPrefix(j.Target, "/dev/") {
		flag |= unix.O_EXLOCK
	}
	if j.Open != nil {
		return j.Open(j.Target, flag)
	}
	return os.OpenFile(j.Target, flag, 0)
}
