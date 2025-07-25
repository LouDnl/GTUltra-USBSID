//
// GTUltra sound routines (GOATTRACKER sound routines)
//

#define GSOUND_C

//#define __WIN32__

#ifdef __WIN32__
#include <windows.h>
#endif

#include "goattrk2.h"
#include "usbsid/USBSIDInterface.h" // TODO: FINISH

extern void JPSoundMixer(Sint32 *dest, unsigned samples);

// General / reSID output
int playspeed;
int usehardsid = 0;
int lefthardsid = 0;
int righthardsid = 0;
int usecatweasel = 0;
int useusbsid = 0; // NOTE: CHANGED
int initted = 0;
int firsttimeinit = 1;
unsigned framerate = PALFRAMERATE;
Sint16 *sid0buffer = NULL;
Sint16 *sid1buffer = NULL;
Sint16 *sid2buffer = NULL;
Sint16 *sid3buffer = NULL;
Sint16 *tempbuffer = NULL;

FILE *writehandle = NULL;
SDL_TimerID timer = 0;

void sound_playrout(void);
void sound_mixer(Sint32 *dest, unsigned samples);
Uint32 sound_timer(Uint32 interval, void *para);

#ifdef __WIN32__

// Win32 HardSID output
typedef void (CALLBACK* lpWriteToHardSID)(Uint8 DeviceID, Uint8 SID_reg, Uint8 Data);
typedef Uint8(CALLBACK* lpReadFromHardSID)(Uint8 DeviceID, Uint8 SID_reg);
typedef void (CALLBACK* lpInitHardSID_Mapper)(void);
typedef void (CALLBACK* lpMuteHardSID_Line)(int Mute);
typedef void (CALLBACK* lpHardSID_Delay)(Uint8 DeviceID, Uint16 Cycles);
typedef void (CALLBACK* lpHardSID_Write)(Uint8 DeviceID, Uint16 Cycles, Uint8 SID_reg, Uint8 Data);
typedef void (CALLBACK* lpHardSID_Flush)(Uint8 DeviceID);
typedef void (CALLBACK* lpHardSID_SoftFlush)(Uint8 DeviceID);
lpWriteToHardSID WriteToHardSID = NULL;
lpReadFromHardSID ReadFromHardSID = NULL;
lpInitHardSID_Mapper InitHardSID_Mapper = NULL;
lpMuteHardSID_Line MuteHardSID_Line = NULL;
lpHardSID_Delay HardSID_Delay = NULL;
lpHardSID_Write HardSID_Write = NULL;
lpHardSID_Flush HardSID_Flush = NULL;
lpHardSID_SoftFlush HardSID_SoftFlush = NULL;
HINSTANCE hardsiddll = 0;
int dll_initialized = FALSE;
// Cycle-exact HardSID support
int cycleexacthardsid = FALSE;
SDL_Thread* playerthread = NULL;
SDL_mutex* flushmutex = NULL;
volatile int runplayerthread = FALSE;
volatile int flushplayerthread = FALSE;
volatile int suspendplayroutine = FALSE;
int sound_thread(void *userdata);

void InitHardDLL(void);

// Win32 CatWeasel MK3 PCI output
#define SID_SID_PEEK_POKE   CTL_CODE(FILE_DEVICE_SOUND,0x0800UL + 1,METHOD_BUFFERED,FILE_ANY_ACCESS)
HANDLE catweaselhandle;

#else

// Unix HardSID & CatWeasel output
int lefthardsidfd = -1;
int righthardsidfd = -1;
int catweaselfd = -1;

#endif

#ifndef __WIN32__
#define TRUE 1
#define FALSE 0
#endif

// USBSID-Pico output // NOTE: CHANGED
USBSIDitf usbsiddev;
SDL_Thread* usbsidthread = NULL;
SDL_mutex* flushusbsidmutex = NULL;
int cycleexactusbsid = FALSE;
volatile int runusbsidthread = FALSE;
volatile int flushusbsidthread = FALSE;
volatile int suspendusbsidroutine = FALSE;
int usbsid_sound_thread(void *userdata);

