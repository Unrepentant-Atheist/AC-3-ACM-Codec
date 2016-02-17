
; AC3ASM.asm
; Copyright (C) 2005-2011 fccHandler <fcchandler@comcast.net>
; Copyright (C) 2000-2003 Michel Lespinasse <walken@zoy.org>
; Copyright (C) 1999-2000 Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
; Copyright (C) 2000-2002 Fabrice Bellard
;
; AC3ACM is an open source ATSC A-52 codec for Windows Audio
; Compression Manager. The AC-3 decoder was taken from liba52.
; The AC-3 encoder was adapted from ffmpeg. The ACM interface
; was written by me (fccHandler).
;
; See http://fcchandler.home.comcast.net/ for AC3ACM updates.
; See http://liba52.sourceforge.net/ for liba52 updates.
; See http://ffmpeg.sourceforge.net/ for ac3enc updates.
;
; AC3ACM is free software; you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation; either version 2 of the License, or
; (at your option) any later version.
;
; AC3ACM is distributed in the hope that it will be useful,
; but WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
; GNU General Public License for more details.
;
; You should have received a copy of the GNU General Public License
; along with this program; if not, write to the Free Software
; Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA


; Assembling for "win64"?
%ifidni __YASM_OBJFMT__,win64
	%define _WIN64 1
%endif

	segment .rdata, align=16


; Map table of supported conversions.
; Address this in C as a [2][6][6] array:
;	First index is 1 if MMX is OK, else 0
;	Second index is source (nChannels - 1)
;	Third index is destination (nChannels - 1)
; NULL (0) means the conversion isn't supported

%ifdef _WIN64
	%define pvoid dq
	global MapTab
	align 16
	MapTab:
%else
	%define pvoid dd
	global _MapTab
	align 16
	_MapTab:
%endif

	; Non-MMX conversions
		; src is 1 channel
			pvoid	asm_convert_1_to_1
			pvoid	asm_convert_2_to_2		; liba52 upmixes
			pvoid	0, 0, 0, 0
		; src is 2 channels
			pvoid	asm_convert_1_to_1		; liba52 downmixes
			pvoid	asm_convert_2_to_2
			pvoid	0, 0, 0, 0
		; src is 3 channels
			pvoid	asm_convert_1_to_1
			pvoid	asm_convert_2_to_2
			pvoid	asm_convert_3_to_3
			pvoid	0, 0, 0
		; src is 4 channels
			pvoid	asm_convert_1_to_1
			pvoid	asm_convert_2_to_2
			pvoid	0, asm_convert_4_to_4
			pvoid	0, 0
		; src is 5 channels
			pvoid	asm_convert_1_to_1
			pvoid	asm_convert_2_to_2
			pvoid	0, 0, asm_convert_5_to_5
			pvoid	0
		; src is 6 channels
			pvoid	asm_convert_1_to_1
			pvoid	asm_convert_2_to_2
			pvoid	0, 0, 0, asm_convert_6_to_6

	; MMX conversions
		; src is 1 channel
			pvoid	mmx_convert_1_to_1
			pvoid	mmx_convert_2_to_2
			pvoid	0, 0, 0, 0
		; src is 2 channels
			pvoid	mmx_convert_1_to_1
			pvoid	mmx_convert_2_to_2
			pvoid	0, 0, 0, 0
		; src is 3 channels
			pvoid	mmx_convert_1_to_1
			pvoid	mmx_convert_2_to_2
			pvoid	mmx_convert_3_to_3
			pvoid	0, 0, 0
		; src is 4 channels
			pvoid	mmx_convert_1_to_1
			pvoid	mmx_convert_2_to_2
			pvoid	0, mmx_convert_4_to_4
			pvoid	0, 0
		; src is 5 channels
			pvoid	mmx_convert_1_to_1
			pvoid	mmx_convert_2_to_2
			pvoid	0, 0, mmx_convert_5_to_5
			pvoid	0
		; src is 6 channels
			pvoid	mmx_convert_1_to_1
			pvoid	mmx_convert_2_to_2
			pvoid	0, 0, 0, mmx_convert_6_to_6

mmxconst	dq	43C0000043C00000h


; These must be the same as in a52.h
	A52_CHANNEL			EQU 0
	A52_MONO			EQU 1
	A52_STEREO			EQU 2
	A52_3F				EQU 3
	A52_2F1R			EQU 4
	A52_3F1R			EQU 5
	A52_2F2R			EQU 6
	A52_3F2R			EQU 7
	A52_CHANNEL1		EQU 8
	A52_CHANNEL2		EQU 9
	A52_DOLBY			EQU 10
	A52_CHANNEL_MASK	EQU 15
	A52_LFE				EQU 16


