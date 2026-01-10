/***************************************************************************//**
* @file cn0561.c
* @brief Implementation of Main Function.
* @authors Andrei Drimbarean (andrei.drimbarean@analog.com)
********************************************************************************
* Copyright 2022(c) Analog Devices, Inc.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice,
*    this list of conditions and the following disclaimer.
*
* 2. Redistributions in binary form must reproduce the above copyright notice,
*    this list of conditions and the following disclaimer in the documentation
*    and/or other materials provided with the distribution.
*
* 3. Neither the name of Analog Devices, Inc. nor the names of its
*    contributors may be used to endorse or promote products derived from this
*    software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY ANALOG DEVICES, INC. “AS IS” AND ANY EXPRESS OR
* IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
* EVENT SHALL ANALOG DEVICES, INC. BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
* OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
* LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************/

#include <stdio.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xil_cache.h>
#include <xparameters.h>
#include <math.h>
#include "xil_printf.h"
#include "spi_engine.h"
#include "ad713x.h"
#include "no_os_spi.h"
#include "xilinx_spi.h"
#include "no_os_delay.h"
#include "no_os_gpio.h"
#include "xilinx_gpio.h"
#include "no_os_util.h"
#include "no_os_error.h"
#include "parameters.h"
#include "no_os_pwm.h"
#include "axi_pwm_extra.h"
#include "clk_axi_clkgen.h"
#include "no_os_axi_io.h"
#include "axi_dmac.h"

/******************************************************************************/
/********************** Macros and Constants Definitions **********************/
/******************************************************************************/

/* AD4134 Step 1 Configuration Mode
 * Set to 1: Configuration only (no DMA, no offload) - for Step 1 testing
 * Set to 0: Full streaming mode (with DMA and offload) - for Step 3
 */
#define STEP1_CONFIG_ONLY  1
#define AXI_CLKGEN_REG_RESETN 0x40
#define AXI_CLKGEN_MMCM_RESETN (1U << 1)
#define AXI_CLKGEN_RESETN (1U << 0)