int sound_init(unsigned b, unsigned mr, unsigned writer,
	unsigned hardsid, unsigned m, unsigned ntsc,
	unsigned multiplier, unsigned catweasel, unsigned usbsid, // NOTE: CHANGED
	unsigned interpolate, unsigned customclockrate,
	unsigned reinit)
{
	int c;
	// printf("DEBUG:\nb: %u, mr: %u, writer %u,\nhardsid: %u, m: %u, ntsc %u, multiplier: %u,\ncatweasel: %u, usbsid: %u, interpolate: %u,\ncustomclockrate: %u, reinit: %u\n",
	// 	b, mr, writer,
	// 	hardsid, m, ntsc,
	// 	multiplier, catweasel, usbsid,
	// 	interpolate, customclockrate,
	// 	reinit);
#ifdef __WIN32__
	if (!flushmutex)
		flushmutex = SDL_CreateMutex();
#endif

	sound_uninit(reinit);

	if (multiplier)
	{
		if (ntsc)
		{
			framerate = NTSCFRAMERATE * multiplier;
			snd_bpmtempo = 150 * multiplier;
		}
		else
		{
			framerate = PALFRAMERATE * multiplier;
			snd_bpmtempo = 125 * multiplier;
		}
	}
	else
	{
		if (ntsc)
		{
			framerate = NTSCFRAMERATE / 2;
			snd_bpmtempo = 150 / 2;
		}
		else
		{
			framerate = PALFRAMERATE / 2;
			snd_bpmtempo = 125 / 2;
		}
	}

	if (hardsid)
	{
		lefthardsid = (hardsid & 0xf) - 1;
		righthardsid = ((hardsid >> 4) & 0xf) - 1;

		if ((righthardsid == lefthardsid) || (righthardsid < 0))
			righthardsid = lefthardsid + 1;

#ifdef __WIN32__
		InitHardDLL();
		if (dll_initialized)
		{
			usehardsid = hardsid;

			for (c = 0; c < NUMSIDREGS; c++)
			{
				sidreg[c] = 0;
				WriteToHardSID(lefthardsid, c, 0x00);
				WriteToHardSID(righthardsid, c, 0x00);
			}
			MuteHardSID_Line(FALSE);
		}
		else return 0;
		if (!cycleexacthardsid)
		{
			timer = SDL_AddTimer(1000 / framerate, sound_timer, NULL);
		}
		else
		{
			runplayerthread = TRUE;
			playerthread = SDL_CreateThread(sound_thread, NULL, NULL);
			if (!playerthread) return 0;
		}
#else
		{
			char filename[80];
			sprintf(filename, "/dev/sid%d", lefthardsid);
			lefthardsidfd = open(filename, O_WRONLY, S_IREAD | S_IWRITE);
			sprintf(filename, "/dev/sid%d", righthardsid);
			righthardsidfd = open(filename, O_WRONLY, S_IREAD | S_IWRITE);
			if ((lefthardsidfd >= 0) && (righthardsidfd >= 0))
			{
				usehardsid = hardsid;
				for (c = 0; c < NUMSIDREGS; c++)
				{
					Uint32 dataword = c << 8;
					write(lefthardsidfd, &dataword, 4);
					write(righthardsidfd, &dataword, 4);
				}
			}
			else return 0;
			timer = SDL_AddTimer(1000 / framerate, sound_timer, NULL);
		}
#endif
		goto SOUNDOK;
	}

	if (catweasel)
	{
#ifdef __WIN32__
		catweaselhandle = CreateFile("\\\\.\\SID6581_1", GENERIC_READ, FILE_SHARE_WRITE | FILE_SHARE_READ, 0L,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0L);
		if (catweaselhandle == INVALID_HANDLE_VALUE)
			return 0;
#else
		catweaselfd = open("/dev/sid", O_WRONLY);
		if (catweaselfd < 0)
			catweaselfd = open("/dev/misc/sid", O_WRONLY);
		if (catweaselfd < 0)
			return 0;
#ifndef __amigaos__
		if (ntsc)
			ioctl(catweaselfd, CWSID_IOCTL_NTSC);
		else
			ioctl(catweaselfd, CWSID_IOCTL_PAL);
#endif
#endif

		usecatweasel = 1;
		timer = SDL_AddTimer(1000 / framerate, sound_timer, NULL);
		goto SOUNDOK;
	}

  if (usbsid > 0) // NOTE: CHANGED
  {
		useusbsid = 1;
		if (usbsid == 2) cycleexactusbsid = TRUE;
		if (reinit == 0) {
      if (usbsiddev == NULL) {
				usbsiddev = create_USBSID();
			}
			if (!portisopen_USBSID(usbsiddev)) {
				if (cycleexactusbsid) {
					if (init_USBSID(usbsiddev, true, true) < 0) {
						return -1;
					}
					runusbsidthread = TRUE;
					usbsidthread = SDL_CreateThread(usbsid_sound_thread, NULL, NULL);
					if (!usbsidthread) return 0;
				} else {
					if (init_USBSID(usbsiddev, true, true) < 0) {
						return -1;
					}
					timer = SDL_AddTimer(1000 / framerate, sound_timer, NULL);
				}
			} else
			if (portisopen_USBSID(usbsiddev)) {
				if (cycleexactusbsid) {
				 	runusbsidthread = TRUE;
					if (runusbsidthread) {
						if (usbsidthread == NULL) {
							usbsidthread = SDL_CreateThread(usbsid_sound_thread, NULL, NULL);
						}
						if (!usbsidthread) return 0;
					}
				}
			}
		}
		if (usbsiddev != NULL && portisopen_USBSID(usbsiddev)) {
			if (ntsc)
				setclockrate_USBSID(usbsiddev, 1022727, true); /* TESTING */
			else
				setclockrate_USBSID(usbsiddev, 985248, true); /* TESTING */
		}
    goto SOUNDOK;
  }

	if (!tempbuffer) tempbuffer = malloc(MIXBUFFERSIZE * 2 * sizeof(Sint16));

	if (!sid0buffer) sid0buffer = malloc(MIXBUFFERSIZE * 2 * sizeof(Sint16));
	if (!sid1buffer) sid1buffer = malloc(MIXBUFFERSIZE * 2 * sizeof(Sint16));

	if (!sid2buffer) sid2buffer = malloc(MIXBUFFERSIZE * 2 * sizeof(Sint16));
	if (!sid3buffer) sid3buffer = malloc(MIXBUFFERSIZE * 2 * sizeof(Sint16));

	if ((!sid0buffer) || (!sid1buffer)) return 0;

	if (writer)
		writehandle = fopen("sidaudio.raw", "wb");

	playspeed = mr;
	if (playspeed < MINMIXRATE) playspeed = MINMIXRATE;
	if (playspeed > MAXMIXRATE) playspeed = MAXMIXRATE;
	if (b < MINBUF) b = MINBUF;
	if (b > MAXBUF) b = MAXBUF;


	if (firsttimeinit)
	{
		if (!snd_init(mr, SIXTEENBIT | STEREO, b, 1, 0)) return 0;
		firsttimeinit = 0;
	}

	playspeed = snd_mixrate;

	sid_init(playspeed, m, ntsc, interpolate & 1, customclockrate, interpolate >> 1);

	snd_player = &sound_playrout;
	snd_setcustommixer(sound_mixer);

SOUNDOK:
	initted = 1;
	return 1;
}

