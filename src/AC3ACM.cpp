/*
 * AC3ACM.cpp
 * Copyright (C) 2003-2012 fccHandler <fcchandler@comcast.net>
 * Copyright (C) 2000-2003 Michel Lespinasse <walken@zoy.org>
 * Copyright (C) 1999-2000 Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 * Copyright (C) 2000-2002 Fabrice Bellard
 *
 * AC3ACM is an open source ATSC A-52 codec for Windows Audio
 * Compression Manager. The AC-3 decoder was taken from liba52.
 * The AC-3 encoder was adapted from ffmpeg. The ACM interface
 * was written by me (fccHandler).
 *
 * See http://fcchandler.home.comcast.net/ for AC3ACM updates.
 * See http://liba52.sourceforge.net/ for liba52 updates.
 * See http://ffmpeg.sourceforge.net/ for ac3enc updates.
 *
 * AC3ACM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * AC3ACM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

#if defined(_MSC_VER) && (_MSC_VER >= 1400)
	#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <msacm.h>
#include <ks.h>
#include <ksmedia.h>
#include "WinDDK.h"
#include "resource.h"

#ifdef _DEBUG
	#define AC3ACM_LOGFILE
#endif

#include	<stdio.h>

// liba52 headers
extern "C" {
	#include "config.h"
	#include "inttypes.h"
	#include "a52.h"
	#include "a52_internal.h"
	#include "mm_accel.h"
}

#include "ac3enc.h"

typedef struct {
	FOURCC	fccType;		// fccType from ACMDRVOPENDESC
	HDRVR	hdrvr;			// hdrvr (as sent by the caller)
	HMODULE	hmod;			// driver module handle
	DWORD	dwVersion;		// dwVersion from ACMDRVOPENDESC
	DWORD	dwFlags;		// codec flags (see below)
	bool    UseMMX;			// use MMX converters
#ifdef AC3ACM_LOGFILE
	ACMDRVOPENDESCW	*pdod;
	FILE			*fLogFile;	// "C:\AC3ACM.log" (for debugging)
#endif
} MyData;

// Flags for MyData.dwFlags
#define	AC3ACM_MULTICHANNEL		1   // enable > 2 channel output
#define	AC3ACM_DYNAMICRANGE		2   // enable dynamic range control
#define AC3ACM_DOLBYSURROUND	4   // Dolby Surround compatible downmix
#define AC3ACM_DONTUSEMMX		8   // Disable MMX code (for testing)
#define AC3ACM_USE64			16  // Use (64 * channels) for encoding
#define AC3ACM_NOEXTENSIBLE     32  // Don't expose WAVE_FORMAT_EXTENSIBLE

// Registry key
static const char MyRegKey[] = "Software\\fccHandler\\AC3ACM";

// Generic conversion proc
typedef void (*ConvertProc)(const void *src, void *dst, int flags);

extern "C" ConvertProc MapTab[2][6][6];
extern "C" bool IsMMX();

typedef struct {
	int				framelen;
	int				blocks;
	int				flags;
	ConvertProc		convert;
	a52_state_t		*a52state;
	unsigned char	*buf;
	unsigned char	*bufptr;
	unsigned char	*bufend;
	unsigned char	*frame;
} MyStreamData;

// Technically, there is no such thing:
#define WAVE_FORMAT_AC3 0x2000

// Extensible AC-3 GUID which ffmpeg uses:
static const GUID GUID_AC3ACM_EXTENSIBLE = {
	0x00002000, 0x0000, 0x0010, 0x80, 0x00,
	0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
};

// We support 3*6*19 = 342 AC3 input formats
// (valid samplerates) * (valid #channels) * (valid bitrates)

// We support 3*6 = 18 PCM, PCMEX, and IEEE output formats
// (valid samplerates) * (valid #channels)

// We can downmix 3+ AC3 channels to stereo or mono PCM
// We can upmix a mono AC3 to a stereo PCM

// bsid  8:  0 = 32000, 1 = 44100, 2 = 48000
// bsid  9:  0 = 16000, 1 = 22050, 2 = 24000
// bsid 10:  0 =  8000, 1 = 11025, 2 = 12000

static const long srates[3] = {32000, 44100, 48000};

static const short framesizes[19][4] = {
//	32K, 44.1K, 48K, bitrate
	{  96,   69,   64,  32},
	{ 120,   87,   80,  40},
	{ 144,  104,   96,  48},
	{ 168,  121,  112,  56},
	{ 192,  139,  128,  64},
	{ 240,  174,  160,  80},
	{ 288,  208,  192,  96},
	{ 336,  243,  224, 112},
	{ 384,  278,  256, 128},
	{ 480,  348,  320, 160},
	{ 576,  417,  384, 192},
	{ 672,  487,  448, 224},
	{ 768,  557,  512, 256},
	{ 960,  696,  640, 320},
	{1152,  835,  768, 384},
	{1344,  975,  896, 448},
	{1536, 1114, 1024, 512},
	{1728, 1253, 1152, 576},
	{1920, 1393, 1280, 640}
};

// There are 19 valid bitrates, 3 valid sampling rates,
// and 6 valid channel configurations, total of 342 formats!


// These are the channel configurations we support:
static const DWORD channel_masks[6] = {
	SPEAKER_FRONT_CENTER,
	SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT,
	SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER,
	SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT,
	SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT,
	SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT
};


BOOL APIENTRY DllMain(HANDLE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	return TRUE;
}


static int LoadResString(HINSTANCE hInstance, UINT uID, LPWSTR lpBuffer, int cchBuffer)
{
	int ret = 0;

	if (lpBuffer != NULL && cchBuffer > 0)
	{
		// Assume the OS has no Unicode support.
		LPSTR pRes = (LPSTR)GlobalAlloc(GMEM_FIXED, cchBuffer + 1);
		if (pRes != NULL)
		{
			int i = LoadStringA(hInstance, uID, pRes, cchBuffer);
			if (i > 0)
			{
				if (i >= cchBuffer) i = cchBuffer - 1;
				pRes[i] = '\0';

				i = MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED,
					pRes, i + 1, lpBuffer, cchBuffer);

				if (i > 0)
				{
					if (i >= cchBuffer) i = cchBuffer - 1;
					ret = i;
				}
			}
			GlobalFree((HGLOBAL)pRes);
		}

		lpBuffer[ret] = L'\0';
	}

	return ret;
}


static bool IsValidPCMEX(const WAVEFORMATEX *wfex)
{
	if (wfex != NULL)
	{
		const WAVEFORMATPCMEX *wfx = (WAVEFORMATPCMEX *)wfex;

		const WORD  ch  = wfx->Format.nChannels;
		const DWORD ba  = wfx->Format.nBlockAlign;
		const WORD  bps = wfx->Format.wBitsPerSample;
		const DWORD sps = wfx->Format.nSamplesPerSec;

		if (wfx->Format.wFormatTag != WAVE_FORMAT_EXTENSIBLE) goto Abort;
		if (ch < 1 || ch > 6) goto Abort;
		if (bps != 16) goto Abort;
		if (ba != (ch * 2U)) goto Abort;
		if (wfx->Format.nAvgBytesPerSec != ba * sps) goto Abort;

		if (sps != 48000 && sps != 44100 && sps != 32000 &&
		    sps != 24000 && sps != 22050 && sps != 16000 &&
		    sps != 12000 && sps != 11025 && sps != 8000) goto Abort;

		if (wfx->Format.cbSize < (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) goto Abort;
		if (wfx->Samples.wValidBitsPerSample != bps) goto Abort;
		if (wfx->dwChannelMask != channel_masks[ch - 1]) goto Abort;
		if (wfx->SubFormat != KSDATAFORMAT_SUBTYPE_PCM) goto Abort;

		return true;
	}

Abort:
	return false;
}


static bool IsValidPCM(const WAVEFORMATEX *wfex, const MyData *md)
{
	// Does "wfex" describe a PCM format we can decompress to?

	if (wfex != NULL)
	{
		if (wfex->wFormatTag == WAVE_FORMAT_PCM)
		{
			const WORD  ch  = wfex->nChannels;
			const DWORD ba  = wfex->nBlockAlign;
			const WORD  bps = wfex->wBitsPerSample;
			const DWORD sps = wfex->nSamplesPerSec;

			if (ch < 1 || ch > 6) goto Abort;
			if (bps != 16) goto Abort;
			if (ba != (ch * 2U)) goto Abort;
			if (wfex->nAvgBytesPerSec != ba * sps) goto Abort;

			if (sps != 48000 && sps != 44100 && sps != 32000 &&
			    sps != 24000 && sps != 22050 && sps != 16000 &&
			    sps != 12000 && sps != 11025 && sps != 8000) goto Abort;
		
			return true;
		}

		if (md != NULL)
		{
			if (!(md->dwFlags & AC3ACM_NOEXTENSIBLE))
			{
				return IsValidPCMEX(wfex);
			}
		}
	}

Abort:
	return false;
}


static bool IsValidAC3EX(const WAVEFORMATEX *wfex)
{
	// Does "wfex" describe an AC-3 format we can decode?

	if (wfex != NULL)
	{
		const WAVEFORMATEXTENSIBLE *wfx = (WAVEFORMATEXTENSIBLE *)wfex;

		const WORD  ch  = wfx->Format.nChannels;
		const DWORD sps = wfx->Format.nSamplesPerSec;
		const DWORD bps = wfx->Format.nAvgBytesPerSec;

		if (wfx->Format.wFormatTag != WAVE_FORMAT_EXTENSIBLE) goto Abort;
		if (ch < 1 || ch > 6) goto Abort;
		if (bps < 3000 || bps > 81000UL) goto Abort;
		if (wfx->Format.nBlockAlign == 0) goto Abort;

		if (sps != 48000 && sps != 44100 && sps != 32000 &&
		    sps != 24000 && sps != 22050 && sps != 16000 &&
		    sps != 12000 && sps != 11025 && sps != 8000) goto Abort;

		if (wfx->Format.cbSize < (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) goto Abort;
		if (wfx->Samples.wSamplesPerBlock != 1536) goto Abort;
		if (wfx->dwChannelMask != channel_masks[ch - 1]) goto Abort;
		if (wfx->SubFormat != KSDATAFORMAT_SUBTYPE_AC3_AUDIO) goto Abort;
		if (wfx->SubFormat != GUID_AC3ACM_EXTENSIBLE) goto Abort;
		
		return true;
	}

Abort:
	return false;
}


static bool IsValidAC3(const WAVEFORMATEX *wfex, const MyData *md)
{
	// Does "wfex" describe an AC-3 format we can decode?

	if (wfex != NULL)
	{
		if (wfex->wFormatTag == WAVE_FORMAT_AC3)
		{
			const WORD  ch  = wfex->nChannels;
			const DWORD sps = wfex->nSamplesPerSec;
			const DWORD bps = wfex->nAvgBytesPerSec;

			if (ch < 1 || ch > 6) goto Abort;

			if (sps != 48000 && sps != 44100 && sps != 32000 &&
			    sps != 24000 && sps != 22050 && sps != 16000 &&
			    sps != 12000 && sps != 11025 && sps != 8000) goto Abort;

			// Minimum AC-3 byterate is 4000 bytes per sec
			// Maximum AC-3 byterate is 80000 bytes per sec
			// (We'll allow some slop of 1000 bps)
			if (bps < 3000 || bps > 81000UL) goto Abort;
			if (wfex->nBlockAlign == 0) goto Abort;
		
			return true;
		}

		if (md != NULL)
		{
			if (!(md->dwFlags & AC3ACM_NOEXTENSIBLE))
			{
				return IsValidAC3EX(wfex);
			}
		}
	}

Abort:
	return false;
}


/* TODO: implement this if possible
static bool IsValidIEEEEX(const WAVEFORMATEX *wfex)
{
	if (wfex != NULL)
	{
		const WAVEFORMATIEEEFLOATEX *wfx = (WAVEFORMATIEEEFLOATEX *)wfex;

		const WORD  ch  = wfx->Format.nChannels;
		const DWORD ba  = wfx->Format.nBlockAlign;
		const WORD  bps = wfx->Format.wBitsPerSample;
		const DWORD sps = wfx->Format.nSamplesPerSec;

		if (wfx->Format.wFormatTag != WAVE_FORMAT_EXTENSIBLE) goto Abort;
		if (ch < 1 || ch > 6) goto Abort;
		if (bps != 32) goto Abort;
		if (ba != (ch * 4)) goto Abort;
		if (wfx->Format.nAvgBytesPerSec != ba * sps) goto Abort;

		if (sps != 48000 && sps != 44100 && sps != 32000 &&
		    sps != 24000 && sps != 22050 && sps != 16000 &&
		    sps != 12000 && sps != 11025 && sps != 8000) goto Abort;

		if (wfx->Format.cbSize < (sizeof(WAVEFORMATIEEEFLOATEX) - sizeof(WAVEFORMATEX))) goto Abort;
		if (wfx->Samples.wValidBitsPerSample != bps) goto Abort;
		if (wfx->dwChannelMask != channel_masks[ch - 1]) goto Abort;
		if (wfx->SubFormat != KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) goto Abort;

		return true;
	}

Abort:
	return false;
}


static bool IsValidIEEE(const WAVEFORMATEX *wfex, const MyData *md)
{
	if (wfex != NULL)
	{
		if (wfex->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
		{
			const WORD  ch  = wfex->nChannels;
			const DWORD ba  = wfex->nBlockAlign;
			const WORD  bps = wfex->wBitsPerSample;
			const DWORD sps = wfex->nSamplesPerSec;

			if (ch < 1 || ch > 6) goto Abort;
			if (bps != 32) goto Abort;
			if (ba != (ch * 4)) goto Abort;
			if (wfex->nAvgBytesPerSec != ba * sps) goto Abort;

			if (sps != 48000 && sps != 44100 && sps != 32000 &&
			    sps != 24000 && sps != 22050 && sps != 16000 &&
			    sps != 12000 && sps != 11025 && sps != 8000) goto Abort;

			return true;
		}

		if (md != NULL)
		{
			if (!(md->dwFlags & AC3ACM_NOEXTENSIBLE))
			{
				return IsValidIEEEEX(wfex);
			}
		}
	}

Abort:
	return false;
}
*/


