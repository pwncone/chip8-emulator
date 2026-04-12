#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <stack>
#include <SDL3/SDL.h>

SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
SDL_Texture* texture = NULL;
uint8_t rgba_white[4] = { 255, 255, 255, 255 };
uint8_t rgba_black[4] = { 0, 0, 0, 255 };

namespace chip8
{
	uint8_t memory[0x1000] = { 0 };	// 4kb of memory
	std::stack<uint16_t> stack;		// Max stack size = 16 levels (but we don't need to care about that? not sure)
	bool keypad[16] = { 0 };		// A 16-key hex keypad (0 to F)
	uint8_t display[32][64] = { 0 };	// 64x32 pixel display where pixels are either on (1) or off (0)

	namespace timers
	{
		// Count down at 60hz. Buzzer beeps while sound > 0
		uint8_t sound = 0;
		uint8_t delay = 0;
	}

	namespace reg
	{
		// 1x 16-bit Program Counter (PC)
		// Points to the current instruction
		// ROMs load at 0x200 in memory, so start there
		uint16_t pc = 0x200;
		
		// 1x 16-bit index register
		// Points to memory address
		uint16_t i = 0;
		
		// 16x 8-bit general purpose registers: V0 to VF
		uint8_t v[16] = { 0 };

		uint8_t draw_flag = 0;

		// Stack pointer
		// Points to topmost level of the stack
		uint8_t sp = 0;
	}
}

namespace instructions
{
	// 0x00E0 - CLS
	// Clear screen
	void cls()
	{
		printf("cls \n");
		
		for (int i = 0; i < 32; i++)
			for (int j = 0; j < 64; j++)
				chip8::display[i][j] = 0;
		
		return;
	}

	// 0x00EE - RET
	// Return from a subroutine
	void ret()
	{
		printf("ret \n");
		
		// Set program counter to address at top of stack, then sub 1 from stack pointer
		chip8::reg::pc = chip8::stack.top();
		chip8::stack.pop();
		//chip8::reg::sp -= 1;
	}

	// 0x1nnn - JP addr
	// Jump to location nnn
	void jp(uint16_t addr)
	{
		printf("jp 0x%x \n", addr);
		chip8::reg::pc = addr;
	}

	// 0x6xkk - LD Vx, byte
	// Set Vx register to kk
	void set_register_to_byte(uint8_t reg, uint8_t byte)
	{
		printf("ld V%x, %d \n", reg, byte);
		chip8::reg::v[reg] = byte;
	}

	// 0x7xNN - ADD Vx, byte
	void add_to_register(uint8_t reg, uint8_t byte)
	{
		printf("add V%x, %d \n", reg, byte);
		chip8::reg::v[reg] += byte;
	}

	// 0xAnnn - LD I, addr
	// Set I = nnn
	void ld_i(uint16_t addr)
	{
		printf("ld I, 0x%x \n", addr);
		chip8::reg::i = addr;
	}

