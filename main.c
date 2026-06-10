/*
 * Slimme Straatverlichting - AVR128DB28
 * DALI TX op PD1 via optocoupler
 *
 * States:
 *   S_IDLE   ? wacht op PIR of geluid, telt idle timeout
 *   S_ON     ? lamp aan, timer loopt, PIR verlengt timer
 *   S_ALARM  ? lamp knippert, geluid gedetecteerd
 *   S_SLEEP  ? Power-Down sleep, wacht op PIR (PD2 rising edge)
 *
 * Timings via TCB0 (20ms tick):
 *   TIMER_5SEC  = 250 ticks = 5 seconden
 *   GELUID_3SEC = 150 ticks = 3 seconden
 *   IDLE_1MIN   = 3000 ticks = 60 seconden
 *   KNIPPER_T   =  25 ticks = 0.5 seconden per halve periode
 *
 * KERNREGELS:
 *  1. DALI frames alleen bij state-overgang, nooit in een ISR of poll-loop.
 *  2. ISR's zetten alleen vlags of tellers, geen events.
 *  3. PIR ISR zet pir_flag; main verwerkt dit via verwerkPIR()
 *     zodat de ISR nooit E_TIMER kan overschrijven.
 *  4. setEvent() overschrijft nooit een bestaand event,
 *     behalve E_TIMER dat altijd de hoogste prioriteit heeft.
 */

#include "mcc_generated_files/system/system.h"
#include "dali_tx.h"
#include <avr/sleep.h>
#include <util/delay.h>

#define F_CPU 4000000UL

/* ---------- Drempelwaarden ---------- */
#define GELUID_WAARDE   230     // 8-bit ADC waarde (adc_res >> 4)
#define TIMER_5SEC      250     // 250 x 20ms = 5 seconden
#define GELUID_3SEC     150     // 150 x 20ms = 3 seconden
#define IDLE_1MIN       3000    // 3000 x 20ms = 60 seconden
#define KNIPPER_T       25      //  25 x 20ms = 0.5 seconden (halve periode)

/* ---------- ADC prescaler waarden ---------- */
#define ADC_PRESC_NORMAAL   ADC_PRESC_DIV2_gc
#define ADC_PRESC_LAAG      ADC_PRESC_DIV256_gc

/* ---------- Vluchtige variabelen (gedeeld met ISR's) ---------- */
volatile uint16_t adc_res       = 0;    // Ruwe ADC uitslag
volatile uint8_t  sendflag      = 0;    // Nieuwe ADC meting klaar
volatile uint8_t  geluidActief  = 0;    // Geluid boven drempel, timer loopt
volatile uint16_t timerTeller   = 0;    // Telt 20ms ticks voor 5s timer
volatile uint16_t idleTeller    = 0;    // Telt 20ms ticks voor idle timeout
volatile uint16_t geluidTeller  = 0;    // Telt 20ms ticks voor geluidvenster
volatile uint16_t knipperTeller = 0;    // Telt 20ms ticks voor knipper periode
volatile uint8_t  knipperen     = 0;    // 1 = lamp knippert via TCB0
volatile uint8_t  knipper_flag  = 0;    // Gezet door ISR, main doet de DALI call
volatile uint8_t  lampAan       = 0;    // Huidige lamp staat (voor knipperen)
volatile uint8_t  pir_flag      = 0;    // Gezet door PIR ISR, verwerkt in main

/* ---------- State machine ---------- */
typedef enum { S_IDLE, S_ON, S_ALARM, S_SLEEP } states;
typedef enum { E_NA, E_TIMER, E_PIR, E_GELUID, E_SLEEP } events;

states currentState = S_IDLE;
events currentEvent = E_NA;

/* ---------- Functiedeclaraties ---------- */
static void setADCPrescaler(uint8_t presc);
static void setEvent(events e);
static void resetEvent(void);
static void startTimer(void);
static void startIdleTimer(void);
static uint8_t verwerkADC(void);
static void verwerkPIR(void);
static void verwerkKnipper(void);
static void daliAan(void);
static void daliUit(void);
static void doIDLE(void);
static void doON(void);
static void doALARM(void);
static void doSLEEP(void);
void ADC0_Interrupt_handler(void);
void TCB0_InterruptHandler(void);
void PIR_interruptHandler(void);

/* ============================================================
 * DALI hulpfuncties
 * Alleen aanroepen bij state-overgangen in main, nooit in ISR.
 * ============================================================ */
