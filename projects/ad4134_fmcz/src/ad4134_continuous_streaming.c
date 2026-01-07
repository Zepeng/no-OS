/***************************************************************************/ /**
																			   *   @file   ad4134_continuous_streaming.c
																			   *   @brief  AD4134 Continuous Streaming Implementation
																			   *   @author Claude Code (AI-assisted)
																			   ********************************************************************************
																			   * Copyright 2024(c) Stanford Readout Project
																			   *
																			   * Redistribution and use in source and binary forms, with or without
																			   * modification, are permitted.
																			   *******************************************************************************/

/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>
#include "parameters.h"
#include "ad713x.h"
#include "no_os_spi.h"
#include "no_os_pwm.h"
#include "no_os_gpio.h"
#include "no_os_delay.h"
#include "no_os_alloc.h"
#include "no_os_util.h"
#include "xilinx_spi.h"
#include "spi_engine.h"
#include "clk_axi_clkgen.h"
#include "axi_pwm_extra.h"
#include "xilinx_gpio.h"
#include "xil_cache.h"
#include "xil_printf.h"
#ifdef _XPARAMETERS_PS_H_
#include "xtime_l.h"
#endif

/******************************************************************************/
/********************** Macros and Constants Definitions **********************/
/******************************************************************************/

#ifndef pr_info
#define pr_info printf
#endif

#ifndef pr_err
#define pr_err printf
#endif

/* Buffer configuration for ping-pong operation */
#ifndef STREAMING_NUM_BUFFERS
#define STREAMING_NUM_BUFFERS 2
#endif
#ifndef STREAMING_BUFFER_SIZE_SAMPLES
#define STREAMING_BUFFER_SIZE_SAMPLES (512 * 1024) /* 512K samples per buffer */
#endif
#ifndef STREAMING_BYTES_PER_SAMPLE
#define STREAMING_BYTES_PER_SAMPLE 16 /* 4 channels x 32 bits */
#endif
#ifndef STREAMING_USE_STATIC_BUFFERS
#define STREAMING_USE_STATIC_BUFFERS 0
#endif
#define STREAMING_BUFFER_SIZE_BYTES (STREAMING_BUFFER_SIZE_SAMPLES * STREAMING_BYTES_PER_SAMPLE)

/* Performance monitoring */
#define STREAMING_ENABLE_STATS 0
#define STREAMING_STATS_INTERVAL_MS 1000

#define SAMPLES_PER_TRANSFER (STREAMING_BUFFER_SIZE_SAMPLES)
#define MAX_STREAMING_BUFFERS 1000		   /* Stop after this many buffers for demo */
#define STREAMING_CALLBACK_STATS_EVERY_N 0 /* Disable to reduce overhead */
#define STREAMING_STATS_EVERY_N 0
#define STREAMING_PRINT_FIRST_SAMPLES 0

#define PWM_TRIGGER_PERIOD_NS 2000U
#define PWM_TRIGGER_DUTY_NS 1U
#define PWM_TRIGGER_PHASE_NS 45U
#define PWM_ODR_PERIOD_NS 2000U
#define PWM_ODR_DUTY_NS 130U
#define PWM_ODR_PHASE_NS 0U
#define STREAMING_PRINT_PWM_CONFIG 1
#define STREAMING_PRINT_FINAL_STATS 1

#define SPI_ENGINE_MAX_SPEED_HZ 50000000U

/* Cache line alignment for DMA buffers (64 bytes for Zynq-7000) */
#define CACHE_LINE_SIZE 64

/* Timeout for DMA transfer (ms) */
#define DMA_TRANSFER_TIMEOUT_MS 1000

/******************************************************************************/
/*************************** Types Declarations *******************************/
/******************************************************************************/

enum streaming_state
{
	STREAMING_STOPPED = 0,
	STREAMING_INITIALIZING,
	STREAMING_RUNNING,
	STREAMING_PAUSED,
	STREAMING_ERROR
};

struct streaming_buffer
{
	uint32_t *data;
	uint32_t size_samples;
	uint32_t size_bytes;
	volatile bool ready;
	volatile bool consumed;
	uint32_t sequence_number;
};

struct streaming_stats
{
	uint64_t total_samples;
	uint64_t total_buffers;
	uint32_t buffer_overruns;
	uint32_t dma_errors;
	uint32_t max_processing_time_ms;
	uint32_t avg_processing_time_ms;
	uint64_t start_time_ms;
	uint64_t last_dma_done_us;
	uint64_t dma_idle_min_us;
	uint64_t dma_idle_max_us;
	uint64_t dma_idle_total_us;
	uint64_t dma_idle_count;
	uint64_t dma_period_min_us;
	uint64_t dma_period_max_us;
	uint64_t dma_period_total_us;
	uint64_t dma_period_count;
};

struct streaming_context
{
	struct no_os_spi_desc *spi_desc;
	struct spi_engine_offload_message *offload_msg;
	struct streaming_buffer buffers[STREAMING_NUM_BUFFERS];
	uint32_t active_write_buffer;
	uint32_t active_read_buffer;
	enum streaming_state state;
	volatile bool stop_requested;
	struct streaming_stats stats;
	uint32_t samples_per_transfer;
	void (*data_callback)(uint32_t *data, uint32_t num_samples, void *user_data);
	void *user_data;
};

