/**
 * Copyright (C) 2022 by Mahyar Koshkouei <mk@deltabeard.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

// Peanut-GB emulator settings
#define ENABLE_LCD 1
#define ENABLE_SOUND 0
#define ENABLE_SDCARD 1
#define USE_PS2_KBD 1
#define PEANUT_GB_HIGH_LCD_ACCURACY 1
#define PEANUT_GB_USE_BIOS 0

/* C Headers */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RP2040 Headers */
#include <hardware/pio.h>
#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/spi.h>
#include <hardware/sync.h>
#include <hardware/flash.h>
#include <hardware/timer.h>
#include <hardware/vreg.h>
#include <pico/bootrom.h>
#include <pico/stdio.h>
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <sys/unistd.h>
#include <hardware/irq.h>
#include <hardware/pwm.h> // pwm

/* Project headers */
#include "hedley.h"
#include "minigb_apu.h"
#include "peanut_gb.h"
#include "gbcolors.h"

/* Murm*/
#include "vga.h"
#include "ps2kbd_mrmltr.h"
#include "f_util.h"
#include "ff.h"

/** Definition of ROM data
 * We're going to erase and reprogram a region 1Mb from the start of the flash
 * Once done, we can access this at XIP_BASE + 1Mb.
 * Game Boy DMG ROM size ranges from 32768 bytes (e.g. Tetris) to 1,048,576 bytes (e.g. Pokemod Red)
 */
#define FLASH_TARGET_OFFSET (512 * 1024)
const uint8_t *rom = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);
static unsigned char rom_bank0[8192];

static uint8_t ram[32768];

static const sVmode *vmode = NULL;
struct semaphore vga_start_semaphore;

static FATFS fs;

#if ENABLE_SOUND
#define AUDIO_PIN 27 // you can change this to whatever you like
int16_t stream[1098];

#endif
#define X2(a) (a | (a << 8))
#define X4(a) (a | (a << 8) | (a << 16) | (a << 24))
#define VGA_RGB_222(r, g, b) ((r << 4) | (g << 2) | b)
// Function to convert RGB565 to RGB222
uint8_t convertRGB565toRGB222(uint16_t color565)
{
	// Extract the red, green, and blue components from the RGB565 color
	uint8_t red = (color565 >> 11) & 0x1F;
	uint8_t green = (color565 >> 5) & 0x3F;
	uint8_t blue = color565 & 0x1F;

	return VGA_RGB_222(((red * 255) / 31) >> 6, ((green * 255) / 63) >> 6, ((blue * 255) / 31) >> 6);
}

typedef uint8_t palette222_t[3][4];
static palette222_t palette;
static palette_t palette16; // Colour palette
static uint8_t manual_palette_selected = 0;

struct joypad_bits_t
{
	bool a : true;
	bool b : true;
	bool select : true;
	bool start : true;
	bool right : true;
	bool left : true;
	bool up : true;
	bool down : true;
	bool home : true;
};

static joypad_bits_t joypad_bits = {true, true, true, true, true, true, true, true, true};
static joypad_bits_t prev_joypad_bits = {true, true, true, true, true, true, true, true, true};
struct gb_s gb;
uint16_t screen[LCD_HEIGHT][LCD_WIDTH];

#define putstdio(x) write(1, x, strlen(x))