static void daliAan(void)
{
    while (DALI_TX_Busy()) {}
    DALI_LampOn();
    lampAan = 1;
}

static void daliUit(void)
{
    while (DALI_TX_Busy()) {}
    DALI_LampOff();
    lampAan = 0;
}

/* ============================================================
 * setEvent
 * Overschrijft nooit een bestaand event, behalve E_TIMER
 * dat altijd de hoogste prioriteit heeft.
 * Prioriteit: E_TIMER > E_SLEEP > E_GELUID > E_PIR
 * ============================================================ */
static void setEvent(events e)
{
    if (e == E_TIMER)
    {
        currentEvent = E_TIMER;
        return;
    }
    if (currentEvent == E_NA)
        currentEvent = e;
}

static void resetEvent(void)
{
    currentEvent = E_NA;
}

/* ============================================================
 * Main
 * ============================================================ */
int main(void)
{
    SYSTEM_Initialize();
    DALI_TX_Init();

    ADC0_ConversionDoneCallbackRegister(ADC0_Interrupt_handler);
    TCB0_CaptureCallbackRegister(TCB0_InterruptHandler);
    PIR_SetInterruptHandler(PIR_interruptHandler);

    TCB0_Stop();
    TCB0.CNT = 0;

    daliUit();
    _delay_ms(50);

    TCB0_Start();
    startIdleTimer();

    while (1)
    {
        verwerkPIR();       /* pir_flag omzetten naar E_PIR event */
        verwerkKnipper();   /* knipper_flag omzetten naar DALI toggle */

        switch (currentState)
        {
            case S_IDLE:  doIDLE();  break;
            case S_ON:    doON();    break;
            case S_ALARM: doALARM(); break;
            case S_SLEEP: doSLEEP(); break;
            default: break;
        }
    }
}

/* ============================================================
 * State functies
 * ============================================================ */

/*
 * S_IDLE ? lamp uit, wacht op PIR of geluid.
 * Idle timeout loopt via TCB0 ? E_SLEEP na 60s.
 */
static void doIDLE(void)
{
    if (verwerkADC())
        setEvent(E_GELUID);

    switch (currentEvent)
    {
        case E_PIR:
            idleTeller = 0;
            if (geluidActief)
            {
                geluidActief = 0;
                geluidTeller = 0;
                knipperen    = 1;
                knipperTeller = 0;
                daliAan();
                currentState = S_ALARM;
            }
            else
            {
                daliAan();
                currentState = S_ON;
            }
            startTimer();
            resetEvent();
            break;

        case E_GELUID:
            idleTeller = 0;     /* geluid telt als activiteit */
            resetEvent();
            break;

        case E_SLEEP:
            currentState = S_SLEEP;
            resetEvent();
            break;

        default: break;
    }
}

/*
 * S_ON ? lamp aan, 5s timer loopt.
 * PIR verlengt de timer, geluid stuurt door naar S_ALARM.
 */
static void doON(void)
{
    idleTeller = 0;     /* niet slapen zolang lamp aan is */

    if (verwerkADC())
        setEvent(E_GELUID);

    switch (currentEvent)
    {
        case E_PIR:
            startTimer();
            resetEvent();
            break;

        case E_GELUID:
            geluidActief  = 0;
            geluidTeller  = 0;
            knipperen     = 1;
            knipperTeller = 0;
            currentState  = S_ALARM;
            startTimer();
            resetEvent();
            break;

        case E_TIMER:
            knipperen    = 0;
            daliUit();
            currentState = S_IDLE;
            startIdleTimer();
            resetEvent();
            break;

        default: break;
    }
}

/*
 * S_ALARM ? lamp knippert via TCB0.
 * PIR verlengt de timer.
 * Na 5s timer terug naar S_IDLE.
 */
static void doALARM(void)
{
    idleTeller = 0;     /* niet slapen zolang alarm actief is */
    verwerkADC();       /* geluidvenster bijhouden */

    switch (currentEvent)
    {
        case E_PIR:
            startTimer();
            resetEvent();
            break;

        case E_TIMER:
            knipperen    = 0;
            daliUit();
            currentState = S_IDLE;
            startIdleTimer();
            resetEvent();
            break;

        default: break;
    }
}

/*
 * S_SLEEP ? Power-Down sleep.
 * ADC vertraagd voor lager verbruik. PD2 (Fully Async) wekt de chip.
 * Na wake-up worden tellers gereset en gaat de machine naar S_IDLE.
 */
