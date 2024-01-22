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
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctype.h>
#ifdef _WIN32 // Includes 64-bit windows; macro differentiates from old 16-bit API
#define le16toh(_x) (_x) // x86 and arm Windows are little endian
#endif
#if defined(__APPLE__) && !defined(__POWERPC__)
#define le16toh(_x) (_x) // x86 and arm64 Macs are little endian
#endif
#if defined(__APPLE__) && defined(__POWERPC__) // based on post by tsom on AtariAge
#include <libkern/OSByteOrder.h>
#define le16toh(x) OSSwapLittleToHostInt16(x)
#endif
#ifndef le16toh
#include <endian.h> // This is the POSIX way
#endif

/*
 * Defines
 */

// Assembler-specific text
#define BYTE_PSEUDO_OP ".byte"
#define WORD_PSEUDO_OP ".word"
#define COMMENT ";"
#define STRING_MAX 40 // maximum number of bytes for string: .byte "Long string"

// ATASCII values that are the same in ASCII
#define IS_ATASCII(_a) ( ( (_a) >= ' ' && (_a) <= 'A' ) || isalpha(_a) || (_a) == '|' )
#define IS_QUOTABLE(_a) ( IS_ATASCII(_a) && (_a) != '"' ) // don't quote quotes in quotes

#define ARRAY_SIZE(_a) (sizeof(_a)/sizeof((_a)[0]))
#define MAX_LABEL_SIZE 32

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

/*
 * label tables can be parsed from an input file.
 *
 * LABEL = $ADDR [optional flags]
 *   flags:
 *     +[#]     for multi-byte data (# is length in decimal, $hex, or 0xHEX)
 *     -r or -w for read or write only hardware register
 *     word     for 16-bit word values (often pointers) instead of defaulting to bytes
 *     base#    specify base 2,8,10,16 for data display or 256 for string
 *     string   same as saying base256
 */
struct label {
   int addr;
   char name[MAX_LABEL_SIZE+1]; // +1 for NULL
   int bytes;
   char rw; // 'r' for read-only, 'w' for write only
   int btype; // 1: byte, 2: word
   int base; // 2,8,10,16,256 for %1000001, &0101, 65, $41, "A"
   int defined;
};

struct label_tables {
   const struct label *table;
   int entries;
};