struct streaming_init_param
{
	struct no_os_spi_desc *spi_desc;
	struct spi_engine_offload_message *offload_msg;
	uint32_t samples_per_transfer;
	void (*data_callback)(uint32_t *data, uint32_t num_samples, void *user_data);
	void *user_data;
};

/******************************************************************************/
/************************ Functions Declarations ******************************/
/******************************************************************************/

int32_t streaming_init(struct streaming_context **ctx,
					   const struct streaming_init_param *init_param);
int32_t streaming_start(struct streaming_context *ctx);
int32_t streaming_stop(struct streaming_context *ctx);
int32_t streaming_process(struct streaming_context *ctx);
int32_t streaming_get_stats(struct streaming_context *ctx,
							struct streaming_stats *stats);
void streaming_print_stats(struct streaming_context *ctx);
int32_t streaming_remove(struct streaming_context *ctx);
static void streaming_print_final_stats(struct streaming_context *ctx);

#if STREAMING_USE_STATIC_BUFFERS
static uint32_t streaming_buffer_storage[STREAMING_NUM_BUFFERS]
										[STREAMING_BUFFER_SIZE_SAMPLES * 4]
	__attribute__((aligned(CACHE_LINE_SIZE)));
#endif

/******************************************************************************/
/************************** Global Variables **********************************/
/******************************************************************************/

static uint64_t total_samples_processed = 0;
static uint32_t buffers_processed = 0;

/******************************************************************************/
/************************** Functions Implementation **************************/
/******************************************************************************/

#ifdef _XPARAMETERS_PS_H_
static uint64_t get_time_us(void)
{
	XTime t;

	XTime_GetTime(&t);
	return (uint64_t)t * 1000000U / COUNTS_PER_SECOND;
}

static uint64_t get_time_ms(void)
{
	return get_time_us() / 1000U;
}
#else
static uint64_t get_time_ms(void)
{
	static uint64_t time_ms = 0;

	time_ms++;
	return time_ms;
}

static uint64_t get_time_us(void)
{
	return get_time_ms() * 1000U;
}
#endif

/**
 * @brief Initialize continuous streaming context
 */
int32_t streaming_init(struct streaming_context **ctx,
					   const struct streaming_init_param *init_param)
{
	struct streaming_context *context;
	uint32_t i;

	if (!ctx || !init_param || !init_param->spi_desc || !init_param->offload_msg)
	{
		printf("ERROR: Invalid parameters\n");
		return -1;
	}

	/* Allocate context */
	context = (struct streaming_context *)no_os_calloc(1,
													   sizeof(struct streaming_context));
	if (!context)
	{
		printf("ERROR: Failed to allocate streaming context\n");
		return -1;
	}

	/* Store configuration */
	context->spi_desc = init_param->spi_desc;
	context->offload_msg = init_param->offload_msg;
	context->samples_per_transfer = init_param->samples_per_transfer;
	context->data_callback = init_param->data_callback;
	context->user_data = init_param->user_data;

	/* Allocate ping-pong buffers (cache-aligned for DMA) */
	for (i = 0; i < STREAMING_NUM_BUFFERS; i++)
	{
#if STREAMING_USE_STATIC_BUFFERS
		context->buffers[i].data = streaming_buffer_storage[i];
#else
		/* Allocate aligned memory */
		context->buffers[i].data = (uint32_t *)no_os_calloc(
			STREAMING_BUFFER_SIZE_SAMPLES * 4,
			sizeof(uint32_t));
#endif

		if (!context->buffers[i].data)
		{
			printf("ERROR: Failed to allocate buffer %" PRIu32 "\n", i);
			/* Clean up previously allocated buffers */
			while (i > 0)
			{
				i--;
				no_os_free(context->buffers[i].data);
			}
			no_os_free(context);
			return -1;
		}

		/* Ensure cache-line alignment by checking address */
		if (((uintptr_t)context->buffers[i].data) % CACHE_LINE_SIZE != 0)
		{
			printf("WARNING: Buffer %" PRIu32 " not cache-aligned (addr=0x%" PRIxPTR ")\n",
				   i, (uintptr_t)context->buffers[i].data);
		}

		context->buffers[i].size_samples = STREAMING_BUFFER_SIZE_SAMPLES;
		context->buffers[i].size_bytes = STREAMING_BUFFER_SIZE_BYTES;
		context->buffers[i].ready = false;
		context->buffers[i].consumed = true;
		context->buffers[i].sequence_number = 0;

		printf("Allocated buffer %" PRIu32 " at 0x%" PRIxPTR " (%" PRIu32
			   " samples, %" PRIu32 " bytes)\n",
			   i, (uintptr_t)context->buffers[i].data,
			   context->buffers[i].size_samples,
			   context->buffers[i].size_bytes);
	}

	/* Initialize state */
	context->state = STREAMING_STOPPED;
	context->stop_requested = false;
	context->active_write_buffer = 0;
	context->active_read_buffer = 0;

	/* Reset statistics */
	memset(&context->stats, 0, sizeof(struct streaming_stats));

	*ctx = context;

	printf("Streaming context initialized successfully\n");
	printf("Configuration:\n");
	printf("  - Buffers: %d x %u samples (%u bytes each)\n",
		   STREAMING_NUM_BUFFERS,
		   STREAMING_BUFFER_SIZE_SAMPLES,
		   STREAMING_BUFFER_SIZE_BYTES);
	printf("  - Samples per transfer: %" PRIu32 "\n",
		   context->samples_per_transfer);
	printf("  - Total buffer capacity: %u samples\n",
		   STREAMING_NUM_BUFFERS * STREAMING_BUFFER_SIZE_SAMPLES);

	return 0;
}

