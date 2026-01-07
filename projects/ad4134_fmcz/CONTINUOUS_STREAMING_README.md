# AD4134 Continuous Streaming - no-OS Implementation

## Overview

This implementation provides **gap-free, continuous data streaming** from the AD4134 ADC to system memory using ping-pong buffering. It eliminates the dead time that occurs in single-shot DMA transfers by using dual buffers that alternate between DMA writes and software processing.

## Architecture

### Ping-Pong Buffer Flow

```
┌─────────────────────────────────────────────────────────────┐
│             Continuous Streaming Operation                  │
└─────────────────────────────────────────────────────────────┘

Time 0:  Start DMA → Buffer A

Time 1:  DMA completes Buffer A
         Start DMA → Buffer B
         Process Buffer A (in parallel with DMA filling B)

Time 2:  DMA completes Buffer B
         Start DMA → Buffer A
         Process Buffer B (in parallel with DMA filling A)

Time 3:  Repeat cycle...

Result: ZERO dead time - always capturing or processing!
```

### Software Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                Application Layer                             │
│  - ad713x_fmc_streaming_example.c                           │
│  - Custom data processing callbacks                         │
└────────────────────┬─────────────────────────────────────────┘
                     │
                     ▼
┌──────────────────────────────────────────────────────────────┐
│            Streaming API Layer                               │
│  - ad4134_continuous_streaming.h/.c                         │
│  - Buffer management                                        │
│  - Statistics tracking                                      │
│  - State machine                                            │
└────────────────────┬─────────────────────────────────────────┘
                     │
                     ▼
┌──────────────────────────────────────────────────────────────┐
│              Driver Layer                                    │
│  - SPI Engine driver (spi_engine.c)                         │
│  - AXI DMAC driver (axi_dmac.c)                             │
│  - AD713x device driver (ad713x.c)                          │
└────────────────────┬─────────────────────────────────────────┘
                     │
                     ▼
┌──────────────────────────────────────────────────────────────┐
│              Hardware Layer                                  │
│  - FPGA (modified ad4134_bd.tcl with CYCLIC=1)              │
│  - SPI Engine IP                                            │
│  - AXI DMAC IP                                              │
│  - AD4134 ADC hardware                                      │
└──────────────────────────────────────────────────────────────┘
```

## Files

### New Files Created

1. **ad4134_continuous_streaming.h**
   - API header for streaming functionality
   - Data structures for buffers, statistics, configuration

2. **ad4134_continuous_streaming.c**
   - Core streaming implementation
   - Ping-pong buffer management
   - DMA transfer coordination
   - Performance monitoring

3. **ad713x_fmc_streaming_example.c**
   - Complete example application
   - Shows how to use streaming API
   - Includes sample data processing callbacks

4. **CONTINUOUS_STREAMING_README.md** (this file)
   - Documentation and usage guide

### Modified Files

**In HDL project:**
- `projects/ad4134_fmc/common/ad4134_bd.tcl`
  - Changed `CONFIG.CYCLIC` from 0 to 1
  - Changed `CONFIG.DMA_2D_TRANSFER` from 0 to 1
  - Added `CONFIG.MAX_BYTES_PER_BURST` = 128
  - Added `CONFIG.FIFO_SIZE` = 16

## Configuration

### Buffer Configuration

Edit `ad4134_continuous_streaming.h` to adjust buffer parameters:

```c
/* Number of buffers (2 = ping-pong, 3+ = circular queue) */
#define STREAMING_NUM_BUFFERS           2

/* Samples per buffer */
#define STREAMING_BUFFER_SIZE_SAMPLES   (512 * 1024)  /* 512K samples */

/* Bytes per sample (4 channels × 32 bits = 16 bytes) */
#define STREAMING_BYTES_PER_SAMPLE      16
```

**Buffer Size Selection:**

| Buffer Size | Fill Time @ 1.176 MHz | Processing Budget |
|-------------|----------------------|-------------------|
| 256K samples | ~217 ms | 217 ms |
| 512K samples | ~435 ms | 435 ms |
| 1M samples | ~850 ms | 850 ms |

Choose based on your processing requirements:
- **Faster processing** (< 200ms) → Use smaller buffers (256K)
- **Slower processing** (file I/O, network) → Use larger buffers (1M)
- **Very slow processing** → Use 3-4 buffers instead of 2

### Sample Rate Configuration

The sample rate is controlled by the PWM generator in the FPGA:

**In ad4134_bd.tcl:**
```tcl
ad_ip_parameter odr_generator CONFIG.PULSE_0_PERIOD 85  # Clock cycles
```

**Calculation:**
```
Sample Rate = Clock Frequency / PULSE_0_PERIOD
            = 100 MHz / 85
            = 1.176 MHz
