// VibeSDR Flasher — makes and updates VibeSDR R36S cards. docs/v12/R36S-FLASHER.md.
//
// One process, run as the person: the card is opened through macOS's authopen (internal/authopen) — nothing runs as root.
package main

import (
	"embed"
	"fmt"
	"os"


	"github.com/wailsapp/wails/v2"
	"github.com/wailsapp/wails/v2/pkg/options"
	"github.com/wailsapp/wails/v2/pkg/options/assetserver"
	"github.com/wailsapp/wails/v2/pkg/options/mac"
)

//go:embed all:frontend/dist
var assets embed.FS

func main() {
	app := NewApp()
	err := wails.Run(&options.App{
		Title:            "VibeSDR Flasher",
		Width:            760,
		Height:           620,
		MinWidth:         640,
		MinHeight:        560,
		AssetServer:      &assetserver.Options{Assets: assets},
		BackgroundColour: &options.RGBA{R: 14, G: 12, B: 10, A: 1},
		OnStartup:        app.startup,
		Bind:             []interface{}{app},
		Mac: &mac.Options{
			TitleBar:   mac.TitleBarHiddenInset(),
			Appearance: mac.NSAppearanceNameDarkAqua,
			About:      &mac.AboutInfo{Title: "VibeSDR Flasher", Message: "Makes VibeSDR cards for the R36S. " + Version},
		},
	})
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