/**
 * @brief Start continuous streaming
 */
int32_t streaming_start(struct streaming_context *ctx)
{
	if (!ctx)
	{
		printf("ERROR: Invalid context\n");
		return -1;
	}

	if (ctx->state == STREAMING_RUNNING)
	{
		printf("WARNING: Streaming already running\n");
		return 0;
	}

	printf("Starting continuous streaming...\n");

	/* Reset state */
	ctx->active_write_buffer = 0;
	ctx->active_read_buffer = 0;
	ctx->stop_requested = false;

	/* Mark all buffers as consumed */
	for (uint32_t i = 0; i < STREAMING_NUM_BUFFERS; i++)
	{
		ctx->buffers[i].ready = false;
		ctx->buffers[i].consumed = true;
		ctx->buffers[i].sequence_number = 0;
	}

	/* Reset statistics */
	ctx->stats.total_samples = 0;
	ctx->stats.total_buffers = 0;
	ctx->stats.buffer_overruns = 0;
	ctx->stats.dma_errors = 0;
	ctx->stats.max_processing_time_ms = 0;
	ctx->stats.avg_processing_time_ms = 0;
	ctx->stats.start_time_ms = get_time_ms();
	ctx->stats.last_dma_done_us = 0;
	ctx->stats.dma_idle_min_us = 0;
	ctx->stats.dma_idle_max_us = 0;
	ctx->stats.dma_idle_total_us = 0;
	ctx->stats.dma_idle_count = 0;
	ctx->stats.dma_period_min_us = 0;
	ctx->stats.dma_period_max_us = 0;
	ctx->stats.dma_period_total_us = 0;
	ctx->stats.dma_period_count = 0;

	ctx->state = STREAMING_RUNNING;

	printf("Streaming started\n");

	return 0;
}

/**
 * @brief Stop continuous streaming
 */
int32_t streaming_stop(struct streaming_context *ctx)
{
	if (!ctx)
	{
		printf("ERROR: Invalid context\n");
		return -1;
	}

	if (ctx->state != STREAMING_RUNNING)
	{
		printf("WARNING: Streaming not running\n");
		return 0;
	}

	printf("Stopping continuous streaming...\n");

	ctx->stop_requested = true;
	ctx->state = STREAMING_STOPPED;

	/* Print final statistics */
	streaming_print_stats(ctx);

	printf("Streaming stopped\n");

	return 0;
}

/**
 * @brief Main streaming processing function
 *
 * This implements ping-pong buffering:
 * 1. Start DMA transfer to buffer A
 * 2. Wait for completion
 * 3. Start DMA transfer to buffer B
 * 4. While DMA fills buffer B, process buffer A
 * 5. Start DMA transfer to buffer A
 * 6. While DMA fills buffer A, process buffer B
 * 7. Repeat from step 3
 */