void sound_uninit(unsigned reinit)  // NOTE: CHANGED
{
	// printf("DEBUG: reinit: %u\n",reinit);

	int c;

	if (!initted) return;
	if (reinit == 0) initted = 0;

	// Apparently a delay is needed to make sure the sound timer thread is
	// not mixing stuff anymore, and we can safely delete related structures
	SDL_Delay(50);

	if (usehardsid || usecatweasel)
	{
#ifdef __WIN32__
		if (!playerthread)
		{
			SDL_RemoveTimer(timer);
		}
		else
		{
			runplayerthread = FALSE;
			SDL_WaitThread(playerthread, NULL);
			playerthread = NULL;
		}
#else
		SDL_RemoveTimer(timer);
#endif
	}
	else if (useusbsid && (reinit == 0)) // NOTE: CHANGE)
	{
		if (cycleexactusbsid)
		{
			if (!usbsidthread)
			{
				// printf("cycleexactusbsid: %d, reinit: %d, playerthread: %d, SDL_RemoveTimer(timer)\n", cycleexactusbsid, reinit, usbsidthread);
				SDL_RemoveTimer(timer);
			}
			else
			{
				runusbsidthread = FALSE;
				SDL_WaitThread(usbsidthread, NULL);
				usbsidthread = NULL;
			}
		}
		else
		{
			// printf("cycleexactusbsid: %d, reinit: %d, initted: %d, SDL_RemoveTimer(timer)\n", cycleexactusbsid, reinit, initted);
			SDL_RemoveTimer(timer);
		}
	}
	else
	{
		snd_setcustommixer(NULL);
		snd_player = NULL;
	}

	if (writehandle)
	{
		fclose(writehandle);
		writehandle = NULL;
	}

	if (sid0buffer)
	{
		free(sid0buffer);
		sid0buffer = NULL;
	}
	if (sid1buffer)
	{
		free(sid1buffer);
		sid1buffer = NULL;
	}

	if (sid2buffer)
	{
		free(sid2buffer);
		sid2buffer = NULL;
	}

	if (sid3buffer)
	{
		free(sid3buffer);
		sid3buffer = NULL;
	}

	if (tempbuffer)
	{
		free(tempbuffer);
		tempbuffer = NULL;
	}
	if (usehardsid)
	{
#ifdef __WIN32__
		for (c = 0; c < NUMSIDREGS; c++)
		{
			WriteToHardSID(lefthardsid, c, 0x00);
			WriteToHardSID(righthardsid, c, 0x00);
		}
		MuteHardSID_Line(TRUE);
#else
		if ((lefthardsidfd >= 0) && (righthardsidfd >= 0))
		{
			for (c = 0; c < NUMSIDREGS; c++)
			{
				Uint32 dataword = c << 8;
				write(lefthardsidfd, &dataword, 4);
				write(righthardsidfd, &dataword, 4);
			}
			close(lefthardsidfd);
			close(righthardsidfd);
			lefthardsidfd = -1;
			righthardsidfd = -1;
		}
#endif
	}

	if (usecatweasel)
	{
#ifdef __WIN32__
		DWORD w;
		unsigned char buf[NUMSIDREGS * 2];
		for (w = 0; w < NUMSIDREGS; w++)
		{
			buf[w * 2] = 0x18 - w;
			buf[w * 2 + 1] = 0;
		}
		DeviceIoControl(catweaselhandle, SID_SID_PEEK_POKE, buf, sizeof(buf), 0L, 0UL, &w, 0L);
		CloseHandle(catweaselhandle);
		catweaselhandle = INVALID_HANDLE_VALUE;
#else
		if (catweaselfd >= 0)
		{
			unsigned char buf[NUMSIDREGS];
			memset(buf, 0, sizeof(buf));
			lseek(catweaselfd, 0, SEEK_SET);
			write(catweaselfd, buf, sizeof(buf));
			close(catweaselfd);
			catweaselfd = -1;
		}
#endif
	}

  if (useusbsid) // NOTE: CHANGED
  {
    if ((reinit == 0) && portisopen_USBSID(usbsiddev))
		{
			close_USBSID(usbsiddev);
		}/*  else {
			printf("reinit: %d initted: %d portisopen_USBSID: %d, skipping close\n", reinit, initted, portisopen_USBSID(usbsiddev));
		} */

  }

}