	// Dxyn - DRW Vx, Vy, nibble
	void draw(uint8_t x, uint8_t y, uint8_t bytes)
	{
		printf("draw V[x]=%d V[y]=%d bytes=%d\n", chip8::reg::v[x], chip8::reg::v[y], bytes);

		// Reset VF (used to track collision)
		chip8::reg::v[0xF] = 0;
		
		/*
		Imagine a series of 5 bytes: 0xF0, 0x90, 0x90, 0x90, 0xF0
		Each byte is a row (like in the diagram below)
		And in each byte you have 8 bits. These 8 bits are the pixels to turn on/off on the display (1 = on, 0 = off)
		The image below makes a 0 (a square-looking one)
		0xF0  =  11110000
		0x90  =  10010000
		0x90  =  10010000
		0x90  =  10010000
		0xF0  =  11110000
		*/
		
		// Loop through bytes (each byte is a row)
		for (int row = 0; row < bytes; row++)
		{
			uint8_t byte = chip8::memory[chip8::reg::i + row];
			
			// Loop through "columns" (the 8 bits in each byte)
			for (int col = 0; col < 8; col++)
			{
				// Grab bit from byte
				// (shift by 7 - col brings bit we care about to rightmost position, then & 1 isolates it)
				uint8_t bit = (byte >> (7 - col)) & 1;

				// Grab screen co-ordinates of where to draw sprite
				uint8_t pixel_x = (chip8::reg::v[x] + col) % 64; // wraps horizontally
				uint8_t pixel_y = (chip8::reg::v[y] + row) % 32; // wraps vertically
				
				uint8_t prev = chip8::display[pixel_y][pixel_x];
					
				// XOR bytes onto screen
				/*
				CHIP-8 sprites don't overwrite the display, they toggle pixels.
				So if 2 sprites overlap, they will simply turn off the overlapping pixel/s
				*/ 
				chip8::display[pixel_y][pixel_x] ^= bit;

				// If pixel is erased, set VF to 1 to track sprite collision
				if (prev == 1 && chip8::display[pixel_y][pixel_x] == 0)
					chip8::reg::v[0xF] = 1;
			}
		}

		// Need to tell renderer to draw pixels so set draw flag. Renderer will reset it
		chip8::reg::draw_flag = 1;

		return;
	}
}

void DecodeInstruction()
{
	// Fetch 2-byte opcode pointed to by PC
	uint16_t opcode = (chip8::memory[chip8::reg::pc] << 8) | chip8::memory[chip8::reg::pc + 1];

	uint8_t first4bits = (opcode >> 12) & 0xF;
	uint8_t scnd4bits = (opcode >> 8) & 0xF;
	uint8_t third4bits = (opcode >> 4) & 0xF;
	uint8_t last4bits = opcode & 0xF;
	uint8_t lower8bits = (opcode & 0xFF);
	uint8_t higher8bits = (opcode >> 8) & 0xFF;
	uint16_t lower12bits = (opcode & 0x0FFF);

	// Advance program counter
	// (do this before executing instructions as some instructions will modify program counter)
	chip8::reg::pc += 2;

	printf("[pc] 0x%x [opcode] 0x%04x - ", chip8::reg::pc, opcode);

	// Decode opcode & execute
	if (first4bits == 0x0)
	{
		if (opcode == 0x00E0)
			instructions::cls();
		else if (opcode == 0x00EE)
			instructions::ret();
	}
	else if (first4bits == 0x1)
	{
		// 0x1nnn
		instructions::jp(lower12bits);
	}
	else if (first4bits == 0x6)
	{
		// 0x6Xnn
		instructions::set_register_to_byte(scnd4bits, lower8bits);
	}
	else if (first4bits == 0x7)
	{
		// 0x7Xnn
		instructions::add_to_register(scnd4bits, lower8bits);
	}
	else if (first4bits == 0xA)
	{
		// 0xAnnn
		instructions::ld_i(lower12bits);
	}
	else if (first4bits == 0xD)
	{
		instructions::draw(scnd4bits, third4bits, last4bits);
	}

	// At 60hz, decrement timer

	return;
}

bool LoadROM(char* rom_path)
{	
	printf("* Opening rom @ %s \n", rom_path);
	
	FILE* f = fopen(rom_path, "rb");
	if (f == NULL) {
		fprintf(stderr, "Error opening '%s': %s \n", rom_path, strerror(errno));
		return false;
	}

	// Get from size
	fseek(f, 0, SEEK_END);
	long rom_size = ftell(f);
	rewind(f);

	// Check rom isn't too big for memory 
	/* 
	Roms load at 0x200 in memory. Max memory size is 0x1000, so
	0x1000 - 0x200 = 0xE00 = 3584 bytes of space for roms
	*/
	if (rom_size > 3584) {
		fprintf(stderr, "Rom size too big. Needs to be less than 3584 bytes (current: %ld) \n", rom_size);
		fclose(f);
		return false;
	}
	
	size_t r = fread(&chip8::memory[0x200], 1, (size_t)rom_size, f);
	if (r != rom_size) {
		if (feof(f)) {
			fprintf(stderr, "Error reading '%s': unexpected end of file \n", rom_path);
			fclose(f);
			return false;
		}
		else if (ferror(f)) {
			fprintf(stderr, "Error reading '%s': %s \n", rom_path, strerror(errno));
			fclose(f);
			return false;
		}
	}

	printf("First ROM bytes: %02x %02x %02x %02x \n", chip8::memory[0x200], chip8::memory[0x201], chip8::memory[0x202], chip8::memory[0x203]);

	return true;
}