int32_t streaming_process(struct streaming_context *ctx)
{
	int32_t ret;
	uint32_t current_buffer;
	uint32_t process_buffer;
	uint64_t start_process_time, end_process_time, process_time_ms;
	struct streaming_buffer *buf;

	if (!ctx)
	{
		printf("ERROR: Invalid context\n");
		return -1;
	}

	if (ctx->state != STREAMING_RUNNING)
	{
		printf("ERROR: Streaming not running\n");
		return -1;
	}

	/* Get current buffer to fill */
	current_buffer = ctx->active_write_buffer;
	buf = &ctx->buffers[current_buffer];

	/* Check if previous buffer processing is complete */
	if (current_buffer > 0)
	{
		process_buffer = current_buffer - 1;
	}
	else
	{
		process_buffer = STREAMING_NUM_BUFFERS - 1;
	}

	/* CRITICAL PATH: Minimize operations before DMA start */
	uint64_t dma_start_us = get_time_us();

	/* Mark current buffer as not ready (required before DMA) */
	buf->ready = false;
	buf->consumed = false;

	/* Update offload message with current buffer address (required) */
	ctx->offload_msg->rx_addr = (uint32_t)buf->data;

	/* Start DMA transfer to current buffer - HIGHEST PRIORITY */
	ret = spi_engine_offload_transfer(ctx->spi_desc,
									  *ctx->offload_msg,
									  ctx->samples_per_transfer);

	if (ret != 0)
	{
		printf("ERROR: DMA transfer failed for buffer %" PRIu32 ": %" PRId32
			   "\n",
			   current_buffer, ret);
		ctx->stats.dma_errors++;
		return ret;
	}

	/* Calculate stats AFTER DMA starts (don't add to idle gap) */
	uint64_t dma_done_us = get_time_us();

	if (ctx->stats.last_dma_done_us != 0)
	{
		uint64_t idle_us = dma_start_us - ctx->stats.last_dma_done_us;
		uint64_t period_us = dma_done_us - ctx->stats.last_dma_done_us;

		/* Update idle gap stats */
		if (ctx->stats.dma_idle_count == 0 ||
			idle_us < ctx->stats.dma_idle_min_us)
		{
			ctx->stats.dma_idle_min_us = idle_us;
		}
		if (idle_us > ctx->stats.dma_idle_max_us)
		{
			ctx->stats.dma_idle_max_us = idle_us;
		}
		ctx->stats.dma_idle_total_us += idle_us;
		ctx->stats.dma_idle_count++;

		/* Update period stats */
		if (ctx->stats.dma_period_count == 0 ||
			period_us < ctx->stats.dma_period_min_us)
		{
			ctx->stats.dma_period_min_us = period_us;
		}
		if (period_us > ctx->stats.dma_period_max_us)
		{
			ctx->stats.dma_period_max_us = period_us;
		}
		ctx->stats.dma_period_total_us += period_us;
		ctx->stats.dma_period_count++;
	}

	ctx->stats.last_dma_done_us = dma_done_us;

	/* DMA transfer completed - buffer is now ready */
	buf->ready = true;
	buf->sequence_number = ctx->stats.total_buffers;
	ctx->stats.total_buffers++;
	ctx->stats.total_samples += ctx->samples_per_transfer;

	/* Check for buffer overrun (moved after DMA start) */
	if (!ctx->buffers[process_buffer].consumed &&
		ctx->buffers[process_buffer].ready)
	{
		ctx->stats.buffer_overruns++;
		printf("ERROR: Buffer overrun detected! Buffer %" PRIu32
			   " not consumed in time\n",
			   process_buffer);
	}

	/* Invalidate D-cache for this buffer region AFTER DMA */
	Xil_DCacheInvalidateRange((INTPTR)buf->data, buf->size_bytes);

	/* Switch to next buffer for next DMA transfer */
	ctx->active_write_buffer = (current_buffer + 1) % STREAMING_NUM_BUFFERS;

	/* Process the buffer we just filled */
	if (buf->ready && !buf->consumed)
	{
		start_process_time = get_time_ms();

		/* Call user callback if registered */
		if (ctx->data_callback)
		{
			ctx->data_callback(buf->data, ctx->samples_per_transfer, ctx->user_data);
		}

		/* Mark as consumed */
		buf->consumed = true;

		end_process_time = get_time_ms();
		process_time_ms = end_process_time - start_process_time;

		/* Update max processing time only (avg calculation is expensive) */
		if (process_time_ms > ctx->stats.max_processing_time_ms)
		{
			ctx->stats.max_processing_time_ms = process_time_ms;
		}
	}

	/* Check if stop was requested */
	if (ctx->stop_requested)
	{
		return -2; /* Signal to exit loop */
	}

	return 0;
}

/**
 * @brief Get current statistics
 */
int32_t streaming_get_stats(struct streaming_context *ctx,
							struct streaming_stats *stats)
{
	if (!ctx || !stats)
	{
		printf("ERROR: Invalid parameters\n");
		return -1;
	}

	memcpy(stats, &ctx->stats, sizeof(struct streaming_stats));

	return 0;
}

/**
 * @brief Print statistics to console
 */
void streaming_print_stats(struct streaming_context *ctx)
{
	uint64_t runtime_ms;
	uint64_t samples_per_sec;

	if (!ctx)
	{
		return;
	}

#if !STREAMING_ENABLE_STATS
	return;
#endif

	runtime_ms = get_time_ms() - ctx->stats.start_time_ms;
	if (runtime_ms == 0)
	{
		runtime_ms = 1; /* Avoid division by zero */
	}

	samples_per_sec = (ctx->stats.total_samples * 1000) / runtime_ms;

	printf("\n=== Streaming Statistics ===\n");
	printf("Runtime: %llu ms (%.2f seconds)\n",
		   runtime_ms, runtime_ms / 1000.0f);
	printf("Total samples: %llu\n", ctx->stats.total_samples);
	printf("Total buffers: %llu\n", ctx->stats.total_buffers);
	printf("Sample rate: %llu samples/sec\n", samples_per_sec);
	printf("Buffer overruns: %" PRIu32 "\n", ctx->stats.buffer_overruns);
	printf("DMA errors: %" PRIu32 "\n", ctx->stats.dma_errors);
	printf("Max processing time: %" PRIu32 " ms\n",
		   ctx->stats.max_processing_time_ms);
	printf("Avg processing time: %" PRIu32 " ms\n",
		   ctx->stats.avg_processing_time_ms);
	if (ctx->stats.dma_idle_count > 0)
	{
		uint64_t avg_idle_us = ctx->stats.dma_idle_total_us /
							   ctx->stats.dma_idle_count;

		printf("DMA idle gap: min=%llu us, max=%llu us, avg=%llu us\n",
			   ctx->stats.dma_idle_min_us,
			   ctx->stats.dma_idle_max_us,
			   avg_idle_us);
	}
	else
	{
		printf("DMA idle gap: n/a\n");
	}
	if (ctx->stats.dma_period_count > 0)
	{
		uint64_t avg_period_us = ctx->stats.dma_period_total_us /
								 ctx->stats.dma_period_count;

		printf("DMA period: min=%llu us, max=%llu us, avg=%llu us\n",
			   ctx->stats.dma_period_min_us,
			   ctx->stats.dma_period_max_us,
			   avg_period_us);
	}
	else
	{
		printf("DMA period: n/a\n");
	}

	if (ctx->stats.buffer_overruns > 0)
	{
		printf("WARNING: %" PRIu32 " buffer overruns detected!\n",
			   ctx->stats.buffer_overruns);
		printf("Data loss occurred - processing too slow for sample rate\n");
	}
	else
	{
		printf("SUCCESS: No buffer overruns - continuous streaming working!\n");
	}

	printf("===========================\n\n");
}

