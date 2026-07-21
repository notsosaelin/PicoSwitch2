/*
 * Out-of-band UART diagnostics for live console sessions.
 *
 * USB remains attached to the Switch while UART0 uses GP0/GP1 to answer a
 * deliberately tiny command set from a PC. The implementation never routes
 * stdio/printf to UART and never blocks waiting for UART FIFO space.
 */
#ifndef NS2_UART_DIAG_H
#define NS2_UART_DIAG_H

#include <stdbool.h>

void ns2_uart_diag_init(void);
void ns2_uart_diag_task(void);
/* Core0-only edge: consume one UART-requested same-personality USB reset. */
bool ns2_uart_diag_take_reenumerate_request(void);

#endif