; This is how liba52 creates its "flags" value:
;	static uint8_t lfeon[8] = {16, 16, 4, 4, 4, 1, 4, 1};
;	int acmod = buf[6] >> 5;
;	*flags = (
;		(((buf[6] & 0xf8) == 0x50)? A52_DOLBY: acmod)
;		|
;		((buf[6] & lfeon[acmod])? A52_LFE: 0)
;	);
;
; From this, only certain combinations are possible:
;	acmod ranges from 0 to 7
;	if ((buf[6] & 0xF8) == 0x50) {	; 0101 0...
;		acmod must be 2, therefore lfeon[acmod] is 4
;		the result is A52_DOLBY, or (A52_DOLBY | A52_LFE)
;	} else {
;	the result is acmod, or (acmod | A52_LFE)
;	}
;
; This is from the Dolby spec:
;	if ((acmod & 1) && (acmod != 1)) /* if 3 front channels */ {cmixlev} 2
;	if (acmod & 4)	/* if a surround channel exists */ {surmixlev} 2
;	if (acmod == 2) /* if in 2/0 mode */ {dsurmod} 2
;
; Ultimately these are 9 mutually exclusive possibilities:
;	000l ....	1+1		A52_CHANNEL
;	001l ....	1/0		A52_MONO
;	0100 xl..	2/0		A52_STEREO
;	0101 xl..	2/0		A52_STEREO | A52_DOLBY
;	100s sl..	2/1		A52_2F1R
;	101c cssl	3/1		A52_3F1R
;	110s sl..	2/2		A52_2F2R
;	111c cssl	3/2		A52_3F2R
; Any of these may be combined with the A52_LFE flag.
; A surround channel exists only if (flags & 4)
;
; Note that A52_DOLBY includes the A52_STEREO flag.


%macro clip1 1
	; Valid range is 43BF8000h to 43C07FFFh
	; (this clipper definitely needs improvement!)

	cmp		%1, 43C07FFFh
	jbe		%%else
		mov		%1, 00007FFFh
		jmp		%%endif
	%%else
	cmp		%1, 43BF8000h
	jae		%%endif
		mov		%1, 00008000h
	%%endif

%endmacro

%macro clip2 2
	; clip two registers back-to-back
	clip1	%1
	clip1	%2
%endmacro


	segment .text

%ifdef _WIN64
	global IsMMX
	align 16
	IsMMX:
		mov		rax, 1
		ret
%else
	global _IsMMX
	align 16
	_IsMMX:
		pushfd
		mov		eax, 200000h
		xor		eax, [esp]			; toggle ID flag
		push	eax
		popfd
		pushfd
		pop		eax
		xor		eax, [esp]
		popfd
		and		eax, 200000h
		jz		.Done
			push	ebx
			push	ecx
			push	edx
			xor		eax, eax
			cpuid
			test	eax, eax
			jz		.Zero			; EAX must be at least 1!
				mov		eax, 1
				cpuid
				xor		eax, eax
				shr		edx, 24		; CF = IA MMX bit
				adc		eax, eax
			.Zero
			pop		edx
			pop		ecx
			pop		ebx
		.Done
		ret
%endif

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Destination and Source are 1 channel

%macro CONVERT1 0

%ifdef _WIN64
	%define %%src rcx
	%define %%dst rdx
	%define %%rg1 r8d
	%define %%rg2 r9d
	%define %%cnt al
%else
	%define %%src esi
	%define %%dst edi
	%define %%rg1 eax
	%define %%rg2 edx
	%define %%cnt cl
%endif

	xor		%%cnt, %%cnt
	%%REPEAT
		mov		%%rg1, [%%src]		; C0
		mov		%%rg2, [%%src+4]	; C1
		clip2	%%rg1, %%rg2
		shl		%%rg1, 16
		add		%%src, 8
		shld	%%rg2, %%rg1, 16	; [C1][C0]
		mov		[%%dst], %%rg2
		add		%%dst, 4
		sub		%%cnt, 2			; did 2 samples
	JNZ %%REPEAT

%endmacro

align 16
asm_convert_1_to_1:

%ifndef _WIN64
	push	esi
	push	edi
	mov		esi, [esp+12]	; src
	mov		edi, [esp+16]	; dst
%endif
	CONVERT1
%ifndef _WIN64
	pop		edi
	pop		esi
%endif
	ret


%macro MMXCONVERT1 0

%ifdef _WIN64
	%define %%src rcx
	%define %%dst rdx
%else
	%define %%src ecx
	%define %%dst edx
%endif

	xor		al, al
	%%REPEAT
		movq		mm0, [%%src]	; [  C1  ][  C0  ]

		movq		mm1, [%%src+8]	; [  C3  ][  C2  ]
		psubd		mm0, mm7

		movq		mm2, [%%src+16]	; [  C5  ][  C4  ]
		psubd		mm1, mm7

		movq		mm3, [%%src+24]	; [  C7  ][  C6  ]
		packssdw	mm0, mm1		; [C3][C2][C1][C0]

		psubd		mm2, mm7
		psubd		mm3, mm7

		movq		[%%dst], mm0
		packssdw	mm2, mm3		; [C7][C6][C5][C4]

		add			%%src, 32
		sub			al, 8			; did 8 samples

		movq		[%%dst+8], mm2

		lea			%%dst, [%%dst+16]
	JNZ %%REPEAT

%endmacro

align 16
mmx_convert_1_to_1:
	; convert floating mono to 16-bit mono

%ifdef _WIN64
	movq	mm7, [mmxconst wrt rip]
