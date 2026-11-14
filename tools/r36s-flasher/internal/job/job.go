// Package job is Simple mode's whole run against one target — a card's raw device, or (tests) an image file:
// back up → check the backup → read the card → the image's boot partition with this R36S's drivers → write → check.
// It runs as root in the helper (writing a card needs it); nothing here talks to the window.
package job

import (
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"vibesdr.net/r36s-flasher/internal/card"

	"github.com/klauspost/pgzip"
)

// Job is everything the helper needs, written by the window as JSON.
type Job struct {
	Target       string `json:"target"`       // /dev/rdiskN, or an image file (tests)
	Disk         string `json:"disk"`         // /dev/diskN for diskutil; "" for a file
	Size         int64  `json:"size"`         // the card's size in bytes
	BackupKind   string `json:"backupKind"`   // "whole" | "boot"
	BackupPath   string `json:"backupPath"`   // …/<name>.img.gz
	ImagePath    string `json:"imagePath"`    // the VibeSDR .img.gz
	ImageSHA256  string `json:"imageSha256"`  // its published checksum
	ImageVersion string `json:"imageVersion"` //
	ProgressPath string `json:"progressPath"` // JSON lines for the window
	UID          int    `json:"uid"`          // the person's — the backup files are theirs, not root's
	GID          int    `json:"gid"`
	Tmp          string `json:"tmp"`

	// Open opens the card (the app: through authopen, one system password prompt). nil = open it directly (tests).
	Open func(path string, flags int) (*os.File, error) `json:"-"`
}

// Event is one progress line.
type Event struct {
	Stage string  `json:"stage"`
	Frac  float64 `json:"frac"` // 0..1 across the whole run
	Text  string  `json:"text"`
	Done  bool    `json:"done,omitempty"`
	Error string  `json:"error,omitempty"`
}

// Reporter receives progress.
type Reporter func(Event)

// FileReporter appends events to a file the window reads — at most one line per 0.25 % of the bar unless the words
// change, and always the last one.
func FileReporter(p string) Reporter {
	lastFrac, lastText := -1.0, ""
	return func(e Event) {
		if !e.Done && e.Error == "" && e.Text == lastText && e.Frac-lastFrac < 0.0025 {
			return
		}
		lastFrac, lastText = e.Frac, e.Text
		f, err := os.OpenFile(p, os.O_WRONLY|os.O_APPEND|os.O_CREATE, 0o644)
		if err != nil {
			return
		}
		b, _ := json.Marshal(e)
		f.Write(append(b, '\n'))
		f.Close()
	}
}

// The share of the bar each stage takes.
const (
	pImgCheck = 0.04
	pBackup   = 0.30
	pBackChk  = 0.10
	pPrepare  = 0.04
	pWrite    = 0.36
	pVerify   = 0.16
)

const chunk = 4 << 20

// testCorrupt — tests only: changes bytes after they are hashed and before they are written.
var testCorrupt func(off int64, b []byte)

// Friendly is an error meant for the person, already in plain words.
type Friendly struct{ Msg string }

func (f Friendly) Error() string { return f.Msg }