```

To change sample rate, modify `PULSE_0_PERIOD`:
- **500 kHz:** PULSE_0_PERIOD = 200
- **1 MHz:** PULSE_0_PERIOD = 100
- **2 MHz:** PULSE_0_PERIOD = 50 (if SPI can keep up)

## Usage

### Basic Usage

```c
#include "ad4134_continuous_streaming.h"

/* 1. Define data processing callback */
void my_data_callback(uint32_t *data, uint32_t num_samples, void *user_data)
{
    /* Process samples here */
    for (uint32_t i = 0; i < num_samples; i++) {
        uint32_t ch0 = data[i * 4 + 0];
        uint32_t ch1 = data[i * 4 + 1];
        uint32_t ch2 = data[i * 4 + 2];
        uint32_t ch3 = data[i * 4 + 3];

        /* Your processing: DSP, file write, network send, etc. */
    }
}

/* 2. Initialize streaming */
struct streaming_context *ctx;
struct streaming_init_param init_param = {
    .spi_desc = spi_eng_desc,
    .offload_msg = &offload_message,
    .samples_per_transfer = STREAMING_BUFFER_SIZE_SAMPLES,
    .data_callback = my_data_callback,
    .user_data = NULL
};

streaming_init(&ctx, &init_param);

/* 3. Start streaming */
streaming_start(ctx);

/* 4. Process data in loop */
while (keep_running) {
    int ret = streaming_process(ctx);
    if (ret != 0) {
        break;  /* Error or stop requested */
    }
}

/* 5. Stop and cleanup */
streaming_stop(ctx);
streaming_remove(ctx);
```

### Running the Example

1. **Build the FPGA project** with modified `ad4134_bd.tcl`:
   ```bash
   cd /path/to/hdl
   make ad4134_fmc.zed
   ```

2. **Program the FPGA** with generated bitstream

3. **Build the no-OS project:**
   ```bash
   cd /path/to/no-OS/projects/ad4134_fmcz
   make
   ```

4. **Run on target:**
   - Copy executable to SD card or load via JTAG
   - Execute binary
   - Observe continuous streaming output

### Expected Output

```
========================================
AD4134 Continuous Streaming Example
========================================

Initializing AD4134 device...
Initializing SPI Engine...
Synchronizing AD4134 channels...

--- Initializing Continuous Streaming ---
Allocated buffer 0 at 0x10000000 (524288 samples, 8388608 bytes)
Allocated buffer 1 at 0x10800000 (524288 samples, 8388608 bytes)
Streaming context initialized successfully
Configuration:
  - Buffers: 2 x 524288 samples (8388608 bytes each)
  - Samples per transfer: 524288
  - Total buffer capacity: 1048576 samples

Streaming started

========================================
Continuous streaming active!
Processing buffers...
Press CTRL+C to stop (or will auto-stop after 1000 buffers)
========================================

First 10 samples from buffer 1:
Sample 0: 0x12345600, 0x23456700, 0x34567800, 0x45678900
Sample 1: 0x12345A00, 0x23456B00, 0x34567C00, 0x45678D00
...

--- Buffer 100 Statistics ---
Total samples processed: 52428800
CH0: Min=-3.9876V, Max=+4.0123V, Avg=+0.0023V
CH1: Min=-4.0012V, Max=+3.9987V, Avg=-0.0001V
CH2: Min=-3.9954V, Max=+4.0054V, Avg=+0.0012V
CH3: Min=-4.0023V, Max=+3.9976V, Avg=-0.0003V
----------------------------

