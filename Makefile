CC       := gcc
ARM_CC   := arm-none-eabi-gcc

BUILD_DIR := build

HOST_SOURCES := main.c task.c scheduler.c kernel_ipc.c mutex.c trace.c ipc.c memory_pool.c
ARM_SOURCES  := arm/startup.S arm/context_switch.S arm/arm_demo.c
ARM_HEADERS  := arm/port_layout.h

HOST_FLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic

ARM_FLAGS := -g -std=c11 -mcpu=cortex-m4 -mthumb \
             -ffreestanding -fno-builtin -I. \
             -ffunction-sections -fdata-sections \
             -Wall -Wextra -Wpedantic

.PHONY: all host arm-demo test-host clean memory-pool-test test-arm-qemu dashboard-demo dashboard-test

all: host arm-demo

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

host: $(BUILD_DIR)/microkernel_host.exe

$(BUILD_DIR)/microkernel_host.exe: $(HOST_SOURCES) task.h scheduler.h kernel_ipc.h mutex.h trace.h ipc.h | $(BUILD_DIR)
	$(CC) $(HOST_FLAGS) -o $@ $(HOST_SOURCES)

arm-demo: $(BUILD_DIR)/microkernel_arm.elf

ARM_OBJECTS := $(BUILD_DIR)/startup.o \
               $(BUILD_DIR)/context_switch.o \
               $(BUILD_DIR)/task.o \
               $(BUILD_DIR)/scheduler.o \
               $(BUILD_DIR)/memory_pool.o \
               $(BUILD_DIR)/memory_pool_critical.o \
               $(BUILD_DIR)/mutex.o \
               $(BUILD_DIR)/kernel_ipc.o \
               $(BUILD_DIR)/ipc.o \
               $(BUILD_DIR)/arm_demo.o \
               $(BUILD_DIR)/trace_arm.o

$(BUILD_DIR)/startup.o: arm/startup.S | $(BUILD_DIR)
	$(ARM_CC) $(ARM_FLAGS) -c $< -o $@

$(BUILD_DIR)/context_switch.o: arm/context_switch.S | $(BUILD_DIR)
	$(ARM_CC) $(ARM_FLAGS) -c $< -o $@

$(BUILD_DIR)/task.o: task.c task.h | $(BUILD_DIR)
	$(ARM_CC) $(ARM_FLAGS) -S $< -o $(BUILD_DIR)/task_generated.s
	echo '.section .note.GNU-stack,"",%progbits' >> $(BUILD_DIR)/task_generated.s
	$(ARM_CC) $(ARM_FLAGS) -c $(BUILD_DIR)/task_generated.s -o $@


$(BUILD_DIR)/scheduler.o: scheduler.c scheduler.h task.h kernel_ipc.h mutex.h trace.h | $(BUILD_DIR)
	$(ARM_CC) $(ARM_FLAGS) -DMICROKERNEL_ARM -S $< -o $(BUILD_DIR)/scheduler_generated.s
	echo '.section .note.GNU-stack,"",%progbits' >> $(BUILD_DIR)/scheduler_generated.s
	$(ARM_CC) $(ARM_FLAGS) -DMICROKERNEL_ARM -c $(BUILD_DIR)/scheduler_generated.s -o $@


$(BUILD_DIR)/mutex.o: mutex.c mutex.h scheduler.h task.h trace.h | $(BUILD_DIR)
	$(ARM_CC) $(ARM_FLAGS) -DMICROKERNEL_ARM -S $< -o $(BUILD_DIR)/mutex_generated.s
	echo '.section .note.GNU-stack,"",%progbits' >> $(BUILD_DIR)/mutex_generated.s
	$(ARM_CC) $(ARM_FLAGS) -DMICROKERNEL_ARM -c $(BUILD_DIR)/mutex_generated.s -o $@

$(BUILD_DIR)/kernel_ipc.o: kernel_ipc.c kernel_ipc.h scheduler.h task.h ipc.h | $(BUILD_DIR)
	$(ARM_CC) $(ARM_FLAGS) -DMICROKERNEL_ARM -S $< -o $(BUILD_DIR)/kernel_ipc_generated.s
	echo '.section .note.GNU-stack,"",%progbits' >> $(BUILD_DIR)/kernel_ipc_generated.s
	$(ARM_CC) $(ARM_FLAGS) -DMICROKERNEL_ARM -c $(BUILD_DIR)/kernel_ipc_generated.s -o $@

$(BUILD_DIR)/ipc.o: ipc.c ipc.h | $(BUILD_DIR)
	$(ARM_CC) $(ARM_FLAGS) -DMICROKERNEL_ARM -S $< -o $(BUILD_DIR)/ipc_generated.s
	echo '.section .note.GNU-stack,"",%progbits' >> $(BUILD_DIR)/ipc_generated.s
	$(ARM_CC) $(ARM_FLAGS) -DMICROKERNEL_ARM -c $(BUILD_DIR)/ipc_generated.s -o $@

