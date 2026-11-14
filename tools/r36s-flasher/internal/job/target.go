package job

import (
	"errors"
	"os"
	"strings"

	"golang.org/x/sys/unix"
)

// openTarget opens the card. ★ A raw device is opened EXCLUSIVELY (O_EXLOCK): while we hold it, Disk Arbitration
// cannot mount the partitions we have just written (a mounted FAT gets .fseventsd written into it — the read-back
// would then differ from what we wrote). A file (tests) is opened plainly.
func openTarget(j *Job) (*os.File, error) {
	flag := os.O_RDWR
	if strings.HasPrefix(j.Target, "/dev/") {
		flag |= unix.O_EXLOCK
	}
	if j.Open != nil {
		return j.Open(j.Target, flag)
	}
	return os.OpenFile(j.Target, flag, 0)
}

// ErrCancelled is what an Open returns when the person cancels the password prompt.
var ErrCancelled = errors.New("cancelled")

// openFailure says, in one sentence, why the card could not be opened — nothing has been read or written yet.
func openFailure(err error) error {
	switch {
	case errors.Is(err, ErrCancelled):
		return Friendly{"The password was cancelled, so nothing was done. Your card has not changed."}
	case errors.Is(err, unix.EPERM) || strings.Contains(err.Error(), "not permitted"):
		return Friendly{"macOS did not let VibeSDR Flasher open the card. In System Settings › Privacy & Security › Files & Folders, turn on Removable Volumes for VibeSDR Flasher, then try again. Your card has not changed."}
	case errors.Is(err, unix.EBUSY) || errors.Is(err, unix.EAGAIN):
		return Friendly{"The card is busy — another app is using it. Close it (Finder windows on the card too) and try again. Your card has not changed."}
	}
	return Friendly{"Could not open the card (" + err.Error() + "). Your card has not changed."}
}

// syncTarget flushes the card. ★★ On a raw device (/dev/rdiskN) neither fsync nor F_FULLFSYNC applies — both answer
// "inappropriate ioctl for device" (found 2026-11-13 on an hdiutil-attached raw disk: it would have failed every real
// write at its last step). Writes to a raw device are unbuffered — they have reached the driver when WriteAt returns —
// so F_FULLFSYNC is asked where a device takes it, and otherwise the READ-BACK that follows is the proof, and
// `diskutil eject` at the end flushes the reader before it lets go. A file (tests) is fsynced.
func syncTarget(f *os.File) error {
	if !strings.HasPrefix(f.Name(), "/dev/") {
		return f.Sync()
	}
	_, _ = unix.FcntlInt(f.Fd(), unix.F_FULLFSYNC, 0)
	return nil
}