=== Streaming Statistics ===
Runtime: 100000 ms (100.00 seconds)
Total samples: 104857600
Total buffers: 200
Sample rate: 1048576 samples/sec
Buffer overruns: 0
DMA errors: 0
Max processing time: 234 ms
Avg processing time: 187 ms
SUCCESS: No buffer overruns - continuous streaming working!
===========================
```

## Data Processing Examples

### Example 1: Calculate Statistics

```c
void stats_callback(uint32_t *data, uint32_t num_samples, void *user_data)
{
    static float min[4] = {999, 999, 999, 999};
    static float max[4] = {-999, -999, -999, -999};

    float lsb = 8.192f / 16777216.0f;  /* 24-bit, ±4.096V */

    for (uint32_t i = 0; i < num_samples; i++) {
        for (int ch = 0; ch < 4; ch++) {
            int32_t raw = data[i * 4 + ch] & 0xFFFFFF00;
            raw >>= 8;
            float voltage = lsb * raw;
            if (voltage > 4.095f) voltage -= 8.192f;

            if (voltage < min[ch]) min[ch] = voltage;
            if (voltage > max[ch]) max[ch] = voltage;
        }
    }
}
```

### Example 2: Write to File (FatFS)

```c
#include "ff.h"  /* FatFS library */

void file_write_callback(uint32_t *data, uint32_t num_samples, void *user_data)
{
    FIL *file = (FIL *)user_data;
    UINT bytes_written;

    /* Write binary data */
    f_write(file, data,
            num_samples * 4 * sizeof(uint32_t),
            &bytes_written);
}

/* In main: */
FIL data_file;
f_open(&data_file, "capture.bin", FA_CREATE_ALWAYS | FA_WRITE);

init_param.data_callback = file_write_callback;
init_param.user_data = &data_file;
```

### Example 3: Network Streaming (lwIP)

```c
#include "lwip/udp.h"

struct network_context {
    struct udp_pcb *pcb;
    ip_addr_t dest_addr;
    uint16_t dest_port;
};

void network_callback(uint32_t *data, uint32_t num_samples, void *user_data)
{
    struct network_context *net_ctx = (struct network_context *)user_data;
    struct pbuf *p;

    /* Allocate packet buffer */
    p = pbuf_alloc(PBUF_TRANSPORT,
                   num_samples * 4 * sizeof(uint32_t),
                   PBUF_RAM);

    if (p != NULL) {
        /* Copy data to packet */
        memcpy(p->payload, data, num_samples * 4 * sizeof(uint32_t));

        /* Send UDP packet */
        udp_sendto(net_ctx->pcb, p, &net_ctx->dest_addr, net_ctx->dest_port);

        /* Free packet */
        pbuf_free(p);
    }
}
```

### Example 4: FFT Processing

```c
#include "arm_math.h"  /* CMSIS-DSP library */

#define FFT_SIZE 1024

void fft_callback(uint32_t *data, uint32_t num_samples, void *user_data)
{
    static float32_t fft_input[FFT_SIZE * 2];  /* Complex input */
    static float32_t fft_output[FFT_SIZE];

    arm_rfft_fast_instance_f32 fft_instance;
    arm_rfft_fast_init_f32(&fft_instance, FFT_SIZE);

    /* Convert first channel to float */
    for (uint32_t i = 0; i < FFT_SIZE && i < num_samples; i++) {
        int32_t raw = data[i * 4] & 0xFFFFFF00;
        raw >>= 8;
        fft_input[i] = (float32_t)raw / 8388608.0f;  /* Normalize */
    }

    /* Compute FFT */
    arm_rfft_fast_f32(&fft_instance, fft_input, fft_output, 0);

    /* Compute magnitude */
    arm_cmplx_mag_f32(fft_output, fft_output, FFT_SIZE / 2);

    /* Process FFT results... */
}
```

## Performance Optimization

### 1. Buffer Size Tuning

**Problem:** Buffer overruns (data loss)

**Solutions:**
- Increase buffer size: `STREAMING_BUFFER_SIZE_SAMPLES`
- Use more buffers: `STREAMING_NUM_BUFFERS = 3 or 4`
- Reduce sample rate (modify PWM period)
- Optimize callback function

### 2. Cache Management

The implementation uses `Xil_DCacheInvalidateRange()` before and after DMA transfers. This is **critical** for cache coherency.

**Tips:**
- Ensure buffers are cache-line aligned (64 bytes)
- Invalidate full buffer region, not partial
- Don't access buffer during DMA transfer

### 3. Callback Optimization

**The data_callback must complete faster than buffer fill time!**

Buffer fill time calculation:
```
Fill Time = Buffer Size / Sample Rate
          = 524288 samples / 1.176 MHz
          = 445 ms
