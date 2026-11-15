/**
 * @file test_key_timing.c
 * @brief Verify KEY1 events while foreground drawing is blocked.
 */
#include <stdint.h>
#include <stdio.h>
#include "main.h"
#include "key_port.h"

void mock_reset(void);
static unsigned failures = 0U;
static void check(int condition, const char *message)
{
    printf("%s %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) failures++;
}
static void advance(uint32_t milliseconds)
{
    for (uint32_t i = 0; i < milliseconds; ++i) {
        KEY_OnSysTick1ms(); /* Display/main loop does not run. */
    }
}
static void key_down(void) { mock_gpiob.IDR &= ~LL_GPIO_PIN_12; }
static void key_up(void) { mock_gpiob.IDR |= LL_GPIO_PIN_12; }

int main(void)
{
    mock_reset();
    key_up();
    KEY_Init();
    advance(50U);
    check(KEY_GetEvent() == KEY_EVENT_NONE, "no event after initialization");
    key_down();
    advance(1350U);
    check(KEY_GetEvent() == KEY_EVENT_LONG, "long press detected during 1350ms blocked main");
    check(KEY_GetLongCount() == 1U, "single long-press callback");
    check(KEY_GetEvent() == KEY_EVENT_NONE, "long event consumed only once");

    key_up();
    advance(450U);
    check(KEY_GetEvent() == KEY_EVENT_NONE, "release after long does not emit click");
    key_down();
    advance(120U);
    key_up();
    advance(450U);
    check(KEY_GetEvent() == KEY_EVENT_SHORT, "short click detected after long hold");
    check(KEY_GetClickCount() == 1U, "one short click counted");
    check(KEY_GetEvent() == KEY_EVENT_NONE, "short event consumed once");

    /* Verify pending LONG cannot overwrite a queued SHORT while LCD draws. */
    key_down(); advance(120U); key_up(); advance(450U);
    key_down(); advance(1350U);
    check(KEY_GetEvent() == KEY_EVENT_LONG, "LONG prioritized with both bits pending");
    check(KEY_GetEvent() == KEY_EVENT_SHORT, "SHORT retained after LONG");
    key_up(); advance(450U);
    KEY_Task(); /* LED hardware feedback runs only in the foreground. */
    printf("test_key_timing: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}
