/*
 * disasm.c
 *
 * Copyright 2024 Preston Crow
 *
 * Distributed under the GNU Public License version 2
 *
 *
 * This is yet another 6502 disassember, focusing on Atari code.
 */

#include <stdio.h>
#include <endian.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctype.h>

/*
 * Defines
 */
#define BYTE_PSEUDO_OP ".byte"

/*
 * data types
 */
enum address_mode {
   E_IMPLIED,
   E_ACCUMULATOR,
   E_IMMEDIATE,
   E_ABSOLUTE,
   E_ABSOLUTE_X,
   E_ABSOLUTE_Y,
   E_INDIRECT,
   E_ZEROPAGE,
   E_ZEROPAGE_X,
   E_ZEROPAGE_Y,
   E_ZEROPAGE_IND_X,
   E_ZEROPAGE_IND_Y,
   E_RELATIVE, // branches
};

struct opcode {
   char mnemonic[3];
   enum address_mode mode; // implies bytes and display
   int unofficial;
};

struct label {
   int addr;
   char name[16];
};

/*
 * Const tables
 */

const int instruction_bytes[]={
   [E_IMPLIED] = 1,
   [E_ACCUMULATOR] = 1,
   [E_IMMEDIATE] = 2,
   [E_ABSOLUTE] = 3,
   [E_ABSOLUTE_X] = 3,
   [E_ABSOLUTE_Y] = 3,
   [E_INDIRECT] = 3,
   [E_ZEROPAGE] = 2,
   [E_ZEROPAGE_X] = 2,
   [E_ZEROPAGE_Y] = 2,
   [E_ZEROPAGE_IND_X] = 2,
   [E_ZEROPAGE_IND_Y] = 2,
   [E_RELATIVE] = 2,
};

const char *mode_printf[]={
   [E_IMPLIED] = "%s",
   [E_ACCUMULATOR] = "%s A",
   [E_IMMEDIATE] = "%s #$%02X",
   [E_ABSOLUTE] = "%s $%02X%02X",
   [E_ABSOLUTE_X] = "%s $%02X%02X,X",
   [E_ABSOLUTE_Y] = "%s $%02X%02X,Y",
   [E_INDIRECT] = "%s ($%02X%02X)", // JMP; will use label
   [E_ZEROPAGE] = "%s $%02X",
   [E_ZEROPAGE_X] = "%s $%02X,X",
   [E_ZEROPAGE_Y] = "%s $%02X,Y",
   [E_ZEROPAGE_IND_X] = "%s ($%02X,X)",
   [E_ZEROPAGE_IND_Y] = "%s ($%02X),Y",
   [E_RELATIVE] = "%s #$%02X", // BRA; will use label instead
};

