10    .OPT NOOBJ
20 ;System equates used
30 BOOT   = $09
40 FMSZPG = $43
50 SDMCTL = $022F
60 SDLSTL = $0230
70 SDLSTH = $0231
80 COLDST = $0244
90 GLBABS = $02E0
0100 DUNIT  = $0301
0110 DSTATS = $0303
0120 DBUFHI = $0305
0130 DTIMLO = $0306
0140 DBYTLO = $0308
0150 DAUX1  = $030A
0160 DAUX2  = $030B
0170 KEYBDV = $E420
0180 ;End of system equates
0190 ;Zero-page equates
0200 Z1     = $A0
0210 DISPL  = $B2
0220 DISPH  = $B3
0230 SECLO  = $B1
0240 NUM    = $B0
0250 TABL   = $C0
0260 TABH   = $E0
0270 ;End of zero-page equates
0280        *=  $0700
0290        .BYTE $00 ;dos flag
0300        .BYTE $03 ;read 3 sectors
0310        .BYTE $00,$07 ;at $0700
0320        .BYTE $77,$E4
0330 ; init:  jsr $E477 (coldstart)
0340        LDY #$00
0350        STY COLDST
0360 ; normal reset
0370        INY
0380        STY BOOT
0390 ; signal disk boot
0400        NOP
0410        NOP
0420        NOP
0430        NOP
0440        NOP
0450        NOP
0460 ; drive #1--already set!!!
0470        LDA #DLST & $00FF ;#$4A
0480        STA SDLSTL
0490        LDA #DLST / $0100 ;#$08
0500        STA SDLSTH
0510 ; display list at $084A
0520        LDA #LINE1 & $00FF ;#$7C
0530        STA DISPL
0540        LDA #LINE1 / $0100 ;#$08
0550        STA DISPH
0560        LDA #$69  ;361 low order
0570        STA SECLO
0580 NDIRS  LDA SECLO
0590        STA DAUX1
0600        LDA #$01  ;sechi
0610        STA DAUX2
0620        JSR DIREAD
0630        INC SECLO
0640        DEX
0650 NDIRE  LDA BUFFER,X
0660        BEQ DIRDONE
0670        BMI NENTRY
0680        CMP #$01
0690        BEQ NENTRY
0700        INC NUM
0710        LDY NUM
0720        LDA BUFFER+3,X
0730 ;      STA TABL,Y
0740        .BYTE $99,TABL,$00
0750        LDA BUFFER+4,X
0760 ;      STA TABH,Y
0770        .BYTE $99,TABH,$00
0780        TYA
0790        CLC
0800        ADC #$A0 ;choice code
0810        LDY #$03
0820        STA (DISPL),Y ;disp char
0830        INY
0840        LDA #$8E ;"."
0850        STA (DISPL),Y
0860        INY ;blank space
0870 DCHAR  INY ;next space
0880        LDA BUFFER+5,X ;filename
0890        INX
0900        SEC
0910        SBC #$20
0920        STA (DISPL),Y
0930        CPY #$10
0940        BNE DCHAR
0950        CLC
0960        LDA DISPL ;$00B2
0970        ADC #$14 ;next line
0980        STA DISPL
0990        BCC LABEL
1000        INC DISPH
1010 LABEL  LDA NUM
1020        CMP #$14 ;too many files!
1030        BEQ DIRDONE ;$078E
1040 NENTRY TXA
1050        AND #$F0
1060        CLC
1070        ADC #$10 ;next entry
1080        TAX
1090        ASL A
1100        BCC NDIRE ;new entry
1110        BCS NDIRS ;new sector
1120 ;*****************************
1130 DIRDONE JSR GETKEY
1140        SEC
1150        SBC #$40
1160        CMP NUM
1170        BEQ L20
1180        BCS DIRDONE ;BAD KEY
1190 L20    TAX
1200        INC L21,X ;set line gr.2
1210        LDA TABL,X
1220        STA DAUX1
1230        LDA TABH ,X
1240        STA DAUX2
1250        JSR READ
1260        DEX ;ldx#$00
1270 GETMEM  JSR L23   ;$07F9
1280        STA FMSZPG
1290        JSR L23   ;$07F9
1300        STA FMSZPG+1
1310        AND FMSZPG
1320        CMP #$FF
1330        BEQ GETMEM ;$07AC
1340        JSR L23   ;$07F9
1350        STA FMSZPG+2 ;$0045
1360        JSR L23   ;$07F9
1370        STA FMSZPG+3 ;$0046
1380 L27    JSR L23   ;$07F9
1390        STA (FMSZPG),Y ;$0043
1400        INC FMSZPG ;$0043
1410        BNE L25   ;$07D3
1420        INC FMSZPG+1 ;$0044
1430        BEQ L26   ;$07DD
1440 L25    LDA FMSZPG+2 ;$0045
1450        CMP FMSZPG ;$0043
1460        LDA FMSZPG+3 ;$0046
1470        SBC FMSZPG+1 ;$0044
1480        BCS L27   ;$07C6
1490 L26    LDA GLBABS+2 ;$02E2
1500        ORA GLBABS+3 ;$02E3
1510        BEQ GETMEM ;$07AC
1520        STX FMSZPG+6 ;$0049
1530        JSR L28   ;$07F6
1540        LDX FMSZPG+6 ;$0049
1550        LDY #$00
1560        STY GLBABS+2 ;$02E2
1570        STY GLBABS+3 ;$02E3
1580        BEQ GETMEM ;$07AC
1590 L28    JMP (GLBABS+2) ;$02E2
1600 L23    CPX #$7D
1610        BNE GETBYTE  ;$083C
1620        LDA DAUX1 ;$030A
1630        ORA DAUX2 ;$030B
1640        BNE READ ;$0815
1650        STA SDMCTL ;$022F
1660        JMP (GLBABS) ;$02E0
1670 DIREAD LDA #$0B
1680        STA DBUFHI ;$0305
1690 ; read into $0B??
1700        NOP
1710        NOP
1720        NOP
1730        NOP
1740        NOP
1750        NOP ; READ (actual)
1760        NOP
1770        NOP
1780        NOP
1790        NOP
1800 READ   JSR $E453
1810        BMI READ  ;error
1820        LDA NEXTH ;$0B7D
1830        AND #$03
1840        STA DAUX2
1850        LDA NEXTL ;$0B7E
1860        STA DAUX1
1870 ; set next sector
1880        NOP
1890        NOP
1900        NOP
1910        LDA BYTES ;$0B7F
1920        AND #$7F
1930        STA L23+1 ;$07FA
1940        LDY #$00
1950        LDX #$00
1960 GETBYTE LDA BUFFER,X ;$0B00
1970        INX
1980        RTS
1990 ; apparently an illegal OS call.
2000 ; like jmp ($E424)
2010 ; seems to be a getkey function.
2020 GETKEY  LDA KEYBDV+5 ;$E425
2030         PHA
2040         LDA KEYBDV+4 ;$E424
2050         PHA
2060         RTS
2070 ; display list
2080 DLST   .BYTE $70,$70,$70
2090 ;       24 blank lines
2100        .BYTE $47
2110        .BYTE DMEM & $00FF ;#$4A
2120        .BYTE DMEM / $0100 ;#$08
2130 ; gr.2 line load at $0868
2140 L21    .BYTE $70 ; 8 blank lines
2150 ; $06 = gr.1
2160        .BYTE $06,$06,$06,$06
2170        .BYTE $06,$06,$06,$06
2180 L37    .BYTE $06,$06,$06,$06
2190        .BYTE $06,$06,$06,$06
2200        .BYTE $06,$06,$06,$06
2210        .BYTE $41
2220        .BYTE DLST & $00FF ;#$4A
2230        .BYTE DLST / $0100 ;#$08
2240 ; jmp to $084A
2250 ; disp.memory is 420 bytes long
2260 DMEM   .BYTE $00,$00,$00 ;
2270        .BYTE $2D,$21,$2B ; MAK
2280        .BYTE $25,$00,$33 ; E S
2290        .BYTE $25,$2C,$25 ; ELE
2300        .BYTE $23,$34,$29 ; CTI
2310        .BYTE $2F,$2E,$00 ; ON
2320        .BYTE $00,$00
2330 LINE1  .BYTE $00,$00,$00,$00
2340 ;External reference equates
2350 BUFFER = $0B00
2360 L30    = $0B7D
2370 L31    = $0B7E
2380 BYTES  = $0B7F
2390 L35    = $08BC
2400 L36    = $0895
2410 ;End of external references
2420        .END      
