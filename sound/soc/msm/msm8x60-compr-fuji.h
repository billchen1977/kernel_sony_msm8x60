/*
 * Copyright (c) 2011, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _MSM_8X60_COMPR_FUJI_H
#define _MSM_8X60_COMPR_FUJI_H

#include <sound/apr_audio.h>
#include <sound/q6asm.h>
#include <sound/compress_params.h>
#include <sound/compress_offload.h>
#include <sound/compress_driver.h>

struct audio_locks {
	spinlock_t event_lock;
	wait_queue_head_t read_wait;
	wait_queue_head_t write_wait;
	wait_queue_head_t eos_wait;
	wait_queue_head_t enable_wait;
	wait_queue_head_t flush_wait;
};

struct msm_audio {
	struct snd_pcm_substream *substream;
	unsigned int pcm_size;
	unsigned int pcm_count;
	unsigned int pcm_irq_pos;       /* IRQ position */
	uint16_t source; /* Encoding source bit mask */

	struct audio_client *audio_client;

	uint16_t session_id;

	uint32_t samp_rate;
	uint32_t channel_mode;
	uint32_t dsp_cnt;

	int abort; /* set when error, like sample rate mismatch */

	int enabled;
	int close_ack;
	int cmd_ack;
	atomic_t open;
	atomic_t start;
	atomic_t out_count;
	atomic_t in_count;
	atomic_t out_needed;
	atomic_t eos;
	int out_head;
	int periods;
	int mmap_flag;
	atomic_t pending_buffer;
	int cmd_interrupt;
};

struct output_meta_data_st {
	uint32_t meta_data_length;
	uint32_t frame_size;
	uint32_t timestamp_lsw;
	uint32_t timestamp_msw;
	uint32_t reserved[12];
};

struct compr_info {
	struct snd_compr_caps compr_cap;
	struct snd_compr_codec_caps codec_caps;
	struct snd_compr_params codec_param;
};

struct compr_audio {
	struct msm_audio prtd;
	struct compr_info info;
	uint32_t codec;
};

#endif /*_MSM_8X60_COMPR_FUJI_H*/
