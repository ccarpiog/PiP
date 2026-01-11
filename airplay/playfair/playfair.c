#include <stdint.h>
#include <string.h>

#include "playfair.h"

void generate_key_schedule(unsigned char* key_material, uint32_t key_schedule[11][4]);
void generate_session_key(unsigned char* oldSap, unsigned char* messageIn, unsigned char* sessionKey);
void cycle(unsigned char* block, uint32_t key_schedule[11][4]);
void z_xor(unsigned char* in, unsigned char* out, int blocks);
void x_xor(unsigned char* in, unsigned char* out, int blocks);

extern unsigned char default_sap[];

void playfair_decrypt(unsigned char* message3, unsigned char* cipherText, unsigned char* keyOut)
{
	unsigned char* chunk1 = &cipherText[16];
	unsigned char* chunk2 = &cipherText[56];
	int i;
	unsigned char blockIn[16];
	unsigned char sapKey[16];
	uint32_t key_schedule[11][4];
	generate_session_key(default_sap, message3, sapKey);
	generate_key_schedule(sapKey, key_schedule);
	z_xor(chunk2, blockIn, 1);
	cycle(blockIn, key_schedule);
	for (i = 0; i < 16; i++) {
		keyOut[i] = blockIn[i] ^ chunk1[i];
	}
	x_xor(keyOut, keyOut, 1);
	z_xor(keyOut, keyOut, 1);
}

// Forward declaration for inverse cycle
void inv_cycle(unsigned char* block, uint32_t key_schedule[11][4]);

void playfair_encrypt(unsigned char* message3, unsigned char* keyIn, unsigned char* cipherText)
{
	// Initialize cipherText to zero (we'll fill in the parts we need)
	memset(cipherText, 0, 72);

	// Set header (bytes 0-15): FPLY magic + version info
	// Based on captured data: 46 50 4c 59 01 02 01 00 00 00 00 3c 00 00 00 00
	cipherText[0] = 0x46;  // 'F'
	cipherText[1] = 0x50;  // 'P'
	cipherText[2] = 0x4c;  // 'L'
	cipherText[3] = 0x59;  // 'Y'
	cipherText[4] = 0x01;
	cipherText[5] = 0x02;
	cipherText[6] = 0x01;
	cipherText[7] = 0x00;
	cipherText[8] = 0x00;
	cipherText[9] = 0x00;
	cipherText[10] = 0x00;
	cipherText[11] = 0x3c;
	cipherText[12] = 0x00;
	cipherText[13] = 0x00;
	cipherText[14] = 0x00;
	cipherText[15] = 0x00;

	unsigned char* chunk1 = &cipherText[16];
	unsigned char* chunk2 = &cipherText[56];
	unsigned char blockIn[16];
	unsigned char blockOut[16];
	unsigned char sapKey[16];
	uint32_t key_schedule[11][4];

	// Step 1: Generate session key (same as decryption)
	generate_session_key(default_sap, (unsigned char*)message3, sapKey);
	generate_key_schedule(sapKey, key_schedule);

	// Step 2: Reverse the decryption process
	// Decryption: keyOut = (cycle(z_xor(chunk2)) XOR chunk1) XOR x_key XOR z_key
	// Encryption: We need to find chunk1 and chunk2 such that:
	//   keyIn = (cycle(z_xor(chunk2)) XOR chunk1) XOR x_key XOR z_key

	// Start with keyIn and reverse the XOR operations
	// keyIn = keyOut after x_xor and z_xor
	// So: keyOut (before x_xor/z_xor) = keyIn XOR z_key XOR x_key
	memcpy(blockOut, (unsigned char*)keyIn, 16);
	z_xor(blockOut, blockOut, 1);  // Reverse z_xor (XOR is its own inverse)
	x_xor(blockOut, blockOut, 1);  // Reverse x_xor (XOR is its own inverse)

	// Now we have: blockOut = cycle(blockIn) XOR chunk1
	// We need to generate chunk1 and blockIn such that this works
	// Strategy: Generate a random blockIn, cycle it, then compute chunk1

	// Generate a random blockIn_before_cycle (we'll use a deterministic approach for now)
	// In practice, this should be cryptographically random
	unsigned char blockIn_before_cycle[16];
	for (int i = 0; i < 16; i++) {
		blockIn_before_cycle[i] = (unsigned char)(i * 17 + 0x42);  // Simple deterministic pattern
	}

	// Copy to blockIn and cycle it (cycle modifies blockIn in place)
	memcpy(blockIn, blockIn_before_cycle, 16);
	cycle(blockIn, key_schedule);

	// Now blockIn contains cycle(blockIn_before_cycle)
	// Compute chunk1: chunk1 = cycle(blockIn_before_cycle) XOR blockOut
	for (int i = 0; i < 16; i++) {
		chunk1[i] = blockIn[i] ^ blockOut[i];
	}

	// Compute chunk2: chunk2 = z_xor(blockIn_before_cycle)
	// z_xor is: out = in XOR z_key
	// So: chunk2 = blockIn_before_cycle XOR z_key
	extern unsigned char z_key[];
	for (int i = 0; i < 16; i++) {
		chunk2[i] = blockIn_before_cycle[i] ^ z_key[i];
	}

	// Middle bytes (32-55) can be padding or derived data
	// Based on captured data, they seem to have some pattern
	// For now, we'll leave them as zero or copy from a template
	// TODO: Determine what these bytes should contain
}
