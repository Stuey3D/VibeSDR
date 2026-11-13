// Package release finds the newest VibeSDR R36S image: in a LOCAL TEST build the newest vibesdr-r36s-*.img.gz in
// ~/Desktop/VibeSDR-Flasher/ (verified against the .sha256 beside it), otherwise the newest GitHub release of
// Stuey3D/VibeSDR whose tag starts "r36s-" (downloaded once into the cache and verified).
package release

import (
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

// LocalTest — ★ this build takes its image from the Desktop, not GitHub (Stuart, 2026-11-13: nothing for the R36S goes
// to GitHub until he has proven the flasher on his own card). Flip to false for the published build.
const LocalTest = true

// Image is a VibeSDR R36S image ready to write.
type Image struct {
	Version string // e.g. "12.0.0-beta75+handheld72-20261114"
	Path    string // the .img.gz on this Mac
	SHA256  string // of the .img.gz, as published
	Source  string // "Desktop" or "GitHub"
}

var nameRE = regexp.MustCompile(`^vibesdr-r36s-(.+)\.img\.gz$`)

// LocalDir is where a local test build looks.
func LocalDir() string {
	h, _ := os.UserHomeDir()
	return filepath.Join(h, "Desktop", "VibeSDR-Flasher")
}

// CacheDir is where downloads are kept.
func CacheDir() string {
	h, _ := os.UserHomeDir()
	return filepath.Join(h, "Library", "Caches", "VibeSDR Flasher")
}

// readSHA reads a sha256sum-format file: "<hex>  <name>".
func readSHA(p string) (string, error) {
	f, err := os.Open(p)
	if err != nil {
		return "", err
	}
	defer f.Close()
	s := bufio.NewScanner(f)
	if s.Scan() {
		if fl := strings.Fields(s.Text()); len(fl) > 0 && len(fl[0]) == 64 {
			return strings.ToLower(fl[0]), nil
		}
	}
	return "", errors.New("no checksum in " + filepath.Base(p))
}

// FileSHA256 hashes a file.
func FileSHA256(p string, progress func(done, total int64)) (string, error) {
	f, err := os.Open(p)
	if err != nil {
		return "", err
	}
	defer f.Close()
	st, _ := f.Stat()
	h := sha256.New()
	buf := make([]byte, 4<<20)
	var done int64
	for {
		n, err := f.Read(buf)
		if n > 0 {
			h.Write(buf[:n])
			done += int64(n)
			if progress != nil {
				progress(done, st.Size())
			}
		}
		if err == io.EOF {
			break
		}
		if err != nil {
			return "", err
		}
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// FindLocal returns the newest image in dir (by name order of its version, then file time). An override path (debug:
// VIBE_FLASHER_IMAGE) is taken as given.
func FindLocal(dir string) (*Image, error) {
	if p := os.Getenv("VIBE_FLASHER_IMAGE"); p != "" {
		return localImage(p)
	}
	ents, err := os.ReadDir(dir)
	if err != nil {
		return nil, fmt.Errorf("no folder %s — put the VibeSDR image there", dir)
	}
	type cand struct {
		name string
		mod  time.Time
	}
	var cs []cand
	for _, e := range ents {
		if nameRE.MatchString(e.Name()) {
			if fi, err := e.Info(); err == nil {
				cs = append(cs, cand{e.Name(), fi.ModTime()})
			}
		}
	}
	if len(cs) == 0 {
		return nil, fmt.Errorf("no VibeSDR image (vibesdr-r36s-….img.gz) in %s", dir)
	}
	sort.Slice(cs, func(i, j int) bool { return cs[i].mod.After(cs[j].mod) })
	return localImage(filepath.Join(dir, cs[0].name))
}

func localImage(p string) (*Image, error) {
	m := nameRE.FindStringSubmatch(filepath.Base(p))
	if m == nil {
		return nil, fmt.Errorf("%s is not a VibeSDR R36S image name", filepath.Base(p))
	}
	sum, err := readSHA(p + ".sha256")
	if err != nil {
		return nil, fmt.Errorf("the checksum file %s.sha256 is missing beside the image", filepath.Base(p))
	}
	return &Image{Version: m[1], Path: p, SHA256: sum, Source: "Desktop"}, nil
}

// ── GitHub (the published build) ─────────────────────────────────────────────────────────────────────────────────

const apiReleases = "https://api.github.com/repos/Stuey3D/VibeSDR/releases?per_page=100"

type ghAsset struct {
	Name string `json:"name"`
	URL  string `json:"browser_download_url"`
	Size int64  `json:"size"`
}
type ghRelease struct {
	Tag       string    `json:"tag_name"`
	Draft     bool      `json:"draft"`
	Published time.Time `json:"published_at"`
	Assets    []ghAsset `json:"assets"`
}

// Remote describes the newest published image (not downloaded yet).
type Remote struct {
	Version, ImageURL, SHAURL string
	Size                     int64
}

// FindRemote asks GitHub for the newest release tagged "r36s-" (not "r36s-flasher-").
func FindRemote(client *http.Client) (*Remote, error) {
	req, _ := http.NewRequest("GET", apiReleases, nil)
	req.Header.Set("Accept", "application/vnd.github+json")
	req.Header.Set("User-Agent", "VibeSDR-Flasher")
	resp, err := client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("could not reach the VibeSDR releases: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("the VibeSDR releases answered %s", resp.Status)
	}
	var rs []ghRelease
	if err := json.NewDecoder(resp.Body).Decode(&rs); err != nil {
		return nil, err
	}
	sort.Slice(rs, func(i, j int) bool { return rs[i].Published.After(rs[j].Published) })
	for _, r := range rs {
		if r.Draft || !strings.HasPrefix(r.Tag, "r36s-") || strings.HasPrefix(r.Tag, "r36s-flasher-") {
			continue
		}
		var out Remote
		for _, a := range r.Assets {
			if m := nameRE.FindStringSubmatch(a.Name); m != nil {
				out.Version, out.ImageURL, out.Size = m[1], a.URL, a.Size
			}
		}
		for _, a := range r.Assets {
			if out.Version != "" && a.Name == "vibesdr-r36s-"+out.Version+".img.gz.sha256" {
				out.SHAURL = a.URL
			}
		}
		if out.ImageURL != "" && out.SHAURL != "" {
			return &out, nil
		}
	}
	return nil, errors.New("no VibeSDR R36S image has been published yet")
}

// Download fetches a remote image into the cache (kept between runs) and verifies it.
func Download(client *http.Client, r *Remote, progress func(done, total int64)) (*Image, error) {
	dir := CacheDir()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	name := "vibesdr-r36s-" + r.Version + ".img.gz"
	dst := filepath.Join(dir, name)
	shaResp, err := client.Get(r.SHAURL)
	if err != nil {
		return nil, err
	}
	shaBody, _ := io.ReadAll(io.LimitReader(shaResp.Body, 4096))
	shaResp.Body.Close()
	fl := strings.Fields(string(shaBody))
	if len(fl) == 0 || len(fl[0]) != 64 {
		return nil, errors.New("the published checksum is unreadable")
	}
	want := strings.ToLower(fl[0])
	if got, err := FileSHA256(dst, nil); err == nil && got == want {
		return &Image{Version: r.Version, Path: dst, SHA256: want, Source: "GitHub"}, nil
	}
	resp, err := client.Get(r.ImageURL)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	tmp := dst + ".part"
	f, err := os.Create(tmp)
	if err != nil {
		return nil, err
	}
	h := sha256.New()
	var done int64
	buf := make([]byte, 1<<20)
	for {
		n, rerr := resp.Body.Read(buf)
		if n > 0 {
			f.Write(buf[:n])
			h.Write(buf[:n])
			done += int64(n)
			if progress != nil {
				progress(done, r.Size)
			}
		}
		if rerr == io.EOF {
			break
		}
		if rerr != nil {
			f.Close()
			return nil, rerr
		}
	}
	f.Close()
	if hex.EncodeToString(h.Sum(nil)) != want {
		os.Remove(tmp)
		return nil, errors.New("the downloaded image did not match its checksum — try again")
	}
	if err := os.Rename(tmp, dst); err != nil {
		return nil, err
	}
	return &Image{Version: r.Version, Path: dst, SHA256: want, Source: "GitHub"}, nil
}
