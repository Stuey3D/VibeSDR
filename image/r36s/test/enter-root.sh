#!/bin/sh
# enter-root.sh <command…> — ON THE R36S under ROCKNIX: run a command inside the IMAGE's own Debian root, unpacked at
# /storage/vibe-r36s-root, in its own mount namespace via pivot_root (not chroot), so WebKit's bubblewrap sandbox works
# with the sandbox ON. The host's /dev, /proc, /sys, /run are bound in. Nothing on the card's boot area is touched.
R=${VIBE_TEST_ROOT:-/storage/vibe-r36s-root}
exec unshare -m sh -c '
  set -e
  R='"$R"'
  mount --make-rprivate /
  mount --bind $R $R
  for m in dev dev/pts proc sys run; do mount --bind /$m $R/$m; done
  mount -t tmpfs tmpfs $R/tmp
  mkdir -p $R/.oldroot
  cd $R
  ./lib/ld-linux-aarch64.so.1 --library-path ./lib/aarch64-linux-gnu:./usr/lib/aarch64-linux-gnu ./usr/sbin/pivot_root . .oldroot
  cd /
  exec /usr/bin/env -i HOME=/root PATH=/usr/sbin:/usr/bin:/sbin:/bin TERM=xterm LANG=C.UTF-8 "$@"
' sh "$@"