%else
	mov		ecx, [esp+4]
	mov		edx, [esp+8]
	movq	mm7, [mmxconst]
%endif
	MMXCONVERT1
	emms
	ret


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Source and Destination are 2 channels
; Source is one of the following:
;	A52_MONO + A52_LFE			return FC,LF
;	A52_STEREO or A52_DOLBY		return FL,FR

%macro CONVERT2 0

%ifdef _WIN64
	%define %%src rcx
	%define %%dst rdx
	%define %%rg1 r8d
	%define %%rg2 r9d
	%define %%rg3 r10
	%define %%cnt al
%else
	%define %%src esi
	%define %%dst edi
	%define %%rg1 eax
	%define %%rg2 edx
	%define %%rg3 ebx
	%define %%cnt cl
%endif

	xor		%%cnt, %%cnt
	%%REPEAT
		mov		%%rg1, [%%src]	; L
		mov		%%rg2, [%%rg3]	; R
		clip2	%%rg1, %%rg2
		shl		%%rg1, 16
		add		%%src, 4
		shld	%%rg2, %%rg1, 16
		add		%%rg3, 4
		mov		[%%dst], %%rg2
		add		%%dst, 4
		dec		%%cnt	; did 1 sample
	JNZ %%REPEAT

%endmacro

align 16
asm_convert_2_to_2:

%ifdef _WIN64
	lea		r10, [rcx+1024]
	test	r8d, A52_LFE
	jz		.endif
	xchg	rcx, r10
%else
	push	esi
	push	edi
	mov		esi, [esp+12]	; src
	push	ebx
	mov		edi, [esp+20]	; dst
	lea		ebx, [esi+1024]
	test	dword [esp+24], A52_LFE
	jz		.endif
	xchg	esi, ebx
%endif
	.endif

	CONVERT2

%ifndef _WIN64
	pop		ebx
	pop		edi
	pop		esi
%endif
	ret


%macro MMXCONVERT2 0

%ifdef _WIN64
	%define %%src rcx
	%define %%rg1 r9
	%define %%dst rdx
%else
	%define %%src ecx
	%define %%rg1 ebx
	%define %%dst edx
%endif

	xor		al, al
	%%REPEAT
		movq		mm0, [%%src]	; [  L1  ][  L0  ]

		movq		mm1, [%%src+8]	; [  L3  ][  L2  ]
		psubd		mm0, mm7

		movq		mm2, [%%rg1]	; [  R1  ][  R0  ]
		psubd		mm1, mm7

		movq		mm4, mm0		; [  L1  ][  L0  ]
		psubd		mm2, mm7

		movq		mm3, [%%rg1+8]	; [  R3  ][  R2  ]
		punpckldq	mm0, mm2		; [  R0  ][  L0  ]

		punpckhdq	mm4, mm2		; [  R1  ][  L1  ]
		psubd		mm3, mm7

		movq		mm2, mm1		; [  L3  ][  L2  ]
		packssdw	mm0, mm4		; [R1][L1][R0][L0]

		punpckldq	mm1, mm3		; [  R2  ][  L2  ]
		add			%%src, 16

		movq		[%%dst], mm0
		punpckhdq	mm2, mm3		; [  R3  ][  L3  ]

		packssdw	mm1, mm2		; [R3][L3][R2][L2]
		add			%%rg1, 16

		movq		[%%dst+8], mm1
		sub			al, 4			; did 4 samples

		lea			%%dst, [%%dst+16]
	JNZ %%REPEAT

%endmacro

align 16
mmx_convert_2_to_2:

%ifdef _WIN64
	movq	mm7, [mmxconst wrt rip]
	lea		r9, [rcx+1024]
	test	r8d, A52_LFE
	jz		.endif
; liba52 would return LF,FC, so swap channels
	xchg	rcx, r9
%else
	push	ebx
	mov		ecx, [esp+8]	; src
	mov		edx, [esp+12]	; dst
	lea		ebx, [ecx+1024]
	movq	mm7, [mmxconst]
	test	dword [esp+16], A52_LFE
	jz		.endif
; liba52 would return LF,FC, so swap channels
	xchg	ecx, ebx
%endif
	.endif

	MMXCONVERT2

%ifndef _WIN64
	pop		ebx
%endif
	emms
	ret


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Source and Destination are 3 channels
; Source is one of the following:
;	A52_STEREO + A52_LFE	LF,FL,FR: return 1,2,0
;	A52_3F					FL,FC,FR: return 0,2,1
;	A52_2F1R				FL,FR,BC: return 0,1,2

%macro CONVERT3	3

%define %%CA %1
%define %%CB %2
%define %%CC %3

%ifdef _WIN64
	%define %%src rcx
	%define %%dst rdx
	%define %%rg1 r8d
	%define %%rg2 r9d
	%define %%rg3 r10d
	%define %%cnt al
%else
	%define %%src esi
	%define %%dst edi
	%define %%rg1 eax
	%define %%rg2 edx
	%define %%rg3 ebx
	%define %%cnt cl
