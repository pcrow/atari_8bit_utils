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
	;;    0600 Everything uses page 6 for things like this, but nice to have it clean
	;;
	TRAMSZ = $6
	SAVMSC = $58 		; two bytes: start of screen memory
	RAMTOP = $6A		; RAM size in pages ($A0 for 40K)
	LOMEM  = $80		; BASIC LOMEM
	COLOR1 = $2C5
	COLOR2 = $2C6
	INITAD = $2E2
	CH     = $2FC
	BASICF = $3F8
	CARTINIT = $BFFE	; JSR indirect here
	CARTRUN  = $BFFA 	; JMP indirect here
	PORTB  = $D301
	EDITRV = $E400		; E handler vectors (open,close,get,put,...)
	E_OPEN = $E400		; E open vector push onto stack and return
	E_CLOSE = $E402
	START = $0400
	.word $ffff
	.word START
	.word END-1
	*= START
	;; Reduce RAMTOP to 40K unless it's already at or below
	;; Does nothing if BASIC already loaded
NOBAS   LDA	#$A0		; See if we can enable BASIC in XL/XE PORTB
	CMP	RAMTOP
	BCS	GR0		; Already <= 40K, do not modify
	STA	RAMTOP
	;; Set graphics 0 to adjust display below new RAMTP
	;; OS listing shows a close of E: does nothing: just re-open it
	;; Call the handler from the table to save code
	;; Also guarantees no garbage on the screen and resets the cursor position
GR0	JSR	DO_OP

#if 0 // Just always try to enable BASIC
CHKBAS	LDA 	TRAMSZ		; set to 1 if cartridge is present
	BNE	BASREADY
#endif

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
	INC	TRAMSZ	; flag cartridge present
BASREADY = *
	;; Set text (color1) to background (color2)
	;; This will be reset with the "GR.0" command later
#if 0 // Disable to save space; GR.0 will wipe the text quickly
	LDA	#$94		; default COLOR2 as set by GR.0 above
	STA	COLOR1
#endif
	LDX	#RUNCMD-MESSAGES
	JSR	CPYMSG
	LDA	#$0C		; key code for RETURN
#if 0 // Debug to stop here
	RTS
#endif
	STA	CH		; Active key
	RTS
	;; Subroutines
DO_OP	LDA	E_OPEN+1	; Hack to call E handler open routine from the table
	PHA
	LDA	E_OPEN
	PHA
DO_RTS	RTS			; Jump to ($E400)+1
CARTI	JMP	(CARTINIT)
	;; Copy Message
	;; X is offset of start of message from MESSAGES
CPYMSG	LDY	#82
CPYBYTE	LDA	MESSAGES,X
	BEQ	DO_RTS
	SEC
	SBC	#$20
	STA	(SAVMSC),Y
	INY
	INX
	BPL	CPYBYTE		; Unconditional (INX won't hit zero)
MESSAGES = *	
FAILMSG	.asc "NEED BASIC"
	.byte $00		; Flag end of message
	;; Run command should be no more than 38 bytes
	;; Doesn't need closing quote; space reserved for 8.3 names
RUNCMD	.asc "GR.0:RUN",$22,"H:RUNFILE.BAS",$22
	.byte $00		; Flag end of command
END	.word INITAD
	.word INITAD+1
	*= INITAD
	.word START
