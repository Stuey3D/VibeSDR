package card

import (
	"os"
	"strings"
	"testing"

	"vibesdr.net/r36s-flasher/internal/fdt"
)

func TestStockDTBParses(t *testing.T) {
	b, err := os.ReadFile("testdata/rf3536k3ka.dtb")
	if err != nil {
		t.Fatal(err)
	}
	in, err := fdt.Parse(b)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("stock EE clone DTB: model %q compatible %v", in.Model, in.Compatible)
	if sha(b) != Boards[0].StockSHA256[0] {
		t.Fatalf("the known board's hash is not the testdata DTB's")
	}
	if len(in.Compatible) == 0 {
		t.Fatal("no compatible strings")
	}
}

func TestParseRewriteExt(t *testing.T) {
	ext := "TIMEOUT 0\nDEFAULT VibeSDR\nLABEL VibeSDR\n  LINUX /KERNEL\n  FDT /rk3326-gameconsole-eeclone.dtb\n" +
		"  FDTOVERLAYS /overlays/mipi-panel.dtbo /overlays/vibe-r36s.dtbo\n  APPEND root=PARTUUID=abc-02 ro quiet\n"
	e := ParseExt(ext)
	if e.FDT != "/rk3326-gameconsole-eeclone.dtb" || len(e.Overlays) != 2 || e.Append[0] != "root=PARTUUID=abc-02" {
		t.Fatalf("parse: %+v", e)
	}
	out := RewriteExt(ext, &Plan{FDT: "/x.dtb", Overlays: []string{"/overlays/a.dtbo"}, AddArgs: []string{"vibe.debuglog"}})
	if !strings.Contains(out, "  FDT /x.dtb\n") || !strings.Contains(out, "  FDTOVERLAYS /overlays/a.dtbo\n") ||
		!strings.Contains(out, "ro quiet vibe.debuglog\n") || !strings.Contains(out, "  LINUX /KERNEL\n") {
		t.Fatalf("rewrite:\n%s", out)
	}
}
