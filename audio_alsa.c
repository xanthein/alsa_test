#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/types.h>
#include <alsa/asoundlib.h>

#define audio_device "default"

struct audio_thread_data {
	snd_pcm_t *pcm;
	unsigned int frame_bits;
	unsigned int rate;
	size_t buffer_size;
	size_t period_frames;
	size_t period_size;
	bool thread_exit;
	pthread_t id;
	pthread_cond_t threadCond;
	pthread_mutex_t CondLock;
	pthread_mutex_t BufferLock;
	int write_pos;
	int read_pos;
	uint8_t *audio_buffer;
	bool buffer_empty;
};

struct audio_thread_data g_audio_data;

bool g_audio_skipframe = false;

size_t get_write_available()
{
	if (g_audio_data.write_pos > g_audio_data.read_pos)
		return g_audio_data.buffer_size - (g_audio_data.write_pos - g_audio_data.read_pos);
	else if (g_audio_data.write_pos < g_audio_data.read_pos)
		return (g_audio_data.read_pos - g_audio_data.write_pos);
	else
		return g_audio_data.buffer_empty?g_audio_data.buffer_size:0;
}

size_t get_read_available()
{
	if (g_audio_data.write_pos > g_audio_data.read_pos)
		return g_audio_data.write_pos - g_audio_data.read_pos;
	else if (g_audio_data.write_pos < g_audio_data.read_pos)
		return g_audio_data.buffer_size - (g_audio_data.read_pos - g_audio_data.write_pos);
	else
		return g_audio_data.buffer_empty?0:g_audio_data.buffer_size;
}

size_t audio_write(const int16_t *buf, unsigned frames)
{
	int write_available = 0;
	int data_size = frames * 4;
	
	pthread_mutex_lock(&g_audio_data.BufferLock);

	size_t written = 0;
	while (written < data_size) {
		write_available = get_write_available();

		if (write_available == 0) {
			pthread_mutex_unlock(&g_audio_data.BufferLock);
			pthread_mutex_lock(&g_audio_data.CondLock);
			pthread_cond_wait(&g_audio_data.threadCond, &g_audio_data.CondLock);
			pthread_mutex_unlock(&g_audio_data.CondLock);
		}
		else {
			size_t size_to_write = (data_size - written) > write_available ? write_available : (data_size - written);
			if (g_audio_data.write_pos + size_to_write >= g_audio_data.buffer_size) {
				int data_to_write = g_audio_data.buffer_size - g_audio_data.write_pos;
				memcpy(g_audio_data.audio_buffer + g_audio_data.write_pos, (uint8_t *)buf, data_to_write);
				memcpy(g_audio_data.audio_buffer, (uint8_t *)buf + data_to_write, (size_to_write - data_to_write));
				g_audio_data.write_pos = size_to_write - data_to_write;
			}
			else {
				memcpy(g_audio_data.audio_buffer + g_audio_data.write_pos, (uint8_t *)buf, size_to_write);
				g_audio_data.write_pos += size_to_write;
			}
			pthread_mutex_unlock(&g_audio_data.BufferLock);
			written += size_to_write;
			g_audio_data.buffer_empty = false;
		}
	}

	return data_size;
}

void thread_audio(void *data_)
{
	struct audio_thread_data *data = (struct audio_thread_data *)data_;
	int read_available = 0;
	int size_to_read = 0;
	int frames;
	uint8_t *buf = (uint8_t *)calloc(1, data->period_size);

	while(!data->thread_exit) {
		pthread_mutex_lock(&data->BufferLock);
		read_available = get_read_available();

		if (data->period_size > read_available)
			printf("read_available = %d\n", read_available);
		size_to_read = data->period_size > read_available?read_available:data->period_size;

		if (data->write_pos > data->read_pos)
			memcpy(buf, (data->audio_buffer + data->read_pos), size_to_read);
		else if (data->write_pos < data->read_pos) {
			int size_to_end = data->buffer_size - data->read_pos;

			if (size_to_end > size_to_read)
				memcpy(buf, (data->audio_buffer + data->read_pos), size_to_read);
			else {
				memcpy(buf, (data->audio_buffer + data->read_pos), size_to_end);
				memcpy(buf+size_to_end, data->audio_buffer, size_to_read - size_to_end);
			}
		}

		if (data->period_size - size_to_read > 0)
			memset(buf + size_to_read, 0, data->period_size - size_to_read);
			
		data->read_pos += size_to_read;
		if (data->read_pos >= data->buffer_size)
			data->read_pos -= data->buffer_size;

		if (data->write_pos == data->read_pos)
			data->buffer_empty = true;
	
		pthread_cond_signal(&data->threadCond);
		pthread_mutex_unlock(&data->BufferLock);
		
		frames = snd_pcm_writei(g_audio_data.pcm, buf, data->period_frames);

		if (frames == -EPIPE || frames == -EINTR || frames == -ESTRPIPE) {
			printf("error\n");
			printf("frames = %d\n", frames);
			if (snd_pcm_recover(g_audio_data.pcm, frames, 1) < 0) {
				printf("recover error\n");
			}
			continue;
		}
		else if (frames < 0) {
			printf("unknown error\n");
		}
	}

	free(buf);
}