// We need to be able to determine the AC-3 frame size
// based on WAVEFORMATEX, even if we have to guess...

static int ac3_framesize(const WAVEFORMATEX *wfex)
{
	if (wfex != NULL)
	{
		const WORD ba = wfex->nBlockAlign;

		int spsindex = (wfex->nSamplesPerSec >> 6) & 3;
		int i;

		if (ba > 1)
		{
			// Maybe nBlockAlign is the frame size?
			for (i = 0; i < 19; i++)
			{
				if (ba == (framesizes[i][spsindex] * 2))
				{
					return (int)ba;
				}
			}
		}

		// Let's try nAvgBytesPerSec...
		// It need not be exact, but it has to be close
		if (wfex->nAvgBytesPerSec <= 81000UL)
		{
			int selec;
			int diff = 0x7FFFFFFF;

			for (i = 0; i < 19; i++)
			{
				int d = (int)(wfex->nAvgBytesPerSec - (125UL * framesizes[i][3]));

				if (d == 0)
				{
					// Exact match
					return (framesizes[i][spsindex] * 2);
				}

				if (d < 0) d = -d;

				if (d < diff)
				{
					selec = i;
					diff = d;
				}
			}

			return (framesizes[selec][spsindex] * 2);
		}

		// Drat; just assume the worst case
		return (framesizes[18][spsindex] * 2);
	}

	return 0;
}