const struct opcode opcode[256]={
   [0x00] = { "BRK", E_IMPLIED, 0 },
   [0x18] = { "CLC", E_IMPLIED, 0 },
   [0xd8] = { "CLD", E_IMPLIED, 0 },
   [0x58] = { "CLI", E_IMPLIED, 0 },
   [0xb8] = { "CLV", E_IMPLIED, 0 },
   [0xca] = { "DEX", E_IMPLIED, 0 },
   [0x88] = { "DEY", E_IMPLIED, 0 },
   [0xe8] = { "INX", E_IMPLIED, 0 },
   [0xc8] = { "INY", E_IMPLIED, 0 },
   [0x02] = { "JAM", E_IMPLIED, 1 },
   [0x12] = { "JAM", E_IMPLIED, 1 },
   [0x22] = { "JAM", E_IMPLIED, 1 },
   [0x32] = { "JAM", E_IMPLIED, 1 },
   [0x42] = { "JAM", E_IMPLIED, 1 },
   [0x52] = { "JAM", E_IMPLIED, 1 },
   [0x62] = { "JAM", E_IMPLIED, 1 },
   [0x72] = { "JAM", E_IMPLIED, 1 },
   [0x92] = { "JAM", E_IMPLIED, 1 },
   [0xb2] = { "JAM", E_IMPLIED, 1 },
   [0xd2] = { "JAM", E_IMPLIED, 1 },
   [0xf2] = { "JAM", E_IMPLIED, 1 },
   [0x1a] = { "NOP", E_IMPLIED, 1 },
   [0x3a] = { "NOP", E_IMPLIED, 1 },
   [0x5a] = { "NOP", E_IMPLIED, 1 },
   [0x7a] = { "NOP", E_IMPLIED, 1 },
   [0xda] = { "NOP", E_IMPLIED, 1 },
   [0xea] = { "NOP", E_IMPLIED, 0 },
   [0xfa] = { "NOP", E_IMPLIED, 1 },
   [0x48] = { "PHA", E_IMPLIED, 0 },
   [0x08] = { "PHP", E_IMPLIED, 0 },
   [0x68] = { "PLA", E_IMPLIED, 0 },
   [0x28] = { "PLP", E_IMPLIED, 0 },
   [0x40] = { "RTI", E_IMPLIED, 0 },
   [0x60] = { "RTS", E_IMPLIED, 0 },
   [0x38] = { "SEC", E_IMPLIED, 0 },
   [0xf8] = { "SED", E_IMPLIED, 0 },
   [0x78] = { "SEI", E_IMPLIED, 0 },
   [0xaa] = { "TAX", E_IMPLIED, 0 },
   [0xa8] = { "TAY", E_IMPLIED, 0 },
   [0xba] = { "TSX", E_IMPLIED, 0 },
   [0x8a] = { "TXA", E_IMPLIED, 0 },
   [0x9a] = { "TXS", E_IMPLIED, 0 },
   [0x98] = { "TYA", E_IMPLIED, 0 },
   [0x0a] = { "ASL", E_ACCUMULATOR, 0 },
   [0x4a] = { "LSR", E_ACCUMULATOR, 0 },
   [0x2a] = { "ROL", E_ACCUMULATOR, 0 },
   [0x6a] = { "ROR", E_ACCUMULATOR, 0 },
   // E_IMMEDIATE,
   [0x69] = { "ADC", E_IMMEDIATE, 0 },
   [0x0b] = { "ANC", E_IMMEDIATE, 1 },
   [0x2b] = { "ANC", E_IMMEDIATE, 1 },
   [0x29] = { "AND", E_IMMEDIATE, 0 },
   [0x6b] = { "ARR", E_IMMEDIATE, 1 },
   [0x4b] = { "ASR", E_IMMEDIATE, 1 },
   [0xc9] = { "CMP", E_IMMEDIATE, 0 },
   [0xe0] = { "CPX", E_IMMEDIATE, 0 },
   [0xc0] = { "CPY", E_IMMEDIATE, 0 },
   [0x49] = { "EOR", E_IMMEDIATE, 0 },
   [0xab] = { "LAX", E_IMMEDIATE, 1 },
   [0xa9] = { "LDA", E_IMMEDIATE, 0 },
   [0xa2] = { "LDX", E_IMMEDIATE, 0 },
   [0xa0] = { "LDY", E_IMMEDIATE, 0 },
   [0x80] = { "NOP", E_IMMEDIATE, 1 },
   [0x82] = { "NOP", E_IMMEDIATE, 1 },
   [0x89] = { "NOP", E_IMMEDIATE, 1 },
   [0xc2] = { "NOP", E_IMMEDIATE, 1 },
   [0xe2] = { "NOP", E_IMMEDIATE, 1 },
   [0x09] = { "ORA", E_IMMEDIATE, 0 },
   [0xe9] = { "SBC", E_IMMEDIATE, 0 },
   [0xeb] = { "SBC", E_IMMEDIATE, 1 },
   [0xcb] = { "SBX", E_IMMEDIATE, 1 },
   [0x8b] = { "XAA", E_IMMEDIATE, 1 },
   // E_ABSOLUTE,
   [0x6D] = { "ADC", E_ABSOLUTE, 0 },
   [0x2D] = { "AND", E_ABSOLUTE, 0 },
   [0x0E] = { "ASL", E_ABSOLUTE, 0 },
   [0x2C] = { "BIT", E_ABSOLUTE, 0 },
   [0xCD] = { "CMP", E_ABSOLUTE, 0 },
   [0xEC] = { "CPX", E_ABSOLUTE, 0 },
   [0xCC] = { "CPY", E_ABSOLUTE, 0 },
   [0xCF] = { "DCP", E_ABSOLUTE, 1 },
   [0xCE] = { "DEC", E_ABSOLUTE, 0 },
   [0x4D] = { "EOR", E_ABSOLUTE, 0 },
   [0xEE] = { "INC", E_ABSOLUTE, 0 },
   [0xEF] = { "ISC", E_ABSOLUTE, 1 },
   [0x4C] = { "JMP", E_ABSOLUTE, 0 },
   [0x20] = { "JSR", E_ABSOLUTE, 0 },
   [0xAF] = { "LAX", E_ABSOLUTE, 1 },
   [0xAD] = { "LDA", E_ABSOLUTE, 0 },
   [0xAE] = { "LDX", E_ABSOLUTE, 0 },
   [0xAC] = { "LDY", E_ABSOLUTE, 0 },
   [0x4E] = { "LSR", E_ABSOLUTE, 0 },
   [0x0C] = { "NOP", E_ABSOLUTE, 1 },
   [0x0D] = { "ORA", E_ABSOLUTE, 0 },
   [0x2F] = { "RLA", E_ABSOLUTE, 1 },
   [0x2E] = { "ROL", E_ABSOLUTE, 0 },
   [0x6E] = { "ROR", E_ABSOLUTE, 0 },
   [0x6F] = { "RRA", E_ABSOLUTE, 1 },
   [0x8F] = { "SAX", E_ABSOLUTE, 1 },
   [0xED] = { "SBC", E_ABSOLUTE, 0 },
   [0x0F] = { "SLO", E_ABSOLUTE, 1 },
   [0x4F] = { "SRE", E_ABSOLUTE, 1 },
   [0x8D] = { "STA", E_ABSOLUTE, 0 },
   [0x8E] = { "STX", E_ABSOLUTE, 0 },
   [0x8C] = { "STY", E_ABSOLUTE, 0 },
   // E_ABSOLUTE_X,
   [0x7D] = { "ADC", E_ABSOLUTE_X, 0 },
   [0x3D] = { "AND", E_ABSOLUTE_X, 0 },
   [0x1E] = { "ASL", E_ABSOLUTE_X, 0 },
   [0xDD] = { "CMP", E_ABSOLUTE_X, 0 },
   [0xDF] = { "DCP", E_ABSOLUTE_X, 1 },
   [0xDE] = { "DEC", E_ABSOLUTE_X, 0 },
   [0x5D] = { "EOR", E_ABSOLUTE_X, 0 },
   [0xFE] = { "INC", E_ABSOLUTE_X, 0 },
   [0xFF] = { "ISC", E_ABSOLUTE_X, 1 },
   [0xBD] = { "LDA", E_ABSOLUTE_X, 0 },
   [0xBC] = { "LDY", E_ABSOLUTE_X, 0 },
   [0x5E] = { "LSR", E_ABSOLUTE_X, 0 },
   [0x1C] = { "NOP", E_ABSOLUTE_X, 1 },
   [0x3C] = { "NOP", E_ABSOLUTE_X, 1 },
   [0x5C] = { "NOP", E_ABSOLUTE_X, 1 },
   [0x7C] = { "NOP", E_ABSOLUTE_X, 1 },
   [0xDC] = { "NOP", E_ABSOLUTE_X, 1 },
   [0xFC] = { "NOP", E_ABSOLUTE_X, 1 },
   [0x1D] = { "ORA", E_ABSOLUTE_X, 0 },
   [0x3F] = { "RLA", E_ABSOLUTE_X, 1 },
   [0x3E] = { "ROL", E_ABSOLUTE_X, 0 },
   [0x7E] = { "ROR", E_ABSOLUTE_X, 0 },
   [0x7F] = { "RRA", E_ABSOLUTE_X, 1 },
   [0xFD] = { "SBC", E_ABSOLUTE_X, 0 },
   [0x9C] = { "SHY", E_ABSOLUTE_X, 1 },
   [0x1F] = { "SLO", E_ABSOLUTE_X, 1 },
   [0x5F] = { "SRE", E_ABSOLUTE_X, 1 },
   [0x9D] = { "STA", E_ABSOLUTE_X, 0 },
   // E_ABSOLUTE_Y,
   [0x79] = { "ADC", E_ABSOLUTE_Y, 0 },
   [0x39] = { "AND", E_ABSOLUTE_Y, 0 },
   [0xD9] = { "CMP", E_ABSOLUTE_Y, 0 },
   [0xDB] = { "DCP", E_ABSOLUTE_Y, 1 },
   [0x59] = { "EOR", E_ABSOLUTE_Y, 0 },
   [0xFB] = { "ISC", E_ABSOLUTE_Y, 1 },
   [0xBB] = { "LAS", E_ABSOLUTE_Y, 1 },
   [0xBF] = { "LAX", E_ABSOLUTE_Y, 1 },
   [0xB9] = { "LDA", E_ABSOLUTE_Y, 0 },
   [0xBE] = { "LDX", E_ABSOLUTE_Y, 0 },
   [0x19] = { "ORA", E_ABSOLUTE_Y, 0 },
   [0x3B] = { "RLA", E_ABSOLUTE_Y, 1 },
   [0x7B] = { "RRA", E_ABSOLUTE_Y, 1 },
   [0xF9] = { "SBC", E_ABSOLUTE_Y, 0 },
   [0x9F] = { "SHA", E_ABSOLUTE_Y, 1 },
   [0x9B] = { "SHS", E_ABSOLUTE_Y, 1 },
   [0x9E] = { "SHX", E_ABSOLUTE_Y, 1 },
   [0x1B] = { "SLO", E_ABSOLUTE_Y, 1 },
   [0x5B] = { "SRE", E_ABSOLUTE_Y, 1 },
   [0x99] = { "STA", E_ABSOLUTE_Y, 0 },
   // E_INDIRECT,
   [0x6C] = { "JMP", E_INDIRECT, 0 },
   // E_ZEROPAGE,
   [0x65] = { "ADC", E_ZEROPAGE, 0 },
   [0x25] = { "AND", E_ZEROPAGE, 0 },
   [0x06] = { "ASL", E_ZEROPAGE, 0 },
   [0x24] = { "BIT", E_ZEROPAGE, 0 },
   [0xC5] = { "CMP", E_ZEROPAGE, 0 },
   [0xE4] = { "CPX", E_ZEROPAGE, 0 },
   [0xC4] = { "CPY", E_ZEROPAGE, 0 },
   [0xC7] = { "DCP", E_ZEROPAGE, 1 },
   [0xC6] = { "DEC", E_ZEROPAGE, 0 },
   [0x45] = { "EOR", E_ZEROPAGE, 0 },
   [0xE6] = { "INC", E_ZEROPAGE, 0 },
   [0xE7] = { "ISC", E_ZEROPAGE, 1 },
   [0xA7] = { "LAX", E_ZEROPAGE, 1 },
   [0xA5] = { "LDA", E_ZEROPAGE, 0 },
   [0xA6] = { "LDX", E_ZEROPAGE, 0 },
   [0xA4] = { "LDY", E_ZEROPAGE, 0 },
   [0x46] = { "LSR", E_ZEROPAGE, 0 },
   [0x04] = { "NOP", E_ZEROPAGE, 1 },
   [0x44] = { "NOP", E_ZEROPAGE, 1 },
   [0x64] = { "NOP", E_ZEROPAGE, 1 },
   [0x05] = { "ORA", E_ZEROPAGE, 0 },
   [0x27] = { "RLA", E_ZEROPAGE, 1 },
   [0x26] = { "ROL", E_ZEROPAGE, 0 },
   [0x66] = { "ROR", E_ZEROPAGE, 0 },
   [0x67] = { "RRA", E_ZEROPAGE, 1 },
   [0x87] = { "SAX", E_ZEROPAGE, 1 },
   [0xE5] = { "SBC", E_ZEROPAGE, 0 },
   [0x07] = { "SLO", E_ZEROPAGE, 1 },
   [0x47] = { "SRE", E_ZEROPAGE, 1 },
   [0x85] = { "STA", E_ZEROPAGE, 0 },
   [0x86] = { "STX", E_ZEROPAGE, 0 },
   [0x84] = { "STY", E_ZEROPAGE, 0 },
   // E_ZEROPAGE_X,
   [0x75] = { "ADC", E_ZEROPAGE_X, 0 },
   [0x35] = { "AND", E_ZEROPAGE_X, 0 },
   [0x16] = { "ASL", E_ZEROPAGE_X, 0 },
   [0xD5] = { "CMP", E_ZEROPAGE_X, 0 },
   [0xD7] = { "DCP", E_ZEROPAGE_X, 1 },
   [0xD6] = { "DEC", E_ZEROPAGE_X, 0 },
   [0x55] = { "EOR", E_ZEROPAGE_X, 0 },
   [0xF6] = { "INC", E_ZEROPAGE_X, 0 },
   [0xF7] = { "ISC", E_ZEROPAGE_X, 1 },
   [0xB5] = { "LDA", E_ZEROPAGE_X, 0 },
   [0xB4] = { "LDY", E_ZEROPAGE_X, 0 },
   [0x56] = { "LSR", E_ZEROPAGE_X, 0 },
   [0x14] = { "NOP", E_ZEROPAGE_X, 1 },
   [0x34] = { "NOP", E_ZEROPAGE_X, 1 },
   [0x54] = { "NOP", E_ZEROPAGE_X, 1 },
   [0x74] = { "NOP", E_ZEROPAGE_X, 1 },
   [0xD4] = { "NOP", E_ZEROPAGE_X, 1 },
   [0xF4] = { "NOP", E_ZEROPAGE_X, 1 },
   [0x15] = { "ORA", E_ZEROPAGE_X, 0 },
   [0x37] = { "RLA", E_ZEROPAGE_X, 1 },
   [0x36] = { "ROL", E_ZEROPAGE_X, 0 },
   [0x76] = { "ROR", E_ZEROPAGE_X, 0 },
   [0x77] = { "RRA", E_ZEROPAGE_X, 1 },
   [0xF5] = { "SBC", E_ZEROPAGE_X, 0 },
   [0x17] = { "SLO", E_ZEROPAGE_X, 1 },
   [0x57] = { "SRE", E_ZEROPAGE_X, 1 },
   [0x95] = { "STA", E_ZEROPAGE_X, 0 },
   [0x94] = { "STY", E_ZEROPAGE_X, 0 },
   // E_ZEROPAGE_Y,
   [0xB7] = { "LAX", E_ZEROPAGE_Y, 1 },
   [0xB6] = { "LDX", E_ZEROPAGE_Y, 0 },
   [0x97] = { "SAX", E_ZEROPAGE_Y, 1 },
   [0x96] = { "STX", E_ZEROPAGE_Y, 0 },
   // E_ZEROPAGE_IND_X,
   [0x61] = { "ADC", E_ZEROPAGE_IND_X, 0 },
   [0x21] = { "AND", E_ZEROPAGE_IND_X, 0 },
   [0xC1] = { "CMP", E_ZEROPAGE_IND_X, 0 },
   [0xC3] = { "DCP", E_ZEROPAGE_IND_X, 1 },
   [0x41] = { "EOR", E_ZEROPAGE_IND_X, 0 },
   [0xE3] = { "ISC", E_ZEROPAGE_IND_X, 1 },
   [0xA3] = { "LAX", E_ZEROPAGE_IND_X, 1 },
   [0xA1] = { "LDA", E_ZEROPAGE_IND_X, 0 },
   [0x01] = { "ORA", E_ZEROPAGE_IND_X, 0 },
   [0x23] = { "RLA", E_ZEROPAGE_IND_X, 1 },
   [0x63] = { "RRA", E_ZEROPAGE_IND_X, 1 },
   [0x83] = { "SAX", E_ZEROPAGE_IND_X, 1 },
   [0xE1] = { "SBC", E_ZEROPAGE_IND_X, 0 },
   [0x03] = { "SLO", E_ZEROPAGE_IND_X, 1 },
   [0x43] = { "SRE", E_ZEROPAGE_IND_X, 1 },
   [0x81] = { "STA", E_ZEROPAGE_IND_X, 0 },
   // E_ZEROPAGE_IND_Y,
   [0x71] = { "ADC", E_ZEROPAGE_IND_Y, 0 },
   [0x31] = { "AND", E_ZEROPAGE_IND_Y, 0 },
   [0xD1] = { "CMP", E_ZEROPAGE_IND_Y, 0 },
   [0xD3] = { "DCP", E_ZEROPAGE_IND_Y, 1 },
   [0x51] = { "EOR", E_ZEROPAGE_IND_Y, 0 },
   [0xF3] = { "ISC", E_ZEROPAGE_IND_Y, 1 },
   [0xB3] = { "LAX", E_ZEROPAGE_IND_Y, 1 },
   [0xB1] = { "LDA", E_ZEROPAGE_IND_Y, 0 },
   [0x11] = { "ORA", E_ZEROPAGE_IND_Y, 0 },
   [0x33] = { "RLA", E_ZEROPAGE_IND_Y, 1 },
   [0x73] = { "RRA", E_ZEROPAGE_IND_Y, 1 },
   [0xF1] = { "SBC", E_ZEROPAGE_IND_Y, 0 },
   [0x93] = { "SHA", E_ZEROPAGE_IND_Y, 1 },
   [0x13] = { "SLO", E_ZEROPAGE_IND_Y, 1 },
   [0x53] = { "SRE", E_ZEROPAGE_IND_Y, 1 },
   [0x91] = { "STA", E_ZEROPAGE_IND_Y, 0 },
   // E_RELATIVE,
   [0x90] = { "BCC", E_RELATIVE, 0 },
   [0xB0] = { "BCS", E_RELATIVE, 0 },
   [0xF0] = { "BEQ", E_RELATIVE, 0 },
   [0x30] = { "BMI", E_RELATIVE, 0 },
   [0xD0] = { "BNE", E_RELATIVE, 0 },
   [0x10] = { "BPL", E_RELATIVE, 0 },
   [0x50] = { "BVC", E_RELATIVE, 0 },
   [0x70] = { "BVS", E_RELATIVE, 0 },
};