// Run does the whole job.
func Run(j *Job, rep Reporter) (err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("unexpected problem: %v", r)
		}
	}()
	if j.Tmp == "" {
		j.Tmp = os.TempDir()
	}
	tmp, err := os.MkdirTemp(j.Tmp, "vibesdr-flasher-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)

	at := 0.0
	stage := func(name string, share float64) func(frac float64, text string) {
		base := at
		at += share
		return func(frac float64, text string) {
			if frac > 1 {
				frac = 1
			}
			rep(Event{Stage: name, Frac: base + share*frac, Text: text})
		}
	}

	// 1. the image is what was published
	s := stage("image", pImgCheck)
	s(0, "Checking the VibeSDR image…")
	if got, err := hashFile(j.ImagePath, func(d, t int64) { s(float64(d)/float64(t), "Checking the VibeSDR image…") }); err != nil {
		return fmt.Errorf("could not read the VibeSDR image: %w", err)
	} else if got != strings.ToLower(j.ImageSHA256) {
		return Friendly{"The VibeSDR image does not match its checksum, so it was not used. Nothing on the card has changed."}
	}

	// 2. the card
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

	// 3. back up, then check the backup reads back to the same bytes
	span := j.Size
	if j.BackupKind == "boot" {
		span = bootSpan(dev, j.Size)
	}
	s = stage("backup", pBackup)
	rawSum, err := backup(dev, span, j.BackupPath, func(d int64) { s(float64(d)/float64(span), backupText(j.BackupKind, d, span)) })
	if err != nil {
		return Friendly{"The backup could not be made (" + err.Error() + "). Nothing on the card has changed."}
	}
	s = stage("backupcheck", pBackChk)
	s(0, "Checking the backup…")
	if got, n, err := gunzipHash(j.BackupPath, func(d int64) { s(float64(d)/float64(span), "Checking the backup…") }); err != nil || got != rawSum || n != span {
		return Friendly{"The backup did not read back correctly, so the card was not touched. Your card is exactly as it was."}
	}
	writeBackupNotes(j, span, rawSum)

	// 4. what is on the card, and what its new boot partition needs
	s = stage("prepare", pPrepare)
	s(0, "Reading this R36S's drivers…")
	info, err := card.Inspect(dev, tmp)
	if err != nil {
		return Friendly{"Could not read the card: " + err.Error() + ". Your backup is safe."}
	}
	if info.Kind == card.Unknown {
		return Friendly{"Stopped before writing: " + info.Why + ". Your backup is safe and nothing on the card has changed."}
	}
	p1, imgParts, err := imageBoot(j.ImagePath, tmp)
	if err != nil {
		return Friendly{"The VibeSDR image could not be read: " + err.Error() + ". Nothing on the card has changed."}
	}
	defer p1.Close()
	imgEnd := card.ImageEnd(imgParts)
	if imgEnd > j.Size {
		return Friendly{fmt.Sprintf("This card is too small: VibeSDR needs %.1f GB and the card holds %.1f GB. Nothing on the card has changed.", float64(imgEnd)/1e9, float64(j.Size)/1e9)}
	}
	summary, err := prepareBoot(p1, info, dev, tmp)
	if err != nil {
		return Friendly{"Stopped before writing: " + err.Error() + ". Your backup is safe and nothing on the card has changed."}
	}
	s(1, "Using "+summary)
	newBoot, err := os.ReadFile(p1.Name())
	if err != nil {
		return err
	}
	bootPart := imgParts[0]

	// 5. write — the image as published, with its boot partition replaced by the prepared one
	s = stage("write", pWrite)
	wroteSum, err := writeImage(dev, j.ImagePath, imgEnd, bootPart.Offset(), newBoot, func(d int64) {
		s(float64(d)/float64(imgEnd), fmt.Sprintf("Writing VibeSDR… %d%%", d*100/imgEnd))
	})
	if err != nil {
		return Friendly{"Writing the card failed (" + err.Error() + "). Your backup is safe: " + filepath.Base(j.BackupPath)}
	}
	if err := syncTarget(dev); err != nil {
		return Friendly{"The card did not finish writing (" + err.Error() + "). Your backup is safe."}
	}

	// 6. read back what was written
	s = stage("verify", pVerify)
	got, err := hashSpan(dev, imgEnd, func(d int64) { s(float64(d)/float64(imgEnd), fmt.Sprintf("Checking the card… %d%%", d*100/imgEnd)) })
	if err != nil || got != wroteSum {
		return Friendly{"The card did not read back correctly — it may be faulty. Try another card. Your backup is safe."}
	}
	dev.Close()
	if j.Disk != "" {
		_ = exec.Command("/usr/sbin/diskutil", "eject", j.Disk).Run()
	}
	rep(Event{Stage: "done", Frac: 1, Done: true, Text: "Done — put the card in your R36S and switch it on."})
	return nil
}

func backupText(kind string, d, t int64) string {
	what := "Backing up your whole card"
	if kind == "boot" {
		what = "Backing up the boot partition"
	}
	return fmt.Sprintf("%s… %d%%", what, d*100/t)
}

// bootSpan: the first 16 MiB (the loader) and the first partition, when it is the boot partition.
func bootSpan(r io.ReaderAt, size int64) int64 {
	span := int64(16 << 20)
	if parts, err := card.ReadMBR(r); err == nil && parts[0].End() > span && parts[0].End() <= size && parts[0].End() <= 4<<30 {
		span = parts[0].End()
	}
	if span > size {
		span = size
	}
	return span
}

