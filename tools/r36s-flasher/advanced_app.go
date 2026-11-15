package main

// ADVANCED in the window (docs/v12/R36S-FLASHER.md §1.2): choose the drivers (a saved backup or image, the generic
// ROCKNIX drivers, your own device tree), check them before any card is touched, then write to ANY card — or put a
// backup back on a card. The work itself is internal/job (advanced.go).

import (
	"errors"
	"os"
	"path/filepath"
	"strings"

	"vibesdr.net/r36s-flasher/internal/card"
	"vibesdr.net/r36s-flasher/internal/job"
	"vibesdr.net/r36s-flasher/internal/release"

	"github.com/wailsapp/wails/v2/pkg/runtime"
)

// Drivers is the window's Advanced choice.
type Drivers struct {
	Kind     string   `json:"kind"`     // "file" | "generic" | "own"
	File     string   `json:"file"`     // kind "file": the backup or image
	DTB      string   `json:"dtb"`      // kind "own"
	Overlays []string `json:"overlays"` // kind "own", optional
}

func backupsDir() string {
	h, _ := os.UserHomeDir()
	return filepath.Join(h, "Documents", "VibeSDR Backups")
}

// Backups lists the backups this app has made, newest first.
func (a *App) Backups() []card.Backup { return card.ListBackups(backupsDir()) }

func (a *App) startDir() string {
	if st, err := os.Stat(backupsDir()); err == nil && st.IsDir() {
		return backupsDir()
	}
	h, _ := os.UserHomeDir()
	return filepath.Join(h, "Downloads")
}

// ChooseImageFile asks for a backup or card image.
func (a *App) ChooseImageFile(title string) string {
	p, err := runtime.OpenFileDialog(a.ctx, runtime.OpenDialogOptions{
		Title: title, DefaultDirectory: a.startDir(),
		Filters: []runtime.FileFilter{{DisplayName: "Card images and backups (.img, .img.gz, .img.xz, .iso, .zip)",
			Pattern: "*.img;*.gz;*.xz;*.iso;*.zip;*.bin"}},
	})
	if err != nil {
		return ""
	}
	return p
}

// ChooseDTB asks for the person's own device tree.
func (a *App) ChooseDTB() string {
	p, err := runtime.OpenFileDialog(a.ctx, runtime.OpenDialogOptions{
		Title: "Choose your R36S's device tree (.dtb)", DefaultDirectory: a.startDir(),
		Filters: []runtime.FileFilter{{DisplayName: "Device trees (.dtb)", Pattern: "*.dtb"}},
	})
	if err != nil {
		return ""
	}
	return p
}

// ChooseOverlays asks for panel overlays (optional).
func (a *App) ChooseOverlays() []string {
	ps, err := runtime.OpenMultipleFilesDialog(a.ctx, runtime.OpenDialogOptions{
		Title: "Choose the panel overlay(s) (.dtbo)", DefaultDirectory: a.startDir(),
		Filters: []runtime.FileFilter{{DisplayName: "Device tree overlays (.dtbo)", Pattern: "*.dtbo"}},
	})
	if err != nil {
		return nil
	}
	return ps
}

func (a *App) jobFor(dr Drivers, j *job.Job) error {
	switch dr.Kind {
	case "file":
		if dr.File == "" {
			return errors.New("choose a backup or image first")
		}
		j.Drivers, j.DriverFile = "file", dr.File
	case "generic":
		j.Drivers = "generic"
	case "own":
		if dr.DTB == "" {
			return errors.New("choose your device tree (.dtb) first")
		}
		j.Drivers, j.OwnDTB, j.OwnOverlays = "own", dr.DTB, dr.Overlays
	default:
		return errors.New("choose where the drivers come from")
	}
	return nil
}

