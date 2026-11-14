package card

import (
	"errors"
	"fmt"
	"os"
	"path"
	"strings"

	"github.com/diskfs/go-diskfs/filesystem"
)

// Ext is the part of an extlinux.conf the flasher cares about.
type Ext struct {
	FDT      string
	Overlays []string
	Append   []string
}

// ParseExt reads FDT, FDTOVERLAYS and APPEND from the first LABEL.
func ParseExt(s string) Ext {
	var e Ext
	for _, ln := range strings.Split(s, "\n") {
		f := strings.Fields(ln)
		if len(f) == 0 {
			continue
		}
		switch strings.ToUpper(f[0]) {
		case "FDT", "DEVICETREE":
			if e.FDT == "" && len(f) > 1 {
				e.FDT = f[1]
			}
		case "FDTOVERLAYS":
			if e.Overlays == nil {
				e.Overlays = append([]string{}, f[1:]...)
			}
		case "APPEND":
			if e.Append == nil {
				e.Append = append([]string{}, f[1:]...)
			}
		}
	}
	return e
}

// carryTokens are the kernel arguments an owner may have added to a VibeSDR card that a new system keeps.
// ★ Only ours, by name — anything else on the old line belongs to the old system (root=, its PARTUUID, …).
var carryTokens = []string{"vibe.debuglog"}

// Plan is what will be done to the new image's boot partition.
type Plan struct {
	FDT      string
	Overlays []string
	AddArgs  []string
	CopyIn   map[string][]byte // files the new boot partition lacks, from the old card
	Changed  bool
	Summary  string
}

func exists(fsys filesystem.FileSystem, p string) bool {
	_, err := ReadFile(fsys, p)
	return err == nil
}

// PlanDrivers decides what the new image's boot partition (newFS, newExt) gets from the card that was there (src).
// oldFS is the card's own boot partition (VibeSDR cards: where a file the new image lacks is copied from).
func PlanDrivers(src *Info, newFS filesystem.FileSystem, newExt string, oldFS filesystem.FileSystem) (*Plan, error) {
	ne := ParseExt(newExt)
	pl := &Plan{FDT: ne.FDT, Overlays: ne.Overlays, CopyIn: map[string][]byte{}}
	switch src.Kind {
	case VibeSDR:
		oe := ParseExt(src.Extlinux)
		// ★ The CHOICE is carried (which device tree, which overlays); the FILES are the new image's own when it has
		//   them — they were built for its kernel. Only a file the new image lacks is copied from the card.
		take := func(p string) bool {
			if p == "" {
				return false
			}
			if exists(newFS, p) {
				return true
			}
			if oldFS != nil {
				if b, err := ReadFile(oldFS, p); err == nil {
					pl.CopyIn[path.Clean("/"+p)] = b
					return true
				}
			}
			return false
		}
		if oe.FDT != "" && take(oe.FDT) {
			pl.FDT = oe.FDT
		}
		if oe.Overlays != nil {
			ok := true
			for _, o := range oe.Overlays {
				if !take(o) {
					ok = false
				}
			}
			if ok {
				pl.Overlays = oe.Overlays
			}
		}
		for _, t := range carryTokens {
			if contains(oe.Append, t) && !contains(ne.Append, t) {
				pl.AddArgs = append(pl.AddArgs, t)
			}
		}
		pl.Summary = "your VibeSDR card's own settings"
	case Stock:
		if !exists(newFS, src.Board.VibeFDT) {
			return nil, fmt.Errorf("this VibeSDR image has no driver for the %s", src.Board.Name)
		}
		pl.FDT = src.Board.VibeFDT
		pl.Summary = "the " + src.Board.Name + " drivers"
	default:
		return nil, errors.New(src.Why)
	}
	pl.Changed = pl.FDT != ne.FDT || strings.Join(pl.Overlays, " ") != strings.Join(ne.Overlays, " ") ||
		len(pl.AddArgs) > 0 || len(pl.CopyIn) > 0
	return pl, nil
}

func contains(xs []string, s string) bool {
	for _, x := range xs {
		if x == s {
			return true
		}
	}
	return false
}

// RewriteExt applies a plan to an extlinux.conf's text.
func RewriteExt(s string, pl *Plan) string {
	lines := strings.Split(s, "\n")
	for i, ln := range lines {
		f := strings.Fields(ln)
		if len(f) == 0 {
			continue
		}
		indent := ln[:len(ln)-len(strings.TrimLeft(ln, " \t"))]
		switch strings.ToUpper(f[0]) {
		case "FDT", "DEVICETREE":
			lines[i] = indent + f[0] + " " + pl.FDT
		case "FDTOVERLAYS":
			lines[i] = indent + f[0] + " " + strings.Join(pl.Overlays, " ")
		case "APPEND":
			if len(pl.AddArgs) > 0 {
				lines[i] = strings.TrimRight(ln, " \t") + " " + strings.Join(pl.AddArgs, " ")
			}
		}
	}
	return strings.Join(lines, "\n")
}

// writeFile writes a whole file into a FAT filesystem, creating its directory.
func writeFile(fsys filesystem.FileSystem, p string, b []byte) error {
	p = strings.TrimPrefix(path.Clean("/"+p), "/")
	if d := path.Dir(p); d != "." {
		_ = fsys.Mkdir("/" + d)
	}
	f, err := fsys.OpenFile("/"+p, os.O_RDWR|os.O_CREATE|os.O_TRUNC)
	if err != nil {
		return err
	}
	if _, err := f.Write(b); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

// ApplyPlan writes the plan into the new boot partition's filesystem.
func ApplyPlan(newFS filesystem.FileSystem, newExt string, pl *Plan) error {
	if !pl.Changed {
		return nil
	}
	for p, b := range pl.CopyIn {
		if err := writeFile(newFS, p, b); err != nil {
			return fmt.Errorf("could not copy %s into the new card: %w", p, err)
		}
	}
	return writeFile(newFS, "extlinux/extlinux.conf", []byte(RewriteExt(newExt, pl)))
}
