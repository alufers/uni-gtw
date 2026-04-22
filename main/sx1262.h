#pragma once

#include "radio_ops.h"

/**
 * @brief Return the SX1262 radio_ops_t implementation (FSK/2-FSK mode).
 *
 * The ops struct uses GPIO_INTR_POSEDGE for irq_edge (DIO1 goes high when
 * an IRQ fires).
 */
const radio_ops_t *sx1262_get_ops(void);
