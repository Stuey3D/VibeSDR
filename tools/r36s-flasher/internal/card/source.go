package card

// ADVANCED (docs/v12/R36S-FLASHER.md §1.2): drivers from a FILE instead of a card — a whole-card image or backup
// (.img, .iso, .bin; .gz, .xz or .zip around one), or a boot-partition backup made by this app or by hand — and the
// person's own device tree files. Nothing here writes to a card.

import (
	"archive/zip"
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"vibesdr.net/r36s-flasher/internal/fdt"

	"github.com/klauspost/pgzip"
	"github.com/ulikunitz/xz"
)

// ★ How far into an image the drivers can be: the FAT partitions Inspect reads end inside the first few hundred MB on
//   every R36S card seen; this cap keeps a 128 GB backup from being unpacked to find them.
const sourceSpanCap = 8 << 30

// Compression of an image file, from its name.
func compressionOf(p string) string {
	l := strings.ToLower(p)
	switch {
	case strings.HasSuffix(l, ".gz"), strings.HasSuffix(l, ".tgz"):
		return "gz"
	case strings.HasSuffix(l, ".xz"):
		return "xz"
	case strings.HasSuffix(l, ".zip"):
		return "zip"
	}
	return ""
}

// OpenStream opens an image file as one stream of the card's bytes, unpacking it if it is compressed. size is the
// unpacked size when it is known without reading it all (a plain file), else -1.
func OpenStream(p string) (r io.ReadCloser, size int64, err error) {
	f, err := os.Open(p)
	if err != nil {
		return nil, -1, err
	}
	switch compressionOf(p) {
	case "gz":
		zr, err := pgzip.NewReader(f)
		if err != nil {
			f.Close()
			return nil, -1, errors.New("it is not a gzip file, though its name says so")
		}
		return readCloser{zr, func() error { zr.Close(); return f.Close() }}, -1, nil
	case "xz":
		xr, err := xz.NewReader(f)
		if err != nil {
			f.Close()
			return nil, -1, errors.New("it is not an xz file, though its name says so")
		}
		return readCloser{xr, f.Close}, -1, nil
	case "zip":
		st, _ := f.Stat()
		zr, err := zip.NewReader(f, st.Size())
		if err != nil {
			f.Close()
			return nil, -1, errors.New("it is not a zip file, though its name says so")
		}
		// ★ the one image inside: an .img / .iso / .bin, else the largest file
		var best *zip.File
		better := func(a, b *zip.File) bool { // a over b
			if imgName(a.Name) != imgName(b.Name) {
				return imgName(a.Name)
			}
			return a.UncompressedSize64 > b.UncompressedSize64
		}
		for _, zf := range zr.File {
			if !zf.FileInfo().IsDir() && (best == nil || better(zf, best)) {
				best = zf
			}
		}
		if best == nil {
			f.Close()
			return nil, -1, errors.New("the zip file is empty")
		}
		er, err := best.Open()
		if err != nil {
			f.Close()
			return nil, -1, err
		}
		return readCloser{er, func() error { er.Close(); return f.Close() }}, int64(best.UncompressedSize64), nil
	}
	st, err := f.Stat()
	if err != nil {
		f.Close()
		return nil, -1, err
	}
	return f, st.Size(), nil
}

func imgName(n string) bool {
	l := strings.ToLower(n)
	return strings.HasSuffix(l, ".img") || strings.HasSuffix(l, ".iso") || strings.HasSuffix(l, ".bin")
}

type readCloser struct {
	io.Reader
	close func() error
}

func (r readCloser) Close() error { return r.close() }

// Source is an image file opened for reading its drivers: the start of the card it holds, up to the end of the boot
// partitions (the whole file when it is not compressed).
type Source struct {
	io.ReaderAt
	Size    int64 // bytes available through ReaderAt
	cleanup func()
}

func (s *Source) Close() {
	if s != nil && s.cleanup != nil {
		s.cleanup()
	}
}