%endif

	xor		%%cnt, %%cnt
	%%REPEAT	; [CC1 CB1] [CA1 CC0] [CB0 CA0]
		mov		%%rg1, [%%src+%%CA+0]
		mov		%%rg2, [%%src+%%CB+0]
		clip2	%%rg1, %%rg2
		shl		%%rg1, 16			; [CA0][...]
		mov		%%rg3, [%%src+%%CC+0]
		shld	%%rg2, %%rg1, 16	; [CB0][CA0]
		mov		%%rg1, [%%src+%%CA+4]
		mov		[%%dst], %%rg2
		add		%%dst, 4
		clip2	%%rg3, %%rg1
		shl		%%rg3, 16			; [CC0][...]
		mov		%%rg2, [%%src+%%CB+4]
		shld	%%rg1, %%rg3, 16	; [CA1][CC0]
		mov		%%rg3, [%%src+%%CC+4]
		mov		[%%dst], %%rg1
		add		%%dst, 4
		clip2	%%rg2, %%rg3
		shl		%%rg2, 16			; [CB1][...]
		add		%%src, 8
		shld	%%rg3, %%rg2, 16	; [CC1][CB1]
		mov		[%%dst], %%rg3
		add		%%dst, 4
		sub		%%cnt, 2	; did 2 samples
	JNZ %%REPEAT

%endmacro

align 16
asm_convert_3_to_3:

%ifdef _WIN64
	mov		eax, r8d
%else
	push	esi
	push	edi
	mov		esi, [esp+12]	; src
	push	ebx
	mov		edi, [esp+20]	; dst
	mov		eax, [esp+24]	; flags
%endif
	test	eax, A52_LFE
	jz		.else1
		CONVERT3	1024, 2048, 0
		jmp		.endif
	.else1
	cmp		eax, A52_3F
	jne		.else2
		CONVERT3	0, 2048, 1024
		jmp		.endif
	.else2
		; A52_2F1R
		CONVERT3	0, 1024, 2048
	.endif

%ifndef _WIN64
	pop		ebx
	pop		edi
	pop		esi
%endif
	ret


%macro MMXCONVERT3 3

%define %%CA %1
%define %%CB %2
%define %%CC %3

%ifdef _WIN64
	%define %%src rcx
	%define %%dst rdx
%else
	%define %%src ecx
	%define %%dst edx
%endif

	xor		al, al
	%%REPEAT
		movq		mm0, [%%src+%%CA]	; [ CA1 ][ CA0 ]
		movq		mm1, [%%src+%%CB]	; [ CB1 ][ CB0 ]
		psubd		mm0, mm7
		movq		mm2, [%%src+%%CC]	; [ CC1 ][ CC0 ]
		psubd		mm1, mm7
		psubd		mm2, mm7
	; we want     [CA1][CC0][CB0][CA0]
		movq		mm3, mm2       		; [ CC1 ][ CC0 ]
		psllq		mm2, 32				; [ CC0 ][ ... ]
		punpckhdq	mm2, mm0			; [ CA1 ][ CC0 ]
		punpckldq	mm0, mm1			; [ CB0 ][ CA0 ]
		packssdw	mm0, mm2			; [CA1][CC0][CB0][CA0]
		movq		mm2, [%%src+%%CA+8]	; [ CA3 ][ CA2 ]
		punpckhdq	mm1, mm3			; [ CC1 ][ CB1 ]
	; now we want [CB2][CA2][CC1][CB1]
		movq		mm3, [%%src+%%CB+8]	; [ CB3 ][ CB2 ]
		psubd		mm2, mm7
		movq		mm4, [%%src+%%CC+8]	; [ CC3 ][ CC2 ]
		psubd		mm3, mm7
		movq		mm5, mm2			; [ CA3 ][ CA2 ]
		psubd		mm4, mm7
		punpckldq	mm2, mm3			; [ CB2 ][ CA2 ]
	; last we want [CC3][CB3][CA3][CC2]
		movq		[%%dst], mm0
		packssdw	mm1, mm2			; [CB2][CA2][CC1][CB1]
		punpckhdq	mm3, mm4			; [ CC3 ][ CB3 ]
		psllq		mm4, 32				; [ CC2 ][ ... ]
		punpckhdq	mm4, mm5			; [ CA3 ][ CC2 ]
		movq		[%%dst+8], mm1
		packssdw	mm4, mm3			; [CC3][CB3][CA3][CC2]
		add			%%src, 16
		sub			al, 4	; did 4 samples
		movq		[%%dst+16], mm4
		lea			%%dst, [%%dst+24]
	JNZ %%REPEAT

%endmacro

align 16
mmx_convert_3_to_3:

%ifdef _WIN64
	movq	mm7, [mmxconst wrt rip]
	mov		eax, r8d		; flags
%else
	mov		ecx, [esp+4]	; src
	mov		edx, [esp+8]	; dst
	movq	mm7, [mmxconst]
	mov		eax, [esp+12]	; flags
