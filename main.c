#include <stdio.h>
#include <unistd.h>

#define RATE 44100
#define READ_LEN RATE/60 * 2 * 2
#define LATENCY 200

int main()
{
	FILE *wave = fopen("test.wav", "rb");
	char buffer[READ_LEN];
	int data_read = 1;

	audio_init(RATE, LATENCY);

	while(data_read > 0) {
		data_read = fread(buffer, 1, READ_LEN, wave);
		if(data_read > 0) {
			audio_write((unsigned short *)buffer, data_read/4);
			//usleep(1000000/60);
			usleep(16500);
		}
	}

	audio_deinit();

	fclose(wave);
}
