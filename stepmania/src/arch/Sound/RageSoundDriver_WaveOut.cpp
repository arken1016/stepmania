#include "../../stdafx.h"
#include "RageSoundDriver_WaveOut.h"

#pragma comment(lib, "winmm.lib")

#include "../../RageTimer.h"
#include "../../RageLog.h"
#include "../../RageSound.h"
#include "../../RageUtil.h"
#include "../../tls.h"

#include "SDL.h"

/* samples */
const int channels = 2;
const int samplesize = channels*2; /* 16-bit */
const int samplerate = 44100;
const int buffersize_frames = 2048*8;	/* in frames */
const int buffersize = buffersize_frames * samplesize; /* in bytes */

const int num_chunks = 8;
const int chunksize_frames = buffersize_frames / num_chunks;
const int chunksize = buffersize / num_chunks;

int RageSound_WaveOut::MixerThread_start(void *p)
{
	((RageSound_WaveOut *) p)->MixerThread();
	return 0;
}

void RageSound_WaveOut::MixerThread()
{
	InitThreadData("Mixer thread");
	VDCHECKPOINT;

	/* SOUNDMAN will be set once RageSoundManager's ctor returns and
	 * assigns it; we might get here before that happens, though. */
	while(!SOUNDMAN && !shutdown) Sleep(10);

	while(!shutdown) {
		while(GetPCM())
			;

		if(WaitForSingleObject(sound_event, 10) != WAIT_OBJECT_0)
			continue;
	}
}

bool RageSound_WaveOut::GetPCM()
{
	LockMutex L(SOUNDMAN->lock);

	/* Look for a free buffer. */
	int b;
	for(b = 0; b < num_chunks; ++b)
		if(buffers[b].dwFlags & WHDR_DONE) break;

	if(b == num_chunks) return false;

	/* Create a 32-bit buffer to mix sounds. */
	static Sint16 *buf = NULL;
	int bufsize = chunksize_frames * channels;
	if(!buf)
	{
		buf = new Sint16[bufsize];
	}
	memset(buf, 0, bufsize*sizeof(Uint16));
	memset(buffers[b].lpData, 0, bufsize*sizeof(Uint16));

	for(unsigned i = 0; i < sounds.size(); ++i)
	{
		if(sounds[i]->stopping)
		{
			if(sounds[i]->flush_bufs)
				sounds[i]->flush_bufs--;
		} else {
			/* Call the callback. */
			unsigned got = sounds[i]->snd->GetPCM((char *) buf, chunksize, last_cursor_pos);

			SOUNDMAN->MixAudio(
				(Uint8 *) buffers[b].lpData, (Uint8 *) buf, got, SDL_MIX_MAXVOLUME/2);

			if(got < chunksize)
			{
				/* This sound is finishing. */
				sounds[i]->stopping = true;
				sounds[i]->flush_bufs = num_chunks;
			}
		}
	}

	MMRESULT ret = waveOutWrite(wo, &buffers[b], sizeof(buffers[b]));
  	if(ret != MMSYSERR_NOERROR)
		throw "blah";

	/* Increment last_cursor_pos to point at where the data we're about to
	 * ask for will actually be played. */
	last_cursor_pos += chunksize_frames;

	return true;
}

void RageSound_WaveOut::StartMixing(RageSound *snd)
{
	sound *s = new sound;
	s->snd = snd;

	SDL_LockAudio();
	sounds.push_back(s);
	SDL_UnlockAudio();
}

void RageSound_WaveOut::Update(float delta)
{
	LockMutex L(SOUNDMAN->lock);

	/* SoundStopped might erase sounds out from under us, so make a copy
	 * of the sound list. */
	vector<sound *> snds = sounds;
	for(unsigned i = 0; i < snds.size(); ++i)
	{
		if(sounds[i]->stopping && !sounds[i]->flush_bufs)
		{
			/* This sound is done. */
			snds[i]->snd->SoundStopped();
		}
	}
}

void RageSound_WaveOut::StopMixing(RageSound *snd)
{
	LockMutex L(SOUNDMAN->lock);

	/* Find the sound. */
	unsigned i;
	for(i = 0; i < sounds.size(); ++i)
		if(sounds[i]->snd == snd) break;
	if(i == sounds.size())
	{
		LOG->Trace("not stopping a sound because it's not playing");
		return;
	}

	delete sounds[i];
	sounds.erase(sounds.begin()+i, sounds.begin()+i+1);
}

int RageSound_WaveOut::GetPosition(const RageSound *snd) const
{
	LockMutex L(SOUNDMAN->lock);

	MMTIME tm;
	tm.wType = TIME_SAMPLES;
	MMRESULT ret = waveOutGetPosition(wo, &tm, sizeof(tm));

	return tm.u.sample;
}

RageSound_WaveOut::RageSound_WaveOut()
{
	shutdown = false;
	last_cursor_pos = 0;

	sound_event = CreateEvent(NULL, false, true, NULL);

	WAVEFORMATEX fmt;

	fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = channels;
	fmt.cbSize = 0;
	fmt.nSamplesPerSec = samplerate;
	fmt.wBitsPerSample = 16;
	fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
	fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

	MMRESULT ret = waveOutOpen(&wo, WAVE_MAPPER, &fmt, 
		(DWORD_PTR) sound_event, NULL, CALLBACK_EVENT);

	if(ret != MMSYSERR_NOERROR)
		throw "blah";

	for(int b = 0; b < num_chunks; ++b)
	{
		memset(&buffers[b], 0, sizeof(buffers[b]));
		buffers[b].dwBufferLength = chunksize;
		buffers[b].lpData = new char[chunksize];
		ret = waveOutPrepareHeader(wo, &buffers[b], sizeof(buffers[b]));
		if(ret != MMSYSERR_NOERROR)
			throw "blah";
		buffers[b].dwFlags |= WHDR_DONE;
	}
	
	MixerThreadPtr = SDL_CreateThread(MixerThread_start, this);
}

RageSound_WaveOut::~RageSound_WaveOut()
{
	/* Signal the mixing thread to quit. */
	shutdown = true;
	LOG->Trace("Shutting down mixer thread ...");
	SDL_WaitThread(MixerThreadPtr, NULL);
	LOG->Trace("Mixer thread shut down.");

	CloseHandle(sound_event);
	waveOutClose(wo);

	for(int b = 0; b < num_chunks; ++b)
		delete [] buffers[b].lpData;
}

float RageSound_WaveOut::GetPlayLatency() const
{
	return (1.0f / samplerate) * (buffersize_frames - chunksize_frames);
}

/*
 * Copyright (c) 2002 by the person(s) listed below.  All rights reserved.
 *
 * Glenn Maynard
 */