%endif

	test	eax, A52_LFE
	jz		.else1
		MMXCONVERT3	1024, 2048, 0
		jmp		.endif
	.else1
	cmp		eax, A52_3F
	jne		.else2
		MMXCONVERT3	0, 2048, 1024
		jmp		.endif
	.else2
		; A52_2F1R
		MMXCONVERT3	0, 1024, 2048
	.endif

	emms
	ret


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Source and Destination are 4 channels
; Source is one of the following:
;	A52_2F1R + A52_LFE		LF,FL,FR,BC: return 1,2,0,3
;	A52_3F + A52_LFE		LF,FL,FC,FR: return 1,3,2,0
;	A52_2F2R				FL,FR,BL,BR: return 0,1,2,3
;	A52_3F1R				FL,FC,FR,BC: return 0,2,1,3

%macro CONVERT4 4

%define %%CA %1
%define %%CB %2
%define %%CC %3
%define %%CD %4

%ifdef _WIN64
	%define %%src rcx
	%define %%dst rdx
	%define %%rg1 r8d
	%define %%rg2 r9d
	%define %%rg3 r10d
	%define %%cnt al
%else
	%define %%src esi
	%define %%dst edi
	%define %%rg1 eax
	%define %%rg2 edx
	%define %%rg3 ebx
	%define %%cnt cl
%endif

	xor		%%cnt, %%cnt
	%%REPEAT
		mov		%%rg1, [%%src+%%CA]	; [  CA  ]
		mov		%%rg2, [%%src+%%CB]	; [  CB  ]
		clip2	%%rg1, %%rg2
		shl		%%rg1, 16			; [CA][..]
		mov		%%rg3, [%%src+%%CC]	; [  CC  ]
		shld	%%rg2, %%rg1, 16	; [CB][CA]
		mov		%%rg1, [%%src+%%CD]	; [  CD  ]
		mov		[%%dst], %%rg2
		add		%%dst, 4
		clip2	%%rg3, %%rg1
		shl		%%rg3, 16			; [CC][..]
		add		%%src, 4
		shld	%%rg1, %%rg3, 16	; [CD][CC]
		mov		[%%dst], %%rg1
		add		%%dst, 4
		dec		%%cnt	; did 1 sample
	JNZ %%REPEAT

%endmacro


align 16
asm_convert_4_to_4:
	; convert 4-channel floating to 16-bit

%ifdef _WIN64
	mov		eax, r8d		; flags
%else
	push	esi
	push	edi
	mov		esi, [esp+12]	; src
	push	ebx
	mov		edi, [esp+20]	; dst
	mov		eax, [esp+24]	; flags
%endif
	cmp		eax, A52_2F1R | A52_LFE
	jne		.else1
		CONVERT4	1024, 2048, 0, 3072
		jmp		.endif
	.else1
	cmp		eax, A52_3F | A52_LFE
	jne		.else2
		CONVERT4	1024, 3072, 2048, 0
		jmp		.endif
	.else2
	cmp		eax, A52_2F2R
	jne		.else3
		CONVERT4	0, 1024, 2048, 3072
		jmp		.endif
	.else3
		; A52_3F1R
		CONVERT4	0, 2048, 1024, 3072
	.endif

%ifndef _WIN64
	pop		ebx
	pop		edi
	pop		esi
%endif
	ret


%macro MMXCONVERT4 4

%define %%CA %1
%define %%CB %2
%define %%CC %3
%define %%CD %4

%ifdef _WIN64
	%define %%src rcx
	%define %%dst rdx
%else
	%define %%src ecx
	%define %%dst edx
%endif

	xor		al, al
	%%REPEAT
		movq		mm0, [%%src+%%CA]	; [ CA1 ][ CA0 ]
		movq		mm1, [%%src+%%CB]	; [ CB1 ][ CB0 ]
		psubd		mm0, mm7
		movq		mm2, [%%src+%%CC]	; [ CC1 ][ CC0 ]
		psubd		mm1, mm7
		movq		mm3, [%%src+%%CD]	; [ CD1 ][ CD0 ]
		psubd		mm2, mm7
		movq		mm4, mm0			; [ CA1 ][ CA0 ]
		psubd		mm3, mm7
	; we want     [CD0][CC0][CB0][CA0]
		punpckldq	mm0, mm1			; [ CB0 ][ CA0 ]
		movq		mm5, mm2			; [ CC1 ][ CC0 ]
		punpckldq	mm2, mm3			; [ CD0 ][ CC0 ]
		packssdw	mm0, mm2			; [CD0][CC0][CB0][CA0]
		add			%%src, 8
	; now we want [CD1][CC1][CB1][CA1]
		punpckhdq	mm4, mm1            ; [ CB1 ][ CA1 ]
		punpckhdq	mm5, mm3			; [ CD1 ][ CC1 ]
		packssdw	mm4, mm5			; [CD1][CC1][CB1][CA1]
		sub			al, 2		; did 2 samples
		movq		[%%dst], mm0
		movq		[%%dst+8], mm4
		lea			%%dst, [%%dst+16]
	JNZ %%REPEAT

%endmacro

align 16
mmx_convert_4_to_4:
	; convert 4-channel floating to 16-bit

%ifdef _WIN64
	movq	mm7, [mmxconst wrt rip]
	mov		eax, r8d		; flags
%else
	mov		ecx, [esp+4]	; src
	mov		edx, [esp+8]	; dst
	movq	mm7, [mmxconst]
	mov		eax, [esp+12]	; flags
