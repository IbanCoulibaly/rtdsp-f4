# Verification strategy

## The test pyramid

| level | what runs | where | count | command |
|---|---|---|---|---|
| **Unit** (C) | `libs/dsp`, `libs/link` with UBSan (+ ASan on Linux) | host | 11 suites, ~3 700 checks | `make unit` |
| **Contract** | C headers ↔ Python mirror (message IDs, layouts, enums) | host | 3 | `pytest -k contract` |
| **Design** | quantised coefficients still meet spec; overflow and scaling proofs | host | 7 | `pytest -k design` |
| **SIL** (software-in-the-loop) | the firmware's C engine via ctypes vs. bit-exact Python model and float64 scipy | host | ~30 | `make test-sil` |
| **PIL** (processor-in-the-loop) | the real ELF on QEMU's STM32F405, driven over the wire protocol | emulator | 17 | `make test-pil` |
| **POST** | on-target self test against Python-generated golden CRCs | every boot | 5 checks | (automatic) |

The three levels are independent witnesses. The Python model, the C code on
the host and the C code cross-compiled for Cortex-M4 must agree **bit for
bit** on every output sample for all four kernel types, including float32
(DSP_MATH.md §6 explains why that is achievable).

## Requirements traceability

| ID | Requirement | Verified by |
|---|---|---|
| R1 | Run FIR and IIR filters, in fixed and floating point, on an STM32F405 under an RTOS | `test_pil.py::test_target_output_bit_exact[*]` |
| R2 | Target output identical to the reference model | `test_sil.py::test_c_engine_is_bit_exact_with_python_model`, `test_pil.py::test_target_output_bit_exact`, POST |
| R3 | Quantised filters meet their design spec | `test_design.py::*`, `test_sil.py::test_close_to_ideal_float64_filter` (SQNR > 55 dB) |
| R4 | No arithmetic overflow; saturate instead of wrapping | `test_post_shift_rule_prevents_accumulator_overflow`, `test_saturates_instead_of_wrapping`, unit `test_engine_bypass_and_saturation` |
| R5 | Unstable or malformed filters are rejected; the running filter is unaffected | `test_engine_rejects_*`, `test_pil.py::test_unstable_filter_rejected_and_previous_kept` |
| R6 | Coefficient updates are glitch-free and take effect at a block boundary | `test_pil.py::test_swap_happens_exactly_between_blocks` |
| R7 | Processing is independent of block segmentation | `test_sil.py::test_block_size_does_not_change_output`, unit `*_block_invariance`, POST (uneven chunks) |
| R8 | Corrupted link traffic is detected, counted and survived | `test_protocol.py::*`, `test_pil.py::test_link_survives_corruption` |
| R9 | ISR-driven acquisition reaches the DSP task without loss | `test_pil.py::test_siggen_isr_pipeline` (bit-exact, no drops, rate check) |
| R10 | Faults are captured, survive reset and are reported | `test_pil_faults.py::*` |
| R11 | Health is observable: cycles, load, stack margins, error counters | `test_pil.py::test_health_metrics` |
| R12 | Host and firmware agree on the protocol | `test_contract.py::*`, `test_protocol.py::test_c_and_python_build_identical_frames` |

## Running

```bash
make unit          # C unit tests
make test-sil      # everything that does not need the emulator
make test          # unit + SIL + PIL (builds firmware first)
make demo          # plots + report in build/reports/
```

PIL tests skip themselves if `qemu-system-arm` or the ELF is missing, so
`make test-sil` works on a machine without the cross-toolchain.
