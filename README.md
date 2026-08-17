# GPU-Resident High-Fidelity Audio Pipeline

An engineering worksheet for testing whether a GPU-resident Linux audio pipeline can improve the quality–compute–latency envelope over a conventional CPU and system-memory design.

This repository begins as an experiment specification, not a performance claim. Fill in the worksheet before implementation, record every result with reproducible evidence, and advance only when the measured gates are met.

## Feasibility baseline

Keep these constraints visible throughout the project:

- A GPU cannot restore information absent from the source or exceed the analog performance of the DAC and output chain.
- Higher internal precision and oversampling can reduce processing error, but they do not make an untouched bit-perfect source intrinsically higher fidelity.
- No codec “beats every codec.” Lossless, perceptual quality, bitrate, latency, energy, random access, and implementation maturity form a multi-objective trade-off.
- GPU throughput is not the same as real-time determinism. Worst-case scheduling and synchronization latency matter more than average kernel time.
- Direct VRAM-to-audio transport requires compatible GPU memory export, PCIe topology, IOMMU policy, driver support, and an endpoint capable of peer DMA. Most commodity USB audio devices cannot consume VRAM directly.
- Codec, neural, resampling, and DSP work belongs in userspace. Kernel code should remain limited to transport, synchronization, DMA, timestamps, interrupts, recovery, and power-management hooks.

## How to use this worksheet

1. Replace each em dash or blank line with a measured value, decision, or link to evidence.
2. Check an option by changing “[ ]” to “[x]”.
3. Keep the CPU reference and GPU candidate functionally equivalent.
4. Report median, p95, p99, p99.9, maximum, run duration, and sample count for timing results.
5. Treat a result as valid only when the raw data, configuration, software versions, and test procedure are recorded.

---

## 1. Project definition

**Working project name:**  
—

**Owner(s):**  
—

**Experiment date / revision:**  
—

### Primary objective

- [ ] Higher measured playback fidelity
- [ ] Lower CPU utilization
- [ ] Lower end-to-end latency
- [ ] Greater real-time DSP capacity
- [ ] Better compression efficiency
- [ ] Fewer memory copies
- [ ] Direct VRAM-to-audio-device transport
- [ ] Neural or latent audio reconstruction
- [ ] Multichannel or spatial audio
- [ ] Lower total system energy
- [ ] Other: —

### Core hypothesis

> A GPU-resident audio pipeline can decode, process, buffer, and possibly transport audio from VRAM to an audio endpoint while maintaining deterministic playback and improving at least one defined metric without unacceptable regressions elsewhere.

### What does “better” mean?

Set targets before running the candidate implementation.

| Metric | CPU baseline | GPU target | Measurement method |
|---|---:|---:|---|
| End-to-end latency | — ms | — ms | — |
| Stable hardware period | — samples | — samples | — |
| Sample rate | — kHz | — kHz | — |
| Output word length | — bit | — bit | — |
| Internal precision | — | — | — |
| THD+N | — dB | — dB | — |
| SINAD | — dB | — dB | — |
| CPU utilization | — % | — % | — |
| GPU utilization | — % | — % | — |
| Host↔device traffic | — GB/s | — GB/s | — |
| Total system power | — W | — W | — |
| XRUNs | — /hour | — /hour | — |
| Worst-case deadline margin | — µs | — µs | — |
| DSP capacity | — | — | — |

**Primary success metric:**  
—

**Allowed regressions and limits:**  
—

---

## 2. Proposed data path

### First-generation path

    Source
      ↓
    Compressed or lossless input
      ↓
    VRAM input ring
      ↓
    GPU decode or PCM upload
      ↓
    GPU DSP graph
      ↓
    High-precision PCM ring
      ↓
    Pinned host staging ring
      ↓
    ALSA
      ↓
    Audio interface
      ↓
    DAC

### Transport candidate

- [ ] VRAM → pageable RAM → ALSA
- [ ] VRAM → pinned RAM → ALSA
- [ ] VRAM → DMA-BUF → driver
- [ ] VRAM → PCIe peer-to-peer DMA → audio endpoint
- [ ] Custom PCIe audio hardware
- [ ] USB audio endpoint through host memory
- [ ] Thunderbolt or PCIe endpoint
- [ ] Other: —

**Selected Phase 1 path:**  
—

**Fallback path:**  
—

**Expected copies per block:**  
—

**Expected synchronization points per block:**  
—

---

## 3. Source representation

**Initial source format:**

