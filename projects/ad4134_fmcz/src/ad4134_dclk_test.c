/***************************************************************************//**
 * @file ad4134_dclk_test.c
 * @brief AD4134 DCLK Slave Mode Test Application
 * @authors Modified for DCLK slave mode testing
********************************************************************************
 * Copyright 2024(c) Analog Devices, Inc.
 *
 * Test application for single AD4134 in DCLK slave mode where FPGA
 * generates DCLK and ODR signals.
*******************************************************************************/

#include <stdio.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <xil_cache.h>
#include <xparameters.h>
#include "xil_printf.h"
#include "no_os_spi.h"
#include "xilinx_spi.h"
#include "no_os_delay.h"
#include "no_os_gpio.h"
#include "xilinx_gpio.h"
#include "no_os_util.h"
#include "no_os_error.h"
#include "parameters.h"
#include "axi_dmac.h"
#include "ad713x.h"

// Base addresses (from ad4134_bd_single_test.tcl)
#define GPIO_CTRL_BASEADDR     0x44a00000  // AXI GPIO for enable control
#define DMA_BASEADDR           0x44a30000  // AXI DMAC
#define SPI_DEVICE_ID          0           // PS SPI for AD4134 config

// Buffer configuration
// 1 ADC × 4 channels × 24-bit = 96 bits = 12 bytes per sample
#define SAMPLES_PER_TRANSFER   1000
#define BYTES_PER_SAMPLE       16          // Padded to 128-bit (DMA width)
#define BUFFER_SIZE           (SAMPLES_PER_TRANSFER * BYTES_PER_SAMPLE)

static uint8_t adc_buffer[BUFFER_SIZE] __attribute__((aligned(1024)));

