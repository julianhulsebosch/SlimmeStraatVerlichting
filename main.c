#include "mcc_generated_files/system/system.h"
#include "dali_tx.h"
#include <avr/sleep.h>
#include <util/delay.h>

#define F_CPU 4000000UL

#define GELUID_DREMPEL_WAARDE   230
#define TIMER_5SEC      250     /* 250 x 20ms = 5 seconden   */
#define GELUID_3SEC     150     /* 150 x 20ms = 3 seconden   */
#define IDLE_1MIN       3000    /* 3000 x 20ms = 60 seconden */
#define KNIPPER_T       25      /*  25 x 20ms = 0.5 seconden */

#define ADC_PRESC_NORMAAL   ADC_PRESC_DIV2_gc
#define ADC_PRESC_LAAG      ADC_PRESC_DIV256_gc

typedef enum { S_IDLE, S_ON, S_ALARM, S_SLEEP }           states;
typedef enum { E_NA, E_TIMER, E_PIR, E_GELUID, E_SLEEP }  events;
typedef enum { F_ENTRY, F_ACTIVITY, F_EXIT }               flow;

/* volatile omdat de TCB0 ISR currentState leest en currentEvent schrijft */
volatile states currentState = S_IDLE;
volatile events currentEvent = E_NA;

states nextState   = S_IDLE;
flow   currentFlow = F_ENTRY;

volatile uint16_t adc_res       = 0;
volatile uint8_t  sendflag      = 0;
volatile uint8_t  geluidActief  = 0;
volatile uint16_t timerTeller   = 0;
volatile uint16_t idleTeller    = 0;
volatile uint16_t geluidTeller  = 0;
volatile uint16_t knipperTeller = 0;
volatile uint8_t  knipperen     = 0;
volatile uint8_t  knipper_flag  = 0;
volatile uint8_t  lampAan       = 0;
volatile uint8_t  pir_flag      = 0;

static void setADCPrescaler(uint8_t presc);
static void setEvent(events e);
static void resetEvent(void);
static void startTimer(void);
static void verlengTimer(void);
static uint8_t verwerkADC(void);
static void verwerkPIR(void);
static void daliAan(void);
static void daliUit(void);

static void entry_IDLE(void);    static void activity_IDLE(void);    static void exit_IDLE(void);
static void entry_ON(void);      static void activity_ON(void);      static void exit_ON(void);
static void entry_ALARM(void);   static void activity_ALARM(void);   static void exit_ALARM(void);
static void entry_SLEEP(void);   static void activity_SLEEP(void);   static void exit_SLEEP(void);

void ADC0_Interrupt_handler(void);
void TCB0_InterruptHandler(void);
void PIR_interruptHandler(void);

/* ?? DALI ??????????????????????????????????????????????????? */

static void daliAan(void)
{
    DALI_LampOn();
    lampAan = 1;
}

static void daliUit(void)
{
    DALI_LampOff();
    lampAan = 0;
}

/* ?? Events ????????????????????????????????????????????????? */

/* E_TIMER heeft altijd voorrang; andere events alleen als slot leeg is */
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

/* Verlengt de 5s timer en sluit het bijbehorende event af.
   Gebruikt bij E_PIR in S_ON en S_ALARM.                   */
static void verlengTimer(void)
{
    startTimer();
    resetEvent();
}

/* ?? Main ??????????????????????????????????????????????????? */

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
    idleTeller = 0;

    while (1)
    {
        verwerkPIR();

        switch (currentState)
        {
            case S_IDLE:
                switch (currentEvent)
                {
                    case E_PIR:
                        if (geluidActief)
                        {
                            nextState = S_ALARM;
                        }
                        else
                        {
                            nextState = S_ON;
                        }
                        resetEvent();
                        break;

                    case E_GELUID:
                        resetEvent();
                        break;

                    case E_SLEEP:
                        nextState = S_SLEEP;
                        resetEvent();
                        break;

                    default: break;
                }
                switch (currentFlow)
                {
                    case F_ENTRY:
                        entry_IDLE();
                        currentFlow = F_ACTIVITY;
                    case F_ACTIVITY:
                        activity_IDLE();
                        if (nextState == currentState) break;
                        currentFlow = F_EXIT;
                    case F_EXIT:
                        exit_IDLE();
                        currentFlow = F_ENTRY;
                        break;
                }
                break;

            case S_ON:
                 switch (currentEvent)
                {
                    case E_PIR:
                        verlengTimer();
                        break;

                    case E_GELUID:
                        nextState = S_ALARM;
                        resetEvent();
                        break;

                    case E_TIMER:
                        nextState = S_IDLE;
                        resetEvent();
                        break;

                    default: break;
                }
                switch (currentFlow)
                {
                    case F_ENTRY:
                        entry_ON();
                        currentFlow = F_ACTIVITY;
                    case F_ACTIVITY:
                        activity_ON();
                        if (nextState == currentState) break;
                        currentFlow = F_EXIT;
                    case F_EXIT:
                        exit_ON();
                        currentFlow = F_ENTRY;
                        break;
                }
                break;

            case S_ALARM:
                switch (currentEvent)
                {
                    case E_PIR:
                        verlengTimer();
                        break;

                    case E_TIMER:
                        nextState = S_IDLE;
                        resetEvent();
                        break;

                    default:
                        break;
                }
                switch (currentFlow)
                {
                    case F_ENTRY:
                        entry_ALARM();
                        currentFlow = F_ACTIVITY;
                    case F_ACTIVITY:
                        activity_ALARM();
                        if (nextState == currentState) break;
                        currentFlow = F_EXIT;
                    case F_EXIT:
                        exit_ALARM();
                        currentFlow = F_ENTRY;
                        break;
                }
                break;

            case S_SLEEP:
                switch (currentFlow)
                {
                    case F_ENTRY:
                        entry_SLEEP();
                        currentFlow = F_ACTIVITY;
                    case F_ACTIVITY:
                        activity_SLEEP();
                        if (nextState == currentState) break;
                        currentFlow = F_EXIT;
                    case F_EXIT:
                        exit_SLEEP();
                        currentFlow = F_ENTRY;
                        break;
                }
                break;

            default: break;
        }

        currentState = nextState;
    }
}