- [ ] PCM / WAV
- [ ] FLAC
- [ ] ALAC
- [ ] Opus
- [ ] AAC
- [ ] Custom lossless codec
- [ ] Custom perceptual codec
- [ ] Neural codec
- [ ] Latent representation

| Source property | Value |
|---|---|
| Sample rate | — kHz |
| Word length | — bit |
| Channel count | — |
| Channel layout | — |
| Nominal bitrate | — kb/s |
| Peak bitrate | — kb/s |
| Block or frame size | — |

### Compression goal

- [ ] Bit-exact lossless
- [ ] Perceptually transparent at a declared test condition
- [ ] Maximum compression
- [ ] GPU-decode optimized
- [ ] Low-latency optimized
- [ ] Random-access optimized
- [ ] Low-energy optimized

**Target bitrate or ratio:**  
—

**Reference corpus:**  
—

**Corpus license and checksum manifest:**  
—

---

## 4. Fidelity architecture

### Internal processing format

- [ ] FP16
- [ ] BF16
- [ ] FP32
- [ ] FP64
- [ ] Fixed point
- [ ] Mixed precision

**Chosen format:**  
—

**Accumulator format:**  
—

**Reason and numerical error budget:**  
—

### Internal DSP sample rate

- [ ] Native
- [ ] 2×
- [ ] 4×
- [ ] 8×
- [ ] 192 kHz
- [ ] 384 kHz
- [ ] 768 kHz
- [ ] Other: —

### Final DAC format

| Property | Value |
|---|---|
| Sample rate | — kHz |
| Word length | — bit |
| Channels | — |
| ALSA sample format | — |

### Quantization strategy

- [ ] Single final quantization
- [ ] TPDF dithering
- [ ] Noise shaping
- [ ] Integer output
- [ ] Floating-point-capable endpoint

**Clipping and headroom policy:**  
—

**Bit-perfect bypass definition:**  
—

---

## 5. GPU DSP graph

Check only features included in the current experimental comparison.

- [ ] Codec decode
- [ ] Resampling
- [ ] Oversampling
- [ ] Parametric EQ
- [ ] Linear-phase EQ
- [ ] FIR convolution
- [ ] IIR filtering
- [ ] Crossover
- [ ] Room correction
- [ ] Headphone correction
- [ ] HRTF
- [ ] Binaural rendering
- [ ] Object-based spatial rendering
- [ ] Dynamic-range control
- [ ] Limiting
- [ ] Clipping protection
- [ ] Speaker correction
- [ ] Phase correction
- [ ] Loudness normalization
- [ ] Stem separation
- [ ] Neural enhancement
- [ ] Neural decoding
- [ ] Other: —

### Maximum expected workload

| Parameter | Value |
|---|---:|
| FIR taps per channel | — |
| Channels | — |
| Convolution partitions | — |
| Oversampling factor | — |
| FFT size | — |
| Model parameters, if applicable | — |
| Required processing time per block | — µs |

**Graph ordering and latency contribution:**  
—

---

## 6. Real-time requirements

### Period duration

| Samples | 48 kHz | 96 kHz | 192 kHz |
|---:|---:|---:|---:|
| 32 | 0.67 ms | 0.33 ms | 0.17 ms |
| 64 | 1.33 ms | 0.67 ms | 0.33 ms |
| 128 | 2.67 ms | 1.33 ms | 0.67 ms |
| 256 | 5.33 ms | 2.67 ms | 1.33 ms |
| 512 | 10.67 ms | 5.33 ms | 2.67 ms |
| 1024 | 21.33 ms | 10.67 ms | 5.33 ms |

| Requirement | Target |
|---|---:|
| Hardware period | — samples |
| GPU processing block | — samples |
| Render-ahead depth | — periods |
| Maximum end-to-end latency | — ms |
| Worst-case deadline margin | — µs |
| Maximum tolerated XRUN rate | — /hour |
| Stress-test duration | — hours |

### Deadline rule

For every period:

    GPU execution
    + queueing and synchronization
    + buffer publication
    + transfer or DMA scheduling
    + safety margin
    < audio-period deadline

Average performance is insufficient. Record the worst observed block and the distribution tail.

---

## 7. Buffer architecture

### Input ring

**Location:**

- [ ] VRAM
- [ ] RAM
- [ ] Shared or unified memory

**Size:**  
—

**Block alignment:**  
—

### Output PCM ring

**Location:**

- [ ] VRAM
- [ ] Pinned RAM
- [ ] DMA-BUF
- [ ] Endpoint-accessible P2P memory