```

Your callback must complete in < 445 ms (for 512K buffers).

**Optimization tips:**
- Avoid printf/print in callback (very slow!)
- Use DMA for file/network I/O
- Process in chunks if possible
- Use hardware accelerators (FFT, crypto engines)
- Consider offloading to second CPU core

### 4. Interrupt vs Polling

Current implementation uses **polling** (no interrupts). This is simpler but uses more CPU.

To use interrupts:
1. Enable DMA interrupts in driver init
2. Set IRQ handler in callback
3. Use semaphore/event to wake processing thread

## Troubleshooting

### Issue: Buffer Overruns

**Symptoms:**
```
Buffer overrun detected! Buffer 0 not consumed in time
Processing is too slow
```

**Causes:**
- Callback takes too long
- Sample rate too high
- Buffers too small

**Solutions:**
1. Profile callback with timer
2. Increase `STREAMING_BUFFER_SIZE_SAMPLES`
3. Use 3-4 buffers instead of 2
4. Reduce sample rate
5. Optimize processing code

### Issue: DMA Errors

**Symptoms:**
```
DMA transfer failed for buffer 1: -1
DMA errors: 5
```

**Causes:**
- Memory address invalid
- Buffer not cache-aligned
- DMA configuration mismatch with FPGA

**Solutions:**
1. Check buffer allocation succeeded
2. Verify cache alignment
3. Rebuild FPGA with correct DMA settings
4. Check `xparameters.h` addresses match FPGA

### Issue: Incorrect Data Values

**Symptoms:**
- All zeros or all 0xFFFFFFFF
- Random garbage
- Data not changing

**Causes:**
- Cache coherency issue
- Incorrect data format parsing
- ADC not configured properly

**Solutions:**
1. Verify `Xil_DCacheInvalidateRange()` calls
2. Check 24-bit extraction code
3. Verify AD4134 SPI configuration
4. Use ILA to verify SPI data in FPGA

### Issue: Low Sample Rate

**Symptoms:**
- Actual rate << expected rate
- Statistics show sample rate too low

**Causes:**
- PWM period wrong
- SPI clock too slow
- Processing bottleneck

**Solutions:**
1. Check PWM PULSE_0_PERIOD register
2. Verify SPI Engine clock = 100 MHz
3. Profile callback execution time
4. Check for software delays

## Advanced Features

### Using 3-4 Buffers (Circular Queue)

For more processing headroom:

```c
#define STREAMING_NUM_BUFFERS  4  /* Instead of 2 */
```

This gives you 3× the processing time:
- Buffer 0: Being filled by DMA
- Buffer 1: Being processed
- Buffer 2: Ready for processing (slack)
- Buffer 3: Ready for processing (more slack)

### Partial Buffer Processing

For very large buffers, process in chunks:

```c
void chunked_callback(uint32_t *data, uint32_t num_samples, void *user_data)
{
    #define CHUNK_SIZE 1024

    for (uint32_t offset = 0; offset < num_samples; offset += CHUNK_SIZE) {
        uint32_t chunk_samples = (offset + CHUNK_SIZE > num_samples) ?
                                  (num_samples - offset) : CHUNK_SIZE;

        process_chunk(&data[offset * 4], chunk_samples);
    }
}
```

### Multi-Core Processing (Zynq dual-core)

Use second ARM core for processing:

1. Copy buffer pointer to shared memory
2. Wake up Core 1 via SGI interrupt
3. Core 1 processes while Core 0 manages DMA
4. Synchronize via spinlock/mutex

## Comparison: Before vs After

### Before (Single-Shot DMA)

```
Capture 10 samples → Process → Print → Delay → Repeat

Timeline:
[DMA: 8μs] [Process: 1ms] [Dead time]
                          ^^^^^^^^^^^
                          Data lost here!

Sample rate: Inconsistent, gaps
Throughput: ~100 samples/sec
Data loss: Significant
```

### After (Continuous Streaming)

```
Buffer A: [DMA: 445ms] [Process: 300ms]
Buffer B:              [DMA: 445ms]     [Process: 300ms]
          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
          Zero gaps - always capturing!