void sound_suspend(void)
{
#ifdef __WIN32__

	SDL_LockMutex(flushmutex);
	suspendplayroutine = TRUE;
	SDL_UnlockMutex(flushmutex);
#endif
	if (useusbsid)
	{
		SDL_LockMutex(flushusbsidmutex);
		suspendusbsidroutine = TRUE;
		SDL_UnlockMutex(flushusbsidmutex);
	}
}

void sound_flush(void)
{
#ifdef __WIN32__
	SDL_LockMutex(flushmutex);
	flushplayerthread = TRUE;
	SDL_UnlockMutex(flushmutex);
#endif
	if (useusbsid)
	{
		SDL_LockMutex(flushusbsidmutex);
		flushusbsidthread = TRUE;
		SDL_UnlockMutex(flushusbsidmutex);
	}
}

Uint32 sound_timer(Uint32 interval, void *param)
{
	if (initted == 0) return interval;
	sound_playrout();
	return interval;
}

#ifdef __WIN32__
int sound_thread(void *userdata)
{
	unsigned long flush_cycles_interactive = hardsidbufinteractive * 1000; /* 0 = flush off for interactive mode*/
	unsigned long flush_cycles_playback = hardsidbufplayback * 1000; /* 0 = flush off for playback mode*/
	unsigned long cycles_after_flush = 0;
	boolean interactive;

	while (runplayerthread)
	{
		unsigned cycles = 1000000 / framerate; // HardSID should be clocked at 1MHz
		int c;

		if (flush_cycles_interactive > 0 || flush_cycles_playback > 0)
		{
			cycles_after_flush += cycles;
		}

		// Do flush if starting playback, stopping playback, starting an interactive note etc.
		if (flushplayerthread)
		{
			SDL_LockMutex(flushmutex);
			if (HardSID_Flush)
			{
				HardSID_Flush(lefthardsid);
			}
			// Can clear player suspend now (if set)
			suspendplayroutine = FALSE;
			flushplayerthread = FALSE;
			SDL_UnlockMutex(flushmutex);

			SDL_Delay(0);
		}

		if (!suspendplayroutine) playroutine(&gtObject);

		interactive = !(boolean)recordmode /* jam mode */ || !(boolean)isplaying(&gtObject);

		// Left side
		for (c = 0; c < NUMSIDREGS; c++)
		{
			unsigned o = sid_getorder(c, editorInfo.adparam);

			// Extra delay before loading the waveform (and mt_chngate,x)
			if ((o == 4) || (o == 11) || (o == 18))
			{
				HardSID_Write(lefthardsid, SIDWRITEDELAY + SIDWAVEDELAY, o, sidreg[o]);
				cycles -= SIDWRITEDELAY + SIDWAVEDELAY;
			}
			else
			{
				HardSID_Write(lefthardsid, SIDWRITEDELAY, o, sidreg[o]);
				cycles -= SIDWRITEDELAY;
			}
		}

		// Right side
		for (c = 0; c < NUMSIDREGS; c++)
		{
			unsigned o = sid_getorder(c, editorInfo.adparam);

			// Extra delay before loading the waveform (and mt_chngate,x)
			if ((o == 4) || (o == 11) || (o == 18))
			{
				HardSID_Write(righthardsid, SIDWRITEDELAY + SIDWAVEDELAY, o, sidreg2[o]);
				cycles -= SIDWRITEDELAY + SIDWAVEDELAY;
			}
			else
			{
				HardSID_Write(righthardsid, SIDWRITEDELAY, o, sidreg2[o]);
				cycles -= SIDWRITEDELAY;
			}
		}

		// Now wait the rest of frame
		while (cycles)
		{
			unsigned runnow = cycles;
			if (runnow > 65535) runnow = 65535;
			HardSID_Delay(lefthardsid, runnow);
			cycles -= runnow;
		}

		if ((flush_cycles_interactive > 0 && interactive && cycles_after_flush >= flush_cycles_interactive) ||
			(flush_cycles_playback > 0 && !interactive && cycles_after_flush >= flush_cycles_playback))
		{
			if (HardSID_SoftFlush)
				HardSID_SoftFlush(lefthardsid);
			cycles_after_flush = 0;
		}
	}

	unsigned r;

	for (r = 0; r < NUMSIDREGS; r++)
	{
		HardSID_Write(lefthardsid, SIDWRITEDELAY, r, 0);
		HardSID_Write(righthardsid, SIDWRITEDELAY, r, 0);
	}
	if (HardSID_SoftFlush)
		HardSID_SoftFlush(lefthardsid);

	return 0;
}
#endif