#ifdef AC3ACM_LOGFILE
static void log_format(DWORD_PTR dwDriverId, const WAVEFORMATEX *wfex) {
	FILE *fh = ((MyData *)dwDriverId)->fLogFile;

	if (fh != NULL)
	{
		if (wfex != NULL)
		{
			fprintf(fh, "  wFormatTag      = 0x%04X\n", wfex->wFormatTag);
			fprintf(fh, "  nChannels       = %hu\n", wfex->nChannels);
			fprintf(fh, "  nSamplesPerSec  = %lu\n", wfex->nSamplesPerSec);
			fprintf(fh, "  nAvgBytesPerSec = %lu\n", wfex->nAvgBytesPerSec);
			fprintf(fh, "  nBlockAlign     = %lu\n", wfex->nBlockAlign);
			fprintf(fh, "  wBitsPerSample  = %hu\n", wfex->wBitsPerSample);

			if (wfex->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
			{
				const WAVEFORMATEXTENSIBLE *wfx = (WAVEFORMATEXTENSIBLE *)wfex;

				fprintf(fh, "  cbSize          = %hu\n", wfx->Format.cbSize);
				fprintf(fh, "  Samples         = %hu\n", wfx->Samples.wReserved);
				fprintf(fh, "  dwChannelMask   = %lu\n", wfx->dwChannelMask);

				fprintf(fh, "  SubFormat       = "
					"{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}\n",
					wfx->SubFormat.Data1,
					wfx->SubFormat.Data2,
					wfx->SubFormat.Data3,
					wfx->SubFormat.Data4[0],
					wfx->SubFormat.Data4[1],
					wfx->SubFormat.Data4[2],
					wfx->SubFormat.Data4[3],
					wfx->SubFormat.Data4[4],
					wfx->SubFormat.Data4[5],
					wfx->SubFormat.Data4[6],
					wfx->SubFormat.Data4[7]
				);
			}
		}
		else
		{
			fprintf(fh, "  log_format(): wfex is NULL!");
		}
	}
}
#endif


static void ReadReg(MyData *md)
{
	// Read configuration options from Registry

	if (md != NULL)
	{
		HKEY hKey;

		// Default
		md->dwFlags = AC3ACM_MULTICHANNEL;

		if (RegOpenKeyEx(HKEY_CURRENT_USER, MyRegKey,
			0, KEY_READ, &hKey) == ERROR_SUCCESS)
		{
			DWORD dwData;
			DWORD dwType;

			DWORD cbData = sizeof(dwData);

			if (RegQueryValueEx(hKey, "Flags", NULL, &dwType,
				(LPBYTE)&dwData, &cbData) == ERROR_SUCCESS)
			{
				if (dwType == REG_DWORD && cbData == sizeof(dwData))
				{
					md->dwFlags = dwData;
				}
			}

			RegCloseKey(hKey);
		}
	}
}


static void WriteReg(const MyData *md)
{
	// Store configuration options in Registry

	if (md != NULL)
	{
		HKEY hKey;

		if (RegCreateKeyEx(HKEY_CURRENT_USER, MyRegKey, 0,
			NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS,
			NULL, &hKey, NULL) == ERROR_SUCCESS)
		{
			DWORD dwData = md->dwFlags;

			RegSetValueEx(hKey, "Flags", 0, REG_DWORD,
				(CONST BYTE *)&dwData, sizeof(dwData));

			RegCloseKey(hKey);
		}
	}
}


#ifdef AC3ACM_LOGFILE
static inline char GetHomeDrive()
{
	char pszHome[4];
	if (ExpandEnvironmentStringsA("%HOMEDRIVE%", pszHome, sizeof(pszHome)) > 1)
	{
		return pszHome[0];
	}
	return 'C';
}
#endif


static LRESULT drv_open(HDRVR hdrvr, LPARAM lParam2)
{
	ACMDRVOPENDESCW *dod = (ACMDRVOPENDESCW *)lParam2;
	MyData *md;

	if (dod != NULL)
	{
		if (dod->fccType != ACMDRIVERDETAILS_FCCTYPE_AUDIOCODEC)
		{
			return NULL;
		}
	}

	md = (MyData *)LocalAlloc(LPTR, sizeof(MyData));

	if (md != NULL)
	{
		md->hdrvr	= hdrvr;
		md->hmod	= GetDriverModuleHandle(hdrvr);
		md->fccType	= ACMDRIVERDETAILS_FCCTYPE_AUDIOCODEC;

		// Fetch options from Registry
		ReadReg(md);

		if (md->dwFlags & AC3ACM_DONTUSEMMX) {
			md->UseMMX = 0;
		} else {
			md->UseMMX = IsMMX();
		}

#ifdef AC3ACM_LOGFILE
		if (md->pdod = dod) {
#else
		if (dod != NULL) {
#endif
			//md->fccType = dod->fccType;
			md->dwVersion = dod->dwVersion;
            dod->dwError  = MMSYSERR_NOERROR;
		}

#ifdef AC3ACM_LOGFILE
		{
			char logname[64];
			sprintf(logname, "%c:\\AC3ACM%p.log", GetHomeDrive(), md);

			if (logname[0] != '\0')
			{
				md->fLogFile = fopen(logname, "a");
				if (md->fLogFile != NULL)
				{
					fprintf(md->fLogFile, "DRV_OPEN: successful\n");
					fprintf(md->fLogFile, " ACMDRVOPENDESCW = 0x%p\n", dod);
					fflush(md->fLogFile);
				}
			}
		}
#endif
	}
	else if (dod != NULL)
	{
		dod->dwError = MMSYSERR_NOMEM;
	}

	return (LRESULT)md;
}


static LRESULT drv_close(DWORD_PTR dwDriverId)
{
    if (dwDriverId != NULL)
    {
#ifdef AC3ACM_LOGFILE
		const MyData *md = (MyData *)dwDriverId;

		if (md->fLogFile != NULL && md->pdod != NULL)
		{
			fprintf(md->fLogFile, "DRV_CLOSE\n");
			fclose(md->fLogFile);
		}
#endif

		LocalFree((HLOCAL)dwDriverId);
	}

	return (LRESULT)TRUE;
}


static LRESULT driver_details(DWORD_PTR dwDriverId, LPARAM lParam1)
{
	const MyData *md = (MyData *)dwDriverId;
	ACMDRIVERDETAILSW tmp = { 0 };

	int cbStruct = ((ACMDRIVERDETAILSW *)lParam1)->cbStruct;

	if (cbStruct > sizeof(tmp)) cbStruct = sizeof(tmp);

	tmp.cbStruct    = cbStruct;
	tmp.fccType     = ACMDRIVERDETAILS_FCCTYPE_AUDIOCODEC;
	tmp.fccComp     = ACMDRIVERDETAILS_FCCCOMP_UNDEFINED;
	tmp.wMid        = 0;
	tmp.wPid        = 0;
	tmp.vdwACM      = 0x03320000;   // 3.50
	tmp.vdwDriver   = 0x02020000;   // 2.02
	tmp.fdwSupport  = ACMDRIVERDETAILS_SUPPORTF_CODEC;
	tmp.cFormatTags = 2;
	tmp.cFilterTags = 0;
	tmp.hicon       = NULL;

	if (!(md->dwFlags & AC3ACM_NOEXTENSIBLE))
	{
		++tmp.cFormatTags;
	}

	if (cbStruct >= (FIELD_OFFSET(ACMDRIVERDETAILSW, szShortName) + RTL_FIELD_SIZE(ACMDRIVERDETAILSW, szShortName)))
		LoadResString(md->hmod, IDS_SHORTNAME, tmp.szShortName, ACMDRIVERDETAILS_SHORTNAME_CHARS);

	if (cbStruct >= (FIELD_OFFSET(ACMDRIVERDETAILSW, szLongName) + RTL_FIELD_SIZE(ACMDRIVERDETAILSW, szLongName)))
		LoadResString(md->hmod, IDS_LONGNAME, tmp.szLongName, ACMDRIVERDETAILS_LONGNAME_CHARS);

	if (cbStruct >= (FIELD_OFFSET(ACMDRIVERDETAILSW, szCopyright) + RTL_FIELD_SIZE(ACMDRIVERDETAILSW, szCopyright)))
		LoadResString(md->hmod, IDS_COPYRIGHT, tmp.szCopyright, ACMDRIVERDETAILS_COPYRIGHT_CHARS);

	if (cbStruct >= (FIELD_OFFSET(ACMDRIVERDETAILSW, szLicensing) + RTL_FIELD_SIZE(ACMDRIVERDETAILSW, szLicensing)))
		LoadResString(md->hmod, IDS_LICENSING, tmp.szLicensing, ACMDRIVERDETAILS_LICENSING_CHARS);

	if (cbStruct >= (FIELD_OFFSET(ACMDRIVERDETAILSW, szFeatures) + RTL_FIELD_SIZE(ACMDRIVERDETAILSW, szFeatures)))
		LoadResString(md->hmod, IDS_FEATURES, tmp.szFeatures, ACMDRIVERDETAILS_FEATURES_CHARS);

	memcpy((void *)lParam1, &tmp, cbStruct);

#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		fprintf(md->fLogFile, "ACMDM_DRIVERDETAILS\n");
		fflush(md->fLogFile);
	}
#endif

	return MMSYSERR_NOERROR;
}


static LRESULT format_suggest(DWORD_PTR dwDriverId, LPARAM lParam1)
{
	const MyData *md = (MyData *)dwDriverId;
	ACMDRVFORMATSUGGEST *fs = (ACMDRVFORMATSUGGEST *)lParam1;

	WAVEFORMATEX *src = fs->pwfxSrc;
	WAVEFORMATEX *dst = fs->pwfxDst;

	const int flags = fs->fdwSuggest & (
		ACM_FORMATSUGGESTF_WFORMATTAG |			/* 0x00010000L */
		ACM_FORMATSUGGESTF_NCHANNELS |			/* 0x00020000L */
		ACM_FORMATSUGGESTF_NSAMPLESPERSEC |		/* 0x00040000L */
		ACM_FORMATSUGGESTF_WBITSPERSAMPLE		/* 0x00080000L */
	);

	if ((~flags) & fs->fdwSuggest)
	{
#ifdef AC3ACM_LOGFILE
		if (md->fLogFile != NULL)
		{
			fprintf(md->fLogFile, "ACMDM_FORMAT_SUGGEST: unsupported fdwSuggest (0x%08X)\n", fs->fdwSuggest);
			fflush(md->fLogFile);
		}
#endif
		return MMSYSERR_NOTSUPPORTED;
	}

	if (IsValidAC3(src, md))
	{
		if (flags & ACM_FORMATSUGGESTF_WFORMATTAG)
		{
			// ACM gives us a dst format tag to verify.
			if (dst->wFormatTag != WAVE_FORMAT_PCM)
			{
				if (dst->wFormatTag != WAVE_FORMAT_EXTENSIBLE
					|| fs->cbwfxDst < sizeof(WAVEFORMATEXTENSIBLE))
				{
					goto Abort;
				}
			}
		}
		else
		{
			// ACM wants us to suggest a dst format tag.
			if (!(md->dwFlags & AC3ACM_NOEXTENSIBLE)
			  && src->nChannels > 2
			  && fs->cbwfxDst >= sizeof(WAVEFORMATEXTENSIBLE))
			{
				dst->wFormatTag = WAVE_FORMAT_EXTENSIBLE;
			}
			else
			{
				dst->wFormatTag = WAVE_FORMAT_PCM;
			}
		}

		if (flags & ACM_FORMATSUGGESTF_NCHANNELS)
		{
			// ACM gives us a dst channel count to verify.
			// We can convert anything to mono/stereo.
			if (dst->nChannels != 1 && dst->nChannels != 2)
			{
				// Are we allowed to return multichannel PCM?
				if (!(md->dwFlags & AC3ACM_MULTICHANNEL))
				{
					if (dst->wFormatTag == WAVE_FORMAT_PCM) goto Abort;
				}

				// Dst channels must be the same as src channels.
				if (dst->nChannels != src->nChannels) goto Abort;
			}
		}
		else
		{
			// ACM wants us to suggest a dst channel count.
			if (dst->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
			{
				dst->nChannels = src->nChannels;
			}
			else
			{
				if (!(md->dwFlags & AC3ACM_MULTICHANNEL))
				{
					// We prefer downmixing to stereo PCM.
					dst->nChannels = (src->nChannels > 1)? 2: 1;
				}
				else
				{
					dst->nChannels = src->nChannels;
				}
			}
		}

		if (flags & ACM_FORMATSUGGESTF_NSAMPLESPERSEC)
		{
			// ACM gives us a dst sample rate to verify.
			// We do not convert, so it must match the src.
			if (dst->nSamplesPerSec != src->nSamplesPerSec) goto Abort;
		}
		else
		{
			// ACM wants us to suggest a dst sample rate.
			dst->nSamplesPerSec = src->nSamplesPerSec;
		}

		// We only support 16-bit PCM
		if (flags & ACM_FORMATSUGGESTF_WBITSPERSAMPLE)
		{
			// ACM gives us a bit depth to verify.
			if (dst->wBitsPerSample != 16) goto Abort;
		}
		else
		{
			// ACM wants us to suggest a dst bit depth.
			dst->wBitsPerSample = 16;
		}

		dst->nBlockAlign     = 2 * dst->nChannels;
		dst->nAvgBytesPerSec = dst->nBlockAlign * dst->nSamplesPerSec;

		if (fs->cbwfxDst >= sizeof(WAVEFORMATEX))
		{
			if (dst->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
			{
				WAVEFORMATEXTENSIBLE *wfx = (WAVEFORMATEXTENSIBLE *)dst;

				wfx->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
				wfx->Samples.wValidBitsPerSample = 16;
				wfx->dwChannelMask = channel_masks[src->nChannels - 1];
				wfx->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
			}
			else
			{
				dst->cbSize = 0;
			}
		}
	}
	else if (IsValidPCM(src, md))
	{
		if (flags & ACM_FORMATSUGGESTF_WFORMATTAG)
		{
			// ACM gives us a dst format tag to verify.
			if (dst->wFormatTag != WAVE_FORMAT_AC3)
			{
				if (dst->wFormatTag != WAVE_FORMAT_EXTENSIBLE
					|| fs->cbwfxDst < sizeof(WAVEFORMATEXTENSIBLE))
				{
					goto Abort;
				}
			}
		}
		else
		{
			// ACM wants us to suggest a dst format tag.
			dst->wFormatTag = WAVE_FORMAT_AC3;
		}

		if (flags & ACM_FORMATSUGGESTF_NCHANNELS)
		{
			// We won't make an AC-3 with a different
			// number of channels than the PCM source
			if (dst->nChannels != src->nChannels) goto Abort;
		}
		else
		{
			dst->nChannels = src->nChannels;
		}

		// We do not encode at low sample rates
		if (src->nSamplesPerSec < 32000) goto Abort;

		// We don't convert the sample rate
		if (flags & ACM_FORMATSUGGESTF_NSAMPLESPERSEC)
		{
			if (dst->nSamplesPerSec != src->nSamplesPerSec) goto Abort;
		}
		else
		{
			dst->nSamplesPerSec = src->nSamplesPerSec;
		}

		// Hmm...
		if (flags & ACM_FORMATSUGGESTF_WBITSPERSAMPLE) {
			if (dst->wBitsPerSample) goto Abort;
		} else {
			dst->wBitsPerSample = 0;
		}

		// Suggest a bitrate based on nChannels
		if (md->dwFlags & AC3ACM_USE64) {
			dst->nAvgBytesPerSec = 64 * 125 * src->nChannels;
		} else {
			dst->nAvgBytesPerSec = 96 * 125 * src->nChannels;
		}

		// New discovery September 21, 2007:
		// ac3enc does NOT pad its 44100 Hz blocks

		for (int i = 0; i < 19; ++i)
		{
			if (dst->nAvgBytesPerSec == 125UL * framesizes[i][3])
			{
				dst->nBlockAlign = framesizes[i][(dst->nSamplesPerSec >> 6) & 3] * 2;
				break;
			}
		}

		// We need to refine nAvgBytesPerSec for 44100 Hz frames:
		//	At 32000 Hz, each frame is 1536/32000 = 0.048 seconds
		//	At 44100 Hz, each frame is 1536/44100 = 0.0348299 seconds
		//	At 48000 Hz, each frame is 1536/48000 = 0.032 seconds
		// nAvgBytesPerSec = (nBlockAlign * 44100) / 1536;

		if (src->nSamplesPerSec == 44100)
		{
			dst->nAvgBytesPerSec = ((dst->nBlockAlign * 44100) + 768) / 1536;
		}

		if (fs->cbwfxDst >= sizeof(WAVEFORMATEX))
		{
			if (dst->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
			{
				WAVEFORMATEXTENSIBLE *wfx = (WAVEFORMATEXTENSIBLE *)dst;

				wfx->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
				wfx->Samples.wSamplesPerBlock = 1536;
				wfx->dwChannelMask = channel_masks[src->nChannels - 1];
//				wfx->SubFormat = KSDATAFORMAT_SUBTYPE_AC3_AUDIO;
				wfx->SubFormat = GUID_AC3ACM_EXTENSIBLE;
			}
			else
			{
				dst->cbSize = 0;
			}
		}
	}
	else
	{
		// Unknown source format
		goto Abort;
	}

#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		fprintf(md->fLogFile, "ACMDM_FORMAT_SUGGEST:\n");
		fprintf(md->fLogFile, " fdwSuggest = 0x%08X\n", fs->fdwSuggest);

		fprintf(md->fLogFile, " source:\n");
		log_format(dwDriverId, src);

		fprintf(md->fLogFile, " destination:\n");
		log_format(dwDriverId, dst);

		fflush(md->fLogFile);
	}
#endif

	return MMSYSERR_NOERROR;

Abort:
#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		fprintf(md->fLogFile, "ACMDM_FORMAT_SUGGEST: not possible\n");
		fprintf(md->fLogFile, " fdwSuggest = 0x%08X\n", fs->fdwSuggest);

		fprintf(md->fLogFile, " source:\n");
		log_format(dwDriverId, src);

		fprintf(md->fLogFile, " destination:\n");
		log_format(dwDriverId, dst);

		fflush(md->fLogFile);
	}
#endif

	return ACMERR_NOTPOSSIBLE;
}


static LRESULT formattag_details(DWORD_PTR dwDriverId, LPARAM lParam1, LPARAM lParam2)
{
	ACMFORMATTAGDETAILSW *fmtd = (ACMFORMATTAGDETAILSW *)lParam1;
	const MyData *md = (MyData *)dwDriverId;
	DWORD format;

#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		fprintf(md->fLogFile, "ACMDM_FORMATTAG_DETAILS\n");
		fflush(md->fLogFile);
	}
#endif

	switch (lParam2 & ACM_FORMATTAGDETAILSF_QUERYMASK) {

	case ACM_FORMATTAGDETAILSF_INDEX:

#ifdef AC3ACM_LOGFILE
		if (md->fLogFile != NULL)
		{
			fprintf(md->fLogFile, " ACM_FORMATTAGDETAILSF_INDEX\n");
			fprintf(md->fLogFile, " dwFormatTagIndex = %lu\n", fmtd->dwFormatTagIndex);
			fflush(md->fLogFile);
		}
#endif
		if (fmtd->dwFormatTagIndex == 0)
		{
			format = WAVE_FORMAT_PCM;
			break;
		}
		if (fmtd->dwFormatTagIndex == 1)
		{
			format = WAVE_FORMAT_AC3;
			break;
		}
		if (fmtd->dwFormatTagIndex == 2)
		{
			format = WAVE_FORMAT_EXTENSIBLE;
			break;
		}
		goto Abort;

	case ACM_FORMATTAGDETAILSF_FORMATTAG:

#ifdef AC3ACM_LOGFILE
		if (md->fLogFile != NULL)
		{
			fprintf(md->fLogFile, " ACM_FORMATTAGDETAILSF_FORMATTAG\n");
			fprintf(md->fLogFile, " dwFormatTag = 0x%04X\n", fmtd->dwFormatTag);
			fflush(md->fLogFile);
		}
#endif
		format = fmtd->dwFormatTag;

		if (format == WAVE_FORMAT_PCM) break;
		if (format == WAVE_FORMAT_AC3) break;
		if (format == WAVE_FORMAT_EXTENSIBLE) break;
		goto Abort;

	case ACM_FORMATTAGDETAILSF_LARGESTSIZE:

#ifdef AC3ACM_LOGFILE
		if (md->fLogFile != NULL)
		{
			fprintf(md->fLogFile, " ACM_FORMATTAGDETAILSF_LARGESTSIZE\n");
			fprintf(md->fLogFile, " dwFormatTag = 0x%04X\n", fmtd->dwFormatTag);
			fflush(md->fLogFile);
		}
#endif
		format = fmtd->dwFormatTag;
		switch (format)
		{
			case WAVE_FORMAT_UNKNOWN:
				if (!(md->dwFlags & AC3ACM_NOEXTENSIBLE)) {
					format = WAVE_FORMAT_EXTENSIBLE;
				} else {
					format = WAVE_FORMAT_AC3;
				}
				break;

			case WAVE_FORMAT_PCM:
			case WAVE_FORMAT_AC3:
			case WAVE_FORMAT_EXTENSIBLE:
				break;

			default:
				goto Abort;
		}

		break;

	default:
		return MMSYSERR_NOTSUPPORTED;
	}

	// Now "format" is the format to return details about.

	if (format == WAVE_FORMAT_PCM)
	{
		fmtd->dwFormatTag       = WAVE_FORMAT_PCM;
		fmtd->dwFormatTagIndex  = 0;
        fmtd->cbFormatSize      = sizeof(PCMWAVEFORMAT);
		fmtd->fdwSupport        = ACMDRIVERDETAILS_SUPPORTF_CODEC;

		// Are we allowed to return multichannel PCM?
		if (md->dwFlags & AC3ACM_MULTICHANNEL) {
			fmtd->cStandardFormats = 3 * 6;
		} else {
			fmtd->cStandardFormats = 3 * 2;
		}
		fmtd->szFormatTag[0] = L'\0';
	}
	else if (format == WAVE_FORMAT_AC3)
	{
		fmtd->dwFormatTag       = WAVE_FORMAT_AC3;
		fmtd->dwFormatTagIndex  = 1;
		fmtd->cbFormatSize      = sizeof(WAVEFORMATEX);
		fmtd->fdwSupport        = ACMDRIVERDETAILS_SUPPORTF_CODEC;

        fmtd->cStandardFormats  = 3 * 6 * 19;

		LoadResString(md->hmod, IDS_FORMAT_2000,
			fmtd->szFormatTag, ACMFORMATTAGDETAILS_FORMATTAG_CHARS);
	}
	else    // WAVE_FORMAT_EXTENSIBLE
	{
		fmtd->dwFormatTag       = WAVE_FORMAT_EXTENSIBLE;
		fmtd->dwFormatTagIndex  = 2;
		fmtd->cbFormatSize      = sizeof(WAVEFORMATEXTENSIBLE);
		fmtd->fdwSupport        = ACMDRIVERDETAILS_SUPPORTF_CODEC
		                        | ACMDRIVERDETAILS_SUPPORTF_CONVERTER;

        fmtd->cStandardFormats  = (3 * 6) + (3 * 6 * 19);

		LoadResString(md->hmod, IDS_FORMAT_FFFE,
			fmtd->szFormatTag, ACMFORMATTAGDETAILS_FORMATTAG_CHARS);
	}

	if (fmtd->cbStruct > sizeof(ACMFORMATTAGDETAILSW))
		fmtd->cbStruct = sizeof(ACMFORMATTAGDETAILSW);

	return MMSYSERR_NOERROR;

Abort:
	return ACMERR_NOTPOSSIBLE;
}


static LRESULT format_details(DWORD_PTR dwDriverId, LPARAM lParam1, LPARAM lParam2)
{
	ACMFORMATDETAILSW *fmtd = (ACMFORMATDETAILSW *)lParam1;
	const MyData *md = (MyData *)dwDriverId;

	static LPCWSTR channels_string[6] = {
		L"Mono",
		L"Stereo",
		L"3/0 channels",
		L"2/2 channels",
		L"3/2 channels",
		L"5.1 channels"
	};

	WAVEFORMATEXTENSIBLE out = { 0 };
	int idiv, imod;

#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		fprintf(md->fLogFile, "ACMDM_FORMAT_DETAILS\n");
		fflush(md->fLogFile);
	}
#endif

	fmtd->fdwSupport = ACMDRIVERDETAILS_SUPPORTF_CODEC;
	fmtd->szFormat[0] = L'\0';

	switch (lParam2 & ACM_FORMATDETAILSF_QUERYMASK) {
	
	case ACM_FORMATDETAILSF_INDEX:

		switch (fmtd->dwFormatTag) {
		
		case WAVE_FORMAT_PCM:
			out.Format.wFormatTag = WAVE_FORMAT_PCM;

			// Are we allowed to return multichannel PCM?
			if (md->dwFlags & AC3ACM_MULTICHANNEL) {
				if (fmtd->dwFormatIndex >= 3 * 6) goto Abort;
			} else {	// 2 channels max
				if (fmtd->dwFormatIndex >= 3 * 2) goto Abort;
			}

			idiv = fmtd->dwFormatIndex / 3;
			imod = fmtd->dwFormatIndex % 3;

			out.Format.nChannels       = idiv + 1;
			out.Format.nSamplesPerSec  = srates[imod];
			out.Format.nBlockAlign     = 2 * out.Format.nChannels;
			out.Format.nAvgBytesPerSec = out.Format.nBlockAlign * out.Format.nSamplesPerSec;
			out.Format.wBitsPerSample  = 16;
			out.Format.cbSize          = 0;
            break;

        case WAVE_FORMAT_AC3:
            if (fmtd->dwFormatIndex >= 3 * 6 * 19) goto Abort;

			idiv = fmtd->dwFormatIndex / (19 * 3);
			imod = fmtd->dwFormatIndex % (19 * 3);

            out.Format.wFormatTag      = WAVE_FORMAT_AC3;
			out.Format.nChannels       = idiv + 1;

			idiv = imod / 19;
			imod = imod % 19;

			out.Format.nSamplesPerSec  = srates[idiv];
			out.Format.nAvgBytesPerSec = 125 * framesizes[imod][3];

			// New discovery September 21, 2007:
			// ac3enc does NOT pad its 44100 Hz blocks

			out.Format.nBlockAlign = (WORD)(framesizes[imod]
				[(out.Format.nSamplesPerSec >> 6) & 3] * 2);

			// We need to refine nAvgBytesPerSec for 44100 Hz frames:

			if (out.Format.nSamplesPerSec == 44100)
			{
				out.Format.nAvgBytesPerSec = ((out.Format.nBlockAlign * 44100) + 768) / 1536;
			}

			out.Format.wBitsPerSample  = 0;
			out.Format.cbSize          = 0;

			// AC-3 description string
			_snwprintf(fmtd->szFormat,
				ACMFORMATDETAILS_FORMAT_CHARS,
				L"%hu kBit/s, %lu Hz, %s",
				framesizes[imod][3],
				out.Format.nSamplesPerSec,
				channels_string[out.Format.nChannels - 1]
			);
			fmtd->szFormat[ACMFORMATDETAILS_FORMAT_CHARS - 1] = L'\0';

            break;

		case WAVE_FORMAT_EXTENSIBLE:
			out.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;

			if (fmtd->dwFormatIndex >= (3 * 6 + 3 * 6 * 19)) goto Abort;
			
			if (fmtd->dwFormatIndex < (3 * 6))
			{
				idiv = fmtd->dwFormatIndex / 3;
				imod = fmtd->dwFormatIndex % 3;

				out.Format.nChannels       = idiv + 1;
				out.Format.nSamplesPerSec  = srates[imod];
				out.Format.nBlockAlign     = 2 * out.Format.nChannels;
				out.Format.nAvgBytesPerSec = out.Format.nBlockAlign * out.Format.nSamplesPerSec;
				out.Format.wBitsPerSample  = 16;
				out.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

				out.dwChannelMask          = channel_masks[out.Format.nChannels - 1];
				out.Samples.wValidBitsPerSample = 16;
				out.SubFormat              = KSDATAFORMAT_SUBTYPE_PCM;
			}
			else
			{
				idiv = (fmtd->dwFormatIndex - (3 * 6)) / (19 * 3);
				imod = (fmtd->dwFormatIndex - (3 * 6)) % (19 * 3);

				out.Format.nChannels       = idiv + 1;

				idiv = imod / 19;
				imod = imod % 19;

				out.Format.nSamplesPerSec  = srates[idiv];
				out.Format.nAvgBytesPerSec = 125 * framesizes[imod][3];

				out.Format.nBlockAlign = (WORD)(framesizes[imod]
				    [(out.Format.nSamplesPerSec >> 6) & 3] * 2);

				// We need to refine nAvgBytesPerSec for 44100 Hz frames:

				if (out.Format.nSamplesPerSec == 44100)
				{
					out.Format.nAvgBytesPerSec = ((out.Format.nBlockAlign * 44100) + 768) / 1536;
				}

				out.Format.wBitsPerSample  = 0;
				out.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

				out.dwChannelMask          = channel_masks[out.Format.nChannels - 1];
				out.Samples.wSamplesPerBlock = 1536;
//				out.SubFormat              = KSDATAFORMAT_SUBTYPE_AC3_AUDIO;
				out.SubFormat              = GUID_AC3ACM_EXTENSIBLE;

				// AC-3 description string
				_snwprintf(fmtd->szFormat,
					ACMFORMATDETAILS_FORMAT_CHARS,
					L"%hu kBit/s, %lu Hz, %s",
					framesizes[imod][3],
					out.Format.nSamplesPerSec,
					channels_string[out.Format.nChannels - 1]
				);
				fmtd->szFormat[ACMFORMATDETAILS_FORMAT_CHARS - 1] = L'\0';
			}
			break;

		default:
			goto Abort;
		}

		idiv = fmtd->cbwfx;
		if (idiv > 0)
		{
			if (idiv > sizeof(WAVEFORMATEXTENSIBLE))
				idiv = sizeof(WAVEFORMATEXTENSIBLE);

			memcpy(fmtd->pwfx, &out, idiv);
		}
		break;

	case ACM_FORMATDETAILSF_FORMAT:

		switch (fmtd->dwFormatTag) {
		
		case WAVE_FORMAT_PCM:
			if (!IsValidPCM(fmtd->pwfx, md)) goto Abort;

			// Are we allowed to return multichannel PCM?
			if ((md->dwFlags & AC3ACM_MULTICHANNEL) == 0)
			{
				if (fmtd->pwfx->nChannels > 2) goto Abort;
			}

			break;

		case WAVE_FORMAT_AC3:
			if (!IsValidAC3(fmtd->pwfx, md)) goto Abort;
			break;

		case WAVE_FORMAT_EXTENSIBLE:
			if (!IsValidPCMEX(fmtd->pwfx))
			{
				if (!IsValidAC3EX(fmtd->pwfx)) goto Abort;
			}
			break;

		default:
			goto Abort;
		}
		break;

	default:
		return MMSYSERR_NOTSUPPORTED;
	}

	if (fmtd->cbStruct > sizeof(ACMFORMATDETAILSW))
		fmtd->cbStruct = sizeof(ACMFORMATDETAILSW);

	return MMSYSERR_NOERROR;

Abort:
	return ACMERR_NOTPOSSIBLE;
}


