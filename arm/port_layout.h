#ifndef MICROKERNEL_ARM_PORT_LAYOUT_H
#define MICROKERNEL_ARM_PORT_LAYOUT_H

/* Shared C/assembly contract for the Cortex-M4 demo port. */
#define ARM_PORT_STACK_WORDS            128
#define ARM_PORT_SOFTWARE_FRAME_WORDS   8
#define ARM_PORT_HARDWARE_FRAME_WORDS    8
#define ARM_PORT_INITIAL_FRAME_WORDS    16
#define ARM_PORT_INITIAL_XPSR           0x01000000

#endif
