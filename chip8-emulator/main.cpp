#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <stack>
#include <SDL3/SDL.h>
#include <time.h>

#ifdef _DEBUG
//#define Dbg(x, ...) fprintf(stderr, "[%s:%d] " x "\n", __FILE__, __LINE__, __VA_ARGS__)
#define Dbg(x, ...) printf(x, __VA_ARGS__)
#else
#define Dbg(x, ...)
#endif

SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
SDL_Texture* texture = NULL;
SDL_Event event = { 0 };
uint8_t rgba_white[4] = { 255, 255, 255, 255 };
uint8_t rgba_black[4] = { 0, 0, 0, 255 };
uint8_t cls_requested = false;
uint8_t instruction_counter = 0;

#define SIZE_SPRITE_GROUPS 5

namespace chip8
{
	uint8_t memory[0x1000] = { 0 };		// 4kb of memory
	std::stack<uint16_t> stack;			// Max stack size = 16 levels (but we don't need to care about that? not sure)
	bool keypad[16] = { 0 };			// A 16-key hex keypad (0 to F)
	uint8_t display[32][64] = { 0 };	// 64x32 pixel display where pixels are either on (1) or off (0)

	uint8_t font[] = {
		0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
		0x20, 0x60, 0x20, 0x20, 0x70, // 1 
		0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
		0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
		0x90, 0x90, 0xF0, 0x10, 0x10, // 4
		0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
		0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
		0xF0, 0x10, 0x20, 0x40, 0x40, // 7
		0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
		0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
		0xF0, 0x90, 0xF0, 0x90, 0x90, // A
		0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
		0xF0, 0x80, 0x80, 0x80, 0xF0, // C
		0xE0, 0x90, 0x90, 0x90, 0xE0, // D
		0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
		0xF0, 0x80, 0xF0, 0x80, 0x80  // F
	};

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
		Dbg("cls @ pc=0x%x\n", chip8::reg::pc);
		
