	;;  basrun.asm
	;;
	;;  Copyright (c) 2023 Preston Crow
	;;  Released under the Gnu Public License version 2
	;;
	;;  Use this as an AUTORUN.SYS file to run a BASIC program
	;;
	;;  On an XL/XE system, it will enable BASIC if it's not enabled
	;;  On a 400/800/1200XL, it will report an error if BASIC is missing
	;;
	;;  The default program to run is on H for me, as I use this to boot DOS
	;;  and then run a BASIC program using H through the Atari800 emulator.
	;;
	;;  I build this with 'xa' on Linux, hence the manual binary load headers.
	;;
	;;  The start address is set to $600, but if this is an issue, there are
	;;  other options:
	;;    0100 If run at boot, then the stack only uses the top 10 bytes or so
	;;    0400 Cassette buffer and boot disk buffer, plus 480-4ff is unused
	;;    0500 BASIC uses some of this, but it's likely safe here
	;;  If I wanted to keep page 6 empty, I would put this on page 4, or maybe
	;;  put a routine on page 4 to wipe page 6 before returning at the end.
	;;
	TRAMSZ = $6
	SAVMSC = $58 		; two bytes: start of screen memory
	RAMTOP = $6A		; RAM size in pages ($A0 for 40K)
	LOMEM  = $80		; BASIC LOMEM
	COLOR1 = $2C5
	COLOR2 = $2C6
	INITAD = $2E2
	BASICF = $3F8
	CARTINIT = $BFFE	; JSR indirect here
	CARTRUN  = $BFFA 	; JMP indirect here
	PORTB  = $D301
	EDITRV = $E400		; E handler vectors (open,close,get,put,...)
	E_OPEN = $E400		; E open vector push onto stack and return
	E_CLOSE = $E402
	START = $0600
	.word $ffff
	.word START
	.word END-1
	*= START
CHKBAS	LDA 	TRAMSZ		; set to 1 if cartridge is present
	BNE	SETCLR
	;; Reduce RAMTOP to 40K unless it's already at or below
NOBAS   LDA	#$A0		; See if we can enable BASIC in XL/XE PORTB
	CMP	RAMTOP
	BCC	GR0		; Already <= 40K, do not modify
	STA	RAMTOP
	;; Set graphics 0 to adjust display below new RAMTP
	;; OS listing shows a close of E: does nothing: just re-open it
	;; Call the handler from the table to save code
GR0	JSR	DO_OP

	;; Enable BASIC in PORTB
	;; Should be 10110001 on XE
#if 0
BASENAB	LDA	PORTB		; I see $FF, so maybe it's write-only?
	AND	#$FD		; Mask off #$02 to enable BASIC
#else
BASENAB	LDA	#%10110001
#endif
	STA	PORTB
	LDX	#$00
	STX	BASICF		; Set flag to keep BASIC enabled in PORTB on reset
	;; Init BASIC
	;LDX	#$00	    	; Already zero
	STX	CARTINIT+1	; Should be ROM if PORTB enabled BASIC
	LDX	CARTINIT+1	; Page of init address must be in A0-BF range
	BNE	BASGOOD		; Branch if we found it
	;; Here we were unable to enable BASIC
	;; LDX	#FAILMSG-MESSAGES ; That's zero, which is already there from above
	JSR	CPYMSG
DIE	BEQ	DIE		; Loop here forever (CPYMSG returns from BEQ)
BASGOOD	JSR	CARTI 		; want JSR (CARTINIT), but only JMP indirect exists
	LDA	#$01
	STA	TRAMSZ	; flag cartridge present
	;; Set text (color1) to background (color2)
	;; This will be reset with the "GR.0" command later
SETCLR	LDA	COLOR2
#if 1 // Disable for debugging to see the autorun message
	STA	COLOR1
#endif
	LDX	#RUNCMD-MESSAGES
	JSR	CPYMSG
#if 0 // Debug to stop here
DEAD	BNE DEAD
#endif
	LDA	#$0D
	STA	$034A 		; IOCB 0 set to return mode for auto-input
	RTS
	;; Subroutines
DO_OP	LDA	E_OPEN+1	; Hack to call E handler open routine from the table
	PHA
	LDA	E_OPEN
	PHA
	RTS			; Jump to ($E400)+1
CARTI	JMP	(CARTINIT)
	;; Copy Message
	;; X is offset of start of message from MSG
CPYMSG	LDY	#82
CPYBYTE	LDA	MESSAGES,X
	BEQ	CPYDONE
	SEC
	SBC	#$20
	STA	(SAVMSC),Y
	INY
	INX
	BPL	CPYBYTE		; Unconditional (INX won't hit zero)
CPYDONE	RTS
MESSAGES = *	
FAILMSG	.asc "BASIC MISSING"
	.byte $00		; Flag end of message
	;; Run command should be no more than 38 bytes
	;; Doesn't need closing quote or extra space; space reserved for longer names
	;; Could add one more character and a drive number in some weird case
RUNCMD	.asc "POKE842,12:GR.0:RUN",$22,"H:RUNFILE.BAS",$22," "
	.byte $00		; Flag end of command
END	.word INITAD
	.word INITAD+1
	*= INITAD
	.word $0600