int usbsid_sound_thread(void *userdata)
{
	unsigned long flush_cycles_interactive = hardsidbufinteractive * 1000; /* 0 = flush off for interactive mode*/
	unsigned long flush_cycles_playback = hardsidbufplayback * 1000; /* 0 = flush off for playback mode*/
	unsigned long cycles_after_flush = 0;
	bool interactive;

	while (runusbsidthread)
	{
		unsigned cycles = getclockrate_USBSID(usbsiddev) / framerate; //1000000 / framerate; // HardSID should be clocked at 1MHz
		int c;

		if (flush_cycles_interactive > 0 || flush_cycles_playback > 0)
		{
			cycles_after_flush += cycles;
		}

		// Do flush if starting playback, stopping playback, starting an interactive note etc.
		if (flushusbsidthread)
		{
			SDL_LockMutex(flushusbsidmutex);
			/* flush_USBSID(usbsiddev); */
			setflush_USBSID(usbsiddev);

			// Can clear player suspend now (if set)
			suspendusbsidroutine = FALSE;
			flushusbsidthread = FALSE;
			SDL_UnlockMutex(flushusbsidmutex);

			SDL_Delay(0);
		}

		if (!suspendusbsidroutine) playroutine(&gtObject);

		interactive = !(bool)recordmode /* jam mode */ || !(bool)isplaying(&gtObject);

		// Left side
		for (c = 0; c < NUMSIDREGS; c++)
		{
			unsigned o = sid_getorder(c, editorInfo.adparam);

			// Extra delay before loading the waveform (and mt_chngate,x)
			if ((o == 4) || (o == 11) || (o == 18))
			{
				// HardSID_Write(lefthardsid, SIDWRITEDELAY + SIDWAVEDELAY, o, sidreg[o]);
				writeringcycled_USBSID(usbsiddev, o, sidreg[o], (SIDWRITEDELAY + SIDWAVEDELAY));
				waitforcycle_USBSID(usbsiddev, (SIDWRITEDELAY + SIDWAVEDELAY));
				cycles -= SIDWRITEDELAY + SIDWAVEDELAY;
			}
			else
			{
				// HardSID_Write(lefthardsid, SIDWRITEDELAY, o, sidreg[o]);
				writeringcycled_USBSID(usbsiddev, o, sidreg[o], SIDWRITEDELAY);
				waitforcycle_USBSID(usbsiddev, SIDWRITEDELAY);
				cycles -= SIDWRITEDELAY;
			}
		}

		// Right side
		for (c = 0; c < NUMSIDREGS; c++)
		{
			unsigned o = sid_getorder(c, editorInfo.adparam);

			// Extra delay before loading the waveform (and mt_chngate,x)
			if ((o == 4) || (o == 11) || (o == 18))
			{
				// HardSID_Write(righthardsid, SIDWRITEDELAY + SIDWAVEDELAY, o, sidreg2[o]);
				writeringcycled_USBSID(usbsiddev, (0x20 | o), sidreg2[o], (SIDWRITEDELAY + SIDWAVEDELAY));
				waitforcycle_USBSID(usbsiddev, (SIDWRITEDELAY + SIDWAVEDELAY));
				cycles -= SIDWRITEDELAY + SIDWAVEDELAY;
			}
			else
			{
				// HardSID_Write(righthardsid, SIDWRITEDELAY, o, sidreg2[o]);
				writeringcycled_USBSID(usbsiddev, (0x20 | o), sidreg2[o], SIDWRITEDELAY);
				waitforcycle_USBSID(usbsiddev, SIDWRITEDELAY);
				cycles -= SIDWRITEDELAY;
			}
		}

		// Now wait the rest of frame
		while (cycles)
		{
			unsigned runnow = cycles;
			if (runnow > 65535) runnow = 65535;
			/* HardSID_Delay(lefthardsid, runnow); */
				writeringcycled_USBSID(usbsiddev, 0xFF, 0x0, runnow);
				waitforcycle_USBSID(usbsiddev, runnow);
			cycles -= runnow;
		}

		if ((flush_cycles_interactive > 0 && interactive && cycles_after_flush >= flush_cycles_interactive) ||
			(flush_cycles_playback > 0 && !interactive && cycles_after_flush >= flush_cycles_playback))
		{
			setflush_USBSID(usbsiddev);
			cycles_after_flush = 0;
		}
	}

	unsigned r;

	for (r = 0; r < NUMSIDREGS; r++)
	{
		// HardSID_Write(lefthardsid, SIDWRITEDELAY, r, 0);
		// HardSID_Write(righthardsid, SIDWRITEDELAY, r, 0);
		writeringcycled_USBSID(usbsiddev, r, 0x0, SIDWRITEDELAY);
		writeringcycled_USBSID(usbsiddev, (0x20 | r), 0x0, SIDWRITEDELAY);
	}
	// if (HardSID_SoftFlush)
	// 	HardSID_SoftFlush(lefthardsid);
	setflush_USBSID(usbsiddev);

	return 0;
}

