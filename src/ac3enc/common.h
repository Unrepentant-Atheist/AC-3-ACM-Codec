/*
 * Some defines and structs for ac3enc.cpp
 * Copyright (c) 2003-2011 fccHandler
 * Copyright (c) 2000, 2001, 2002 Fabrice Bellard.
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file common.h
 * common internal api header.
 */

#ifndef COMMON_H
#define COMMON_H

#define AC3_MAX_CODED_FRAME_SIZE 3840	// in bytes
#define AC3_MAX_CHANNELS 6				// including LFE channel

#define NB_BLOCKS 6		// number of PCM blocks inside an AC3 frame
#define AC3_FRAME_SIZE (NB_BLOCKS * 256)

// exponent encoding strategy
#define EXP_REUSE	0
#define EXP_NEW		1

#define EXP_D15		1
#define EXP_D25		2
#define EXP_D45		3

#define M_PI	3.14159265358979323846

typedef struct {
	int fscod;	// frequency
	int halfratecod;
	int sgain;
	int sdecay;
	int fdecay;
	int dbknee;
	int floor;
	int cplfleak;
	int cplsleak;
} AC3BitAllocParameters;

typedef struct {
    unsigned int bit_buf;
    int bit_left;
    unsigned char *buf;
	unsigned char *buf_ptr;
	unsigned char *buf_end;
} PutBitContext;

#endif // COMMON_H