static void ad4134_slave_mode_checks(struct ad713x_dev *dev)
{
	uint8_t reg = 0;
	int32_t ret;

	xil_printf("\r\n=== AD4134 Slave Mode Checks ===\r\n");

	ret = ad713x_spi_reg_read(dev, AD713X_REG_DEVICE_STATUS, &reg);
	if (ret == 0) {
		xil_printf("DEVICE_STATUS: 0x%02X\r\n", reg);
		xil_printf("  PLL_LOCK:   %s\r\n",
		           (reg & AD713X_DEV_STAT_PLL_LOCK_MSK) ? "OK" : "FAIL");
		xil_printf("  MODE:       %s\r\n",
		           (reg & AD713X_DEV_STAT_MODE_MSK) ? "MASTER (FAIL)" : "SLAVE (OK)");
		xil_printf("  DCLKIO:     %s\r\n",
		           (reg & AD713X_DEV_STAT_DCLKIO_MSK) ? "OUTPUT (FAIL)" : "INPUT (OK)");
		xil_printf("  DCLKMODE:   %s\r\n",
		           (reg & AD713X_DEV_STAT_DCLKMODE_MSK) ? "FREE" : "GATED");
	} else {
		xil_printf("DEVICE_STATUS: read error\r\n");
	}

	ret = ad713x_spi_reg_read(dev, AD713X_REG_DEVICE_CONFIG, &reg);
	if (ret == 0) {
		xil_printf("DEVICE_CONFIG: 0x%02X\r\n", reg);
		xil_printf("  NO_CHIP_ERR: %s\r\n",
		           (reg & AD713X_DEV_CONFIG_NO_CHIP_ERR_MSK) ? "OK" : "FAIL");
		xil_printf("  OP_IN_PROG:  %s\r\n",
		           (reg & AD713X_DEV_CONFIG_OP_IN_PROGRESS_MSK) ? "BUSY" : "IDLE");
		xil_printf("  PWR_MODE:    %s\r\n",
		           (reg & AD713X_DEV_CONFIG_PWR_MODE_MSK) ? "NORMAL" : "LOW");
	} else {
		xil_printf("DEVICE_CONFIG: read error\r\n");
	}

	ret = ad713x_spi_reg_read(dev, AD713X_REG_DEVICE_CONFIG1, &reg);
	if (ret == 0) {
		xil_printf("DEVICE_CONFIG1: 0x%02X\r\n", reg);
		xil_printf("  CLKOUT_EN:   %s\r\n",
		           (reg & AD713X_DEV_CONFIG1_CLKOUT_EN_MSK) ? "OK" : "FAIL");
		xil_printf("  REF_GAIN_CORR: %s\r\n",
		           (reg & AD713X_DEV_CONFIG1_REF_GAIN_CORR_EN_MSK) ? "OK" : "FAIL");
	} else {
		xil_printf("DEVICE_CONFIG1: read error\r\n");
	}

	ret = ad713x_spi_reg_read(dev, AD713X_REG_DIGITAL_INTERFACE_CONFIG, &reg);
	if (ret == 0) {
		uint8_t fmt = reg & AD713X_DIG_INT_CONFIG_FORMAT_MSK;

		xil_printf("DIGITAL_INTERFACE_CONFIG: 0x%02X\r\n", reg);
		xil_printf("  FORMAT:     %s\r\n",
		           (fmt == AD713X_DIG_INT_CONFIG_FORMAT_MODE(QUAD_CH_PO)) ?
		           "QUAD_CH_PO (OK)" : "NOT QUAD_CH_PO");
	} else {
		xil_printf("DIGITAL_INTERFACE_CONFIG: read error\r\n");
	}

	ret = ad713x_spi_reg_read(dev, AD713X_REG_CHAN_DIG_FILTER_SEL, &reg);
	if (ret == 0) {
		xil_printf("CHAN_DIG_FILTER_SEL: 0x%02X\r\n", reg);
		for (uint8_t ch = 0; ch <= 3; ch++) {
			uint8_t val = (reg >> (2 * ch)) & 0x3;

			xil_printf("  CH%u:        %s\r\n", ch,
			           (val == SINC3) ? "SINC3 (OK)" : "NOT SINC3");
		}
	} else {
		xil_printf("CHAN_DIG_FILTER_SEL: read error\r\n");
	}

	ret = ad713x_spi_reg_read(dev, AD713X_REG_POWER_DOWN_CONTROL, &reg);
	if (ret == 0) {
		xil_printf("POWER_DOWN_CONTROL: 0x%02X\r\n", reg);
		xil_printf("  POWERDOWN:  %s\r\n", reg ? "ENABLED (WARN)" : "DISABLED (OK)");
	} else {
		xil_printf("POWER_DOWN_CONTROL: read error\r\n");
	}

	ret = ad713x_spi_reg_read(dev, AD713X_REG_DATA_PACKET_CONFIG, &reg);
	if (ret == 0) {
		uint8_t frame = (reg & AD713X_DATA_PACKET_CONFIG_FRAME_MSK) >> 4;
		uint8_t dclk = reg & AD713X_DATA_PACKET_CONFIG_DCLK_FREQ_MSK;

		xil_printf("DATA_PACKET_CONFIG: 0x%02X\r\n", reg);
		xil_printf("  FRAME:      %u\r\n", frame);
		xil_printf("  DCLK_FREQ:  %u\r\n", dclk);
	} else {
		xil_printf("DATA_PACKET_CONFIG: read error\r\n");
	}

	ret = ad713x_spi_reg_read(dev, AD713X_REG_TRANSFER_REGISTER, &reg);
	if (ret == 0) {
		xil_printf("TRANSFER_REGISTER: 0x%02X\r\n", reg);
		xil_printf("  MASTER/SLAVE_TX: %s\r\n",
		           (reg & AD713X_TRANSFER_MASTER_SLAVE_TX_BIT_MSK) ? "1" : "0");
	} else {
		xil_printf("TRANSFER_REGISTER: read error\r\n");
	}

	ret = ad713x_spi_reg_read(dev, AD713X_REG_STREAM_MODE, &reg);
	if (ret == 0)
		xil_printf("STREAM_MODE: 0x%02X\r\n", reg);
	else
		xil_printf("STREAM_MODE: read error\r\n");
}

#ifdef IIO_SUPPORT
#include "no_os_irq.h"
#include "xilinx_irq.h"
#include "no_os_uart.h"
#include "xilinx_uart.h"
#include "iio_ad713x.h"
#include "iio.h"
#include "iio_app.h"
#endif // IIO_SUPPORT

