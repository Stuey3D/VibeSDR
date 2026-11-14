// Package disks lists the cards a person could mean — never the Mac's own disks.
package disks

import (
	"bytes"
	"fmt"
	"os/exec"
	"sort"
	"strings"

	"howett.net/plist"
)

// Disk is one card the window may offer.
type Disk struct {
	ID       string   `json:"id"`       // disk4
	Device   string   `json:"device"`   // /dev/disk4
	Raw      string   `json:"raw"`      // /dev/rdisk4
	Name     string   `json:"name"`     // the reader's or card's name
	Size     int64    `json:"size"`     // bytes
	Bus      string   `json:"bus"`      // Secure Digital, USB
	Volumes  []string `json:"volumes"`  // the volume names on it (VIBEBOOT, VIBESDR, …)
	Looks    string   `json:"looks"`    // "VibeSDR card", "R36S card", …
	Large    bool     `json:"large"`    // over 256 GB: needs an extra confirmation
	Internal bool     `json:"internal"` // the built-in SD slot reports itself internal — said, not hidden
}

func run(args ...string) ([]byte, error) {
	var out bytes.Buffer
	c := exec.Command("/usr/sbin/diskutil", args...)
	c.Stdout = &out
	err := c.Run()
	return out.Bytes(), err
}

type listPlist struct {
	AllDisksAndPartitions []struct {
		DeviceIdentifier string
		Partitions       []struct {
			VolumeName string
			Content    string
		}
		VolumeName string
	}
	WholeDisks []string
}

type infoPlist struct {
	DeviceIdentifier  string
	WholeDisk         bool
	Internal          bool
	RemovableMedia    bool
	Removable         bool
	Ejectable         bool
	BusProtocol       string
	VirtualOrPhysical string
	TotalSize         int64
	Size              int64
	MediaName         string
	IORegistryEntryName string
}

func info(id string) (*infoPlist, error) {
	b, err := run("info", "-plist", id)
	if err != nil {
		return nil, err
	}
	var in infoPlist
	if _, err := plist.Unmarshal(b, &in); err != nil {
		return nil, err
	}
	return &in, nil
}

// bootWhole is the whole disk the Mac started from (its APFS physical store), to be refused always.
func bootWhole() map[string]bool {
	out := map[string]bool{"disk0": true}
	b, err := run("info", "-plist", "/")
	if err != nil {
		return out
	}
	var m map[string]any
	if _, err := plist.Unmarshal(b, &m); err != nil {
		return out
	}
	if s, ok := m["ParentWholeDisk"].(string); ok {
		out[s] = true
	}
	if a, ok := m["APFSPhysicalStores"].([]any); ok {
		for _, x := range a {
			if d, ok := x.(map[string]any); ok {
				if s, ok := d["APFSPhysicalStore"].(string); ok && strings.HasPrefix(s, "disk") {
					// disk0s2 → disk0: the whole disk the startup volume's store lives on
					if i := strings.Index(s[4:], "s"); i > 0 {
						out[s[:4+i]] = true
					} else {
						out[s] = true
					}
				}
			}
		}
	}
	return out
}

// List returns the removable physical disks with media in them.
func List() ([]Disk, error) {
	b, err := run("list", "-plist", "physical")
	if err != nil {
		return nil, fmt.Errorf("could not list disks: %w", err)
	}
	var l listPlist
	if _, err := plist.Unmarshal(b, &l); err != nil {
		return nil, err
	}
	boot := bootWhole()
	vols := map[string][]string{}
	for _, d := range l.AllDisksAndPartitions {
		for _, p := range d.Partitions {
			if p.VolumeName != "" {
				vols[d.DeviceIdentifier] = append(vols[d.DeviceIdentifier], p.VolumeName)
			}
		}
	}
	var out []Disk
	for _, id := range l.WholeDisks {
		if boot[id] {
			continue
		}
		in, err := info(id)
		if err != nil || !in.WholeDisk || strings.EqualFold(in.VirtualOrPhysical, "Virtual") ||
			strings.EqualFold(in.BusProtocol, "Disk Image") {
			continue
		}
		// ★ Removable media only. The MacBook's own SD slot says Internal — it is still removable media, so it is
		//   offered; a fixed internal drive never is.
		if !(in.RemovableMedia || in.Removable || in.Ejectable) {
			continue
		}
		size := in.TotalSize
		if size == 0 {
			size = in.Size
		}
		if size == 0 {
			continue // a reader with no card in it
		}
		name := strings.TrimSpace(in.MediaName)
		if name == "" {
			name = strings.TrimSpace(in.IORegistryEntryName)
		}
		d := Disk{ID: id, Device: "/dev/" + id, Raw: "/dev/r" + id, Name: name, Size: size, Bus: in.BusProtocol,
			Volumes: vols[id], Large: size > 256e9, Internal: in.Internal}
		d.Looks = looks(d.Volumes)
		out = append(out, d)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out, nil
}

func looks(v []string) string {
	has := func(s string) bool {
		for _, x := range v {
			if strings.EqualFold(x, s) {
				return true
			}
		}
		return false
	}
	switch {
	case has("VIBEBOOT") || has("VIBESDR"):
		return "VibeSDR card"
	case has("BOOT") || has("EASYROMS") || has("ROMS") || has("STORAGE") || has("ROCKNIX"):
		return "Looks like an R36S card"
	case len(v) == 0:
		return "No readable volumes"
	}
	return strings.Join(v, ", ")
}
