VIBESDR — the files on your VibeSDR handheld
=============================================

This drive is the part of the handheld's memory card that is yours. Take the card out
of the handheld (power it off first), put it in a computer, and this drive appears.

  Recordings\        Audio and IQ recordings, named "YYYY-MM-DD HHMM - <kHz> kHz <MODE>",
                     in UTC.
  Gallery\           Pictures and logs from the decoders, one folder each
                     (SSTV, WEFAX, RTTY, NAVTEX, FT8 ...).
  import\            Put a bookmark backup here to load it into the handheld. It is read
                     when the handheld starts (or from IMPORT in the menu), then moved to
                     import\done\ with a .txt saying what happened.
  EiBi\              The shortwave station schedule that SEARCH looks in. The handheld has no
                     internet, so fetch it yourself: on a computer go to eibispace.de and
                     download sked-b26.csv - the CSV, not the freq-b26.txt / time-b26.txt
                     versions (those are printable lists the handheld cannot read). B = winter,
                     A = summer: take the newest. Put it here, then press IMPORT EIBI on the
                     handheld (frequency pad -> BOOKMARKS). It is kept through restarts; the
                     file here stays as your copy. A new one comes out twice a year - repeat
                     with the new file.
  export\            Files the handheld writes for you to take away.
  Diagnostic logs\   EXPORT DIAGNOSTICS in the menu writes a report here, named by UTC
                     time. Send it with a bug report; personal details are removed.

!! NEVER FORMAT THE OTHER DRIVE !!
---------------------------------
The card has other parts that hold the handheld's system. Windows may show one as a
small drive (VIBEBOOT) or offer to "format" a drive it cannot read. Say NO / Cancel:
formatting any part other than this VIBESDR drive stops the handheld from starting,
and the card has to be written again from the VibeSDR image.

Only change files on this VIBESDR drive. Always "Eject" the card before pulling it out.

https://vibesdr.net
