This AUTORUN.SYS file will run a BASIC program, but first it makes sure a cartridge is inserted.  If not, it will enable BASIC or display a message if it is unable to do so.

I wanted to get it down to 124 bytes so that it will fit in one sector, and I've managed to do that.  It's now 121 bytes, so I have 3 bytes to spare.  I would like to set the text color to the background color to hide the command, but that requires 5 bytes.

Things I could do to save space:

Save upto 11 bytes:
Limit to a one-character file name

Save 2 bytes:
Change "NO BASIC" to simply "BASIC?"

Save 3 bytes:
Store messages in screen-memory character numbers (i.e., pre-subract $20)

Save 1 byte:
Use page 6 and assume next byte is zero to avoid including the last data byte

Save 5 bytes:
Don't issue GR.0 before the RUN.  It's only really necessary if I set the text color to the background color.