/* ?? S_IDLE ????????????????????????????????????????????????? */

static void entry_IDLE(void)
{
    nextState = S_IDLE;
    idleTeller = 0;
}

static void activity_IDLE(void)
{
    LED_SetHigh();
    if (verwerkADC())
        setEvent(E_GELUID);
}

static void exit_IDLE(void)
{
    /* lamp staat al uit */
}

/* ?? S_ON ??????????????????????????????????????????????????? */

static void entry_ON(void)
{
    idleTeller = 0;
    nextState = S_ON;
    daliAan();
    startTimer();
}

static void activity_ON(void)
{
    if (verwerkADC())
        setEvent(E_GELUID);

   
}

static void exit_ON(void)
{
    LED_SetLow();
    if (nextState == S_IDLE)
        daliUit();
    /* naar S_ALARM: lamp blijft aan */
}

/* ?? S_ALARM ???????????????????????????????????????????????? */

static void entry_ALARM(void)
{
    nextState = S_ALARM;
    
    idleTeller = 0;
    geluidActief  = 0;
    geluidTeller  = 0;
    knipperen     = 1;
    knipperTeller = 0;
    knipper_flag  = 0;

    startTimer();

    if (!lampAan)
        daliAan();
}

static void activity_ALARM(void)
{
    verwerkADC();

    if (knipper_flag)
    {
        knipper_flag = 0;

        if (lampAan)
            daliUit();
        else
            daliAan();
    }
}

static void exit_ALARM(void)
{
    LED_SetLow();
    knipperen    = 0;
    knipper_flag = 0;

    daliUit();
}

/* ?? S_SLEEP ???????????????????????????????????????????????? */

static void entry_SLEEP(void)
{
    nextState = S_SLEEP;
    daliUit();
    _delay_ms(20);
    setADCPrescaler(ADC_PRESC_LAAG);
}

static void activity_SLEEP(void)
{
    pir_flag     = 0;

    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    sleep_enable();

    /* interrupt-flag wissen vóór sleep zodat een al-hoge PIR-lijn
       de chip meteen wekt zonder dat de volgende beweging gemist wordt */
    PORTD.INTFLAGS = PIN2_bm;

    sei();
    sleep_cpu();        /* chip slaapt hier ? PIR op PD2 wekt op */
    sleep_disable();

    /* na wake-up nogmaals wissen; PIR-lijn kan nog steeds hoog staan */
    PORTD.INTFLAGS = PIN2_bm;

    nextState = S_IDLE;
}

static void exit_SLEEP(void)
{
    setADCPrescaler(ADC_PRESC_NORMAAL);
    idleTeller  = 0;
    timerTeller = 0;
    pir_flag    = 0;
}

/* ?? Hulpfuncties ??????????????????????????????????????????? */

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

/* leest ADC-resultaat als sendflag gezet is;
   geeft 1 terug als geluid boven drempel werd gedetecteerd */
static uint8_t verwerkADC(void)
{
    if (!sendflag) return 0;
    uint8_t waarde = adc_res >> 4;  /* 12-bit naar 8-bit */
    sendflag = 0;
    if (waarde > GELUID_DREMPEL_WAARDE)
    {
        geluidActief = 1;
        geluidTeller = 0;
        return 1;
    }
    return 0;
}

/* zet pir_flag atomair om naar E_PIR zodat de PIR ISR nooit
   een bestaand event kan overschrijven                       */
static void verwerkPIR(void)
{
    if (!pir_flag) return;
    cli();
    pir_flag = 0;
    sei();
    setEvent(E_PIR);
}

/* ?? Interrupt handlers ????????????????????????????????????? */

void ADC0_Interrupt_handler(void)
{
    adc_res  = ADC0.RES;
    sendflag = 1;
}

void TCB0_InterruptHandler(void)
{
    timerTeller++;

    if (geluidActief)
    {
        geluidTeller++;
        if (geluidTeller >= GELUID_3SEC)
        {
            geluidTeller = 0;
            geluidActief = 0;
        }
    }

    if (currentState == S_ALARM && knipperen)
    {
        knipperTeller++;
        if (knipperTeller >= KNIPPER_T)
        {
            knipperTeller = 0;
            knipper_flag  = 1;
        }
    }

    /* E_TIMER heeft altijd voorrang ? direct schrijven, niet via setEvent() */
    if (timerTeller >= TIMER_5SEC)
    {
        timerTeller  = 0;
        currentEvent = E_TIMER;
    }

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

void PIR_interruptHandler(void)
{
    pir_flag = 1;
    LED_SetHigh();
}