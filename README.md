<div align="center">

# MICROKERNEL-X

**An experimental C microkernel / RTOS-style kernel with an ARM Cortex-M4 port**

Task scheduling · Context switching · Mutexes · IPC · Stack protection · Watchdog recovery

<img src="docs/assets/dashboard-preview.svg" alt="MICROKERNEL-X real host execution dashboard preview" width="100%">

**Dashboard preview generated from a real host execution of the C kernel.**

[Open the interactive dashboard](https://balu2004naidu-hue007.github.io/MICROKERNEL-X/)

</div>

---

**Status:** Laptop-validated release candidate. Host regression tests and the ARM/QEMU runtime suite passed in the local development environment. Physical Cortex-M hardware validation and the first remote GitHub Actions run remain outstanding. This project is experimental and is not production-ready firmware.

## See the kernel output

The repository includes a visual dashboard snapshot with event distribution, task activity and a filterable event timeline. It reflects actual records emitted by the C kernel during a host regression run; it is not a fabricated animation or live hardware telemetry. The kernel trace ring retains up to 64 recent events.

Enable the interactive dashboard through **Settings → Pages → Build and deployment → GitHub Actions**. You can also open `docs/index.html` locally.

In the complete local source checkout, regenerate the dashboard from a fresh run:

```bash
make dashboard
```

## Main capabilities

- **Scheduling:** task lifecycle, priority-aware selection, time slicing, preemption, delays and idle-task support.
- **Context protection:** initial and saved stack-frame validation, stack bounds and guards, high-water measurement and warnings.
- **Recovery:** watchdog progress tracking, bounded restart attempts, resource cleanup and task isolation; recovery success requires demonstrated progress.
- **Synchronization:** mutex ownership, circular deadlock detection, priority inheritance, waiter handoff and fault cleanup.
- **IPC:** fixed-size message queue, receiver registration, wake-up, timeout and recovery cleanup.
- **Memory:** fixed-block bitmap pool supporting up to 32 blocks, invalid-free/double-free detection and optional critical-section callbacks.
- **ARM Cortex-M4 port:** startup, SVC, PendSV, SysTick, R4–R11 preservation and QEMU fault-injection tests.

## Build and run

In the complete project source checkout:

```bash
make test-host
make dashboard
make all
make test-arm-qemu
```

Requirements: GCC, GNU Make and Python 3 for host/dashboard; `arm-none-eabi-gcc`, `arm-none-eabi-objcopy`, `qemu-system-arm` and `gdb-multiarch` for ARM/QEMU validation.

## Validation record

The November 9, 2026 local validation reported:
- Host executable exit code 0; memory-pool regression checks passed.
- ARM memory-pool smoke test: 13 checks, 0 failures.
- ARM stack high-water: 2 checks, 0 failures; stack warnings: 3 checks, 0 failures.
- ARM context restart: 2 checks, 0 failures; scheduler recovery: 4 checks, 0 failures.
- Register preservation: 1,000 checks, 0 failures.
- ARM runtime: 2,002 context switches; SysTick reschedule requests and normal ticks: 2,002 each.
- Time-slice progress/expiry checks: 2,002 each, with 0 timing failures.
- Stack guard and saved PC/xPSR corruption detected; invalid saved-stack-pointer task isolated.

These are local build and QEMU results, not physical hardware certification or a universal performance claim.

## Known limitations

- Physical STM32L476/Cortex-M hardware validation has not been performed.
- SysTick timing assumes the demo's stated reset-clock configuration; the actual board clock must be verified.
- The host critical-section implementation assumes a single-threaded host runtime.
- The trace ring is bounded, not a persistent logging backend.
- More interrupt-sequence, stress and concurrent-resource testing is required.
- The GitHub Actions workflow has not yet had its first verified remote run.

MIT License. See [LICENSE](LICENSE).

## Author

Balaji Naidu
