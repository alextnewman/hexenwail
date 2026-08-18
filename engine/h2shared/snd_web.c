#include "quakedef.h"
#include "snd_sys.h"

#include <emscripten/emscripten.h>

#define WEB_AUDIO_RATE		44100
#define WEB_AUDIO_SAMPLES	16384

static dma_t web_dma;
static qboolean web_audio_started;

static qboolean WebAudio_Init (dma_t *dma)
{
	memset(&web_dma, 0, sizeof(web_dma));
	web_dma.channels = 2;
	web_dma.samplebits = 16;
	web_dma.speed = WEB_AUDIO_RATE;
	web_dma.samples = WEB_AUDIO_SAMPLES;
	web_dma.submission_chunk = 1;
	web_dma.buffer = Hunk_AllocName(WEB_AUDIO_SAMPLES * sizeof(short), "webaudio");
	if (!web_dma.buffer)
		return false;
	memset(web_dma.buffer, 0, WEB_AUDIO_SAMPLES * sizeof(short));
	*dma = web_dma;
	shm = dma;

	web_audio_started = EM_ASM_INT({
		try {
			const AudioContext = globalThis.AudioContext || globalThis.webkitAudioContext;
			if (!AudioContext) return 0;
			const context = new AudioContext({sampleRate: $2});
			const node = context.createScriptProcessor(1024, 0, 2);
			const base = $0 >> 1;
			const mask = $1 - 1;
			let position = 0;
			node.onaudioprocess = event => {
				const left = event.outputBuffer.getChannelData(0);
				const right = event.outputBuffer.getChannelData(1);
				const heap = HEAP16;
				for (let i = 0; i < left.length; ++i) {
					left[i] = heap[base + (position & mask)] / 32768;
					position++;
					right[i] = heap[base + (position & mask)] / 32768;
					position++;
				}
				Module.webAudioPosition = position & mask;
			};
			node.connect(context.destination);
			Module.webAudioContext = context;
			Module.webAudioNode = node;
			Module.webAudioPosition = 0;
			addEventListener('pointerdown', () => context.resume(), {passive: true});
			addEventListener('keydown', () => context.resume(), {passive: true});
			return 1;
		} catch (error) {
			console.error('WebAudio initialization failed', error);
			return 0;
		}
	}, web_dma.buffer, WEB_AUDIO_SAMPLES, WEB_AUDIO_RATE);

	if (!web_audio_started)
	{
		shm = NULL;
		return false;
	}
	return true;
}

static void WebAudio_Shutdown (void)
{
	if (!web_audio_started)
		return;
	EM_ASM({
		if (Module.webAudioNode) Module.webAudioNode.disconnect();
		if (Module.webAudioContext) Module.webAudioContext.close();
		delete Module.webAudioNode;
		delete Module.webAudioContext;
	});
	web_audio_started = false;
	shm = NULL;
}

static int WebAudio_GetDMAPos (void)
{
	return EM_ASM_INT({ return Module.webAudioPosition || 0; });
}

static void WebAudio_Lock (void) {}
static void WebAudio_Submit (void) {}
static void WebAudio_Block (void)
{
	EM_ASM({ if (Module.webAudioContext) Module.webAudioContext.suspend(); });
}
static void WebAudio_Unblock (void)
{
	EM_ASM({ if (Module.webAudioContext) Module.webAudioContext.resume(); });
}

snd_driver_t snddrv_web = {
	WebAudio_Init,
	WebAudio_Shutdown,
	WebAudio_GetDMAPos,
	WebAudio_Lock,
	WebAudio_Submit,
	WebAudio_Block,
	WebAudio_Unblock,
	"WebAudio",
	SNDDRV_ID_WEB,
	false,
	NULL
};
