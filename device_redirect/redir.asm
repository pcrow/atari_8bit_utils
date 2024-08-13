ICDNOZ	=	$0021
ICBALZ	=	$24
ICBAHZ	=	$25
ICIDNO	=	$2E
INITAD	=	$02E2
HATABS	=	$031A
ICDNO	=	$0341
ICBAL	=	$0344
ICBAH	=	$0345
CIO	=	$E456
COMENT	=	$E63D
GOHAND	=	$E689
DEVSRC	=	$E69E
; The device handler table only needs the open
	.word $ffff,INIT,INITE-1	;Atari binload header
	*= $0400
; On XL/XE systems, the handler address table has one less entry
;MAXDEV	=	$21		; HATABS + MAXDEV points at last usable entry
MAXDEV	=	$21-3		; HATABS + MAXDEV points at last usable entry
INIT
	LDA	#$FF		; 'C or whatever device
	TAY			; save in Y
;HATS FINDS DEVICE NAMED IN A OR UNUSED, RETURNS ITS INDEX IN X

HATS	LDX	#MAXDEV
HATSL	CMP	HATABS,X
	BEQ	HATF		;found match; replace it
	LDA	HATABS,X
	BEQ	HATF		;found empty entry; use it
	TYA			;restore A
	DEX
	DEX
	DEX
	BPL	HATSL		;check next entry
	RTS	;TABLE IS FULL
	; Now put the table entry here
HATF	TYA
	STA	HATABS,X  PUT NAME IN TABLE
	LDA	#DRH&$FF
	STA	HATABS+1,X
	LDA	#DRH/256
	STA	HATABS+2,X
STF	RTS
INITE	=*
	.WORD $FFFF,INITAD,INITAD+1,INIT ; do the init
	.WORD $ffff,DRH,END-1		; Atari binload header
	*= $0400
DRH	.WORD	EOPEN-1
EOPEN	;LDX	ICIDNO	;GET IOCB # ; testing shows that X is already set
	LDA	#RDIR&255
	;STA	ICBALZ
	STA	ICBAL,X	;FOR FMS
	LDA	#RDIR/256
	;STA	ICBAHZ
	STA	ICBAH,X	;FOR FMS
	JMP	CIO
	; The following breaks with the XL operating system
	; The addresses for the three calls changed
	; Why not just call CIOV now?
	;JSR	DEVSRC	;GO FIND PHYS DEVICE HID/DNO
	;BCS	HERRX	;IF ERRORS
	;LDA	ICDNOZ
	;STA	ICDNO,X	;FOR FMS
	;JSR	COMENT	;GET PHYS DEV HDLR OPEN ENTRY
	;BCS	HERRX
	;JMP	GOHAND	;GO DO REAL OPEN
HERRX	;RTS
RDIR	= *	
;RDIR	.BYTE	"Dn:ABCDEFGH.IJK",0
END	= *