**Periods buffered:**  
—

**Back-pressure policy:**  
—

### Buffer state model

| State | Owner | Valid transition(s) |
|---|---|---|
| FREE | — | — |
| GPU_PROCESSING | — | — |
| READY | — | — |
| DMA_ACTIVE | — | — |
| COMPLETE | — | — |
| ERROR | — | — |

### Synchronization

- [ ] GPU fences
- [ ] DMA fences
- [ ] Timeline semaphore
- [ ] Atomic producer/consumer counters
- [ ] Interrupt completion
- [ ] Polling
- [ ] Hybrid

**Memory-ordering contract:**  
—

**Timeout and recovery behavior:**  
—

---

## 8. Linux software stack

### Userspace

- [ ] PipeWire
- [ ] JACK
- [ ] Direct ALSA
- [ ] CUDA
- [ ] HIP
- [ ] Vulkan Compute
- [ ] OpenCL
- [ ] Custom daemon

**Preferred GPU API:**  
—

**Audio API and scheduling policy:**  
—

**Privilege model:**  
—

### Kernel scope

Potential transport components:

- [ ] ALSA PCM driver
- [ ] DMA-BUF importer or exporter
- [ ] PCI P2PDMA integration
- [ ] IOMMU handling
- [ ] GPU memory mapping
- [ ] Audio DMA descriptor handling
- [ ] Hardware timestamps
- [ ] Period interrupts
- [ ] XRUN recovery
- [ ] Power-management hooks

Keep these in userspace:

- Codec implementation
- Neural model
- DSP graph
- Resampler
- Convolution engine
- User configuration
- Experiment logging

**Kernel ABI boundary:**  
—

**Upstream or out-of-tree strategy:**  
—

---

## 9. Hardware requirements

### GPU

- [ ] NVIDIA
- [ ] AMD
- [ ] Intel
- [ ] Other: —

| Property | Value |
|---|---|
| Model | — |
| Architecture | — |
| VRAM | — GiB |
| Driver | — |
| Runtime / API version | — |
| PCIe generation and width | — |
| Resizable BAR | — |
| Persistence / power mode | — |

### Audio device

- [ ] PCIe
- [ ] USB
- [ ] Thunderbolt
- [ ] Integrated
- [ ] Custom FPGA endpoint

| Property | Value |
|---|---|
| Model | — |
| Firmware | — |
| Maximum sample rate | — kHz |
| Maximum channels | — |
| Supported PCM formats | — |
| DMA architecture documented | Yes / No |
| Hardware clock accessible | Yes / No |

### PCIe topology

GPU and audio endpoint share:

- [ ] Same PCIe switch
- [ ] Same root port
- [ ] Same root complex
- [ ] Different root complexes
- [ ] Unknown

| Gate | Result / evidence |
|---|---|
| P2P DMA supported by both devices | — |
| P2P route allowed by topology | — |
| ACS policy compatible | — |
| IOMMU mapping compatible | — |
| DMA address width compatible | — |
| Cache coherency and fencing defined | — |

---

## 10. Fidelity test suite

### Test paths

- [ ] Digital loopback
- [ ] Analog loopback
- [ ] Audio analyzer
- [ ] High-quality ADC
- [ ] Reference DAC
- [ ] Reference headphone amplifier
- [ ] Level-matched, randomized ABX test

### Measurements

- [ ] Bit-exact comparison
- [ ] Frequency response
- [ ] Phase response
- [ ] THD
- [ ] THD+N
- [ ] SINAD
- [ ] IMD
- [ ] Noise floor
- [ ] Dynamic range
- [ ] Crosstalk
- [ ] Jitter
- [ ] Inter-sample peaks
- [ ] Reconstruction-filter behavior
- [ ] Resampling artifacts
- [ ] Dither spectrum

| Test control | Value |
|---|---|
| Analyzer / ADC model | — |
| Calibration date | — |
| Sample corpus | — |
| Output level | — dBFS / Vrms |
| Gain matching tolerance | — dB |
| Warm-up time | — |
| Trial count | — |
| Statistical threshold | — |

**Raw measurement location:**  
—

---

## 11. Codec comparison matrix

Populate measurements on the same corpus, hardware, output format, and quality criterion.