int audio_init(unsigned int rate, unsigned int latency)
{
	snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_hw_params_t *params = NULL;
	snd_pcm_sw_params_t *sw_params = NULL;
	unsigned int latency_usec = latency* 1000;
	unsigned channels = 2;
	unsigned periods = 4;
	unsigned orig_rate = rate;
	if(snd_pcm_open(&g_audio_data.pcm, audio_device, SND_PCM_STREAM_PLAYBACK, 0) < 0)
		return -1;

	if(snd_pcm_hw_params_malloc(&params) < 0)
		goto error;
	if (snd_pcm_hw_params_any(g_audio_data.pcm, params) < 0)
		goto error;
	if (snd_pcm_hw_params_set_access(g_audio_data.pcm, params,
		SND_PCM_ACCESS_RW_INTERLEAVED) < 0)
		goto error;
	
	g_audio_data.frame_bits = snd_pcm_format_physical_width(format) * 2;
	printf("g_frame_bits = %d\n", g_audio_data.frame_bits);
	if (snd_pcm_hw_params_set_format(g_audio_data.pcm, params, format) < 0)
		goto error;

	if (snd_pcm_hw_params_set_channels(g_audio_data.pcm, params, channels) < 0)
		goto error;

	snd_pcm_hw_params_set_rate_resample(g_audio_data.pcm, params, 1 );
	if (snd_pcm_hw_params_set_rate_near(g_audio_data.pcm, params, &rate, 0) < 0)
		goto error;

	if (rate != orig_rate)
		printf("got new rate\n");

	printf("rate = %d\n", rate);
	g_audio_data.rate = rate;

	if (snd_pcm_hw_params_set_buffer_time_near(
		g_audio_data.pcm, params, &latency_usec, NULL) < 0)
		goto error;

	if (snd_pcm_hw_params_set_periods_near(
		g_audio_data.pcm, params, &periods, NULL) < 0)
		goto error;

	if (snd_pcm_hw_params(g_audio_data.pcm, params) < 0)
		goto error;

	if (snd_pcm_hw_params_get_period_size(params, &buffer_size, NULL))
		snd_pcm_hw_params_get_period_size_min(params, &buffer_size, NULL);

	g_audio_data.period_frames = buffer_size;
	g_audio_data.period_size = snd_pcm_frames_to_bytes(g_audio_data.pcm, g_audio_data.period_frames);
	
	printf("Period frame: %d frames\n", (int)g_audio_data.period_frames);
	printf("Period size: %d bytes\n", (int)g_audio_data.period_size);

	if (snd_pcm_hw_params_get_buffer_size(params, &buffer_size))
		snd_pcm_hw_params_get_buffer_size_max(params, &buffer_size);

	printf("Buffer size: %d frames\n", (int)buffer_size);

	g_audio_data.buffer_size = snd_pcm_frames_to_bytes(g_audio_data.pcm, buffer_size);
	g_audio_data.audio_buffer = (uint8_t *)calloc(1, g_audio_data.buffer_size);

	printf("Buffer size: %d bytes\n", (int)g_audio_data.buffer_size);

	g_audio_data.buffer_empty = true;

   if (snd_pcm_sw_params_malloc(&sw_params) < 0)
      goto error;

   if (snd_pcm_sw_params_current(g_audio_data.pcm, sw_params) < 0)
      goto error;

   if (snd_pcm_sw_params_set_start_threshold(
            g_audio_data.pcm, sw_params, buffer_size / 2) < 0)
      goto error;

   if (snd_pcm_sw_params(g_audio_data.pcm, sw_params) < 0)
      goto error;

	snd_pcm_sw_params_free(sw_params);
	snd_pcm_hw_params_free(params);

	pthread_cond_init(&g_audio_data.threadCond, NULL);
	pthread_mutex_init(&g_audio_data.CondLock, NULL);
	pthread_mutex_init(&g_audio_data.BufferLock, NULL);

	g_audio_data.thread_exit = false;
	
	if (pthread_create(&g_audio_data.id, NULL, (void *)thread_audio, &g_audio_data))
		printf("failed to create thread\n");

	return 0;

error:
	printf("Failed to initialize...\n");
	if (params)
	snd_pcm_hw_params_free(params);
   if (sw_params)
      snd_pcm_sw_params_free(sw_params);

	if (g_audio_data.pcm)
	{
		snd_pcm_close(g_audio_data.pcm);
		snd_config_update_free_global();
	}

	return -1;
}

void audio_deinit()
{
	if (g_audio_data.pcm)
	{
		g_audio_data.thread_exit = true;
		pthread_join(g_audio_data.id, NULL);
		snd_pcm_drop(g_audio_data.pcm);
		snd_pcm_close(g_audio_data.pcm);
		snd_config_update_free_global();
	}

	if (g_audio_data.audio_buffer)
		free(g_audio_data.audio_buffer);
}
