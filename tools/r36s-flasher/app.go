package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"vibesdr.net/r36s-flasher/internal/authopen"
	"vibesdr.net/r36s-flasher/internal/disks"
	"vibesdr.net/r36s-flasher/internal/job"
	"vibesdr.net/r36s-flasher/internal/release"

	"github.com/wailsapp/wails/v2/pkg/runtime"
)

// Version of the flasher.
const Version = "0.2.0-localtest"

// App is what the window calls.
type App struct {
	ctx     context.Context
	mu      sync.Mutex
	img     *release.Image
	imgErr  string
	running bool
}

func NewApp() *App { return &App{} }

func (a *App) startup(ctx context.Context) {
	a.ctx = ctx
	go a.findImage()
}

func (a *App) findImage() {
	var img *release.Image
	var err error
	if release.LocalTest {
		img, err = release.FindLocal(release.LocalDir())
	} else {
		err = errors.New("published images are not used in this build")
	}
	a.mu.Lock()
	a.img = img
	if err != nil {
		a.imgErr = err.Error()
	} else {
		a.imgErr = ""
	}
	a.mu.Unlock()
	runtime.EventsEmit(a.ctx, "status")
}

// Status is the first screen's line about the image.
type Status struct {
	LocalTest bool   `json:"localTest"`
	Version   string `json:"version"`
	ImageOK   bool   `json:"imageOk"`
	Image     string `json:"image"`
	ImageErr  string `json:"imageErr"`
	Flasher   string `json:"flasher"`
}

func (a *App) Status() Status {
	a.mu.Lock()
	defer a.mu.Unlock()
	s := Status{LocalTest: release.LocalTest, Flasher: Version, ImageErr: a.imgErr}
	if a.img != nil {
		s.ImageOK, s.Version, s.Image = true, a.img.Version, filepath.Base(a.img.Path)
	}
	return s
}

// Rescan looks for the image again (the person may have just put it on the Desktop).
func (a *App) Rescan() Status { a.findImage(); return a.Status() }

// Disks lists the cards.
func (a *App) Disks() ([]disks.Disk, error) { return disks.List() }

// DefaultBackupDir is where the backup goes unless the person picks somewhere else.
func (a *App) DefaultBackupDir(name string) string {
	h, _ := os.UserHomeDir()
	safe := strings.Map(func(r rune) rune {
		if strings.ContainsRune(`/\:*?"<>|`, r) {
			return '-'
		}
		return r
	}, strings.TrimSpace(name))
	if safe == "" {
		safe = "SD card"
	}
	return filepath.Join(h, "Documents", "VibeSDR Backups", time.Now().Format("2006-01-02 1504")+" "+safe)
}

// ChooseFolder asks where the backup should go.
func (a *App) ChooseFolder(def string) string {
	// ★ the dialog refuses to open at a folder that does not exist yet (Wails) — and "VibeSDR Backups" does not until
	//   the first backup: Stuart's Change… did nothing (2026-11-14). Start at the nearest folder that does exist.
	start := filepath.Dir(def)
	for start != "/" {
		if st, err := os.Stat(start); err == nil && st.IsDir() {
			break
		}
		start = filepath.Dir(start)
	}
	dir, err := runtime.OpenDirectoryDialog(a.ctx, runtime.OpenDialogOptions{
		Title: "Where should the backup go?", DefaultDirectory: start, CanCreateDirectories: true,
	})
	if err != nil || dir == "" {
		return def
	}
	return filepath.Join(dir, filepath.Base(def))
}

// Start runs Simple mode on one card. Progress arrives as "progress" events; the end as "finished".
func (a *App) Start(diskID, kind, folder string, confirmedLarge bool) error {
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
	if kind != "whole" && kind != "boot" && kind != "update" {
		return errors.New("choose what to back up")
	}
	// ★ the card is looked up AGAIN, now: never write to a disk from a list that may be out of date
	list, err := disks.List()
	if err != nil {
		return err
	}
	var d *disks.Disk
	for i := range list {
		if list[i].ID == diskID {
			d = &list[i]
		}
	}
	if d == nil {
		return errors.New("that card is no longer there — put it back in and choose it again")
	}
	if d.Large && !confirmedLarge {
		return errors.New("this disk is over 256 GB — confirm it really is the R36S's card")
	}
	a.mu.Lock()
	a.running = true
	a.mu.Unlock()
	go a.run(d, kind, folder, img)
	return nil
}

func (a *App) emit(e job.Event) { runtime.EventsEmit(a.ctx, "progress", e) }

