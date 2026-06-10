#ifndef DALI_TX_H
#define DALI_TX_H

#include <stdint.h>
#include <stdbool.h>

// Pin definitie ? AVR128DB28, PD1
#define DALI_TX_PIN          PIN1_bm
#define DALI_TX_PORT         PORTD

// DALI commando's
#define DALI_CMD_OFF         0x00
#define DALI_CMD_ON          0x05

// Broadcast adressen
#define DALI_BROADCAST_ADDR  0xFE   // broadcast DAPC
#define DALI_BROADCAST_CMD   0xFF   // broadcast command

// Basis functies
void DALI_TX_Init(void);
void DALI_TX_SendFrame(uint8_t address, uint8_t command);
bool DALI_TX_Busy(void);

// Aan / Uit
void DALI_LampOn(void);
void DALI_LampOff(void);

// Dimmen 0-100%
void DALI_SetLevel(uint8_t percent);

// Scene activeren (0..15)
void DALI_GotoScene(uint8_t scene);

// Individuele lamp aansturen (adres 0..63)
void DALI_SetLampLevel(uint8_t dali_addr, uint8_t percent);

// Groep aansturen (groep 0..15)
void DALI_SetGroupLevel(uint8_t group, uint8_t percent);

#endif