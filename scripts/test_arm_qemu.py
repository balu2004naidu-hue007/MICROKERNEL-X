#!/usr/bin/env python3
"""Run automated Cortex-M4 runtime checks under QEMU using GDB."""

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
ELF = BUILD / "microkernel_arm.elf"
BINARY = BUILD / "microkernel_arm.bin"
TIMEOUT_SECONDS = 120


def stop_process(process):
    if process is None or process.poll() is not None:
        return

    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def show_qemu_log(log):
    log.flush()
    log.seek(0)
    output = log.read().decode("utf-8", errors="replace")
    if output.strip():
        print("QEMU output:")
        print(output)


def main():
    qemu_exe = shutil.which("qemu-system-arm")
    gdb_exe = shutil.which("gdb-multiarch")

    if not qemu_exe or not gdb_exe:
        print("FAIL: qemu-system-arm and gdb-multiarch are required.")
        return 1

    if not ELF.is_file():
        print("FAIL: ARM ELF is missing; run make arm-demo first.")
        return 1

    objcopy_exe = shutil.which("arm-none-eabi-objcopy")
    if not objcopy_exe:
        print("FAIL: arm-none-eabi-objcopy is required.")
        return 1

    try:
        subprocess.run(
            [objcopy_exe, "-O", "binary", str(ELF), str(BINARY)],
            cwd=str(ROOT),
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except subprocess.CalledProcessError as error:
        print("FAIL: Could not generate the ARM binary image.")
        if error.stdout:
            print(error.stdout)
        return 1

    print("Generated ARM binary:", BINARY)

    # Ask the OS for an available local TCP port.
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]

    gdb_commands = f"""
set pagination off
set confirm off
target remote 127.0.0.1:{port}

break ARM_TaskInit
break ARM_RegisterPreservationTest
break ARM_StartFirstTask

commands 2
silent
continue
end

continue

if arm_memory_pool_test_checks != 13
    printf "FAIL: memory-pool checks expected 13, got %u\\n", arm_memory_pool_test_checks
    quit 1
end
if arm_memory_pool_test_failures != 0
    printf "FAIL: memory-pool failure count is %u\\n", arm_memory_pool_test_failures
    quit 1
end
printf "PASS: memory-pool checks=%u failures=%u\\n", arm_memory_pool_test_checks, arm_memory_pool_test_failures

disable 1
continue

set $trace_mutex_lock = 0
set $trace_mutex_wait = 0
set $trace_recovery_start = 0
set $trace_recovery_failed = 0
set $trace_recovery_success = 0
set $trace_i = 0

while $trace_i < arm_trace_count
    if arm_trace_buffer[$trace_i].type == 10
        set $trace_mutex_lock = $trace_mutex_lock + 1
    end
    if arm_trace_buffer[$trace_i].type == 11
        set $trace_mutex_wait = $trace_mutex_wait + 1
    end

    if arm_trace_buffer[$trace_i].type == 4
        set $trace_recovery_start = $trace_recovery_start + 1
    end

    if arm_trace_buffer[$trace_i].type == 5
        set $trace_recovery_success = $trace_recovery_success + 1
    end

    if arm_trace_buffer[$trace_i].type == 6
        set $trace_recovery_failed = $trace_recovery_failed + 1
    end

    set $trace_i = $trace_i + 1
end

if $trace_mutex_lock == 0
    printf "FAIL: no ARM mutex-lock events recorded\\n"
    quit 1
end

if $trace_mutex_wait == 0
    printf "FAIL: no ARM mutex-wait events recorded\\n"
    quit 1
end

printf "PASS: ARM mutex-lock events=%u mutex-wait events=%u\\n", $trace_mutex_lock, $trace_mutex_wait

if $trace_recovery_start == 0
    printf "FAIL: no ARM recovery-start events recorded\\n"
    quit 1
end

printf "PASS: ARM recovery-start events=%u\\n", $trace_recovery_start

if $trace_recovery_failed == 0
    printf "FAIL: no ARM recovery-failure events recorded\\n"
    quit 1
end

printf "PASS: ARM recovery-failure events=%u\\n", $trace_recovery_failed

if $trace_recovery_success == 0
    printf "FAIL: no ARM recovery-success events recorded\\n"
    quit 1
end

printf "PASS: ARM recovery-success events=%u\\n", $trace_recovery_success

disable 3

if arm_stack_high_water_test_checks != 2
    printf "FAIL: expected 2 stack high-water checks, got %u\\n", arm_stack_high_water_test_checks
    quit 1
end
if arm_stack_high_water_test_failures != 0
    printf "FAIL: stack high-water failures=%u\\n", arm_stack_high_water_test_failures
    quit 1
end
printf "PASS: ARM stack high-water checks=%u failures=%u\\n", arm_stack_high_water_test_checks, arm_stack_high_water_test_failures
if arm_stack_warning_test_checks != 3
    printf "FAIL: expected 3 stack-warning checks, got %u\\n", arm_stack_warning_test_checks
    quit 1
end
if arm_stack_warning_test_failures != 0
    printf "FAIL: stack-warning failures=%u\\n", arm_stack_warning_test_failures
    quit 1
end
printf "PASS: ARM stack-warning checks=%u failures=%u\\n", arm_stack_warning_test_checks, arm_stack_warning_test_failures

if arm_context_restart_test_checks != 2
    printf "FAIL: expected 2 context-restart checks, got %u\\n", arm_context_restart_test_checks
    quit 1
end
if arm_context_restart_test_failures != 0
    printf "FAIL: context-restart failures=%u\\n", arm_context_restart_test_failures
    quit 1
end
printf "PASS: ARM context-restart checks=%u failures=%u\\n", arm_context_restart_test_checks, arm_context_restart_test_failures

if arm_scheduler_recovery_test_checks != 4
    printf "FAIL: expected 4 scheduler-recovery checks, got %u\\n", arm_scheduler_recovery_test_checks
    quit 1
end
if arm_scheduler_recovery_test_failures != 0
    printf "FAIL: scheduler-recovery failures=%u\\n", arm_scheduler_recovery_test_failures
    quit 1
end
printf "PASS: ARM scheduler-recovery checks=%u failures=%u\\n", arm_scheduler_recovery_test_checks, arm_scheduler_recovery_test_failures


break arm/context_switch.S:189
ignore 4 1000
break arm/context_switch.S:196
continue

if arm_register_test_failures != 0
    printf "FAIL: register-preservation failures=%u\\n", arm_register_test_failures
    quit 1
end
if arm_register_test_checks < 1000
    printf "FAIL: only %u register checks completed\\n", arm_register_test_checks
    quit 1
end
if arm_context_switches == 0 || arm_register_test_last_switches == 0
    printf "FAIL: no context-switch activity was recorded\\n"
    quit 1
end

printf "PASS: register checks=%u failures=%u\\n", arm_register_test_checks, arm_register_test_failures
printf "PASS: context switches=%u last recorded=%u\\n", arm_context_switches, arm_register_test_last_switches

printf "ARM trace events retained=%u write index=%u\\n", arm_trace_count, arm_trace_write_index

if arm_trace_count == 0
    printf "FAIL: ARM trace buffer is empty\\n"
    quit 1
end

set $trace_i = 0
set $trace_scheduled = 0
set $trace_context_switch = 0

while $trace_i < arm_trace_count
    if arm_trace_buffer[$trace_i].type == 0
        set $trace_scheduled = $trace_scheduled + 1
    end
    if arm_trace_buffer[$trace_i].type == 1
        set $trace_context_switch = $trace_context_switch + 1
    end
    set $trace_i = $trace_i + 1
end

printf "PASS: trace scheduling events=%u context-switch events=%u\\n", $trace_scheduled, $trace_context_switch

if $trace_scheduled == 0
    printf "FAIL: no task-scheduling events found in trace buffer\\n"
    quit 1
end

if $trace_context_switch == 0
    printf "FAIL: no context-switch events found in trace buffer\\n"
    quit 1
end

if arm_systick_reschedule_requests == 0
    printf "FAIL: SysTick never requested rescheduling\\n"
    quit 1
end

if arm_systick_no_reschedule_ticks == 0
    printf "FAIL: no normal SysTick ticks were recorded\\n"
    quit 1
end

if arm_systick_reschedule_requests != arm_systick_no_reschedule_ticks
    printf "FAIL: unexpected balance between rescheduling and normal ticks\\n"
    quit 1
end

printf "PASS: SysTick reschedule requests=%u normal ticks=%u\\n", arm_systick_reschedule_requests, arm_systick_no_reschedule_ticks

if arm_systick_slice_progress_checks == 0
    printf "FAIL: no time-slice progress checks recorded\\n"
    quit 1
end

if arm_systick_slice_expiry_checks == 0
    printf "FAIL: no time-slice expiration checks recorded\\n"
    quit 1
end

if arm_systick_slice_timing_failures != 0
    printf "FAIL: time-slice timing failures=%u\\n", arm_systick_slice_timing_failures
    quit 1
end

printf "PASS: time-slice progress=%u expiry=%u timing failures=%u\\n", arm_systick_slice_progress_checks, arm_systick_slice_expiry_checks, arm_systick_slice_timing_failures

if arm_stack_runtime_high_water_usage <= 64
    printf "FAIL: runtime stack high-water did not exceed 64 bytes: %u\\n", arm_stack_runtime_high_water_usage
    quit 1
end
printf "PASS: ARM Task B runtime stack high-water=%u bytes\\n", arm_stack_runtime_high_water_usage

if arm_stack_runtime_warning_events != 1
    printf "FAIL: expected one runtime stack warning, got %u\\n", arm_stack_runtime_warning_events
    quit 1
end
if arm_stack_runtime_warning_task_id != 2
    printf "FAIL: runtime warning task ID expected 2, got %d\\n", arm_stack_runtime_warning_task_id
    quit 1
end
if task_b.stack_warning_reported != 1
    printf "FAIL: Task B warning latch was not set\\n"
    quit 1
end
printf "PASS: runtime stack warning events=%u task_id=%d\\n", arm_stack_runtime_warning_events, arm_stack_runtime_warning_task_id


# Disable normal register-test breakpoints before fault injection.
disable 4 5

# Verify stack-guard corruption detection on the ARM target.
set $original_guard = task_b.stack[0]
set variable task_b.stack[0] = 0
set $guard_result = (int)Task_ValidateContext(&task_b)
set variable task_b.stack[0] = $original_guard

if $guard_result != -7
    printf "FAIL: ARM stack-guard corruption was not detected\\n"
    quit 1
end
printf "PASS: ARM stack-guard corruption detected\\n"

# Verify the live saved exception frame before injecting corruption.
set $saved_frame = (unsigned int *)task_b.context.sp
set $saved_frame_result = (int)ARM_ValidateSavedContext(&task_b)

if $saved_frame_result != 0
    printf "FAIL: valid saved exception frame rejected: %d\\n", $saved_frame_result
    quit 1
end
printf "PASS: valid saved exception frame accepted\\n"

# Corrupt the saved PC (word 14 of the 16-word saved frame).
set $original_saved_pc = $saved_frame[14]
set {{unsigned int}}($saved_frame + 14) = 0
set $saved_pc_result = (int)ARM_ValidateSavedContext(&task_b)
set {{unsigned int}}($saved_frame + 14) = $original_saved_pc

if $saved_pc_result != -2
    printf "FAIL: saved PC corruption was not detected: %d\\n", $saved_pc_result
    quit 1
end
printf "PASS: saved PC corruption detected\\n"

# Clear the Thumb-state bit in the saved xPSR (word 15).
set $original_saved_xpsr = $saved_frame[15]
set {{unsigned int}}($saved_frame + 15) = $original_saved_xpsr & ~0x01000000
set $saved_xpsr_result = (int)ARM_ValidateSavedContext(&task_b)
set {{unsigned int}}($saved_frame + 15) = $original_saved_xpsr

if $saved_xpsr_result != -3
    printf "FAIL: saved xPSR corruption was not detected: %d\\n", $saved_xpsr_result
    quit 1
end
printf "PASS: saved xPSR corruption detected\\n"

# Inject an invalid saved stack pointer into task_b.
set variable task_b.context.sp = (uintptr_t)&task_b.stack[0] - 4
printf "INJECT: invalid saved stack pointer into task_b\\n"

# Stop when the ARM scheduler isolates the invalid task.
watch task_b.isolated
continue

if task_b.isolated != 1
    printf "FAIL: invalid-stack task was not isolated\\n"
    quit 1
end
if task_b.state != 4
    printf "FAIL: invalid-stack task was not marked FAULTED\\n"
    quit 1
end
if task_b.fault_type != 2
    printf "FAIL: invalid-stack fault type was not recorded\\n"
    quit 1
end
printf "PASS: invalid-stack task was isolated and marked FAULTED\\n"

# Disable the watchpoint, then wait for a subsequent scheduler entry.
disable 6
tbreak ARM_ScheduleNext
continue

set $trace_fault = 0
set $trace_isolated = 0
set $trace_i = 0

while $trace_i < arm_trace_count
    if arm_trace_buffer[$trace_i].type == 3
        set $trace_fault = $trace_fault + 1
    end
    if arm_trace_buffer[$trace_i].type == 7
        set $trace_isolated = $trace_isolated + 1
    end
    set $trace_i = $trace_i + 1
end

if $trace_fault == 0
    printf "FAIL: no TASK_FAULT event recorded after injection\\n"
    quit 1
end

if $trace_isolated == 0
    printf "FAIL: no TASK_ISOLATED event recorded after injection\\n"
    quit 1
end

printf "PASS: fault trace events=%u isolation trace events=%u\\n", $trace_fault, $trace_isolated

if g_current_tcb != &task_a
    printf "FAIL: scheduler did not return to task_a\\n"
    quit 1
end
if g_current_saved_sp != &task_a.context.sp
    printf "FAIL: current saved-SP pointer is incorrect\\n"
    quit 1
end
if task_a.isolated != 0
    printf "FAIL: valid task_a was unexpectedly isolated\\n"
    quit 1
end

printf "PASS: valid task_a remains selected after task_b isolation\\n"
detach
quit 0
"""
    qemu_log = tempfile.TemporaryFile()
    gdb_script_path = None
    qemu = None

    try:
        qemu = subprocess.Popen(
            [
                qemu_exe,
                "-M", "b-l475e-iot01a",
                "-nographic",
                "-kernel", str(BINARY),
                "-S",
                "-gdb", f"tcp:127.0.0.1:{port}",
            ],
            cwd=str(ROOT),
            stdout=qemu_log,
            stderr=subprocess.STDOUT,
        )

        time.sleep(1)

        if qemu.poll() is not None:
            print("FAIL: QEMU exited before the GDB test began.")
            show_qemu_log(qemu_log)
            return 1

        with tempfile.NamedTemporaryFile(
            mode="w",
            suffix=".gdb",
            encoding="utf-8",
            delete=False,
        ) as command_file:
            command_file.write(gdb_commands)
            gdb_script_path = command_file.name

        try:
            result = subprocess.run(
                [
                    gdb_exe,
                    "--batch",
                    "--quiet",
                    "-x", gdb_script_path,
                    str(ELF),
                ],
                cwd=str(ROOT),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=TIMEOUT_SECONDS,
            )
        except subprocess.TimeoutExpired as error:
            print("FAIL: ARM runtime test exceeded its time limit.")
            if error.stdout:
                output = error.stdout
                if isinstance(output, bytes):
                    output = output.decode("utf-8", errors="replace")
                print(output)
            return 1

        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")

        if result.returncode != 0:
            print(f"FAIL: GDB test exited with code {result.returncode}.")
            show_qemu_log(qemu_log)
            return 1

        print("PASS: all automated ARM runtime checks completed.")
        return 0

    except OSError as error:
        print(f"FAIL: could not start the ARM runtime test: {error}")
        return 1

    finally:
        stop_process(qemu)
        qemu_log.close()

        if gdb_script_path and os.path.exists(gdb_script_path):
            os.unlink(gdb_script_path)


if __name__ == "__main__":
    sys.exit(main())
