// VibeSDR Flasher — makes and updates VibeSDR R36S cards. docs/v12/R36S-FLASHER.md.
//
//	"VibeSDR Flasher"                  the window
//	"VibeSDR Flasher" --helper job.json  the privileged part (run as root by the window, through osascript)
package main

import (
	"embed"
	"encoding/json"
	"fmt"
	"os"

	"vibesdr.net/r36s-flasher/internal/job"

	"github.com/wailsapp/wails/v2"
	"github.com/wailsapp/wails/v2/pkg/options"
	"github.com/wailsapp/wails/v2/pkg/options/assetserver"
	"github.com/wailsapp/wails/v2/pkg/options/mac"
)

//go:embed all:frontend/dist
var assets embed.FS

func main() {
	if len(os.Args) == 3 && os.Args[1] == "--helper" {
		os.Exit(helper(os.Args[2]))
	}
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

// helper runs one job as root and reports through its progress file.
func helper(jobPath string) int {
	b, err := os.ReadFile(jobPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	var j job.Job
	if err := json.Unmarshal(b, &j); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	rep := job.FileReporter(j.ProgressPath)
	if err := job.Run(&j, rep); err != nil {
		rep(job.Event{Stage: "error", Error: err.Error()})
		return 1
	}
	return 0
}