| Representation | Bitrate | Fidelity mode | GPU decoder maturity | Decode latency | Lossless | VRAM-friendly | Notes |
|---|---:|---|---|---:|:---:|:---:|---|
| PCM | — | Exact, uncompressed | Trivial copy / ingest | — | Yes | Yes | Bandwidth and storage baseline |
| FLAC | — | Exact, compressed | Candidate / measure | — | Yes | Candidate | Dependency-heavy serial work may limit gains |
| Opus | — | Perceptual | Candidate / measure | — | No | Candidate | Low-latency modes available |
| AAC | — | Perceptual | Candidate / measure | — | No | Candidate | Compare like-for-like profiles |
| Custom GPU codec | — | — | Prototype | — | — | Design goal | Must justify ecosystem cost |
| Neural latent codec | — | — | Prototype | — | Usually no | Design goal | Include model cost and artifacts |

### Claim rule

Do not claim a codec win unless it improves a declared objective under the same constraints. Normalize metrics before combining them, publish the individual metrics, and show the Pareto frontier rather than relying only on a scalar score.

One optional lower-is-better score is:

    score =
        a × normalized reconstruction error
      + b × normalized bitrate
      + c × normalized decode latency
      + d × normalized total energy
      + e × normalized memory use
      + f × normalized deadline-miss rate

| Weight | Value | Rationale |
|---|---:|---|
| a | — | — |
| b | — | — |
| c | — | — |
| d | — | — |
| e | — | — |
| f | — | — |

**Quality constraint used for bitrate comparisons:**  
—

---

## 12. Prototype stages

### Phase 1 — CPU baseline

- [ ] Build conventional ALSA or PipeWire output path
- [ ] Establish bit-perfect bypass
- [ ] Measure end-to-end latency and jitter
- [ ] Measure XRUN rate under idle and stress conditions
- [ ] Measure CPU use and total system power
- [ ] Freeze the reference corpus and configuration

**Exit evidence:**  
—

### Phase 2 — GPU DSP

- [ ] Allocate persistent GPU buffers
- [ ] Implement functionally equivalent PCM processing
- [ ] Add GPU resampling
- [ ] Add partitioned convolution
- [ ] Measure execution and scheduling jitter
- [ ] Compare output numerically against the CPU reference

**Exit evidence:**  
—

### Phase 3 — GPU decode

- [ ] Decode compressed input into VRAM
- [ ] Remove avoidable host copies
- [ ] Benchmark throughput and tail latency
- [ ] Benchmark total system energy
- [ ] Validate malformed-input handling

**Exit evidence:**  
—

### Phase 4 — Render-ahead engine

- [ ] Persistent GPU worker
- [ ] Timestamped audio blocks
- [ ] Configurable render-ahead
- [ ] Deadline monitoring
- [ ] XRUN recovery
- [ ] Clock-drift correction

**Exit evidence:**  
—

### Phase 5 — DMA-BUF

- [ ] Export or import GPU-backed buffers
- [ ] Validate ownership and synchronization
- [ ] Integrate the ALSA transport path
- [ ] Count and measure all memory copies
- [ ] Exercise GPU reset and process-exit cleanup

**Exit evidence:**  
—

### Phase 6 — PCIe P2P

- [ ] Verify PCIe topology
- [ ] Validate GPU P2P capability
- [ ] Validate endpoint DMA capability
- [ ] Attempt VRAM-to-endpoint DMA
- [ ] Verify IOMMU and ACS behavior
- [ ] Benchmark transfer latency and jitter
- [ ] Prove safe fallback when P2P is unavailable

**Exit evidence:**  
—

### Phase 7 — Custom audio endpoint

- [ ] FPGA proof of concept
- [ ] PCIe endpoint
- [ ] DMA descriptor engine
- [ ] Sufficient hardware FIFO
- [ ] Stable hardware clock
- [ ] I²S or TDM output
- [ ] DAC integration
- [ ] Fault containment and firmware update path

**Exit evidence:**  
—

---

## 13. Critical failure modes

