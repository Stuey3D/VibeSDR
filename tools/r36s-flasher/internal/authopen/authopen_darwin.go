// Package authopen opens a disk device the way macOS intends an app to: the system's own /usr/libexec/authopen opens
// it after the person authorises it, and hands the open file back over a socket. Nothing runs as root.
//
// ★ Why not "do shell script … with administrator privileges" (the first build, 2026-11-14 on Stuart's Mac): macOS
// warned "Apple could not verify this script is free of malware", and then root itself was refused the card — "open
// /dev/rdisk4: operation not permitted" — because privacy control applies to raw removable media, root or not.
// authopen is the route Raspberry Pi Imager takes on the Mac.
package authopen

/*
#cgo LDFLAGS: -framework Security
#include <Security/Authorization.h>
#include <Security/AuthorizationTags.h>
#include <string.h>
#include <stdlib.h>

static OSStatus vibeAuthorise(const char *right, const char *prompt, AuthorizationRef *ref, AuthorizationExternalForm *ext) {
	OSStatus st = AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment, kAuthorizationFlagDefaults, ref);
	if (st != errAuthorizationSuccess) return st;
	AuthorizationItem item = { right, 0, NULL, 0 };
	AuthorizationRights rights = { 1, &item };
	AuthorizationItem envItem = { kAuthorizationEnvironmentPrompt, strlen(prompt), (void *)prompt, 0 };
	AuthorizationEnvironment env = { 1, &envItem };
	AuthorizationFlags fl = kAuthorizationFlagInteractionAllowed | kAuthorizationFlagExtendRights | kAuthorizationFlagPreAuthorize;
	st = AuthorizationCopyRights(*ref, &rights, &env, fl, NULL);
	if (st == errAuthorizationSuccess) st = AuthorizationMakeExternalForm(*ref, ext);
	return st;
}
static void vibeFree(AuthorizationRef ref) { AuthorizationFree(ref, kAuthorizationFlagDefaults); }
*/
import "C"

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"unsafe"

	"golang.org/x/sys/unix"
)

// ErrCancelled: the person pressed Cancel at the password prompt.
var ErrCancelled = errors.New("cancelled")

// Open opens path with the given open(2) flags after the person authorises it (one system prompt carrying prompt).
func Open(path string, flags int, prompt string) (*os.File, error) {
	name := "sys.openfile.readwrite." + path
	if flags&(unix.O_WRONLY|unix.O_RDWR) == 0 {
		name = "sys.openfile.readonly." + path
	}
	right := C.CString(name)
	defer C.free(unsafe.Pointer(right))
	cp := C.CString(prompt)
	defer C.free(unsafe.Pointer(cp))
	var ref C.AuthorizationRef
	var ext C.AuthorizationExternalForm
	st := C.vibeAuthorise(right, cp, &ref, &ext)
	if ref != nil {
		defer C.vibeFree(ref)
	}
	switch st {
	case C.errAuthorizationSuccess:
	case C.errAuthorizationCanceled:
		return nil, ErrCancelled
	default:
		return nil, fmt.Errorf("authorisation failed (%d)", int(st))
	}
	return receive(path, flags, C.GoBytes(unsafe.Pointer(&ext), C.int(unsafe.Sizeof(ext))))
}

// receive runs authopen and takes the open file it hands back. ext nil = no pre-made authorisation (authopen then
// opens with the person's own rights where it can — the test uses that on a file of its own, with no prompt).
func receive(path string, flags int, ext []byte) (*os.File, error) {
	fds, err := unix.Socketpair(unix.AF_UNIX, unix.SOCK_STREAM, 0)
	if err != nil {
		return nil, err
	}
	ours := os.NewFile(uintptr(fds[0]), "authopen-ours")
	theirs := os.NewFile(uintptr(fds[1]), "authopen-theirs")
	defer ours.Close()
	var stderr bytes.Buffer
	args := []string{"-stdoutpipe"}
	if ext != nil {
		args = append(args, "-extauth")
	}
	cmd := exec.Command("/usr/libexec/authopen", append(args, "-o", strconv.Itoa(flags), path)...)
	if ext != nil {
		cmd.Stdin = bytes.NewReader(ext)
	}
	cmd.Stdout = theirs
	cmd.Stderr = &stderr
	if err := cmd.Start(); err != nil {
		theirs.Close()
		return nil, err
	}
	theirs.Close()
	buf := make([]byte, 64)
	oob := make([]byte, unix.CmsgSpace(4))
	_, oobn, _, _, rerr := unix.Recvmsg(fds[0], buf, oob, 0)
	werr := cmd.Wait()
	if rerr == nil && oobn > 0 {
		if msgs, err := unix.ParseSocketControlMessage(oob[:oobn]); err == nil {
			for _, m := range msgs {
				if rights, err := unix.ParseUnixRights(&m); err == nil && len(rights) > 0 {
					for _, extra := range rights[1:] {
						unix.Close(extra)
					}
					return os.NewFile(uintptr(rights[0]), path), nil
				}
			}
		}
	}
	msg := bytes.TrimSpace(stderr.Bytes())
	if len(msg) == 0 && werr != nil {
		msg = []byte(werr.Error())
	}
	if len(msg) == 0 {
		msg = []byte("no file was handed back")
	}
	return nil, fmt.Errorf("%s", msg)
}