static void streaming_print_final_stats(struct streaming_context *ctx)
{
	uint64_t runtime_ms;
	uint64_t samples_per_sec;
	uint64_t avg_idle_us = 0;
	uint64_t avg_period_us = 0;

	if (!ctx)
	{
		return;
	}

	runtime_ms = get_time_ms() - ctx->stats.start_time_ms;
	if (runtime_ms == 0)
	{
		runtime_ms = 1;
	}

	samples_per_sec = (ctx->stats.total_samples * 1000) / runtime_ms;

	if (ctx->stats.dma_idle_count > 0)
	{
		avg_idle_us = ctx->stats.dma_idle_total_us /
					  ctx->stats.dma_idle_count;
	}
	if (ctx->stats.dma_period_count > 0)
	{
		avg_period_us = ctx->stats.dma_period_total_us /
						ctx->stats.dma_period_count;
	}

	printf("Final stats: runtime=%llu ms samples=%llu rate=%llu sps "
		   "idle_us=%llu period_us=%llu overruns=%" PRIu32
		   " dma_err=%" PRIu32 " max_proc_ms=%" PRIu32 "\n",
		   runtime_ms, ctx->stats.total_samples, samples_per_sec,
		   avg_idle_us, avg_period_us, ctx->stats.buffer_overruns,
		   ctx->stats.dma_errors, ctx->stats.max_processing_time_ms);
}

/**
 * @brief Free streaming context and resources
 */
int32_t streaming_remove(struct streaming_context *ctx)
{
	uint32_t i;

	if (!ctx)
	{
		return -1;
	}

	/* Stop streaming if running */
	if (ctx->state == STREAMING_RUNNING)
	{
		streaming_stop(ctx);
	}

	/* Free buffers */
	for (i = 0; i < STREAMING_NUM_BUFFERS; i++)
	{
		if (ctx->buffers[i].data)
		{
#if !STREAMING_USE_STATIC_BUFFERS
			no_os_free(ctx->buffers[i].data);
#endif
			ctx->buffers[i].data = NULL;
		}
	}

	/* Free context */
	no_os_free(ctx);

	printf("Streaming context removed\n");

	return 0;
}

/******************************************************************************/
/************************** Example Application *******************************/
/******************************************************************************/

void data_processing_callback(uint32_t *data, uint32_t num_samples,
							  void *user_data)
{
	/* Minimal processing for maximum throughput */
	/* Uncomment below for detailed voltage processing when needed */
#if 0
	uint32_t i, j;
	float lsb = 8.192f / 16777216.0f;
	int32_t raw_value;
	float voltage;

	static float ch_min[4] = {999.0f, 999.0f, 999.0f, 999.0f};
	static float ch_max[4] = {-999.0f, -999.0f, -999.0f, -999.0f};
	static float ch_sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};

	for (i = 0; i < num_samples; i++) {
		for (j = 0; j < AD4134_FMC_CH_NO; j++) {
			raw_value = data[i * AD4134_FMC_CH_NO + j];
			raw_value &= 0xFFFFFF00;
			raw_value >>= 8;

			voltage = lsb * (int32_t)raw_value;
			if (voltage > 4.095f) {
				voltage = voltage - 8.192f;
			}

			if (voltage < ch_min[j]) {
				ch_min[j] = voltage;
			}
			if (voltage > ch_max[j]) {
				ch_max[j] = voltage;
			}
			ch_sum[j] += voltage;
		}
	}
#endif

	total_samples_processed += num_samples;
	buffers_processed++;

#if STREAMING_CALLBACK_STATS_EVERY_N > 0
	if (buffers_processed % STREAMING_CALLBACK_STATS_EVERY_N == 0)
	{
		float ch_avg[4];
		for (j = 0; j < AD4134_FMC_CH_NO; j++)
		{
			ch_avg[j] = ch_sum[j] / total_samples_processed;
		}

		pr_info("\n--- Buffer %u Statistics ---\n", buffers_processed);
		pr_info("Total samples processed: %llu\n", total_samples_processed);
		for (j = 0; j < AD4134_FMC_CH_NO; j++)
		{
			pr_info("CH%u: Min=%+1.4fV, Max=%+1.4fV, Avg=%+1.4fV\n",
					j, ch_min[j], ch_max[j], ch_avg[j]);
		}
		pr_info("----------------------------\n\n");
	}
#endif

	if (buffers_processed == 1)
	{
#if STREAMING_PRINT_FIRST_SAMPLES
		pr_info("\nFirst 10 samples from buffer 1:\n");
		for (i = 0; i < 10; i++)
		{
			pr_info("Sample %u: ", i);
			for (j = 0; j < AD4134_FMC_CH_NO; j++)
			{
				raw_value = data[i * AD4134_FMC_CH_NO + j];
				if (j != 3)
				{
					pr_info("0x%08X, ", raw_value);
				}
				else
				{
					pr_info("0x%08X\n", raw_value);
				}
			}
		}
		pr_info("\n");
#endif
	}