int bypassPlayRoutine = 0;

void sound_playrout(void)
{
	int c;

	if (!bypassPlayRoutine)
		playroutine(&gtObject);
	if (usehardsid)
	{
#ifdef __WIN32__
		for (c = 0; c < NUMSIDREGS; c++)
		{
			unsigned o = sid_getorder(c, editorInfo.adparam);
			WriteToHardSID(lefthardsid, o, sidreg[o]);
			WriteToHardSID(righthardsid, o, sidreg2[o]);
		}
#else
		for (c = 0; c < NUMSIDREGS; c++)
		{
			unsigned o = sid_getorder(c, editorInfo.adparam);
			Uint32 dataword = (o << 8) | sidreg[o];
			write(lefthardsidfd, &dataword, 4);
			dataword = (o << 8) | sidreg2[o];
			write(righthardsidfd, &dataword, 4);
		}
#endif
	}
	else if (usecatweasel)
	{
#ifdef __WIN32__
		DWORD w;
		unsigned char buf[NUMSIDREGS * 2];

		for (w = 0; w < NUMSIDREGS; w++)
		{
			unsigned o = sid_getorder(w, editorInfo.adparam);

			buf[w * 2] = o;
			buf[w * 2 + 1] = sidreg[o];
		}
		DeviceIoControl(catweaselhandle, SID_SID_PEEK_POKE, buf, sizeof(buf), 0L, 0UL, &w, 0L);
#else
		for (c = 0; c < NUMSIDREGS; c++)
		{
			unsigned o = sid_getorder(c, editorInfo.adparam);

			lseek(catweaselfd, o, SEEK_SET);
			write(catweaselfd, &sidreg[o], 1);
		}
#endif
	}
  else if (useusbsid) // NOTE: CHANGED
  {
    for (c = 0; c < NUMSIDREGS; c++)
    {
      unsigned o = sid_getorder(c, editorInfo.adparam);
			writeringcycled_USBSID(usbsiddev, o, sidreg[o], SIDWRITEDELAY);
			writeringcycled_USBSID(usbsiddev, (0x20 | o), sidreg2[o], SIDWRITEDELAY);
    }
    	/* flush_USBSID(usbsiddev); */ // ISSUE: Causes a lockup!
			setflush_USBSID(usbsiddev);  /* Will set flush to 1 and will be picked up automatically */
  }
}


int bypassMixer = 0;

//882 samples for 1x
//441 for 2x
//220 for 4x
void sound_mixer(Sint32 *dest, unsigned samples)
{
	//	printf("samples: %d\n", samples);
	if (!bypassPlayRoutine)
		JPSoundMixer(dest, samples);
}