void TerminateWindow()
{
	SDL_DestroyTexture(texture);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return;
}

int main(int argc, char* argv[])
{
	char* rom_path = argv[1];
	if (argc != 2) {
		printf("Need full rom path to load \n");
		return -1;
	}

	bool b = LoadROM(rom_path);
	if (b == false)
		return -1;

	b = SDL_Init(SDL_INIT_VIDEO);
	if (b == false) {
		SDL_Log("Couldn't initialise SDL: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	b = SDL_CreateWindowAndRenderer("chip8", 640, 320, SDL_WINDOW_RESIZABLE, &window, &renderer);
	if (b == false) {
		SDL_Log("Coudln't create window/renderer: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, 64, 32);
	if (texture == NULL) {
		SDL_Log("Coudln't create texture: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	// Gets rid of red huge when scaling up pixels that SDL creates
	SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

	while (1)
	{
		uint64_t frame_start = SDL_GetTicks();	
		
		SDL_Event event = { 0 };
		if (SDL_PollEvent(&event) != false)
		{
			switch (event.type)
			{
			case SDL_EVENT_QUIT:
				TerminateWindow();
				return -1;
			case SDL_EVENT_KEY_DOWN:
				break;
			case SDL_EVENT_KEY_UP:
				break;
			}
		}

		DecodeInstruction();
		
		// If draw_flag is set, update the display
		if (chip8::reg::draw_flag)
		{
			void* pixels = NULL;
			int pitch = 0;
			b = SDL_LockTexture(texture, NULL, &pixels, &pitch);
			if (b == false) {
				SDL_Log("Failed to lock texture: %s", SDL_GetError());
				return SDL_APP_FAILURE;
			}

			for (int row = 0; row < 32; row++)
			{
				for (int col = 0; col < 64; col++)
				{					
					// Find the current pixel in the SDL texture buffer and write our RGBA value to it
					/*
					Each pixel in the texture buffer is 4 bytes large.
					To find the next pixel in the buffer, we do: base_pixel_pointer + row + column.
					- (row * pitch) jumps to the right row
					- (col * 4) jumps to the right column (because each pixel is 4 bytes large)
					*/
					uint32_t* current_pixel = (uint32_t*)((uint8_t*)pixels + (row * pitch) + (col * 4));
					
					// If pixel is set to ON in display, write white pixel
					if (chip8::display[row][col])
						memcpy(current_pixel, &rgba_white, 4);
					else
						memcpy(current_pixel, &rgba_black, 4);
				}
			}

			SDL_UnlockTexture(texture);
			b = SDL_RenderTexture(renderer, texture, NULL, NULL);
			if (b == false) {
				SDL_Log("Failed to render texture: %s", SDL_GetError());
				return SDL_APP_FAILURE;
			}

			b = SDL_RenderPresent(renderer);
			if (b == false) {
				SDL_Log("Failed to render present: %s", SDL_GetError());
				return SDL_APP_FAILURE;
			}

			chip8::reg::draw_flag = 0;
		}

		// Cap to ~700 instructions per second (1000ms / 700 = ~1.4ms per instruction)
		uint64_t frame_time = SDL_GetTicks() - frame_start;
		if (frame_time < 1)
			SDL_Delay(1);
	}
	
	return 0;
}