/*
 * global variables
 */

// The 6502 memory
unsigned char mem[64*1024];

// Flags per-byte
char mem_loaded[64*1024];
char instruction[64*1024]; // first byte of an instruction?
char branch_target[64*1024];
char data_target[64*1024];

// Table of known labels; use these instead of Lxxxx
// https://atariwiki.org/wiki/Wiki.jsp?page=Memory%20Map
// Script:
// cat map | sed -e 's@</*tr[^>]*>@@g' -e 's/ class="[^"]*"//g' -e 's/ href="[^"]*"//g' -e 's/ title="[^"]*"//g' -e '/^<td>[^0-9]*<.td>/d' -e 's@</[^>]*>@@g' -e 's/^[^$]*//' -e 's/[$]/0x/g' -e '/^ *$/d' -e 's/<a>//g' -e 's/^\([^<]*<td>[^<]*\)<.*/\1/' -e 's/\(.*[^ ]\) *<td> *\(.*[^ ]\) *$/   { \1, "\2" },/' | grep -v '"."' | sed -e 's/0x\([0-9A-F][0-9A-F][0-9A-F][^0-9A-F]\)/0x0\1/g' | sed -e 's/^.*{ \(0x....\),\(0x....\), "\([^,]*\),\([^"]*\)" },.*/   { \1, "\3" },\n   { \2, "\4" },/' | sed -e '/^0x/d' -e 's/^.*{ \(0x....\)[,;]\(0x....\), "\([^,"]*\)" },.*/   { \1, "\3" },\n   { \2, "(\3+1)" },/'
// Then patch up the ones that are ranges
// Registers that are different for read/write use both names with a comma.  The display
// code will extract the right one; both will be printed as defines even if only one is used.
const struct label label_table[] = {
   { 0x0000, "LINZBS" },
   { 0x0001, "LINZBS+1" },
   { 0x0000, "LINFLG" },
   { 0x0001, "NGFLAG" },
   { 0x0002, "CASINI" },
   { 0x0003, "CASINI+1" },
   { 0x0004, "RAMLO" },
   { 0x0005, "RAMLO+1" },
   { 0x0006, "TRAMSZ" },
   { 0x0006, "TRNSMZ" },
   { 0x0007, "TSTDAT" },
   { 0x0007, "TSTDAT" },
   { 0x0008, "WARMST" },
   { 0x0009, "BOOT?" },
   { 0x000A, "DOSVEC" },
   { 0x000B, "DOSVEC+1" },
   { 0x000C, "DOSINI" },
   { 0x000D, "DOSINI+1" },
   { 0x000E, "APPMHI" },
   { 0x000F, "APPMHI+1" },
   { 0x0010, "POKMSK" },
   { 0x0011, "BRKKEY" },
   { 0x0012, "RTCLOK" },
   { 0x0013, "RTCLOK+1" },
   { 0x0014, "RTCLOK+2" },
   { 0x0015, "BUFADR" },
   { 0x0016, "BUFADR+1" },
   { 0x0017, "ICCOMT" },
   { 0x0018, "DSKFMS" },
   { 0x0019, "DSKFMS+1" },
   { 0x001A, "DSKUTL" },
   { 0x001B, "DSKUTL+1" },
   { 0x001C, "PTIMOT" },
   { 0x001D, "PBPNT" },
   { 0x001E, "PBUFSZ" },
   { 0x001F, "PTEMP" },
   { 0x0020, "ICHIDZ" },
   { 0x0021, "ICDNOZ" },
   { 0x0022, "ICCOMZ" },
   { 0x0023, "ICSTAZ" },
   { 0x0024, "ICBALZ" },
   { 0x0025, "ICBAHZ" },
   { 0x0026, "ICPTLZ" },
   { 0x0027, "ICPTHZ" },
   { 0x0028, "ICBLLZ" },
   { 0x0029, "ICBLHZ" },
   { 0x002A, "ICAX1Z" },
   { 0x002B, "ICAX2Z" },
   { 0x002C, "ICAX3Z" },
   { 0x002D, "ICAX4Z" },
   { 0x002E, "ICAX5Z" },
   { 0x002F, "ICAX6Z" },
   { 0x0030, "STATUS" },
   { 0x0031, "CHKSUM" },
   { 0x0032, "BUFRLO" },
   { 0x0033, "BUFRHI" },
   { 0x0034, "BFENLO" },
   { 0x0035, "BFENHI" },
   { 0x0036, "CRETRY" },
   { 0x0037, "DRETRY" },
   { 0x0036, "LTEMP" },
   { 0x0037, "LTEMP+1" },
   { 0x0038, "BUFRFL" },
   { 0x0039, "RECVDN" },
   { 0x003A, "XMTDON" },
   { 0x003B, "CHKSNT" },
   { 0x003C, "NOCKSM" },
   { 0x003D, "BPTR" },
   { 0x003E, "FTYPE" },
   { 0x003F, "FEOF" },
   { 0x0040, "FREQ" },
   { 0x0041, "SOUNDR" },
   { 0x0042, "CRITIC" },
   //{ 0x0043-0x0049, "FMSZPG" },
   { 0x0043, "ZBUFP" },
   { 0x0044, "ZBUFP+1" },
   { 0x0045, "ZDRVA" },
   { 0x0046, "ZDRVA+1" },
   { 0x0047, "ZSBA" },
   { 0x0048, "ZSBA+1" },
   { 0x0049, "ERRNO" },
   { 0x004A, "CKEY" },
   { 0x004B, "CASSBT" },
   { 0x004A, "ZCHAIN" },
   { 0x004B, "ZCHAIN+1" },
   { 0x004C, "DSTAT" },
   { 0x004D, "ATRACT" },
   { 0x004E, "DRKMSK" },
   { 0x004F, "COLRSH" },
   { 0x0050, "TEMP" },
   { 0x0051, "HOLD1" },
   { 0x0052, "LMARGN" },
   { 0x0053, "RMARGN" },
   { 0x0054, "ROWCRS" },
   { 0x0055, "COLCRS" },
   { 0x0056, "COLCRS+1" },
   { 0x0057, "DINDEX" },
   { 0x0058, "SAVMSC" },
   { 0x0059, "SAVMSC+1" },
   { 0x005A, "OLDROW" },
   { 0x005B, "OLDCOL" },
   { 0x005C, "OLDCOL+1" },
   { 0x005D, "OLDCHR" },
   { 0x005E, "OLDADR" },
   { 0x005F, "OLDADR+1" },
   { 0x0060, "NEWROW" },
   { 0x0060, "FKDEF" },
   { 0x0061, "FKDEF+1" },
   { 0x0061, "NEWCOL" },
   { 0x0062, "NEWCOL+1" },
   { 0x0062, "PALNTS" },
   { 0x0063, "LOGCOL" },
   { 0x0064, "ADRESS" },
   { 0x0065, "ADRESS+1" },
   { 0x0066, "MLTTMP" },
   { 0x0067, "MLTTMP+1" },
   { 0x0068, "SAVADR" },
   { 0x0069, "SAVADR+1" },
   { 0x006A, "RAMTOP" },
   { 0x006B, "BUFCNT" },
   { 0x006C, "BUFSTR" },
   { 0x006D, "BUFSTR+1" },
   { 0x006E, "BITMSK" },
   { 0x006F, "SHFAMT" },
   { 0x0070, "ROWAC" },
   { 0x0071, "ROWAC+1" },
   { 0x0072, "COLAC" },
   { 0x0073, "COLAC+1" },
   { 0x0074, "ENDPT" },
   { 0x0075, "ENDPT+1" },
   { 0x0076, "DELTAR" },
   { 0x0077, "DELTAC" },
   { 0x0078, "DELTAC+1" },
   { 0x0079, "ROWINC" },
   { 0x007A, "COLINC" },
   { 0x0079, "KEYDEF" },
   { 0x007A, "KEYDEF+1" },
   { 0x007B, "SWPFLG" },
   { 0x007C, "HOLDCH" },
   { 0x007D, "INSDAT" },
   { 0x007E, "COUNTR" },
   { 0x007F, "COUNTR+1" },
   { 0x0080, "LOMEM" },
   { 0x0081, "LOMEM+1" },
   { 0x0082, "VNTP" },
   { 0x0083, "VNTP+1" },
   { 0x0084, "VNTD" },
   { 0x0085, "VNTD+1" },
   { 0x0086, "VVTP" },
   { 0x0087, "VVTP+1" },
   { 0x0088, "STMTAB" },
   { 0x0089, "STMTAB+1" },
   { 0x008A, "STMCUR" },
   { 0x008B, "STMCUR+1" },
   { 0x008C, "STARP" },
   { 0x008D, "STARP+1" },
   { 0x008E, "RUNSTK" },
   { 0x008F, "RUNSTK+1" },
   { 0x0090, "MEMTOP" },
   { 0x0091, "MEMTOP+1" },
   { 0x00BA, "STOPLN" },
   { 0x00BB, "STOPLN+1" },
   { 0x00C3, "ERRSAVE" },
   { 0x00C9, "PTABW" },
   { 0x00D4, "FR0" },
   { 0x00D5, "FR0+1" },
   { 0x00D6, "FR0+2" },
   { 0x00D7, "FR0+3" },
   { 0x00D8, "FR0+4" },
   { 0x00D9, "FR0+5" },
   { 0x00DA, "FRE" },
   { 0x00DB, "FRE+1" },
   { 0x00DC, "FRE+2" },
   { 0x00DD, "FRE+3" },
   { 0x00DE, "FRE+4" },
   { 0x00DF, "FRE+5" },
   { 0x00E0, "FR1" },
   { 0x00E1, "FR1+1" },
   { 0x00E2, "FR1+2" },
   { 0x00E3, "FR1+3" },
   { 0x00E4, "FR1+4" },
   { 0x00E5, "FR1+5" },
   { 0x00E6, "FR2" },
   { 0x00E7, "FR2+1" },
   { 0x00E8, "FR2+2" },
   { 0x00E9, "FR2+3" },
   { 0x00EA, "FR2+4" },
   { 0x00EB, "FR2+5" },
   { 0x00EC, "FRX" },
   { 0x00ED, "EEXP" },
   { 0x00EE, "NSIGN" },
   { 0x00EF, "ESIGN" },
   { 0x00F0, "FCHRFLG" },
   { 0x00F1, "DIGRT" },
   { 0x00F2, "CIX" },
   { 0x00F3, "INBUFF" },
   { 0x00F4, "INBUFF+1" },
   { 0x00F5, "ZTEMP1" },
   { 0x00F6, "ZTEMP1+1" },
   { 0x00F7, "ZTEMP4" },
   { 0x00F8, "ZTEMP4+1" },
   { 0x00F9, "ZTEMP3" },
   { 0x00FA, "ZTEMP3+1" },
   { 0x00FB, "RADFLG" },
   { 0x00FC, "FLPTR" },
   { 0x00FD, "FLPTR+1" },
   { 0x00FE, "FPTR2" },
   { 0x00FF, "FPTR2+1" },
   { 0x0200, "VDSLST" },
   { 0x0201, "VDSLST+1" },
   { 0x0202, "VPRCED" },
   { 0x0203, "VPRCED+1" },
   { 0x0204, "VINTER" },
   { 0x0205, "VINTER+1" },
   { 0x0206, "VBREAK" },
   { 0x0207, "VBREAK+1" },
   { 0x0208, "VKEYBD" },
   { 0x0209, "VKEYBD+1" },
   { 0x020A, "VSERIN" },
   { 0x020B, "VSERIN+1" },
   { 0x020C, "VSEROR" },
   { 0x020D, "VSEROR+1" },
   { 0x020E, "VSEROC" },
   { 0x020F, "VSEROC+1" },
   { 0x0210, "VTIMR1" },
   { 0x0211, "VTIMR1+1" },
   { 0x0212, "VTIMR2" },
   { 0x0213, "VTIMR2+1" },
   { 0x0214, "VTIMR4" },
   { 0x0215, "VTIMR4+1" },
   { 0x0216, "VIMIRQ" },
   { 0x0217, "VIMIRQ+1" },
   { 0x0218, "CDTMV1" },
   { 0x0219, "CDTMV1+1" },
   { 0x021A, "CDTMV2" },
   { 0x021B, "CDTMV2+1" },
   { 0x021C, "CDTMV3" },
   { 0x021D, "CDTMV3+1" },
   { 0x021E, "CDTMV4" },
   { 0x021F, "CDTMV4+1" },
   { 0x0220, "CDTMV5" },
   { 0x0221, "CDTMV5+1" },
   { 0x0222, "VVBLKI" },
   { 0x0223, "VVBLKI+1" },
   { 0x0224, "VVBLKD" },
   { 0x0225, "VVBLKD+1" },
   { 0x0226, "CDTMA1" },
   { 0x0227, "CDTMA1+1" },
   { 0x0228, "CDTMA2" },
   { 0x0229, "CDTMA2+1" },
   { 0x022A, "CDTMF3" },
   { 0x022B, "SRTIMR" },
   { 0x022C, "CDTMF4" },
   { 0x022D, "INTEMP" },
   { 0x022E, "CDTMF5" },
   { 0x022F, "SDMCTL" },
   { 0x0230, "SDLSTL" },
   { 0x0231, "SDLSTH" },
   { 0x0232, "SSKCTL" },
   { 0x0233, "SPARE" },
   { 0x0233, "LCOUNT" },
   { 0x0234, "LPENH" },
   { 0x0235, "LPENV" },
   { 0x0236, "BRKKY" },
   { 0x0237, "BRKKY+1" },
   { 0x0238, "VPIRQ" },
   { 0x0239, "VPIRQ+1" },
   { 0x023A, "CDEVIC" },
   { 0x023B, "CCOMND" },
   { 0x023C, "CAUX1" },
   { 0x023D, "CAUX2" },
   { 0x023E, "TEMP" },
   { 0x023F, "ERRFLG" },
   { 0x0240, "DFLAGS" },
   { 0x0241, "DESECT" },
   { 0x0242, "BOOTAD" },
   { 0x0243, "BOOTAD+1" },
   { 0x0244, "COLDST" },
   { 0x0245, "RECLEN" },
   { 0x0246, "DSKTIM" },
   //{ 0x0247-0x026E, "LINBUF" }, // 40-character line buffer; not sure about what the other stuff in the same range is
   { 0x0247, "PDVMSK" },
   { 0x0248, "SHPDVS" },
   { 0x0249, "PDIMSK" },
   { 0x024A, "RELADR" },
   { 0x024B, "RELADR+1" },
   { 0x024C, "PPTMPA" },
   { 0x024D, "PPTMPX" },
   { 0x026B, "CHSALT" },
   { 0x026C, "VSFLAG" },
   { 0x026D, "KEYDIS" },
   { 0x026E, "FINE" },
   { 0x026F, "GPRIOR" },
   { 0x0270, "PADDL0" },
   { 0x0271, "PADDL1" },
   { 0x0272, "PADDL2" },
   { 0x0273, "PADDL3" },
   { 0x0274, "PADDL4" },
   { 0x0275, "PADDL5" },
   { 0x0276, "PADDL6" },
   { 0x0277, "PADDL7" },
   { 0x0278, "STICK0" },
   { 0x0279, "STICK1" },
   { 0x027A, "STICK2" },
   { 0x027B, "STICK3" },
   { 0x027C, "PTRIG0" },
   { 0x027D, "PTRIG1" },
   { 0x027E, "PTRIG2" },
   { 0x027F, "PTRIG3" },
   { 0x0280, "PTRIG4" },
   { 0x0281, "PTRIG5" },
   { 0x0282, "PTRIG6" },
   { 0x0283, "PTRIG7" },
   { 0x0284, "STRIG0" },
   { 0x0285, "STRIG1" },
   { 0x0286, "STRIG2" },
   { 0x0287, "STRIG3" },
   { 0x0288, "CSTAT" },
   { 0x0288, "HIBZTE" },
   { 0x0289, "WMODE" },
   { 0x028A, "BLIM" },
   { 0x028B, "IMASK" },
   { 0x028C, "JVECK" },
   { 0x028D, "JVECK+1" },
   { 0x028E, "NEWADR" },
   { 0x028F, "NEWADR+1" },
   { 0x0290, "TXTROW" },
   { 0x0291, "TXTCOL" },
   { 0x0292, "TXTCOL+1" },
   { 0x0293, "TINDEX" },
   { 0x0294, "TXTMSC" },
   { 0x0295, "TXTMSC+1" },
   //{ 0x0296-0x029B, "TXTOLD" }, // used for cursor position in split screen mode
   { 0x029C, "TMPX1" },
   { 0x029C, "CRETRY" },
   { 0x029D, "HOLD3" },
   { 0x029E, "SUBTMP" },
   { 0x029F, "HOLD2" },
   { 0x02A0, "DMASK" },
   { 0x02A1, "TMPLBT" },
   { 0x02A2, "ESCFLG" },
   //{ 0x02A3-0x02B1, "TABMAP" }, // map of tab positions
   //{ 0x02B2-0x02B5, "LOGMAP" }, // map of logical line breaks
   { 0x02B6, "INVFLG" },
   { 0x02B7, "FILFLG" },
   { 0x02B8, "TMPROW" },
   { 0x02B9, "TMPCOL" },
   { 0x02BA, "TMPCOL+1" },
   { 0x02BB, "SCRFLG" },
   { 0x02BC, "HOLD4" },
   { 0x02BD, "HOLD5" },
   { 0x02BD, "DRETRY" },
   { 0x02BE, "SHFLOK" },
   { 0x02BF, "BOTSCR" },
   { 0x02C0, "PCOLR0" },
   { 0x02C1, "PCOLR1" },
   { 0x02C2, "PCOLR2" },
   { 0x02C3, "PCOLR3" },
   { 0x02C4, "COLOR0" },
   { 0x02C5, "COLOR1" },
   { 0x02C6, "COLOR2" },
   { 0x02C7, "COLOR3" },
   { 0x02C8, "COLOR4" },
   { 0x02C9, "RUNADR" },
   { 0x02CA, "RUNADR+1" },
   { 0x02CB, "HIUSED" },
   { 0x02CC, "HIUSED+1" },
   { 0x02CD, "ZHIUSE" },
   { 0x02CE, "ZHIUSE+1" },
   { 0x02CF, "GBYTEA" },
   { 0x02D0, "GBYTEA+1" },
   { 0x02D1, "LOADAD" },
   { 0x02D2, "LOADAD+1" },
   { 0x02D3, "ZLOADA" },
   { 0x02D4, "ZLOADA+1" },
   { 0x02D5, "DSCTLN" },
   { 0x02D6, "DSCTLN+1" },
   { 0x02D7, "ACMISR" },
   { 0x02D8, "ACMISR+1" },
   { 0x02D9, "KRPDEL" },
   { 0x02DA, "KEYREP" },
   { 0x02DB, "NOCLIK" },
   { 0x02FC, "HELPFG" },
   { 0x02DD, "DMASAV" },
   { 0x02DE, "PBPNT" },
   { 0x02DF, "PBUFSZ" },
   { 0x02E0, "RUNAD" },
   { 0x02E1, "RUNAD+1" },
   { 0x02E2, "INITAD" },
   { 0x02E3, "INITAD+1" },
   { 0x02E4, "RAMSIZ" },
   { 0x02E5, "MEMTOP" },
   { 0x02E6, "MEMTOP+1" },
   { 0x02E7, "MEMLO" },
   { 0x02E8, "MEMLO+1" },
   { 0x02E9, "HNDLOD" },
   { 0x02EA, "DVSTAT" },
   { 0x02EB, "DVSTAT+1" },
   { 0x02EC, "DVSTAT+2" },
   { 0x02ED, "DVSTAT+3" },
   { 0x02EE, "CBAUDL" },
   { 0x02EF, "CBAUDH" },
   { 0x02F0, "CRSINH" },
   { 0x02F1, "KEYDEL" },
   { 0x02F2, "CH1" },
   { 0x02F3, "CHACT" },
   { 0x02F4, "CHBAS" },
   { 0x02F5, "NEWROW" },
   { 0x02F6, "NEWCOL" },
   { 0x02F7, "NEWCOL+1" },
   { 0x02F8, "ROWINC" },
   { 0x02F9, "COLINC" },
   { 0x02FA, "CHAR" },
   { 0x02FB, "ATACHR" },
   { 0x02FC, "CH" },
   { 0x02FD, "FILDAT" },
   { 0x02FE, "DSPFLG" },
   { 0x02FF, "SSFLAG" },
   { 0x0300, "DDEVIC" },
   { 0x0301, "DUNIT" },
   { 0x0302, "DCOMND" },
   { 0x0303, "DSTATS" },
   { 0x0304, "DBUFLO" },
   { 0x0305, "DBUFHI" },
   { 0x0306, "DTIMLO" },
   { 0x0307, "DUNUSE" },
   { 0x0308, "DBYTLO" },
   { 0x0309, "DBYTHI" },
   { 0x030A, "DAUX1" },
   { 0x030B, "DAUX2" },
   { 0x030C, "TIMER1" },
   { 0x030D, "TIMER1+1" },
   { 0x030E, "ADDCOR" },
   { 0x030E, "JMPERS" },
   { 0x030F, "CASFLG" },
   { 0x0310, "TIMER2" },
   { 0x0311, "TIMER2+1" },
   { 0x0312, "TEMP1" },
   { 0x0313, "TEMP1+1" },
   { 0x0314, "TEMP2" },
   { 0x0314, "PTIMOT" },
   { 0x0315, "TEMP3" },
   { 0x0316, "SAVIO" },
   { 0x0317, "TIMFLG" },
   { 0x0318, "STACKP" },
   { 0x0319, "TSTAT" },
   //{ 0x031A-0x033F, "HATABS" }, // Handler address tables, 3 bytes per entry, two zeros at end
   { 0x0340, "IOCB0" },
   { 0x0341, "IOCB0+1" },
   { 0x0342, "IOCB0+2" },
   { 0x0343, "IOCB0+3" },
   { 0x0344, "IOCB0+4" },
   { 0x0345, "IOCB0+5" },
   { 0x0346, "IOCB0+6" },
   { 0x0347, "IOCB0+7" },
   { 0x0348, "IOCB0+8" },
   { 0x0349, "IOCB0+9" },
   { 0x034A, "IOCB0+10" },
   { 0x034B, "IOCB0+11" },
   { 0x034C, "IOCB0+12" },
   { 0x034D, "IOCB0+13" },
   { 0x034E, "IOCB0+14" },
   { 0x034F, "IOCB0+15" },
   { 0x0350, "IOCB1" },
   { 0x0351, "IOCB1+1" },
   { 0x0352, "IOCB1+2" },
   { 0x0353, "IOCB1+3" },
   { 0x0354, "IOCB1+4" },
   { 0x0355, "IOCB1+5" },
   { 0x0356, "IOCB1+6" },
   { 0x0357, "IOCB1+7" },
   { 0x0358, "IOCB1+8" },
   { 0x0359, "IOCB1+9" },
   { 0x035A, "IOCB1+10" },
   { 0x035B, "IOCB1+11" },
   { 0x035C, "IOCB1+12" },
   { 0x035D, "IOCB1+13" },
   { 0x035E, "IOCB1+14" },
   { 0x035F, "IOCB1+15" },
   { 0x0360, "IOCB2" },
   { 0x0361, "IOCB2+1" },
   { 0x0362, "IOCB2+2" },
   { 0x0363, "IOCB2+3" },
   { 0x0364, "IOCB2+4" },
   { 0x0365, "IOCB2+5" },
   { 0x0366, "IOCB2+6" },
   { 0x0367, "IOCB2+7" },
   { 0x0368, "IOCB2+8" },
   { 0x0369, "IOCB2+9" },
   { 0x036A, "IOCB2+10" },
   { 0x036B, "IOCB2+11" },
   { 0x036C, "IOCB2+12" },
   { 0x036D, "IOCB2+13" },
   { 0x036E, "IOCB2+14" },
   { 0x036F, "IOCB2+15" },
   { 0x0370, "IOCB3" },
   { 0x0371, "IOCB3+1" },
   { 0x0372, "IOCB3+2" },
   { 0x0373, "IOCB3+3" },
   { 0x0374, "IOCB3+4" },
   { 0x0375, "IOCB3+5" },
   { 0x0376, "IOCB3+6" },
   { 0x0377, "IOCB3+7" },
   { 0x0378, "IOCB3+8" },
   { 0x0379, "IOCB3+9" },
   { 0x037A, "IOCB3+10" },
   { 0x037B, "IOCB3+11" },
   { 0x037C, "IOCB3+12" },
   { 0x037D, "IOCB3+13" },
   { 0x037E, "IOCB3+14" },
   { 0x037F, "IOCB3+15" },
   { 0x0380, "IOCB4" },
   { 0x0381, "IOCB4+1" },
   { 0x0382, "IOCB4+2" },
   { 0x0383, "IOCB4+3" },
   { 0x0384, "IOCB4+4" },
   { 0x0385, "IOCB4+5" },
   { 0x0386, "IOCB4+6" },
   { 0x0387, "IOCB4+7" },
   { 0x0388, "IOCB4+8" },
   { 0x0389, "IOCB4+9" },
   { 0x038A, "IOCB4+10" },
   { 0x038B, "IOCB4+11" },
   { 0x038C, "IOCB4+12" },
   { 0x038D, "IOCB4+13" },
   { 0x038E, "IOCB4+14" },
   { 0x038F, "IOCB4+15" },
   { 0x0390, "IOCB5" },
   { 0x0391, "IOCB5+1" },
   { 0x0392, "IOCB5+2" },
   { 0x0393, "IOCB5+3" },
   { 0x0394, "IOCB5+4" },
   { 0x0395, "IOCB5+5" },
   { 0x0396, "IOCB5+6" },
   { 0x0397, "IOCB5+7" },
   { 0x0398, "IOCB5+8" },
   { 0x0399, "IOCB5+9" },
   { 0x039A, "IOCB5+10" },
   { 0x039B, "IOCB5+11" },
   { 0x039C, "IOCB5+12" },
   { 0x039D, "IOCB5+13" },
   { 0x039E, "IOCB5+14" },
   { 0x039F, "IOCB5+15" },
   { 0x03A0, "IOCB6" },
   { 0x03A1, "IOCB6+1" },
   { 0x03A2, "IOCB6+2" },
   { 0x03A3, "IOCB6+3" },
   { 0x03A4, "IOCB6+4" },
   { 0x03A5, "IOCB6+5" },
   { 0x03A6, "IOCB6+6" },
   { 0x03A7, "IOCB6+7" },
   { 0x03A8, "IOCB6+8" },
   { 0x03A9, "IOCB6+9" },
   { 0x03AA, "IOCB6+10" },
   { 0x03AB, "IOCB6+11" },
   { 0x03AC, "IOCB6+12" },
   { 0x03AD, "IOCB6+13" },
   { 0x03AE, "IOCB6+14" },
   { 0x03AF, "IOCB6+15" },
   { 0x03B0, "IOCB7" },
   { 0x03B1, "IOCB7+1" },
   { 0x03B2, "IOCB7+2" },
   { 0x03B3, "IOCB7+3" },
   { 0x03B4, "IOCB7+4" },
   { 0x03B5, "IOCB7+5" },
   { 0x03B6, "IOCB7+6" },
   { 0x03B7, "IOCB7+7" },
   { 0x03B8, "IOCB7+8" },
   { 0x03B9, "IOCB7+9" },
   { 0x03BA, "IOCB7+10" },
   { 0x03BB, "IOCB7+11" },
   { 0x03BC, "IOCB7+12" },
   { 0x03BD, "IOCB7+13" },
   { 0x03BE, "IOCB7+14" },
   { 0x03BF, "IOCB7+15" },
   //{ 0x03C0-0x03E7, "PRNBUF" },
   { 0x03E8, "SUPERF" },
   { 0x03E9, "CKEY" },
   { 0x03EA, "CASSBT" },
   { 0x03EB, "CARTCK" },
   { 0x03EC, "DEERF" },
   //{ 0x03ED-0x03F7, "ACMVAR" },
   { 0x03F8, "BASICF" },
   { 0x03F9, "MINTLK" },
   { 0x03FA, "GINTLK" },
   { 0x03FB, "CHLINK" },
   { 0x03FC, "CHLINK+1" },
   //{ 0x03FD-0x047F, "CASBUF" },
   { 0x057E, "LBPR1" },
   { 0x057F, "LBPR2" },
   //{ 0x0580-0x05FF, "LBUFF" },
   { 0x05E0, "PLYARG" },
   //{ 0x05E6-0x05EB, "FPSCR" },
   //{ 0x05EC-0x05FF, "FPSCR1" },
   { 0xD000, "HPOSP0,M0PF" },
   { 0xD001, "HPOSP1,M1PF" },
   { 0xD002, "HPOSP2,M2PF" },
   { 0xD003, "HPOSP3,M3PF" },
   { 0xD004, "HPOSM0,P0PF" },
   { 0xD005, "HPOSM1,P1PF" },
   { 0xD006, "HPOSM2,P2PF" },
   { 0xD007, "HPOSM3,P3PF" },
   { 0xD008, "SIZEP0,M0PL" },
   { 0xD009, "SIZEP1,M1PL" },
   { 0xD00A, "SIZEP2,M2PL" },
   { 0xD00B, "SIZEP3,M3PL" },
   { 0xD00C, "SIZEM,P0PL" },
   { 0xD00D, "GRAFP0,P1PL" },
   { 0xD00E, "GRAFP1,P2PL" },
   { 0xD00F, "GRAFP2,P3PL" },
   { 0xD010, "GRAFP3,TRIG0" },
   { 0xD011, "GRAFM,TRIG1" },
   { 0xD012, "COLPM0,TRIG2" },
   { 0xD013, "COLPM1,TRIG3" },
   { 0xD014, "COLPM2,PAL" },
   { 0xD015, "COLPM3" },
   { 0xD016, "COLPF0" },
   { 0xD017, "COLPF1" },
   { 0xD018, "COLPF2" },
   { 0xD019, "COLPF3" },
   { 0xD01A, "COLBK" },
   { 0xD01B, "PRIOR" },
   { 0xD01C, "VDELAY" },
   { 0xD01D, "GRACTL" },
   { 0xD01E, "HITCLR" },
   { 0xD01F, "CONSOL" },
   { 0xD200, "AUDF1,POT0" },
   { 0xD201, "AUDC1,POT1" },
   { 0xD202, "AUDF2,POT2" },
   { 0xD203, "AUDC2,POT3" },
   { 0xD204, "AUDF3,POT4" },
   { 0xD205, "AUDC3,POT5" },
   { 0xD206, "AUDF4,POT6" },
   { 0xD207, "AUDC4,POT7" },
   { 0xD208, "AUDCTL,ALLPOT" },
   { 0xD209, "STIMER,KBCODE" },
   { 0xD20A, "SKREST,RANDOM" },
   { 0xD20B, "POTGO" },
   { 0xD20D, "SEROUT,SERIN" },
   { 0xD20E, "IRQEN,IRQST" },
   { 0xD20F, "SKCTL,SKSTAT" },
   { 0xD300, "PORTA" },
   { 0xD301, "PORTB" },
   { 0xD302, "PACTL" },
   { 0xD303, "PBCTL" },
   { 0xD400, "DMACTL" },
   { 0xD401, "CHACTL" },
   { 0xD402, "DLISTL" },
   { 0xD403, "DLISTH" },
   { 0xD404, "HSCROL" },
   { 0xD405, "VSCROL" },
   { 0xD407, "PMBASE" },
   { 0xD409, "CHBASE" },
   { 0xD40A, "WSYNC" },
   { 0xD40B, "VCOUNT" },
   { 0xD40C, "PENH" },
   { 0xD40D, "PENV" },
   { 0xD40E, "NMIEN" },
   { 0xD40F, "NMIST,NMIRES" },
   { 0xD800, "AFP" },
   { 0xD8E6, "FASC" },
   { 0xD9AA, "IFP" },
   { 0xD9D2, "FPI" },
   { 0xDA44, "ZFR0" },
   { 0xDA46, "ZF1" },
   { 0xDA60, "FSUB" },
   { 0xDA66, "FADD" },
   { 0xDADB, "FMUL" },
   { 0xDB28, "FDIV" },
   { 0xDD40, "PLYEVL" },
   { 0xDD89, "FLD0R" },
   { 0xDD8D, "FLD0P" },
   { 0xDD98, "FLD1R" },
   { 0xDD9C, "FLD1P" },
   { 0xDDA7, "FSTOR" },
   { 0xDDAB, "FSTOP" },
   { 0xDDB6, "FMOVE" },
   { 0xDDC0, "EXP" },
   { 0xDDCC, "EXP10" },
   { 0xDECD, "LOG" },
   { 0xDED1, "LOG10" },
   { 0xE400, "EDITRV" },
   { 0xE410, "SCRENV" },
   { 0xE420, "KEYBDV" },
   { 0xE430, "PRINTV" },
   { 0xE440, "CASETV" },
   { 0xE450, "DISKIV" },
   { 0xE453, "DSKINV" },
   { 0xE456, "CIOV" },
   { 0xE459, "SIOV" },
   { 0xE45C, "SETVBV" },
   { 0xE45F, "SYSVBV" },
   { 0xE462, "XITVBV" },
   { 0xE465, "SIOINV" },
   { 0xE468, "SENDEV" },
   { 0xE46B, "INTINV" },
   { 0xE46E, "CIOINV" },
   { 0xE471, "BLKBDV" },
   { 0xE474, "WARMSV" },
   { 0xE477, "COLDSV" },
   { 0xE47A, "RBLOKV" },
   { 0xE47D, "CSOPIV" },
   { 0xE480, "PUPDIV" },
   { 0xE483, "SLFTSV" },
   { 0xE486, "PHENTV" },
   { 0xE489, "PHULNV" },
   { 0xE48C, "PHINIV" },
   { 0xE48F, "GPDVV" },
   { 0xE7AE, "SYSVBL" },
   { 0xE7D1, "SYSVBL" },
   { 0xFFF8, "CHKSUN" },
   { 0xFFFA, "PVECT" },
   { 0xFFFB, "PVECT+1" },
};

