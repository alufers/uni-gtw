#pragma once

#include "radio_ops.h"

/**
 * @brief Return the CC1101 radio_ops_t implementation.
 *
 * The ops struct uses GPIO_INTR_NEGEDGE for irq_edge (GDO0 falls at
 * end-of-packet).  Pass the returned pointer to radio.c for use as
 * s_ops.
 */
const radio_ops_t *cc1101_get_ops(void);
