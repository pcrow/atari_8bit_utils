This AUTORUN.SYS file will run a BASIC program, but first it makes sure a cartridge is inserted.  If not, it will enable BASIC or display a message if it is unable to do so.

With lots of help from AtariAge forum members, I managed to get it down to 124 bytes to fit in one sector.

Things I could do to save even space:

Save upto 11 bytes:
Limit to a one-character file name

Save 2 bytes:
Change "NO BASIC" to simply "BASIC?"

Save 3 bytes:
Store messages in screen-memory character numbers (i.e., pre-subract $20)

Save 1 byte:
Use page 6 and assume next byte is zero to avoid including the last data byte

Save 8 bytes:
Don't issue GR.0 before the RUN, which also requires not setting the command text color to match the backgroun.
