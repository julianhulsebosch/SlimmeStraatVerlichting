#include "dali_tx.h"
#include "mcc_generated_files/timer/tca0.h"
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

/* 4 MHz, DIV1 -> 1 tick = 0.25 µs
   Half-bit = 416.7 µs -> 1666 ticks
   Volledig frame = 35 half-bits = ~14.6ms
   Stop conditie tussen twee frames: minimaal 22 half-bits = ~9.2ms
   Wij wachten 14ms om ruim binnen spec te blijven               */
#define HALF_BIT_TICKS   1666u
#define STOP_MS            14    /* wachttijd tussen frame 1 en frame 2 */

#define DALI_LINE_HIGH()  (DALI_TX_PORT.OUTCLR = DALI_TX_PIN)
#define DALI_LINE_LOW()   (DALI_TX_PORT.OUTSET = DALI_TX_PIN)

#define TIMER_START() (TCA0.SINGLE.CTRLA = TCA_SINGLE_CLKSEL_DIV1_gc | TCA_SINGLE_ENABLE_bm)
#define TIMER_STOP()  (TCA0.SINGLE.CTRLA &= ~TCA_SINGLE_ENABLE_bm)

typedef enum { DALI_IDLE, DALI_SENDING } dali_state_t;

static volatile dali_state_t dali_state  = DALI_IDLE;
static volatile uint32_t     dali_frame  = 0;
static volatile int8_t       current_bit = 0;
static volatile bool         first_half  = true;

static uint32_t BuildFrame(uint8_t addr, uint8_t cmd)
{
    uint32_t frame = 0;
    frame |= (uint32_t)1    << 16;
    frame |= (uint32_t)addr <<  8;
    frame |= (uint32_t)cmd;
    return frame;
}

static void DALI_TCA0_Callback(void)
{
    if (dali_state != DALI_SENDING)
    {
        TIMER_STOP();
        return;
    }

    uint8_t bit_val = (dali_frame >> current_bit) & 0x01;

    if (first_half)
    {
        if (bit_val) DALI_LINE_HIGH();
        else         DALI_LINE_LOW();
        first_half = false;
    }
    else
    {
        if (current_bit == 0)
        {
            DALI_LINE_HIGH();   /* lijn terug naar idle hoog */
            TIMER_STOP();
            dali_state = DALI_IDLE;
            return;
        }
        current_bit--;
        first_half = true;
        bit_val = (dali_frame >> current_bit) & 0x01;
        if (bit_val) DALI_LINE_LOW();
        else         DALI_LINE_HIGH();
    }
}

void DALI_TX_Init(void)
{
    TCA0.SINGLE.CTRLA   = 0;
    TCA0.SINGLE.CTRLB   = TCA_SINGLE_WGMODE_NORMAL_gc;
    TCA0.SINGLE.CTRLC   = 0;
    TCA0.SINGLE.CTRLD   = 0;
    TCA0.SINGLE.INTCTRL = TCA_SINGLE_OVF_bm;
    TCA0.SINGLE.PER     = HALF_BIT_TICKS - 1;
    TCA0.SINGLE.CNT     = 0;

    TCA0_OverflowCallbackRegister(DALI_TCA0_Callback);

    DALI_TX_PORT.DIRSET = DALI_TX_PIN;
    DALI_LINE_HIGH();
}

void DALI_TX_SendFrame(uint8_t address, uint8_t command)
{
    while (dali_state == DALI_SENDING) {}

    dali_frame  = BuildFrame(address, command);
    current_bit = 16;
    first_half  = true;
    dali_state  = DALI_SENDING;

    DALI_LINE_LOW();
    TCA0.SINGLE.CNT = 0;
    TIMER_START();
}

bool DALI_TX_Busy(void)
{
    return (dali_state == DALI_SENDING);
}

/*
 * DALI_TX_SendCommand: stuurt een command frame twee keer.
 *
 * Volgorde:
 *  1. Stuur frame 1
 *  2. Wacht tot frame 1 volledig klaar is (busy = 0)
 *  3. Wacht STOP_MS extra zodat de lijn lang genoeg hoog staat
 *     voordat frame 2 begint (DALI spec: min 9.17ms stop conditie)
 *  4. Stuur frame 2
 *
 * De 14ms wacht na busy=0 zorgt dat de driver de stop-conditie
 * volledig ziet voordat het tweede startbit komt.
 */
static void DALI_TX_SendCommand(uint8_t address, uint8_t command)
{
    /* Frame 1 */
    DALI_TX_SendFrame(address, command);
    while (DALI_TX_Busy()) {}
    _delay_ms(STOP_MS);

    /* Frame 2 */
    DALI_TX_SendFrame(address, command);
    while (DALI_TX_Busy()) {}
    _delay_ms(STOP_MS);   /* stop-conditie na frame 2 voor goede maat */
}

/* ============================================================
 * Publieke functies
 * ============================================================ */

void DALI_LampOn(void)
{
    DALI_TX_SendCommand(DALI_BROADCAST_CMD, 0x05);  /* RECALL MAX LEVEL */
}

void DALI_LampOff(void)
{
    DALI_TX_SendCommand(DALI_BROADCAST_CMD, 0x00);  /* OFF */
}

void DALI_SetLevel(uint8_t percent)
{
    /* DAPC: directe dimwaarde, één frame voldoende */
    if (percent > 100) percent = 100;
    uint8_t level = (uint8_t)((uint16_t)percent * 254 / 100);
    DALI_TX_SendFrame(DALI_BROADCAST_ADDR, level);
    while (DALI_TX_Busy()) {}
}

void DALI_GotoScene(uint8_t scene)
{
    if (scene > 15) return;
    DALI_TX_SendCommand(DALI_BROADCAST_CMD, 0x40 + scene);
}

void DALI_SetLampLevel(uint8_t dali_addr, uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint8_t level     = (uint8_t)((uint16_t)percent * 254 / 100);
    uint8_t addr_byte = (dali_addr << 1) & 0xFE;
    DALI_TX_SendFrame(addr_byte, level);
    while (DALI_TX_Busy()) {}
}

void DALI_SetGroupLevel(uint8_t group, uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint8_t level     = (uint8_t)((uint16_t)percent * 254 / 100);
    uint8_t addr_byte = 0x80 | ((group << 1) & 0x1E);
    DALI_TX_SendFrame(addr_byte, level);
    while (DALI_TX_Busy()) {}
}