%endif
	cmp		eax, A52_2F1R | A52_LFE
	jne		.else1
		MMXCONVERT4	1024, 2048, 0, 3072
		jmp		.endif
	.else1
	cmp		eax, A52_3F | A52_LFE
	jne		.else2
		MMXCONVERT4	1024, 3072, 2048, 0
		jmp		.endif
	.else2
	cmp		eax, A52_2F2R
	jne		.else3
		MMXCONVERT4	0, 1024, 2048, 3072
		jmp		.endif
	.else3
		; A52_3F1R
		MMXCONVERT4	0, 2048, 1024, 3072
	.endif

	emms
	ret


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Source and Destination are 5 channels
; Source is one of the following:
;	A52_2F2R + A52_LFE		LF,FL,FR,BL,BR: return 1,2,0,3,4
;	A52_3F1R + A52_LFE		LF,FL,FC,FR,BC: return 1,3,2,0,4
;	A52_3F2R				FL,FC,FR,BL,BR: return 0,2,1,3,4

%macro CONVERT5 5

%define %%CA %1
%define %%CB %2
%define %%CC %3
%define %%CD %4
%define %%CE %5

%ifdef _WIN64
	%define %%src rcx
	%define %%dst rdx
	%define %%rg1 r8d
	%define %%rg2 r9d
	%define %%rg3 r10d
	%define %%cnt al
%else
	%define %%src esi
	%define %%dst edi
	%define %%rg1 eax
	%define %%rg2 edx
	%define %%rg3 ebx
	%define %%cnt cl
%endif

	xor		%%cnt, %%cnt
	%%REPEAT
		mov		%%rg1, [%%src+%%CA+0]	; [   CA0  ]
		mov		%%rg2, [%%src+%%CB+0]	; [   CB0  ]
		clip2	%%rg1, %%rg2
		shl		%%rg1, 16				; [CA0][...]
		mov		%%rg3, [%%src+%%CC+0]	; [   CC0  ]
		shld	%%rg2, %%rg1, 16		; [CB0][CA0]
		mov		%%rg1, [%%src+%%CD+0]	; [   CD0  ]
		mov		[%%dst], %%rg2
		add		%%dst, 4
		clip2	%%rg3, %%rg1
		shl		%%rg3, 16				; [CC0][...]
		mov		%%rg2, [%%src+%%CE+0]	; [   CE0  ]
		shld	%%rg1, %%rg3, 16		; [CD0][CC0]
		mov		%%rg3, [%%src+%%CA+4]	; [   CA1  ]
		mov		[%%dst], %%rg1
		add		%%dst, 4
		clip2	%%rg2, %%rg3
		shl		%%rg2, 16				; [CE0][...]
		mov		%%rg1, [%%src+%%CB+4]	; [   CB1  ]
		shld	%%rg3, %%rg2, 16		; [CA1][CE0]
		mov		%%rg2, [%%src+%%CC+4]	; [   CC1  ]
		mov		[%%dst], %%rg3
		add		%%dst, 4
		clip2	%%rg1, %%rg2
		shl		%%rg1, 16				; [CB1][...]
		mov		%%rg3, [%%src+%%CD+4]	; [   CD1  ]
		shld	%%rg2, %%rg1, 16		; [CC1][CB1]
		mov		%%rg1, [%%src+%%CE+4]	; [   CE1  ]
		mov		[%%dst], %%rg2
		add		%%dst, 4
		clip2	%%rg3, %%rg1
		shl		%%rg3, 16				; [CD1][...]
		add		%%src, 8
		shld	%%rg1, %%rg3, 16		; [CE1][CD1]
		mov		[%%dst], %%rg1
		add		%%dst, 4
		sub		%%cnt, 2	; did 2 samples
	JNZ %%REPEAT

%endmacro

align 16
asm_convert_5_to_5:
	; convert 5-channel floating to 16-bit

%ifdef _WIN64
	mov		eax, r8d		; flags
%else
	push	esi
	push	edi
	mov		esi, [esp+12]	; src
	push	ebx
	mov		edi, [esp+20]	; dst
	mov		eax, [esp+24]	; flags
%endif
	cmp		eax, A52_2F2R | A52_LFE
	jne		.else1
		CONVERT5	1024, 2048, 0, 3072, 4096
		jmp		.endif
	.else1
	cmp		eax, A52_3F1R | A52_LFE
	jne		.else2
		CONVERT5	1024, 3072, 2048, 0, 4096
		jmp		.endif
	.else2
		; A52_3F2R
		CONVERT5	0, 2048, 1024, 3072, 4096
	.endif

%ifndef _WIN64
	pop		ebx
	pop		edi
	pop		esi
%endif
	ret


%macro MMXCONVERT5 5

%define %%CA %1
%define %%CB %2
%define %%CC %3
%define %%CD %4
%define %%CE %5

%ifdef _WIN64
	%define %%src rcx
	%define %%dst rdx
%else
	%define %%src ecx
	%define %%dst edx
