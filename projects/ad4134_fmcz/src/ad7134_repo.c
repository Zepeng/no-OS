/***************************************************************************//**
* @file ad713x_fmc.c
* @brief Implementation of Main Function.
* @authors SPopa (stefan.popa@analog.com)
* @authors Andrei Drimbarean (andrei.drimbarean@analog.com)
********************************************************************************
* Copyright 2020(c) Analog Devices, Inc.
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
* THIS SOFTWARE IS PROVIDED BY ANALOG DEVICES, INC. "AS IS" AND ANY EXPRESS OR
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
#include "axi_dmac.h"

#ifdef _XPARAMETERS_PS_H_
#include "xtime_l.h"
#endif

#ifdef IIO_SUPPORT
#include "no_os_irq.h"
#include "xilinx_irq.h"
#include "no_os_uart.h"
#include "xilinx_uart.h"
#include "iio_dual_ad713x.h"
#include "iio_ad713x.h"
#include "iio.h"
#include "iio_app.h"
#endif // IIO_SUPPORT

static uint32_t adc_buffer[ADC_BUFFER_SIZE] __attribute__((aligned(1024)));

/* Timing measurement functions */
#ifdef _XPARAMETERS_PS_H_
static uint64_t get_time_us(void)
{
	XTime t;
	XTime_GetTime(&t);
	return (uint64_t)t * 1000000U / COUNTS_PER_SECOND;
}
#else
static uint64_t get_time_us(void)
{
	static uint64_t time_us = 0;
	time_us++;
	return time_us;
}
#endif

/* Timing statistics structure */
struct timing_stats {
	uint64_t last_dma_done_us;
	uint64_t idle_min_us;
	uint64_t idle_max_us;
	uint64_t idle_total_us;
	uint64_t idle_count;
	uint64_t period_min_us;
	uint64_t period_max_us;
	uint64_t period_total_us;
	uint64_t period_count;
};

static void timing_stats_init(struct timing_stats *stats)
{
	memset(stats, 0, sizeof(struct timing_stats));
}

static void timing_stats_update(struct timing_stats *stats, uint64_t dma_start_us, uint64_t dma_done_us)
{
	if (stats->last_dma_done_us != 0) {
		uint64_t idle_us = dma_start_us - stats->last_dma_done_us;
		uint64_t period_us = dma_done_us - stats->last_dma_done_us;

		if (stats->idle_count == 0 || idle_us < stats->idle_min_us) {
			stats->idle_min_us = idle_us;
		}
		if (idle_us > stats->idle_max_us) {
			stats->idle_max_us = idle_us;
		}
		stats->idle_total_us += idle_us;
		stats->idle_count++;

		if (stats->period_count == 0 || period_us < stats->period_min_us) {
			stats->period_min_us = period_us;
		}
		if (period_us > stats->period_max_us) {
			stats->period_max_us = period_us;
		}
		stats->period_total_us += period_us;
		stats->period_count++;
	}

	stats->last_dma_done_us = dma_done_us;
}

static void timing_stats_print(struct timing_stats *stats, uint32_t num_transfers)
{
	uint64_t avg_idle_us = 0;
	uint64_t avg_period_us = 0;

	if (stats->idle_count > 0) {
		avg_idle_us = stats->idle_total_us / stats->idle_count;
	}
	if (stats->period_count > 0) {
		avg_period_us = stats->period_total_us / stats->period_count;
	}

	printf("\n=== DMA Timing Statistics (after %lu transfers) ===\n", (unsigned long)num_transfers);
	if (stats->idle_count > 0) {
		printf("DMA idle gap: min=%llu us, max=%llu us, avg=%llu us\n",
		       stats->idle_min_us, stats->idle_max_us, avg_idle_us);
	} else {
		printf("DMA idle gap: n/a\n");
	}
	if (stats->period_count > 0) {
		printf("DMA period: min=%llu us, max=%llu us, avg=%llu us\n",
		       stats->period_min_us, stats->period_max_us, avg_period_us);
	} else {
		printf("DMA period: n/a\n");
	}
	printf("===================================================\n\n");
}

#include <stdint.h>

uint32_t reverse_bits(uint32_t n) {
    n = (n >> 1)  & 0x55555555 | (n << 1)  & 0xAAAAAAAA;
    n = (n >> 2)  & 0x33333333 | (n << 2)  & 0xCCCCCCCC;
    n = (n >> 4)  & 0x0F0F0F0F | (n << 4)  & 0xF0F0F0F0;
    n = (n >> 8)  & 0x00FF00FF | (n << 8)  & 0xFF00FF00;
    n = (n >> 16) & 0x0000FFFF | (n << 16) & 0xFFFF0000;
    return n;
}


