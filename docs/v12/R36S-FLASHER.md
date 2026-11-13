# VibeSDR R36S Flasher — spec

Stuart, 2026-11-13: **"the design is to make it as simple as possible so that a child could do it."**

One small desktop app — **Mac (signed + notarised), Windows, Linux** — that turns an R36S's own SD card into a VibeSDR
card, and later keeps it up to date, without the person ever seeing a partition, a `dd` command or a `.dtb`. It is part
of the **R36S release**, a separate release from the server and the apps. Images come **only from our repo**.

## 1. The three things it does

The window opens on three big choices. Nothing else is on the first screen.

### 1.1 MAKE A VIBESDR CARD — Simple mode (the default)

For someone holding the card that came in their R36S.

1. **Insert your R36S's SD card.** The app waits, finds it, and says what it found ("R36S card, 64 GB, ArkOS").
2. **Choose how much to back up** — two buttons, one pre-selected:
   - **Whole card** (recommended): every byte, compressed — the person can put their R36S back exactly as it was.
   - **Boot partition only**: small and quick — enough to rebuild a VibeSDR card later, not to restore the old system.
3. **Press START.** Everything else is done for them, with one progress bar and plain words:
   back up → check the backup → read this R36S's drivers from it → download the latest VibeSDR image (checked) →
   write it → put this R36S's drivers in → check the card → **"Done — put the card in your R36S and switch it on."**

No questions in between. If anything is not as expected it stops and says so in one sentence, with what to do, and
the backup is already safe on the computer.

### 1.2 ADVANCED

For someone who has not got the original card, has a damaged one, or knows what they are doing. Same flow, but the
**drivers can come from**:

- **A saved backup or image** — a whole-card image (`.img`, `.img.gz`, `.img.xz`, `.iso` as people often call them) or a
  boot-partition backup made by this app or by hand. The app opens it and reads the drivers out of it — no card needed.
- **Generic ROCKNIX drivers** — the ones in our image (ROCKNIX's, proven on Stuart's EE clone). For a card that never had
  its own, or to try when the R36S's own do not work.
- **Your own `.dtb` files** — picked from disk (and, optionally, a panel overlay). Checked that they are device trees for
  an RK3326 before anything is written.

…and **onto a blank card** (or any card), as well as the original. Advanced also lists the backups this app has made.

### 1.3 UPDATE EXISTING VIBESDR CARD

"Does what it says on the tin." Insert the card, press UPDATE:

- The app recognises a VibeSDR card (its partition layout and the version on it) and says "VibeSDR 12.0.0 beta74 →
  beta76".
- It rewrites **only the system**: the boot partition (keeping this R36S's drivers that are already on it) and the
  read-only system partition.
- It **never touches** the settings partition (`vibestate`: settings, bookmarks, the engine's data, the clock) or the
  `VIBESDR` drive (recordings, gallery, EiBi, exports, logs).
- Before it writes, it backs up the boot partition (small) so a failed update can be put back.

## 2. Rules it never breaks

- **Never write to the wrong disk.** Cards are written only on removable disks, never the computer's own; the size and
  what is on it shown before any write; a card that is neither a stock R36S card nor a VibeSDR card needs Advanced and
  an extra confirm. The **only** thing ever written to the computer's own disks is a **backup**, to a folder the person
  chooses (Stuart, 2026-11-13).
- **Back up before every write**, and check the backup reads back before the card is touched.
- **Check everything**: the downloaded image against its published checksum (and signature), the card after writing.
- **Keep the last backup** until the new card has booted (the app says so; the person deletes it when happy).
- **One sentence per problem**, in plain words, with the fix — never a raw error.

## 3. How it works (for us)

| Piece | What |
|---|---|
| The drivers | What differs between R36S clones lives in the boot area: the device tree (`.dtb`), the panel overlay, and sometimes the loader in the first 16 MiB. Our kernel, modules and Debian root are the same for all. |
| Backup | Boot only = the first 16 MiB (loader) + the FAT boot partition. Whole card = a compressed image of every byte. |
| Reading drivers from an image | Open the image file, find the FAT boot partition from its partition table, read the `.dtb`s and overlays — no mounting, no card. |
| Choosing the `.dtb` | From what each `.dtb` declares (model / compatible strings, the panel). If it cannot tell, it says so and offers Generic ROCKNIX (Advanced). |
| Writing a new card | The full VibeSDR image, then this R36S's drivers into our boot partition and `extlinux.conf` pointed at them. |
| Updating | p1 (boot, drivers carried over) and p2 (system) only. **Needs a fixed-size p2** — see §4. |
| Admin rights | Writing a card needs them on all three systems: one prompt per write. |
| Image source | Our repo only — the newest R36S image, its `.sha256` and signature. |

## 4. What has to change in the image first

**A fixed-size system partition: 3 GB** (agreed with Stuart, 2026-11-13). Today p2 is sized to its contents + 15 % + 320 MiB, so a later, bigger
system would not fit an older card, and making room would mean moving the settings and VIBESDR partitions — exactly the
risk an updater must never take with people's data. From the next image p2 is a fixed ~3 GB. Cards written before that
take one last full write (after a backup) to get the new layout; the app says so when it meets one.

## 5. Tested on

Stuart's EE clone only (the one unit we have). Other clones are "should work" until someone tries one — Generic ROCKNIX
and own `.dtb` (Advanced) are the way round a board it does not recognise.

## 6. Always up to date

Stuart, 2026-11-13: **the app always checks for updates, so the user always has the latest VibeSDR.** On every start it
checks our repo for:
- **a newer VibeSDR R36S image** — Make and Update always use the newest (downloaded once, kept, checked);
- **a newer version of the app itself** — offered before anything else is done.

## 7. Decided

- **System partition: 3 GB, fixed** (Stuart, 2026-11-13) — see §4.

## 8. Not yet decided

- The toolkit (one codebase for the three systems, small download) — ours to choose.