// OpenSource opens an image or backup file for Inspect. A plain file is read where it is; a compressed one is unpacked
// into tmp only as far as its boot partitions (a boot-only backup simply ends there).
func OpenSource(p, tmp string) (*Source, error) {
	if compressionOf(p) == "" {
		f, err := os.Open(p)
		if err != nil {
			return nil, err
		}
		st, _ := f.Stat()
		return &Source{ReaderAt: f, Size: st.Size(), cleanup: func() { f.Close() }}, nil
	}
	r, _, err := OpenStream(p)
	if err != nil {
		return nil, err
	}
	defer r.Close()
	head := make([]byte, 512)
	if _, err := io.ReadFull(r, head); err != nil {
		return nil, errors.New("the file is too short to be a card image")
	}
	need := int64(512)
	if parts, err := ReadMBR(bytes.NewReader(head)); err == nil {
		for _, pt := range parts {
			if pt.IsFAT() && pt.Size() <= 2<<30 && pt.End() <= sourceSpanCap && pt.End() > need {
				need = pt.End()
			}
		}
	}
	out, err := os.CreateTemp(tmp, "source-*.img")
	if err != nil {
		return nil, err
	}
	rm := func() { out.Close(); os.Remove(out.Name()) }
	if _, err := out.Write(head); err != nil {
		rm()
		return nil, err
	}
	n, err := io.CopyN(out, r, need-512)
	if err != nil && !errors.Is(err, io.EOF) && !errors.Is(err, io.ErrUnexpectedEOF) {
		rm()
		return nil, fmt.Errorf("the file could not be unpacked: %w", err)
	}
	return &Source{ReaderAt: out, Size: 512 + n, cleanup: rm}, nil
}

// InspectFile is Inspect on an image or backup file.
func InspectFile(p, tmp string) (*Info, *Source, error) {
	src, err := OpenSource(p, tmp)
	if err != nil {
		return nil, nil, err
	}
	in, err := Inspect(src, tmp)
	if err != nil {
		src.Close()
		return nil, nil, err
	}
	return in, src, nil
}

// ── the person's own device tree files ──

// IsStockKernelDTB: a vendor device tree made for the R36S's original (Rockchip 4.4) system — it names the
// Rockchip evaluation board ("rockchip,rk3326-evb-…-linux"). VibeSDR's kernel cannot boot with one: it only says which
// of OUR device trees a board needs (Boards).
func IsStockKernelDTB(in fdt.Info) bool {
	for _, c := range in.Compatible {
		if strings.HasPrefix(c, "rockchip,rk3326-evb") || strings.HasSuffix(c, "-linux") {
			return true
		}
	}
	return false
}

// IsRK3326 — the R36S's processor. Fakes in R36S shells (RK3128, Allwinner, MediaTek…) cannot run VibeSDR at all.
func IsRK3326(in fdt.Info) bool {
	for _, c := range in.Compatible {
		if c == "rockchip,rk3326" || strings.HasPrefix(c, "rockchip,rk3326-") {
			return true
		}
	}
	return false
}

// OwnFiles are the person's own device tree and overlays, read and checked.
type OwnFiles struct {
	DTB      []byte
	DTBName  string
	Info     fdt.Info
	Board    *Board // the DTB is a stock one of a board we know: OUR device tree is used instead
	Overlays map[string][]byte
	Order    []string
}

// ReadOwn reads and checks the person's .dtb (and any .dtbo overlays). Errors are plain sentences.
func ReadOwn(dtbPath string, overlays []string) (*OwnFiles, error) {
	b, err := os.ReadFile(dtbPath)
	if err != nil {
		return nil, fmt.Errorf("%s could not be read", filepath.Base(dtbPath))
	}
	if len(b) > 4<<20 {
		return nil, fmt.Errorf("%s is too big to be a device tree", filepath.Base(dtbPath))
	}
	in, err := fdt.Parse(b)
	if err != nil {
		return nil, fmt.Errorf("%s is not a device tree (.dtb)", filepath.Base(dtbPath))
	}
	o := &OwnFiles{DTB: b, DTBName: filepath.Base(dtbPath), Info: in, Overlays: map[string][]byte{}}
	if !IsRK3326(in) {
		what := in.Model
		if what == "" && len(in.Compatible) > 0 {
			what = in.Compatible[len(in.Compatible)-1]
		}
		return nil, fmt.Errorf("%s is not for an RK3326 (it is for %s) — VibeSDR runs only on the R36S's RK3326", o.DTBName, orUnknown(what))
	}
	s := sha(b)
	for i := range Boards {
		for _, h := range Boards[i].StockSHA256 {
			if h == s {
				o.Board = &Boards[i]
			}
		}
	}
	if o.Board == nil && IsStockKernelDTB(in) {
		return nil, fmt.Errorf("%s is your R36S's original device tree, made for its old system — VibeSDR cannot start with it. Choose the generic ROCKNIX drivers, or a device tree made for ROCKNIX", o.DTBName)
	}
	for _, p := range overlays {
		ob, err := os.ReadFile(p)
		if err != nil {
			return nil, fmt.Errorf("%s could not be read", filepath.Base(p))
		}
		if !fdt.IsDTB(ob) || len(ob) > 1<<20 {
			return nil, fmt.Errorf("%s is not a device tree overlay (.dtbo)", filepath.Base(p))
		}
		n := safeName(filepath.Base(p))
		if _, dup := o.Overlays[n]; dup {
			continue
		}
		o.Overlays[n] = ob
		o.Order = append(o.Order, n)
	}
	return o, nil
}

