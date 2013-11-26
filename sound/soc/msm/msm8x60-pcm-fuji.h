/*
 * Copyright (C) 2008 Google, Inc.
 * Copyright (C) 2008 HTC Corporation
 * Copyright (c) 2010-2011, The Linux Foundation. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can find it at http://www.fsf.org.
 */

#ifndef _MSM_8X60_PCM_FUJI_H
#define _MSM_8X60_PCM_FUJI_H

#include <sound/apr_audio.h>
#include <sound/q6asm.h>

struct audio_locks {
	wait_queue_head_t read_wait;
	wait_queue_head_t write_wait;
	wait_queue_head_t eos_wait;
};

struct msm_audio {
	struct snd_pcm_substream *substream;
	unsigned int pcm_size;
	unsigned int pcm_count;
	unsigned int pcm_irq_pos;       /* IRQ position */

	struct audio_client *audio_client;

	int enabled;
	int cmd_ack;
	atomic_t start;
	atomic_t out_count;
	atomic_t in_count;
	atomic_t out_needed;
	int periods;
	int mmap_flag;
	
	struct audio_locks the_locks;
	uint32_t in_frame_info[8][2];
};

/* platform data */
extern struct snd_soc_platform_driver msm_soc_platform;

#endif /*_MSM_8X60_PCM_FUJI_H*/