| Failure | Detection | Mitigation |
|---|---|---|
| GPU scheduling spike | Deadline telemetry and maximum block time | Render ahead; reserve headroom; fall back to CPU |
| XRUN | ALSA counters and discontinuity detector | Silence or crossfade policy; resync; increase periods |
| DMA starvation | Descriptor and completion timeout | Prequeue descriptors; bounded retry; fallback copy |
| P2P unsupported | Startup capability and topology probe | Pinned host staging path |
| IOMMU or ACS incompatibility | Mapping failure and route validation | Supported IOMMU mode; host staging; fail closed |
| GPU reset | Driver event and lost fence | Invalidate buffers; restart graph; CPU fallback |
| Clock drift | Timestamp slope and ring occupancy | Adaptive resampling or controlled frame slip |
| Ring underrun | Producer/consumer distance | Increase render ahead; shed optional DSP |
| Ring overrun | Producer/consumer distance | Back pressure; drop only under declared policy |
| Excess power | Wall-power and telemetry threshold | Batch work; lower clocks; use CPU below crossover |
| Neural artifact | Objective checks and blinded listening | Confidence gate; bypass; model rollback |
| Sample corruption | Checksums, sentinels, digital loopback | Quarantine block; reset path; fail to silence |
| Priority inversion | Scheduler and trace analysis | Isolate threads; bound locks; lock-free handoff |
| Thermal throttling | Clock and temperature telemetry | Thermal headroom; sustained-load qualification |
| Device hot-unplug | Device and driver event | Stop DMA safely; release mappings; reconnect flow |

**Additional project-specific hazards:**  
—

---

## 14. Go / no-go criteria

Advance beyond a prototype only when all mandatory criteria pass:

- [ ] Lossless mode is bit-perfect at the declared observation point.
- [ ] The candidate adds no statistically significant analog distortion.
- [ ] Playback survives the declared idle and stress runs.
- [ ] Worst-case processing remains inside the deadline envelope with margin.
- [ ] XRUN incidence is no worse than the CPU baseline.
- [ ] The GPU enables materially more DSP, lower CPU use, or another primary win.
- [ ] Total system power remains inside the budget.
- [ ] End-to-end latency remains inside the budget.
- [ ] Zero-copy or P2P provides a measured benefit after synchronization cost.
- [ ] Unsupported hardware degrades safely to a documented fallback.
- [ ] The implementation does not require kernel-space codecs or DSP.
- [ ] Results reproduce across at least — runs and — hardware configurations.

**Decision:** Go / Conditional / No-go  

**Decision owner and date:**  
—

**Evidence links:**  
—

---

## 15. Central research question

> Can GPU-resident audio processing produce a materially better quality–compute–latency envelope than a conventional CPU and system-memory audio architecture?

**Result:**

- [ ] Yes
- [ ] Partially
- [ ] No
- [ ] Requires custom hardware
- [ ] Inconclusive

**What improved:**  
—

**What regressed:**  
—

**Confidence and limitations:**  
—

**Evidence:**  
—

---

## 16. First experimental build

### Recommended minimum experiment

    FLAC or WAV
       ↓
    decode on CPU, or upload PCM as the controlled first step
       ↓
    GPU VRAM
       ↓
    FP32 GPU DSP
       ↓
    GPU circular buffer
       ↓
    pinned host-memory staging ring
       ↓
    ALSA
       ↓
    24-bit or 32-bit DAC

The first build deliberately uses pinned host staging. It tests whether GPU DSP is worthwhile before taking on DMA-BUF, peer DMA, or a custom kernel driver. Add GPU-native decode only after the PCM path is stable and measured.

### Measure simultaneously

1. GPU execution-time distribution.
2. Queueing and synchronization latency.
3. Worst-case scheduling latency.
4. ALSA period timing and end-to-end latency.
5. XRUN count and discontinuities.
6. CPU and GPU utilization.
7. Host↔device bytes per second and copies per block.
8. Bit-perfect digital output.
9. THD+N and SINAD through the analog path.
10. Total system power.
11. Output delta against the CPU reference.

### Prototype 1 success criterion

> The GPU path must maintain bit-perfect lossless output, complete the sustained stress run without XRUNs, retain the declared worst-case timing margin, and demonstrate either materially greater DSP capacity or materially lower CPU cost without violating the latency and power budgets.

| Prototype result | Value |
|---|---|
| Commit / build ID | — |
| Test date | — |
| Configuration | — |
| Run duration | — |
| Primary metric result | — |
| Deadline margin | — |
| XRUNs | — |
| Bit-perfect | — |
| Power delta | — |
| Decision | — |

---

## 17. Experiment log

Add one row per controlled run. Link to raw results rather than pasting only summaries.

| Run ID | Commit | Hardware | Configuration | Duration | Result | Raw data |
|---|---|---|---|---:|---|---|
| — | — | — | — | — | — | — |

## 18. Decision record

| Date | Decision | Evidence | Consequence | Owner |
|---|---|---|---|---|
| — | — | — | — | — |

## License

No license has been selected yet. Add one before accepting external contributions or redistributing implementation code.