Sample rate: 1.176 MHz continuous
Throughput: 1.176M samples/sec × 4 channels = 4.7M samples/sec
Data loss: ZERO (if processing < buffer fill time)
```

## Performance Metrics

With default configuration (512K samples/buffer, 1.176 MHz):

| Metric | Value |
|--------|-------|
| Sample Rate | 1.176 MHz |
| Channels | 4 |
| Total Data Rate | 75.264 Mbps |
| Buffer Fill Time | 445 ms |
| Processing Budget | < 445 ms |
| Memory Bandwidth | 18.8 MB/s |
| Minimum RAM | 16.8 MB (2 buffers) |

## References

- **FPGA Changes:** `/hdl/projects/ad4134_fmc/CONTINUOUS_STREAMING_GUIDE.md`
- **SPI Engine Docs:** `/hdl/docs/library/spi_engine/`
- **AXI DMAC Docs:** `/hdl/docs/regmap/adi_regmap_dmac.txt`
- **AD4134 Datasheet:** Analog Devices website
- **no-OS Wiki:** https://wiki.analog.com/resources/tools-software/uc-drivers

## License

Copyright 2024 Stanford Readout Project

Redistribution and use in source and binary forms, with or without
modification, are permitted.

---

## Optimization History

### January 2026 - ODR Synchronization Optimizations

**Objective:** Eliminate dead time between captures and achieve perfect synchronization where actual sample rate matches the ODR setting.

#### Changes Made

**1. SPI Engine Clock Speed Increase (100 MHz)**
- **File:** [ad4134_continuous_streaming.c:914](src/ad4134_continuous_streaming.c#L914)
- **Change:** `max_speed_hz = 80000000` → `max_speed_hz = 100000000`
- **Impact:** Increased sample rate from 395 kHz → 476 kHz (at 80 MHz) → **~500 kHz expected (at 100 MHz)**
- **Rationale:** Hardware provides 100 MHz clock; utilizing full capability to match 500 kHz ODR target (2000ns period)

**2. DMA Cyclic Mode**
- **File:** [ad4134_continuous_streaming.c:956](src/ad4134_continuous_streaming.c#L956)
- **Change:** `spi_eng_dma_flg = DMA_LAST` → `spi_eng_dma_flg = DMA_CYCLIC`
- **Impact:** Enables hardware-managed automatic buffer cycling, eliminates software intervention overhead
- **Benefit:** Reduces idle gap between buffer switches

**3. Cache Invalidation Optimization**
- **File:** [ad4134_continuous_streaming.c:533](src/ad4134_continuous_streaming.c#L533)
- **Location:** Cache invalidation positioned AFTER DMA start, BEFORE buffer switch
- **Impact:** Allows cache invalidation to overlap with next DMA transfer filling
- **Benefit:** Reduces idle gap by ~150-200 μs

**4. Critical Path Optimization**
- **File:** [ad4134_continuous_streaming.c:455-478](src/ad4134_continuous_streaming.c#L455-L478)
- **Change:** Moved statistics calculation and error checking AFTER DMA start
- **Impact:** Only essential operations remain before `spi_engine_offload_transfer()`
- **Benefit:** Minimizes time to start next DMA transfer

#### Performance Targets

| Metric | Before (80 MHz) | After (100 MHz) | Target |
|--------|-----------------|-----------------|--------|
| SPI Clock | 80 MHz | 100 MHz | 100 MHz |
| Sample Rate | 476 kHz | ~500 kHz | 500 kHz |
| ODR Period | 2000 ns | 2000 ns | 2000 ns |
| Sample Loss | 4.8% | ~0% | 0% |
| Idle Gap | 382-414 μs | <300 μs | Minimal |
| DMA Period | 34.4 ms | ~32.8 ms | - |
| DMA Efficiency | 98.8% | >99% | >99% |
| Buffer Overruns | 0 | 0 | 0 |

#### Configuration Details

**PWM ODR Settings:**
- Period: 2000 ns (500 kHz target rate)
- Matches AD4134 datasheet requirement for synchronous operation

**Buffer Configuration:**
- Size: 16K samples (defined in [parameters.h:38](src/parameters.h#L38))
- Number of buffers: 2 (ping-pong)
- Cache alignment: 64 bytes

**HDL Configuration:**
- CYCLIC mode: Enabled (hdl/projects/ad4134_fmc/common/ad4134_bd.tcl)
- DMA_2D_TRANSFER: Enabled for ping-pong buffering

#### Expected Results

With 100 MHz SPI Engine clock:
- **Perfect ODR synchronization:** Actual sample rate = 500 kHz = 1/(2000 ns)
- **Zero sample loss:** Every ADC conversion captured
- **Minimal idle gap:** Hardware cyclic DMA eliminates most software overhead
- **Continuous streaming:** No dead time between buffer transfers

---

**For questions or issues, please contact the project maintainer.**