#if 0
	file_write(data, num_samples * AD4134_FMC_CH_NO * sizeof(uint32_t));
#endif

#if 0
	udp_send(socket, data, num_samples * AD4134_FMC_CH_NO * sizeof(uint32_t));
#endif
}

int main(void)
{
	int32_t ret;
	struct streaming_context *streaming_ctx;
	struct streaming_init_param streaming_init_param;

	struct axi_clkgen_init clkgen_init = {
		.base = 0x44b10000,
		.name = "ad4134_clkgen",
		.parent_rate = AD713x_SPI_ENG_REF_CLK_FREQ_HZ};
	struct axi_clkgen *clkgen;

	struct axi_pwm_init_param axi_pwm_trigger_init = {
		.base_addr = 0x44b00000,
		.ref_clock_Hz = AD713x_SPI_ENG_REF_CLK_FREQ_HZ,
		.channel = 0};
	struct axi_pwm_init_param axi_pwm_odr_init = {
		.base_addr = 0x44b00000,
		.ref_clock_Hz = AD713x_SPI_ENG_REF_CLK_FREQ_HZ,
		.channel = 1};
	struct no_os_pwm_init_param pwm_trigger_init = {
		.period_ns = PWM_TRIGGER_PERIOD_NS,
		.duty_cycle_ns = PWM_TRIGGER_DUTY_NS,
		.phase_ns = PWM_TRIGGER_PHASE_NS,
		.platform_ops = &axi_pwm_ops,
		.extra = &axi_pwm_trigger_init};
	struct no_os_pwm_init_param pwm_odr_init = {
		.period_ns = PWM_ODR_PERIOD_NS,
		.duty_cycle_ns = PWM_ODR_DUTY_NS,
		.phase_ns = PWM_ODR_PHASE_NS,
		.platform_ops = &axi_pwm_ops,
		.extra = &axi_pwm_odr_init};
	struct no_os_pwm_desc *axi_pwm_trigger;
	struct no_os_pwm_desc *axi_pwm;
	uint32_t pwm_period_ns;
	uint32_t pwm_duty_ns;
	uint32_t pwm_phase_ns;

	static struct xil_spi_init_param spi_engine_init_params = {
		.type = SPI_PS,
	};
	struct xil_gpio_init_param gpio_extra_param;

	struct no_os_gpio_init_param gpio_dclkio_1 = {
		.number = GPIO_DCLKIO_1,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param};
	struct no_os_gpio_init_param gpio_dclkmode = {
		.number = GPIO_DCLKMODE,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param};
	struct no_os_gpio_init_param gpio_mode_1 = {
		.number = GPIO_MODE_1,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param};
	struct no_os_gpio_init_param gpio_resetn_1 = {
		.number = GPIO_RESETN_1,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param};
	struct no_os_gpio_init_param gpio_pdn_1 = {
		.number = GPIO_PDN_1,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param};
	struct no_os_gpio_init_param gpio_cs_sync = {
		.number = GPIO_CS_SYNC,
		.platform_ops = &xil_gpio_ops,
		.extra = &gpio_extra_param};
	struct no_os_gpio_desc *ad4134_resetn_1;
	struct no_os_gpio_desc *ad4134_pdn_1;

	struct ad713x_init_param ad713x_init_param_1 = {
		.spi_init_prm = {
			.max_speed_hz = 80000000,
			.chip_select = AD4134_1_SPI_CS,
			.device_id = SPI_DEVICE_ID,
			.mode = NO_OS_SPI_MODE_0,
			.platform_ops = &xil_spi_ops,
			.extra = (void *)&spi_engine_init_params},
		.gpio_mode = &gpio_mode_1,
		.gpio_dclkmode = &gpio_dclkmode,
		.gpio_dclkio = &gpio_dclkio_1,
		.gpio_resetn = &gpio_resetn_1,
		.gpio_pnd = &gpio_pdn_1,
		.gpio_cs_sync = &gpio_cs_sync,
		.mode_master_nslave = false,
		.dclkmode_free_ngated = false,
		.dclkio_out_nin = false,
		.pnd = true,
		.dev_id = ID_AD4134,
		.adc_data_len = ADC_24_BIT_DATA,
		.crc_header = CRC_6,
		.format = QUAD_CH_PO,
		.clk_delay_en = false,
		.spi_common_dev = NULL};
	struct ad713x_dev *ad713x_dev_1;

	uint32_t spi_eng_msg_cmds[1] = {
		CS_LOW << 8 | READ(4)};

	struct spi_engine_init_param spi_eng_init_param = {
		.type = SPI_ENGINE,
		.spi_engine_baseaddr = AD4134_SPI_ENGINE_BASEADDR,
		.cs_delay = 0,
		.data_width = 32,
		.ref_clk_hz = AD713x_SPI_ENG_REF_CLK_FREQ_HZ};
	const struct no_os_spi_init_param spi_eng_init_prm = {
		.chip_select = AD4134_1_SPI_CS,
		.max_speed_hz = SPI_ENGINE_MAX_SPEED_HZ,
		.mode = NO_OS_SPI_MODE_1,
		.platform_ops = &spi_eng_platform_ops,
		.extra = (void *)&spi_eng_init_param};
	struct spi_engine_offload_init_param spi_engine_offload_init_param;
	struct spi_engine_offload_message spi_engine_offload_message;
	struct no_os_spi_desc *spi_eng_desc;
	uint32_t spi_eng_dma_flg = DMA_LAST;

	pr_info("\n\n========================================\n");
	pr_info("AD4134 Continuous Streaming Example\n");
	pr_info("========================================\n\n");

	Xil_ICacheEnable();
	Xil_DCacheEnable();

	ret = axi_clkgen_init(&clkgen, &clkgen_init);
	if (ret != 0)
	{
		pr_err("Failed to initialize clock generator\n");
		return ret;
	}

	ret = axi_clkgen_set_rate(clkgen, 100000000);
	if (ret != 0)
	{
		pr_err("Failed to set clock rate\n");
		return ret;
	}

	ret = no_os_pwm_init(&axi_pwm_trigger, &pwm_trigger_init);
	if (ret != 0)
	{
		pr_err("Failed to initialize trigger PWM\n");
		return ret;
	}

	ret = no_os_pwm_set_period(axi_pwm_trigger, PWM_TRIGGER_PERIOD_NS);
	if (ret != 0)
	{
		pr_err("Failed to set trigger PWM period\n");
		return ret;
	}

	ret = no_os_pwm_set_duty_cycle(axi_pwm_trigger, PWM_TRIGGER_DUTY_NS);
	if (ret != 0)
	{
		pr_err("Failed to set trigger PWM duty\n");
		return ret;
	}

	ret = no_os_pwm_set_phase(axi_pwm_trigger, PWM_TRIGGER_PHASE_NS);
	if (ret != 0)
	{
		pr_err("Failed to set trigger PWM phase\n");
		return ret;
	}

	ret = no_os_pwm_enable(axi_pwm_trigger);
	if (ret != 0)
	{
		pr_err("Failed to enable trigger PWM\n");
		return ret;
	}

	ret = no_os_pwm_init(&axi_pwm, &pwm_odr_init);
	if (ret != 0)
	{
		pr_err("Failed to initialize ODR PWM\n");
		return ret;
	}

	ret = no_os_pwm_set_period(axi_pwm, PWM_ODR_PERIOD_NS);
	if (ret != 0)
	{
		pr_err("Failed to set ODR PWM period\n");
		return ret;
	}

	ret = no_os_pwm_set_duty_cycle(axi_pwm, PWM_ODR_DUTY_NS);
	if (ret != 0)
	{
		pr_err("Failed to set ODR PWM duty\n");
		return ret;
	}

	ret = no_os_pwm_set_phase(axi_pwm, PWM_ODR_PHASE_NS);
	if (ret != 0)
	{
		pr_err("Failed to set ODR PWM phase\n");
		return ret;
	}

	ret = no_os_pwm_enable(axi_pwm);
	if (ret != 0)
	{
		pr_err("Failed to enable ODR PWM\n");
		return ret;
	}

#if STREAMING_PRINT_PWM_CONFIG
	ret = no_os_pwm_get_period(axi_pwm_trigger, &pwm_period_ns);
	if (ret != 0)
	{
		pr_err("Failed to read trigger PWM period\n");
		return ret;
	}

	ret = no_os_pwm_get_duty_cycle(axi_pwm_trigger, &pwm_duty_ns);
	if (ret != 0)
	{
		pr_err("Failed to read trigger PWM duty\n");
		return ret;
	}

	ret = no_os_pwm_get_phase(axi_pwm_trigger, &pwm_phase_ns);
	if (ret != 0)
	{
		pr_err("Failed to read trigger PWM phase\n");
		return ret;
	}

	pr_info("Trigger PWM: period=%u ns duty=%u ns phase=%u ns\n",
			pwm_period_ns, pwm_duty_ns, pwm_phase_ns);

	ret = no_os_pwm_get_period(axi_pwm, &pwm_period_ns);
	if (ret != 0)
	{
		pr_err("Failed to read ODR PWM period\n");
		return ret;
	}

	ret = no_os_pwm_get_duty_cycle(axi_pwm, &pwm_duty_ns);
	if (ret != 0)
	{
		pr_err("Failed to read ODR PWM duty\n");
		return ret;
	}

	ret = no_os_pwm_get_phase(axi_pwm, &pwm_phase_ns);
	if (ret != 0)
	{
		pr_err("Failed to read ODR PWM phase\n");
		return ret;
	}

	pr_info("ODR PWM: period=%u ns duty=%u ns phase=%u ns\n",
			pwm_period_ns, pwm_duty_ns, pwm_phase_ns);
#endif

	gpio_extra_param.device_id = GPIO_DEVICE_ID;
	gpio_extra_param.type = GPIO_PS;

	ret = no_os_gpio_get(&ad4134_resetn_1, &gpio_resetn_1);
	if (ret != 0)
	{
		pr_err("Failed to get RESETN GPIO\n");
		return ret;
	}

	ret = no_os_gpio_get(&ad4134_pdn_1, &gpio_pdn_1);
	if (ret != 0)
	{
		pr_err("Failed to get PDN GPIO\n");
		return ret;
	}

	ret = no_os_gpio_direction_output(ad4134_resetn_1, NO_OS_GPIO_HIGH);
	if (ret != 0)
	{
		pr_err("Failed to set RESETN direction\n");
		return ret;
	}

	ret = no_os_gpio_direction_output(ad4134_pdn_1, NO_OS_GPIO_HIGH);
	if (ret != 0)
	{
		pr_err("Failed to set PDN direction\n");
		return ret;
	}

	pr_info("Initializing AD4134 device...\n");
	ret = ad713x_init(&ad713x_dev_1, &ad713x_init_param_1);
	if (ret != 0)
	{
		pr_err("Failed to initialize AD4134\n");
		return -1;
	}

	for (int ch = 0; ch < AD4134_FMC_CH_NO; ch++)
	{
		ret = ad713x_dig_filter_sel_ch(ad713x_dev_1, FIR, ch);
		if (ret != 0)
		{
			pr_err("Failed to set FIR filter for channel %d\n", ch);
			return -1;
		}
		ret = ad713x_wideband_bw_sel(ad713x_dev_1, ch, 0);
		if (ret != 0)
		{
			pr_err("Failed to set wideband BW for channel %d\n", ch);
			return -1;
		}
	}

	no_os_mdelay(1000);

	pr_info("Initializing SPI Engine...\n");
	ret = no_os_spi_init(&spi_eng_desc, &spi_eng_init_prm);
	if (ret != 0)
	{
		pr_err("Failed to initialize SPI Engine\n");
		return -1;
	}

	spi_engine_offload_init_param.rx_dma_baseaddr = AD4134_DMA_BASEADDR;
	spi_engine_offload_init_param.offload_config = OFFLOAD_RX_EN;
	spi_engine_offload_init_param.dma_flags = spi_eng_dma_flg;

	ret = spi_engine_offload_init(spi_eng_desc, &spi_engine_offload_init_param);
	if (ret != 0)
	{
		pr_err("Failed to initialize SPI Engine offload\n");
		return -1;
	}

	spi_engine_offload_message.commands = spi_eng_msg_cmds;
	spi_engine_offload_message.no_commands = NO_OS_ARRAY_SIZE(spi_eng_msg_cmds);
	spi_engine_offload_message.commands_data = NULL;
	spi_engine_offload_message.tx_addr = 0xA000000;

	pr_info("Synchronizing AD4134 channels...\n");
	ret = ad713x_channel_sync(ad713x_dev_1);
	if (ret != 0)
	{
		pr_err("Failed to synchronize channels\n");
		return ret;
	}

	pr_info("\n--- Initializing Continuous Streaming ---\n");

	streaming_init_param.spi_desc = spi_eng_desc;
	streaming_init_param.offload_msg = &spi_engine_offload_message;
	streaming_init_param.samples_per_transfer = SAMPLES_PER_TRANSFER;
	streaming_init_param.data_callback = data_processing_callback;
	streaming_init_param.user_data = NULL;

	ret = streaming_init(&streaming_ctx, &streaming_init_param);
	if (ret != 0)
	{
		pr_err("Failed to initialize streaming context\n");
		return ret;
	}

	ret = streaming_start(streaming_ctx);
	if (ret != 0)
	{
		pr_err("Failed to start streaming\n");
		return ret;
	}

	pr_info("\n========================================\n");
	pr_info("Continuous streaming active!\n");
	pr_info("Processing buffers...\n");
	pr_info("Press CTRL+C to stop (or will auto-stop after %u buffers)\n",
			MAX_STREAMING_BUFFERS);
	pr_info("========================================\n\n");

	while (buffers_processed < MAX_STREAMING_BUFFERS)
	{
		ret = streaming_process(streaming_ctx);

		if (ret == -2)
		{
			pr_info("Stop requested\n");
			break;
		}
		else if (ret != 0)
		{
			pr_err("Streaming error: %d\n", ret);
			break;
		}

		if (STREAMING_STATS_EVERY_N > 0 &&
			buffers_processed % STREAMING_STATS_EVERY_N == 0 &&
			buffers_processed > 0)
		{
			streaming_print_stats(streaming_ctx);
		}
	}

	pr_info("\n--- Stopping and cleaning up ---\n");

	ret = streaming_stop(streaming_ctx);
	if (ret != 0)
	{
		pr_err("Warning: Error stopping streaming: %d\n", ret);
	}

#if STREAMING_PRINT_FINAL_STATS
	streaming_print_final_stats(streaming_ctx);
#endif

	ret = streaming_remove(streaming_ctx);
	if (ret != 0)
	{
		pr_err("Warning: Error removing streaming context: %d\n", ret);
	}

	pr_info("\n========================================\n");
	pr_info("Final Summary\n");
	pr_info("========================================\n");
	pr_info("Total samples processed: %llu\n", total_samples_processed);
	pr_info("Total buffers processed: %u\n", buffers_processed);
	pr_info("Average samples/buffer: %llu\n",
			total_samples_processed / (buffers_processed ? buffers_processed : 1));
	pr_info("========================================\n\n");

	ad713x_remove(ad713x_dev_1);
	no_os_spi_remove(spi_eng_desc);
	no_os_pwm_remove(axi_pwm_trigger);
	no_os_pwm_remove(axi_pwm);
	no_os_gpio_remove(ad4134_resetn_1);
	no_os_gpio_remove(ad4134_pdn_1);
	axi_clkgen_remove(clkgen);

	Xil_DCacheDisable();
	Xil_ICacheDisable();

	pr_info("Application complete. Goodbye!\n");

	return 0;
}
