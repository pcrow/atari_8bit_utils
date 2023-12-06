This AUTORUN.SYS file will run a BASIC program, but first it makes sure a cartridge is inserted.  If not, it will enable BASIC or display a message if it is unable to do so.

I wanted to get it down to 124 bytes so that it will fit in one sector.  I'm not quite there.  As of right now, it's at 140 bytes, so I need to save 16 more bytes.  Options:

Save 11 bytes:
Limit to a one-character file name

Save 4 bytes:
Change "NEED BASIC" to simply "BASIC?"

Save 3 bytes:
Store messages in screen-memory character numbers (i.e., pre-subract $20)