func backup(r io.ReaderAt, span int64, dst string, prog func(int64)) (string, error) {
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return "", err
	}
	f, err := os.Create(dst + ".part")
	if err != nil {
		return "", err
	}
	zw, _ := pgzip.NewWriterLevel(f, gzip.BestSpeed)
	h := sha256.New()
	buf := make([]byte, chunk)
	for off := int64(0); off < span; {
		n := int64(len(buf))
		if span-off < n {
			n = span - off
		}
		if _, err := r.ReadAt(buf[:n], off); err != nil && err != io.EOF {
			f.Close()
			return "", fmt.Errorf("reading the card at %d MB: %w", off>>20, err)
		}
		h.Write(buf[:n])
		if _, err := zw.Write(buf[:n]); err != nil {
			f.Close()
			return "", err
		}
		off += n
		prog(off)
	}
	if err := zw.Close(); err != nil {
		f.Close()
		return "", err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		return "", err
	}
	f.Close()
	if err := os.Rename(dst+".part", dst); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func gunzipHash(p string, prog func(int64)) (string, int64, error) {
	f, err := os.Open(p)
	if err != nil {
		return "", 0, err
	}
	defer f.Close()
	zr, err := pgzip.NewReader(f)
	if err != nil {
		return "", 0, err
	}
	h := sha256.New()
	buf := make([]byte, chunk)
	var n int64
	for {
		k, err := zr.Read(buf)
		if k > 0 {
			h.Write(buf[:k])
			n += int64(k)
			prog(n)
		}
		if err == io.EOF {
			break
		}
		if err != nil {
			return "", n, err
		}
	}
	return hex.EncodeToString(h.Sum(nil)), n, nil
}

func hashFile(p string, prog func(d, t int64)) (string, error) {
	f, err := os.Open(p)
	if err != nil {
		return "", err
	}
	defer f.Close()
	st, _ := f.Stat()
	h := sha256.New()
	buf := make([]byte, chunk)
	var d int64
	for {
		n, err := f.Read(buf)
		if n > 0 {
			h.Write(buf[:n])
			d += int64(n)
			prog(d, st.Size())
		}
		if err == io.EOF {
			return hex.EncodeToString(h.Sum(nil)), nil
		}
		if err != nil {
			return "", err
		}
	}
}