#if !STEP1_CONFIG_ONLY
/* DMA buffer only needed for full streaming mode (Step 3) */
static uint32_t adc_buffer[ADC_BUFFER_SIZE] __attribute__((aligned(1024)));
#endif

int main()
{
	struct axi_clkgen *clkgen_cn0561;
	struct axi_clkgen_init clkgen_cn0561_init = {
		.base = CN0561_CLKGEN_BASEADDR,
		.name = "cn0561_clkgen",
		.parent_rate = 100000000
	};
	struct ad713x_dev *cn0561_dev;
	struct ad713x_init_param cn0561_init_param;
	uint32_t adc_channel;
	int32_t ret;
	uint32_t max_speed_hz = ZED_DATA_CLK_FREQ_HZ;

#if !STEP1_CONFIG_ONLY
	/* DMA variables - only for full streaming mode (Step 3) */
	uint32_t i = 0, j;
	const float lsb = 4.096 / (pow(2, 23));
	float data;
	struct axi_dmac *axi_dma;
	struct axi_dmac_init axi_dma_init = {
		.name = "ad4134_dma",
		.base = CN0561_DMA_BASEADDR,
		.irq_option = IRQ_DISABLED
	};
#endif
	static struct xil_spi_init_param spi_engine_init_params = {
		.type = SPI_PS,
	};
	struct xil_gpio_init_param gpio_extra_param;
	struct no_os_gpio_init_param cn0561_pnd = {
		.number = GPIO_PDN,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};
#ifdef CN0561_ZED_CARRIER
	struct no_os_gpio_init_param cn0561_mode = {
		.number = GPIO_MODE,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};
	struct no_os_gpio_init_param cn0561_resetn = {
		.number = GPIO_RESETN,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};
	max_speed_hz = ZED_DATA_CLK_FREQ_HZ;
#endif
	struct no_os_pwm_desc *axi_pwm;
	struct axi_pwm_init_param axi_zed_pwm_init_trigger = {
		.base_addr = XPAR_ODR_GENERATOR_BASEADDR,
		.ref_clock_Hz = ZED_DATA_CLK_FREQ_HZ,
		.channel = 0
	};
	struct axi_pwm_init_param axi_zed_pwm_init_odr = {
		.base_addr = XPAR_ODR_GENERATOR_BASEADDR,
		.ref_clock_Hz = ZED_DATA_CLK_FREQ_HZ,
		.channel = 1
	};
	struct no_os_pwm_init_param axi_pwm_init_trigger = {
		.period_ns = 3000,
		.duty_cycle_ns = 1,
		.phase_ns = 45,
		.platform_ops = &axi_pwm_ops,
		.extra = &axi_zed_pwm_init_trigger
	};
	struct no_os_pwm_init_param axi_pwm_init_odr = {
		.period_ns = 3000,
		.duty_cycle_ns = 130,
		.phase_ns = 0,
		.platform_ops = &axi_pwm_ops,
		.extra = &axi_zed_pwm_init_odr
	};

	gpio_extra_param.device_id = GPIO_DEVICE_ID;
	gpio_extra_param.type = GPIO_PS;

	cn0561_init_param.adc_data_len = ADC_24_BIT_DATA;
	cn0561_init_param.clk_delay_en = false;
	cn0561_init_param.crc_header = CRC_6;
	cn0561_init_param.dev_id = ID_AD4134;
	cn0561_init_param.format = QUAD_CH_PO;
	cn0561_init_param.gpio_dclkio = NULL;
	cn0561_init_param.gpio_dclkmode = NULL;
	cn0561_init_param.gpio_pnd = &cn0561_pnd;
#ifdef CN0561_ZED_CARRIER
	cn0561_init_param.gpio_mode = &cn0561_mode;
	cn0561_init_param.gpio_resetn = &cn0561_resetn;
#else
	cn0561_init_param.gpio_mode = NULL;
	cn0561_init_param.gpio_resetn = NULL;
#endif
	cn0561_init_param.mode_master_nslave = false;
	cn0561_init_param.dclkmode_free_ngated = false;
	cn0561_init_param.dclkio_out_nin = false;
	cn0561_init_param.pnd = true;
	cn0561_init_param.spi_init_prm.chip_select = CN0561_SPI_CS;
	cn0561_init_param.spi_init_prm.device_id = SPI_DEVICE_ID;
	cn0561_init_param.spi_init_prm.max_speed_hz = 10000000;
	cn0561_init_param.spi_init_prm.mode = NO_OS_SPI_MODE_0;
	cn0561_init_param.spi_init_prm.platform_ops = &xil_spi_ops;
	cn0561_init_param.spi_init_prm.extra = (void *)&spi_engine_init_params;
	cn0561_init_param.spi_common_dev = 0;

#if !STEP1_CONFIG_ONLY
	spi_eng_msg_cmds[0] = READ(4);
#endif

	Xil_ICacheEnable();
	Xil_DCacheEnable();

	xil_printf("\r\n========================================\r\n");
#if STEP1_CONFIG_ONLY
	xil_printf("AD4134 Step 1 - Configuration Test\r\n");
	xil_printf("DMA: DISABLED (removed from HDL)\r\n");
	xil_printf("Offload: DISABLED (no trigger)\r\n");
	xil_printf("ILA: Use Vivado Hardware Manager\r\n");
#else
	xil_printf("AD4134 Custom Streaming Mode\r\n");
	xil_printf("DMA: ENABLED\r\n");
	xil_printf("Capture: CUSTOM AXI-STREAM\r\n");
#endif
	xil_printf("========================================\r\n\r\n");

	ret = axi_clkgen_init(&clkgen_cn0561, &clkgen_cn0561_init);
	if (ret != 0)
		return -1;
	/* Keep HDL clockgen defaults; axi_clkgen_set_rate forces CLKOUT1=CLKOUT0/4. */
	ret = no_os_axi_io_write(clkgen_cn0561_init.base, AXI_CLKGEN_REG_RESETN,
	                         AXI_CLKGEN_RESETN | AXI_CLKGEN_MMCM_RESETN);
	if (ret != 0)
		return ret;

	ret = no_os_pwm_init(&axi_pwm, &axi_pwm_init_trigger);
	if (ret != 0)
		return ret;

	ret = no_os_pwm_init(&axi_pwm, &axi_pwm_init_odr);
	if (ret != 0)
		return ret;

	ret = ad713x_init(&cn0561_dev, &cn0561_init_param);
	if (ret != 0)
		return -1;

	for (adc_channel = CH0; adc_channel <= CH3; adc_channel++) {
		ret = ad713x_dig_filter_sel_ch(cn0561_dev, SINC3, adc_channel);
		if (ret != 0)
			return -1;
	} /* Select SINC3 filtering, enable higher data convertion rates */

	no_os_mdelay(1000);

	ret = ad713x_spi_reg_write(cn0561_dev, AD713X_REG_GPIO_DIR_CTRL, 0xE7);
	if (ret != 0)
		return -1;
	ret = ad713x_spi_reg_write(cn0561_dev, AD713X_REG_GPIO_DATA, 0x84);
	if (ret != 0)
		return -1;

	/* Print register status */
	uint8_t chip_type, status, device_config;
	ad713x_spi_reg_read(cn0561_dev, AD713X_REG_CHIP_TYPE, &chip_type);
	ad713x_spi_reg_read(cn0561_dev, AD713X_REG_DEVICE_STATUS, &status);
	ad713x_spi_reg_read(cn0561_dev, AD713X_REG_DEVICE_CONFIG, &device_config);

	xil_printf("=== AD4134 Status ===\r\n");
	xil_printf("CHIP_TYPE:     0x%02X %s\r\n", chip_type,
	           (chip_type == 0x07) ? "[OK]" : "[ERROR]");
	xil_printf("STATUS:        0x%02X\r\n", status);
	xil_printf("DEVICE_CONFIG: 0x%02X\r\n", device_config);
	xil_printf("=====================\r\n\r\n");
	ad4134_slave_mode_checks(cn0561_dev);

#if STEP1_CONFIG_ONLY
	/******************************************************************
	 * STEP 1: Configuration Only Mode
	 * - No DMA offload initialization
	 * - Monitor status periodically
	 * - Use ILA to observe DCLK, ODR, DOUT signals
	 ******************************************************************/

	xil_printf("ADC configured for continuous conversion.\r\n");
	xil_printf("Data is output on DOUT[3:0] pins.\r\n");
	xil_printf("Use ILA in Vivado Hardware Manager to observe signals.\r\n\r\n");
	xil_printf("Monitoring status (10 iterations):\r\n\r\n");

	for (uint32_t loop_count = 1; loop_count <= 10; loop_count++) {
		ret = ad713x_spi_reg_read(cn0561_dev, AD713X_REG_DEVICE_STATUS, &status);
		if (ret == 0) {
			xil_printf("Loop %4lu: Status = 0x%02X%s%s%s\r\n",
			           (unsigned long)loop_count, status,
			           (status & 0x01) ? " [PLL_LOCKED]" : "",
			           (status & 0x04) ? " [INT_OSC]" : "",
			           (status & 0x08) ? " [MASTER]" : "");
		} else {
			xil_printf("Loop %4lu: Failed to read status\r\n",
			           (unsigned long)loop_count);
		}

		sleep(2);  // Every 2 seconds

		/* Every 10 loops, print full register dump */
		if (loop_count % 10 == 0) {
			xil_printf("\r\n--- Register dump at loop %lu ---\r\n",
			           (unsigned long)loop_count);
			ad713x_spi_reg_read(cn0561_dev, AD713X_REG_CHIP_TYPE, &chip_type);
			ad713x_spi_reg_read(cn0561_dev, AD713X_REG_DEVICE_CONFIG, &device_config);
			xil_printf("CHIP_TYPE:     0x%02X\r\n", chip_type);
			xil_printf("DEVICE_CONFIG: 0x%02X\r\n", device_config);
			xil_printf("STATUS:        0x%02X\r\n", status);
			xil_printf("--------------------------------\r\n\r\n");
		}
	}

#else
	/******************************************************************
	 * STEP 3: Full Streaming Mode
	 * - Initialize DMA for custom AXI-stream capture
	 * - Start data capture
	 ******************************************************************/

#ifdef IIO_SUPPORT
	xil_printf("IIO mode is not supported with custom capture.\r\n");
	return -1;
#endif /* IIO_SUPPORT */

	ret = axi_dmac_init(&axi_dma, &axi_dma_init);
	if (ret != 0)
		return ret;

	struct axi_dma_transfer rx_transfer = {
		.size = CN0561_FMC_CH_NO * CN0561_FMC_SAMPLE_NO * sizeof(uint32_t),
		.transfer_done = 0,
		.cyclic = NO,
		.src_addr = 0,
		.dest_addr = (uintptr_t)adc_buffer
	};

	ret = axi_dmac_transfer_start(axi_dma, &rx_transfer);
	if (ret != 0)
		return ret;
	ret = axi_dmac_transfer_wait_completion(axi_dma, 5000);
	if (ret != 0)
		return ret;

	Xil_DCacheInvalidateRange((INTPTR)adc_buffer,
				  CN0561_FMC_SAMPLE_NO * CN0561_FMC_CH_NO *
				  sizeof(uint32_t));

	for (i = 0; i < CN0561_FMC_SAMPLE_NO; i++) {
		int line_len = 0;
		char line[256];

		line_len += snprintf(line + line_len, sizeof(line) - line_len,
		                     "%lu:", (unsigned long)i);
		j = 0;
		while (j < CN0561_FMC_CH_NO && line_len < (int)sizeof(line)) {
			adc_buffer[CN0561_FMC_CH_NO * i + j] &= 0xffffff00;
			adc_buffer[CN0561_FMC_CH_NO * i + j] >>= 8;
			data = lsb * (int32_t)adc_buffer[CN0561_FMC_CH_NO * i + j];
			if (data > 4.095)
				data = data - 8.192;
			line_len += snprintf(line + line_len,
			                     sizeof(line) - line_len,
			                     " CH%lu: 0x%08lx = %+1.5fV",
			                     (unsigned long)j,
			                     (unsigned long)adc_buffer[CN0561_FMC_CH_NO * i + j],
			                     data);
			j++;
		}
		printf("%s\r\n", line);
	}

#ifdef CN0561_REG_DUMP
	ret = ad713x_spi_reg_dump(cn0561_dev);
	if (ret != 0)
		return ret;
#endif /* CN0561_REG_DUMP */

#endif /* STEP1_CONFIG_ONLY */

	/* Cleanup (only reached in full streaming mode, not in Step 1 loop) */
	ad713x_remove(cn0561_dev);
	print("Bye\r\n");

	return 0;
}
