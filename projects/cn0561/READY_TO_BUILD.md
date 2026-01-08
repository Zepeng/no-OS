# ✅ Ready to Build - AD4134 Step 1

## Status: All Files Updated

The CN0561 project has been **directly modified** and is ready to build for AD4134 Step 1 testing.

## What Was Done

### ✅ HDL Files (Modified Earlier)
- [x] `projects/ad4134_fmc/common/ad4134_bd.tcl` - DMA removed, ILA added
- [x] `projects/ad4134_fmc/zed/system_top.v` - ILA probes connected

### ✅ no-OS Files (Just Updated)
- [x] `/no-OS/projects/cn0561/src/parameters.h` - AD4134 addresses, DMA commented out
- [x] `/no-OS/projects/cn0561/src/cn0561.c` - Step 1 mode with status monitoring

## Build Now

### 1. Build HDL (~10-30 minutes)
```bash
cd /Users/zepengli/work/StanfordReadout/hdl_fork/projects/ad4134_fmc/zed
make clean
make
```

Output: `ad4134_fmc_zed.bit` (with ILA, no DMA)

### 2. Build no-OS (~1-5 minutes)
```bash
cd /Users/zepengli/work/StanfordReadout/no-OS/projects/cn0561
make clean
make
```

Output: `cn0561.elf` (Step 1 configuration mode)

### 3. Test
1. Program FPGA with bitstream
2. Open Vivado Hardware Manager (ILA)
3. Open serial console (115200 baud)
4. Run ELF file
5. Observe both outputs!

## Expected Serial Output

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
```

## Expected ILA Waveforms

- **DCLK**: Toggling at ~9.6 MHz
- **ODR**: Pulses every 85 DCLK cycles
- **DOUT[3:0]**: Serial data streams (all 4 channels active)

## Success Checklist

- [ ] HDL builds without errors
- [ ] no-OS builds without errors
- [ ] FPGA programs successfully
- [ ] Serial shows "CHIP_TYPE: 0x40 [OK]"
- [ ] Serial shows "Status: 0x01 [READY]"
- [ ] ILA detects and connects
- [ ] ILA shows DCLK toggling
- [ ] ILA shows ODR pulses
- [ ] ILA shows DOUT activity

## Files Modified Summary

| File | Location | Change |
|------|----------|--------|
| ad4134_bd.tcl | hdl_fork/projects/ad4134_fmc/common/ | DMA removed, ILA added |
| system_top.v | hdl_fork/projects/ad4134_fmc/zed/ | ILA probes connected |
| parameters.h | no-OS/projects/cn0561/src/ | AD4134 addrs, DMA off |
| cn0561.c | no-OS/projects/cn0561/src/ | Step 1 mode added |

## Documentation Available

| File | Purpose |
|------|---------|
| **READY_TO_BUILD.md** | This file - quick start |
| **CN0561_STEP1_CHANGES.md** | Detailed change log |
| **STEP1_QUICK_START.md** | Quick reference card |
| **README_STEP1.md** | Complete guide with ILA |
| **STEP1_MODIFICATIONS.md** | HDL changes explained |
| **STEP1_SUMMARY.md** | Technical overview |

## Switching to Step 3 Later

When ready for full streaming with DMA:

**Edit `cn0561.c` line 67:**
```c
#define STEP1_CONFIG_ONLY  0  // Change 1 to 0
```

**Edit `parameters.h` line 42:**
```c
#define CN0561_DMA_BASEADDR  XPAR_AXI_CN0561_DMA_BASEADDR  // Uncomment
```

**Rebuild both HDL and no-OS with Step 3 modifications**

## Quick Commands

```bash
# Build everything
cd /Users/zepengli/work/StanfordReadout/hdl_fork/projects/ad4134_fmc/zed && make clean && make
cd /Users/zepengli/work/StanfordReadout/no-OS/projects/cn0561 && make clean && make

# Open serial console
screen /dev/ttyUSB1 115200

# Or minicom
minicom -D /dev/ttyUSB1 -b 115200
```

## Troubleshooting

### Build Error: XPAR_AXI_CN0561_CLKGEN_BASEADDR not found
**Solution:** HDL not built yet or xparameters.h not found. Build HDL first.

### Build Error: CN0561_DMA_BASEADDR undefined
**Solution:** This is expected and OK! It's commented out for Step 1.

### Serial: CHIP_TYPE shows 0xFF
**Solution:** Check SPI connections, ADC power, FPGA programmed correctly

### ILA: Not detected
**Solution:** Ensure you programmed the Step 1 bitstream with ILA enabled

## You're Ready!

Everything is set up and modified. Just run the build commands above and test!

---

**Next:** After successful Step 1 validation, proceed to Step 3 (custom capture module + DMA)