// Labels
struct label *labels;
int num_labels;

/*
 * Prototypes
 */
void trace_code(void);


/*
 * Code
 */

/*
 * add_label()
 */
const char *add_label(const char *name,int addr)
{
   // Check if label already exists
   for (int i=0;i<num_labels;++i)
   {
      if ( labels[i].addr == addr ) return labels[i].name;
   }

   // Label name from table
   if ( !name || !*name )
   {
      // Constant table of OS labels
      for (unsigned int i=0;i<sizeof(label_table)/sizeof(label_table[0]);++i)
      {
         if ( label_table[i].addr == addr )
         {
            // Need to add the base label if it's not already there
            int j=i;
            while ( label_table[j].name[0] == '(' ) --j;
            add_label(label_table[j].name,label_table[j].addr);
            return add_label(label_table[i].name,addr);
         }
      }
   }
   
   // Default label name
   char n[16];
   if ( !name || !*name )
   {
      sprintf(n,"L%04X",addr);
      name=n;
   }

   // Add label
   labels = realloc(labels,(num_labels+1) * sizeof(labels[0]));
   if ( !labels )
   {
      fprintf(stderr,"Unable to allocate memory\n");
      exit(1);
   }
   strcpy(labels[num_labels].name,name);
   labels[num_labels].addr = addr;
   ++num_labels;
   return labels[num_labels-1].name;
}

