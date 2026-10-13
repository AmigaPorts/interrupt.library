/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

// Preserve the PC backend's shared IRQ numbering and the GIC API's reserved IDs.
#define INTERRUPT_SHARED_FIRST 32u
#define INTERRUPT_IRQ_LIMIT 1020u