// CheckDrivers reads the chosen drivers now — before any card — and says what will be used, or why not.
func (a *App) CheckDrivers(dr Drivers) (string, error) {
	a.mu.Lock()
	img := a.img
	a.mu.Unlock()
	if img == nil {
		return "", errors.New("there is no VibeSDR image to write yet")
	}
	tmp, err := os.MkdirTemp(release.CacheDir(), "check-")
	if err != nil {
		os.MkdirAll(release.CacheDir(), 0o755)
		if tmp, err = os.MkdirTemp(release.CacheDir(), "check-"); err != nil {
			return "", err
		}
	}
	defer os.RemoveAll(tmp)
	j := &job.Job{ImagePath: img.Path, Tmp: tmp}
	if err := a.jobFor(dr, j); err != nil {
		return "", err
	}
	ab, err := job.PrepareAdvanced(j, tmp)
	if err != nil {
		return "", errors.New(strings.TrimSuffix(err.Error(), " Nothing has been changed."))
	}
	return ab.Summary, nil
}

// RestorePreview: what a backup or image is, read quickly (its first sector and the notes beside it). The whole file
// is checked when it is put back.
type RestorePreview struct {
	Name    string `json:"name"`
	Bytes   int64  `json:"bytes"` // -1 when it is only known after reading it all
	Whole   bool   `json:"whole"`
	Checked bool   `json:"checked"` // a .sha256 is beside it (checked when it is put back)
}

func (a *App) PreviewRestore(p string) (RestorePreview, error) {
	rp := RestorePreview{Name: filepath.Base(p), Bytes: -1, Whole: true, Checked: card.SHA256Beside(p) != ""}
	r, size, err := card.OpenStream(p)
	if err != nil {
		return rp, errors.New(filepath.Base(p) + " could not be opened: " + err.Error())
	}
	defer r.Close()
	head := make([]byte, 512)
	if _, err := r.Read(head); err != nil || head[510] != 0x55 || head[511] != 0xaa {
		return rp, errors.New(filepath.Base(p) + " is not an image of a card (it has no partition table)")
	}
	rp.Bytes = size
	for _, b := range card.ListBackups(filepath.Dir(filepath.Dir(p))) {
		if b.Path == p {
			rp.Bytes, rp.Whole = b.Bytes, b.Kind == "whole"
		}
	}
	return rp, nil
}

// StartAdvanced writes VibeSDR with the chosen drivers to any card; kind "whole" | "boot" | "none" (its backup).
func (a *App) StartAdvanced(diskID, kind, folder string, confirmedLarge bool, dr Drivers) error {
	a.mu.Lock()
	if a.running {
		a.mu.Unlock()
		return errors.New("already running")
	}
	img := a.img
	a.mu.Unlock()
	if img == nil {
		return errors.New("there is no VibeSDR image to write yet")
	}
	if kind != "whole" && kind != "boot" && kind != "none" {
		return errors.New("choose what to back up")
	}
	var probe job.Job
	if err := a.jobFor(dr, &probe); err != nil {
		return err
	}
	d, err := findDisk(diskID)
	if err != nil {
		return err
	}
	if d.Large && !confirmedLarge {
		return errors.New("this disk is over 256 GB — confirm it really is the card for VibeSDR")
	}
	a.mu.Lock()
	a.running = true
	a.mu.Unlock()
	go a.run(d, kind, folder, img, func(j *job.Job) {
		j.Mode, j.BackupKind = "make", kind
		a.jobFor(dr, j)
	})
	return nil
}

// StartRestore puts a backup or image back on a card; kind is the card's own backup first ("whole" | "boot" | "none").
func (a *App) StartRestore(diskID, kind, folder string, confirmedLarge bool, path string) error {
	a.mu.Lock()
	if a.running {
		a.mu.Unlock()
		return errors.New("already running")
	}
	a.mu.Unlock()
	if path == "" {
		return errors.New("choose the backup to put back")
	}
	if kind != "whole" && kind != "boot" && kind != "none" {
		return errors.New("choose what to back up")
	}
	d, err := findDisk(diskID)
	if err != nil {
		return err
	}
	if d.Large && !confirmedLarge {
		return errors.New("this disk is over 256 GB — confirm it really is the card")
	}
	a.mu.Lock()
	a.running = true
	a.mu.Unlock()
	go a.run(d, kind, folder, nil, func(j *job.Job) {
		j.Mode, j.BackupKind, j.RestorePath = "restore", kind, path
	})
	return nil
}

