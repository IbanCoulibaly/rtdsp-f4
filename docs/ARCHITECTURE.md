# Architecture

## 1. Layers

```
┌──────────────────────────────────────────────────────────────────────────┐
│ app/        tasks, command protocol, supervisor, POST, fault capture       │  target-independent
├──────────────────────────────────────────────────────────────────────────┤
│ libs/dsp    filter_engine, FIR, biquad, fixed point     (host + target)   │  portable C11, no OS,
│ libs/link   COBS, CRC-16, frames, protocol_defs.h       (host + target)   │  no HW, unit-tested
├──────────────────────────────────────────────────────────────────────────┤
│ FreeRTOS V11.1 (static allocation only)   rtos/FreeRTOSConfig.h, hooks   │
├──────────────────────────────────────────────────────────────────────────┤
│ bsp/        clocks, NVIC, USART driver, sample timer, timebase, IWDG     │  the only code that
│ startup/    vector table, Reset_Handler        linker/ memory map        │  touches registers
│ libc/       memcpy/memset/strlen, bounded printf (no newlib)             │
└──────────────────────────────────────────────────────────────────────────┘
```

The DSP and link libraries compile unchanged for the host, so the exact
code that runs on the MCU is unit-tested with sanitizers and driven from
Python through ctypes. The BSP is the only layer with register access.
Every register offset is pinned by a `_Static_assert` against the
reference manual.

## 2. Tasks and interrupts

| context | prio | job | blocks on (always bounded) |
|---|---|---|---|
| **sample-clock ISR** | NVIC 5 | produce one sample, hand off full blocks | never |
| **USART1 ISR** | NVIC 6 | data link bytes ↔ stream buffers | never |
| **USART2 ISR** | NVIC 7 | console TX | never |
| **dsp** | 6 | run the active filter on each block, swap banks | DSP queue, 100 ms |
| **link_rx** | 5 | de-frame, validate, dispatch commands | rx stream buffer, 100 ms |
| **link_tx** | 4 | sole owner of the data UART TX: frame and send | TX queue, 100 ms |
| **supervisor** | 2 | heartbeats, CPU load, stack margins, IWDG | `xTaskDelayUntil` 500 ms |
| **log** | 1 | sole owner of the console UART | log queue, 100 ms |
| idle | 0 | `WFI` (sleep until next interrupt) | — |

The task priorities are rate-monotonic (DSP_MATH.md §8.2). All ISR
priorities are numerically ≥ `configMAX_SYSCALL_INTERRUPT_PRIORITY` (5), so
they may use `FromISR` APIs. Priorities 0–4 are left free for future
zero-latency handlers that must not touch the kernel.

## 3. Data flow and ownership (why there is no mutex on the data path)

```
             ┌──────── pool (queue of 8 block pointers) ◄───────────────┐
             ▼                                                          │
sample ISR ──┐                                                          │
             ├─► DSP queue ─► dsp task ─► TX queue ─► link_tx ─► UART ──┘ (block returned)
link_rx ─────┘   (blocks + SWAP + RESET, FIFO)
```

* A **sample block** belongs to whoever holds its pointer. Pointers move
  through queues and are never shared, so no locks are needed and no
  samples are copied between the ISR, DSP and TX stages.
* **Control and data share one FIFO.** SET_FILTER becomes a SWAP message
  behind the blocks already queued, so commands take effect at an exact
  block boundary in stream order. `test_swap_happens_exactly_between_blocks`
  proves this on the emulated board.
* **Ping-pong coefficient banks.** `g_engines[2]` live in CCM. The DSP task
  only touches `g_engines[active]`. link_rx builds the *other* one (parse,
  validate, copy, clear state) outside the real-time path, then queues SWAP.
  A second SET_FILTER while a swap is pending gets `ST_ERR_BUSY`, so the
  inactive bank is never written while it might become active.
* **Single writers.** Each UART has exactly one writer task. Each statistics
  counter has exactly one writer. Aligned 32-bit loads and stores are atomic
  on Cortex-M4, so readers need no lock.

## 4. Real-time rules applied

1. The DSP task never blocks on a slower consumer. If the TX queue is full,
   the block is dropped and counted (`tx_dropped`).
2. ISRs never wait. An empty pool is counted as `siggen_overruns`.
3. Every blocking call has a timeout (≤ 100 ms), so every task loop reaches
   its heartbeat.
4. Nothing slow happens in the hot path: no logging, no formatting, no
   division (except the one integer divide in the EMA), no allocation.
