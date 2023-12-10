There are tons of AUTORUN.SYS files that will run a BASIC program, but this one has a number of nice features that you probably won't find elsewhere:

 * Fits in one sector (124 bytes max)
 * BASIC is enabled on XL/XE if not already active
 * An error is displayed if BASIC is missing on 400/800/1200XL
 * Space in the command for any 8.3 filename
 * Text is in ATASCII in the binary for easy editing
 * The command text does not flash on the screen
 * Runs in the cassette buffer, leaving Page 6 clean

The only downside is there is one keypress click.  I could turn that off with three bytes (decrement NOCLK $2DB), I couldn't turn it back on without a poke in the BASIC command, making it just as bad as using auto-return mode.  I can live with that.

I am concerned about behavior on a 400/800 with < 48K when it reads memory.  My understanding is that a 600XL will return all 1s on such reads, but it's not as clear on a 400 or 800.

Thanks to lots of help from AtariAge forum members.  I wouldn't have gotten this down to 124 bytes or less on my own.

It's currently 121 bytes.

Things I could do to save even space:

Save upto 11 bytes:
Limit to a one-character file name

Save 2 bytes:
Change "NO BASIC" to simply "BASIC?"

Save 3 bytes:
Store messages in screen-memory character numbers (i.e., pre-subract $20)

Save 8 bytes:
Don't issue GR.0 before the RUN (5 bytes), which also requires not setting the command text color to match the background (3 bytes).