/*	There are a total of 16 channel configs allowed for AC-3:

	acmod  config channels	Microsoft's suggestion
	-----  ------ --------	----------------------
	'000'	1+1		2		FL,FR
	'001'	1/0		1		FC
	'010'	2/0		2		FL,FR
	'011'	3/0		3		FL,FR,FC
	'100'	2/1		3		FL,FR,BC
	'101'	3/1		4		FL,FR,FC,BC
	'110'	2/2		4		FL,FR,BL,BR
	'111'	3/2		5		FL,FR,FC,BL,BR

	(All of these may include a low frequency channel)

	'000'	1+1+1	3		FL,FR,LF
	'001'	1/0+1	2		FC,LF
	'010'	2/0+1	3		FL,FR,LF
	'011'	3/0+1	4		FL,FR,FC,LF
	'100'	2/1+1	4		FL,FR,LF,BC
	'101'	3/1+1	5		FL,FR,FC,LF,BC
	'110'	2/2+1	5		FL,FR,LF,BL,BR
	'111'	3/2+1	6		FL,FR,FC,LF,BL,BR

	There isn't enough info in the WAVEFORMATEX	structure to
	derive the correct mappings. Can it be done in convert proc?
*/

static inline LRESULT stream_convert_ac3(DWORD_PTR dwDriverId, LPARAM lParam1, LPARAM lParam2)
{
	const MyData *md = (MyData *)dwDriverId;
	const ACMDRVSTREAMINSTANCE *si;

	ACMDRVSTREAMHEADER *sh;
	MyStreamData *msd;
	const unsigned char *src;
	unsigned char *dst;

	long srcLen, dstLen;
	level_t a52level;
	int a52flags;
	int sr, sr2, br, fs;

	sh = (ACMDRVSTREAMHEADER *)lParam2;
	sh->cbSrcLengthUsed = 0;
	sh->cbDstLengthUsed = 0;

	src = (unsigned char *)sh->pbSrc;
	dst = (unsigned char *)sh->pbDst;
	srcLen = sh->cbSrcLength;
	dstLen = sh->cbDstLength;

	si = (ACMDRVSTREAMINSTANCE *)lParam1;
	msd = (MyStreamData *)si->dwDriver;

#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		fprintf(md->fLogFile, "ACMDM_STREAM_CONVERT (from AC-3 to PCM):\n");
		fprintf(md->fLogFile, " pbSrc = 0x%p, cbSrcLength = %ld\n", src, srcLen);
		fprintf(md->fLogFile, " pbDst = 0x%p, cbDstLength = %ld\n", dst, dstLen);
		fflush(md->fLogFile);
	}