/*
 * load_boot()
 *
 * Load into memory boot sectors.
 */
int load_boot(const unsigned char *load,int size)
{
   if ( size < 128 ) return -1; // Must be at least 128 bytes
   memcpy(&mem[4*256],load,128); // Sector 1 is first loaded into 0400-047f
   int sectors = load[1];
   if ( sectors * 128 > size ) return -1;
   int target = le16toh(*(uint16_t *)&load[2]);
   if ( target < 1 || target + size > 0xffff ) return -1;
   memcpy(&mem[target],load,sectors*128); // Sector 1 is first loaded into 0400-047f
   for (int i=0;i<sectors*128;++i) mem_loaded[target+i]=1;
   branch_target[le16toh(*(uint16_t *)&load[4])] = 1;
   add_label("BOOT_INI",le16toh(*(uint16_t *)&load[4]));
   branch_target[le16toh(*(uint16_t *)&load[6])] = 1;
   add_label("BOOT_EXEC",le16toh(*(uint16_t *)&load[6]));
   add_label("BOOT_SECS",target+1);
   add_label("BOOT_ADDR",target+2);
   // Normally don't access 0x400-0x47f in boot code, but just in case, add it if it's referenced.
   trace_code();
   int page4=0;
   for (int lab=0;lab<num_labels;++lab)
   {
      if ( labels[lab].addr >= 0x400 && labels[lab].addr < 0x480 ) page4=1;
   }
   if ( page4 ) for (int i=0x400;i<0x480;++i) mem_loaded[i]=1;
   return 0;
}