struct syntax_options {
   int bracket;
   int noa;
   int org;
   int orgdot;
   int colon;
   int noundoc;
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

// Table of known labels; use these instead of Lxxxx
// https://atariwiki.org/wiki/Wiki.jsp?page=Memory%20Map
// There's an alogorical story about the data below.  It's the label table fable.
const struct label label_table_atari[] = {
   { 0x0000, "LINZBS", 2, 'a', 2, 16, 0 },
   { 0x0000, "LINFLG", 1, 'a', 1, 16, 0 },
   { 0x0001, "NGFLAG", 1, 'a', 1, 16, 0 },
   { 0x0002, "CASINI", 2, 'a', 2, 16, 0 },
   { 0x0004, "RAMLO", 2, 'a', 2, 16, 0 },
   { 0x0006, "TRAMSZ", 1, 'a', 1, 16, 0 },
   { 0x0007, "TSTDAT", 1, 'a', 1, 16, 0 },
   { 0x0008, "WARMST", 1, 'a', 1, 16, 0 },
   { 0x0009, "BOOT", 1, 'a', 1, 16, 0 }, // Officially "BOOT?" but I'm not sure the '?' works in all assemblers
   { 0x000A, "DOSVEC", 2, 'a', 2, 16, 0 },
   { 0x000C, "DOSINI", 2, 'a', 2, 16, 0 },
   { 0x000E, "APPMHI", 2, 'a', 2, 16, 0 },
   { 0x0010, "POKMSK", 1, 'a', 1, 16, 0 },
   { 0x0011, "BRKKEY", 1, 'a', 1, 16, 0 },
   { 0x0012, "RTCLOK", 3, 'a', 1, 16, 0 },
   { 0x0015, "BUFADR", 2, 'a', 2, 16, 0 },
   { 0x0017, "ICCOMT", 1, 'a', 1, 16, 0 },
   { 0x0018, "DSKFMS", 2, 'a', 2, 16, 0 },
   { 0x001A, "DSKUTL", 2, 'a', 2, 16, 0 },
   { 0x001C, "PTIMOT", 1, 'a', 1, 16, 0 },
   { 0x001D, "PBPNT", 1, 'a', 1, 16, 0 },
   { 0x001E, "PBUFSZ", 1, 'a', 1, 16, 0 },
   { 0x001F, "PTEMP", 1, 'a', 1, 16, 0 },
   { 0x0020, "ICHIDZ", 1, 'a', 1, 16, 0 },
   { 0x0021, "ICDNOZ", 1, 'a', 1, 16, 0 },
   { 0x0022, "ICCOMZ", 1, 'a', 1, 16, 0 },
   { 0x0023, "ICSTAZ", 1, 'a', 1, 16, 0 },
   { 0x0024, "ICBALZ", 1, 'a', 1, 16, 0 },
   { 0x0025, "ICBAHZ", 1, 'a', 1, 16, 0 },
   { 0x0026, "ICPTLZ", 1, 'a', 1, 16, 0 },
   { 0x0027, "ICPTHZ", 1, 'a', 1, 16, 0 },
   { 0x0028, "ICBLLZ", 1, 'a', 1, 16, 0 },
   { 0x0029, "ICBLHZ", 1, 'a', 1, 16, 0 },
   { 0x002A, "ICAX1Z", 1, 'a', 1, 16, 0 },
   { 0x002B, "ICAX2Z", 1, 'a', 1, 16, 0 },
   { 0x002C, "ICAX3Z", 1, 'a', 1, 16, 0 },
   { 0x002D, "ICAX4Z", 1, 'a', 1, 16, 0 },
   { 0x002E, "ICAX5Z", 1, 'a', 1, 16, 0 },
   { 0x002F, "ICAX6Z", 1, 'a', 1, 16, 0 },
   { 0x0030, "STATUS", 1, 'a', 1, 16, 0 },
   { 0x0031, "CHKSUM", 1, 'a', 1, 16, 0 },
   { 0x0032, "BUFRLO", 1, 'a', 1, 16, 0 },
   { 0x0033, "BUFRHI", 1, 'a', 1, 16, 0 },
   { 0x0034, "BFENLO", 1, 'a', 1, 16, 0 },
   { 0x0035, "BFENHI", 1, 'a', 1, 16, 0 },
   { 0x0036, "CRETRY", 1, 'a', 1, 16, 0 },
   { 0x0037, "DRETRY", 1, 'a', 1, 16, 0 },
   { 0x0036, "LTEMP", 2, 'a', 2, 16, 0 },
   { 0x0038, "BUFRFL", 1, 'a', 1, 16, 0 },
   { 0x0039, "RECVDN", 1, 'a', 1, 16, 0 },
   { 0x003A, "XMTDON", 1, 'a', 1, 16, 0 },
   { 0x003B, "CHKSNT", 1, 'a', 1, 16, 0 },
   { 0x003C, "NOCKSM", 1, 'a', 1, 16, 0 },
   { 0x003D, "BPTR", 1, 'a', 1, 16, 0 },
   { 0x003E, "FTYPE", 1, 'a', 1, 16, 0 },
   { 0x003F, "FEOF", 1, 'a', 1, 16, 0 },
   { 0x0040, "FREQ", 1, 'a', 1, 16, 0 },
   { 0x0041, "SOUNDR", 1, 'a', 1, 16, 0 },
   { 0x0042, "CRITIC", 1, 'a', 1, 16, 0 },
   //{ 0x0043-0x0049, "FMSZPG", 1, 'a', 1, 16, 0 },
   { 0x0043, "ZBUFP", 2, 'a', 2, 16, 0 },
   { 0x0045, "ZDRVA", 2, 'a', 2, 16, 0 },
   { 0x0047, "ZSBA", 2, 'a', 2, 16, 0 },
   { 0x0049, "ERRNO", 1, 'a', 1, 16, 0 },
   { 0x004A, "CKEY", 1, 'a', 1, 16, 0 },
   { 0x004B, "CASSBT", 1, 'a', 1, 16, 0 },
   { 0x004A, "ZCHAIN", 2, 'a', 2, 16, 0 },
   { 0x004C, "DSTAT", 1, 'a', 1, 16, 0 },
   { 0x004D, "ATRACT", 1, 'a', 1, 16, 0 },
   { 0x004E, "DRKMSK", 1, 'a', 1, 16, 0 },
   { 0x004F, "COLRSH", 1, 'a', 1, 16, 0 },
   { 0x0050, "TEMP", 1, 'a', 1, 16, 0 },
   { 0x0051, "HOLD1", 1, 'a', 1, 16, 0 },
   { 0x0052, "LMARGN", 1, 'a', 1, 16, 0 },
   { 0x0053, "RMARGN", 1, 'a', 1, 16, 0 },
   { 0x0054, "ROWCRS", 1, 'a', 1, 16, 0 },
   { 0x0055, "COLCRS", 2, 'a', 2, 16, 0 },
   { 0x0057, "DINDEX", 1, 'a', 1, 16, 0 },
   { 0x0058, "SAVMSC", 2, 'a', 2, 16, 0 },
   { 0x005A, "OLDROW", 1, 'a', 1, 16, 0 },
   { 0x005B, "OLDCOL", 2, 'a', 2, 16, 0 },
   { 0x005D, "OLDCHR", 1, 'a', 1, 16, 0 },
   { 0x005E, "OLDADR", 2, 'a', 2, 16, 0 },
   { 0x0060, "NEWROW", 1, 'a', 1, 16, 0 },
   { 0x0060, "FKDEF", 2, 'a', 2, 16, 0 },
   { 0x0061, "NEWCOL", 2, 'a', 2, 16, 0 },
   { 0x0062, "PALNTS", 1, 'a', 1, 16, 0 },
   { 0x0063, "LOGCOL", 1, 'a', 1, 16, 0 },
   { 0x0064, "ADRESS", 2, 'a', 2, 16, 0 },
   { 0x0066, "MLTTMP", 2, 'a', 2, 16, 0 },
   { 0x0068, "SAVADR", 2, 'a', 2, 16, 0 },
   { 0x006A, "RAMTOP", 1, 'a', 1, 16, 0 },
   { 0x006B, "BUFCNT", 1, 'a', 1, 16, 0 },
   { 0x006C, "BUFSTR", 2, 'a', 2, 16, 0 },
   { 0x006E, "BITMSK", 1, 'a', 1, 16, 0 },
   { 0x006F, "SHFAMT", 1, 'a', 1, 16, 0 },
   { 0x0070, "ROWAC", 2, 'a', 2, 16, 0 },
   { 0x0072, "COLAC", 2, 'a', 2, 16, 0 },
   { 0x0074, "ENDPT", 2, 'a', 2, 16, 0 },
   { 0x0076, "DELTAR", 1, 'a', 1, 16, 0 },
   { 0x0077, "DELTAC", 2, 'a', 2, 16, 0 },
   { 0x0079, "ROWINC", 1, 'a', 1, 16, 0 },
   { 0x007A, "COLINC", 1, 'a', 1, 16, 0 },
   { 0x0079, "KEYDEF", 2, 'a', 2, 16, 0 },
   { 0x007B, "SWPFLG", 1, 'a', 1, 16, 0 },
   { 0x007C, "HOLDCH", 1, 'a', 1, 16, 0 },
   { 0x007D, "INSDAT", 1, 'a', 1, 16, 0 },
   { 0x007E, "COUNTR", 2, 'a', 2, 16, 0 },
   { 0x0200, "VDSLST", 2, 'a', 2, 16, 0 },
   { 0x0202, "VPRCED", 2, 'a', 2, 16, 0 },
   { 0x0204, "VINTER", 2, 'a', 2, 16, 0 },
   { 0x0206, "VBREAK", 2, 'a', 2, 16, 0 },
   { 0x0208, "VKEYBD", 2, 'a', 2, 16, 0 },
   { 0x020A, "VSERIN", 2, 'a', 2, 16, 0 },
   { 0x020C, "VSEROR", 2, 'a', 2, 16, 0 },
   { 0x020E, "VSEROC", 2, 'a', 2, 16, 0 },
   { 0x0210, "VTIMR1", 2, 'a', 2, 16, 0 },
   { 0x0212, "VTIMR2", 2, 'a', 2, 16, 0 },
   { 0x0214, "VTIMR4", 2, 'a', 2, 16, 0 },
   { 0x0216, "VIMIRQ", 2, 'a', 2, 16, 0 },
   { 0x0218, "CDTMV1", 2, 'a', 2, 16, 0 },
   { 0x021A, "CDTMV2", 2, 'a', 2, 16, 0 },
   { 0x021C, "CDTMV3", 2, 'a', 2, 16, 0 },
   { 0x021E, "CDTMV4", 2, 'a', 2, 16, 0 },
   { 0x0220, "CDTMV5", 2, 'a', 2, 16, 0 },
   { 0x0222, "VVBLKI", 2, 'a', 2, 16, 0 },
   { 0x0224, "VVBLKD", 2, 'a', 2, 16, 0 },
   { 0x0226, "CDTMA1", 2, 'a', 2, 16, 0 },
   { 0x0228, "CDTMA2", 2, 'a', 2, 16, 0 },
   { 0x022A, "CDTMF3", 1, 'a', 1, 16, 0 },
   { 0x022B, "SRTIMR", 1, 'a', 1, 16, 0 },
   { 0x022C, "CDTMF4", 1, 'a', 1, 16, 0 },
   { 0x022D, "INTEMP", 1, 'a', 1, 16, 0 },
   { 0x022E, "CDTMF5", 1, 'a', 1, 16, 0 },
   { 0x022F, "SDMCTL", 1, 'a', 1, 16, 0 },
   { 0x0230, "SDLSTL", 1, 'a', 1, 16, 0 },
   { 0x0231, "SDLSTH", 1, 'a', 1, 16, 0 },
   { 0x0232, "SSKCTL", 1, 'a', 1, 16, 0 },
   { 0x0233, "SPARE", 1, 'a', 1, 16, 0 },
   { 0x0233, "LCOUNT", 1, 'a', 1, 16, 0 },
   { 0x0234, "LPENH", 1, 'a', 1, 16, 0 },
   { 0x0235, "LPENV", 1, 'a', 1, 16, 0 },
   { 0x0236, "BRKKY", 2, 'a', 2, 16, 0 },
   { 0x0238, "VPIRQ", 2, 'a', 2, 16, 0 },
   { 0x023A, "CDEVIC", 1, 'a', 1, 16, 0 },
   { 0x023B, "CCOMND", 1, 'a', 1, 16, 0 },
   { 0x023C, "CAUX1", 1, 'a', 1, 16, 0 },
   { 0x023D, "CAUX2", 1, 'a', 1, 16, 0 },
   { 0x023E, "TEMP", 1, 'a', 1, 16, 0 },
   { 0x023F, "ERRFLG", 1, 'a', 1, 16, 0 },
   { 0x0240, "DFLAGS", 1, 'a', 1, 16, 0 },
   { 0x0241, "DESECT", 1, 'a', 1, 16, 0 },
   { 0x0242, "BOOTAD", 2, 'a', 2, 16, 0 },
   { 0x0244, "COLDST", 1, 'a', 1, 16, 0 },
   { 0x0245, "RECLEN", 1, 'a', 1, 16, 0 },
   { 0x0246, "DSKTIM", 1, 'a', 1, 16, 0 },
   //{ 0x0247-0x026E, "LINBUF", 1, 'a', 1, 16, 0 }, // 40-character line buffer; not sure about what the other stuff in the same range is
   { 0x0247, "PDVMSK", 1, 'a', 1, 16, 0 },
   { 0x0248, "SHPDVS", 1, 'a', 1, 16, 0 },
   { 0x0249, "PDIMSK", 1, 'a', 1, 16, 0 },
   { 0x024A, "RELADR", 2, 'a', 2, 16, 0 },
   { 0x024C, "PPTMPA", 1, 'a', 1, 16, 0 },
   { 0x024D, "PPTMPX", 1, 'a', 1, 16, 0 },
   { 0x026B, "CHSALT", 1, 'a', 1, 16, 0 },
   { 0x026C, "VSFLAG", 1, 'a', 1, 16, 0 },
   { 0x026D, "KEYDIS", 1, 'a', 1, 16, 0 },
   { 0x026E, "FINE", 1, 'a', 1, 16, 0 },
   { 0x026F, "GPRIOR", 1, 'a', 1, 16, 0 },
   { 0x0270, "PADDL0", 1, 'a', 1, 16, 0 },
   { 0x0271, "PADDL1", 1, 'a', 1, 16, 0 },
   { 0x0272, "PADDL2", 1, 'a', 1, 16, 0 },
   { 0x0273, "PADDL3", 1, 'a', 1, 16, 0 },
   { 0x0274, "PADDL4", 1, 'a', 1, 16, 0 },
   { 0x0275, "PADDL5", 1, 'a', 1, 16, 0 },
   { 0x0276, "PADDL6", 1, 'a', 1, 16, 0 },
   { 0x0277, "PADDL7", 1, 'a', 1, 16, 0 },
   { 0x0278, "STICK0", 1, 'a', 1, 16, 0 },
   { 0x0279, "STICK1", 1, 'a', 1, 16, 0 },
   { 0x027A, "STICK2", 1, 'a', 1, 16, 0 },
   { 0x027B, "STICK3", 1, 'a', 1, 16, 0 },
   { 0x027C, "PTRIG0", 1, 'a', 1, 16, 0 },
   { 0x027D, "PTRIG1", 1, 'a', 1, 16, 0 },
   { 0x027E, "PTRIG2", 1, 'a', 1, 16, 0 },
   { 0x027F, "PTRIG3", 1, 'a', 1, 16, 0 },
   { 0x0280, "PTRIG4", 1, 'a', 1, 16, 0 },
   { 0x0281, "PTRIG5", 1, 'a', 1, 16, 0 },
   { 0x0282, "PTRIG6", 1, 'a', 1, 16, 0 },
   { 0x0283, "PTRIG7", 1, 'a', 1, 16, 0 },
   { 0x0284, "STRIG0", 1, 'a', 1, 16, 0 },
   { 0x0285, "STRIG1", 1, 'a', 1, 16, 0 },
   { 0x0286, "STRIG2", 1, 'a', 1, 16, 0 },
   { 0x0287, "STRIG3", 1, 'a', 1, 16, 0 },
   { 0x0288, "CSTAT", 1, 'a', 1, 16, 0 },
   { 0x0288, "HIBZTE", 1, 'a', 1, 16, 0 },
   { 0x0289, "WMODE", 1, 'a', 1, 16, 0 },
   { 0x028A, "BLIM", 1, 'a', 1, 16, 0 },
   { 0x028B, "IMASK", 1, 'a', 1, 16, 0 },
   { 0x028C, "JVECK", 2, 'a', 2, 16, 0 },
   { 0x028E, "NEWADR", 2, 'a', 2, 16, 0 },
   { 0x0290, "TXTROW", 1, 'a', 1, 16, 0 },
   { 0x0291, "TXTCOL", 2, 'a', 2, 16, 0 },
   { 0x0293, "TINDEX", 1, 'a', 1, 16, 0 },
   { 0x0294, "TXTMSC", 2, 'a', 2, 16, 0 },
   { 0x0296, "TXTOLD", 6, 'a', 1, 16, 0 }, // used for cursor position in split screen mode
   { 0x029C, "TMPX1", 1, 'a', 1, 16, 0 },
   { 0x029C, "CRETRY", 1, 'a', 1, 16, 0 },
   { 0x029D, "HOLD3", 1, 'a', 1, 16, 0 },
   { 0x029E, "SUBTMP", 1, 'a', 1, 16, 0 },
   { 0x029F, "HOLD2", 1, 'a', 1, 16, 0 },
   { 0x02A0, "DMASK", 1, 'a', 1, 16, 0 },
   { 0x02A1, "TMPLBT", 1, 'a', 1, 16, 0 },
   { 0x02A2, "ESCFLG", 1, 'a', 1, 16, 0 },
   { 0x02A3, "TABMAP", 15, 'a', 1, 16, 0 }, // map of tab positions
   { 0x02B2, "LOGMAP", 4, 'a', 1, 16, 0 }, // map of logical line breaks
   { 0x02B6, "INVFLG", 1, 'a', 1, 16, 0 },
   { 0x02B7, "FILFLG", 1, 'a', 1, 16, 0 },
   { 0x02B8, "TMPROW", 1, 'a', 1, 16, 0 },
   { 0x02B9, "TMPCOL", 2, 'a', 2, 16, 0 },
   { 0x02BB, "SCRFLG", 1, 'a', 1, 16, 0 },
   { 0x02BC, "HOLD4", 1, 'a', 1, 16, 0 },
   { 0x02BD, "HOLD5", 1, 'a', 1, 16, 0 },
   { 0x02BD, "DRETRY", 1, 'a', 1, 16, 0 },
   { 0x02BE, "SHFLOK", 1, 'a', 1, 16, 0 },
   { 0x02BF, "BOTSCR", 1, 'a', 1, 16, 0 },
   { 0x02C0, "PCOLR0", 1, 'a', 1, 16, 0 },
   { 0x02C1, "PCOLR1", 1, 'a', 1, 16, 0 },
   { 0x02C2, "PCOLR2", 1, 'a', 1, 16, 0 },
   { 0x02C3, "PCOLR3", 1, 'a', 1, 16, 0 },
   { 0x02C4, "COLOR0", 1, 'a', 1, 16, 0 },
   { 0x02C5, "COLOR1", 1, 'a', 1, 16, 0 },
   { 0x02C6, "COLOR2", 1, 'a', 1, 16, 0 },
   { 0x02C7, "COLOR3", 1, 'a', 1, 16, 0 },
   { 0x02C8, "COLOR4", 1, 'a', 1, 16, 0 },
   { 0x02C9, "RUNADR", 2, 'a', 2, 16, 0 },
   { 0x02CB, "HIUSED", 2, 'a', 2, 16, 0 },
   { 0x02CD, "ZHIUSE", 2, 'a', 2, 16, 0 },
   { 0x02CF, "GBYTEA", 2, 'a', 2, 16, 0 },
   { 0x02D1, "LOADAD", 2, 'a', 2, 16, 0 },
   { 0x02D3, "ZLOADA", 2, 'a', 2, 16, 0 },
   { 0x02D5, "DSCTLN", 2, 'a', 2, 16, 0 },
   { 0x02D7, "ACMISR", 2, 'a', 2, 16, 0 },
   { 0x02D9, "KRPDEL", 1, 'a', 1, 16, 0 },
   { 0x02DA, "KEYREP", 1, 'a', 1, 16, 0 },
   { 0x02DB, "NOCLIK", 1, 'a', 1, 16, 0 },
   { 0x02FC, "HELPFG", 1, 'a', 1, 16, 0 },
   { 0x02DD, "DMASAV", 1, 'a', 1, 16, 0 },
   { 0x02DE, "PBPNT", 1, 'a', 1, 16, 0 },
   { 0x02DF, "PBUFSZ", 1, 'a', 1, 16, 0 },
   { 0x02E0, "RUNAD", 2, 'a', 2, 16, 0 },
   { 0x02E2, "INITAD", 2, 'a', 2, 16, 0 },
   { 0x02E4, "RAMSIZ", 1, 'a', 1, 16, 0 },
   { 0x02E5, "MEMTOP", 2, 'a', 2, 16, 0 },
   { 0x02E7, "MEMLO", 2, 'a', 2, 16, 0 },
   { 0x02E9, "HNDLOD", 1, 'a', 1, 16, 0 },
   { 0x02EA, "DVSTAT", 4, 'a', 1, 16, 0 },
   { 0x02EE, "CBAUDL", 1, 'a', 1, 16, 0 },
   { 0x02EF, "CBAUDH", 1, 'a', 1, 16, 0 },
   { 0x02F0, "CRSINH", 1, 'a', 1, 16, 0 },
   { 0x02F1, "KEYDEL", 1, 'a', 1, 16, 0 },
   { 0x02F2, "CH1", 1, 'a', 1, 16, 0 },
   { 0x02F3, "CHACT", 1, 'a', 1, 16, 0 },
   { 0x02F4, "CHBAS", 1, 'a', 1, 16, 0 },
   { 0x02F5, "NEWROW", 1, 'a', 1, 16, 0 },
   { 0x02F6, "NEWCOL", 2, 'a', 2, 16, 0 },
   { 0x02F8, "ROWINC", 1, 'a', 1, 16, 0 },
   { 0x02F9, "COLINC", 1, 'a', 1, 16, 0 },
   { 0x02FA, "CHAR", 1, 'a', 1, 16, 0 },
   { 0x02FB, "ATACHR", 1, 'a', 1, 16, 0 },
   { 0x02FC, "CH", 1, 'a', 1, 16, 0 },
   { 0x02FD, "FILDAT", 1, 'a', 1, 16, 0 },
   { 0x02FE, "DSPFLG", 1, 'a', 1, 16, 0 },
   { 0x02FF, "SSFLAG", 1, 'a', 1, 16, 0 },
   { 0x0300, "DDEVIC", 1, 'a', 1, 16, 0 },
   { 0x0301, "DUNIT", 1, 'a', 1, 16, 0 },
   { 0x0302, "DCOMND", 1, 'a', 1, 16, 0 },
   { 0x0303, "DSTATS", 1, 'a', 1, 16, 0 },
   { 0x0304, "DBUFLO", 1, 'a', 1, 16, 0 },
   { 0x0305, "DBUFHI", 1, 'a', 1, 16, 0 },
   { 0x0306, "DTIMLO", 1, 'a', 1, 16, 0 },
   { 0x0307, "DUNUSE", 1, 'a', 1, 16, 0 },
   { 0x0308, "DBYTLO", 1, 'a', 1, 16, 0 },
   { 0x0309, "DBYTHI", 1, 'a', 1, 16, 0 },
   { 0x030A, "DAUX1", 1, 'a', 1, 16, 0 },
   { 0x030B, "DAUX2", 1, 'a', 1, 16, 0 },
   { 0x030C, "TIMER1", 2, 'a', 2, 16, 0 },
   { 0x030E, "ADDCOR", 1, 'a', 1, 16, 0 },
   { 0x030E, "JMPERS", 1, 'a', 1, 16, 0 },
   { 0x030F, "CASFLG", 1, 'a', 1, 16, 0 },
   { 0x0310, "TIMER2", 2, 'a', 2, 16, 0 },
   { 0x0312, "TEMP1", 2, 'a', 2, 16, 0 },
   { 0x0314, "TEMP2", 1, 'a', 1, 16, 0 },
   { 0x0314, "PTIMOT", 1, 'a', 1, 16, 0 },
   { 0x0315, "TEMP3", 1, 'a', 1, 16, 0 },
   { 0x0316, "SAVIO", 1, 'a', 1, 16, 0 },
   { 0x0317, "TIMFLG", 1, 'a', 1, 16, 0 },
   { 0x0318, "STACKP", 1, 'a', 1, 16, 0 },
   { 0x0319, "TSTAT", 1, 'a', 1, 16, 0 },
   { 0x03E8, "SUPERF", 1, 'a', 1, 16, 0 },
   { 0x03E9, "CKEY", 1, 'a', 1, 16, 0 },
   { 0x03EA, "CASSBT", 1, 'a', 1, 16, 0 },
   { 0x03EB, "CARTCK", 1, 'a', 1, 16, 0 },
   { 0x03EC, "DEERF", 1, 'a', 1, 16, 0 },
   //{ 0x03ED-0x03F7, "ACMVAR", 1, 'a', 1, 16, 0 },
   { 0x03F8, "BASICF", 1, 'a', 1, 16, 0 },
   { 0x03F9, "MINTLK", 1, 'a', 1, 16, 0 },
   { 0x03FA, "GINTLK", 1, 'a', 1, 16, 0 },
   { 0x03FB, "CHLINK", 2, 'a', 2, 16, 0 },
   { 0x057E, "LBPR1", 1, 'a', 1, 16, 0 },
   { 0x057F, "LBPR2", 1, 'a', 1, 16, 0 },
   { 0x05E0, "PLYARG", 1, 'a', 1, 16, 0 },
   { 0xD000, "HPOSP0", 1, 'w', 1, 16, 0 },
   { 0xD000, "M0PF", 1, 'r', 1, 16, 0 },
   { 0xD001, "HPOSP1", 1, 'w', 1, 16, 0 },
   { 0xD001, "M1PF", 1, 'r', 1, 16, 0 },
   { 0xD002, "HPOSP2", 1, 'w', 1, 16, 0 },
   { 0xD002, "M2PF", 1, 'r', 1, 16, 0 },
   { 0xD003, "HPOSP3", 1, 'w', 1, 16, 0 },
   { 0xD003, "M3PF", 1, 'r', 1, 16, 0 },
   { 0xD004, "HPOSM0", 1, 'w', 1, 16, 0 },
   { 0xD004, "P0PF", 1, 'r', 1, 16, 0 },
   { 0xD005, "HPOSM1", 1, 'w', 1, 16, 0 },
   { 0xD005, "P1PF", 1, 'r', 1, 16, 0 },
   { 0xD006, "HPOSM2", 1, 'w', 1, 16, 0 },
   { 0xD006, "P2PF", 1, 'r', 1, 16, 0 },
   { 0xD007, "HPOSM3", 1, 'w', 1, 16, 0 },
   { 0xD007, "P3PF", 1, 'r', 1, 16, 0 },
   { 0xD008, "SIZEP0", 1, 'w', 1, 16, 0 },
   { 0xD008, "M0PL", 1, 'r', 1, 16, 0 },
   { 0xD009, "SIZEP1", 1, 'w', 1, 16, 0 },
   { 0xD009, "M1PL", 1, 'r', 1, 16, 0 },
   { 0xD00A, "SIZEP2", 1, 'w', 1, 16, 0 },
   { 0xD00A, "M2PL", 1, 'r', 1, 16, 0 },
   { 0xD00B, "SIZEP3", 1, 'w', 1, 16, 0 },
   { 0xD00B, "M3PL", 1, 'r', 1, 16, 0 },
   { 0xD00C, "SIZEM", 1, 'w', 1, 16, 0 },
   { 0xD00C, "P0PL", 1, 'r', 1, 16, 0 },
   { 0xD00D, "GRAFP0", 1, 'w', 1, 16, 0 },
   { 0xD00D, "P1PL", 1, 'r', 1, 16, 0 },
   { 0xD00E, "GRAFP1", 1, 'w', 1, 16, 0 },
   { 0xD00E, "P2PL", 1, 'r', 1, 16, 0 },
   { 0xD00F, "GRAFP2", 1, 'w', 1, 16, 0 },
   { 0xD00F, "P3PL", 1, 'r', 1, 16, 0 },
   { 0xD010, "GRAFP3", 1, 'w', 1, 16, 0 },
   { 0xD010, "TRIG0", 1, 'r', 1, 16, 0 },
   { 0xD011, "GRAFM", 1, 'w', 1, 16, 0 },
   { 0xD011, "TRIG1", 1, 'r', 1, 16, 0 },
   { 0xD012, "COLPM0", 1, 'w', 1, 16, 0 },
   { 0xD012, "TRIG2", 1, 'r', 1, 16, 0 },
   { 0xD013, "COLPM1", 1, 'w', 1, 16, 0 },
   { 0xD013, "TRIG3", 1, 'r', 1, 16, 0 },
   { 0xD014, "COLPM2", 1, 'w', 1, 16, 0 },
   { 0xD014, "PAL", 1, 'r', 1, 16, 0 },
   { 0xD015, "COLPM3", 1, 'a', 1, 16, 0 },
   { 0xD016, "COLPF0", 1, 'a', 1, 16, 0 },
   { 0xD017, "COLPF1", 1, 'a', 1, 16, 0 },
   { 0xD018, "COLPF2", 1, 'a', 1, 16, 0 },
   { 0xD019, "COLPF3", 1, 'a', 1, 16, 0 },
   { 0xD01A, "COLBK", 1, 'a', 1, 16, 0 },
   { 0xD01B, "PRIOR", 1, 'a', 1, 16, 0 },
   { 0xD01C, "VDELAY", 1, 'a', 1, 16, 0 },
   { 0xD01D, "GRACTL", 1, 'a', 1, 16, 0 },
   { 0xD01E, "HITCLR", 1, 'a', 1, 16, 0 },
   { 0xD01F, "CONSOL", 1, 'a', 1, 16, 0 },
   { 0xD200, "AUDF1", 1, 'w', 1, 16, 0 },
   { 0xD200, "POT0", 1, 'r', 1, 16, 0 },
   { 0xD201, "AUDC1", 1, 'w', 1, 16, 0 },
   { 0xD201, "POT1", 1, 'r', 1, 16, 0 },
   { 0xD202, "AUDF2", 1, 'w', 1, 16, 0 },
   { 0xD202, "POT2", 1, 'r', 1, 16, 0 },
   { 0xD203, "AUDC2", 1, 'w', 1, 16, 0 },
   { 0xD203, "POT3", 1, 'r', 1, 16, 0 },
   { 0xD204, "AUDF3", 1, 'w', 1, 16, 0 },
   { 0xD204, "POT4", 1, 'r', 1, 16, 0 },
   { 0xD205, "AUDC3", 1, 'w', 1, 16, 0 },
   { 0xD205, "POT5", 1, 'r', 1, 16, 0 },
   { 0xD206, "AUDF4", 1, 'w', 1, 16, 0 },
   { 0xD206, "POT6", 1, 'r', 1, 16, 0 },
   { 0xD207, "AUDC4", 1, 'w', 1, 16, 0 },
   { 0xD207, "POT7", 1, 'r', 1, 16, 0 },
   { 0xD208, "AUDCTL", 1, 'w', 1, 16, 0 },
   { 0xD208, "ALLPOT", 1, 'r', 1, 16, 0 },
   { 0xD209, "STIMER", 1, 'w', 1, 16, 0 },
   { 0xD209, "KBCODE", 1, 'r', 1, 16, 0 },
   { 0xD20A, "SKREST", 1, 'w', 1, 16, 0 },
   { 0xD20A, "RANDOM", 1, 'r', 1, 16, 0 },
   { 0xD20B, "POTGO", 1, 'a', 1, 16, 0 },
   { 0xD20D, "SEROUT", 1, 'w', 1, 16, 0 },
   { 0xD20D, "SERIN", 1, 'r', 1, 16, 0 },
   { 0xD20E, "IRQEN", 1, 'w', 1, 16, 0 },
   { 0xD20E, "IRQST", 1, 'r', 1, 16, 0 },
   { 0xD20F, "SKCTL", 1, 'w', 1, 16, 0 },
   { 0xD20F, "SKSTAT", 1, 'r', 1, 16, 0 },
   { 0xD300, "PORTA", 1, 'a', 1, 16, 0 },
   { 0xD301, "PORTB", 1, 'a', 1, 16, 0 },
   { 0xD302, "PACTL", 1, 'a', 1, 16, 0 },
   { 0xD303, "PBCTL", 1, 'a', 1, 16, 0 },
   { 0xD400, "DMACTL", 1, 'a', 1, 16, 0 },
   { 0xD401, "CHACTL", 1, 'a', 1, 16, 0 },
   { 0xD402, "DLISTL", 1, 'a', 1, 16, 0 },
   { 0xD403, "DLISTH", 1, 'a', 1, 16, 0 },
   { 0xD404, "HSCROL", 1, 'a', 1, 16, 0 },
   { 0xD405, "VSCROL", 1, 'a', 1, 16, 0 },
   { 0xD407, "PMBASE", 1, 'a', 1, 16, 0 },
   { 0xD409, "CHBASE", 1, 'a', 1, 16, 0 },
   { 0xD40A, "WSYNC", 1, 'a', 1, 16, 0 },
   { 0xD40B, "VCOUNT", 1, 'a', 1, 16, 0 },
   { 0xD40C, "PENH", 1, 'a', 1, 16, 0 },
   { 0xD40D, "PENV", 1, 'a', 1, 16, 0 },
   { 0xD40E, "NMIEN", 1, 'a', 1, 16, 0 },
   { 0xD40F, "NMIST", 1, 'w', 1, 16, 0 },
   { 0xD40F, "NMIRES", 1, 'r', 1, 16, 0 },
   { 0xD800, "AFP", 1, 'a', 1, 16, 0 },
   { 0xD8E6, "FASC", 1, 'a', 1, 16, 0 },
   { 0xD9AA, "IFP", 1, 'a', 1, 16, 0 },
   { 0xD9D2, "FPI", 1, 'a', 1, 16, 0 },
   { 0xDA44, "ZFR0", 1, 'a', 1, 16, 0 },
   { 0xDA46, "ZF1", 1, 'a', 1, 16, 0 },
   { 0xDA60, "FSUB", 1, 'a', 1, 16, 0 },
   { 0xDA66, "FADD", 1, 'a', 1, 16, 0 },
   { 0xDADB, "FMUL", 1, 'a', 1, 16, 0 },
   { 0xDB28, "FDIV", 1, 'a', 1, 16, 0 },
   { 0xDD40, "PLYEVL", 1, 'a', 1, 16, 0 },
   { 0xDD89, "FLD0R", 1, 'a', 1, 16, 0 },
   { 0xDD8D, "FLD0P", 1, 'a', 1, 16, 0 },
   { 0xDD98, "FLD1R", 1, 'a', 1, 16, 0 },
   { 0xDD9C, "FLD1P", 1, 'a', 1, 16, 0 },
   { 0xDDA7, "FSTOR", 1, 'a', 1, 16, 0 },
   { 0xDDAB, "FSTOP", 1, 'a', 1, 16, 0 },
   { 0xDDB6, "FMOVE", 1, 'a', 1, 16, 0 },
   { 0xDDC0, "EXP", 1, 'a', 1, 16, 0 },
   { 0xDDCC, "EXP10", 1, 'a', 1, 16, 0 },
   { 0xDECD, "LOG", 1, 'a', 1, 16, 0 },
   { 0xDED1, "LOG10", 1, 'a', 1, 16, 0 },
   { 0xE400, "EDITRV", 1, 'a', 1, 16, 0 },
   { 0xE410, "SCRENV", 1, 'a', 1, 16, 0 },
   { 0xE420, "KEYBDV", 1, 'a', 1, 16, 0 },
   { 0xE430, "PRINTV", 1, 'a', 1, 16, 0 },
   { 0xE440, "CASETV", 1, 'a', 1, 16, 0 },
   { 0xE450, "DISKIV", 1, 'a', 1, 16, 0 },
   { 0xE453, "DSKINV", 1, 'a', 1, 16, 0 },
   { 0xE456, "CIOV", 1, 'a', 1, 16, 0 },
   { 0xE459, "SIOV", 1, 'a', 1, 16, 0 },
   { 0xE45C, "SETVBV", 1, 'a', 1, 16, 0 },
   { 0xE45F, "SYSVBV", 1, 'a', 1, 16, 0 },
   { 0xE462, "XITVBV", 1, 'a', 1, 16, 0 },
   { 0xE465, "SIOINV", 1, 'a', 1, 16, 0 },
   { 0xE468, "SENDEV", 1, 'a', 1, 16, 0 },
   { 0xE46B, "INTINV", 1, 'a', 1, 16, 0 },
   { 0xE46E, "CIOINV", 1, 'a', 1, 16, 0 },
   { 0xE471, "BLKBDV", 1, 'a', 1, 16, 0 },
   { 0xE474, "WARMSV", 1, 'a', 1, 16, 0 },
   { 0xE477, "COLDSV", 1, 'a', 1, 16, 0 },
   { 0xE47A, "RBLOKV", 1, 'a', 1, 16, 0 },
   { 0xE47D, "CSOPIV", 1, 'a', 1, 16, 0 },
   { 0xE480, "PUPDIV", 1, 'a', 1, 16, 0 },
   { 0xE483, "SLFTSV", 1, 'a', 1, 16, 0 },
   { 0xE486, "PHENTV", 1, 'a', 1, 16, 0 },
   { 0xE489, "PHULNV", 1, 'a', 1, 16, 0 },
   { 0xE48C, "PHINIV", 1, 'a', 1, 16, 0 },
   { 0xE48F, "GPDVV", 1, 'a', 1, 16, 0 },
   { 0xE7AE, "SYSVBL", 1, 'a', 1, 16, 0 },
   { 0xE7D1, "SYSVBL", 1, 'a', 1, 16, 0 },
   { 0xFFF8, "CHKSUN", 1, 'a', 1, 16, 0 },
   { 0xFFFA, "PVECT", 2, 'a', 2, 16, 0 },
};
const struct label label_table_atari_cio[] = {
   { 0x031A-0x033F, "HATABS", 27, 'a', 1, 16, 0 }, // Handler address tables, 3 bytes per entry, two zeros at end
   { 0x0340, "IOCB0", 16, 'a', 1, 16, 0 },
   { 0x0350, "IOCB1", 16, 'a', 1, 16, 0 },
   { 0x0360, "IOCB2", 16, 'a', 1, 16, 0 },
   { 0x0370, "IOCB3", 16, 'a', 1, 16, 0 },
   { 0x0380, "IOCB4", 16, 'a', 1, 16, 0 },
   { 0x0390, "IOCB5", 16, 'a', 1, 16, 0 },
   { 0x03A0, "IOCB6", 16, 'a', 1, 16, 0 },
   { 0x03B0, "IOCB7", 16, 'a', 1, 16, 0 },
   { 0x03C0-0x03E7, "PRNBUF", 40, 'a', 1, 16, 0 }, // printer buffer for LPRINT statements; is this BASIC or CIO?
   //{ 0x03FD-0x047F, "CASBUF", 1, 'a', 1, 16, 0 }, // 3FD-3FF are also used for other stuff
};
const struct label label_table_atari_float[] = {
   { 0x00D4, "FR0", 6, 'a', 1, 16, 0 },
   { 0x00DA, "FRE", 6, 'a', 1, 16, 0 },
   { 0x00E0, "FR1", 6, 'a', 1, 16, 0 },
   { 0x00E6, "FR2", 6, 'a', 1, 16, 0 },
   { 0x00EC, "FRX", 1, 'a', 1, 16, 0 },
   { 0x00ED, "EEXP", 1, 'a', 1, 16, 0 },
   { 0x00EE, "NSIGN", 1, 'a', 1, 16, 0 },
   { 0x00EF, "ESIGN", 1, 'a', 1, 16, 0 },
   { 0x00F0, "FCHRFLG", 1, 'a', 1, 16, 0 },
   { 0x00F1, "DIGRT", 1, 'a', 1, 16, 0 },
   { 0x00F2, "CIX", 1, 'a', 1, 16, 0 },
   { 0x00F3, "INBUFF", 2, 'a', 2, 16, 0 },
   { 0x00F5, "ZTEMP1", 2, 'a', 2, 16, 0 },
   { 0x00F7, "ZTEMP4", 2, 'a', 2, 16, 0 },
   { 0x00F9, "ZTEMP3", 2, 'a', 2, 16, 0 },
   { 0x00FB, "RADFLG", 1, 'a', 1, 16, 0 },
   { 0x00FC, "FLPTR", 2, 'a', 2, 16, 0 },
   { 0x00FE, "FPTR2", 2, 'a', 2, 16, 0 },
   { 0x05E6, "FPSCR", 6, 'a', 1, 16, 0 },
   { 0x05EC, "FPSCR1", 4, 'a', 1, 16, 0 },
};
const struct label label_table_atari_basic[] = {
   { 0x0080, "LOMEM", 2, 'a', 2, 16, 0 },
   { 0x0082, "VNTP", 2, 'a', 2, 16, 0 },
   { 0x0084, "VNTD", 2, 'a', 2, 16, 0 },
   { 0x0086, "VVTP", 2, 'a', 2, 16, 0 },
   { 0x0088, "STMTAB", 2, 'a', 2, 16, 0 },
   { 0x0090, "MEMTOP", 2, 'a', 2, 16, 0 },
   { 0x008A, "STMCUR", 2, 'a', 2, 16, 0 },
   { 0x008C, "STARP", 2, 'a', 2, 16, 0 },
   { 0x008E, "RUNSTK", 2, 'a', 2, 16, 0 },
   { 0x00BA, "STOPLN", 2, 'a', 2, 16, 0 },
   { 0x00C3, "ERRSAVE", 1, 'a', 1, 16, 0 },
   { 0x00C9, "PTABW", 1, 'a', 1, 16, 0 },
   { 0x0580, "LBUFF", 128, 'a', 1, 16, 0 }, // BASIC line buffer
};

// Templates for 'orig' value when adding new labels
const struct label label_dec = { -1, "", 1, 'a', 1, 10, 0 };
const struct label label_word = { -1, "", 2, 'a', 2, 16, 0 };

/*
 * global variables
 */

// The 6502 memory
unsigned char mem[64*1024];

// Flags per-byte
uint16_t mem_loaded[64*1024];
char instruction[64*1024]; // first byte of an instruction?
char branch_target[64*1024];
char data_target[64*1024];

// Options
struct syntax_options syntax;

// Labels
struct label *labels;
int num_labels;
struct label_tables *label_tables;
int num_tables;

/*
 * Prototypes
 */
void trace_code(void);
void fix_up_labels(void);
void sort_labels(void);
void output_disasm(void);


/*
 * Code
 */

/*
 * add_table()
 */
void add_table(const struct label *table,int entries)
{
   // Skip if table already added:
   for (int i=0;i<num_tables;++i) if ( label_tables[i].table == table ) return;

   // Add the specified table
   label_tables = realloc(label_tables,(num_tables+1)*sizeof(label_tables[0]));
   if ( !label_tables )
   {
      fprintf(stderr,"Unable to allocate label table memory\n");
      exit(1);
   }
   label_tables[num_tables].table = table;
   label_tables[num_tables].entries = entries;
   ++num_tables;
}

int add_label_file(const char *filename)
{
   if ( !filename || !filename[0] )
   {
      printf("Missing label file specification\n");
      return -1;
   }
   FILE *f = fopen(filename,"r");
   if ( !f )
   {
      fprintf(stderr,"Unable to open %s\n",filename);
      return -2;
   }

   struct label *table = NULL;
   int entries = 0;
   char line[1024];
   char line_orig[1024];
   char *c,*equal;
   while ( fgets(line,sizeof(line),f) )
   {
      strcpy(line_orig,line); // Save to print out in case of parsing error
      // entry defaults:
      char name[MAX_LABEL_SIZE+2];
      int bytes = 1; // default to 1 byte
      char rw = 'a'; // any; could also be r or w
      int display = 1; // default to byte, not word
      int base = 16; // valid: 2, 8, 10, 16, 256 (string)
      int addr;

      /*
       * line format:
       *
       * LABEL = $xxxx+2-w word base16
       * spaces around '=' are optional, as are spaces before '+' and '-'
       * order of +, -, word, base (or string) are arbitrary
       * Anything following a '#' or ';' is a comment
       * Any line without an '=' character is ignored
       */
      // Remove comments
      c=strchr(line,'#');
      if ( c ) *c = 0;
      c=strchr(line,';');
      if ( c ) *c = 0;
      equal = strchr(line,'=');
      if ( !equal ) continue;

      // Read label text

      // skip leading spaces
      c=line;
      while ( isspace(*c) ) ++c;
      // move equal backwards to kill spaces
      while ( equal > c && isspace(equal[-1]) )
      {
         equal[-1] = '=';
         equal[0] = ' ';
         --equal;
      }
      if ( c == equal )
      {
         printf("Invalid label line: No label\n");
         printf("  %s\n",line_orig);
         fclose(f);
         return -3;
      }
      strncpy(name,c,sizeof(name));
      char *e = strchr(name,'=');
      if ( e ) *e=0;
      else
      {
         printf("Invalid label line: label too long (limit: %d)\n",MAX_LABEL_SIZE);
         printf("  %s\n",line_orig);
         fclose(f);
         return -3;
      }

      // Read value

      // skip leading spaces
      c=equal+1;
      while ( isspace(*c) ) ++c;
      // get address/value
      {
         char *end;
         if ( *c == '$' ) // allow '$' for hex
         {
            addr = strtol(c+1,&end,16);
         }
         else
         {
            addr = strtol(c,&end,0);
         }
         if ( end == c || addr < 0 || addr > 0xffff )
         {
            printf("Invalid label line: address/value invalid\n");
            printf("  %s\n",line_orig);
            fclose(f);
            return -3;
         }
         equal = end;
      }

      // -r, -w, or -a for hardware registers
      c=strchr(equal,'-');
      if ( c )
      {
         if ( c[1] == 'a' || c[1] == 'r' || c[1] == 'w' )
         {
            rw = c[1];
            c[0]=' ';
            c[1]=' ';
         }
         else
         {
            printf("Invalid label line: '-' must be followed by a, r, or w.\n");
            printf("  %s\n",line_orig);
            fclose(f);
            return -3;
         }
      }

      // +n for multi-byte labels
      c=strchr(equal,'+');
      if ( c )
      {
         char *end;
         if ( c[1] == '$' ) // allow '$' for hex
         {
            bytes = strtol(c+2,&end,16);
         }
         else
         {
            bytes = strtol(c+1,&end,0);
         }
         if ( bytes < 1 || bytes > 16*1024 )
         {
            printf("Invalid label line: +size must be from 1 to 16K\n");
            printf("  %s\n",line_orig);
            fclose(f);
            return -3;
         }
         while ( c < end ) *c++ = ' '; // wipe option
      }

      // 'baseX' to specify the base
      c=strstr(equal,"base");
      if ( c )
      {
         base = strtol(c+4,NULL,10);
         switch(base) {
            case 2:
            case 8:
            case 10:
            case 16:
            case 256:
               break;
            default:
               printf("Invalid label line: invalid base\n");
               printf("  %s\n",line_orig);
               fclose(f);
               return -3;
         }
         for (int i=0;i<4;++i) *c++=' ';
         while (isdigit(*c)) *c++=' ';
      }

      // 'string' to specify base 256
      c=strstr(equal,"string");
      if ( c )
      {
         base = 256;
         for (int i=0;i<6;++i) *c++=' ';
      }

      // word to specify .word instead of .byte for data
      c=strstr(equal,"word");
      if ( c )
      {
         display = 2;
         for (int i=0;i<4;++i) *c++=' ';
      }

      // Check for garbage
      c=equal+1;
      while ( isspace(*c) ) ++c;
      if (*c)
      {
         printf("Invalid label line: unrecognized text\n");
         printf("  %s\n",line_orig);
         fclose(f);
         return -3;
      }

      /*
       * add the new entry
       */
      table = realloc(table,(entries+1)*sizeof(table[0]));
      if ( !table )
      {
         fprintf(stderr,"Unable to allocate space for table\n");
         return -4;
      }
      table[entries].addr = addr;
      strcpy(table[entries].name,name);
      table[entries].bytes = bytes;
      table[entries].rw = rw;
      table[entries].btype = display;
      table[entries].base = base;
      ++entries;
   }
   // add the table
   add_table(table,entries);

   // done
   fclose(f);
   return 0;
}

/*
 * add_label()
 */
const char *add_label(const char *name,int addr,int write,const struct label *orig)
{
   // Check if label already exists
   for (int i=0;i<num_labels;++i)
   {
      if ( write && labels[i].rw == 'r' ) continue;
      if ( labels[i].addr == addr ) return labels[i].name;
      if ( labels[i].addr < addr && labels[i].addr+labels[i].bytes > addr )
      {
         // Check for exact match
         for (int j=i+1;j<num_labels;++j)
         {
            if ( write && labels[j].rw == 'r' ) continue;
            if ( labels[j].addr == addr ) return labels[j].name;
         }
         // Add offset label
         if ( !name || !*name )
         {
            char newname[MAX_LABEL_SIZE+1+3*sizeof(int)+1];
            sprintf(newname,"%s+%d",labels[i].name,addr-labels[i].addr);
            return(add_label(newname,addr,write,&labels[i]));
         }
         // else actually add it below
      }
   }

   // Label name from table
   if ( !name || !*name )
   {
      for (int table=0; table<num_tables; ++table)
      {
         for (int i=0;i<label_tables[table].entries;++i)
         {
            if ( write && labels[i].rw == 'r' ) continue;
            if ( label_tables[table].table[i].addr == addr ) return add_label(label_tables[table].table[i].name,addr,write,&label_tables[table].table[i]);
            if ( label_tables[table].table[i].addr < addr && label_tables[table].table[i].addr+label_tables[table].table[i].bytes > addr )
            {
               // Need to add the base label if it's not already there
               add_label(label_tables[table].table[i].name,label_tables[table].table[i].addr,write,&label_tables[table].table[i]);
               // Add offset label
               char newname[MAX_LABEL_SIZE+1+3*sizeof(int)+1];
               sprintf(newname,"%s+%d",label_tables[table].table[i].name,addr-label_tables[table].table[i].addr);
               return(add_label(newname,addr,write,&label_tables[table].table[i]));
            }
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

   labels[num_labels].rw = 'a'; // All new labels are for any access
   if ( orig )
   {
      memcpy(&labels[num_labels],orig,sizeof(*orig));
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
   int dosini = le16toh(*(uint16_t *)&load[4]);
   if ( dosini >= target + 6 )
   {
      branch_target[dosini] = 1;
      add_label("BOOT_INI",dosini,0,&label_word);
   }
   branch_target[target+6] = 1;
   add_label("BOOT_EXEC",target+6,0,NULL);
   add_label("BOOT_SECS",target+1,0,&label_dec);
   add_label("BOOT_ADDR",target+2,0,&label_word);
   add_label("LOAD_ADDR",target+4,0,&label_word);
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
 * load_rom()
 *
 * Load a 8K or 16K straight ROM
 */
int load_rom(const unsigned char *load,int size)
{
   int addr = 0xc000 - size;
   int run;
   int r;

   if ( size != 8*1024 && size != 16*1024 ) return -1;
   r = load_blob(addr,load,size);
   if ( r ) return r;
   run = le16toh(*(uint16_t *)&mem[0xbffa]);
   if ( run >= addr && run < 0xc000 )
   {
      add_label("CART_STRT",run,0,NULL);
      branch_target[run] = 1;
   }
   run = le16toh(*(uint16_t *)&mem[0xbffe]);
   if ( run >= addr && run < 0xc000 )
   {
      add_label("CART_INIT",run,0,NULL);
      branch_target[run] = 1;
   }
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
   int first_addr = 0;
   int block = 0; // increment on each block

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
      if ( !first_addr ) first_addr = start;
      end=le16toh(*(uint16_t *)&load[0]);
      load +=2;
      size -=2;
      ++block;
      if ( size < end-start+1 ) return -1;
      if ( end < start ) return -1;

      // Add the init label and target, even if overlapping
      if ( start <= 0x2e2 && end >= 0x2e3 )
      {
         char name[16];
         ++init;
         sprintf(name,"INIT%d",init);
         add_label(name,le16toh(*(uint16_t *)&load[0x2e2-start]),0,NULL);
         branch_target[le16toh(*(uint16_t *)&load[0x2e2-start])] = 1;
      }

      // Check for overlap with previous regions
      for (int i=start;i<=end;++i)
      {
         if ( mem_loaded[i] )
         {
            trace_code();
            fix_up_labels();
            sort_labels();
            output_disasm();
            break;
         }
      }

      memcpy(&mem[start],load,end-start+1);
      for (int i=start;i<=end;++i) mem_loaded[i]=block;
      load += end-start+1;
      size -= end-start+1;

      if ( start <= 0x2e0 && end >= 0x2e1 )
      {
         // Add target, but do not add label just in case there are more; only the last one runs
         branch_target[le16toh(*(uint16_t *)&load[0x2e0-start])] = 1;
      }
   }
   // Run address at end
   if ( mem_loaded[0x2e0] && mem_loaded[0x2e1] )
   {
      add_label("RUN",le16toh(*(uint16_t *)&mem[0x2e0]),0,NULL);
   }
   
   for (int i=0;i<0xffff;++i) if ( branch_target[i] ) return 0;
   branch_target[first_addr] = 1; // Assume the first block starts the code if there are no start addresses
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
         add_label(NULL,addr+2+(signed char)(mem[addr+1]),0,NULL);
      }
      // If this is a JSR, flag the target
      else if ( strcmp("JSR",opcode[mem[addr]].mnemonic) == 0 )
      {
         branch_target[le16toh(*(uint16_t *)&mem[addr+1])]=1;
         add_label(NULL,le16toh(*(uint16_t *)&mem[addr+1]),0,NULL);
      }
      // If this is a JMP absolute, flag the target and stop
      else if ( mem[addr] == 0x4C )
      {
         branch_target[le16toh(*(uint16_t *)&mem[addr+1])]=1;
         add_label(NULL,le16toh(*(uint16_t *)&mem[addr+1]),0,NULL);
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
         int write = (strncmp(opcode[mem[addr]].mnemonic,"ST",2)==0);
         switch(opcode[mem[addr]].mode)
         {
            case E_ABSOLUTE:
            case E_ABSOLUTE_X:
            case E_ABSOLUTE_Y:
               add_label(NULL,le16toh(*(uint16_t *)&mem[addr+1]),write,NULL);
               break;
            case E_ZEROPAGE:
            case E_ZEROPAGE_X:
            case E_ZEROPAGE_Y:
            case E_ZEROPAGE_IND_X:
            case E_ZEROPAGE_IND_Y:
               add_label(NULL,mem[addr+1],write,NULL);
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
      name = add_label(NULL,addr,0,NULL);
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
      if ( write && labels[lab].rw == 'r' ) continue;
      if ( labels[lab].addr == target )
      {
         if ( syntax.bracket && strchr(labels[lab].name,'+') )
         {
            printf("[%s]",labels[lab].name);
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
   static int block=1; // Next block to process
   int max_block = block;

   // Display labels that are outside loaded memory
   for (int lab=0;lab<num_labels;++lab)
   {
      if ( labels[lab].defined ) continue;
      if ( strchr(labels[lab].name,'+') ) continue; // Don't need to print 'LABEL+1' fake labels
      if ( !mem_loaded[labels[lab].addr] )
      {
         char *c = strchr(labels[lab].name,',');
         if ( c ) // separate read/write labels
         {
            char name[16];
            strcpy(name,labels[lab].name);
            strchr(name,',')[0]=0;
            printf("%s\t= $%04X "COMMENT" read register\n",name,labels[lab].addr);
            printf("%s\t= $%04X "COMMENT" write register\n",c+1,labels[lab].addr);
         }
         else // normal case
         {
            printf("%s\t= $%04X\n",labels[lab].name,labels[lab].addr);
         }
         labels[lab].defined = 1; // don't print again
      }
   }

   // Display loaded memory
   int set=0;
   int lab;
   for ( ; block <= max_block; ++block ) // output blocks in order
   {
      for (int addr=0;addr<0x10000;++addr)
      {
         // If the start of a new block of loaded memory; set the address
         if ( mem_loaded[addr] > max_block ) max_block = mem_loaded[addr];
         if ( mem_loaded[addr] != block )
         {
            set=0;
            continue;
         }
         if ( !set )
         {
            if ( syntax.org )
            {
               if ( syntax.orgdot ) printf(".");
               printf("org");
               //if ( syntax.colon ) printf(":");
            }
            printf("\t");
            if ( !syntax.org ) printf("*= ");
            printf("$%04X\n",addr);
            set=1;
         }
         // Display this address
         for (lab=0;lab<num_labels;++lab)
         {
            if ( labels[lab].addr == addr )
            {
               if ( !strchr(labels[lab].name,'+') )
               {
                  printf("%s",labels[lab].name);
                  if ( syntax.colon ) printf(":");
               }
               break;
            }
            // Get label if in a longer label (for base and type)
            if ( labels[lab].addr < addr && labels[lab].addr+labels[lab].bytes > addr )
            {
               break;
            }

         }
         if ( lab >= num_labels )
         {
            lab = -1;
         }
         if ( !instruction[addr] )
         {
            // check for word labels, but not at odd offsets
            if ( lab >= 0 && labels[lab].btype == 2 && labels[lab].bytes >= 2 && (labels[lab].bytes&0x01)==0 )
            {
               unsigned int val = le16toh(*(uint16_t *)&mem[addr]);
               int base = 16;
               if ( lab >= 0 ) base = labels[lab].base;
               switch(base)
               {
                  case 2:
                     printf("\t"WORD_PSEUDO_OP" %%");
                     for ( int i=0x8000; i; i=i>>1 )
                     {
                        printf("%d",(val&i)>0);
                     }
                     break;
                  case 8:
                     printf("\t"WORD_PSEUDO_OP" &%o",val);
                     break;
                  case 10:
                     printf("\t"WORD_PSEUDO_OP" %u",val);
                     break;
                  case 16:
                  default: // for words, strings make no sense
                     printf("\t"WORD_PSEUDO_OP" $%04X",val);
                     break;
               }
               ++addr;
            }
            else
            {
               unsigned int val = mem[addr];
               int base = 16;
               int count = 1;
               if ( lab >= 0 )
               {
                  base = labels[lab].base;
                  count = labels[lab].addr + labels[lab].bytes - addr; // Max bytes to display
               }
               switch(base)
               {
                  case 2:
                     printf("\t"BYTE_PSEUDO_OP" %%");
                     for ( int i=0x80; i; i=i>>1 )
                     {
                        printf("%d",(val&i)>0);
                     }
                     break;
                  case 8:
                     printf("\t"BYTE_PSEUDO_OP" &%o",val);
                     break;
                  case 10:
                     printf("\t"BYTE_PSEUDO_OP" %u",val);
                     break;
                  case 256:
                     // If a string, consume as many bytes as possible within the label
                     if ( count > STRING_MAX ) count = STRING_MAX;
                     if ( IS_QUOTABLE(val) )
                     {
                        printf("\t"BYTE_PSEUDO_OP" %c",'"');
                        for ( int i=0; i<count; ++i )
                        {
                           if ( IS_QUOTABLE(mem[addr]) )
                           {
                              printf("%c",mem[addr]);
                              ++addr;
                           }
                           else break;
                        }
                        --addr;
                        printf("%c",'"');
                        break;
                     }
                     // else: ATASCII-specific character in hex
                     // fall through
                  default:
                  case 16:
                     printf("\t"BYTE_PSEUDO_OP" $%02X",val);
                     if ( IS_ATASCII(val) )
                        printf(" "COMMENT" '%c'",val);
                     break;
               }
            }
            printf("\n");
         }
         else
         {
            int didbytes = 0;
            printf("\t");
            if ( opcode[mem[addr]].unofficial &&
                 ( syntax.noundoc || strcmp("NOP",opcode[mem[addr]].mnemonic) == 0 ) )
            {
               int bytes = instruction_bytes[opcode[mem[addr]].mode];
               printf(""BYTE_PSEUDO_OP" ");
               for (int i=0;i<bytes;++i)
               {
                  printf("%s$%02X",i?",":"",mem[addr+i]);
               }
               printf(" "COMMENT" (undocumented opcode) - ");
               didbytes = 1;
            }
            printf("%s",opcode[mem[addr]].mnemonic);
            int target = le16toh(*(uint16_t *)&mem[addr+1]);
            int ztarget = mem[addr+1];
            int btarget = addr+2+(signed char)(mem[addr+1]);
            int write = (strncmp(opcode[mem[addr]].mnemonic,"ST",2)==0);
            switch(opcode[mem[addr]].mode)
            {
               case E_ACCUMULATOR:
                  if ( !syntax.noa ) printf(" A");
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
            if ( opcode[mem[addr]].unofficial && !didbytes )
            {
               printf(" "COMMENT" (undocumented opcode)");
            }
            printf("\n");
            addr += instruction_bytes[opcode[mem[addr]].mode];
            --addr; // for loop also adds one
         }
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
          " --labels=atari,cio,float,basic Specify wanted labels (basic off by default)\n"
          " --ltable=[filename] Load label table from a file (may be repeated)\n"
          " --lfile=[filename]  Load active labels from a file (may be repeated)\n"
          " --syntax=[option][,option]  Set various syntax options:\n"
          "     bracket     Use brackets for label math: [LABEL+1]\n"
          "     noa         Leave off the 'A' on ASL, ROR, and the like\n"
          "     org         Use '.org =' instead of '*=' to set PC\n"
          "     colon       Put a colon after labels\n"
          "     noundoc     No undocumented opcodes\n"
          "     mads        Defaults for MADS assembler: noa,org,colon\n"
          "     ca65        Defaults for ca65 assembler: noa,org,colon\n"
          "     cc65        Alias for ca65\n"
          "     xa          Defaults for xa assembler: noundoc \n"
          "     asmedit     Defaults for Atari Assembler/Editor cartridge: noundoc \n"
          "\n"
          "If no options are specified, the file is auto-parsed for type\n"
          "Supported types:\n"
          "  binary load    -- any file that starts with ffff\n"
          "  ROM files      -- exactly 16K or 8K with valid init and run addresses\n"
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
   char *label_table_selection = NULL;
   while ( argc > 2 )
   {
      if ( argv[1][0] != '-' && argv[1][1] != '-' )
      {
         usage(progname);
         return 1;
      }
      if ( strcmp(argv[1],"--bracket") == 0 )
      {
         syntax.bracket = 1;
         ++argv;
         --argc;
         continue;
      }
      if ( strcmp(argv[1],"--noa") == 0 )
      {
         syntax.noa = 1;
         ++argv;
         --argc;
         continue;
      }
      if ( strncmp(argv[1],"--syntax=",sizeof("--syntax=")-1) == 0 )
      {
         const char *opt = argv[1]+sizeof("--syntax=")-1;

         while ( *opt )
         {
            // remove commas
            while ( *opt == ',' ) ++opt;
            if ( !*opt ) break; // ended with a ','; weird, but allow it

            // specific syntax options
            if ( strncmp(opt,"bracket",sizeof("bracket")-1)==0 )
            {
               syntax.bracket = 1;
               opt+=sizeof("bracket")-1;
               if ( *opt == 's' ) ++opt; // Allow plural
               continue;
            }
            if ( strncmp(opt,"noa",sizeof("noa")-1)==0 )
            {
               syntax.noa = 1;
               opt+=sizeof("noa")-1;
               continue;
            }
            if ( strncmp(opt,"org",sizeof("org")-1)==0 )
            {
               syntax.org = 1;
               opt+=sizeof("org")-1;
               continue;
            }
            if ( strncmp(opt,"colon",sizeof("colon")-1)==0 )
            {
               syntax.colon = 1;
               opt+=sizeof("colon")-1;
               continue;
            }
            if ( strncmp(opt,"noundoc",sizeof("noundoc")-1)==0 )
            {
               syntax.noundoc = 1;
               opt+=sizeof("noundoc")-1;
               continue;
            }

            // select defaults for a given assembler
            if ( strncmp(opt,"mads",sizeof("mads")-1)==0 )
            {
               syntax.noa = 1;
               syntax.org = 1;
               syntax.colon = 1;
               opt+=sizeof("mads")-1;
               continue;
            }
            if ( strncmp(opt,"ca65",sizeof("ca65")-1)==0 ||
                 strncmp(opt,"cc65",sizeof("cc65")-1)==0 )
            {
               syntax.noa = 1;
               syntax.org = 1;
               syntax.orgdot = 1;
               syntax.colon = 1;
               opt+=sizeof("cc65")-1;
               continue;
            }
            if ( strncmp(opt,"xa",sizeof("xa")-1)==0 )
            {
               syntax.noundoc = 1;
               opt+=sizeof("xa")-1;
               continue;
            }
            if ( strncmp(opt,"asmedit",sizeof("asmedit")-1)==0 )
            {
               syntax.noundoc = 1;
               opt+=sizeof("asmedit")-1;
               continue;
            }

            // Didn't find anything
            printf("Invalid option: %s\n",argv[1]);
            usage(progname);
            return 1;
         }

         ++argv;
         --argc;
         continue;
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
            add_label(name,startaddr,0,NULL);
            branch_target[startaddr] = 1;

            ++argv;
            --argc;
            continue;
         }
      }
      if ( strncmp(argv[1],"--labels=",sizeof("--labels=")-1) == 0 )
      {
         label_table_selection = argv[1]+sizeof("--labels=")-1;
         ++argv;
         --argc;
         continue;
      }
      if ( strncmp(argv[1],"--ltable=",sizeof("--ltable=")-1) == 0 )
      {
         if ( add_label_file(argv[1]+sizeof("--ltable=")-1) < 0 ) return 1;
         ++argv;
         --argc;
         continue;
      }
      if ( strncmp(argv[1],"--lfile=",sizeof("--lfile=")-1) == 0 )
      {
         if ( add_label_file(argv[1]+sizeof("--lfile=")-1) < 0 ) return 1;
         // Now actually add the labels
         const struct label *t = label_tables[num_tables-1].table;
         int entries = label_tables[num_tables-1].entries;
         for (int i=0;i<entries;++i)
         {
            add_label(t[i].name,t[i].addr,0,&t[i]);
         }
         ++argv;
         --argc;
         continue;
      }
      printf("Invalid option: %s\n",argv[1]);
      usage(progname);
      return 1;
   }

   if ( label_table_selection )
   {
      while (label_table_selection)
      {
         char *c = strrchr(label_table_selection,',');
         if ( c ) { *c=0; ++c; } // Add the last table first
         else
         {
            c = label_table_selection;
            label_table_selection=NULL; // Flag done
         }
         if (strcmp(c,"atari")==0)
         {
            add_table(label_table_atari,ARRAY_SIZE(label_table_atari));
         }
         else if  (strcmp(c,"cio")==0)
         {
            add_table(label_table_atari,ARRAY_SIZE(label_table_atari));
            add_table(label_table_atari_cio,ARRAY_SIZE(label_table_atari_cio));
         }
         else if  (strcmp(c,"float")==0)
         {
            add_table(label_table_atari,ARRAY_SIZE(label_table_atari));
            add_table(label_table_atari_float,ARRAY_SIZE(label_table_atari_float));
         }
         else if  (strcmp(c,"basic")==0)
         {
            add_table(label_table_atari,ARRAY_SIZE(label_table_atari));
            add_table(label_table_atari_cio,ARRAY_SIZE(label_table_atari_cio));
            add_table(label_table_atari_float,ARRAY_SIZE(label_table_atari_float));
            add_table(label_table_atari_basic,ARRAY_SIZE(label_table_atari_basic));
         }
         else
         {
            fprintf(stderr,"Invalid label table selection: %s\n",c);
         }
      }
   }
   else
   {
      add_table(label_table_atari,ARRAY_SIZE(label_table_atari));
      add_table(label_table_atari_cio,ARRAY_SIZE(label_table_atari_cio));
      add_table(label_table_atari_float,ARRAY_SIZE(label_table_atari_float));
      // note default: add_table(label_table_atari_basic,ARRAY_SIZE(label_table_atari_basic));
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
   else if ( load_rom(data,statbuf.st_size) == 0 ) ; // It was a ROM
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
