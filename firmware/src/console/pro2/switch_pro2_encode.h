#ifndef _SWITCH_PRO2_ENCODE_H_
#define _SWITCH_PRO2_ENCODE_H_

#include <stdint.h>

#include "switch_pro.h"

/* Pure report-0x09 button encoder used by production and host goldens. */
void switch_pro2_encode_buttons(const switch_pro_input_t *in, uint8_t out[3]);

#endif