#endif

	if (sh->fdwConvert & ACM_STREAMCONVERTF_START)
	{
		msd->bufptr = msd->bufend = msd->buf;
		msd->blocks = 0;
	}
	else if (msd->blocks > 0)
	{
		// Blocks left over from the last call
		sr = 512 * si->pwfxDst->nChannels;
		do {
			if ((dstLen -= sr) < 0) goto Abort;		// out of space
			a52_block(msd->a52state);
			msd->convert(a52_samples(msd->a52state), dst, msd->flags);
			sh->cbDstLengthUsed += sr;
			dst += sr;
		} while (--msd->blocks);
	}

	// nBlockAlign can be 1, so we can't rely on receiving
	// complete frames.  We want to use the entire source,
	// so some rather tricky buffering is required here...

	for (;;)
	{
		fs = 128 - (int)(msd->bufend - msd->buf);
		sr = (int)(msd->bufend - msd->bufptr);

		if (sr >= 8)
		{
			for (;;)
			{
				if (fs = a52_syncinfo((uint8_t *)msd->bufptr, &a52flags, &sr2, &br))
				{
					if (sr < fs)
					{
						fs -= sr;
						break;		// need fs more bytes of data
					}

// The following are the only possible "flags"
// values which can be returned by ac3_syncinfo:
//		A52_CHANNEL		flags = 0000
//		A52_MONO		flags = 0001
//		A52_STEREO		flags = 0010
//		A52_3F			flags = 0011
//		A52_2F1R		flags = 0100
//		A52_3F1R		flags = 0101
//		A52_2F2R		flags = 0110
//		A52_3F2R		flags = 0111
//		A52_DOLBY		flags = 1010 (mutually exclusive)
// All of the above can be combined with A52_LFE (10000)

					// Do we need to convert the channel count?
					if (si->pwfxDst->nChannels != si->pwfxSrc->nChannels)
					{
						// Are we downmixing to fewer channels?
						if (si->pwfxDst->nChannels < si->pwfxSrc->nChannels)
						{
							if (si->pwfxDst->nChannels == 1)
							{
								// downmixing to mono
								a52flags = A52_MONO;
							}
							else
							{
								// downmixing 2+ channels to 2
								if ((a52flags & A52_CHANNEL_MASK) != A52_DOLBY)
								{
									if (md->dwFlags & AC3ACM_DOLBYSURROUND) {
										// Downmix to Dolby Surround
										a52flags = A52_DOLBY;
									} else {
										// Downmix to Stereo
										a52flags = A52_STEREO;
									}
								}
							}
						}
						else
						{
							// Upmix mono to stereo
							a52flags = A52_STEREO;
						}
					}

					// Store flags for ConvertProc:
					msd->flags = a52flags;

					a52flags |= A52_ADJUST_LEVEL;
					a52level = 1.0;

					// Using 384.0 with level 1.0 causes the sample values
					// to range from 383.0 (minimum) to 385.0 (over the top)

					a52_frame(msd->a52state, (uint8_t *)msd->bufptr, &a52flags, &a52level, 384.0);

					msd->bufptr = msd->bufend = msd->buf;

					// sr = destination stride (256 samples)
					sr = 512 * si->pwfxDst->nChannels;

					// If it is impossible for a complete frame to follow
					// this one, then don't decode all of the blocks (this
					// is necessary when the source buffer is too small)
					fs = (sh->cbSrcLength * 2 - sh->cbSrcLengthUsed) < (DWORD)fs;

					// decode the blocks
					for (msd->blocks = 6; msd->blocks > fs; --msd->blocks)
					{
						if ((dstLen -= sr) < 0) goto Abort;	// out of space
						a52_block(msd->a52state);
						msd->convert(a52_samples(msd->a52state), dst, msd->flags);
						sh->cbDstLengthUsed += sr;
						dst += sr;
					}

					fs = 128;
					break;
				}

				++msd->bufptr;
				--sr;

				if (sr < 8)
				{
					for (sr = 0; sr < 8; sr++)
					{
						msd->buf[sr] = msd->bufptr[sr];
					}
					msd->bufptr = msd->buf;
					msd->bufend = msd->buf + 8;
					fs = 120;
					break;
				}
			}
		}

		if (srcLen <= 0) break;

		if (fs > srcLen) fs = srcLen;

		memcpy(msd->bufend, src, fs);
		msd->bufend += fs;

		sh->cbSrcLengthUsed += fs;
		srcLen -= fs;
		src += fs;
	}