int main()
{
	struct axi_clkgen *clkgen_4134;
	struct axi_clkgen_init clkgen_4134_init = {
		.base = XPAR_AXI_AD4134_CLKGEN_BASEADDR,
		.name = "ad4134_clkgen",
		.parent_rate = 100000000
	};
	struct ad713x_dev *ad713x_dev_1;
	//struct ad713x_dev *ad713x_dev_2;
	struct ad713x_init_param ad713x_init_param_1;
	//struct ad713x_init_param ad713x_init_param_2;
	uint32_t i = 0, j;
	int32_t ret;
	const float lsb = 4.096 / (pow(2, 23));
	float data;
	uint32_t spi_eng_dma_flg = DMA_LAST | DMA_PARTIAL_REPORTING_EN;
	struct spi_engine_offload_init_param spi_engine_offload_init_param;
	struct spi_engine_offload_message spi_engine_offload_message;
	uint32_t spi_eng_msg_cmds[1];
	static struct xil_spi_init_param spi_engine_init_params = {
		.type = SPI_PS,
	};
	struct xil_gpio_init_param gpio_extra_param;
	struct no_os_gpio_init_param ad4134_1_dclkio = {
		.number = GPIO_DCLKIO_1,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};
	/*struct no_os_gpio_init_param ad4134_2_dclkio = {
		.number = GPIO_DCLKIO_2,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};*/
	struct no_os_gpio_init_param ad4134_1_dclkmode = {
		.number = GPIO_DCLKMODE,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};
	/*struct no_os_gpio_init_param ad4134_2_dclkmode = {
		.number = GPIO_DCLKMODE,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};*/
	struct no_os_gpio_init_param ad4134_1_mode = {
		.number = GPIO_MODE_1,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};
	/*struct no_os_gpio_init_param ad4134_2_mode = {
		.number = GPIO_MODE_2,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};*/
	struct no_os_gpio_init_param ad4134_1_pnd = {
		.number = GPIO_PDN_1,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};
	/*struct no_os_gpio_init_param ad4134_2_pnd = {
		.number = GPIO_PDN_2,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};*/
	struct no_os_gpio_init_param ad4134_1_resetn = {
		.number = GPIO_RESETN_1,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};
	/*struct no_os_gpio_init_param ad4134_2_resetn = {
		.number = GPIO_RESETN_2,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};*/
	struct no_os_gpio_init_param ad4134_cs_sync = {
		.number = GPIO_CS_SYNC,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};
	/*struct no_os_gpio_init_param ad4134_cs_sync_1 = {
		.number = GPIO_CS_SYNC_1,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param
	};*/

	struct no_os_spi_desc *spi_eng_desc;
	struct spi_engine_init_param spi_eng_init_param  = {
		.type = SPI_ENGINE,
		.spi_engine_baseaddr = AD4134_SPI_ENGINE_BASEADDR,
		.cs_delay = 0,
		.data_width = 32,
		.ref_clk_hz = AD713x_SPI_ENG_REF_CLK_FREQ_HZ
	};
	const struct no_os_spi_init_param spi_eng_init_prm  = {
		.chip_select = AD4134_1_SPI_CS,
		.max_speed_hz = 10000000,
		.mode = NO_OS_SPI_MODE_1,
		.platform_ops = &spi_eng_platform_ops,
		.extra = (void*)&spi_eng_init_param,
	};

	struct no_os_pwm_desc *axi_pwm;
	struct axi_pwm_init_param axi_zed_pwm_init_trigger = {
		.base_addr = XPAR_ODR_GENERATOR_BASEADDR,
		.ref_clock_Hz = 100000000,
		.channel = 0
	};
	struct axi_pwm_init_param axi_zed_pwm_init_odr = {
		.base_addr = XPAR_ODR_GENERATOR_BASEADDR,
		.ref_clock_Hz = 100000000,
		.channel = 1
	};
	struct no_os_pwm_init_param axi_pwm_init_trigger = {
		.period_ns = 2000,
		.duty_cycle_ns = 1,
		.phase_ns = 45,
		.platform_ops = &axi_pwm_ops,
		.extra = &axi_zed_pwm_init_trigger
	};
	struct no_os_pwm_init_param axi_pwm_init_odr = {
		.period_ns = 2000,
		.duty_cycle_ns = 230,
		.phase_ns = 0,
		.platform_ops = &axi_pwm_ops,
		.extra = &axi_zed_pwm_init_odr
	};

	/* Initialize timing statistics */
	struct timing_stats timing;
	timing_stats_init(&timing);
	uint32_t transfer_count = 0;
	uint64_t dma_start_us, dma_done_us;

	gpio_extra_param.device_id = GPIO_DEVICE_ID;

	gpio_extra_param.device_id = GPIO_DEVICE_ID;
	gpio_extra_param.type = GPIO_PS;

	ad713x_init_param_1.adc_data_len = ADC_24_BIT_DATA;
	ad713x_init_param_1.clk_delay_en = false;
	ad713x_init_param_1.crc_header = CRC_6;
	ad713x_init_param_1.dev_id = ID_AD4134;
	ad713x_init_param_1.format = QUAD_CH_PO;
	ad713x_init_param_1.gpio_dclkio = &ad4134_1_dclkio;
	ad713x_init_param_1.gpio_dclkmode = &ad4134_1_dclkmode;
	ad713x_init_param_1.gpio_mode = &ad4134_1_mode;
	ad713x_init_param_1.gpio_pnd = &ad4134_1_pnd;
	ad713x_init_param_1.gpio_resetn = &ad4134_1_resetn;
	// NOTE: gpio_cs_sync not supported in current driver version
	// ad713x_init_param_1.gpio_cs_sync = &ad4134_cs_sync;
	ad713x_init_param_1.mode_master_nslave = false;
	ad713x_init_param_1.dclkmode_free_ngated = false;
	ad713x_init_param_1.dclkio_out_nin = false;
	ad713x_init_param_1.pnd = true;
	ad713x_init_param_1.spi_init_prm.chip_select = AD4134_1_SPI_CS;
	ad713x_init_param_1.spi_init_prm.device_id = SPI_DEVICE_ID;
	ad713x_init_param_1.spi_init_prm.max_speed_hz = 50000000;
	ad713x_init_param_1.spi_init_prm.mode = NO_OS_SPI_MODE_0;
	ad713x_init_param_1.spi_init_prm.platform_ops = &xil_spi_ops;
	ad713x_init_param_1.spi_init_prm.extra = (void *)&spi_engine_init_params;
	ad713x_init_param_1.spi_common_dev = 0;

	/*ad713x_init_param_2.adc_data_len = ADC_24_BIT_DATA;
	ad713x_init_param_2.clk_delay_en = false;
	ad713x_init_param_2.crc_header = CRC_6;
	ad713x_init_param_2.dev_id = ID_AD4134;
	ad713x_init_param_2.format = QUAD_CH_PO;
	ad713x_init_param_2.gpio_dclkio = &ad4134_2_dclkio;
	ad713x_init_param_2.gpio_dclkmode = &ad4134_2_dclkmode;
	ad713x_init_param_2.gpio_mode = &ad4134_2_mode;
	ad713x_init_param_2.gpio_pnd = &ad4134_2_pnd;
	ad713x_init_param_2.gpio_resetn = &ad4134_2_resetn;
	ad713x_init_param_2.gpio_cs_sync = &ad4134_cs_sync_1;
	ad713x_init_param_2.mode_master_nslave = false;
	ad713x_init_param_2.dclkmode_free_ngated = false;
	ad713x_init_param_2.dclkio_out_nin = false;
	ad713x_init_param_2.pnd = true;
	ad713x_init_param_2.spi_init_prm.device_id = SPI_DEVICE_ID;
	ad713x_init_param_2.spi_init_prm.chip_select = AD4134_2_SPI_CS;
	ad713x_init_param_2.spi_init_prm.max_speed_hz = 10000000;
	ad713x_init_param_2.spi_init_prm.mode = NO_OS_SPI_MODE_0;
	ad713x_init_param_2.spi_init_prm.platform_ops = &xil_spi_ops;
	ad713x_init_param_2.spi_init_prm.extra = (void *)&spi_engine_init_params;
	ad713x_init_param_2.spi_common_dev = 0;*/

	spi_eng_msg_cmds[0] = READ(4);

	Xil_ICacheEnable();
	Xil_DCacheEnable();

	printf("\n\n========================================\n");
	printf("AD4134 Official Repository Example\n");
	printf("With DMA Timing Measurement\n");
	printf("========================================\n\n");

	ret = axi_clkgen_init(&clkgen_4134, &clkgen_4134_init);
	if (ret != 0)
		return -1;

	ret = axi_clkgen_set_rate(clkgen_4134, AD713x_SPI_ENG_REF_CLK_FREQ_HZ);
	if (ret != 0)
		return -1;

	ret = no_os_pwm_init(&axi_pwm, &axi_pwm_init_trigger);
	if (ret != 0)
		return ret;

	ret = no_os_pwm_init(&axi_pwm, &axi_pwm_init_odr);
	if (ret != 0)
		return ret;

	ret = ad713x_init(&ad713x_dev_1, &ad713x_init_param_1);
	if (ret != 0)
		return -1;

	int adc_channel;
	for (adc_channel = 0; adc_channel <= 3; adc_channel++) {
		ret = ad713x_dig_filter_sel_ch(ad713x_dev_1, SINC3, adc_channel);
		if (ret != 0)
			return -1;
	}

	no_os_mdelay(1000);
	//ad713x_init_param_2.spi_common_dev = ad713x_dev_1->spi_desc;
	//ret = ad713x_init(&ad713x_dev_2, &ad713x_init_param_2);
	//if (ret != 0)
	//	return -1;
	//no_os_mdelay(1000);

	spi_engine_offload_init_param.rx_dma_baseaddr = AD4134_DMA_BASEADDR;
	spi_engine_offload_init_param.offload_config = OFFLOAD_RX_EN;
	spi_engine_offload_init_param.dma_flags = &spi_eng_dma_flg;

	ret = no_os_spi_init(&spi_eng_desc, &spi_eng_init_prm);
	if (ret != 0)
		return -1;

	ret = spi_engine_offload_init(spi_eng_desc, &spi_engine_offload_init_param);
	if (ret != 0)
		return -1;

	spi_engine_offload_message.commands = spi_eng_msg_cmds;
	spi_engine_offload_message.no_commands = NO_OS_ARRAY_SIZE(spi_eng_msg_cmds);
	spi_engine_offload_message.commands_data = NULL;
	spi_engine_offload_message.rx_addr = (uint32_t)adc_buffer;
	spi_engine_offload_message.tx_addr = 0xA000000;

#ifdef IIO_SUPPORT
	struct iio_ad713x *iio_ad713x;
	/**
	 * iio devices corresponding to every device.
	 */
	struct iio_device *ad713x_dev_desc;

	struct iio_ad713x_init_par iio_ad713x_init_par = {
		.dev = ad713x_dev_1,
		.num_channels = 4,
		.spi_eng_desc = spi_eng_desc,
		.spi_engine_offload_message = &spi_engine_offload_message,
		.dcache_invalidate_range = (void (*)(uint32_t, uint32_t))Xil_DCacheInvalidateRange,
	};

	struct xil_uart_init_param platform_uart_init_par = {
#ifdef XPAR_XUARTLITE_NUM_INSTANCES
		.type = UART_PL,
#else
		.type = UART_PS,
		.irq_id = UART_IRQ_ID
#endif
	};

	struct no_os_uart_init_param iio_uart_ip = {
		.device_id = UART_DEVICE_ID,
		.irq_id = UART_IRQ_ID,
		.baud_rate = UART_BAUDRATE,
		.size = NO_OS_UART_CS_8,
		.parity = NO_OS_UART_PAR_NO,
		.stop = NO_OS_UART_STOP_1_BIT,
		.extra = &platform_uart_init_par,
		.platform_ops = &xil_uart_ops
	};

	struct iio_app_desc *app;
	struct iio_app_init_param app_init_param = { 0 };

	ret = iio_dual_ad713x_init(&iio_ad713x, &iio_ad713x_init_par);
	if (ret < 0)
		return ret;

	iio_dual_ad713x_get_dev_descriptor(iio_ad713x, &ad713x_dev_desc);

	struct iio_data_buffer rd_buff = {
		.buff = (void *)adc_buffer,
		.size = ADC_BUFFER_SIZE
	};

	struct iio_app_device devices[] = {
		/*IIO_APP_DEVICE("dual_ad4134", iio_ad713x, ad713x_dev_desc,
			       &rd_buff, NULL, NULL),*/
		IIO_APP_DEVICE("ad4134_1", ad713x_dev_1, &ad713x_iio_desc,
			       NULL, NULL, NULL)
		/*IIO_APP_DEVICE("ad4134_2", ad713x_dev_2, &ad713x_iio_desc,
			       NULL, NULL, NULL)*/

	};

	app_init_param.devices = devices;
	app_init_param.nb_devices = NO_OS_ARRAY_SIZE(devices);
	app_init_param.uart_init_params = iio_uart_ip;

	ret = iio_app_init(&app, app_init_param);
	if (ret)
		return ret;

	iio_app_run(app);

#endif /* IIO_SUPPORT */

	// NOTE: ad713x_channel_sync not available in current driver version
	// ret = ad713x_channel_sync(ad713x_dev_1);
	// if (ret != 0)
	//	return ret;

	printf("Starting DMA timing test with 100 transfers...\n\n");
	printf("NOTE: This version does NOT have gpio_cs_sync or ad713x_channel_sync\n");
	printf("Expected behavior: Fast DMA without ODR synchronization\n\n");

	while(transfer_count < 100) {
		/* Phase 1: Software delay workaround to pace transfers at ODR rate */
		/* ODR = 500 kHz = 2 µs per sample */
		/* Expected time for ADC to accumulate (AD4134_FMC_CH_NO * AD4134_FMC_SAMPLE_NO) samples */
		uint64_t expected_accumulation_us = (AD4134_FMC_CH_NO * AD4134_FMC_SAMPLE_NO) * 2;

		/* Check if we need to wait for ADC to accumulate enough samples */
		if (timing.last_dma_done_us != 0) {
			uint64_t current_time_us = get_time_us();
			uint64_t elapsed_us = current_time_us - timing.last_dma_done_us;

			if (elapsed_us < expected_accumulation_us) {
				/* Wait for ADC to accumulate enough samples at ODR rate */
				uint64_t delay_us = expected_accumulation_us - elapsed_us;
				no_os_udelay(delay_us);
			}
		}

		/* Record DMA start time */
		dma_start_us = get_time_us();

		ret = spi_engine_offload_transfer(spi_eng_desc, spi_engine_offload_message,
						  (AD4134_FMC_CH_NO * AD4134_FMC_SAMPLE_NO));
		if (ret != 0)
			return ret;

		/* Record DMA done time */
		dma_done_us = get_time_us();

		/* Update timing statistics */
		timing_stats_update(&timing, dma_start_us, dma_done_us);
		transfer_count++;

		Xil_DCacheInvalidateRange((INTPTR)adc_buffer,
					  AD4134_FMC_SAMPLE_NO * AD4134_FMC_CH_NO *
					  sizeof(uint32_t));

		/* Print data from first transfer only */
		if (transfer_count == 1) {
			float ch_voltages[4];
			for (i = 0; i < AD4134_FMC_SAMPLE_NO; i++) {
				j = 0;
				printf("%lu: ", i);
				while (j < AD4134_FMC_CH_NO) {
					//adc_buffer[AD4134_FMC_CH_NO * i +j ] = reverse_bits(adc_buffer[AD4134_FMC_CH_NO * i +j ]);
					if (j != 3) {
						//printf("%X, ", adc_buffer[AD4134_FMC_CH_NO * i + j]);
					} else {
						//printf("%X\r\n", adc_buffer[AD4134_FMC_CH_NO * i + j]);
					}
					adc_buffer[AD4134_FMC_CH_NO * i + j] &= 0xffffff00;
					adc_buffer[AD4134_FMC_CH_NO * i + j] >>= 8;
					data = lsb * (int32_t)adc_buffer[AD4134_FMC_CH_NO * i + j];
					if (data > 4.095)
						data = data - 8.192;
					ch_voltages[j] = data;
					j++;
				}
				printf("%+1.5f, %+1.5f, %+1.5f, %+1.5f\r\n", ch_voltages[0], ch_voltages[1],
								ch_voltages[2], ch_voltages[3]);
			}
		}

		/* Print timing stats every 10 transfers */
		if (transfer_count % 10 == 0) {
			timing_stats_print(&timing, transfer_count);
		}
	}

	/* Print final timing statistics */
	printf("\n========================================\n");
	printf("Final Timing Statistics\n");
	printf("========================================\n");
	timing_stats_print(&timing, transfer_count);

	/*ret = ad713x_spi_reg_dump(ad713x_dev_1);
	if (ret != 0)
		return ret;*/
	ad713x_remove(ad713x_dev_1);
	//ad713x_remove(ad713x_dev_2);
	//print("Bye\n\r");

	Xil_DCacheDisable();
	Xil_ICacheDisable();

	return 0;
}