int main()
{
	int32_t ret;
	uint32_t i;
	struct ad713x_dev *ad4134_dev;
	struct ad713x_init_param ad4134_init_param;
	struct axi_dmac *dma_desc;
	struct axi_dmac_init dma_init;
	struct no_os_gpio_desc *gpio_enable;
	struct no_os_gpio_desc *gpio_busy;
	struct no_os_gpio_init_param gpio_init_param;
	struct xil_gpio_init_param gpio_extra_param;

	// GPIO init params for AD4134
	struct no_os_gpio_init_param ad4134_resetn_param;
	struct no_os_gpio_init_param ad4134_mode_param;
	struct no_os_gpio_init_param ad4134_dclkio_param;
	struct no_os_gpio_init_param ad4134_dclkmode_param;
	struct no_os_gpio_init_param ad4134_pdn_param;

	struct no_os_gpio_desc *gpio_resetn;
	struct no_os_gpio_desc *gpio_mode;
	struct no_os_gpio_desc *gpio_dclkio;
	struct no_os_gpio_desc *gpio_dclkmode;
	struct no_os_gpio_desc *gpio_pdn;

	// SPI init params for AD4134 configuration
	static struct xil_spi_init_param spi_init_params = {
		.type = SPI_PS,
		.flags = 0
	};

	Xil_ICacheEnable();
	Xil_DCacheEnable();

	printf("\n\n========================================\n");
	printf("AD4134 DCLK Slave Mode Test\n");
	printf("FPGA Master - Single ADC\n");
	printf("========================================\n\n");

	// =========================================================================
	// Step 1: Initialize GPIO
	// =========================================================================

	printf("Step 1: Initializing GPIO...\n");

	gpio_extra_param.device_id = GPIO_DEVICE_ID;
	gpio_extra_param.type = GPIO_PS;

	// GPIO for enable control (connects to FPGA module)
	gpio_init_param.number = 0;  // Adjust based on your GPIO mapping
	gpio_init_param.platform_ops = &xil_gpio_ops;
	gpio_init_param.extra = &gpio_extra_param;

	// Initialize AD4134 GPIO pins
	ad4134_resetn_param = (struct no_os_gpio_init_param) {
		.number = GPIO_RESETN_1,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};

	ad4134_mode_param = (struct no_os_gpio_init_param) {
		.number = GPIO_MODE_1,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};

	ad4134_dclkio_param = (struct no_os_gpio_init_param) {
		.number = GPIO_DCLKIO_1,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};

	ad4134_dclkmode_param = (struct no_os_gpio_init_param) {
		.number = GPIO_DCLKMODE,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};

	ad4134_pdn_param = (struct no_os_gpio_init_param) {
		.number = GPIO_PDN_1,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};

	ret = no_os_gpio_get(&gpio_resetn, &ad4134_resetn_param);
	ret |= no_os_gpio_get(&gpio_mode, &ad4134_mode_param);
	ret |= no_os_gpio_get(&gpio_dclkio, &ad4134_dclkio_param);
	ret |= no_os_gpio_get(&gpio_dclkmode, &ad4134_dclkmode_param);
	ret |= no_os_gpio_get(&gpio_pdn, &ad4134_pdn_param);

	if (ret != 0) {
		printf("ERROR: GPIO initialization failed!\n");
		return -1;
	}

	// Configure GPIOs as outputs
	no_os_gpio_direction_output(gpio_resetn, 1);
	no_os_gpio_direction_output(gpio_mode, 0);
	no_os_gpio_direction_output(gpio_dclkio, 0);
	no_os_gpio_direction_output(gpio_dclkmode, 0);
	no_os_gpio_direction_output(gpio_pdn, 1);

	printf("  GPIO initialized\n\n");

	// =========================================================================
	// Step 2: Configure AD4134 for DCLK Slave Mode
	// =========================================================================

	printf("Step 2: Configuring AD4134 for DCLK slave mode...\n");

	// Initialize AD4134 via SPI (for register configuration only)
	ad4134_init_param.adc_data_len = ADC_24_BIT_DATA;
	ad4134_init_param.dev_id = ID_AD4134;
	ad4134_init_param.format = QUAD_CH_PO;
	ad4134_init_param.crc_header = CRC_6;
	ad4134_init_param.clk_delay_en = false;

	// IMPORTANT: Configure for SLAVE mode (FPGA generates DCLK)
	ad4134_init_param.mode_master_nslave = true;      // TRUE = Slave mode!
	ad4134_init_param.dclkmode_free_ngated = false;   // Gated mode
	ad4134_init_param.dclkio_out_nin = true;          // DCLK is INPUT to ADC
	ad4134_init_param.pnd = true;

	// SPI configuration
	ad4134_init_param.spi_init_prm.chip_select = AD4134_1_SPI_CS;
	ad4134_init_param.spi_init_prm.device_id = SPI_DEVICE_ID;
	ad4134_init_param.spi_init_prm.max_speed_hz = 10000000;
	ad4134_init_param.spi_init_prm.mode = NO_OS_SPI_MODE_0;
	ad4134_init_param.spi_init_prm.platform_ops = &xil_spi_ops;
	ad4134_init_param.spi_init_prm.extra = (void *)&spi_init_params;

	// GPIO assignments
	ad4134_init_param.gpio_resetn = &ad4134_resetn_param;
	ad4134_init_param.gpio_mode = &ad4134_mode_param;
	ad4134_init_param.gpio_dclkio = &ad4134_dclkio_param;
	ad4134_init_param.gpio_dclkmode = &ad4134_dclkmode_param;
	ad4134_init_param.gpio_pnd = &ad4134_pdn_param;
	ad4134_init_param.spi_common_dev = 0;

	ret = ad713x_init(&ad4134_dev, &ad4134_init_param);
	if (ret != 0) {
		printf("ERROR: AD4134 initialization failed!\n");
		return -1;
	}

	// Set MODE pin HIGH to enable DCLK slave mode
	no_os_gpio_set_value(gpio_mode, 1);
	printf("  MODE pin set HIGH (DCLK slave mode enabled)\n");

	// Configure digital filters
	for (i = 0; i <= 3; i++) {
		ret = ad713x_dig_filter_sel_ch(ad4134_dev, SINC3, i);
		if (ret != 0) {
			printf("ERROR: Filter configuration failed for channel %d\n", i);
			return -1;
		}
	}

	printf("  AD4134 configured successfully\n\n");

	no_os_mdelay(100);

	// =========================================================================
	// Step 3: Initialize DMA
	// =========================================================================

	printf("Step 3: Initializing DMA...\n");

	dma_init.name = "ad4134_dma";
	dma_init.base = DMA_BASEADDR;
	dma_init.irq_option = IRQ_DISABLED;

	ret = axi_dmac_init(&dma_desc, &dma_init);
	if (ret != 0) {
		printf("ERROR: DMA initialization failed!\n");
		return -1;
	}

	printf("  DMA initialized at 0x%08X\n\n", DMA_BASEADDR);

	// =========================================================================
	// Step 4: Enable FPGA DCLK/ODR Generation
	// =========================================================================

	printf("Step 4: Enabling FPGA DCLK/ODR generation...\n");

	// Write to GPIO control register to enable streaming
	// This sets the 'enable' input on the ad4134_multi_adc_slave module
	uint32_t *gpio_ctrl = (uint32_t *)GPIO_CTRL_BASEADDR;
	*gpio_ctrl = 0x00000001;  // Enable bit

	printf("  FPGA now generating:\n");
	printf("    DCLK @ 12 MHz\n");
	printf("    ODR @ 500 kHz\n\n");

	// =========================================================================
	// Step 5: Capture Data via DMA
	// =========================================================================

	printf("Step 5: Capturing data (10 transfers)...\n\n");

	for (uint32_t transfer = 0; transfer < 10; transfer++) {
		// Start DMA transfer
		struct axi_dma_transfer dma_transfer = {
			.size = BUFFER_SIZE,
			.transfer_done = 0,
			.src_addr = 0,
			.dest_addr = (uintptr_t)adc_buffer
		};

		ret = axi_dmac_transfer_start(dma_desc, &dma_transfer);
		if (ret != 0) {
			printf("ERROR: DMA transfer start failed!\n");
			return -1;
		}

		// Wait for transfer to complete (timeout 1000 ms)
		ret = axi_dmac_transfer_wait_completion(dma_desc, 1000);
		if (ret != 0) {
			printf("ERROR: DMA transfer timeout!\n");
			return -1;
		}

		// Invalidate cache
		Xil_DCacheInvalidateRange((INTPTR)adc_buffer, BUFFER_SIZE);

		printf("Transfer %d: Captured %d samples\n", transfer + 1, SAMPLES_PER_TRANSFER);

		// Print first sample of first transfer
		if (transfer == 0) {
			printf("\nFirst sample data (raw bytes):\n");
			for (i = 0; i < 16; i++) {
				printf("%02X ", adc_buffer[i]);
				if ((i + 1) % 4 == 0) printf(" ");
			}
			printf("\n\n");
		}
	}

	printf("\nTest complete!\n");
	printf("========================================\n\n");

	// Cleanup
	*gpio_ctrl = 0x00000000;  // Disable streaming
	ad713x_remove(ad4134_dev);
	axi_dmac_remove(dma_desc);

	Xil_DCacheDisable();
	Xil_ICacheDisable();

	return 0;
}
