# Building the AD4134 Continuous Streaming Project

## Prerequisites

1. **FPGA Bitstream**: You must first rebuild the HDL project with the modified `ad4134_bd.tcl` that enables cyclic DMA mode.

2. **Xilinx SDK/Vitis**: Required for building the no-OS application

3. **ARM Toolchain**: Cross-compiler for Zynq-7000 (arm-none-eabi-gcc)

## Step 1: Rebuild FPGA Design

```bash
cd /Users/zepengli/work/StanfordReadout/hdl
make ad4134_fmc.zed
```

This will generate a new bitstream with:
- Cyclic DMA mode enabled
- 2D transfer support
- Optimized burst configuration

The output will be in:
```
projects/ad4134_fmc/zed/ad4134_fmc_zed.runs/impl_1/system_top.bit
```

## Step 2: Option A - Build Streaming Example (Recommended)

To build the **new continuous streaming example** application:

### Modify src.mk

Add the streaming source files to `/Users/zepengli/work/StanfordReadout/no-OS/projects/ad4134_fmcz/src.mk`:

```makefile
# Add after line 12 (SRC_DIRS += $(PROJECT)/src)

# Continuous streaming support
SRCS += $(PROJECT)/src/ad4134_continuous_streaming.c
INCS += $(PROJECT)/src/ad4134_continuous_streaming.h

# Choose which main to build:
# Option 1: Streaming example (recommended)
SRCS += $(PROJECT)/src/ad713x_fmc_streaming_example.c

# Option 2: Original example (comment out the line above and uncomment below)
# SRCS += $(PROJECT)/src/ad713x_fmc.c
```

### Build Command

```bash
cd /Users/zepengli/work/StanfordReadout/no-OS/projects/ad4134_fmcz
make PLATFORM=xilinx TARGET=zed
```

## Step 2: Option B - Integrate into Existing Application

If you want to add streaming to your existing `ad713x_fmc.c`:

### 1. Add Includes

At the top of `ad713x_fmc.c`:
```c
#include "ad4134_continuous_streaming.h"
```

### 2. Define Callback

Add your data processing function:
```c
void my_data_callback(uint32_t *data, uint32_t num_samples, void *user_data)
{
    /* Your processing here */
    float lsb = 8.192f / 16777216.0f;

    for (uint32_t i = 0; i < num_samples; i++) {
        for (int ch = 0; ch < 4; ch++) {
            int32_t raw = data[i * 4 + ch] & 0xFFFFFF00;
            raw >>= 8;
            float voltage = lsb * raw;
            if (voltage > 4.095f) voltage -= 8.192f;

            /* Process voltage... */
        }
    }
}
```

### 3. Replace Main Loop

Replace the existing `while(1)` loop (around line 396-428) with:

```c
/* Initialize streaming */
struct streaming_context *streaming_ctx;
struct streaming_init_param streaming_init = {
    .spi_desc = spi_eng_desc,
    .offload_msg = &spi_engine_offload_message,
    .samples_per_transfer = STREAMING_BUFFER_SIZE_SAMPLES,
    .data_callback = my_data_callback,
    .user_data = NULL
};

ret = streaming_init(&streaming_ctx, &streaming_init);
if (ret != 0) {
    pr_err("Failed to initialize streaming\n");
    return ret;
}

ret = streaming_start(streaming_ctx);
if (ret != 0) {
    pr_err("Failed to start streaming\n");
    return ret;
}

/* Main streaming loop */
while (1) {
    ret = streaming_process(streaming_ctx);
    if (ret != 0) {
        break;  /* Error or stop requested */
    }
}

/* Cleanup */
streaming_stop(streaming_ctx);
streaming_remove(streaming_ctx);
```

### 4. Update src.mk

```makefile
# Add streaming support
SRCS += $(PROJECT)/src/ad4134_continuous_streaming.c
INCS += $(PROJECT)/src/ad4134_continuous_streaming.h
```

### 5. Build

```bash
cd /Users/zepengli/work/StanfordReadout/no-OS/projects/ad4134_fmcz
make PLATFORM=xilinx TARGET=zed
```

## Step 3: Program and Run

### Program FPGA

Using Vivado Hardware Manager or JTAG:

```bash
# Program FPGA with new bitstream
vivado -mode batch -source program_fpga.tcl
```

Or via Vitis:
1. Open Vitis workspace
2. Xilinx → Program FPGA
3. Select `system_top.bit`
4. Program

### Load Application

#### Option A: JTAG Download (for debugging)

In Vitis:
1. Right-click project → Debug As → Launch on Hardware
2. Application loads to DDR and runs

#### Option B: SD Card Boot (for standalone)

1. Copy files to SD card FAT32 partition:
   ```
   BOOT.bin          (FSBL + bitstream + application)
   ```

2. Create BOOT.bin:
   ```bash
   # In Vitis, create boot image:
   # Xilinx → Create Boot Image
   # Add: fsbl.elf, system_top.bit, application.elf
   ```

3. Insert SD card and power on board

