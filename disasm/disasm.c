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
#define POST_OPCODE "\t" // should be a syntax option
#define COMMENT ";"
#define STRING_MAX 40 // maximum number of bytes for string: .byte "Long string"

// ATASCII values that are the same in ASCII
#define IS_ASCII(_a) ( ( (_a) >= ' ' && (_a) <= 'A' ) || isalpha(_a) || (_a) == '|' || (_a) == 0x9b )
#define IS_QUOTABLE(_a) ( IS_ASCII(_a) && (_a) != syntax.stringquote && (_a) != 0x9b ) // don't quote quotes in quotes
#define ATASCII_TO_SCREEN(_a) ( ( ((_a)&0x7f) < 0x20 ) ? ((_a)+0x40) : ( ((_a)&0x7f) < 0x60 ) ? (_a) - 0x20 : (_a) )
#define SCREEN_TO_ATASCII(_a) ( ( ((_a)&0x7f) < 0x40 ) ? ((_a)+0x20) : ( ((_a)&0x7f) < 0x60 ) ? (_a) - 0x40 : (_a) )
#define IS_SCREEN_ASCII(_a) ( (_a) != 0x9b && IS_ASCII(SCREEN_TO_ATASCII(_a)) )
#define IS_SCREEN_QUOTABLE(_a) ( IS_SCREEN_ASCII(_a) && SCREEN_TO_ATASCII(_a) != syntax.screenquote )

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