void mk_ili9225_get_letter(uint8_t *fbuf, char l, uint8_t color, uint8_t bgcolor)
{
	uint8_t letter[8];
	uint8_t row;

	switch (l)
	{
	case 'a':
	case 'A':
	{
		const uint8_t letter_[8] = {0b00111100,
									0b01100110,
									0b01100110,
									0b01111110,
									0b01100110,
									0b01100110,
									0b01100110,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'b':
	case 'B':
	{
		const uint8_t letter_[8] = {0b01111100,
									0b01100110,
									0b01100110,
									0b01111100,
									0b01100110,
									0b01100110,
									0b01111100,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'c':
	case 'C':
	{
		const uint8_t letter_[8] = {0b00011110,
									0b00110000,
									0b01100000,
									0b01100000,
									0b01100000,
									0b00110000,
									0b00011110,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'd':
	case 'D':
	{
		const uint8_t letter_[8] = {0b01111000,
									0b01101100,
									0b01100110,
									0b01100110,
									0b01100110,
									0b01101100,
									0b01111000,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'e':
	case 'E':
	{
		const uint8_t letter_[8] = {0b01111110,
									0b01100000,
									0b01100000,
									0b01111000,
									0b01100000,
									0b01100000,
									0b01111110,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'f':
	case 'F':
	{
		const uint8_t letter_[8] = {0b01111110,
									0b01100000,
									0b01100000,
									0b01111000,
									0b01100000,
									0b01100000,
									0b01100000,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'g':
	case 'G':
	{
		const uint8_t letter_[8] = {0b00111100,
									0b01100110,
									0b01100000,
									0b01101110,
									0b01100110,
									0b01100110,
									0b00111110,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'h':
	case 'H':
	{
		const uint8_t letter_[8] = {0b01100110,
									0b01100110,
									0b01100110,
									0b01111110,
									0b01100110,
									0b01100110,
									0b01100110,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'i':
	case 'I':
	{
		const uint8_t letter_[8] = {0b00111100,
									0b00011000,
									0b00011000,
									0b00011000,
									0b00011000,
									0b00011000,
									0b00111100,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'j':
	case 'J':
	{
		const uint8_t letter_[8] = {0b00000110,
									0b00000110,
									0b00000110,
									0b00000110,
									0b00000110,
									0b01100110,
									0b00111100,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'k':
	case 'K':
	{
		const uint8_t letter_[8] = {0b11000110,
									0b11001100,
									0b11011000,
									0b11110000,
									0b11011000,
									0b11001100,
									0b11000110,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'l':
	case 'L':
	{
		const uint8_t letter_[8] = {0b01100000,
									0b01100000,
									0b01100000,
									0b01100000,
									0b01100000,
									0b01100000,
									0b01111110,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'm':
	case 'M':
	{
		const uint8_t letter_[8] = {0b11000110,
									0b11101110,
									0b11111110,
									0b11010110,
									0b11000110,
									0b11000110,
									0b11000110,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'n':
	case 'N':
	{
		const uint8_t letter_[8] = {0b11000110,
									0b11100110,
									0b11110110,
									0b11011110,
									0b11001110,
									0b11000110,
									0b11000110,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'o':
	case 'O':
	{
		const uint8_t letter_[8] = {0b00111100,
									0b01100110,
									0b01100110,
									0b01100110,
									0b01100110,
									0b01100110,
									0b00111100,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'p':
	case 'P':
	{
		const uint8_t letter_[8] = {0b01111100,
									0b01100110,
									0b01100110,
									0b01111100,
									0b01100000,
									0b01100000,
									0b01100000,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'q':
	case 'Q':
	{
		const uint8_t letter_[8] = {0b01111000,
									0b11001100,
									0b11001100,
									0b11001100,
									0b11001100,
									0b11011100,
									0b01111110,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'r':
	case 'R':
	{
		const uint8_t letter_[8] = {0b01111100,
									0b01100110,
									0b01100110,
									0b01111100,
									0b01101100,
									0b01100110,
									0b01100110,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 's':
	case 'S':
	{
		const uint8_t letter_[8] = {0b00111100,
									0b01100110,
									0b01110000,
									0b00111100,
									0b00001110,
									0b01100110,
									0b00111100,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 't':
	case 'T':
	{
		const uint8_t letter_[8] = {0b01111110,
									0b00011000,
									0b00011000,
									0b00011000,
									0b00011000,
									0b00011000,
									0b00011000,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'u':
	case 'U':
	{
		const uint8_t letter_[8] = {0b01100110,
									0b01100110,
									0b01100110,
									0b01100110,
									0b01100110,
									0b01100110,
									0b00111100,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'v':
	case 'V':
	{
		const uint8_t letter_[8] = {0b01100110,
									0b01100110,
									0b01100110,
									0b01100110,
									0b00111100,
									0b00111100,
									0b00011000,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'w':
	case 'W':
	{
		const uint8_t letter_[8] = {0b11000110,
									0b11000110,
									0b11000110,
									0b11010110,
									0b11111110,
									0b11101110,
									0b11000110,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'x':
	case 'X':
	{
		const uint8_t letter_[8] = {0b11000011,
									0b01100110,
									0b00111100,
									0b00011000,
									0b00111100,
									0b01100110,
									0b11000011,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'y':
	case 'Y':
	{
		const uint8_t letter_[8] = {0b11000011,
									0b01100110,
									0b00111100,
									0b00011000,
									0b00011000,
									0b00011000,
									0b00011000,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case 'z':
	case 'Z':
	{
		const uint8_t letter_[8] = {0b11111110,
									0b00001100,
									0b00011000,
									0b00110000,
									0b01100000,
									0b11000000,
									0b11111110,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case '-':
	{
		const uint8_t letter_[8] = {0b00000000,
									0b00000000,
									0b00000000,
									0b01111110,
									0b00000000,
									0b00000000,
									0b00000000,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case '(':
	case '[':
	case '{':
	{
		const uint8_t letter_[8] = {0b00001100,
									0b00011000,
									0b00110000,
									0b00110000,
									0b00110000,
									0b00011000,
									0b00001100,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case ')':
	case ']':
	case '}':
	{
		const uint8_t letter_[8] = {0b00110000,
									0b00011000,
									0b00001100,
									0b00001100,
									0b00001100,
									0b00011000,
									0b00110000,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case ',':
	{
		const uint8_t letter_[8] = {0b00000000,
									0b00000000,
									0b00000000,
									0b00000000,
									0b00000000,
									0b00011000,
									0b00011000,
									0b00110000};
		memcpy(letter, letter_, 8);
		break;
	}

	case '.':
	{
		const uint8_t letter_[8] = {0b00000000,
									0b00000000,
									0b00000000,
									0b00000000,
									0b00000000,
									0b00011000,
									0b00011000,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case '!':
	{
		const uint8_t letter_[8] = {0b00011000,
									0b00011000,
									0b00011000,
									0b00011000,
									0b00011000,
									0b00000000,
									0b00011000,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case '&':
	{
		const uint8_t letter_[8] = {0b00111000,
									0b01101100,
									0b01101000,
									0b01110110,
									0b11011100,
									0b11001110,
									0b01111011,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case '\'':
	{
		const uint8_t letter_[8] = {0b00011000,
									0b00011000,
									0b00110000,
									0b00000000,
									0b00000000,
									0b00000000,
									0b00000000,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case '0':
	{
		const uint8_t letter_[8] = {0b00111100,
									0b01100110,
									0b01101110,
									0b01111110,
									0b01110110,
									0b01100110,
									0b00111100,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case '1':
	{
		const uint8_t letter_[8] = {0b00011000,
									0b00111000,
									0b01111000,
									0b00011000,
									0b00011000,
									0b00011000,
									0b00011000,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case '2':
	{
		const uint8_t letter_[8] = {0b00111100,
									0b01100110,
									0b00000110,
									0b00001100,
									0b00011000,
									0b00110000,
									0b01111110,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case '3':
	{
		const uint8_t letter_[8] = {0b00111100,
									0b01100110,
									0b00000110,
									0b00011100,
									0b00000110,
									0b01100110,
									0b00111100,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case '4':
	{
		const uint8_t letter_[8] = {0b00011100,
									0b00111100,
									0b01101100,
									0b11001100,
									0b11111110,
									0b00001100,
									0b00001100,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case '5':
	{
		const uint8_t letter_[8] = {0b01111110,
									0b01100000,
									0b01111100,
									0b00000110,
									0b00000110,
									0b01100110,
									0b00111100,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case '6':
	{
		const uint8_t letter_[8] = {0b00011100,
									0b00110000,
									0b01100000,
									0b01111100,
									0b01100110,
									0b01100110,
									0b00111100,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case '7':
	{
		const uint8_t letter_[8] = {0b01111110,
									0b00000110,
									0b00000110,
									0b00001100,
									0b00011000,
									0b00011000,
									0b00011000,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case '8':
	{
		const uint8_t letter_[8] = {0b00111100,
									0b01100110,
									0b01100110,
									0b00111100,
									0b01100110,
									0b01100110,
									0b00111100,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	case '9':
	{
		const uint8_t letter_[8] = {0b00111100,
									0b01100110,
									0b01100110,
									0b00111110,
									0b00000110,
									0b00001100,
									0b00111000,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}

	default:
	{
		const uint8_t letter_[8] = {0b00000000,
									0b00000000,
									0b00000000,
									0b00000000,
									0b00000000,
									0b00000000,
									0b00000000,
									0b00000000};
		memcpy(letter, letter_, 8);
		break;
	}
	}

	for (uint8_t y = 0; y < 8; y++)
	{
		row = letter[y];
		for (uint8_t x = 0; x < 8; x++)
		{
			if (row & 128)
			{
				fbuf[y * 8 + x] = color;
			}
			else
			{
				fbuf[y * 8 + x] = bgcolor;
			}
			row = row << 1;
		}
	}
}

void mk_ili9225_text(char *s, uint8_t x, uint8_t y, uint8_t color, uint8_t bgcolor)
{
	uint8_t fbuf[8 * 8];

	for (uint8_t i = 0; i < strlen(s); i++)
	{
		mk_ili9225_get_letter(fbuf, s[i], color, bgcolor);

		for (uint8_t row = 0; row < 8; row++)
		{
			// uint32_t *buf = (uint32_t *)&(linebuf->line);
			memcpy(&screen[row + y][x], &fbuf[row * 8], 8);
		}

		//	 Грязный хак чтобы записать 8 байт
		x += 4;
		if (i >= 39)
		{
			break;
		}
	}
}

void print(hid_keyboard_report_t const *report)
{
	printf("HID key report modifiers %2.2X report ", report->modifier);
	for (int i = 0; i < 6; ++i)
		printf("%2.2X", report->keycode[i]);
	printf("\n");
}

static bool isInReport(hid_keyboard_report_t const *report, const unsigned char keycode)
{
	for (unsigned int i = 0; i < 6; i++)
	{
		if (report->keycode[i] == keycode)
			return true;
	}
	return false;
}

void __not_in_flash_func(process_kbd_report)(hid_keyboard_report_t const *report, hid_keyboard_report_t const *prev_report)
{
	print(report);
	// HOME button
	if (isInReport(report, 0x4A))
	{
		joypad_bits.home = 0;
	}
	else if (joypad_bits.home == 0 && !isInReport(report, 0x4A))
	{
		joypad_bits.home = 1;
	}
	if (isInReport(report, 0x28))
	{
		joypad_bits.start = 0;
	}
	else if (joypad_bits.start == 0 && !isInReport(report, 0x28))
	{
		joypad_bits.start = 1;
	}
	if (isInReport(report, 0x2A))
	{
		joypad_bits.select = 0;
	}
	else if (joypad_bits.select == 0 && !isInReport(report, 0x2A))
	{
		joypad_bits.select = 1;
	}

	if (isInReport(report, 0x1D))
	{
		joypad_bits.a = 0;
	}
	else if (joypad_bits.a == 0 && !isInReport(report, 0x1D))
	{
		joypad_bits.a = 1;
	}
	if (isInReport(report, 0x1B))
	{
		joypad_bits.b = 0;
	}
	else if (joypad_bits.b == 0 && !isInReport(report, 0x1B))
	{
		joypad_bits.b = 1;
	}

	if (isInReport(report, 0x52))
	{
		joypad_bits.up = 0;
	}
	else if (joypad_bits.up == 0 && !isInReport(report, 0x52))
	{
		joypad_bits.up = 1;
	}
	if (isInReport(report, 0x51))
	{
		joypad_bits.down = 0;
	}
	else if (joypad_bits.down == 0 && !isInReport(report, 0x51))
	{
		joypad_bits.down = 1;
	}
	if (isInReport(report, 0x50))
	{
		joypad_bits.left = 0;
	}
	else if (joypad_bits.left == 0 && !isInReport(report, 0x50))
	{
		joypad_bits.left = 1;
	}
	if (isInReport(report, 0x4F))
	{
		joypad_bits.right = 0;
	}
	else if (joypad_bits.right == 0 && !isInReport(report, 0x4F))
	{
		joypad_bits.right = 1;
	}
}

#ifdef USE_PS2_KBD
Ps2Kbd_Mrmltr ps2kbd(
	pio1,
	0,
	process_kbd_report);
#endif

/**
 * Returns a byte from the ROM file at the given address.
 */
uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
	(void)gb;
	if (addr < sizeof(rom_bank0))
		return rom_bank0[addr];

	return rom[addr];
}

/**
 * Returns a byte from the cartridge RAM at the given address.
 */
uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
	(void)gb;
	return ram[addr];
}

/**
 * Writes a given byte to the cartridge RAM at the given address.
 */
void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr,
					   const uint8_t val)
{
	ram[addr] = val;
}

/**
 * Ignore all errors.
 */
void gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t addr)
{
#if 1
	const char *gb_err_str[4] = {
		"UNKNOWN",
		"INVALID OPCODE",
		"INVALID READ",
		"INVALID WRITE"};
	printf("Error %d occurred: %s at %04X\n.\n", gb_err, gb_err_str[gb_err], addr);
//	abort();
#endif
}

/* Renderer loop on Pico's second core */
void __time_critical_func(render_loop)()
{
	multicore_lockout_victim_init();
	VgaLineBuf *linebuf;
	printf("Video on Core#%i running...\n", get_core_num());

	sem_acquire_blocking(&vga_start_semaphore);
	VgaInit(vmode, 640, 480);

	while (linebuf = get_vga_line())
	{

		// uint32_t *buf = (uint32_t *)&(linebuf->line);
		uint32_t y = linebuf->row;

		if (y >= 48 && y < 48 + LCD_HEIGHT)
		{
			memcpy(&linebuf->line[160], &screen[y - 48][0], LCD_WIDTH * 2);
		}
		else
		{
			memset(&linebuf->line, 0, 640);
		}
	}

	HEDLEY_UNREACHABLE();
}

#if ENABLE_LCD
/**
 * Draws scanline into framebuffer.
 */
void lcd_draw_line(struct gb_s *gb, const uint8_t pixels[160], const uint_fast8_t y)
{

	// struct priv_t *priv = (priv_t *)(gb->direct.priv);

	for (unsigned int x = 0; x < LCD_WIDTH; x++)
		screen[y][x] = X2(palette[(pixels[x] & LCD_PALETTE_ALL) >> 4]
								 [pixels[x] & 3]);
	// my_palette[pixels[x] & 3];
}
#endif

#if ENABLE_SDCARD
/**
 * Load a save file from the SD card
 */
void read_cart_ram_file(struct gb_s *gb)
{
	char filename[16];
	uint_fast32_t save_size;
	UINT br;

	gb_get_rom_name(gb, filename);
	save_size = gb_get_save_size(gb);
	if (save_size > 0)
	{

		FRESULT fr = f_mount(&fs, "", 1);
		if (FR_OK != fr)
		{
			printf("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
			return;
		}

		FIL fil;
		fr = f_open(&fil, filename, FA_READ);
		if (fr == FR_OK)
		{
			f_read(&fil, ram, f_size(&fil), &br);
		}
		else
		{
			printf("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
		}

		fr = f_close(&fil);
		if (fr != FR_OK)
		{
			printf("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
		}
	}
	printf("I read_cart_ram_file(%s) COMPLETE (%lu bytes)\n", filename, save_size);
}

/**
 * Write a save file to the SD card
 */
void write_cart_ram_file(struct gb_s *gb)
{
	char filename[16];
	uint_fast32_t save_size;
	UINT bw;

	gb_get_rom_name(gb, filename);
	save_size = gb_get_save_size(gb);
	if (save_size > 0)
	{

		FRESULT fr = f_mount(&fs, "", 1);
		if (FR_OK != fr)
		{
			printf("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
			return;
		}

		FIL fil;
		fr = f_open(&fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
		if (fr == FR_OK)
		{
			f_write(&fil, ram, save_size, &bw);
		}
		else
		{
			printf("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
		}

		fr = f_close(&fil);
		if (fr != FR_OK)
		{
			printf("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
		}
	}
	printf("I write_cart_ram_file(%s) COMPLETE (%lu bytes)\n", filename, save_size);
}

/**
 * Load a .gb rom file in flash from the SD card
 */
void load_cart_rom_file(char *filename)
{
	UINT br;
	uint8_t buffer[FLASH_SECTOR_SIZE];
	bool mismatch = false;

	FRESULT fr = f_mount(&fs, "", 1);
	if (FR_OK != fr)
	{
		printf("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
		return;
	}
	FIL fil;
	fr = f_open(&fil, filename, FA_READ);
	if (fr == FR_OK)
	{
		multicore_lockout_start_blocking();
		uint32_t ints = save_and_disable_interrupts();
		uint32_t flash_target_offset = FLASH_TARGET_OFFSET;
		for (;;)
		{
			f_read(&fil, buffer, sizeof buffer, &br);
			if (br == 0)
				break; /* end of file */

			printf("I Erasing target region...\n");
			flash_range_erase(flash_target_offset, FLASH_SECTOR_SIZE);
			printf("I Programming target region...\n");
			flash_range_program(flash_target_offset, buffer, FLASH_SECTOR_SIZE);
			/* Read back target region and check programming */
			printf("I Done. Reading back target region...\n");
			for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++)
			{
				if (rom[flash_target_offset + i] != buffer[i])
				{
					mismatch = true;
				}
			}

			/* Next sector */
			flash_target_offset += FLASH_SECTOR_SIZE;
		}
		restore_interrupts(ints);
		multicore_lockout_end_blocking();
		if (mismatch)
		{
			printf("I Programming successful!\n");
		}
		else
		{
			printf("E Programming failed!\n");
		}
	}
	else
	{
		printf("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
	}

	fr = f_close(&fil);
	if (fr != FR_OK)
	{
		printf("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
	}

	printf("I load_cart_rom_file(%s) COMPLETE (%lu bytes)\n", filename, br);
}

/**
 * Function used by the rom file selector to display one page of .gb rom files
 */
uint16_t rom_file_selector_display_page(char filename[22][256], uint16_t num_page)
{

	DIR dj;
	FILINFO fno;
	FRESULT fr;

	fr = f_mount(&fs, "", 1);
	if (FR_OK != fr)
	{
		printf("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
		return 0;
	}

	/* clear the filenames array */
	for (uint8_t ifile = 0; ifile < 22; ifile++)
	{
		strcpy(filename[ifile], "");
	}

	/* search *.gb files */
	uint16_t num_file = 0;
	fr = f_findfirst(&dj, &fno, "", "*.gb");

	/* skip the first N pages */
	if (num_page > 0)
	{
		while (num_file < num_page * 22 && fr == FR_OK && fno.fname[0])
		{
			num_file++;
			fr = f_findnext(&dj, &fno);
		}
	}

	/* store the filenames of this page */
	num_file = 0;
	while (num_file < 22 && fr == FR_OK && fno.fname[0])
	{
		strcpy(filename[num_file], fno.fname);
		num_file++;
		fr = f_findnext(&dj, &fno);
	}
	f_closedir(&dj);

	/* display *.gb rom files on screen */
	// mk_ili9225_fill(0x0000);
	for (uint8_t ifile = 0; ifile < num_file; ifile++)
	{
		mk_ili9225_text(filename[ifile], 0, ifile * 8, 0xFF, 0x00);
	}
	return num_file;
}

/**
 * The ROM selector displays pages of up to 22 rom files
 * allowing the user to select which rom file to start
 * Copy your *.gb rom files to the root directory of the SD card
 */
void rom_file_selector()
{
	uint16_t num_page;
	char filename[22][256];
	uint16_t num_file;

	printf("Selecting ROM\r\n");

	/* display the first page with up to 22 rom files */
	num_file = rom_file_selector_display_page(filename, num_page);

	/* select the first rom */
	uint8_t selected = 0;
	mk_ili9225_text(filename[selected], 0, selected * 8, 0xFF, 0xF8);
	sleep_ms(3000);

	//			load_cart_rom_file(filename[selected]);
	// return;

	while (true)
	{

#ifdef USE_PS2_KBD
		ps2kbd.tick();
#endif

		if (!joypad_bits.start)
		{
			/* copy the rom from the SD card to flash and start the game */
			load_cart_rom_file(filename[selected]);
			break;
		}
		if (!joypad_bits.down)
		{
			/* select the next rom */
			mk_ili9225_text(filename[selected], 0, selected * 8, 0xFF, 0x00);
			selected++;
			if (selected >= num_file)
				selected = 0;
			mk_ili9225_text(filename[selected], 0, selected * 8, 0xFF, 0xF8);
			sleep_ms(150);
		}
		if (!joypad_bits.up)
		{
			/* select the previous rom */
			mk_ili9225_text(filename[selected], 0, selected * 8, 0xFF, 0x00);
			if (selected == 0)
			{
				selected = num_file - 1;
			}
			else
			{
				selected--;
			}
			mk_ili9225_text(filename[selected], 0, selected * 8, 0xFF, 0xF8);
			sleep_ms(150);
		}
		if (!joypad_bits.right)
		{
			/* select the next page */
			num_page++;
			num_file = rom_file_selector_display_page(filename, num_page);
			if (num_file == 0)
			{
				/* no files in this page, go to the previous page */
				num_page--;
				num_file = rom_file_selector_display_page(filename, num_page);
			}
			/* select the first file */
			selected = 0;
			mk_ili9225_text(filename[selected], 0, selected * 8, 0xFF, 0xF8);
			sleep_ms(150);
		}
		if ((!joypad_bits.left) && num_page > 0)
		{
			/* select the previous page */
			num_page--;
			num_file = rom_file_selector_display_page(filename, num_page);
			/* select the first file */
			selected = 0;
			mk_ili9225_text(filename[selected], 0, selected * 8, 0xFF, 0xF8);
			sleep_ms(150);
		}
		tight_loop_contents();
	}
}

#endif

int main(void)
{
	enum gb_init_error_e ret;

	/* Overclock. */
	{
		const unsigned vco = 1596 * 1000 * 1000; /* 266MHz */
		const unsigned div1 = 6, div2 = 1;

		vreg_set_voltage(VREG_VOLTAGE_1_15);
		sleep_ms(2);
		set_sys_clock_pll(vco, div1, div2);
		sleep_ms(2);
	}

	/* Initialise USB serial connection for debugging. */
	stdio_init_all();
	// time_init();

	putstdio("INIT: ");

	printf("VGA ");
	sleep_ms(50);
	vmode = Video(DEV_VGA, RES_HVGA);
	sleep_ms(50);

#ifdef USE_PS2_KBD
	printf("PS2 KBD ");
	ps2kbd.init_gpio();
#endif

#if ENABLE_SOUND
	printf("SOUND ");
	//    gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);

#endif

	/* Start Core1, which processes requests to the LCD. */
	putstdio("CORE1 ");

	sem_init(&vga_start_semaphore, 0, 1);
	multicore_launch_core1(render_loop);
	sem_release(&vga_start_semaphore);

	while (true)
	{
		bool restart = false;

#if ENABLE_LCD
#if ENABLE_SDCARD
		/* ROM File selector */
		rom_file_selector();
#endif
#endif

		/* Initialise GB context. */
		memcpy(rom_bank0, rom, sizeof(rom_bank0));
		ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read,
					  &gb_cart_ram_write, &gb_error, NULL);
		putstdio("GB ");

		if (ret != GB_INIT_NO_ERROR)
		{
			printf("Error: %d\n", ret);
			break;
		}

		/* Automatically assign a colour palette to the game */
		char rom_title[16];
		auto_assign_palette(palette16, gb_colour_hash(&gb), gb_get_rom_name(&gb, rom_title));
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 4; j++)
				palette[i][j] = convertRGB565toRGB222(palette16[i][j]);

#if ENABLE_LCD
		gb_init_lcd(&gb, &lcd_draw_line);
		putstdio("LCD ");
#endif

#if ENABLE_SOUND
		// Initialize audio emulation
		audio_init();

		putstdio("AUDIO ");
#endif

#if ENABLE_SDCARD
		/* Load Save File. */
		read_cart_ram_file(&gb);
#endif

		putstdio("\n> ");
		uint_fast32_t frames = 0;
		uint64_t start_time = time_us_64();

		while (!restart)
		{
			int input;

#ifdef USE_PS2_KBD
			ps2kbd.tick();
#endif

			gb.gb_frame = 0;

			do
			{
				__gb_step_cpu(&gb);
				tight_loop_contents();
			} while (HEDLEY_LIKELY(gb.gb_frame == 0));

			frames++;

#if ENABLE_SOUND
			if (!gb.direct.frame_skip)
			{
				audio_callback(NULL, stream, 1098);
				// i2s_dma_write(&i2s_config, stream);
			}
#endif

			/* Update buttons state */
			prev_joypad_bits.up = gb.direct.joypad_bits.up;
			prev_joypad_bits.down = gb.direct.joypad_bits.down;
			prev_joypad_bits.left = gb.direct.joypad_bits.left;
			prev_joypad_bits.right = gb.direct.joypad_bits.right;
			prev_joypad_bits.a = gb.direct.joypad_bits.a;
			prev_joypad_bits.b = gb.direct.joypad_bits.b;
			prev_joypad_bits.select = gb.direct.joypad_bits.select;
			prev_joypad_bits.start = gb.direct.joypad_bits.start;

			gb.direct.joypad_bits.up = joypad_bits.up;
			gb.direct.joypad_bits.down = joypad_bits.down;
			gb.direct.joypad_bits.left = joypad_bits.left;
			gb.direct.joypad_bits.right = joypad_bits.right;
			gb.direct.joypad_bits.a = joypad_bits.a;
			gb.direct.joypad_bits.b = joypad_bits.b;
			gb.direct.joypad_bits.select = joypad_bits.select;
			gb.direct.joypad_bits.start = joypad_bits.start;

			/* hotkeys (select + * combo)*/
			if (!gb.direct.joypad_bits.select)
			{
#if ENABLE_SOUND
				if (!gb.direct.joypad_bits.up && prev_joypad_bits.up)
				{
					/* select + up: increase sound volume */
					// i2s_increase_volume(&i2s_config);
				}
				if (!gb.direct.joypad_bits.down && prev_joypad_bits.down)
				{
					/* select + down: decrease sound volume */
					// i2s_decrease_volume(&i2s_config);
				}
#endif
				if (!gb.direct.joypad_bits.right && prev_joypad_bits.right)
				{
					/* select + right: select the next manual color palette */
					if (manual_palette_selected < 12)
					{
						manual_palette_selected++;
						manual_assign_palette(palette16, manual_palette_selected);
						for (int i = 0; i < 3; i++)
							for (int j = 0; j < 4; j++)
								palette[i][j] = convertRGB565toRGB222(palette16[i][j]);
					}
				}
				if (!gb.direct.joypad_bits.left && prev_joypad_bits.left)
				{
					/* select + left: select the previous manual color palette */
					if (manual_palette_selected > 0)
					{
						manual_palette_selected--;
						manual_assign_palette(palette16, manual_palette_selected);
						for (int i = 0; i < 3; i++)
							for (int j = 0; j < 4; j++)
								palette[i][j] = convertRGB565toRGB222(palette16[i][j]);
					}
				}
				if (!gb.direct.joypad_bits.a && prev_joypad_bits.a)
				{
					/* select + A: enable/disable frame-skip => fast-forward */
					gb.direct.frame_skip = !gb.direct.frame_skip;
					printf("I gb.direct.frame_skip = %d\n", gb.direct.frame_skip);
				}
			}

			if (!joypad_bits.home && prev_joypad_bits.home)
			{
				/* HOME button restart */
#if ENABLE_SDCARD
				write_cart_ram_file(&gb);
#endif
				restart = true;
				// goto out;
			}

			/* Serial monitor commands */
			input = getchar_timeout_us(0);
			if (input == PICO_ERROR_TIMEOUT)
				continue;

			switch (input)
			{
#if 0
		static bool invert = false;
		static bool sleep = false;
		static uint8_t freq = 1;
		static ili9225_color_mode_e colour = ILI9225_COLOR_MODE_FULL;

		case 'i':
			invert = !invert;
			mk_ili9225_display_control(invert, colour);
			break;

		case 'f':
			freq++;
			freq &= 0x0F;
			mk_ili9225_set_drive_freq(freq);
			printf("Freq %u\n", freq);
			break;
#endif
			case 'i':
				gb.direct.interlace = !gb.direct.interlace;
				break;

			case 'f':
				gb.direct.frame_skip = !gb.direct.frame_skip;
				break;

			case 'b':
			{
				uint64_t end_time;
				uint32_t diff;
				uint32_t fps;

				end_time = time_us_64();
				diff = end_time - start_time;
				fps = ((uint64_t)frames * 1000 * 1000) / diff;
				printf("Frames: %u\n"
					   "Time: %lu us\n"
					   "FPS: %lu\n",
					   frames, diff, fps);
				stdio_flush();
				frames = 0;
				start_time = time_us_64();
				break;
			}

			case '\n':
			case '\r':
			{
				gb.direct.joypad_bits.start = 0;
				break;
			}

			case '\b':
			{
				gb.direct.joypad_bits.select = 0;
				break;
			}

			case '8':
			{
				gb.direct.joypad_bits.up = 0;
				break;
			}

			case '2':
			{
				gb.direct.joypad_bits.down = 0;
				break;
			}

			case '4':
			{
				gb.direct.joypad_bits.left = 0;
				break;
			}

			case '6':
			{
				gb.direct.joypad_bits.right = 0;
				break;
			}

			case 'z':
			case 'w':
			{
				gb.direct.joypad_bits.a = 0;
				break;
			}

			case 'x':
			{
				gb.direct.joypad_bits.b = 0;
				break;
			}

			default:
				break;
			}
		}
	out:
		puts("\nEmulation Ended");
		/* stop lcd task running on core 1 */
		// multicore_reset_core1();
	}
}