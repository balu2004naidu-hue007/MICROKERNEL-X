.syntax unified
.cpu cortex-m4
.thumb

#include "port_layout.h"

.extern g_current_saved_sp
.extern ARM_ScheduleNext
.extern ARM_TaskAProgress

/*
 * Start the first task.
 * SVC_Handler restores its initial context and exception-returns
 * into Thread mode using PSP.
 */
.section .text.ARM_StartFirstTask, "ax", %progbits
.align 2
.global ARM_StartFirstTask
.type ARM_StartFirstTask, %function
.thumb_func
ARM_StartFirstTask:
    svc #0
1:
    b 1b
.size ARM_StartFirstTask, . - ARM_StartFirstTask

/*
 * TCB layout requirement: saved stack pointer is at offset zero.
 *
 * Initial task SP:
 *   R4-R11             software-saved frame
 *   R0-R3, R12, LR,
 *   PC, xPSR           hardware exception frame
 */
.section .text.SVC_Handler, "ax", %progbits
.align 2
.global SVC_Handler
.type SVC_Handler, %function
.thumb_func
SVC_Handler:
    ldr r0, =g_current_saved_sp
    ldr r0, [r0]
    ldr r0, [r0]

    ldmia r0!, {r4-r11}
    msr psp, r0

    /* Privileged Thread mode, using the Process Stack Pointer. */
    movs r0, #2
    msr CONTROL, r0
    isb

    /* Return to Thread mode and unstack the initial task frame. */
    ldr lr, =0xFFFFFFFD
    bx lr
.size SVC_Handler, . - SVC_Handler

/*
 * PendSV saves the outgoing task, asks the C scheduler for the next
 * task, restores its R4-R11 and returns through the hardware frame.
 */
.section .text.PendSV_Handler, "ax", %progbits
.align 2
.global PendSV_Handler
.type PendSV_Handler, %function
.thumb_func
PendSV_Handler:
    mrs r0, psp
    stmdb r0!, {r4-r11}

    ldr r1, =g_current_saved_sp
    ldr r1, [r1]
    str r0, [r1]

    /* Preserve EXC_RETURN and keep the handler stack 8-byte aligned. */
    push {r3, lr}
    bl ARM_ScheduleNext
    pop {r3, lr}

    ldr r1, [r0]
    ldmia r1!, {r4-r11}
    msr psp, r1
    bx lr
.size PendSV_Handler, . - PendSV_Handler

.section .note.GNU-stack, "", %progbits

.extern arm_register_test_checks
.extern arm_register_test_failures
.extern task_b_runs

.section .text.ARM_RegisterPreservationTest, "ax", %progbits
.align 2
.global ARM_RegisterPreservationTest
.type ARM_RegisterPreservationTest, %function
.thumb_func

ARM_RegisterPreservationTest:
    /* Load a unique pattern into each callee-saved register. */
    movw r4,  #0x4444
    movt r4,  #0x4444
    movw r5,  #0x5555
    movt r5,  #0x5555
    movw r6,  #0x6666
    movt r6,  #0x6666
    movw r7,  #0x7777
    movt r7,  #0x7777
    movw r8,  #0x8888
    movt r8,  #0x8888
    movw r9,  #0x9999
    movt r9,  #0x9999
    movw r10, #0xAAAA
    movt r10, #0xAAAA
    movw r11, #0xBBBB
    movt r11, #0xBBBB

1:
    /* Capture Task B's run count before waiting. */
    ldr r0, =task_b_runs
    ldr r2, [r0]

2:
    /* Wait until Task B has actually executed. */
    ldr r0, =task_b_runs
    ldr r1, [r0]
    cmp r1, r2
    beq 2b

    /* Remember Task B's latest run count. */
    mov r2, r1

    /* Record the observed context-switch count. */
    ldr r0, =arm_context_switches
    ldr r1, [r0]
    ldr r3, =arm_register_test_last_switches
    str r1, [r3]

    /* Check R4. */
    movw r0, #0x4444
    movt r0, #0x4444
    cmp r4, r0
    bne 2f

    /* Check R5. */
    movw r0, #0x5555
    movt r0, #0x5555
    cmp r5, r0
    bne 2f

    /* Check R6. */
    movw r0, #0x6666
    movt r0, #0x6666
    cmp r6, r0
    bne 2f

    /* Check R7. */
    movw r0, #0x7777
    movt r0, #0x7777
    cmp r7, r0
    bne 2f

    /* Check R8. */
    movw r0, #0x8888
    movt r0, #0x8888
    cmp r8, r0
    bne 2f

    /* Check R9. */
    movw r0, #0x9999
    movt r0, #0x9999
    cmp r9, r0
    bne 2f

    /* Check R10. */
    movw r0, #0xAAAA
    movt r0, #0xAAAA
    cmp r10, r0
    bne 2f

    /* Check R11. */
    movw r0, #0xBBBB
    movt r0, #0xBBBB
    cmp r11, r0
    bne 2f

    ldr r0, =arm_register_test_checks
    ldr r1, [r0]
    adds r1, r1, #1
    str r1, [r0]
    bl ARM_TaskAProgress
    b 1b

2:
    ldr r0, =arm_register_test_failures
    ldr r1, [r0]
    adds r1, r1, #1
    str r1, [r0]
    b 2b

.size ARM_RegisterPreservationTest, . - ARM_RegisterPreservationTest