enum base_overload {
   E_ATASCII_STRING = 256,
   E_SCREEN_STRING = 255,
   E_ATASCII_INVERSE_STRING = 254,
   E_SCREEN_INVERSE_STRING = 253
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
   int negative; // how far back for negative offset references (normally zero)
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
   int noscreencode;
   int listing; // Print out address and bytes for each line
   int mads;
   int indent_count; // default 1
   int indent_tab; // default true
   unsigned char stringquote;
   unsigned char screenquote;
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
   { .addr=0x0000, .name="LINZBS", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0000, .name="LINFLG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0001, .name="NGFLAG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0002, .name="CASINI", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0004, .name="RAMLO", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0006, .name="TRAMSZ", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0007, .name="TSTDAT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0008, .name="WARMST", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0009, .name="BOOT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0}, // Officially "BOOT?" but I'm not sure the '?' works in all assemblers
   { .addr=0x000A, .name="DOSVEC", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x000C, .name="DOSINI", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x000E, .name="APPMHI", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0010, .name="POKMSK", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0011, .name="BRKKEY", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0012, .name="RTCLOK", .bytes=3, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0015, .name="BUFADR", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0017, .name="ICCOMT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0018, .name="DSKFMS", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x001A, .name="DSKUTL", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x001C, .name="PTIMOT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x001D, .name="PBPNT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x001E, .name="PBUFSZ", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x001F, .name="PTEMP", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0020, .name="ICHIDZ", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0021, .name="ICDNOZ", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0022, .name="ICCOMZ", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0023, .name="ICSTAZ", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0024, .name="ICBALZ", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0025, .name="ICBAHZ", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0026, .name="ICPTLZ", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0027, .name="ICPTHZ", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0028, .name="ICBLLZ", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0029, .name="ICBLHZ", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x002A, .name="ICAX1Z", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x002B, .name="ICAX2Z", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x002C, .name="ICAX3Z", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x002D, .name="ICAX4Z", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x002E, .name="ICAX5Z", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x002F, .name="ICAX6Z", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0030, .name="STATUS", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0031, .name="CHKSUM", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0032, .name="BUFRLO", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0033, .name="BUFRHI", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0034, .name="BFENLO", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0035, .name="BFENHI", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0036, .name="CRETRY", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0037, .name="DRETRY", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0036, .name="LTEMP", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0038, .name="BUFRFL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0039, .name="RECVDN", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x003A, .name="XMTDON", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x003B, .name="CHKSNT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x003C, .name="NOCKSM", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x003D, .name="BPTR", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x003E, .name="FTYPE", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x003F, .name="FEOF", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0040, .name="FREQ", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0041, .name="SOUNDR", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0042, .name="CRITIC", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   //{ .addr=0x0043-0x0049, .name="FMSZPG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0043, .name="ZBUFP", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0045, .name="ZDRVA", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0047, .name="ZSBA", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0049, .name="ERRNO", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x004A, .name="CKEY", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x004B, .name="CASSBT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x004A, .name="ZCHAIN", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x004C, .name="DSTAT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x004D, .name="ATRACT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x004E, .name="DRKMSK", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x004F, .name="COLRSH", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0050, .name="TEMP", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0051, .name="HOLD1", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0052, .name="LMARGN", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0053, .name="RMARGN", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0054, .name="ROWCRS", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0055, .name="COLCRS", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0057, .name="DINDEX", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0058, .name="SAVMSC", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x005A, .name="OLDROW", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x005B, .name="OLDCOL", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x005D, .name="OLDCHR", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x005E, .name="OLDADR", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0060, .name="NEWROW", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0060, .name="FKDEF", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0061, .name="NEWCOL", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0062, .name="PALNTS", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0063, .name="LOGCOL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0064, .name="ADRESS", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0066, .name="MLTTMP", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0068, .name="SAVADR", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x006A, .name="RAMTOP", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x006B, .name="BUFCNT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x006C, .name="BUFSTR", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x006E, .name="BITMSK", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x006F, .name="SHFAMT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0070, .name="ROWAC", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0072, .name="COLAC", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0074, .name="ENDPT", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0076, .name="DELTAR", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0077, .name="DELTAC", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0079, .name="ROWINC", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x007A, .name="COLINC", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0079, .name="KEYDEF", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x007B, .name="SWPFLG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x007C, .name="HOLDCH", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x007D, .name="INSDAT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x007E, .name="COUNTR", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0200, .name="VDSLST", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0202, .name="VPRCED", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0204, .name="VINTER", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0206, .name="VBREAK", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0208, .name="VKEYBD", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x020A, .name="VSERIN", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x020C, .name="VSEROR", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x020E, .name="VSEROC", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0210, .name="VTIMR1", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0212, .name="VTIMR2", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0214, .name="VTIMR4", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0216, .name="VIMIRQ", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0218, .name="CDTMV1", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x021A, .name="CDTMV2", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x021C, .name="CDTMV3", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x021E, .name="CDTMV4", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0220, .name="CDTMV5", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0222, .name="VVBLKI", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0224, .name="VVBLKD", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0226, .name="CDTMA1", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0228, .name="CDTMA2", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x022A, .name="CDTMF3", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x022B, .name="SRTIMR", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x022C, .name="CDTMF4", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x022D, .name="INTEMP", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x022E, .name="CDTMF5", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x022F, .name="SDMCTL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0230, .name="SDLSTL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0231, .name="SDLSTH", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0232, .name="SSKCTL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0233, .name="SPARE", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0233, .name="LCOUNT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0234, .name="LPENH", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0235, .name="LPENV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0236, .name="BRKKY", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0238, .name="VPIRQ", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x023A, .name="CDEVIC", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x023B, .name="CCOMND", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x023C, .name="CAUX1", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x023D, .name="CAUX2", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x023E, .name="TEMP", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x023F, .name="ERRFLG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0240, .name="DFLAGS", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0241, .name="DESECT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0242, .name="BOOTAD", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0244, .name="COLDST", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0245, .name="RECLEN", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0246, .name="DSKTIM", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   //{ .addr=0x0247-0x026E, .name="LINBUF", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0}, // 40-character line buffer; not sure about what the other stuff in the same range is
   { .addr=0x0247, .name="PDVMSK", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0248, .name="SHPDVS", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0249, .name="PDIMSK", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x024A, .name="RELADR", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x024C, .name="PPTMPA", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x024D, .name="PPTMPX", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x026B, .name="CHSALT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x026C, .name="VSFLAG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x026D, .name="KEYDIS", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x026E, .name="FINE", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x026F, .name="GPRIOR", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0270, .name="PADDL0", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0271, .name="PADDL1", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0272, .name="PADDL2", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0273, .name="PADDL3", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0274, .name="PADDL4", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0275, .name="PADDL5", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0276, .name="PADDL6", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0277, .name="PADDL7", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0278, .name="STICK0", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0279, .name="STICK1", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x027A, .name="STICK2", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x027B, .name="STICK3", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x027C, .name="PTRIG0", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x027D, .name="PTRIG1", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x027E, .name="PTRIG2", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x027F, .name="PTRIG3", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0280, .name="PTRIG4", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0281, .name="PTRIG5", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0282, .name="PTRIG6", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0283, .name="PTRIG7", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0284, .name="STRIG0", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0285, .name="STRIG1", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0286, .name="STRIG2", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0287, .name="STRIG3", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0288, .name="CSTAT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0288, .name="HIBZTE", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0289, .name="WMODE", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x028A, .name="BLIM", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x028B, .name="IMASK", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x028C, .name="JVECK", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x028E, .name="NEWADR", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0290, .name="TXTROW", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0291, .name="TXTCOL", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0293, .name="TINDEX", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0294, .name="TXTMSC", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0296, .name="TXTOLD", .bytes=6, .rw='a', .btype=1, .base=16, .defined=0}, // used for cursor position in split screen mode
   { .addr=0x029C, .name="TMPX1", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x029C, .name="CRETRY", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x029D, .name="HOLD3", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x029E, .name="SUBTMP", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x029F, .name="HOLD2", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02A0, .name="DMASK", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02A1, .name="TMPLBT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02A2, .name="ESCFLG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02A3, .name="TABMAP", .bytes=15, .rw='a', .btype=1, .base=16, .defined=0}, // map of tab positions
   { .addr=0x02B2, .name="LOGMAP", .bytes=4, .rw='a', .btype=1, .base=16, .defined=0}, // map of logical line breaks
   { .addr=0x02B6, .name="INVFLG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02B7, .name="FILFLG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02B8, .name="TMPROW", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02B9, .name="TMPCOL", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x02BB, .name="SCRFLG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02BC, .name="HOLD4", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02BD, .name="HOLD5", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02BD, .name="DRETRY", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02BE, .name="SHFLOK", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02BF, .name="BOTSCR", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02C0, .name="PCOLR0", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02C1, .name="PCOLR1", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02C2, .name="PCOLR2", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02C3, .name="PCOLR3", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02C4, .name="COLOR0", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02C5, .name="COLOR1", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02C6, .name="COLOR2", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02C7, .name="COLOR3", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02C8, .name="COLOR4", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02C9, .name="RUNADR", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x02CB, .name="HIUSED", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x02CD, .name="ZHIUSE", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x02CF, .name="GBYTEA", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x02D1, .name="LOADAD", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x02D3, .name="ZLOADA", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x02D5, .name="DSCTLN", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x02D7, .name="ACMISR", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x02D9, .name="KRPDEL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02DA, .name="KEYREP", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02DB, .name="NOCLIK", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02FC, .name="HELPFG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02DD, .name="DMASAV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02DE, .name="PBPNT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02DF, .name="PBUFSZ", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02E0, .name="RUNAD", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x02E2, .name="INITAD", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x02E4, .name="RAMSIZ", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02E5, .name="MEMTOP", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x02E7, .name="MEMLO", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x02E9, .name="HNDLOD", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02EA, .name="DVSTAT", .bytes=4, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02EE, .name="CBAUDL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02EF, .name="CBAUDH", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02F0, .name="CRSINH", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02F1, .name="KEYDEL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02F2, .name="CH1", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02F3, .name="CHACT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02F4, .name="CHBAS", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02F5, .name="NEWROW", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02F6, .name="NEWCOL", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x02F8, .name="ROWINC", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02F9, .name="COLINC", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02FA, .name="CHAR", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02FB, .name="ATACHR", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02FC, .name="CH", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02FD, .name="FILDAT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02FE, .name="DSPFLG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x02FF, .name="SSFLAG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0300, .name="DDEVIC", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0301, .name="DUNIT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0302, .name="DCOMND", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0303, .name="DSTATS", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0304, .name="DBUFLO", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0305, .name="DBUFHI", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0306, .name="DTIMLO", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0307, .name="DUNUSE", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0308, .name="DBYTLO", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0309, .name="DBYTHI", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x030A, .name="DAUX1", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x030B, .name="DAUX2", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x030C, .name="TIMER1", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x030E, .name="ADDCOR", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x030E, .name="JMPERS", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x030F, .name="CASFLG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0310, .name="TIMER2", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0312, .name="TEMP1", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0314, .name="TEMP2", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0314, .name="PTIMOT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0315, .name="TEMP3", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0316, .name="SAVIO", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0317, .name="TIMFLG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0318, .name="STACKP", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0319, .name="TSTAT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x03E8, .name="SUPERF", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x03E9, .name="CKEY", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x03EA, .name="CASSBT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x03EB, .name="CARTCK", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x03EC, .name="DEERF", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   //{ .addr=0x03ED-0x03F7, .name="ACMVAR", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x03F8, .name="BASICF", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x03F9, .name="MINTLK", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x03FA, .name="GINTLK", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x03FB, .name="CHLINK", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x057E, .name="LBPR1", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x057F, .name="LBPR2", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x05E0, .name="PLYARG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD000, .name="HPOSP0", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD000, .name="M0PF", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD001, .name="HPOSP1", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD001, .name="M1PF", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD002, .name="HPOSP2", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD002, .name="M2PF", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD003, .name="HPOSP3", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD003, .name="M3PF", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD004, .name="HPOSM0", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD004, .name="P0PF", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD005, .name="HPOSM1", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD005, .name="P1PF", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD006, .name="HPOSM2", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD006, .name="P2PF", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD007, .name="HPOSM3", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD007, .name="P3PF", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD008, .name="SIZEP0", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD008, .name="M0PL", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD009, .name="SIZEP1", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD009, .name="M1PL", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD00A, .name="SIZEP2", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD00A, .name="M2PL", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD00B, .name="SIZEP3", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD00B, .name="M3PL", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD00C, .name="SIZEM", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD00C, .name="P0PL", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD00D, .name="GRAFP0", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD00D, .name="P1PL", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD00E, .name="GRAFP1", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD00E, .name="P2PL", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD00F, .name="GRAFP2", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD00F, .name="P3PL", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD010, .name="GRAFP3", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD010, .name="TRIG0", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD011, .name="GRAFM", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD011, .name="TRIG1", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD012, .name="COLPM0", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD012, .name="TRIG2", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD013, .name="COLPM1", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD013, .name="TRIG3", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD014, .name="COLPM2", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD014, .name="PAL", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD015, .name="COLPM3", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD016, .name="COLPF0", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD017, .name="COLPF1", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD018, .name="COLPF2", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD019, .name="COLPF3", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD01A, .name="COLBK", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD01B, .name="PRIOR", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD01C, .name="VDELAY", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD01D, .name="GRACTL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD01E, .name="HITCLR", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD01F, .name="CONSOL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD200, .name="AUDF1", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD200, .name="POT0", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD201, .name="AUDC1", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD201, .name="POT1", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD202, .name="AUDF2", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD202, .name="POT2", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD203, .name="AUDC2", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD203, .name="POT3", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD204, .name="AUDF3", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD204, .name="POT4", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD205, .name="AUDC3", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD205, .name="POT5", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD206, .name="AUDF4", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD206, .name="POT6", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD207, .name="AUDC4", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD207, .name="POT7", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD208, .name="AUDCTL", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD208, .name="ALLPOT", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD209, .name="STIMER", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD209, .name="KBCODE", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD20A, .name="SKREST", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD20A, .name="RANDOM", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD20B, .name="POTGO", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD20D, .name="SEROUT", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD20D, .name="SERIN", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD20E, .name="IRQEN", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD20E, .name="IRQST", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD20F, .name="SKCTL", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD20F, .name="SKSTAT", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD300, .name="PORTA", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD301, .name="PORTB", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD302, .name="PACTL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD303, .name="PBCTL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD400, .name="DMACTL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD401, .name="CHACTL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD402, .name="DLISTL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD403, .name="DLISTH", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD404, .name="HSCROL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD405, .name="VSCROL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD407, .name="PMBASE", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD409, .name="CHBASE", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD40A, .name="WSYNC", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD40B, .name="VCOUNT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD40C, .name="PENH", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD40D, .name="PENV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD40E, .name="NMIEN", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD40F, .name="NMIST", .bytes=1, .rw='w', .btype=1, .base=16, .defined=0},
   { .addr=0xD40F, .name="NMIRES", .bytes=1, .rw='r', .btype=1, .base=16, .defined=0},
   { .addr=0xD800, .name="AFP", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD8E6, .name="FASC", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD9AA, .name="IFP", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xD9D2, .name="FPI", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDA44, .name="ZFR0", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDA46, .name="ZF1", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDA60, .name="FSUB", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDA66, .name="FADD", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDADB, .name="FMUL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDB28, .name="FDIV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDD40, .name="PLYEVL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDD89, .name="FLD0R", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDD8D, .name="FLD0P", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDD98, .name="FLD1R", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDD9C, .name="FLD1P", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDDA7, .name="FSTOR", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDDAB, .name="FSTOP", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDDB6, .name="FMOVE", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDDC0, .name="EXP", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDDCC, .name="EXP10", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDECD, .name="LOG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xDED1, .name="LOG10", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE400, .name="EDITRV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE410, .name="SCRENV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE420, .name="KEYBDV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE430, .name="PRINTV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE440, .name="CASETV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE450, .name="DISKIV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE453, .name="DSKINV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE456, .name="CIOV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE459, .name="SIOV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE45C, .name="SETVBV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE45F, .name="SYSVBV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE462, .name="XITVBV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE465, .name="SIOINV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE468, .name="SENDEV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE46B, .name="INTINV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE46E, .name="CIOINV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE471, .name="BLKBDV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE474, .name="WARMSV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE477, .name="COLDSV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE47A, .name="RBLOKV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE47D, .name="CSOPIV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE480, .name="PUPDIV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE483, .name="SLFTSV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE486, .name="PHENTV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE489, .name="PHULNV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE48C, .name="PHINIV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE48F, .name="GPDVV", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE7AE, .name="SYSVBL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xE7D1, .name="SYSVBL", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xFFF8, .name="CHKSUN", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0xFFFA, .name="PVECT", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
};
const struct label label_table_atari_cio[] = {
   { .addr=0x031A-0x033F, .name="HATABS", .bytes=27, .rw='a', .btype=1, .base=16, .defined=0}, // Handler address tables, 3 bytes per entry, two zeros at end
   { .addr=0x0340, .name="IOCB0", .bytes=16, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0350, .name="IOCB1", .bytes=16, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0360, .name="IOCB2", .bytes=16, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0370, .name="IOCB3", .bytes=16, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0380, .name="IOCB4", .bytes=16, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0390, .name="IOCB5", .bytes=16, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x03A0, .name="IOCB6", .bytes=16, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x03B0, .name="IOCB7", .bytes=16, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x03C0-0x03E7, .name="PRNBUF", .bytes=40, .rw='a', .btype=1, .base=16, .defined=0}, // printer buffer for LPRINT statements; is this BASIC or CIO?
   //{ .addr=0x03FD-0x047F, .name="CASBUF", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0}, // 3FD-3FF are also used for other stuff
};
const struct label label_table_atari_float[] = {
   { .addr=0x00D4, .name="FR0", .bytes=6, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x00DA, .name="FRE", .bytes=6, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x00E0, .name="FR1", .bytes=6, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x00E6, .name="FR2", .bytes=6, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x00EC, .name="FRX", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x00ED, .name="EEXP", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x00EE, .name="NSIGN", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x00EF, .name="ESIGN", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x00F0, .name="FCHRFLG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x00F1, .name="DIGRT", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x00F2, .name="CIX", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x00F3, .name="INBUFF", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x00F5, .name="ZTEMP1", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x00F7, .name="ZTEMP4", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x00F9, .name="ZTEMP3", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x00FB, .name="RADFLG", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x00FC, .name="FLPTR", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x00FE, .name="FPTR2", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x05E6, .name="FPSCR", .bytes=6, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x05EC, .name="FPSCR1", .bytes=4, .rw='a', .btype=1, .base=16, .defined=0},
};
const struct label label_table_atari_basic[] = {
   { .addr=0x0080, .name="LOMEM", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0082, .name="VNTP", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0084, .name="VNTD", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0086, .name="VVTP", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0088, .name="STMTAB", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x0090, .name="MEMTOP", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x008A, .name="STMCUR", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x008C, .name="STARP", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x008E, .name="RUNSTK", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x00BA, .name="STOPLN", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0},
   { .addr=0x00C3, .name="ERRSAVE", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x00C9, .name="PTABW", .bytes=1, .rw='a', .btype=1, .base=16, .defined=0},
   { .addr=0x0580, .name="LBUFF", .bytes=128, .rw='a', .btype=1, .base=16, .defined=0}, // BASIC line buffer
};

// Templates for 'orig' value when adding new labels
const struct label label_dec = { .addr=-1, .name="", .bytes=1, .rw='a', .btype=1, .base=10, .defined=0};
const struct label label_word = { .addr=-1, .name="", .bytes=2, .rw='a', .btype=2, .base=16, .defined=0};

/*
 * global variables
 */

// The 6502 memory
unsigned char mem[64*1024];

// Flags per-byte
uint16_t mem_loaded[64*1024];
char instruction[64*1024]; // first byte of an instruction?
char operand[64*1024]; // second or third byte of an instruction?
char evaluated[64*1024]; // Used in testing for possible instruction sequences
char branch_target[64*1024];
char data_target[64*1024];

// Options
struct syntax_options syntax;
int noundoc;

// Labels
struct label *labels;
int num_labels;
struct label_tables *label_tables;
int num_tables;

/*
 * Prototypes
 */
void trace_code(void);
struct label *find_label(int addr);
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
      int negative = 0; // default: no references with negative offsets
      char rw = 'a'; // any; could also be r or w
      int display = 1; // default to byte, not word
      int base = 16; // valid: 2, 8, 10, 16, 256 (string)
      int addr;
      int inst = 0;
      int data = 0;
      int jsrparms = 0;

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

      // /d for data, /i for instruction
      c=strchr(equal,'/');
      if ( c )
      {
         if ( c[1] == 'i' )
         {
            inst = 1;
            c[0]=' ';
            c[1]=' ';
         }
         else if ( c[1] == 'd' )
         {
            data = 1;
            c[0]=' ';
            c[1]=' ';
         }
         else if ( strncmp(c+1,"p=",2)==0 )
         {
            c[0]=' ';
            c[1]=' ';
            c[2]=' ';
            c+=3;
            char *end;
            if ( *c == '$' ) // allow '$' for hex
            {
               jsrparms = strtol(c+1,&end,16);
            }
            else
            {
               jsrparms = strtol(c,&end,0);
            }
            if ( end == c || jsrparms < 0 || jsrparms > 0x20 )
            {
               printf("Invalid JSR parameter byte count\n");
               printf("  %s\n",line_orig);
               fclose(f);
               return -3;
            }
            while ( c < end ) *c++ = ' ';
         }
         else
         {
            printf("Invalid label line: '/' must be followed by i, d, or p.\n");
            printf("  %s\n",line_orig);
            fclose(f);
            return -3;
         }
      }

      // -r, -w, or -a for hardware registers
      c=strchr(equal,'-');
      if ( c )
      {
         if ( isdigit(c[1]) || c[1]=='$' )
         {
            ; // handled below for negative offset labels
         }
         else if ( c[1] == 'a' || c[1] == 'r' || c[1] == 'w' )
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
         ++bytes; // +1 is two bytes
         while ( c < end ) *c++ = ' '; // wipe option
      }

      // -n for labels referenced with negative offsets
      c=strchr(equal,'-');
      if ( c )
      {
         char *end;
         if ( c[1] == '$' ) // allow '$' for hex
         {
            negative = strtol(c+2,&end,16);
         }
         else
         {
            negative = strtol(c+1,&end,0);
         }
         if ( negative < 1 || negative > 16*1024 )
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
            case E_SCREEN_INVERSE_STRING:
            case E_ATASCII_INVERSE_STRING:
            case E_SCREEN_STRING:
            case E_ATASCII_STRING:
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

      // 'screen' to specify base 255 for screen-code string
      c=strstr(equal,"screen");
      if ( c )
      {
         base = 255;
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
      table[entries].negative = negative;
      if ( jsrparms ) table[entries].base = jsrparms + 0x80; // overload base
      ++entries;

      // Flag instruction or data
      if ( inst ) branch_target[addr] = 1;
      if ( data ) data_target[addr] = 1;
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
      if ( !write && labels[i].rw == 'w' ) continue;
      if ( labels[i].addr == addr ) return labels[i].name;
      if ( labels[i].addr < addr && labels[i].addr+labels[i].bytes > addr )
      {
         if (strchr(labels[i].name,'+')) continue; // don't offset from an offset
         // Check for exact match
         for (int j=i+1;j<num_labels;++j)
         {
            if ( write && labels[j].rw == 'r' ) continue;
            if ( !write && labels[j].rw == 'w' ) continue;
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
            if ( write && label_tables[table].table[i].rw == 'r' ) continue;
            if ( !write && label_tables[table].table[i].rw == 'w' ) continue;
            if ( label_tables[table].table[i].addr - label_tables[table].table[i].negative <= addr && label_tables[table].table[i].addr + label_tables[table].table[i].bytes > addr )
            {
               // Add this label, but include all offsets as well
               for ( int off=-label_tables[table].table[i].negative; off<label_tables[table].table[i].bytes; ++off )
               {
                  if ( !off ) continue;
                  char newname[MAX_LABEL_SIZE+1+3*sizeof(int)+1];
                  sprintf(newname,"%s%s%d",label_tables[table].table[i].name,off>0?"+":"",off);
                  add_label(newname,addr+off,write,&label_tables[table].table[i]);
               }
               return add_label(label_tables[table].table[i].name,label_tables[table].table[i].addr,write,&label_tables[table].table[i]);
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

   memset(&labels[num_labels],0,sizeof(labels[num_labels]));
   labels[num_labels].rw = 'a'; // All new labels are for any access
   labels[num_labels].base = 16; // default to hex
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
         branch_target[le16toh(*(uint16_t *)&mem[0x2e0])] = 1;
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
      int extra_bytes = 0;
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
         // Save space for parameters if special label instructions
         struct label *l = find_label(le16toh(*(uint16_t *)&mem[addr+1]));
         if ( l && l->base > 0x80 ) extra_bytes = l->base - 0x80;
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
         branch_target[addr] = 0;
         return;
      }
      // Clear if told this must be data
      else if ( data_target[addr] )
      {
         instruction[addr] = 0;
         branch_target[addr] = 0;
         return;
      }
      // Clear undocumented opcodes if option is disabled
      else if ( noundoc && opcode[mem[addr]].unofficial )
      {
         instruction[addr] = 0;
         branch_target[addr] = 0;
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
      for (int i=1;i<=instruction_bytes[opcode[mem[addr]].mode];++i)
      {
         operand[addr+i] = 1;
      }
      addr += instruction_bytes[opcode[mem[addr]].mode];
      addr += extra_bytes;
   }
}

/*
 * test_instructions_at_addr()
 *
 * Given an address, see how many valid instructions are found starting at that address.
 * If it ends with a JMP or RTS, return the count.
 * If it encounters a BRK, JAM, or invalid opcode, return 0.
 *
 * Also evaluate branch targets.  If they're already flaged as instructions, good.
 * If not, then use then recurse and see if they hit an invalid instructions.
 *
 * Count may be inflated by one for each time it hits an existing
 * instruction (possibly on each branch).
 */
int test_instructions_at_addr(int addr,int recurse)
{
   int count = 0;
   if ( !recurse ) memset(evaluated,0,sizeof(evaluated));
   //int start_addr = addr;

   while ( 1 )
   {
      ++count;
      if ( instruction[addr] ) return count;
      if ( evaluated[addr] ) return count;
      if ( !mem_loaded[addr] )
      {
         //if ( !recurse ) printf("; not block at %04X due to mem not loaded after %d\n",start_addr,count);
         return 0; // Invalid if not loaded
      }
      if ( data_target[addr] )
      {
         //if ( !recurse ) printf("; not block at %04X due to data target after %d\n",start_addr,count);
         return 0; // Encountered a data target; highly unlikely to be valid
      }
      if ( mem[addr] == 0 || /* BRK */
           strcmp("JAM",opcode[mem[addr]].mnemonic) == 0 ||
           ( noundoc && opcode[mem[addr]].unofficial ) )
      {
         //if ( !recurse ) printf("; not block at %04X due to bad opcode after %d\n",start_addr,count);
         return 0;
      }
      if ( strcmp("JMP",opcode[mem[addr]].mnemonic) == 0 ||
           strcmp("RTS",opcode[mem[addr]].mnemonic) == 0 ||
           strcmp("RTI",opcode[mem[addr]].mnemonic) == 0
         )
      {
         //if ( !recurse ) printf("; block at %04X after %d: %s\n",start_addr,count,opcode[mem[addr]].mnemonic);
         return count;
      }
   
      evaluated[addr] = 1;
      // Check branches
      if ( mem[addr] == 0x4C /* JMP absolute */ || mem[addr] == 0x20 /* JSR absolute */ )
      {
         int target = le16toh(*(uint16_t *)&mem[addr+1]);
         if ( mem_loaded[target] )
         {
            int more = test_instructions_at_addr(target,1);
            if ( !more )
            {
               //if ( !recurse ) printf("; not block at %04X after %d, bad target: %s\n",start_addr,count,opcode[mem[addr]].mnemonic);
               return 0;
            }
            count += more;
         }
      }
      if ( opcode[mem[addr]].mode == E_RELATIVE ) // Branch
      {
         int target = addr+2+(signed char)(mem[addr+1]);;
         int more = test_instructions_at_addr(target,1);
            if ( !more )
            {
               //if ( !recurse ) printf("; not block at %04X after %d, bad branch: %s\n",start_addr,count,opcode[mem[addr]].mnemonic);
               return 0;
            }
         count += more;
      }

      addr += instruction_bytes[opcode[mem[addr]].mode];
   }
}

/*
 * find_blocks()
 *
 * Scan for blocks of instructions following existing instructions
 */
void find_blocks(void)
{
   int found = 1;
   while ( found )
   {
      found = 0;
      for (int addr=1;addr<64*1024;++addr)
      {
         int threshold = 2; // Not sure what a good threshold is
         // Already know what this byte is for?
         if ( !mem_loaded[addr] || instruction[addr] || operand[addr] || data_target[addr] ) continue;

         // Previous byte was an unprocessed RTS; check with a higher threshold
         if ( mem_loaded[addr-1] && mem[addr-1] == 0x60 && !instruction[addr-1] && !operand[addr-1] && !data_target[addr-1] )
         {
            threshold=5; // arbitrary
         }
         else if ( !instruction[addr-1] && !operand[addr-1] ) continue;
      
         if ( test_instructions_at_addr(addr,0) > threshold )
         {
            branch_target[addr] = 2; // Fake branch target
            add_label(NULL,addr,0,NULL);
            trace_code(); // Add new block found including branch targets
            ++found;
         }
      }
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
 * find_label()
 *
 * Return the label entry for a given address or NULL
 */
struct label *find_label(int addr)
{
   for (int lab=0;lab<num_labels;++lab)
   {
      if ( labels[lab].addr == addr ) return &labels[lab];
   }
   return NULL;
}

/*
 * find_strings()
 *
 * Look through data sections for text strings.
 * To-do: Checks for regular text or screen-code text.
 */
struct string_table { char *str; int len; };
#define STRING_TABLE_ENTRY(_s) { _s, sizeof(_s)-1 }
const struct string_table string_table[] = {
   STRING_TABLE_ENTRY("ATARI"),
   STRING_TABLE_ENTRY("atari"),
   STRING_TABLE_ENTRY("COPYRIGHT"),
   STRING_TABLE_ENTRY("Copyright"),
   STRING_TABLE_ENTRY("copyright"),
   STRING_TABLE_ENTRY("PRESS"),
   STRING_TABLE_ENTRY("TRIGGER"),
   STRING_TABLE_ENTRY("PLEASE"),
};
void string_to_base(unsigned char *dest,const char *src,enum base_overload base)
{
   while ( *src )
   {
      switch (base)
      {
         case E_ATASCII_STRING:
            *dest++ = *src++;
            break;
         case E_SCREEN_STRING:
            *dest++ = ATASCII_TO_SCREEN(*src);
            ++src;
            break;
         case E_ATASCII_INVERSE_STRING:
            *dest++ = (*src++ | 0x80);
            break;
         case E_SCREEN_INVERSE_STRING:
            *dest++ = (ATASCII_TO_SCREEN(*src) | 0x80);
            ++src;
            break;
         default: break; // not reached
      }
   }
   *dest = 0;
}
int is_char_match_base(unsigned char c,enum base_overload base)
{
   switch (base)
   {
      case E_ATASCII_STRING:
         return IS_ASCII(c);
         break;
      case E_SCREEN_STRING:
         return IS_SCREEN_ASCII(c);
         break;
      case E_ATASCII_INVERSE_STRING:
         return IS_ASCII(c^0x80);
         break;
      case E_SCREEN_INVERSE_STRING:
         return IS_SCREEN_ASCII(c^0x80);
         break;
      default:
         return 0; // not reached
   }
}
void find_strings(void)
{
   // FIXME: The performance of this is horrible, but my computer is stupid fast
   for (int addr=0;addr<0xffff;++addr)
   {
      if ( !mem_loaded[addr] ) continue;
      if ( instruction[addr] ) continue;
      for ( int base=E_ATASCII_STRING; base >= E_SCREEN_INVERSE_STRING; --base )
      {
         for ( unsigned int s=0; s<ARRAY_SIZE(string_table); ++s )
         {
            unsigned char str[128];
            string_to_base(str,string_table[s].str,base);
            int match=1;
            for ( int c=0;c<string_table[s].len;++c )
            {
               if ( addr+c <= 0xffff &&
                    mem[addr+c] == str[c] &&
                    mem_loaded[addr+c] &&
                    !instruction[addr+c] )
                  continue;
               else
               {
                  match=0;
                  break;
               }
            }
            if ( !match ) continue;

            // Don't count it if there's a label in the middle of the match string
            for ( int c=1;c<string_table[s].len;++c )
            {
               if ( find_label(addr+c) )
               {
                  match=0;
                  break;
               }
            }
            if ( !match ) continue;

            // Expand to collect neighboring characters
            {
               int start = addr;
               int len = string_table[s].len;
               // Expand backwards
               while ( !find_label(start) &&
                       start > 0 &&
                       !instruction[start-1] &&
                       mem_loaded[start-1] &&
                       is_char_match_base(mem[start-1],base) )
               {
                  --start;
                  ++len;
               }
               struct label *l = find_label(start-1);
               if ( l && l->bytes > 1 ) break; // Already part of a label
               // Extend forwards
               while ( !find_label(start+len) &&
                       start+len < 0xffff &&
                       !instruction[start+len] &&
                       mem_loaded[start+len] &&
                       is_char_match_base(mem[start+len],base) )
               {
                  ++len;
               }
               // Add the label
               l = find_label(start);
               if ( l )
               {
                  if ( l->bytes != 0 ) break; // label already set
                  if ( l->base != 0 ) break; // non-default already
                  l->bytes = len;
                  l->base = base;
                  break; // set
               }
               struct label label_str = { .addr=start, .name="", .bytes=len, .rw='a', .btype=1, .base=base, .defined=0 };
               add_label(NULL,start,0,&label_str);
               break;
            }
         }
      }
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
 *
 * Also find any text strings in data and change the type as appropriate.
 */
void fix_up_labels(void)
{
   for (int lab=0;lab<num_labels;++lab)
   {
      int addr = -1;
      const char *name;

      if ( strchr(labels[lab].name,'-') ) continue; // Manual offset labels are assumed to be valid
      if ( !mem_loaded[labels[lab].addr] ) continue;
      if ( instruction[labels[lab].addr] ) continue;
      if ( instruction[labels[lab].addr-1] && instruction_bytes[opcode[mem[labels[lab].addr-1]].mode] >= 2 ) addr = labels[lab].addr - 1;
      if ( instruction[labels[lab].addr-2] && instruction_bytes[opcode[mem[labels[lab].addr-2]].mode] >= 3 ) addr = labels[lab].addr - 2;
      if ( addr < 0 ) continue;
      name = add_label(NULL,addr,0,NULL);
      sprintf(labels[lab].name,"%s+%d",name,labels[lab].addr-addr);
   }

   find_strings();
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
      if ( !write && labels[lab].rw == 'w' ) continue;
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
 * do_indent()
 */
void do_indent(int chars_printed)
{
   if ( syntax.indent_tab )
   {
      int tabs = syntax.indent_count;
      if ( chars_printed / 8 >= tabs ) tabs = 1; // Always indent some
      else tabs -= chars_printed/8;
      for (int i=0;i<tabs;++i) printf("\t");
   }
   else
   {
      int spaces = syntax.indent_count;
      if ( chars_printed >= spaces ) spaces = 1; // Always indent some
      else spaces -= chars_printed;
      printf("%*s",spaces,"");
   }
}

/*
 * output_disasm()
 */
void output_disasm(void)
{
   static int block=1; // Next block to process
   int max_block = block;
   int chars_printed;

   // Display labels that are outside loaded memory
   for (int lab=0;lab<num_labels;++lab)
   {
      if ( labels[lab].defined ) continue;
      if ( strchr(labels[lab].name,'+') ) continue; // Don't need to print 'LABEL+1' fake labels
      if ( !mem_loaded[labels[lab].addr] )
      {
         chars_printed = 0;
         char *c = strchr(labels[lab].name,',');
         if ( syntax.listing ) printf("              | ");
         if ( c ) // separate read/write labels
         {
            char name[16];
            strcpy(name,labels[lab].name);
            strchr(name,',')[0]=0;
            chars_printed=printf("%s",name);
            do_indent(chars_printed);
            printf("= $%04X "COMMENT" read register\n",labels[lab].addr);
            if ( syntax.listing ) printf("              | ");
            chars_printed=printf("%s",c+1);
            do_indent(chars_printed);
            printf("= $%04X "COMMENT" write register\n",labels[lab].addr);
         }
         else // normal case
         {
            chars_printed=printf("%s",labels[lab].name);
            do_indent(chars_printed);
            printf("= $%04X\n",labels[lab].addr);
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
         chars_printed = 0;
         if ( !set )
         {
            if ( syntax.listing ) printf("              | ");
            if ( syntax.org )
            {
               if ( syntax.orgdot ) chars_printed = printf(".");
               chars_printed += printf("org");
               //if ( syntax.colon ) printf(":");
            }
            do_indent(chars_printed);
            if ( !syntax.org ) printf("*= ");
            printf("$%04X\n",addr);
            set=1;
         }

         // If listing option, display hex codes
         if ( syntax.listing )
         {
            printf("%04X %02X ",addr,mem[addr]);
            if ( instruction[addr] )
            {
               switch( instruction_bytes[opcode[mem[addr]].mode] )
               {
                  case 1:
                     printf("      ");
                     break;
                  case 2:
                     printf("%02X    ",mem[addr+1]);
                     break;
                  case 3:
                     printf("%02X %02X ",mem[addr+1],mem[addr+2]);
                     break;
               }
            }
            else
            {
               // FIXME: handle multi-byte data
               printf("      ");
            }
            printf("| ");
         }

         // Display this address
         chars_printed = 0;
         for (lab=0;lab<num_labels;++lab)
         {
            if ( labels[lab].addr == addr )
            {
               if ( !strchr(labels[lab].name,'+') && !strchr(labels[lab].name,'-') )
               {
                  chars_printed += printf("%s",labels[lab].name);
                  if ( syntax.colon ) chars_printed += printf(":");
               }
               break;
            }
            // Get label if in a longer label (for base and type)
            if ( labels[lab].addr < addr && labels[lab].addr+labels[lab].bytes > addr )
            {
               if ( strchr(labels[lab].name,'+') || strchr(labels[lab].name,'-') ) continue;
               break;
            }
         }
         if ( lab >= num_labels )
         {
            lab = -1;
         }
         do_indent(chars_printed);

         if ( !instruction[addr] )
         {
            // check for word labels, but not at odd offsets
            if ( lab >= 0 && labels[lab].btype == 2 && labels[lab].bytes >= 2 && (labels[lab].bytes&0x01)==0 )
            {
               unsigned int val = le16toh(*(uint16_t *)&mem[addr]);
               // look up label for val
               struct label *val_label = find_label(val);
               // if label found, use that
               if ( val_label )
               {
                  printf(""WORD_PSEUDO_OP""POST_OPCODE"%s",val_label->name);
               }
               else // else display data
               {
                  int base = 16;
                  if ( lab >= 0 ) base = labels[lab].base;
                  switch(base)
                  {
                     case 2:
                        printf(""WORD_PSEUDO_OP""POST_OPCODE"%%");
                        for ( int i=0x8000; i; i=i>>1 )
                        {
                           printf("%d",(val&i)>0);
                        }
                        break;
                     case 8:
                        printf(""WORD_PSEUDO_OP""POST_OPCODE"&%o",val);
                        break;
                     case 10:
                        printf(""WORD_PSEUDO_OP""POST_OPCODE"%u",val);
                        break;
                     case 16:
                     default: // for words, strings make no sense
                        printf(""WORD_PSEUDO_OP""POST_OPCODE"$%04X",val);
                        break;
                  }
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
                     printf(""BYTE_PSEUDO_OP""POST_OPCODE"%%");
                     for ( int i=0x80; i; i=i>>1 )
                     {
                        printf("%d",(val&i)>0);
                     }
                     break;
                  case 8:
                     printf(""BYTE_PSEUDO_OP""POST_OPCODE"&%o",val);
                     break;
                  case 10:
                     printf(""BYTE_PSEUDO_OP""POST_OPCODE"%u",val);
                     break;
                  case E_SCREEN_STRING:
                     if ( syntax.screenquote )
                     {
                        if ( count > STRING_MAX ) count = STRING_MAX;
                        if ( IS_SCREEN_QUOTABLE(val) )
                        {
                           printf(""BYTE_PSEUDO_OP""POST_OPCODE"%c",syntax.screenquote);
                           for ( int i=0; i<count; ++i )
                           {
                              if ( IS_SCREEN_QUOTABLE(mem[addr]) )
                              {
                                 printf("%c",SCREEN_TO_ATASCII(mem[addr]));
                                 ++addr;
                              }
                              else break;
                           }
                           --addr;
                           printf("%c",syntax.screenquote);
                        }
                        else printf(""BYTE_PSEUDO_OP""POST_OPCODE"$%02X "COMMENT" Screen code for '%c'",val,SCREEN_TO_ATASCII(val));
                     }
                     else
                     {
                        printf(""BYTE_PSEUDO_OP""POST_OPCODE"$%02X",val);
                        if ( !syntax.noscreencode && IS_SCREEN_QUOTABLE(val) )
                           printf(" "COMMENT" Screen code for '%c'",SCREEN_TO_ATASCII(val));
                     }
                     break;
                  case E_ATASCII_STRING:
                     // If a string, consume as many bytes as possible within the label
                     if ( count > STRING_MAX ) count = STRING_MAX;
                     if ( IS_QUOTABLE(val) )
                     {
                        printf(""BYTE_PSEUDO_OP""POST_OPCODE"%c",syntax.stringquote);
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
                        printf("%c",syntax.stringquote);
                        break;
                     }
                     // else: ATASCII-specific character in hex
                     // fall through
                  case E_ATASCII_INVERSE_STRING:
                     if ( syntax.mads )
                     {
                        if ( count > STRING_MAX ) count = STRING_MAX;
                        if ( IS_QUOTABLE(val^0x80) )
                        {
                           printf("dta c'");
                           for ( int i=0; i<count; ++i )
                           {
                              if ( IS_QUOTABLE(mem[addr]^0x80) )
                              {
                                 printf("%c",mem[addr]^0x80);
                                 ++addr;
                              }
                              else break;
                           }
                           --addr;
                           printf("'* "COMMENT" inverse");
                           break;
                        }
                     }
                     printf(""BYTE_PSEUDO_OP""POST_OPCODE"$%02X",val);
                     if ( IS_ASCII(val^0x80) )
                        printf(" "COMMENT" Inverse character '%c'",val^0x80);
                     break;
                  case E_SCREEN_INVERSE_STRING:
                     if ( syntax.mads )
                     {
                        if ( count > STRING_MAX ) count = STRING_MAX;
                        if ( IS_QUOTABLE(val^0x80) )
                        {
                           printf("dta d'");
                           for ( int i=0; i<count; ++i )
                           {
                              if ( IS_QUOTABLE(mem[addr]^0x80) )
                              {
                                 printf("%c",mem[addr]^0x80);
                                 ++addr;
                              }
                              else break;
                           }
                           --addr;
                           printf("'*  "COMMENT" inverse screen-codes");
                           break;
                        }
                     }
                     printf(""BYTE_PSEUDO_OP""POST_OPCODE"$%02X",val);
                     if ( !syntax.noscreencode && IS_SCREEN_QUOTABLE(val^0x80) )
                        printf(" "COMMENT" Screen code for inverse '%c'",SCREEN_TO_ATASCII(val^0x80));
                     break;
                  default:
                  case 16:
                     printf(""BYTE_PSEUDO_OP""POST_OPCODE"$%02X",val);
                     if ( IS_ASCII(val) )
                        printf(" "COMMENT" '%c'",val);
                     if ( !syntax.noscreencode && IS_SCREEN_QUOTABLE(val) && val != SCREEN_TO_ATASCII(val) )
                        printf(" "COMMENT" Screen code for '%c'",SCREEN_TO_ATASCII(val));
                     break;
               }
            }
            printf("\n");
         }
         else
         {
            int didbytes = 0;
            if ( opcode[mem[addr]].unofficial &&
                 ( syntax.noundoc || strcmp("NOP",opcode[mem[addr]].mnemonic) == 0 ) )
            {
               int bytes = instruction_bytes[opcode[mem[addr]].mode];
               printf(""BYTE_PSEUDO_OP""POST_OPCODE"");
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
            if ( opcode[mem[addr]].mode != E_IMPLIED && opcode[mem[addr]].mode != E_ACCUMULATOR ) printf (POST_OPCODE); // tab or space after opcode
            switch(opcode[mem[addr]].mode)
            {
               case E_IMPLIED: break;
               case E_ACCUMULATOR:
                  if ( !syntax.noa )
                  {
                     printf(POST_OPCODE"A");
                  }
                  break;
               case E_IMMEDIATE:
                  printf("#$%02X",mem[addr+1]);
                  break;
               case E_ABSOLUTE:
                  print_label_or_addr(target,write);
                  break;
               case E_ABSOLUTE_X:
                  print_label_or_addr(target,write);
                  printf(",X");
                  break;
               case E_ABSOLUTE_Y:
                  print_label_or_addr(target,write);
                  printf(",Y");
                  break;
               case E_INDIRECT:
                  printf("(");
                  print_label_or_addr(target,0);
                  printf(")");
                  break;
               case E_ZEROPAGE:
                  print_label_or_addr(ztarget,write);
                  break;
               case E_ZEROPAGE_X:
                  print_label_or_addr(ztarget,write);
                  printf(",X");
                  break;
               case E_ZEROPAGE_Y:
                  print_label_or_addr(ztarget,write);
                  printf(",Y");
                  break;
               case E_ZEROPAGE_IND_X:
                  printf("(");
                  print_label_or_addr(ztarget,write);
                  printf(",X)");
                  break;
               case E_ZEROPAGE_IND_Y:
                  printf("(");
                  print_label_or_addr(ztarget,write);
                  printf("),Y");
                  break;
               case E_RELATIVE:
                  print_label_or_addr(btarget,0);
                  break;
               default: break; // Not reached
            }
            if ( opcode[mem[addr]].unofficial && !didbytes )
            {
               printf(POST_OPCODE""COMMENT" (undocumented opcode)");
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
          " --start=[xxxx]  Specify another starting address (repeat as needed)\n"
          " --labels=atari,cio,float,basic Specify wanted labels (basic off by default)\n"
          " --ltable=[filename] Load label table from a file (may be repeated)\n"
          " --lfile=[filename]  Load active labels from a file (may be repeated)\n"
          " --noundoc       Undocumented opcodes imply data, not instructions\n"
          " --syntax=[option][,option]  Set various syntax options:\n"
          "     bracket      Use brackets for label math: [LABEL+1]\n"
          "     noa          Leave off the 'A' on ASL, ROR, and the like\n"
          "     org          Use '.org =' instead of '*=' to set PC\n"
          "     colon        Put a colon after labels\n"
          "     noundoc      Use comments for undocumented opcodes\n"
          "     mads         Defaults for MADS assembler: noa,org,colon\n"
          "     ca65         Defaults for ca65 assembler: noa,org,colon\n"
          "     cc65         Alias for ca65\n"
          "     xa           Defaults for xa assembler: noundoc \n"
          "     asmedit      Defaults for Atari Assembler/Editor cartridge: noundoc \n"
          "     noscreencode Do not add comments about screen code characters\n"
          "     listing      Print address and hex codes for each line (broken for multi-byte data\n"
          "     indent=[#][s|t]   Specify a number of spaces or tabs to indent (default: 1t)\n"
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
   syntax.stringquote = '"'; // Works with most
   syntax.indent_count = 1; // default to 1 tab to indent
   syntax.indent_tab = 1;
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
            if ( strncmp(opt,"noscreencode",sizeof("noscreencode")-1)==0 )
            {
               syntax.noscreencode = 1;
               opt+=sizeof("noscreencode")-1;
               if ( *opt == 's' ) ++opt; // Allow plural
               continue;
            }

            // select defaults for a given assembler
            if ( strncmp(opt,"mads",sizeof("mads")-1)==0 )
            {
               syntax.noa = 1;
               syntax.org = 1;
               syntax.colon = 1;
               syntax.stringquote = '\'';
               syntax.screenquote = '"';
               syntax.mads = 1;
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
            if ( strncmp(opt,"listing",sizeof("listing")-1)==0 )
            {
               syntax.listing = 1;
               opt+=sizeof("listing")-1;
               continue;
            }
            if ( strncmp(opt,"indent=",sizeof("indent=")-1)==0 )
            {
               const void *save_opt = opt;
               opt+=sizeof("indent=")-1;
               syntax.indent_count=atoi(opt);
               while (isdigit(*opt)) ++opt;
               if ( *opt == 't' )
               {
                  syntax.indent_tab = 1;
                  ++opt;
                  continue;
               }
               else if ( *opt == 's' )
               {
                  syntax.indent_tab = 0;
                  ++opt;
                  continue;
               }
               // Bad syntax; abort
               opt = save_opt;
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
      if ( strncmp(argv[1],"--noundoc",sizeof("--noundoc")-1) == 0 )
      {
         noundoc = 1;
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
            add_label(NULL,t[i].addr,0,NULL); // Will get offsets added automatically
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
   if ( data == MAP_FAILED )
   {
      fprintf(stderr,"Unable to memory map source file: %s\n",argv[1]);
      return 2;
   }
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
   find_blocks();
   fix_up_labels();
   sort_labels();
   output_disasm();
}