5. Hot data sits in CCM (zero wait states, not reachable by DMA): filter
   coefficients, state and scratch.

## 5. Memory

* **Static allocation only** (`configSUPPORT_DYNAMIC_ALLOCATION 0`, no heap,
  no newlib). The linker's `--print-memory-usage` output is the complete
  memory budget:

  ```
  FLASH:  ~25 KB / 1 MB      RAM:  ~25 KB / 128 KB      CCMRAM: 3.6 KB / 64 KB
  ```
* `.noinit` holds the crash record and the boot counter. Reset_Handler does
  not clear it, so it survives a software reset.
* Measured stack margins are 315–419 words free per task (reported in STATS
  and asserted ≥ 64 by the tests). `-fstack-usage` emits per-function `.su`
  files for static analysis.

## 6. Robustness: detect, record, recover, report

| failure | detected by | action |
|---|---|---|
| HardFault / MemManage / BusFault / UsageFault | naked handler picks MSP/PSP, reads the stacked PC/LR, CFSR, BFAR/MMFAR | crash record → `SYSRESETREQ` |
| `configASSERT` | `app_assert_failed(file, line)` | record (file pointer + line) → reset |
| task stack overflow | FreeRTOS method 2 (pattern check) | record task name → reset |
| task hang | supervisor: no heartbeat for 3 s | record task id → reset |
| scheduler or supervisor hang | IWDG kicked only when all tasks are healthy | hardware reset |
| unexpected IRQ | `Default_Handler` | record exception number → reset |
| corrupted / partial frames | CRC-16, COBS, length checks | counted, frame ignored, resync at next 0x00 |
| unstable / malformed filter | stability triangle, range and length checks | NACK, current filter kept |
| clock-tree failure | POST timer-vs-SysTick calibration (hardware) | `POST_FAIL_CLOCK` in BOOT |
| miscompiled or wrong DSP | POST golden CRCs from the Python model | `POST_FAIL_*` in BOOT |

After any reset, the BOOT frame and the console report *why* the previous
run ended, with PC, LR and CFSR. `test_pil_faults.py` crashes the firmware
deliberately (undefined instruction, failed assert) and checks that the
board reboots, reports correctly, and works afterwards.

## 7. Emulator (QEMU `netduinoplus2`) vs real silicon

The firmware has one compile-time switch, `BSP_TARGET_QEMU`, that changes
only BSP behaviour:

| | QEMU build | hardware build (`make fw TARGET=hw`) |
|---|---|---|
| clock tree | fixed 168 MHz by the machine; RCC not modelled | HSE 8 MHz → PLL 168 MHz, all waits time-bounded |
| cycle counter | TIM5 free-running, scaled by the calibrated timer clock | `DWT->CYCCNT` |
| sample clock | **SysTick hook**: fs/1000 samples per 1 ms tick, from ISR context | TIM2 update interrupt per sample |
| IWDG | not modelled (writes ignored) | live |
| debug fault command | enabled | compiled out (`APP_DEBUG_CMDS=0`) |

**Why the sample clock differs.** QEMU 11.1's `stm32f2xx_timer` model has two
bugs. It arms its next alarm at `now + absolute_deadline` instead of
`now + delay`, and its `CNT` never reloads at `ARR`. Periodic update
interrupts therefore drift further and further apart. This was found during
bring-up: the console showed `TIM2->CNT` climbing past 10⁹ with `ARR=20832`.
It was confirmed against the QEMU source and worked around. SysTick (an ARM
core peripheral) is modelled correctly, so the QEMU build produces the samples
in the tick ISR in bursts, the way a DMA half-transfer interrupt delivers
them. The ISR → pool → queue → DSP path is the same code.

**Timing numbers under QEMU** (cycles/sample, CPU load) are indicative only.
QEMU is not cycle-accurate. Functional results (every output sample, every
fault path) are exact.

## 8. Wire protocol

`libs/link/include/link/protocol_defs.h` is the single source of truth.
`python/rtdsp/protocol.py` mirrors it, and `test_contract.py` parses the C
header to keep the two in sync.

```
frame on the wire:  COBS( type | seq | payload ≤ 600 B | crc16 LE ) 0x00

host → device: PING, SET_FILTER, PROCESS, GET_STATS, RESET_STATE, SET_MODE, DEBUG_FAULT
device → host: PONG/BOOT (devinfo), ACK (type, status), PROCESS_RESULT, STATS, STREAM
```

The host keeps at most 4 PROCESS frames in flight (a sliding window), which
bounds the device's buffer use. Results come back in order because the
pipeline is FIFO end to end.