Abort:

#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		fprintf(md->fLogFile, " cbSrcLengthUsed = %lu\n", sh->cbSrcLengthUsed);
		fprintf(md->fLogFile, " cbDstLengthUsed = %lu\n", sh->cbDstLengthUsed);
		fflush(md->fLogFile);
	}
#endif

	return MMSYSERR_NOERROR;
}


static inline void create_channel_map(const WAVEFORMATEX *wfex, unsigned char *dst)
{
	switch (wfex->nChannels) {

	case 1:	// FC -> FC
	case 2: // FL,FR -> FL,FR
	case 4: // FL,FR,BL,BR -> FL,FR,BL,BR
		dst[0] = 0;
		dst[1] = 1;
		dst[2] = 2;
		dst[3] = 3;
		break;

	case 3: // FL,FR,FC -> FL,FC,FR
	case 5: // FL,FR,FC,BL,BR -> FL,FC,FR,BL,BR
		dst[0] = 0;
		dst[1] = 2;
		dst[2] = 1;
		dst[3] = 3;
		dst[4] = 4;
		break;

	case 6: // FL,FR,FC,LF,BL,BR -> FL,FC,FR,BL,BR,LF
		dst[0] = 0;
		dst[1] = 2;
		dst[2] = 1;
		dst[3] = 4;
		dst[4] = 5;
		dst[5] = 3;
		break;
	}
}


static inline LRESULT stream_convert_pcm(DWORD_PTR dwDriverId, LPARAM lParam1, LPARAM lParam2)
{
	const MyData *md = (MyData *)dwDriverId;
	const ACMDRVSTREAMINSTANCE *si;

	ACMDRVSTREAMHEADER *sh;
	MyStreamData *msd;
	const unsigned char *src;
	unsigned char *dst;

	long srcLen, dstLen;
	int fs;
	int needed;

	unsigned char chmap[8];

	sh = (ACMDRVSTREAMHEADER *)lParam2;
	sh->cbSrcLengthUsed = 0;
	sh->cbDstLengthUsed = 0;

	src = (unsigned char *)sh->pbSrc;
	dst = (unsigned char *)sh->pbDst;
	srcLen = sh->cbSrcLength;
	dstLen = sh->cbDstLength;

	si = (ACMDRVSTREAMINSTANCE *)lParam1;
	msd = (MyStreamData *)si->dwDriver;

#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		fprintf(md->fLogFile, "ACMDM_STREAM_CONVERT (from PCM to AC-3):\n");
		fprintf(md->fLogFile, " pbSrc = 0x%p, cbSrcLength = %ld\n", src, srcLen);
		fprintf(md->fLogFile, " pbDst = 0x%p, cbDstLength = %ld\n", dst, dstLen);
		fflush(md->fLogFile);
	}
#endif

	if (sh->fdwConvert & ACM_STREAMCONVERTF_START)
	{
		msd->bufptr = msd->buf;
		msd->bufend = msd->frame;
		msd->blocks = 0;
	}
	else if (msd->blocks > 0)
	{
		int tc = msd->blocks;
		if (tc > dstLen) tc = dstLen;

#ifdef AC3ACM_LOGFILE
		if (md->fLogFile != NULL)
		{
			fprintf(md->fLogFile, " %i bytes carried over\n", msd->blocks);
			fflush(md->fLogFile);
		}
#endif
		if (tc > 0)
		{
			memcpy(dst, msd->bufend, tc);
			sh->cbDstLengthUsed += tc;
			msd->blocks -= tc;
			msd->bufend += tc;
			dstLen -= tc;
			dst += tc;
		}
		if (dstLen <= 0) goto Abort;
	}

	// For working with WAVE_FORMAT_EXTENSIBLE,
	// create a channel map for ac3enc...
	create_channel_map(si->pwfxSrc, chmap);

	// "needed" is a constant
	needed = 1536 * si->pwfxSrc->nChannels * sizeof(short);

	while (srcLen > 0)
	{
		fs = (int)(msd->bufptr - msd->buf);
	
		if (fs < needed)
		{
			int tc = needed - fs;
			if (tc > srcLen) tc = srcLen;

			memcpy(msd->bufptr, src, tc);

			sh->cbSrcLengthUsed += tc;
			msd->bufptr += tc;
			srcLen -= tc;
			src += tc;
			fs += tc;
		}

		if (fs >= needed)
		{
			int tc;

			tc = AC3_encode_frame(msd->frame, (short *)msd->buf, chmap);

			msd->bufptr = msd->buf;
			fs = 0;

			msd->bufend = msd->frame;
			msd->blocks = tc;

			if (tc > dstLen) tc = dstLen;

			if (tc > 0)
			{
				memcpy(dst, msd->frame, tc);
				sh->cbDstLengthUsed += tc;
				msd->blocks -= tc;
				msd->bufend += tc;
				dstLen -= tc;
				dst += tc;
			}

			if (dstLen <= 0) break;
		}
	}

Abort:

#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		fprintf(md->fLogFile, " cbSrcLengthUsed = %lu\n", sh->cbSrcLengthUsed);
		fprintf(md->fLogFile, " cbDstLengthUsed = %lu\n", sh->cbDstLengthUsed);
		fflush(md->fLogFile);
	}
#endif

	return MMSYSERR_NOERROR;
}


static inline LRESULT stream_copy(DWORD_PTR dwDriverId, LPARAM lParam1, LPARAM lParam2)
{
	const MyData *md = (MyData *)dwDriverId;

	ACMDRVSTREAMHEADER *sh;
	sh = (ACMDRVSTREAMHEADER *)lParam2;

#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		fprintf(md->fLogFile, "ACMDM_STREAM_CONVERT (copy):\n");
		fprintf(md->fLogFile, " pbSrc = 0x%p, cbSrcLength = %ld\n", sh->pbSrc, sh->cbSrcLength);
		fprintf(md->fLogFile, " pbDst = 0x%p, cbDstLength = %ld\n", sh->pbDst, sh->cbDstLength);
		fflush(md->fLogFile);
	}
#endif

	unsigned long tc = sh->cbSrcLength;
	if (tc > sh->cbDstLength) tc = sh->cbDstLength;

	memcpy(sh->pbDst, sh->pbSrc, tc);
	sh->cbSrcLengthUsed = tc;
	sh->cbDstLengthUsed = tc;

	return MMSYSERR_NOERROR;
}


static LRESULT stream_convert(DWORD_PTR dwDriverId, LPARAM lParam1, LPARAM lParam2)
{
	const ACMDRVSTREAMINSTANCE *si = (ACMDRVSTREAMINSTANCE *)lParam1;
	const MyData *md = (MyData *)dwDriverId;

	if (IsValidPCM(si->pwfxSrc, md))
	{
		if (IsValidAC3(si->pwfxDst, md))
			return stream_convert_pcm(dwDriverId, lParam1, lParam2);

		if (IsValidPCM(si->pwfxDst, md))
			return stream_copy(dwDriverId, lParam1, lParam2);
	}
	else if (IsValidAC3(si->pwfxSrc, md))
	{
		// AC3 or AC3EX to PCM or PCMEX
		if (IsValidPCM(si->pwfxDst, md))
			return stream_convert_ac3(dwDriverId, lParam1, lParam2);

		if (IsValidAC3(si->pwfxDst, md))
			return stream_copy(dwDriverId, lParam1, lParam2);
	}

	return MMSYSERR_NOTSUPPORTED;
}


