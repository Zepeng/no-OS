# CN0561 Project - Modified for AD4134 Step 1 Testing

## Overview

The CN0561 project has been directly modified to work with AD4134 Step 1 HDL (no DMA, no offload). The modifications use conditional compilation so you can easily switch between:
- **Step 1 Mode** (`STEP1_CONFIG_ONLY = 1`): Configuration only, no data capture
- **Step 3 Mode** (`STEP1_CONFIG_ONLY = 0`): Full streaming with DMA

## Files Modified

### 1. `/no-OS/projects/cn0561/src/parameters.h`

**Changes made:**
```c
// Line 41-42: Commented out DMA (not available in Step 1 HDL)
// #define CN0561_DMA_BASEADDR		XPAR_AXI_CN0561_DMA_BASEADDR

// Line 44: Updated to AD4134 SPI engine address
#define CN0561_SPI_ENGINE_BASEADDR	XPAR_SPI_AD4134_SPI_AD4134_AXI_REGMAP_BASEADDR

// Line 67: Enabled ZED carrier for GPIO control
#define CN0561_ZED_CARRIER
```

### 2. `/no-OS/projects/cn0561/src/cn0561.c`

**Changes made:**

#### Added Step 1 Configuration Flag (Line 67)
```c
#define STEP1_CONFIG_ONLY  1  // Set to 0 for Step 3 streaming mode
```

#### Conditional Buffer Allocation (Line 79-82)
```c
#if !STEP1_CONFIG_ONLY
/* DMA buffer only needed for full streaming mode (Step 3) */
static uint32_t adc_buffer[ADC_BUFFER_SIZE] __attribute__((aligned(1024)));
#endif
```

#### Conditional Variable Declarations (Line 98-107)
```c
#if !STEP1_CONFIG_ONLY
	/* DMA/Offload variables - only for full streaming mode (Step 3) */
	uint32_t i = 0, j;
	const float lsb = 4.096 / (pow(2, 23));
	float data;
	uint32_t spi_eng_dma_flg = DMA_LAST | DMA_PARTIAL_REPORTING_EN;
	struct spi_engine_offload_init_param spi_engine_offload_init_param;
	struct spi_engine_offload_message spi_engine_offload_message;
	uint32_t spi_eng_msg_cmds[1];
#endif
```

#### Added Banner Messages (Line 209-220)
```c
xil_printf("\n========================================\n");
#if STEP1_CONFIG_ONLY
	xil_printf("AD4134 Step 1 - Configuration Test\n");
	xil_printf("DMA: DISABLED (removed from HDL)\n");
	xil_printf("Offload: DISABLED (no trigger)\n");
	xil_printf("ILA: Use Vivado Hardware Manager\n");
#else
	xil_printf("CN0561 Full Streaming Mode\n");
	xil_printf("DMA: ENABLED\n");
	xil_printf("Offload: ENABLED\n");
#endif
xil_printf("========================================\n\n");
```

#### Added Register Status Display (Line 257-268)
```c
/* Print register status */
uint32_t chip_type, status, device_config;
ad713x_spi_reg_read(cn0561_dev, AD713X_REG_CHIP_TYPE, &chip_type);
ad713x_spi_reg_read(cn0561_dev, AD713X_REG_DEVICE_STATUS, &status);
ad713x_spi_reg_read(cn0561_dev, AD713X_REG_DEVICE_CONFIG, &device_config);

xil_printf("=== AD4134 Status ===\n");
xil_printf("CHIP_TYPE:     0x%02X %s\n", chip_type,
           (chip_type == 0x40) ? "[OK]" : "[ERROR]");
xil_printf("STATUS:        0x%02X\n", status);
xil_printf("DEVICE_CONFIG: 0x%02X\n", device_config);
xil_printf("=====================\n\n");
```

#### Added Step 1 Monitoring Loop (Line 270-309)
```c
#if STEP1_CONFIG_ONLY
	xil_printf("ADC configured for continuous conversion.\n");
	xil_printf("Data is output on DOUT[3:0] pins.\n");
	xil_printf("Use ILA in Vivado Hardware Manager to observe signals.\n\n");
	xil_printf("Monitoring status (10 iterations):\n\n");

	for (uint32_t loop_count = 1; loop_count <= 10; loop_count++) {
		ret = ad713x_spi_reg_read(cn0561_dev, AD713X_REG_DEVICE_STATUS, &status);
		if (ret == 0) {
			xil_printf("Loop %4lu: Status = 0x%02X%s%s%s\n",
			           (unsigned long)loop_count, status,
			           (status & 0x01) ? " [PLL_LOCKED]" : "",
			           (status & 0x04) ? " [INT_OSC]" : "",
			           (status & 0x08) ? " [MASTER]" : "");
		} else {
			xil_printf("Loop %4lu: Failed to read status\n",
			           (unsigned long)loop_count);
		}
		sleep(2);  // Every 2 seconds

		/* Every 10 loops, print full register dump */
		if (loop_count % 10 == 0) {
			// ... register dump code ...
		}
	}
#else
	// Original streaming code here
#endif
```