// for TrueStereo, sidnBuffer is *2 size:
// Left = sidnBuffer 0 > MIXBUFFERSIZE-1
// Right = sidnBuffer MIXBUFFERSIZE > MIXBUFFERSIZE*2
void JPSoundMixer(Sint32 *dest, unsigned samples)
{

	int tick = SDL_GetTicks();

	int c;

	if (!initted) return;
	if (samples > MIXBUFFERSIZE) return;

	if (dest == NULL)
		sid_fillbuffer(tempbuffer, tempbuffer, tempbuffer, tempbuffer, samples, MIXBUFFERSIZE, editorInfo.adparam);
	else
		sid_fillbuffer(sid0buffer, sid1buffer, sid2buffer, sid3buffer, samples, MIXBUFFERSIZE, editorInfo.adparam);


	//	sprintf(textbuffer, "sid %d", sid_debug());
	//	printtext(70, 14, 0xe, textbuffer);


	//	int tick = SDL_GetTicks();

		//debugTicks = SDL_GetTicks() - tick;

	if (dest != NULL)
	{
		Sint32 *dp = dest;
		Sint32 v;

		Sint16 *spL0 = sid0buffer;
		Sint16 *spL1 = sid1buffer;
		Sint16 *spL2 = sid2buffer;
		Sint16 *spL3 = sid3buffer;

		Sint16 *spR0 = sid0buffer + MIXBUFFERSIZE;
		Sint16 *spR1 = sid1buffer + MIXBUFFERSIZE;
		Sint16 *spR2 = sid2buffer + MIXBUFFERSIZE;
		Sint16 *spR3 = sid3buffer + MIXBUFFERSIZE;


		float mvf = masterVolume * 0.25f;	// * 0x8000;
	//		int mv = (int)mvf;

		for (c = 0; c < samples; c++)
		{
			v = *spL0 + *spL1 + *spL2 + *spL3;

			float f = (float)v;


			spL0++;
			spL1++;
			spL2++;
			spL3++;

			f *= mvf;
			//			v /= 4;
			//			v *= mv;
			//			v /= 0x8000;

			*dp = (Sint32)f;
			//			*dp = v;
			dp++;

			v = *spR0 + *spR1 + *spR2 + *spR3;
			f = (float)v;

			spR0++;
			spR1++;
			spR2++;
			spR3++;

			f *= mvf;	// /4 (4 SIDs) / 0x8000 to scale

			//			v /= 4;
			//			v *= mv;
			//			v /= 0x8000;

			*dp = f;
			//			*dp = v;
			dp++;

		}


		if (writehandle)
		{
			for (c = 0; c < samples * 2; c++)
			{
				fwrite(&dest[c], sizeof(Sint16), 1, writehandle);
				//				fwrite(&dest[c], sizeof(Sint16), 1, writehandle);
				//				fwrite(&sid2buffer[c], sizeof(Sint16), 1, writehandle);
				//				fwrite(&sid3buffer[c], sizeof(Sint16), 1, writehandle);
			}
		}

	}

	debugTicks = SDL_GetTicks() - tick;
}

// 882 samples generated for 1x
// 441 samples generated for 2x
// 220 samples generated for 4x
/*
Bypass playroute
Call ExportOpenFileHandle()
Tight loop to do normal update until end (same as current code for handling length
call ExportSIDToPCMFile between each update, passing the correct samples size (882/speed)
call ExportCloseFileHandle()
Do PCM>WAV conversion?
*/

FILE *exportFileHandle = NULL;
FILE *exportWAVFileHandle = NULL;

char rawFileName[1000];
char wavFileName[1000];

void GenerateExportFileName()
{
	rawFileName[0] = 0;
	strcat(&rawFileName[0], wavfilename);
	int l = strlen(rawFileName);
	rawFileName[l - 4] = 0;
	strcat(&rawFileName[0], "_temp.raw");
}

void OpenExportFileNameForWriting()
{
	if (exportFileHandle != NULL)
		ExportCloseFileHandle();

	exportFileHandle = fopen(rawFileName, "wb");
}

void fwrite32(FILE *file, unsigned data)
{
	Uint8 bytes[4];

	bytes[0] = data >> 24;
	bytes[1] = data >> 16;
	bytes[2] = data >> 8;
	bytes[3] = data;
	fwrite(bytes, 4, 1, file);
}

Sint32 exportPCMBuffer[4096];

Sint16 exportPCMBuffer2[1];

void convertRAWToWAV(int doNormalize)
{
	GenerateExportFileName();	// Shouldn't be needed eventually, as it's already set when saving raw PCM

	exportFileHandle = fopen(rawFileName, "rb");
	fseek(exportFileHandle, 0, SEEK_END);
	int rawSize = ftell(exportFileHandle);
	fseek(exportFileHandle, 0, SEEK_SET);

	printf("file size:%x\n", rawSize);
	if (rawSize == 0)
	{
		fclose(exportFileHandle);
		return;
	}

	exportWAVFileHandle = fopen(wavfilename, "wb");

	//rawSize = 0x2b10c;	// Use this to test matching a 1 second stereo 44.1 khz wav header

	fwrite32(exportWAVFileHandle, 0x52494646);
	fwritele32(exportWAVFileHandle, rawSize + 0x28);
	fwrite32(exportWAVFileHandle, 0x57415645);
	fwrite32(exportWAVFileHandle, 0x666d7420);
	fwrite32(exportWAVFileHandle, 0x10000000);
	fwrite32(exportWAVFileHandle, 0x01000200);
	fwritele32(exportWAVFileHandle, mr);
	fwritele32(exportWAVFileHandle, rawSize + 4);
	fwrite32(exportWAVFileHandle, 0x04001000);
	fwrite32(exportWAVFileHandle, 0x64617461);
	fwritele32(exportWAVFileHandle, rawSize + 4);
	fwrite32(exportWAVFileHandle, 0);

	float normalize = 1;
	if (doNormalize)
	{
		if (largestExportValue > 0)
			normalize = 0x7fff / largestExportValue;
	}

	for (int c = 0;c < rawSize / 2;c++)
	{
		fread(&exportPCMBuffer2[0], sizeof(Sint16), 1, exportFileHandle);
		if (doNormalize)
		{
			float f = exportPCMBuffer2[0] * normalize;
			exportPCMBuffer2[0] = f;
		}
		fwrite(&exportPCMBuffer2[0], sizeof(Sint16), 1, exportWAVFileHandle);
	}
	fclose(exportWAVFileHandle);
	ExportCloseFileHandle();
	remove(&rawFileName[0]);	// delete raw file

}