static LRESULT stream_open(DWORD_PTR dwDriverId, LPARAM lParam1)
{
	const MyData *md = (MyData *)dwDriverId;
	ACMDRVSTREAMINSTANCE *si = (ACMDRVSTREAMINSTANCE *)lParam1;

	int i, fl;

	si->dwDriver = NULL;

	if (!IsValidAC3(si->pwfxSrc, md))
	{
		bool bDstIsAC3;

		// Source is not AC-3
		if (!IsValidPCM(si->pwfxSrc, md)) goto Abort;

		// Source is PCM or PCMEX
		bDstIsAC3 = IsValidAC3(si->pwfxDst, md);
		if (!bDstIsAC3)
		{
			if (!IsValidPCM(si->pwfxDst, md)) goto Abort;

			// Are we allowed to return multichannel PCM?
			if (!(md->dwFlags & AC3ACM_MULTICHANNEL))
			{
				if ( si->pwfxDst->wFormatTag == WAVE_FORMAT_PCM
				  && si->pwfxDst->nChannels > 2)
				{
					return MMSYSERR_NOTSUPPORTED;
				}
			}
		}
		else
		{
			// We do not encode at low sample rates.
			if (si->pwfxSrc->nSamplesPerSec < 32000) goto Abort;
		}

		// We don't change the number of channels
		if (si->pwfxDst->nChannels != si->pwfxSrc->nChannels)
		{
			return ACMERR_NOTPOSSIBLE;
		}

		// We can't convert the sampling rate,
		if ( si->pwfxDst->nSamplesPerSec != si->pwfxSrc->nSamplesPerSec)
		{
			return MMSYSERR_NOTSUPPORTED;
		}

		if (!bDstIsAC3)
		{
			// PCM-to-PCM
			si->dwDriver = NULL;
			return MMSYSERR_NOERROR;
		}

		// nAvgBytesPerSec MUST be correct
		fl = si->pwfxDst->nAvgBytesPerSec / 125;
		for (i = 0; i < 19; ++i)
		{
			if (fl == framesizes[i][3]) break;
		}

		if (i == 19)
		{
			// Special case for 44100 Hz blocks:
			if (si->pwfxDst->nSamplesPerSec == 44100)
			{
				for (i = 0; i < 19; ++i)
				{
					fl = ((framesizes[i][1] * 2 * 44100) + 768) / 1536;
					if ((DWORD)fl == si->pwfxDst->nAvgBytesPerSec)
					{
						fl = framesizes[i][3];
						break;
					}
				}
			}
			if (i == 19) return MMSYSERR_NOTSUPPORTED;
		}

		if ((si->fdwOpen & ACM_STREAMOPENF_QUERY) == 0)
		{
			fl = AC3_encode_init(
				si->pwfxSrc->nSamplesPerSec, fl * 1000,
				si->pwfxSrc->nChannels);

			if (fl == 0) goto Abort;

			// Allocate all memory as a big block (about 23K)
			MyStreamData *msd = (MyStreamData *)VirtualAlloc(
				NULL,
				sizeof(MyStreamData) +
				1536 * 6 * sizeof(short) +
				4096,
				MEM_COMMIT, PAGE_READWRITE
			);

			if (msd == NULL) goto MemError;

			msd->a52state = NULL;

			// Align buffer to 16-byte boundary
			msd->buf = (unsigned char *)
				(((DWORD_PTR)msd + sizeof(MyStreamData) + 15) & (LONG_PTR)-16);

			msd->frame    = msd->buf + (1536 * 6 * sizeof(short));

			msd->bufptr   = msd->buf;
			msd->bufend   = msd->frame;
			msd->blocks   = 0;

			msd->framelen = fl;
			si->dwDriver  = (DWORD_PTR)msd;
		}
	}
	else
	{
		bool bDstIsPCM;

		// Source is valid AC-3
		ConvertProc converter;

		bDstIsPCM = IsValidPCM(si->pwfxDst, md);
		if (!bDstIsPCM)
		{
			if (!IsValidAC3(si->pwfxDst, md)) goto Abort;
			if (si->pwfxDst->nChannels != si->pwfxSrc->nChannels) goto Abort;
		}
		else
		{
			// Are we allowed to return multichannel PCM?
			if (!(md->dwFlags & AC3ACM_MULTICHANNEL))
			{
				if ( si->pwfxDst->wFormatTag == WAVE_FORMAT_PCM
				  && si->pwfxDst->nChannels > 2)
				{
					return MMSYSERR_NOTSUPPORTED;
				}
			}
		}

		// We can't convert the sampling rate
		if (si->pwfxDst->nSamplesPerSec != si->pwfxSrc->nSamplesPerSec)
		{
			return MMSYSERR_NOTSUPPORTED;
		}

		if (!bDstIsPCM)
		{
			// AC3-to-AC3
			si->dwDriver = NULL;
			return MMSYSERR_NOERROR;
		}

		// Determine if the conversion type is supported
		converter = MapTab[md->UseMMX]
				  [si->pwfxSrc->nChannels - 1]
				  [si->pwfxDst->nChannels - 1];

		if (converter == NULL)
		{
			return MMSYSERR_NOTSUPPORTED;
		}

		if ((si->fdwOpen & ACM_STREAMOPENF_QUERY) == 0)
		{
			// Allocate memory (about 16K total)
			MyStreamData *msd = (MyStreamData *)VirtualAlloc(
				NULL,
				sizeof(MyStreamData) + 4096 + 16,
				MEM_COMMIT, PAGE_READWRITE
			);

			if (msd == NULL) goto MemError;

			// Align buffer to 16-byte boundary
			msd->buf = (unsigned char *)
				(((DWORD_PTR)msd + sizeof(MyStreamData) + 15) & (LONG_PTR)-16);

			msd->bufptr = msd->bufend = msd->buf;
			msd->blocks = 0;

		// Note: the MM_ACCEL_X86_MMX flag actually has no effect

			msd->a52state = a52_init(
				(md->UseMMX)? MM_ACCEL_X86_MMX: 0);

			if (msd->a52state == NULL) goto MemError;

			if ((md->dwFlags & AC3ACM_DYNAMICRANGE) == 0)
			{
				a52_dynrng(msd->a52state, NULL, NULL);
			}

			msd->convert = converter;
			si->dwDriver = (DWORD_PTR)msd;

			msd->framelen = ac3_framesize(si->pwfxSrc);
		}
	}

#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		if (si->fdwOpen & ACM_STREAMOPENF_QUERY) {
			fprintf(md->fLogFile, "ACMDM_STREAM_OPEN: query succeeded\n");
		} else {
			fprintf(md->fLogFile, "ACMDM_STREAM_OPEN: succeeded\n");
		}

		fprintf(md->fLogFile, " source:\n");
		log_format(dwDriverId, si->pwfxSrc);

		fprintf(md->fLogFile, " destination:\n");
		log_format(dwDriverId, si->pwfxDst);

		fflush(md->fLogFile);
	}
#endif
	return MMSYSERR_NOERROR;

Abort:
#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		fprintf(md->fLogFile, "ACMDM_STREAM_OPEN: not possible\n");

		fprintf(md->fLogFile, " source:\n");
		log_format(dwDriverId, si->pwfxSrc);

		fprintf(md->fLogFile, " destination:\n");
		log_format(dwDriverId, si->pwfxDst);

		fflush(md->fLogFile);
	}
#endif
	return ACMERR_NOTPOSSIBLE;

MemError:
#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		fprintf(md->fLogFile, "ACMDM_STREAM_OPEN: out of memory\n");
		fflush(md->fLogFile);
	}
#endif
	return MMSYSERR_NOMEM;
}


static LRESULT stream_close(DWORD_PTR dwDriverId, LPARAM lParam1)
{
	const MyData *md = (MyData *)dwDriverId;
	ACMDRVSTREAMINSTANCE *si = (ACMDRVSTREAMINSTANCE *)lParam1;

	if (md->fccType != (FOURCC)0 && si != NULL)
	{
		MyStreamData *msd = (MyStreamData *)si->dwDriver;
		if (msd != NULL)
		{
			if (msd->a52state != NULL)
			{
				a52_free(msd->a52state);
			}
			VirtualFree((LPVOID)msd, 0, MEM_RELEASE);
			si->dwDriver = NULL;
		}
	}

#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		fprintf(md->fLogFile, "ACMDM_STREAM_CLOSE\n");
		fflush(md->fLogFile);
	}
#endif

	return MMSYSERR_NOERROR;
}