func hashSpan(r io.ReaderAt, span int64, prog func(int64)) (string, error) {
	h := sha256.New()
	buf := make([]byte, chunk)
	for off := int64(0); off < span; {
		n := int64(len(buf))
		if span-off < n {
			n = span - off
		}
		if _, err := r.ReadAt(buf[:n], off); err != nil && err != io.EOF {
			return "", err
		}
		h.Write(buf[:n])
		off += n
		prog(off)
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// imageBoot decompresses the image up to the end of its first partition and returns that partition as a file.
func imageBoot(img, tmp string) (*os.File, []card.Part, error) {
	f, err := os.Open(img)
	if err != nil {
		return nil, nil, err
	}
	defer f.Close()
	zr, err := pgzip.NewReader(f)
	if err != nil {
		return nil, nil, err
	}
	defer zr.Close()
	head := make([]byte, 512)
	if _, err := io.ReadFull(zr, head); err != nil {
		return nil, nil, err
	}
	parts, err := card.ReadMBR(bytes.NewReader(head))
	if err != nil {
		return nil, nil, fmt.Errorf("the image has %s", err)
	}
	p := parts[0]
	if !p.IsFAT() {
		return nil, nil, errors.New("the image's first partition is not its boot partition")
	}
	if _, err := io.CopyN(io.Discard, zr, p.Offset()-512); err != nil {
		return nil, nil, err
	}
	out, err := os.CreateTemp(tmp, "newboot-*.img")
	if err != nil {
		return nil, nil, err
	}
	if _, err := io.CopyN(out, zr, p.Size()); err != nil {
		out.Close()
		return nil, nil, err
	}
	return out, parts, nil
}

// prepareBoot puts this R36S's drivers into the new boot partition (p1) — see card.PlanDrivers.
func prepareBoot(p1 *os.File, info *card.Info, dev io.ReaderAt, tmp string) (string, error) {
	newFS, err := card.OpenFAT(p1)
	if err != nil {
		return "", fmt.Errorf("the image's boot partition: %w", err)
	}
	ext, err := card.ReadFile(newFS, "extlinux/extlinux.conf")
	if err != nil {
		return "", errors.New("the image has no boot menu (extlinux.conf)")
	}
	var oldFS = (*os.File)(nil)
	var plan *card.Plan
	if info.Kind == card.VibeSDR {
		oldFS, err = card.ExtractPart(dev, info.Parts[0], tmp)
		if err != nil {
			return "", err
		}
		defer oldFS.Close()
		ofs, err := card.OpenFAT(oldFS)
		if err != nil {
			return "", fmt.Errorf("the card's boot partition: %w", err)
		}
		plan, err = card.PlanDrivers(info, newFS, string(ext), ofs)
		if err != nil {
			return "", err
		}
	} else {
		plan, err = card.PlanDrivers(info, newFS, string(ext), nil)
		if err != nil {
			return "", err
		}
	}
	if err := card.ApplyPlan(newFS, string(ext), plan); err != nil {
		return "", err
	}
	return plan.Summary, p1.Sync()
}

// writeImage streams the image onto the target with [bootOff, bootOff+len(boot)) replaced by boot; returns the
// SHA-256 of exactly what was written.
func writeImage(dev io.WriterAt, img string, end, bootOff int64, boot []byte, prog func(int64)) (string, error) {
	f, err := os.Open(img)
	if err != nil {
		return "", err
	}
	defer f.Close()
	zr, err := pgzip.NewReader(f)
	if err != nil {
		return "", err
	}
	defer zr.Close()
	h := sha256.New()
	buf := make([]byte, chunk)
	bootEnd := bootOff + int64(len(boot))
	for off := int64(0); off < end; {
		n := int64(len(buf))
		if end-off < n {
			n = end - off
		}
		if _, err := io.ReadFull(zr, buf[:n]); err != nil {
			return "", fmt.Errorf("the image ended early: %w", err)
		}
		// overlay the prepared boot partition
		if off < bootEnd && off+n > bootOff {
			a, b := max64(off, bootOff), min64(off+n, bootEnd)
			copy(buf[a-off:b-off], boot[a-bootOff:b-bootOff])
		}
		h.Write(buf[:n])
		if testCorrupt != nil {
			testCorrupt(off, buf[:n]) // ★ tests only: a card that does not keep what it is given
		}
		if _, err := dev.WriteAt(buf[:n], off); err != nil {
			return "", fmt.Errorf("at %d MB: %w", off>>20, err)
		}
		off += n
		prog(off)
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func max64(a, b int64) int64 {
	if a > b {
		return a
	}
	return b
}
func min64(a, b int64) int64 {
	if a < b {
		return a
	}
	return b
}

func writeBackupNotes(j *Job, span int64, raw string) {
	base := filepath.Base(j.BackupPath)
	os.WriteFile(j.BackupPath+".sha256", []byte(raw+"  "+strings.TrimSuffix(base, ".gz")+"\n"), 0o644)
	what := "the WHOLE card — every byte. To put your R36S back exactly as it was, write it back to the card (VibeSDR Flasher › Advanced, or: gunzip -c \"" + base + "\" | sudo dd of=/dev/rdiskN bs=4m)."
	if j.BackupKind == "boot" {
		what = "the card's first 16 MB and its boot partition — its drivers. Enough to make another VibeSDR card for this R36S; not enough to restore its old system."
	}
	note := fmt.Sprintf("VibeSDR Flasher backup — %s\n\n%s\n\nThis backup holds %s\nSize: %d bytes (%.1f GB) before compression.\nSHA-256 of those bytes: %s\nVibeSDR image written afterwards: %s\n",
		time.Now().Format("2 Jan 2006 15:04"), base, what, span, float64(span)/1e9, raw, j.ImageVersion)
	os.WriteFile(filepath.Join(filepath.Dir(j.BackupPath), "README.txt"), []byte(note), 0o644)
	if j.UID > 0 {
		for _, p := range []string{filepath.Dir(j.BackupPath), j.BackupPath, j.BackupPath + ".sha256", filepath.Join(filepath.Dir(j.BackupPath), "README.txt")} {
			os.Chown(p, j.UID, j.GID)
		}
	}
}