/*
 * load_blob()
 *
 * Load a blob at a given address.
 * The caller will have already set the start point(s) as branch targets and set labels for them.
 */
int load_blob(int addr,const unsigned char *load,int size)
{
   if ( addr+size > 0xffff ) return -1;
   memcpy(&mem[addr],load,size);
   for (int i=0;i<size;++i) mem_loaded[addr+i]=1;
   return 0;
}

/*
 * load_binload()
 *
 * Load a binary load file into memory.
 *
 * This will not work well if there are init routines that are later overwritten.
 * In that case, the file needs to be broken up so that the init routines can be
 * disassembled separately.
 */
int load_binload(const unsigned char *load,int size)
{
   int start,end;
   int init = 0;

   while ( size )
   {
      if ( !size ) return 0;
      if ( size < 4 ) return -1;
      start=le16toh(*(uint16_t *)&load[0]);
      if ( start == 0xffff )
      {
         load +=2;
         size -=2;
         start=le16toh(*(uint16_t *)&load[0]);
      }
      load +=2;
      size -=2;
      if ( size < 2 ) return -1;
      end=le16toh(*(uint16_t *)&load[0]);
      load +=2;
      size -=2;
      if ( size < end-start+1 ) return -1;
      if ( start == 0x2e0 && end >= 0x2e1 )
      {
         start+=2;
         size-=2;
         add_label("RUN",le16toh(*(uint16_t *)&load[0]));
         branch_target[le16toh(*(uint16_t *)&load[0])] = 1;
      }
      if ( start == 0x2e2 && end >= 0x2e3 )
      {
         char name[16];
         ++init;
         sprintf(name,"INIT%d",init);
         start+=2;
         size-=2;
         add_label(name,le16toh(*(uint16_t *)&load[0]));
         branch_target[le16toh(*(uint16_t *)&load[0])] = 1;
      }
      if ( start <= end )
      {
         memcpy(&mem[start],load,end-start+1);
         for (int i=start;i<=end;++i) mem_loaded[i]=1;
         load += end-start+1;
         size -= end-start+1;
      }
   }
   return 0;
}

