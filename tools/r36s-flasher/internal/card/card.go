// Package card reads what is on an SD card (or an image of one): its partition table, its FAT boot partitions, the
// device trees on them — and decides whether it is a VibeSDR card, a stock card of a board we know, or something we
// must not guess about. docs/v12/R36S-FLASHER.md §3.
package card

import (
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path"
	"strings"

	"vibesdr.net/r36s-flasher/internal/fdt"

	"github.com/diskfs/go-diskfs/backend/file"
	"github.com/diskfs/go-diskfs/filesystem"
	"github.com/diskfs/go-diskfs/filesystem/fat16"
	"github.com/diskfs/go-diskfs/filesystem/fat32"
)

// Part is one MBR partition entry.
type Part struct {
	Index        int // 1..4
	Type         byte
	Start, Sects uint32
}

func (p Part) Offset() int64 { return int64(p.Start) * 512 }
func (p Part) Size() int64   { return int64(p.Sects) * 512 }
func (p Part) End() int64    { return p.Offset() + p.Size() }
func (p Part) IsFAT() bool {
	switch p.Type {
	case 0x01, 0x04, 0x06, 0x0b, 0x0c, 0x0e:
		return true
	}
	return false
}

// ReadMBR reads the four primary partitions; an error when there is no MBR signature.
func ReadMBR(r io.ReaderAt) ([]Part, error) {
	b := make([]byte, 512)
	if _, err := r.ReadAt(b, 0); err != nil {
		return nil, fmt.Errorf("could not read the start of the card: %w", err)
	}
	if b[510] != 0x55 || b[511] != 0xaa {
		return nil, errors.New("no partition table")
	}
	var out []Part
	for i := 0; i < 4; i++ {
		e := b[446+16*i : 462+16*i]
		p := Part{Index: i + 1, Type: e[4], Start: binary.LittleEndian.Uint32(e[8:]), Sects: binary.LittleEndian.Uint32(e[12:])}
		if p.Type != 0 && p.Sects != 0 {
			out = append(out, p)
		}
	}
	if len(out) == 0 {
		return nil, errors.New("an empty partition table")
	}
	return out, nil
}

// ImageEnd is the end of the last partition — how many bytes of an image matter.
func ImageEnd(parts []Part) int64 {
	var e int64
	for _, p := range parts {
		if p.End() > e {
			e = p.End()
		}
	}
	return e
}

// OpenFAT opens the FAT filesystem in a file holding exactly one partition. Writable only when asked.
func OpenFAT(f *os.File) (filesystem.FileSystem, error) {
	st, err := f.Stat()
	if err != nil {
		return nil, err
	}
	b := file.New(f, false)
	if fs32, err := fat32.Read(b, st.Size(), 0, 0); err == nil {
		return fs32, nil
	}
	if fs16, err := fat16.Read(b, st.Size(), 0, 0); err == nil {
		return fs16, nil
	}
	return nil, errors.New("not a FAT filesystem")
}

// DTB is one device tree found on a card.
type DTB struct {
	Path   string
	SHA256 string
	Info   fdt.Info
	Data   []byte
}

// Kind of card.
type Kind int

const (
	Unknown Kind = iota
	VibeSDR
	Stock
)

// Info is what Inspect found.
type Info struct {
	Kind     Kind
	Parts    []Part
	Label    string // the first FAT partition's label
	Extlinux string // a VibeSDR card's extlinux.conf
	DTBs     []DTB
	Board    *Board // a stock card of a board we know
	Why      string // why it is Unknown, in plain words
}

// Board is an R36S board VibeSDR knows. Today only Stuart's EE clone is proven (image/r36s/README.md).
type Board struct {
	Name        string
	StockSHA256 []string // the vendor DTB(s) on that board's stock card, byte for byte
	VibeFDT     string   // the ROCKNIX device tree our image boots it with
}

// Boards — ★★ known by the stock card's vendor DTB, byte for byte: a stock (Rockchip BSP 4.4) DTB is NEVER copied into
// our image (it is for another kernel); it only tells us which of OUR device trees the board needs. Anything else is
// refused — never guessed.
var Boards = []Board{{
	Name:        "R36S (EE clone)",
	StockSHA256: []string{"de0329eaede615231f8d0da5c4fac87babd56de22775c3c41e657681a91ab045"}, // image/r36s/board/rf3536k3ka.dtb
	VibeFDT:     "/rk3326-gameconsole-eeclone.dtb",
}}

