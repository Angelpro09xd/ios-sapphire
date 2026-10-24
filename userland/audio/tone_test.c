/* tone_test [seconds]: a 440 Hz tone through iOS's own audio stack (AudioQueue -> mediaserverd -> RedwoodAudio) */
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static double phase;

static void
fill(void *ud, AudioQueueRef q, AudioQueueBufferRef b)
{
	int16_t *s = (int16_t *)b->mAudioData;
	UInt32 n = b->mAudioDataBytesCapacity / 4;
	for (UInt32 i = 0; i < n; i++) {
		int16_t v = (int16_t)(sin(phase) * 8000);
		phase += 2 * M_PI * 440 / 48000;
		s[2 * i] = s[2 * i + 1] = v;
	}
	b->mAudioDataByteSize = n * 4;
	AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

int
main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 3;
	AudioStreamBasicDescription f = { 48000, kAudioFormatLinearPCM, kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
		4, 1, 4, 2, 16, 0 };
	AudioQueueRef q;
	OSStatus st = AudioQueueNewOutput(&f, fill, NULL, NULL, NULL, 0, &q);
	printf("AudioQueueNewOutput: %d\n", (int)st);
	if (st) {
		return 1;
	}
	for (int i = 0; i < 3; i++) {
		AudioQueueBufferRef b;
		AudioQueueAllocateBuffer(q, 4800 * 4, &b);
		fill(NULL, q, b);
	}
	st = AudioQueueStart(q, NULL);
	printf("AudioQueueStart: %d\n", (int)st);
	for (int i = 0; i < secs * 10; i++) {
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
	}
	AudioQueueStop(q, true);
	printf("done\n");
	return 0;
}