$(BUILD_DIR)/memory_pool.o: memory_pool.c memory_pool.h | $(BUILD_DIR)
	$(ARM_CC) $(ARM_FLAGS) -S $< -o $(BUILD_DIR)/memory_pool_generated.s
	echo '.section .note.GNU-stack,"",%progbits' >> $(BUILD_DIR)/memory_pool_generated.s
	$(ARM_CC) $(ARM_FLAGS) -c $(BUILD_DIR)/memory_pool_generated.s -o $@

$(BUILD_DIR)/memory_pool_critical.o: arm/memory_pool_critical.c arm/memory_pool_critical.h memory_pool.h | $(BUILD_DIR)
	$(ARM_CC) $(ARM_FLAGS) -S $< -o $(BUILD_DIR)/memory_pool_critical_generated.s
	echo '.section .note.GNU-stack,"",%progbits' >> $(BUILD_DIR)/memory_pool_critical_generated.s
	$(ARM_CC) $(ARM_FLAGS) -c $(BUILD_DIR)/memory_pool_critical_generated.s -o $@

$(BUILD_DIR)/arm_demo.o: arm/arm_demo.c task.h scheduler.h kernel_ipc.h mutex.h memory_pool.h arm/memory_pool_critical.h arm/port_layout.h | $(BUILD_DIR)
	$(ARM_CC) $(ARM_FLAGS) -S $< -o $(BUILD_DIR)/arm_demo_generated.s
	echo '.section .note.GNU-stack,"",%progbits' >> $(BUILD_DIR)/arm_demo_generated.s
	$(ARM_CC) $(ARM_FLAGS) -c $(BUILD_DIR)/arm_demo_generated.s -o $@

$(BUILD_DIR)/trace_arm.o: arm/trace_arm.c trace.h scheduler.h task.h | $(BUILD_DIR)
	$(ARM_CC) $(ARM_FLAGS) -S $< -o $(BUILD_DIR)/trace_arm_generated.s
	echo '.section .note.GNU-stack,"",%progbits' >> $(BUILD_DIR)/trace_arm_generated.s
	$(ARM_CC) $(ARM_FLAGS) -c $(BUILD_DIR)/trace_arm_generated.s -o $@

$(BUILD_DIR)/microkernel_arm.elf: $(ARM_OBJECTS) arm/stm32l476.ld
	$(ARM_CC) $(ARM_FLAGS) -nostdlib \
	    -T arm/stm32l476.ld \
	    -Wl,--gc-sections \
	    -Wl,-Map,$(BUILD_DIR)/microkernel_arm.map \
	    -o $@ $(ARM_OBJECTS)



memory-pool-test: memory_pool_test.c memory_pool.c memory_pool.h | $(BUILD_DIR)
	$(CC) $(HOST_FLAGS) -I. -o $(BUILD_DIR)/memory_pool_test.exe memory_pool_test.c memory_pool.c
	./$(BUILD_DIR)/memory_pool_test.exe

$(BUILD_DIR)/dashboard_demo.exe: dashboard_demo.c task.c task.h scheduler.c scheduler.h \
		kernel_ipc.c kernel_ipc.h mutex.c mutex.h trace.c trace.h ipc.c ipc.h \
		memory_pool.c memory_pool.h kernel_critical.h | $(BUILD_DIR)
	$(CC) $(HOST_FLAGS) -I. -o $@ dashboard_demo.c task.c scheduler.c kernel_ipc.c mutex.c trace.c ipc.c memory_pool.c

dashboard-demo: $(BUILD_DIR)/dashboard_demo.exe
	./$(BUILD_DIR)/dashboard_demo.exe docs/demo-data.json

dashboard-test: dashboard-demo scripts/test_dashboard.py
	python scripts/test_dashboard.py

test-host: host memory-pool-test
	@./$(BUILD_DIR)/microkernel_host.exe > $(BUILD_DIR)/host_test.log 2>&1; runtime_status=$$?; echo "Host runtime exit code: $$runtime_status"; if [ $$runtime_status -ne 0 ]; then cat $(BUILD_DIR)/host_test.log; exit $$runtime_status; fi
	@if grep -qE 'ERROR|FAIL:' $(BUILD_DIR)/host_test.log; then grep -nE 'ERROR|FAIL:' $(BUILD_DIR)/host_test.log; exit 1; fi
	@grep -E "ERROR|correctly|valid|Total Context Switches|Recovery Events|Kernel Run Scheduler Activity" $(BUILD_DIR)/host_test.log

test-arm-qemu: arm-demo scripts/test_arm_qemu.py
	python scripts/test_arm_qemu.py

clean:
	rm -rf $(BUILD_DIR)
