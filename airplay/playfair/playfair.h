#ifndef PLAYFAIR_H
#define PLAYFAIR_H

void playfair_decrypt(unsigned char* message3, unsigned char* cipherText, unsigned char* keyOut);
void playfair_encrypt(unsigned char* message3, unsigned char* keyIn, unsigned char* cipherText);

#endif