func (a *App) finish(err error, backupDir, doneText string) {
	a.mu.Lock()
	a.running = false
	a.mu.Unlock()
	msg := ""
	if err != nil {
		msg = err.Error()
	}
	runtime.EventsEmit(a.ctx, "finished", map[string]string{"error": msg, "backup": backupDir, "done": doneText})
}

func (a *App) run(d *disks.Disk, kind, folder string, img *release.Image) {
	work := filepath.Join(release.CacheDir(), "run-"+time.Now().Format("20060102-150405"))
	if err := os.MkdirAll(work, 0o755); err != nil {
		a.finish(err, "", "")
		return
	}
	defer os.RemoveAll(work)
	// 1. the image into the cache, checked as this person
	a.emit(job.Event{Stage: "image", Frac: 0, Text: "Getting the VibeSDR image ready…"})
	cached := filepath.Join(release.CacheDir(), filepath.Base(img.Path))
	if got, err := release.FileSHA256(cached, nil); err != nil || got != img.SHA256 {
		if err := copyFile(img.Path, cached, func(d, t int64) {
			a.emit(job.Event{Stage: "image", Frac: 0.02 * float64(d) / float64(t), Text: "Getting the VibeSDR image ready…"})
		}); err != nil {
			a.finish(fmt.Errorf("could not copy the VibeSDR image: %w", err), "", "")
			return
		}
	}
	// 2. the job — run here, as the person. Only the card itself needs permission, and macOS's own authopen opens it
	//    after one system password prompt and hands it back (internal/authopen). Nothing runs as root.
	staged := filepath.Join(work, "backup", filepath.Base(folder)+".img.gz")
	mode, bk := "make", kind
	if kind == "update" { // ★ Update backs up the boot partition (and the loader) only — see internal/job/update.go
		mode, bk = "update", "boot"
	}
	j := job.Job{Mode: mode, Target: d.Raw, Disk: d.Device, Size: d.Size, BackupKind: bk, BackupPath: staged, ImagePath: cached,
		ImageSHA256: img.SHA256, ImageVersion: img.Version, Tmp: work,
		Open: func(path string, flags int) (*os.File, error) {
			a.emit(job.Event{Stage: "open", Frac: 0.02, Text: "Waiting for your password…"})
			f, err := authopen.Open(path, flags, "VibeSDR Flasher needs your password to back up and write the SD card.")
			if errors.Is(err, authopen.ErrCancelled) {
				return nil, job.ErrCancelled
			}
			return f, err
		}}
	doneText := ""
	runErr := job.Run(&j, func(e job.Event) {
		if e.Done {
			doneText = e.Text
		}
		if !e.Done && e.Error == "" { // the end is reported once, by finish
			a.emit(e)
		}
	})
	// 3. the backup to where the person asked — whatever happened, it is theirs
	backupDir := ""
	if _, err := os.Stat(staged); err == nil {
		if err := os.MkdirAll(filepath.Dir(folder), 0o755); err == nil {
			if err := moveDir(filepath.Dir(staged), folder); err == nil {
				backupDir = folder
			}
		}
	}
	a.finish(runErr, backupDir, doneText)
}

func copyFile(src, dst string, prog func(d, t int64)) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	st, _ := in.Stat()
	os.MkdirAll(filepath.Dir(dst), 0o755)
	out, err := os.Create(dst + ".part")
	if err != nil {
		return err
	}
	buf := make([]byte, 4<<20)
	var d int64
	for {
		n, rerr := in.Read(buf)
		if n > 0 {
			if _, err := out.Write(buf[:n]); err != nil {
				out.Close()
				return err
			}
			d += int64(n)
			prog(d, st.Size())
		}
		if rerr == io.EOF {
			break
		}
		if rerr != nil {
			out.Close()
			return rerr
		}
	}
	out.Close()
	return os.Rename(dst+".part", dst)
}

// moveDir moves the staged backup folder to where the person asked (a rename on the same disk, else a copy).
func moveDir(src, dst string) error {
	if err := os.Rename(src, dst); err == nil {
		return nil
	}
	if err := os.MkdirAll(dst, 0o755); err != nil {
		return err
	}
	ents, err := os.ReadDir(src)
	if err != nil {
		return err
	}
	for _, e := range ents {
		if err := copyFile(filepath.Join(src, e.Name()), filepath.Join(dst, e.Name()), func(int64, int64) {}); err != nil {
			return err
		}
	}
	return nil
}

// RevealBackup opens the backup folder in Finder.
func (a *App) RevealBackup(p string) { exec.Command("/usr/bin/open", p).Run() }