/*
 * trace_at_addr()
 *
 * From a given address, trace the instruction flow forwards.
 * Stop if we encounter an instruction we've already processed.
 * Otherwise run until either a JMP, RTS, RTI, JAM, or unloaded memory.
 *
 * Note any labels for branch or data targets.
 */
void trace_at_addr(int addr)
{
   while ( mem_loaded[addr] && !instruction[addr] && addr < 0xfffe )
   {
      instruction[addr] = 1;

      // If this is a branch, flag the target
      if ( opcode[mem[addr]].mode == E_RELATIVE )
      {
         branch_target[addr+2+(signed char)(mem[addr+1])]=1;
         add_label(NULL,addr+2+(signed char)(mem[addr+1]));
      }
      // If this is a JSR, flag the target
      else if ( strcmp("JSR",opcode[mem[addr]].mnemonic) == 0 )
      {
         branch_target[le16toh(*(uint16_t *)&mem[addr+1])]=1;
         add_label(NULL,le16toh(*(uint16_t *)&mem[addr+1]));
      }
      // If this is a JMP absolute, flag the target and stop
      else if ( mem[addr] == 0x4C )
      {
         branch_target[le16toh(*(uint16_t *)&mem[addr+1])]=1;
         add_label(NULL,le16toh(*(uint16_t *)&mem[addr+1]));
      }
      // If this is a JAM, remove the instruction flag and stop
      else if ( strcmp("JAM",opcode[mem[addr]].mnemonic) == 0 )
      {
         instruction[addr] = 0;
         return;
      }
      // Do not add labels for unofficial NOP opcodes that address data
      else if ( strcmp("NOP",opcode[mem[addr]].mnemonic) == 0 )
      {
         ; // Do not add label
      }
      // Add data label
      else
      {
         switch(opcode[mem[addr]].mode)
         {
            case E_ABSOLUTE:
            case E_ABSOLUTE_X:
            case E_ABSOLUTE_Y:
               add_label(NULL,le16toh(*(uint16_t *)&mem[addr+1]));
               break;
            case E_ZEROPAGE:
            case E_ZEROPAGE_X:
            case E_ZEROPAGE_Y:
            case E_ZEROPAGE_IND_X:
            case E_ZEROPAGE_IND_Y:
               add_label(NULL,mem[addr+1]);
               break;

            default: ; // No data access
         }
      }

      // Check if done
      if ( strcmp("JMP",opcode[mem[addr]].mnemonic) == 0 ||
           strcmp("RTS",opcode[mem[addr]].mnemonic) == 0 ||
           strcmp("RTI",opcode[mem[addr]].mnemonic) == 0
         )
      {
         break;
      }

      // Next instruction
      addr += instruction_bytes[opcode[mem[addr]].mode];
   }
}

/*
 * trace_code()
 *
 * Trace through all code blocks.
 * The caller should have loaded memory and set at least one
 * branch target.
 * Keep tracing until no new branch targets are found.
 */