static LRESULT stream_size(DWORD_PTR dwDriverId, LPARAM lParam1, LPARAM lParam2)
{
	const ACMDRVSTREAMINSTANCE *si = (ACMDRVSTREAMINSTANCE *)lParam1;
	const MyStreamData *msd = (MyStreamData *)si->dwDriver;
	const MyData *md = (MyData *)dwDriverId;

	ACMDRVSTREAMSIZE *ss = (ACMDRVSTREAMSIZE *)lParam2;
	long len;

	// It seems that many clients are going to require us to
	// be able to use buffers of any size, so we mustn't fail
	// this message if at all possible...

	// For maximum compatibility, we will use ALL of the source
	// data during stream_convert, but we will not necessarily
	// fill all of the destination buffer.

	switch (ss->fdwSize & ACM_STREAMSIZEF_QUERYMASK) {

	case ACM_STREAMSIZEF_SOURCE:
		// Given a specified source buffer size, how large
		// does a destination buffer need to be in order
		// to hold all of the converted data?

		if (si->pwfxSrc->wFormatTag == WAVE_FORMAT_AC3)
		{
			// How many AC-3 frames in the source? (round up)
			len = (ss->cbSrcLength + (msd->framelen - 1)) / msd->framelen;
			if (len < 1) len = 1;

			// Each AC-3 frame produces 1536 PCM samples
			ss->cbDstLength = len * 1536 * si->pwfxDst->nBlockAlign;
		}
		else
		{
			// WAVE_FORMAT_PCM or WAVE_FORMAT_EXTENSIBLE

			// Given x amount of source audio samples, how
			// many AC-3 frames will this produce? (round up)

			int i = 1536 * si->pwfxSrc->nBlockAlign;
			int nFrames = (ss->cbSrcLength + (i - 1)) / i;
			if (nFrames < 1) nFrames = 1;

			// What bitrate will the destination be?
			len = si->pwfxDst->nAvgBytesPerSec / 125;

			for (i = 0; i < 19; ++i)
			{
				if (len == framesizes[i][3])
				{
					switch (si->pwfxDst->nSamplesPerSec) {

					case 32000:
						len = framesizes[i][0] * 2;
						break;

					case 44100:
						len = framesizes[i][1] * 2 + 2;
						break;

					case 48000:
						len = framesizes[i][2] * 2;
						break;
					}

					break;
				}
			}

			// This shouldn't happen, but if it does...
			if (i == 19)
			{
#ifdef AC3ACM_LOGFILE
				if (md->fLogFile)
				{
					fprintf(md->fLogFile, "ACMDM_STREAM_SIZE: i == 19 (should never happen!)\n");
					fflush(md->fLogFile);
				}
#endif
				ss->cbDstLength = 3840;
			}
			else
			{
				ss->cbDstLength = len * nFrames;
			}
		}
		break;

	case ACM_STREAMSIZEF_DESTINATION:
		// Given a specified destination buffer size, what
		// is the largest amount of source data that can be
		// specified without overflowing the destination buffer?

		if (si->pwfxSrc->wFormatTag == WAVE_FORMAT_AC3)
		{
			// How many AC-3 frames will fit in dst? (truncate?)
			len = ss->cbDstLength / (1536 * si->pwfxDst->nBlockAlign);

			if (len < 1)
			{
				// This happens when Cool Edit wants all 6 channels
				// but doesn't create a large enough output buffer!
				if (ss->cbDstLength < (256U * si->pwfxDst->nBlockAlign))
				{
					goto Abort;		// can't be done
				}
				//ss->cbSrcLength = 3840;
				ss->cbSrcLength = msd->framelen + 2;
			}
			else
			{
				ss->cbSrcLength = len * (msd->framelen + 2);
			}
		}
		else
		{
			// WAVE_FORMAT_PCM or WAVE_FORMAT_EXTENSIBLE

			// Based on the destination, calculate how many
			// PCM samples we can accept in the source buffer.
			// We need to know how big the AC-3 frames will be...
			int i, nFrames;

			// What bitrate will the destination be?
			len = si->pwfxDst->nAvgBytesPerSec / 125;

			for (i = 0; i < 19; ++i)
			{
				if (len == framesizes[i][3]) {

					// "len" is the AC-3 bitrate in kbps
					switch (si->pwfxDst->nSamplesPerSec) {

					case 32000:
						len = framesizes[i][0] * 2;
						break;

					case 44100:
						len = framesizes[i][1] * 2 + 2;
						break;

					case 48000:
						len = framesizes[i][2] * 2;
						break;
					}

					break;
				}
			}

			// This shouldn't happen, but if it does...
			if (i == 19)
			{
				nFrames = 1;
			}
			else
			{
				nFrames = ss->cbDstLength / len;
				if (nFrames < 1) nFrames = 1;
			}

			ss->cbSrcLength = 1536 * si->pwfxSrc->nBlockAlign * nFrames;
		}
		break;

	default:

#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		fprintf(md->fLogFile, "ACMDM_STREAM_SIZE: unsupported query\n");
		fprintf(md->fLogFile, " fdwSize = 0x%08X\n", ss->fdwSize);
		fflush(md->fLogFile);
	}
#endif
		return MMSYSERR_NOTSUPPORTED;
	}

#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		switch (ss->fdwSize & ACM_STREAMSIZEF_QUERYMASK) {

			case ACM_STREAMSIZEF_SOURCE:
				fprintf(md->fLogFile, "ACMDM_STREAM_SIZE: destination size query OK\n");
				break;

			case ACM_STREAMSIZEF_DESTINATION:
				fprintf(md->fLogFile, "ACMDM_STREAM_SIZE: source size query OK\n");
				break;
		}

		fprintf(md->fLogFile, " cbSrcLength = %lu\n", ss->cbSrcLength);
		fprintf(md->fLogFile, " cbDstLength = %lu\n", ss->cbDstLength);
		fprintf(md->fLogFile, " framelen = %i\n", msd->framelen);

		fflush(md->fLogFile);
	}
#endif
	return MMSYSERR_NOERROR;

Abort:
#ifdef AC3ACM_LOGFILE
	if (md->fLogFile != NULL)
	{
		switch (ss->fdwSize & ACM_STREAMSIZEF_QUERYMASK) {

			case ACM_STREAMSIZEF_SOURCE:
				fprintf(md->fLogFile, "ACMDM_STREAM_SIZE: destination size query failed\n");
				break;

			case ACM_STREAMSIZEF_DESTINATION:
				fprintf(md->fLogFile, "ACMDM_STREAM_SIZE: source size query failed\n");
		}

		fprintf(md->fLogFile, " cbSrcLength = %lu\n", ss->cbSrcLength);
		fprintf(md->fLogFile, " cbDstLength = %lu\n", ss->cbDstLength);
		fprintf(md->fLogFile, " framelen = %i\n", msd->framelen);

		fflush(md->fLogFile);
	}
#endif
	return ACMERR_NOTPOSSIBLE;
}


INT_PTR CALLBACK MyDialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {

	case WM_INITDIALOG:
	{
		const MyData *md = (MyData *)lParam;
		SetWindowLongPtr(hwndDlg, DWLP_USER, lParam);

		CheckDlgButton(hwndDlg, IDC_DOLBYSURROUND,
			(md->dwFlags & AC3ACM_DOLBYSURROUND)?
			BST_CHECKED: BST_UNCHECKED);

		CheckDlgButton(hwndDlg, IDC_USEMMX,
			(md->dwFlags & AC3ACM_DONTUSEMMX)?
			BST_UNCHECKED: BST_CHECKED);

		CheckDlgButton(hwndDlg, IDC_MULTICHANNEL,
			(md->dwFlags & AC3ACM_MULTICHANNEL)?
			BST_CHECKED: BST_UNCHECKED);

		CheckDlgButton(hwndDlg, IDC_EXTENSIBLE,
			(md->dwFlags & AC3ACM_NOEXTENSIBLE)?
			BST_UNCHECKED: BST_CHECKED);

		CheckRadioButton(hwndDlg, IDC_USE64, IDC_USE96,
			(md->dwFlags & AC3ACM_USE64)?
			IDC_USE64: IDC_USE96);

		return TRUE;
	}

	case WM_COMMAND:

	switch (LOWORD(wParam)) {

		case IDOK:
		{
			MyData *md = (MyData *)GetWindowLongPtr(hwndDlg, DWLP_USER);

			if (IsDlgButtonChecked(hwndDlg, IDC_DOLBYSURROUND) == BST_CHECKED) {
				md->dwFlags |= AC3ACM_DOLBYSURROUND;
			} else {
				md->dwFlags &= (~AC3ACM_DOLBYSURROUND);
			}

			if (IsDlgButtonChecked(hwndDlg, IDC_USEMMX) == BST_CHECKED) {
				md->dwFlags &= (~AC3ACM_DONTUSEMMX);
			} else {
				md->dwFlags |= AC3ACM_DONTUSEMMX;
			}

			if (IsDlgButtonChecked(hwndDlg, IDC_MULTICHANNEL) == BST_CHECKED) {
				md->dwFlags |= AC3ACM_MULTICHANNEL;
			} else {
				md->dwFlags &= (~AC3ACM_MULTICHANNEL);
			}

			if (IsDlgButtonChecked(hwndDlg, IDC_EXTENSIBLE) == BST_CHECKED) {
				md->dwFlags &= (~AC3ACM_NOEXTENSIBLE);
			} else {
				md->dwFlags |= AC3ACM_NOEXTENSIBLE;
			}

			if (IsDlgButtonChecked(hwndDlg, IDC_USE64) == BST_CHECKED) {
				md->dwFlags |= AC3ACM_USE64;
			} else {
				md->dwFlags &= (~AC3ACM_USE64);
			}

			WriteReg(md);

			EndDialog(hwndDlg, DRVCNF_OK);
			return TRUE;
		}

		case IDCANCEL:
			EndDialog(hwndDlg, DRVCNF_CANCEL);
			return TRUE;
	}
	break;

	}

	return FALSE;
}


static LRESULT drv_configure(DWORD_PTR dwDriverId, LPARAM lParam1, LPARAM lParam2)
{
	// Show configuration dialog

#ifdef AC3ACM_LOGFILE
	FILE *fh = ((MyData *)dwDriverId)->fLogFile;
	if (fh != NULL)
	{
		fprintf(fh, "DRV_CONFIGURE\n");
		fflush(fh);
	}
#endif

	if (lParam1 == -1) return (LRESULT)TRUE;

	return (LRESULT)DialogBoxParamA(
		((MyData *)dwDriverId)->hmod,
		MAKEINTRESOURCEA(IDD_CONFIG),
		(HWND)lParam1,
		MyDialogProc,
		(LPARAM)dwDriverId
	);
}


LRESULT CALLBACK DriverProc(DWORD_PTR dwDriverId, HDRVR hdrvr, UINT msg, LPARAM lParam1, LPARAM lParam2)
{
    switch (msg) {

    case DRV_LOAD:
        return TRUE;

    case DRV_OPEN:
        return drv_open(hdrvr, lParam2);

    case DRV_CLOSE:
        return drv_close(dwDriverId);

    case DRV_FREE:
        return TRUE;

    case DRV_CONFIGURE:
        return drv_configure(dwDriverId, lParam1, lParam2);

    case DRV_QUERYCONFIGURE:
        return TRUE;

    case DRV_INSTALL:
        return DRVCNF_RESTART;

    case DRV_REMOVE:
        return DRVCNF_RESTART;

    case ACMDM_DRIVER_DETAILS:
		return driver_details(dwDriverId, lParam1);

    case ACMDM_DRIVER_ABOUT:
		return MMSYSERR_NOTSUPPORTED;

    case ACMDM_FORMATTAG_DETAILS:
		return formattag_details(dwDriverId, lParam1, lParam2);

    case ACMDM_FORMAT_DETAILS:
		return format_details(dwDriverId, lParam1, lParam2);

    case ACMDM_FORMAT_SUGGEST:
		return format_suggest(dwDriverId, lParam1);

    case ACMDM_STREAM_OPEN:
		return stream_open(dwDriverId, lParam1);

    case ACMDM_STREAM_CLOSE:
		return stream_close(dwDriverId, lParam1);

    case ACMDM_STREAM_SIZE:
		return stream_size(dwDriverId, lParam1, lParam2);

    case ACMDM_STREAM_CONVERT:
		return stream_convert(dwDriverId, lParam1, lParam2);

    default:
        if (msg < DRV_USER)
			return DefDriverProc(dwDriverId, hdrvr, msg, lParam1, lParam2);
    }

	return MMSYSERR_NOTSUPPORTED;
}
