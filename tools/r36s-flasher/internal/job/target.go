package job

import (
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
	return os.OpenFile(j.Target, flag, 0)
}

// syncTarget flushes the card all the way to the media (F_FULLFSYNC on a device: plain fsync on macOS only reaches
// the drive's cache).
func syncTarget(f *os.File) error {
	if strings.HasPrefix(f.Name(), "/dev/") {
		if _, err := unix.FcntlInt(f.Fd(), unix.F_FULLFSYNC, 0); err == nil {
			return nil
		}
	}
	return f.Sync()
}