func sha(b []byte) string { s := sha256.Sum256(b); return hex.EncodeToString(s[:]) }

// walkDTBs collects every .dtb in the filesystem, two directory levels down.
func walkDTBs(fsys filesystem.FileSystem) []DTB {
	var out []DTB
	// ★ go-diskfs takes io/fs-style paths: the root is ".", children "a/b" (no leading slash).
	var walk func(dir string, depth int)
	walk = func(dir string, depth int) {
		ents, err := fsys.ReadDir(dir)
		if err != nil {
			return
		}
		for _, e := range ents {
			if e.Name() == "." || e.Name() == ".." {
				continue
			}
			p := path.Join(dir, e.Name())
			if e.IsDir() {
				if depth < 2 {
					walk(p, depth+1)
				}
				continue
			}
			if !strings.HasSuffix(strings.ToLower(e.Name()), ".dtb") {
				continue
			}
			b, err := fsys.ReadFile(p)
			if err != nil || len(b) > 4<<20 {
				continue
			}
			in, _ := fdt.Parse(b)
			out = append(out, DTB{Path: "/" + p, SHA256: sha(b), Info: in, Data: b})
		}
	}
	walk(".", 0)
	return out
}

// ReadFile reads a file from a FAT filesystem by an absolute or relative path.
func ReadFile(fsys filesystem.FileSystem, p string) ([]byte, error) {
	return fs.ReadFile(fsys, strings.TrimPrefix(path.Clean("/"+p), "/"))
}

// ExtractPart copies one partition out of r into a new temp file (for the FAT library, which wants a file).
func ExtractPart(r io.ReaderAt, p Part, dir string) (*os.File, error) {
	f, err := os.CreateTemp(dir, fmt.Sprintf("p%d-*.img", p.Index))
	if err != nil {
		return nil, err
	}
	if _, err := io.Copy(f, io.NewSectionReader(r, p.Offset(), p.Size())); err != nil {
		f.Close()
		os.Remove(f.Name())
		return nil, err
	}
	return f, nil
}

// Inspect looks at the card (or image) behind r: its partition table and every FAT partition up to 2 GiB.
func Inspect(r io.ReaderAt, tmp string) (*Info, error) {
	in := &Info{}
	parts, err := ReadMBR(r)
	if err != nil {
		in.Why = "this card has " + err.Error() + ", so it is not an R36S card"
		return in, nil
	}
	in.Parts = parts
	first := true
	for _, p := range parts {
		if !p.IsFAT() || p.Size() > 2<<30 {
			continue
		}
		f, err := ExtractPart(r, p, tmp)
		if err != nil {
			return nil, fmt.Errorf("could not read partition %d: %w", p.Index, err)
		}
		fsys, err := OpenFAT(f)
		if err == nil {
			if first {
				in.Label = strings.TrimSpace(fsys.Label())
				first = false
				if b, err := ReadFile(fsys, "extlinux/extlinux.conf"); err == nil && strings.EqualFold(in.Label, "VIBEBOOT") &&
					strings.Contains(string(b), "VibeSDR") {
					in.Kind = VibeSDR
					in.Extlinux = string(b)
				}
			}
			in.DTBs = append(in.DTBs, walkDTBs(fsys)...)
		}
		f.Close()
		os.Remove(f.Name())
	}
	if in.Kind == VibeSDR {
		return in, nil
	}
	for i := range Boards {
		for _, d := range in.DTBs {
			for _, s := range Boards[i].StockSHA256 {
				if d.SHA256 == s {
					in.Kind, in.Board = Stock, &Boards[i]
					return in, nil
				}
			}
		}
	}
	switch {
	case len(in.DTBs) == 0:
		in.Why = "there are no R36S drivers (device trees) on this card"
	default:
		model := in.DTBs[0].Info.Model
		if model == "" {
			model = path.Base(in.DTBs[0].Path)
		}
		in.Why = fmt.Sprintf("this R36S's board (%s) isn't one VibeSDR knows yet", model)
	}
	return in, nil
}