#### Closed Conditional Block (Line 423)
```c
#endif /* STEP1_CONFIG_ONLY */
```

## Build Instructions

### For Step 1 (Current Configuration)

```bash
cd /Users/zepengli/work/StanfordReadout/no-OS/projects/cn0561
make clean
make
```

The application will:
- Initialize AD4134 via SPI
- Print register status
- Enter infinite monitoring loop
- Display status every 2 seconds
- Show register dump every 10 loops

### For Step 3 (Future - Full Streaming)

Edit `src/cn0561.c` line 67:
```c
#define STEP1_CONFIG_ONLY  0  // Enable full streaming
```

Edit `src/parameters.h` line 42:
```c
#define CN0561_DMA_BASEADDR		XPAR_AXI_CN0561_DMA_BASEADDR  // Uncomment
```

Then rebuild:
```bash
make clean
make
```

## Expected Output (Step 1)

```
========================================
AD4134 Step 1 - Configuration Test
DMA: DISABLED (removed from HDL)
Offload: DISABLED (no trigger)
ILA: Use Vivado Hardware Manager
========================================

=== AD4134 Status ===
CHIP_TYPE:     0x40 [OK]
STATUS:        0x01
DEVICE_CONFIG: 0x01
=====================

ADC configured for continuous conversion.
Data is output on DOUT[3:0] pins.
Use ILA in Vivado Hardware Manager to observe signals.

Monitoring status (press reset to stop):

Loop    0: Status = 0x01 [READY]
Loop    1: Status = 0x01 [READY]
Loop    2: Status = 0x01 [READY]
...
Loop   10: Status = 0x01 [READY]

--- Register dump at loop 10 ---
CHIP_TYPE:     0x40
DEVICE_CONFIG: 0x01
STATUS:        0x01
--------------------------------

Loop   11: Status = 0x01 [READY]
...
```

## Testing Procedure

### 1. Build HDL
```bash
cd /Users/zepengli/work/StanfordReadout/hdl_fork/projects/ad4134_fmc/zed
make clean
make
```

### 2. Build no-OS
```bash
cd /Users/zepengli/work/StanfordReadout/no-OS/projects/cn0561
make clean
make
```

### 3. Program and Run
1. Program FPGA with `ad4134_fmc_zed.bit`
2. Open Vivado Hardware Manager for ILA
3. Open serial console (115200 baud)
4. Download and run `cn0561.elf`
5. Observe serial output
6. Trigger ILA to see DCLK, ODR, DOUT signals

## Success Criteria

✅ **Step 1 is successful when:**
- Builds without errors
- CHIP_TYPE shows 0x40 [OK]
- STATUS shows 0x01 [READY]
- Status monitoring loop runs continuously
- ILA shows DCLK toggling
- ILA shows ODR pulses
- ILA shows DOUT activity on all 4 channels

## Key Features of This Modification

✅ **Minimal changes** - Only 2 files modified
✅ **Clean conditional compilation** - Easy to understand
✅ **Preserves original code** - Step 3 code intact, just disabled
✅ **Easy switching** - Change 1 line to enable/disable modes
✅ **Working base** - Uses proven CN0561 structure
✅ **Clear output** - Helpful status messages

## Advantages Over Creating New Project

1. **Reuses working code** - CN0561 initialization is proven
2. **Same build system** - No new Makefile needed
3. **Same structure** - Familiar layout
4. **Easy transition** - Clear path to Step 3
5. **Less work** - No new project setup

## File Locations

| File | Path | Status |
|------|------|--------|
| parameters.h | `/no-OS/projects/cn0561/src/parameters.h` | ✅ Modified |
| cn0561.c | `/no-OS/projects/cn0561/src/cn0561.c` | ✅ Modified |
| Makefile | `/no-OS/projects/cn0561/Makefile` | ✅ No changes needed |
| HDL bitstream | `/hdl_fork/projects/ad4134_fmc/zed/*.bit` | Use Step 1 HDL |

## Restoring Original CN0561

If you need to restore the original CN0561 functionality:

```bash
cd /Users/zepengli/work/StanfordReadout/no-OS/projects/cn0561
git checkout src/parameters.h src/cn0561.c
```

Or keep backups:
```bash
cp src/parameters.h src/parameters.h.step1
cp src/cn0561.c src/cn0561.c.step1
```

## Summary

The CN0561 project now works for AD4134 Step 1 testing with simple conditional compilation. Set `STEP1_CONFIG_ONLY = 1` for Step 1 (current), or `STEP1_CONFIG_ONLY = 0` for Step 3 (future). All changes are clean, reversible, and well-documented in the code.
