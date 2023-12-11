There are tons of AUTORUN.SYS files that will run a BASIC program, but this one has a number of nice features that you probably won't find elsewhere:

 * Fits in one sector (124 bytes max)
 * BASIC is enabled on XL/XE if not already active
 * An error is displayed if BASIC is missing on 400/800/1200XL
 * Space in the command for any 8.3 filename
 * Text is in ATASCII in the binary for easy editing
 * The command text does not flash on the screen
 * Runs in the cassette buffer, leaving Page 6 clean

The only downside is there is one keypress click.  I could turn that off with three bytes (decrement NOCLK $2DB), I couldn't turn it back on without a poke in the BASIC command, making it just as bad as using auto-return mode.  I can live with that.

Detecting BASIC is tricky.  If it's not a 400/800/1200XL, then we will enable it with the write to Port B.  But if it's a 400/800, there might be less than 48K, and we can't be sure what will be returned if nothing responds to the memory reqeust.  I'm told it could be from a data buffer or address reflection, and the 400 and 800 behave differently.  So I verify the Port B write, which will catch the 400/800 cases where Port B is configured for input.  For the 1200XL, I write zero to the upper byte of the cartridge init address which must be in the A0-BF range, and then I read it to see if it's non-zero.  These checks use a lot of bytes, but it should be perfect in all cases now.

Thanks to lots of help from AtariAge forum members.  I wouldn't have gotten this down to 124 bytes or less on my own.

It's currently 124 bytes.

Things I could do to save even space:

Save upto 11 bytes:
Limit to a one-character file name

Save 2 bytes:
Change "NO BASIC" to simply "BASIC?"

Save 3 bytes:
Store messages in screen-memory character numbers (i.e., pre-subract $20)

Save 8 bytes:
Don't issue GR.0 before the RUN (5 bytes), which also requires not setting the command text color to match the background (3 bytes).

Save 3 bytes:
Assume the last byte of screen memory was already zero on 1200XL so no need to write a zero before seeing if BASIC loaded in that address.  (This check isn't reached on 400/800, and BASIC will already be loaded on all other XL/XE systems.)