### Monitor Output

Connect serial terminal (115200 baud, 8N1):
```bash
# Linux
screen /dev/ttyUSB0 115200

# Windows
putty.exe -serial COM3 -sercfg 115200,8,n,1,N

# macOS
screen /dev/tty.usbserial-* 115200
```

Expected output:
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

Streaming started

========================================
Continuous streaming active!
Processing buffers...
========================================

First 10 samples from buffer 1:
Sample 0: 0x12345600, 0x23456700, 0x34567800, 0x45678900
...

=== Streaming Statistics ===
Runtime: 10000 ms (10.00 seconds)
Total samples: 11755520
Total buffers: 20
Sample rate: 1175552 samples/sec
Buffer overruns: 0
DMA errors: 0
Max processing time: 234 ms
Avg processing time: 187 ms
SUCCESS: No buffer overruns - continuous streaming working!
===========================
```

## Build Configurations

### Debug Build

```bash
make PLATFORM=xilinx TARGET=zed BUILD=debug
```

Features:
- Debug symbols included
- No optimization (-O0)
- Assertions enabled
- Larger binary size

### Release Build (Default)

```bash
make PLATFORM=xilinx TARGET=zed BUILD=release
```

Features:
- Optimized (-O2)
- Debug symbols stripped
- Smaller, faster binary

## Customization

### Adjust Buffer Size

Edit `src/ad4134_continuous_streaming.h`:

```c
/* Increase for slower processing */
#define STREAMING_BUFFER_SIZE_SAMPLES   (1024 * 1024)  /* 1M samples */

/* Use more buffers for extra margin */
#define STREAMING_NUM_BUFFERS           3
```

### Change Sample Rate

Edit FPGA parameters and rebuild:

In `hdl/projects/ad4134_fmc/common/ad4134_bd.tcl`:
```tcl
ad_ip_parameter odr_generator CONFIG.PULSE_0_PERIOD 200  # 500 kHz
```

Then rebuild:
```bash
cd /Users/zepengli/work/StanfordReadout/hdl
make ad4134_fmc.zed
```

### Enable IIO Support (Optional)

For remote control via libiio:

```bash
make PLATFORM=xilinx TARGET=zed IIOD=y
```

This enables network control but adds overhead.

## Troubleshooting Build Issues

### Issue: undefined reference to 'streaming_init'

**Cause**: Forgot to add streaming source files to src.mk

**Solution**: Add to src.mk:
```makefile
SRCS += $(PROJECT)/src/ad4134_continuous_streaming.c
```

### Issue: Xil_DCacheInvalidateRange undefined

**Cause**: Missing Xilinx BSP libraries

**Solution**: Ensure Xilinx BSP is in include path:
```makefile
INCS += $(XILINX_SDK)/data/embeddedsw/lib/bsp/standalone_v*/src
```

### Issue: Multiple definition of 'main'

**Cause**: Both `ad713x_fmc.c` and `ad713x_fmc_streaming_example.c` in build

**Solution**: Choose one main source file in src.mk

### Issue: fatal error: xparameters.h: No such file

**Cause**: Hardware platform not exported from Vivado

**Solution**:
1. Open Vivado project
2. File → Export → Export Hardware (include bitstream)
3. Refresh hardware platform in Vitis

## Clean Build

If you encounter strange build errors:

```bash
make clean
make PLATFORM=xilinx TARGET=zed
```

## Performance Verification

After successful build and run, verify continuous streaming:

1. **Check Statistics Output**
   - Buffer overruns should be 0
   - Sample rate should match expected (~1.176 MHz)
   - No DMA errors

2. **Verify Data Integrity**
   - If using test pattern mode, check sample counter increments
   - Check voltage ranges are reasonable
   - Look for discontinuities

3. **Monitor Resource Usage**
   - CPU usage via top/htop (if running Linux)
   - Memory bandwidth via performance counters
   - FPGA temperature and power

## Next Steps

After successful build:

1. **Test with Real Signals**: Connect analog inputs to AD4134
2. **Implement Your Processing**: Modify callback function
3. **Optimize Performance**: Profile and tune buffer sizes
4. **Add Data Output**: Implement file writing or network streaming
5. **Deploy**: Create production boot image

## Support Files Location

All files are in `/Users/zepengli/work/StanfordReadout/no-OS/projects/ad4134_fmcz/src/`:

- `ad4134_continuous_streaming.h` - API header
- `ad4134_continuous_streaming.c` - Implementation
- `ad713x_fmc_streaming_example.c` - Example application
- `CONTINUOUS_STREAMING_README.md` - Detailed documentation

FPGA modifications in `/Users/zepengli/work/StanfordReadout/hdl/`:

- `projects/ad4134_fmc/common/ad4134_bd.tcl` - Modified DMA config
- `projects/ad4134_fmc/CONTINUOUS_STREAMING_GUIDE.md` - FPGA guide

## Questions?

See `CONTINUOUS_STREAMING_README.md` for detailed API documentation and troubleshooting.