void ExportCloseFileHandle()
{
	fclose(exportFileHandle);
	exportFileHandle = NULL;
}



int dataPacket = 0;
int largestExportValue;
// Raw Data - Signed - 16 bit - Stereo
void ExportSIDToPCMFile(int samples,int doNormalize)
{
	if (!initted) return;
	if (samples > MIXBUFFERSIZE) return;

	sid_fillbuffer(sid0buffer, sid1buffer, sid2buffer, sid3buffer, samples, MIXBUFFERSIZE, editorInfo.adparam);


	Sint32 *dp = &exportPCMBuffer[0];
	Sint32 v;

	Sint16 *spL0 = sid0buffer;
	Sint16 *spL1 = sid1buffer;
	Sint16 *spL2 = sid2buffer;
	Sint16 *spL3 = sid3buffer;

	Sint16 *spR0 = sid0buffer + MIXBUFFERSIZE;
	Sint16 *spR1 = sid1buffer + MIXBUFFERSIZE;
	Sint16 *spR2 = sid2buffer + MIXBUFFERSIZE;
	Sint16 *spR3 = sid3buffer + MIXBUFFERSIZE;


	float mvf = masterVolume * 0.25f;	// * 0x8000;

	for (int c = 0; c < samples; c++)
	{
		v = *spL0 + *spL1 + *spL2 + *spL3;
		spL0++;
		spL1++;
		spL2++;
		spL3++;

		float f = (float)v;
		f *= mvf;

		if (doNormalize)
		{
			if (f <0x8000 && f > largestExportValue)
				largestExportValue = f;
			else if (f >= 0x8000 && ((0x10000 - f) > largestExportValue))
				largestExportValue = (0x10000 - f);
		}

		*dp = (Sint32)f;
		dp++;

		v = *spR0 + *spR1 + *spR2 + *spR3;
		spR0++;
		spR1++;
		spR2++;
		spR3++;

		f = (float)v;
		f *= mvf;	// /4 (4 SIDs) / 0x8000 to scale

		if (doNormalize)
		{
			if (f <0x8000 && f > largestExportValue)
				largestExportValue = f;
			else if (f >= 0x8000 && ((0x10000 - f) > largestExportValue))
				largestExportValue = (0x10000 - f);
		}

		*dp = f;
		dp++;
	}



	if (exportFileHandle)
	{
		for (int c = 0; c < samples * 2; c++)
		{
			fwrite(&exportPCMBuffer[c], sizeof(Sint16), 1, exportFileHandle);
		}
	}

}


#ifdef __WIN32__
void InitHardDLL()
{
	if (!(hardsiddll = LoadLibrary("HARDSID.DLL"))) return;

	WriteToHardSID = (lpWriteToHardSID)GetProcAddress(hardsiddll, "WriteToHardSID");
	ReadFromHardSID = (lpReadFromHardSID)GetProcAddress(hardsiddll, "ReadFromHardSID");
	InitHardSID_Mapper = (lpInitHardSID_Mapper)GetProcAddress(hardsiddll, "InitHardSID_Mapper");
	MuteHardSID_Line = (lpMuteHardSID_Line)GetProcAddress(hardsiddll, "MuteHardSID_Line");

	if (!WriteToHardSID) return;

	// Try to get cycle-exact interface
	HardSID_Delay = (lpHardSID_Delay)GetProcAddress(hardsiddll, "HardSID_Delay");
	HardSID_Write = (lpHardSID_Write)GetProcAddress(hardsiddll, "HardSID_Write");
	HardSID_Flush = (lpHardSID_Flush)GetProcAddress(hardsiddll, "HardSID_Flush");
	HardSID_SoftFlush = (lpHardSID_SoftFlush)GetProcAddress(hardsiddll, "HardSID_SoftFlush");
	if ((HardSID_Delay) && (HardSID_Write) && (HardSID_Flush) && (HardSID_SoftFlush))
		cycleexacthardsid = TRUE;

	InitHardSID_Mapper();
	dll_initialized = TRUE;
}
#endif