		for (int row = 0; row < 32; row++)
			for (int col = 0; col < 64; col++)
				chip8::display[row][col] = 0;
				
		
		chip8::reg::draw_flag = 1;
		cls_requested = true;
		return;
	}

	// 0x00EE - RET
	// Return from a subroutine
	void ret()
	{
		Dbg("ret \n");
		
		// Set program counter to address at top of stack, then sub 1 from stack pointer
		chip8::reg::pc = chip8::stack.top();
		chip8::stack.pop();
		//chip8::reg::sp -= 1;
	}

	// 0x1nnn - JP addr
	// Jump to location nnn
	void jp(uint16_t addr)
	{
		Dbg("jp 0x%x \n", addr);
		chip8::reg::pc = addr;
	}

	// 0x2nnn - CALL addr
	// Push PC to stack and put NNN into PC register
	void call(uint16_t addr)
	{
		chip8::stack.push(chip8::reg::pc);
		chip8::reg::pc = addr;

	}

	// 0x3xkk - SE Vx, byte
	// Skip next instruction if Vx = kk and increment PC by 2
	void skip_if_equal(uint8_t x, uint16_t kk)
	{
		if (chip8::reg::v[x] == kk)
			chip8::reg::pc += 2;
	}

	// 0x4kk - SNE Vx, byte
	// Skip next instruction if Vx != kk
	void skip_if_not_equal(uint8_t x, uint16_t kk)
	{
		if (chip8::reg::v[x] != kk)
			chip8::reg::pc += 2;
	}

	// 0x5xy0 - SE Vx, Vy
	// Skip next instruction if Vx = Vy
	void skip_if_registers_equal(uint8_t x, uint8_t y)
	{
		if (chip8::reg::v[x] == chip8::reg::v[y])
			chip8::reg::pc += 2;
	}

	// 0x6xkk - LD Vx, byte
	// Set Vx register to kk
	void set_register_to_byte(uint8_t reg, uint8_t byte)
	{
		Dbg("ld V%x, %d \n", reg, byte);
		chip8::reg::v[reg] = byte;
	}

	// 0x7xkk - ADD Vx, byte
	void add_to_register(uint8_t reg, uint8_t byte)
	{
		Dbg("add V%x, %d \n", reg, byte);
		chip8::reg::v[reg] += byte;
	}

	// 0x8xy0 - LD Vx, Vy
	// Load value of register Vy into register Vx
	void set_registerX_to_registerY(uint8_t reg_x, uint8_t reg_y)
	{
		chip8::reg::v[reg_x] = chip8::reg::v[reg_y];
	}

	// 0x8xy1 - OR Vx, Vy
	// Set Vx to Vx OR Vy
	void bitwise_or(uint8_t x, uint8_t y)
	{
		chip8::reg::v[x] = chip8::reg::v[x] | chip8::reg::v[y];
	}

	// 0x8xy2 - AND Vx, Vy
	// Set Vx = Vx AND Vy
	void bitwise_and(uint8_t x, uint8_t y)
	{
		chip8::reg::v[x] = chip8::reg::v[x] & chip8::reg::v[y];
	}

	// 0x8xy3 - XOR Vx, Vy
	// Set Vx = Vx XOR Vy
	void bitwise_xor(uint8_t x, uint8_t y)
	{
		chip8::reg::v[x] = chip8::reg::v[x] ^ chip8::reg::v[y];
	}

	// 0x8xy4 - ADD Vx, Vy
	// Set Vx = Vx + Vy, set VF = carry.
	void add_registers(uint8_t x, uint8_t y)
	{
		// Add registers together
		uint16_t result = chip8::reg::v[x] + chip8::reg::v[y];
		
		// Only the lowest 8 bits of the result are kept, and stored in Vx
		uint8_t lower8bits = (result & 0xFF);
		chip8::reg::v[x] = lower8bits;

		/*
		Need to update vF register last as vF can also be used as an operand
		e.g. ADD Vx, VF
		If we update vF before storing it in Vx, we will overwrite what's in vF and corrupt the add operation,
		as metnioned in Timendus' flags test: https://github.com/Timendus/chip8-test-suite#flags-test
		*/
		// If result is greater than 8 bits(255), VF(carry flag) set to 1, otherwise 0
		if (result > 255)
			chip8::reg::v[0xF] = 1;
		else
			chip8::reg::v[0xF] = 0;
	}

	// 0x8xy5 - SUB Vx, Vy
	//  Set Vx = Vx - Vy, set VF = 1 if no borrow occured, otherwise set 0
	void sub(uint8_t x, uint8_t y)
	{	
		uint8_t orig_x = chip8::reg::v[x];
		uint8_t orig_y = chip8::reg::v[y];

		chip8::reg::v[x] = chip8::reg::v[x] - chip8::reg::v[y];

		if (orig_x >= orig_y)
			chip8::reg::v[0xF] = 1;
		else
			chip8::reg::v[0xF] = 0;
	}

	// 0x8xy6 - SHR Vx {, Vy}
	// Set Vx = Vx SHR 1.
	void shift_right(uint8_t x)
	{
		uint8_t lsb = chip8::reg::v[x] & 0x1;
		chip8::reg::v[x] = chip8::reg::v[x] / 2;

		if (lsb == 1)
			chip8::reg::v[0xF] = 1;
		else
			chip8::reg::v[0xF] = 0;
	}

	// 0x8xy7 - SUBN Vx, Vy
	// Set Vx = Vy - Vx, set VF = NOT borrow.
	void subn(uint8_t x, uint8_t y)
	{
		uint8_t orig_x = chip8::reg::v[x];
		uint8_t orig_y = chip8::reg::v[y];

		chip8::reg::v[x] = chip8::reg::v[y] - chip8::reg::v[x];

		if (orig_y >= orig_x)
			chip8::reg::v[0xF] = 1;
		else
			chip8::reg::v[0xF] = 0;
	}

	// 0x8xyE - SHL Vx {, Vy}
	void shift_left(uint8_t x)
	{
		// If the most significant bit of Vx is 1, then VF is set to 1, otherwise to 0.
		uint8_t msb = (chip8::reg::v[x] >> 7) & 0x1;

		// Then Vx is multipled by 2
		chip8::reg::v[x] *= 2;

		if (msb == 1)
			chip8::reg::v[0xF] = 1;
		else
			chip8::reg::v[0xF] = 0;
	}

	// 0x9xy0 - SNE Vx, Vy
	// Skip next instruction if Vx != Vy
	void skip_if_registers_not_equal(uint8_t x, uint8_t y)
	{
		if (chip8::reg::v[x] != chip8::reg::v[y])
			chip8::reg::pc += 2;
	}

	// 0xAnnn - LD I, addr
	// Set I = nnn
	void ld_i(uint16_t addr)
	{
		Dbg("ld I, 0x%x \n", addr);
		chip8::reg::i = addr;
	}

	// 0xBnnn - JP V0, addr
	// Jump to location nnn + V0
	void jmp_plus_v0(uint16_t addr)
	{
		chip8::reg::pc = chip8::reg::v[0] + addr;
	}

	// 0xCxkk - RND Vx, byte
	// Set Vx = random byte AND kk
	void random_byte_and(uint8_t x, uint8_t kk)
	{
		/*
		The interpreter generates a random number from 0 to 255, which is then ANDed with the value kk. 
		The results are stored in Vx.
		*/
		int random_num = rand() % 255;
		chip8::reg::v[x] = random_num & kk;
	}

	// Dxyn - DRW Vx, Vy, nibble
	void draw(uint8_t x, uint8_t y, uint8_t bytes)
	{
		Dbg("draw V[x]=%d V[y]=%d bytes=%d\n", chip8::reg::v[x], chip8::reg::v[y], bytes);

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

	// 0xEx9E - SKP Vx
	// Skip next instruction if key with the value of Vx is pressed
	void skip_if_key_pressed(uint8_t x)
	{
		/*
		Checks the keyboard, and if the key corresponding to
		the value of Vx is currently in the down position, PC is increased by 2.
		*/
		uint8_t key = chip8::reg::v[x];
		if (chip8::keypad[key] == 1)
			chip8::reg::pc += 2;
	}

	// 0xExA1 - SKNP Vx
	// Skip next instruction if key with the value of Vx is NOT pressed
	void skip_if_key_not_pressed(uint8_t x)
	{
		uint8_t key = chip8::reg::v[x];
		if (chip8::keypad[key] != 1)
			chip8::reg::pc += 2;
	}

	// 0xFx07 - LD Vx, DT
	// Set Vx = delay timer value
	void set_reg_to_delay_timer(uint8_t reg)
	{
		chip8::reg::v[reg] = chip8::timers::delay;
	}

	// 0xFx0A - LD Vx, K
	// Wait for a key press, store the value of the key in Vx.
	void store_key_in_reg(uint8_t reg)
	{
		static int8_t key_pressed = -1;

		// Phase 1: wait for a key to be pressed
		if (key_pressed == -1)
		{
			for (int key = 0; key < 16; key++)
			{
				if (chip8::keypad[key])
				{
					key_pressed = key;
					break;
				}
			}
			// No key yet, rewind and try again next cycle
			// Rewinding program counter means we can execute the same instruction again without needing to use a blocking while() loop
			chip8::reg::pc -= 2;
			return;
		}

		// Phase 2: wait for that key to be released
		if (chip8::keypad[key_pressed])
		{
			// Still held, rewind and wait
			chip8::reg::pc -= 2;
			return;
		}

		// Key was released - store it and reset
		chip8::reg::v[reg] = key_pressed;
		key_pressed = -1;
	}

	// 0xFx15 - LD DT, Vx
	// Set delay timer = Vx
	void set_delay_timer_to_reg(uint8_t x)
	{
		chip8::timers::delay = chip8::reg::v[x];
	}

	// 0xFx18 - LD ST, Vx
	// Set sound timer = Vx
	void set_sound_timer_to_reg(uint8_t x)
	{
		chip8::timers::sound = chip8::reg::v[x];
	}

	// 0xFx1E - ADD I, Vx
	// Set I = I + Vx
	void set_i_reg_plus_i(uint8_t x)
	{
		chip8::reg::i += chip8::reg::v[x];
	}

	// 0xFx29 - LD F, Vx
	// Set I = location of sprite for digit Vx
	void set_i_to_sprite_location(uint8_t x)
	{
		/*
		Instruction stores the memory address of sprite referenced by Vx
		Fonts are loaded in the interpreter region of memory (0x000 to 0x1FF), starting at 0x050
		The size of sprite groups is 5 bytes long
		*/
		chip8::reg::i = 0x050 + (chip8::reg::v[x] * SIZE_SPRITE_GROUPS);
	}

	// 0xFx33 - LD B, Vx
	// Store BCD representation of Vx in memory locations I, I+1, and I+2.
	void store_bcd_in_memory(uint8_t x)
	{
		// Extract the hundred and store in location memory[i]
		chip8::memory[chip8::reg::i] = chip8::reg::v[x] / 100;
		// Extract the ten and store in location memory[i + 1]
		chip8::memory[chip8::reg::i + 1] = (chip8::reg::v[x] / 10) % 10;
		// Extract the one and store in location memory[i + 2]
		chip8::memory[chip8::reg::i + 2] = chip8::reg::v[x] % 10;
	}
	
	// 0xFx55 - LD [I], Vx
	// The interpreter copies the values of registers V0 through Vx into memory, starting at the address in I
	void store_registers_in_memory(uint8_t x)
	{
		// Need +1 because it's 0 *through* Vx i.e. include Vx register
		size_t sz = sizeof(uint8_t) * (x + 1);
		memcpy(&chip8::memory[chip8::reg::i], &chip8::reg::v[0], sz);
	}

	// 0xFx65 - LD Vx, [I]
	// Read values from memory starting at location I inot registers V0 through Vx
	void store_memory_in_registers(uint8_t x)
	{
		// Need +1 because it's 0 *through* Vx i.e. include Vx register
		size_t sz = sizeof(uint8_t) * (x + 1);
		memcpy(&chip8::reg::v[0], &chip8::memory[chip8::reg::i], sz);
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
	uint8_t rightmost8bits = (opcode & 0xFF);		// Lower 8 bits
	uint8_t leftmost8bits = (opcode >> 8) & 0xFF;	// Higher 8 bits
	uint16_t lower12bits = (opcode & 0x0FFF);

	// Advance program counter
	// (do this before executing instructions as some instructions will modify program counter)
	chip8::reg::pc += 2;

	Dbg("[pc] 0x%x [opcode] 0x%04x - ", chip8::reg::pc, opcode);

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
	else if (first4bits == 0x2)
	{
		// 0x2nnn
		instructions::call(lower12bits);
	}
	else if (first4bits == 0x3)
	{
		// 0x3xkk
		instructions::skip_if_equal(scnd4bits, rightmost8bits);
	}
	else if (first4bits == 0x4)
	{
		// 0x4xkk
		instructions::skip_if_not_equal(scnd4bits, rightmost8bits);
	}
	else if (first4bits == 0x5)
	{
		// 0x5xy0
		instructions::skip_if_registers_equal(scnd4bits, third4bits);
	}
	else if (first4bits == 0x6)
	{
		// 0x6xkk
		instructions::set_register_to_byte(scnd4bits, rightmost8bits);
	}
	else if (first4bits == 0x7)
	{
		// 0x7xkk
		instructions::add_to_register(scnd4bits, rightmost8bits);
	}
	else if (first4bits == 0x8)
	{
		// 0x8xy0
		if (last4bits == 0)
			instructions::set_registerX_to_registerY(scnd4bits, third4bits);
		// 0x8xy1
		else if (last4bits == 1)
			instructions::bitwise_or(scnd4bits, third4bits);
		// 0x8xy2
		else if (last4bits == 2)
			instructions::bitwise_and(scnd4bits, third4bits);
		// 0x8xy3
		else if (last4bits == 3)
			instructions::bitwise_xor(scnd4bits, third4bits);
		// 0x8xy4
		else if (last4bits == 4)
			instructions::add_registers(scnd4bits, third4bits);
		// 0x8xy5
		else if (last4bits == 5)
			instructions::sub(scnd4bits, third4bits);
		// 0x8xy6
		else if (last4bits == 6)
			instructions::shift_right(scnd4bits);
		// 0x8xy7
		else if (last4bits == 7)
			instructions::subn(scnd4bits, third4bits);
		// 0x8xyE
		else if (last4bits == 0xE)
			instructions::shift_left(scnd4bits);
	}
	else if (first4bits == 0x9)
	{
		// 0x9xy0
		instructions::skip_if_registers_not_equal(scnd4bits, third4bits);
	}
	else if (first4bits == 0xA)
	{
		// 0xAnnn
		instructions::ld_i(lower12bits);
	}
	else if (first4bits == 0xB)
	{
		// 0xBnnn
		instructions::jmp_plus_v0(lower12bits);
	}
	else if (first4bits == 0xC)
	{
		// 0xCxkk
		instructions::random_byte_and(scnd4bits, rightmost8bits);
	}
	else if (first4bits == 0xD)
	{
		instructions::draw(scnd4bits, third4bits, last4bits);
	}
	else if (first4bits == 0xE)
	{
		// 0xEx9E
		if (rightmost8bits == 0x9E)
			instructions::skip_if_key_pressed(scnd4bits);
		// 0xExA1
		else if (rightmost8bits == 0xA1)
			instructions::skip_if_key_not_pressed(scnd4bits);
	}
	else if (first4bits == 0xF)
	{
		// 0xFx07
		if (rightmost8bits == 0x07)
			instructions::set_reg_to_delay_timer(scnd4bits);
		// 0xFx0A
		else if (rightmost8bits == 0x0A)
			instructions::store_key_in_reg(scnd4bits);
		// 0xFx15
		else if (rightmost8bits == 0x15)
			instructions::set_delay_timer_to_reg(scnd4bits);
		// 0xFx18
		else if (rightmost8bits == 0x18)
			instructions::set_sound_timer_to_reg(scnd4bits);
		// 0xFx1E
		else if (rightmost8bits == 0x1E)
			instructions::set_i_reg_plus_i(scnd4bits);
		// 0xFx29
		else if (rightmost8bits == 0x29)
			instructions::set_i_to_sprite_location(scnd4bits);
		// 0xFx33
		else if (rightmost8bits == 0x33)
			instructions::store_bcd_in_memory(scnd4bits);
		// 0xFx55
		else if (rightmost8bits == 0x55)
			instructions::store_registers_in_memory(scnd4bits);
		// 0xFx65
		else if (rightmost8bits == 0x65)
			instructions::store_memory_in_registers(scnd4bits);
	}

	// At 60hz, decrement timer
	/*
	We're running ~700 instructions per second,
	so 700 instructions / 60hz  = 12 instructions
	After 12 instructions (60Hz), decrement the timers
	*/
	instruction_counter++;
	if (instruction_counter >= 12)
	{
		if (chip8::timers::delay > 0) chip8::timers::delay--;
		if (chip8::timers::sound > 0) chip8::timers::sound--;
		instruction_counter = 0;
	}

	// 

	// When sound timer at 0, product beep sound
	/*
	Printing the special character ASCII BEL(code 7) causes a sound ??
	https://stackoverflow.com/questions/4060601/make-sounds-beep-with-c
	Good platform indepent solution for a beep - otherwise could use WinAPI Beep()
	*/
	if (chip8::timers::sound == 0)
		printf("\a");

	return;
}

bool LoadROM(char* rom_path)
{	
	Dbg("* Opening rom @ %s \n", rom_path);
	
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

	Dbg("First ROM bytes: %02x %02x %02x %02x \n", chip8::memory[0x200], chip8::memory[0x201], chip8::memory[0x202], chip8::memory[0x203]);

	return true;
}

bool LoadFont()
{
	size_t font_sz = (sizeof(chip8::font) / sizeof(chip8::font[0]));
	void* ret = memcpy(&chip8::memory[0x50], &chip8::font, font_sz);
	if (ret == NULL)
		return false;

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

/*
Keyboard to CHIP8 mapping:
Keyboard:       CHIP-8:
1 2 3 4    →    1 2 3 C
Q W E R    →    4 5 6 D
A S D F    →    7 8 9 E
Z X C V    →    A 0 B F
*/
void InterpretKeypad(SDL_Event* event)
{
	// If it's keyup, keystate is unpressed. Otherwise, key is pressed
	bool keystate = false;
	if (event->type == SDL_EVENT_KEY_DOWN)
		keystate = true;
	
	switch (event->key.scancode)
	{
	case SDL_SCANCODE_1:
		chip8::keypad[1] = keystate;
		break;
	case SDL_SCANCODE_2:
		chip8::keypad[2] = keystate;
		break;
	case SDL_SCANCODE_3:
		chip8::keypad[3] = keystate;
		break;
	case SDL_SCANCODE_4:
		chip8::keypad[0xC] = keystate;
		break;
	case SDL_SCANCODE_Q:
		chip8::keypad[4] = keystate;
		break;
	case SDL_SCANCODE_W:
		chip8::keypad[5] = keystate;
		break;
	case SDL_SCANCODE_E:
		chip8::keypad[6] = keystate;
		break;
	case SDL_SCANCODE_R:
		chip8::keypad[0xD] = keystate;
		break;
	case SDL_SCANCODE_A:
		chip8::keypad[7] = keystate;
		break;
	case SDL_SCANCODE_S:
		chip8::keypad[8] = keystate;
		break;
	case SDL_SCANCODE_D:
		chip8::keypad[9] = keystate;
		break;
	case SDL_SCANCODE_F:
		chip8::keypad[0xE] = keystate;
		break;
	case SDL_SCANCODE_Z:
		chip8::keypad[0xA] = keystate;
		break;
	case SDL_SCANCODE_X:
		chip8::keypad[0] = keystate;
		break;
	case SDL_SCANCODE_C:
		chip8::keypad[0xB] = keystate;
		break;
	case SDL_SCANCODE_V:
		chip8::keypad[0xF] = keystate;
		break;
	default:
		break;
	}
	
	return;
}

int main(int argc, char* argv[])
{
	char* rom_path = argv[1];
	if (argc != 2) {
		printf("Need full rom path to load \n");
		return -1;
	}

	// Seed random number for 0xCxkk instruction
	srand(time(NULL));

	bool b = LoadROM(rom_path);
	if (b == false)
		return -1;

	b = LoadFont();
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

	// Gets rid of red hue when scaling up pixels that SDL creates
	SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

	while (1)
	{	
		uint64_t frame_start = SDL_GetTicks();
		
		while (SDL_PollEvent(&event) != false)
		{
			switch (event.type)
			{
			case SDL_EVENT_QUIT:
				TerminateWindow();
				return -1;
			case SDL_EVENT_KEY_DOWN:
				InterpretKeypad(&event);
				break;
			case SDL_EVENT_KEY_UP:
				InterpretKeypad(&event);
				break;
			}
		}

		DecodeInstruction();

		// If draw_flag is set, update the display
		if (chip8::reg::draw_flag)
		{
			// count how many pixels are on
			int on_count = 0;
			for (int r = 0; r < 32; r++)
				for (int c = 0; c < 64; c++)
					if (chip8::display[r][c]) on_count++;

			Dbg("rendering: %d pixels on\n", on_count);
			
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
					// Clear pixel
					else
						memcpy(current_pixel, &rgba_black, 4);
				}
			}

			SDL_UnlockTexture(texture);
			
			// Clear backbuffer that's storing old frame
			SDL_RenderClear(renderer);
			
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