func orUnknown(s string) string {
	if s == "" {
		return "another processor"
	}
	return s
}

// safeName keeps a file name FAT and the boot menu can both take: letters, digits, - _ . only.
func safeName(n string) string {
	var b strings.Builder
	for _, r := range n {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9', r == '-', r == '_', r == '.':
			b.WriteRune(r)
		default:
			b.WriteRune('-')
		}
	}
	return b.String()
}

// OwnDir is where the person's own files go on the new card's boot partition.
const OwnDir = "/own"

// ourOverlay is VibeSDR's own overlay (CPU steps, backlight, the OTG port's 5.2 V for the radio) — kept with every
// device tree, including the person's own.
const ourOverlay = "/overlays/vibe-r36s.dtbo"

// PlanOwn: the person's own device tree (and panel overlays) on the new card. A stock DTB of a board we know uses OUR
// device tree for it, as Simple mode does.
func PlanOwn(newExt string, o *OwnFiles) (*Plan, error) {
	ne := ParseExt(newExt)
	pl := &Plan{FDT: ne.FDT, Overlays: ne.Overlays, CopyIn: map[string][]byte{}}
	if o.Board != nil {
		pl.FDT = o.Board.VibeFDT
		pl.Summary = o.DTBName + " — the " + o.Board.Name + " (VibeSDR's own drivers for it)"
		pl.Changed = pl.FDT != ne.FDT
		return pl, nil
	}
	pl.FDT = OwnDir + "/" + safeName(o.DTBName)
	pl.CopyIn[pl.FDT] = o.DTB
	pl.Overlays = nil
	for _, n := range o.Order {
		p := OwnDir + "/overlays/" + n
		pl.CopyIn[p] = o.Overlays[n]
		pl.Overlays = append(pl.Overlays, p)
	}
	if contains(ne.Overlays, ourOverlay) {
		pl.Overlays = append(pl.Overlays, ourOverlay)
	}
	model := o.Info.Model
	if model == "" {
		model = "your device tree"
	}
	pl.Summary = o.DTBName + " (" + model + ")"
	if len(o.Order) > 0 {
		pl.Summary += " with " + strings.Join(o.Order, ", ")
	}
	pl.Changed = true
	return pl, nil
}

// ── backups this app has made ──

// Backup is one backup in the person's backups folder.
type Backup struct {
	Path    string `json:"path"`
	Name    string `json:"name"`   // the folder's name ("2026-11-15 1031 Built In SDXC Reader")
	Kind    string `json:"kind"`   // "whole" | "boot"
	Bytes   int64  `json:"bytes"`  // unpacked size, from its README
	Written string `json:"written"` // the VibeSDR image written after it ("" for none)
	When    string `json:"when"`
}

// ListBackups lists the backups in dir (…/Documents/VibeSDR Backups), newest first.
func ListBackups(dir string) []Backup {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var out []Backup
	for _, e := range ents {
		if !e.IsDir() {
			continue
		}
		d := filepath.Join(dir, e.Name())
		files, _ := os.ReadDir(d)
		for _, f := range files {
			if !strings.HasSuffix(f.Name(), ".img.gz") {
				continue
			}
			b := Backup{Path: filepath.Join(d, f.Name()), Name: e.Name(), Kind: "boot"}
			if note, err := os.ReadFile(filepath.Join(d, "README.txt")); err == nil {
				s := string(note)
				if strings.Contains(s, "the WHOLE card") {
					b.Kind = "whole"
				}
				for _, ln := range strings.Split(s, "\n") {
					var n int64
					if _, err := fmt.Sscanf(ln, "Size: %d bytes", &n); err == nil {
						b.Bytes = n
					}
					if v, ok := strings.CutPrefix(ln, "VibeSDR image written afterwards: "); ok {
						b.Written = strings.TrimSpace(v)
					}
					if v, ok := strings.CutPrefix(ln, "VibeSDR Flasher backup — "); ok {
						b.When = strings.TrimSpace(v)
					}
				}
			}
			out = append(out, b)
		}
	}
	sort.Slice(out, func(i, k int) bool { return out[i].Name > out[k].Name })
	return out
}

// SHA256Beside is the checksum written beside a backup (<file>.sha256, "<hex>  <name>"), or "".
func SHA256Beside(p string) string {
	b, err := os.ReadFile(p + ".sha256")
	if err != nil {
		return ""
	}
	f := strings.Fields(string(b))
	if len(f) == 0 || len(f[0]) != 64 {
		return ""
	}
	return strings.ToLower(f[0])
}