%endif

	xor		al, al
	%%REPEAT
		movq		mm0, [%%src+%%CA+0]	; [ CA1 ][ CA0 ]
		movq		mm1, [%%src+%%CB+0]	; [ CB1 ][ CB0 ]
		psubd		mm0, mm7
		movq		mm2, [%%src+%%CC+0]	; [ CC1 ][ CC0 ]
		psubd		mm1, mm7
		movq		mm3, [%%src+%%CD+0]	; [ CD1 ][ CD0 ]
		psubd		mm2, mm7
		movq		mm4, [%%src+%%CE+0]	; [ CE1 ][ CE0 ]
		psubd		mm3, mm7

	; we want     [CD0][CC0][CB0][CA0]
		movq		mm5, mm0			; [ CA1 ][ CA0 ]
		psubd		mm4, mm7
		movq		mm6, mm2			; [ CC1 ][ CC0 ]
		psrlq		mm0, 32				; [ ... ][ CA1 ]
		punpckldq	mm5, mm1			; [ CB0 ][ CA0 ]
		punpckldq	mm6, mm3			; [ CD0 ][ CC0 ]
		packssdw	mm5, mm6			; [CD0][CC0][CB0][CA0]
		movq		[%%dst], mm5

	; now we want [CC1][CB1][CA1][CE0]
		movq		mm5, mm4			; [ CE1 ][ CE0 ]
		movq		mm6, mm1			; [ CB1 ][ CB0 ]
		punpckldq	mm5, mm0			; [ CA1 ][ CE0 ]
		punpckhdq	mm6, mm2			; [ CC1 ][ CB1 ]
		packssdw	mm5, mm6			; [CC1][CB1][CA1][CE0]
		movq		[%%dst+8], mm5

	; now we want [CB2][CA2][CE1][CD1]
		movq		mm0, [%%src+%%CA+8]	; [ CA3 ][ CA2 ]
		movq		mm5, mm3			; [ CD1 ][ CD0 ]
		movq		mm1, [%%src+%%CB+8]	; [ CB3 ][ CB2 ]
		psubd		mm0, mm7
		movq		mm2, [%%src+%%CC+8]	; [ CC3 ][ CC2 ]
		psubd		mm1, mm7
		movq		mm3, [%%src+%%CD+8]	; [ CD3 ][ CD2 ]
		movq		mm6, mm0			; [ CA3 ][ CA2 ]
		punpckhdq	mm5, mm4			; [ CE1 ][ CD1 ]
		punpckldq	mm6, mm1			; [ CB2 ][ CA2 ]
		movq		mm4, [%%src+%%CE+8]	; [ CE3 ][ CE2 ]
		psubd		mm2, mm7
		packssdw	mm5, mm6			; [CB2][CA2][CE1][CD1]
		psubd		mm3, mm7
		movq		[%%dst+16], mm5
		psubd		mm4, mm7

	; now we want [CA3][CE2][CD2][CC2]
		movq		mm5, mm2			; [ CC3 ][ CC2 ]
		psrlq		mm0, 32				; [ ... ][ CA3 ]
		movq		mm6, mm4			; [ CE3 ][ CE2 ]
		punpckldq	mm5, mm3			; [ CD2 ][ CC2 ]
		punpckldq	mm6, mm0			; [ CA3 ][ CE2 ]
		packssdw	mm5, mm6			; [CA3][CE2][CD2][CC2]
		movq		[%%dst+24], mm5

	; last we want [CE3][CD3][CC3][CB3]
		movq		mm5, mm1			; [ CB3 ][ CB2 ]
		movq		mm6, mm3			; [ CD3 ][ CD2 ]
		punpckhdq	mm5, mm2			; [ CC3 ][ CB3 ]
		punpckhdq	mm6, mm4			; [ CE3 ][ CD3 ]
		packssdw	mm5, mm6			; [CE3][CD3][CC3][CB3]
		movq		[%%dst+32], mm5

		add			%%src, 16
		add			%%dst, 40
		sub			al, 4		; did 4 samples
	JNZ %%REPEAT

%endmacro

align 16
mmx_convert_5_to_5:
	; convert 5-channel floating to 16-bit

%ifdef _WIN64
	movq	mm7, [mmxconst wrt rip]
	mov		eax, r8d		; flags
%else
	mov		ecx, [esp+4]	; src
	mov		edx, [esp+8]	; dst
	movq	mm7, [mmxconst]
	mov		eax, [esp+12]	; flags
%endif
	cmp		eax, A52_2F2R | A52_LFE
	jne		.else1
		MMXCONVERT5	1024, 2048, 0, 3072, 4096
		jmp		.endif
	.else1
	cmp		eax, A52_3F1R | A52_LFE
	jne		.else2
		MMXCONVERT5	1024, 3072, 2048, 0, 4096
		jmp		.endif
	.else2
		; A52_3F2R
		MMXCONVERT5	0, 2048, 1024, 3072, 4096
	.endif

	emms
	ret


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Source and Destination are 6 channels
; Source is A52_3F2R + A52_LFE
; LF,FL,FC,FR,BL,BR: return 1,3,2,0,4,5

%macro CONVERT6 0