void trace_code(void)
{
   while(1)
   {
      int found = 0;
      for (int addr=0;addr<0xffff;++addr)
      {
         if ( mem_loaded[addr] && branch_target[addr] && !instruction[addr] )
         {
            found = 1;
            trace_at_addr(addr);
         }
      }
      if ( !found ) break;
   }
}

/*
 * fix_up_labels()
 *
 * Check for any label that is to an offset from an instruction.
 * These won't be generated during the output, so instead use a label for the
 * instruction with an offset.
 *
 * Generally these imply either self-modifying code or data incorrectly parsed as code
 */
void fix_up_labels(void)
{
   for (int lab=0;lab<num_labels;++lab)
   {
      int addr = -1;
      const char *name;
      
      if ( !mem_loaded[labels[lab].addr] ) continue;
      if ( instruction[labels[lab].addr] ) continue;
      if ( instruction[labels[lab].addr-1] && instruction_bytes[opcode[mem[labels[lab].addr-1]].mode] >= 2 ) addr = labels[lab].addr - 1;
      if ( instruction[labels[lab].addr-2] && instruction_bytes[opcode[mem[labels[lab].addr-2]].mode] >= 2 ) addr = labels[lab].addr - 2;
      if ( addr < 0 ) continue;
      name = add_label(NULL,addr);
      sprintf(labels[lab].name,"%s+%d",name,labels[lab].addr-addr);
   }
}

/*
 * sort_labels()
 */
int cmp_label(const void *lab1,const void *lab2)
{
   const struct label *l1,*l2;
   l1=lab1;
   l2=lab2;
   return (l1->addr > l2->addr);
}
void sort_labels(void)
{
   qsort(labels,num_labels,sizeof(labels[0]),cmp_label);
}


/*
 * print_label_or_addr()
 */
void print_label_or_addr(int target,int write)
{
   for (int lab=0;lab<num_labels;++lab)
   {
      if ( labels[lab].addr == target )
      {
         char *c = strchr(labels[lab].name,',');
         if ( c ) // separate read/write labels
         {
            if ( write )
            {
               printf("%s",c+1);
            }
            else
            {
               char name[16];
               strcpy(name,labels[lab].name);
               strchr(name,',')[0]=0;
               printf("%s",name);
            }
         }
         else
         {
            printf("%s",labels[lab].name);
         }
         return;
      }
   }
   if ( target < 0x100 ) printf("$%02X",target);
   else printf("$%04X",target);
}

/*
 * output_disasm()
 */
void output_disasm(void)
{
   // Display labels that are outside loaded memory
   for (int lab=0;lab<num_labels;++lab)
   {
      if ( strchr(labels[lab].name,'+') ) continue; // Don't need to print 'LABEL+1' fake labels
      if ( !mem_loaded[labels[lab].addr] )
      {
         char *c = strchr(labels[lab].name,',');
         if ( c ) // separate read/write labels
         {
            char name[16];
            strcpy(name,labels[lab].name);
            strchr(name,',')[0]=0;
            printf("%s = $%04X ; read register\n",name,labels[lab].addr);
            printf("%s = $%04X ; write register\n",c+1,labels[lab].addr);
         }
         else // normal case
         {
            printf("%s = $%04X\n",labels[lab].name,labels[lab].addr);
         }
      }
   }

   // Display loaded memory
   int set=0;
   for (int addr=0;addr<0x10000;++addr)
   {
      // If the start of a new block of loaded memory; set the address
      if ( !mem_loaded[addr] )
      {
         set=0;
         continue;
      }
      if ( !set )
      {
         printf("\t*= $%04X\n",addr);
         set=1;
      }
      // Display this address
      for (int lab=0;lab<num_labels;++lab)
      {
         if ( labels[lab].addr == addr )
         {
            printf("%s",labels[lab].name);
            break;
         }
      }
      if ( !instruction[addr] )
      {
         printf("\t"BYTE_PSEUDO_OP" $%02X",mem[addr]);
         if ( isalnum(mem[addr]) || ispunct(mem[addr]) || mem[addr]==' ' )
            printf(" ; '%c'",mem[addr]);
         printf("\n");
      }
      else if ( opcode[mem[addr]].unofficial && strcmp("NOP",opcode[mem[addr]].mnemonic) == 0 )
      {
         // NOP has multiple opcodes with the same addressing mode, so we have to
         // specify the bytes or it won't assemble to the same code
         int bytes = instruction_bytes[opcode[mem[addr]].mode];
         printf("\t"BYTE_PSEUDO_OP" ");
         for (int i=0;i<bytes;++i)
         {
            printf("%s$%02X",i?",":"",mem[addr+i]);
         }
         printf(" ; NOP (unofficial)\n");
      }
      else
      {
         printf("\t%s",opcode[mem[addr]].mnemonic);
         int target = le16toh(*(uint16_t *)&mem[addr+1]);
         int ztarget = mem[addr+1];
         int btarget = addr+2+(signed char)(mem[addr+1]);
         int write = (strncmp(opcode[mem[addr]].mnemonic,"ST",2)==0);
         switch(opcode[mem[addr]].mode)
         {
            case E_ACCUMULATOR:
               printf(" A");
               break;
            case E_IMMEDIATE:
               printf(" #$%02X",mem[addr+1]);
               break;
            case E_ABSOLUTE:
               printf(" ");
               print_label_or_addr(target,write);
               break;
            case E_ABSOLUTE_X:
               printf(" ");
               print_label_or_addr(target,write);
               printf(",X");
               break;
            case E_ABSOLUTE_Y:
               printf(" ");
               print_label_or_addr(target,write);
               printf(",Y");
               break;
            case E_INDIRECT:
               printf(" (");
               print_label_or_addr(target,0);
               printf(")");
               break;
            case E_ZEROPAGE:
               printf(" ");
               print_label_or_addr(ztarget,write);
               break;
            case E_ZEROPAGE_X:
               printf(" ");
               print_label_or_addr(ztarget,write);
               printf(",X");
               break;
            case E_ZEROPAGE_Y:
               printf(" ");
               print_label_or_addr(ztarget,write);
               printf(",Y");
               break;
            case E_ZEROPAGE_IND_X:
               printf(" (");
               print_label_or_addr(ztarget,write);
               printf(",X)");
               break;
            case E_ZEROPAGE_IND_Y:
               printf(" (");
               print_label_or_addr(ztarget,write);
               printf("),Y");
               break;
            case E_RELATIVE:
               printf(" ");
               print_label_or_addr(btarget,0);
               break;
            default: break; // Not reached
         }
         printf("\n");
         addr += instruction_bytes[opcode[mem[addr]].mode];
         --addr; // for loop also adds one
      }
   }
}

/*
 * usage()
 */
void usage(const char *progname)
{
   printf("Usage:\n"
          "\n"
          "%s [options] [file]\n"
          "\n"
          "Dissassemble [file]\n"
          "Options:\n"
          " --addr=[xxxx]   Load the file at the specified (hex) address\n"
          " --start=[xxxx]  Specify a starting address for code execution\n"
          " --start=[xxxx]  Specify another starting address (repeat as needed\n"
          "\n"
          "If no options are specified, the file is auto-parsed for type\n"
          "Supported types:\n"
          "  binary load    -- any file that starts with ffff\n"
          "  boot sectors   -- default if no other match\n"
          "\n"
          ,progname);
}

/*
 * main()
 */
int main(int argc,char *argv[])
{
   const char *progname = argv[0];
   if ( argc < 2 )
   {
      usage(progname);
      return 1;
   }

   int addr=0;
   int start=0;
   while ( argc > 2 )
   {
      if ( argv[1][0] != '-' && argv[1][1] != '-' )
      {
         usage(progname);
         return 1;
      }
      if ( strncmp(argv[1],"--addr=",sizeof("--addr=")-1) == 0 )
      {
         addr = strtol(argv[1]+sizeof("--addr=")-1,NULL,16);
         if ( addr > 0 && addr < 0xffff )
         {
            ++argv;
            --argc;
            continue;
         }
      }
      if ( strncmp(argv[1],"--start=",sizeof("--start=")-1) == 0 )
      {
         int startaddr;
         char name[20];
         startaddr = strtol(argv[1]+sizeof("--start=")-1,NULL,16);
         if ( startaddr > 0 && startaddr < 0xffff )
         {
            ++start;
            sprintf(name,"START%d",start);
            add_label(name,startaddr);
            branch_target[startaddr] = 1;

            ++argv;
            --argc;
            continue;
         }
      }
      printf("Invalid option: %s\n",argv[1]);
      usage(progname);
      return 1;
   }
   
   int fd = open(argv[1],O_RDONLY);
   if ( fd < 0 )
   {
      fprintf(stderr,"Unable to open %s\n",argv[1]);
      return 1;
   }
   struct stat statbuf;
   if ( fstat(fd,&statbuf) )
   {
      return 2;
   }
   void *data = mmap(NULL,statbuf.st_size,PROT_READ,MAP_SHARED,fd,0);

   if ( addr ) load_blob(addr,data,statbuf.st_size);
   else if ( ((uint16_t *)data)[0] == 0xffff ) load_binload(data,statbuf.st_size);
   else if ( load_boot(data,statbuf.st_size) < 0 )
   {
      fprintf(stderr,"Invalid data for boot sectors; failed to parse file type\n");
      usage(progname);
      return 1;
   }
   trace_code();
   fix_up_labels();
   sort_labels();
   output_disasm();
}