static void doSLEEP(void)
{
    knipperen = 0;
    daliUit();
    _delay_ms(20);

    setADCPrescaler(ADC_PRESC_LAAG);
    currentEvent  = E_NA;
    pir_flag      = 0;

    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    sleep_enable();
    sei();
    sleep_cpu();        /* -- chip slaapt hier -- PIR op PD2 wekt op */
    sleep_disable();

    setADCPrescaler(ADC_PRESC_NORMAAL);

    idleTeller    = 0;
    timerTeller   = 0;
    pir_flag      = 0;  /* PIR vlag wissen na wake-up */
    currentState  = S_IDLE;
    /* currentEvent bevat nu E_PIR vanuit PIR_interruptHandler,
       wordt in de volgende iteratie verwerkt via verwerkPIR() */
}

/* ============================================================
 * Hulpfuncties
 * ============================================================ */

static void setADCPrescaler(uint8_t presc)
{
    ADC0.CTRLC = (ADC0.CTRLC & ~ADC_PRESC_gm) | presc;
}

static void startTimer(void)
{
    timerTeller = 0;
    TCB0_Stop();
    TCB0.CNT = 0;
    TCB0_Start();
}

static void startIdleTimer(void)
{
    idleTeller = 0;
}

/*
 * Lees ADC resultaat als sendflag gezet is.
 * Retourneert 1 als geluid boven drempel werd gedetecteerd.
 */
static uint8_t verwerkADC(void)
{
    if (!sendflag) return 0;
    uint8_t waarde = adc_res >> 4;  /* 12-bit naar 8-bit */
    sendflag = 0;
    if (waarde > GELUID_WAARDE)
    {
        geluidActief = 1;
        geluidTeller = 0;
        return 1;
    }
    return 0;
}

/*
 * Zet pir_flag (gezet door ISR) om naar E_PIR event.
 * Atomaire lees-en-wis voorkomt race met de ISR.
 * Zo kan de PIR ISR nooit E_TIMER overschrijven.
 */
static void verwerkPIR(void)
{
    if (!pir_flag) return;
    cli();
    pir_flag = 0;
    sei();
    setEvent(E_PIR);
}

/*
 * Zet knipper_flag (gezet door ISR) om naar een DALI toggle.
 * DALI calls mogen nooit in een ISR staan vanwege de blocking wait.
 */
static void verwerkKnipper(void)
{
    if (!knipper_flag) return;
    cli();
    knipper_flag = 0;
    sei();

    if (lampAan)
        daliUit();
    else
        daliAan();
}

/* ============================================================
 * Interrupt handlers
 * Geen DALI calls, geen blocking code, alleen vlags en tellers.
 * ============================================================ */

/* ADC conversie klaar ? sla resultaat op en zet vlag */
void ADC0_Interrupt_handler(void)
{
    adc_res  = ADC0.RES;
    sendflag = 1;
}

/*
 * TCB0 ? 20ms tick.
 * Beheert alle software timers en het DALI knipper ritme.
 * E_TIMER en E_SLEEP worden direct geschreven (hoogste prioriteit).
 */
void TCB0_InterruptHandler(void)
{
    timerTeller++;

    /* Geluidvenster aftellen */
    if (geluidActief)
    {
        geluidTeller++;
        if (geluidTeller >= GELUID_3SEC)
        {
            geluidTeller = 0;
            geluidActief = 0;
        }
    }

    /* Lamp knipperen in S_ALARM ? zet alleen een vlag, main doet de DALI call */
    if (knipperen)
    {
        knipperTeller++;
        if (knipperTeller >= KNIPPER_T)
        {
            knipperTeller = 0;
            knipper_flag  = 1;  /* main loop roept verwerkKnipper() aan */
        }
    }

    /* 5-seconden timeout ? hoogste prioriteit, altijd overschrijven */
    if (timerTeller >= TIMER_5SEC)
    {
        timerTeller  = 0;
        currentEvent = E_TIMER;
    }

    /* Idle timeout ? alleen tellen in S_IDLE */
    if (currentState == S_IDLE)
    {
        idleTeller++;
        if (idleTeller >= IDLE_1MIN)
        {
            idleTeller   = 0;
            currentEvent = E_SLEEP;
        }
    }
}

/* PIR interrupt ? zet alleen een vlag, main verwerkt via verwerkPIR() */
void PIR_interruptHandler(void)
{
    pir_flag = 1;
}