%define %%FL (1024*1)
%define %%FR (1024*3)
%define %%FC (1024*2)
%define %%LF (1024*0)
%define %%BL (1024*4)
%define %%BR (1024*5)

%ifdef _WIN64
	%define %%src rcx
	%define %%dst rdx
	%define %%rg1 r8d
	%define %%rg2 r9d
	%define %%rg3 r10d
	%define %%cnt al
%else
	%define %%src esi
	%define %%dst edi
	%define %%rg1 eax
	%define %%rg2 edx
	%define %%rg3 ebx
	%define %%cnt cl
%endif

	xor		%%cnt, %%cnt
	%%REPEAT
		mov		%%rg1, [%%src+%%FL]	; [  FL  ]
		mov		%%rg2, [%%src+%%FR]	; [  FR	 ]
		clip2	%%rg1, %%rg2
		shl		%%rg1, 16			; [FL][..]
		mov		%%rg3, [%%src+%%FC]	; [  FC	 ]
		shld	%%rg2, %%rg1, 16	; [FR][FL]
		mov		%%rg1, [%%src+%%LF]	; [  LF  ]
		mov		[%%dst], %%rg2
		add		%%dst, 4
		clip2	%%rg3, %%rg1
		shl		%%rg3, 16			; [FC][..]
		mov		%%rg2, [%%src+%%BL]	; [  BL  ]
		shld	%%rg1, %%rg3, 16	; [LF][FC]
		mov		%%rg3, [%%src+%%BR]	; [  BR  ]
		mov		[%%dst], %%rg1
		add		%%dst, 4
		clip2	%%rg2, %%rg3
		shl		%%rg2, 16			; [BL][..]
		add		%%src, 4
		shld	%%rg3, %%rg2, 16	; [BR][BL]
		mov		[%%dst], %%rg3
		add		%%dst, 4
		dec		%%cnt	; did 1 sample
	JNZ %%REPEAT

%endmacro

align 16
asm_convert_6_to_6:
	; convert 6-channel floating to 16-bit

%ifndef _WIN64
	push	esi
	push	edi
	mov		esi, [esp+12]	; src
	push	ebx
	mov		edi, [esp+20]	; dst
%endif
	CONVERT6
%ifndef _WIN64
	pop		ebx
	pop		edi
	pop		esi
%endif
	ret


%macro MMXCONVERT6 0

%define %%FL (1024*1)
%define %%FR (1024*3)
%define %%FC (1024*2)
%define %%LF (1024*0)
%define %%BL (1024*4)
%define %%BR (1024*5)

%ifdef _WIN64
	%define %%src rcx
	%define %%dst rdx
%else
	%define %%src ecx
	%define %%dst edx
%endif

	xor		al, al
	%%REPEAT
		movq		mm0, [%%src+%%FL]	; [ FL1 ][ FL0 ]
		movq		mm1, [%%src+%%FR]	; [ FR1 ][ FR0 ]
		psubd		mm0, mm7
		movq		mm2, [%%src+%%FC]	; [ FC1 ][ FC0 ]
		psubd		mm1, mm7
		movq		mm3, [%%src+%%LF]	; [ LF1 ][ LF0 ]
		psubd		mm2, mm7
		psubd		mm3, mm7
	; we want [FL0][FR0][FC0][LF0]
		movq		mm4, mm0			; [ FL1 ][ FL0 ]
		punpckldq	mm0, mm1			; [ FR0 ][ FL0 ]
		movq		mm5, mm2			; [ FC1 ][ FC0 ]
		punpckldq	mm2, mm3			; [ LF0 ][ FC0 ]
		movq		mm6, [%%src+%%BL]	; [ BL1 ][ BL0 ]
		packssdw	mm0, mm2			; [LF0][FC0][FR0][FL0]
	; we want [FR1][FL1][BR0][BL0]
		movq		mm2, [%%src+%%BR]	; [ BR1 ][ BR0 ]
		psubd		mm6, mm7
		psubd		mm2, mm7
		punpckhdq	mm4, mm1			; [ FR1 ][ FL1 ]
		movq		mm1, mm6			; [ BL1 ][ BL0 ]
		punpckldq	mm6, mm2			; [ BR0 ][ BL0 ]
		movq		[%%dst], mm0
		packssdw	mm6, mm4			; [FR1][FL1][BR0][BL0]
	; we want [BR1][BL1][LF1][FC1]
		punpckhdq	mm1, mm2			; [ BR1 ][ BL1 ]
		punpckhdq	mm5, mm3			; [ LF1 ][ FC1 ]
		add			%%src, 8
		packssdw	mm5, mm1			; [BR1][BL1][LF1][FC1]
		sub			al, 2		; did 2 samples
		movq		[%%dst+8], mm6
		movq		[%%dst+16], mm5
		lea			%%dst, [%%dst+24]
	JNZ %%REPEAT

%endmacro


align 16
mmx_convert_6_to_6:
	; convert 6-channel floating to 16-bit

%ifdef _WIN64
	movq	mm7, [mmxconst wrt rip]
%else
	mov		ecx, [esp+4]
	mov		edx, [esp+8]
	movq	mm7, [mmxconst]
%endif
	MMXCONVERT6
	emms
	ret

