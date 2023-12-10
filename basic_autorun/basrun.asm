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
	;;  Start address options:
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
GR0	JSR	DO_GR0

CHKBAS	LDX 	TRAMSZ		; set to 1 if cartridge is present
	BNE	BASREADY

	;; Enable BASIC in PORTB
	;; Should be 1x11xx01 on XE, second xx should be 0 on 1200XL for LEDs off
BASENAB	LDA	#%10110001	; Save 3 bytes by not read/and old value
	STA	PORTB
	;; Note X is zero from TRAMSZ check above
	STX	BASICF		; Set flag to keep BASIC enabled in PORTB on reset
	;; Init BASIC
	;; STX	CARTINIT+1	; last character on screen if RAM on 400/800/1200XL without cart
	;; Write is irrelevant unless last character on screen was inverse @ or inverse A
	LDA	CARTINIT+1	; Page of init address must be in A0-BF range
	;; BASIC is enabled on all but 400/800/1200XL
	;; If 1200XL without BASIC or 400/800 with 48K and no cart, will read zero
	;; If BASIC was enabled or cart is present, will read Ax or Bx.
	;; Check binary -- 101x xxxx
	;; If 400/800 with < 48K and no BASIC, value may be unpredictable
	;; Small chance this check will fail and it will crash.
	;; Note that emulators may return $FF; real hardware reflects busses.
	AND	#%11100000	; A is 1010, B is 1011
	CMP	#%10100000
	BNE	BASBAD		; Write failed, but read $FF because <48K of RAM
BASGOOD	JSR	CARTI 		; want JSR (CARTINIT), but only JMP indirect exists
	INC	TRAMSZ		; flag cartridge present
BASREADY = *
	;; Set text (color1) to background (color2)
	;; This will be reset with the "GR.0" command later
	;; Since this is always standard GR.0, COLOR2 is $94 and COLOR1 is $CA
	;; Instead of loading the target value, we are lucky that we can just do
	;; a shift to get it
	ASL	COLOR1		; $ca << 1 is $0194 and $94 happens to be color0 for GR.0

	;; Copy the BASIC command to the screen
	LDX	#RUNCMD-MESSAGES
BASBAD = *
CPYMSG	LDY	#82
CPYBYTE	LDA	MESSAGES,X
	BMI	STO_CH
	SEC
	SBC	#$20
	STA	(SAVMSC),Y
	INY
	INX

	CPX	#RUNCMD-FAILMSG
DIE	BEQ	DIE		; Loop here forever if FAILMSG displayed
	BNE	CPYBYTE		; Unconditional

	;; Subroutines
DO_GR0	LDA	E_OPEN+1	; Hack to call E handler open routine from the table
	PHA
	LDA	E_OPEN
	PHA
	;; The key press will be overwritten at the end, so save an RTS and fall through
STO_CH	STA	CH		; Active key press (inject SHIFT+RETURN to execute command)
	RTS
CARTI	JMP	(CARTINIT)
	;; Copy Message
	;; X is offset of start of message from MESSAGES
MESSAGES = *	
FAILMSG	.asc "NO BASIC"	     ; End detected by comparison
	;; Run command should be no more than 38 bytes
	;; Doesn't need closing quote; space reserved for 8.3 names
RUNCMD	.asc "GR.0:RUN",$22,"D:AUTORUN.BAS",$22
	.byte $8C		; Flag end of command ($8C is also keycode for Shift+Return)
END	.word INITAD
	.word INITAD+1
	*= INITAD
	.word START
