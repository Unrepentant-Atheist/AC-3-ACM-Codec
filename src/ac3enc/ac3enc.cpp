/*
 * The simplest AC3 encoder
 * Copyright (c) 2003-2011 fccHandler
 * Copyright (c) 2000 Fabrice Bellard.
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

/**
 * @file ac3enc.c
 * The simplest AC3 encoder.
 */

#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <crtdbg.h>

#include "common.h"
#include "ac3tab.h"

#define MDCT_NBITS 9
#define MDCT_N (1 << MDCT_NBITS)
// (1 << 9) = 512

typedef struct {
	PutBitContext pb;
	int nb_channels;
	int nb_all_channels;
	int lfe_channel;
	int bit_rate;
	unsigned int sample_rate;
	unsigned int bsid;
	unsigned int frame_size_min;	// minimum frame size in case rounding is necessary
	unsigned int frame_size;		// current frame size in words
	int halfratecod;
	unsigned int frmsizecod;
	unsigned int fscod;				// frequency
	unsigned int acmod;
	int lfe;
	unsigned int bsmod;

	short last_samples[AC3_MAX_CHANNELS][256];
	unsigned int chbwcod[AC3_MAX_CHANNELS];
	int nb_coefs[AC3_MAX_CHANNELS];
    
	// bitrate allocation control
	int sgaincod;
	int sdecaycod;
	int fdecaycod;
	int dbkneecod;
	int floorcod; 

	AC3BitAllocParameters bit_alloc;
	int csnroffst;
	int fgaincod[AC3_MAX_CHANNELS];
	int fsnroffst[AC3_MAX_CHANNELS];

	// mantissa encoding
	int mant1_cnt;
	int mant2_cnt;
	int mant4_cnt;

} AC3EncodeContext;

static AC3EncodeContext ac3enc_state;

// Memory
static short input_samples[MDCT_N];
static long mdct_coef[NB_BLOCKS][AC3_MAX_CHANNELS][MDCT_N/2];
static unsigned char exponent[NB_BLOCKS][AC3_MAX_CHANNELS][MDCT_N/2];
static unsigned char exp_strategy[NB_BLOCKS][AC3_MAX_CHANNELS];
static unsigned char encoded_exp[NB_BLOCKS][AC3_MAX_CHANNELS][MDCT_N/2];
static unsigned char bap[NB_BLOCKS][AC3_MAX_CHANNELS][MDCT_N/2];
static signed char exp_samples[NB_BLOCKS][AC3_MAX_CHANNELS];

// new exponents are sent if their Norm 1 exceed this number
#define EXP_DIFF_THRESHOLD 1000

#if defined(_AMD64_) || defined(_M_AMD64)

#if _MSC_VER >= 1300
	extern unsigned long _byteswap_ulong(unsigned long v);
	#pragma intrinsic(_byteswap_ulong)
	static inline unsigned long bswap(unsigned long v) {
		return _byteswap_ulong(v);
	}
#else
	static inline unsigned long bswap(unsigned long v) {
		return (
			(v << 24) |
			((v << 8) & 0x00FF0000) |
			((v >> 8) & 0x0000FF00) |
			(v >> 24)
		);
	}
#endif

#else

#pragma warning(push)
#pragma warning(disable:4035)
static inline unsigned long bswap(unsigned long x) {
	__asm {
		mov		eax, x
		bswap	eax
	}
}
#pragma warning (pop)

#endif

static void init_put_bits(PutBitContext *s, unsigned char *buffer, int buffer_size)
{
	s->buf = s->buf_ptr = buffer;
	s->buf_end = s->buf + buffer_size;
	s->bit_left = 32;
	s->bit_buf = 0;
}

// pad the end of the output stream with zeros
static void flush_put_bits(PutBitContext *s)
{
	s->bit_buf <<= s->bit_left;

	while (s->bit_left < 32) {
		// XXX: should test end of buffer
		*s->buf_ptr++ = s->bit_buf >> 24;
		s->bit_buf <<= 8;
		s->bit_left += 8;
	}
	s->bit_left = 32;
	s->bit_buf = 0;
}

// Note: "n" is never greater than 16
static inline void put_bits(PutBitContext *s, int n, unsigned int value)
{
	unsigned int bit_buf;
	int bit_left;

	_ASSERT(n == 32 || value < (1U << n));
    
	bit_buf = s->bit_buf;
	bit_left = s->bit_left;

	// XXX: optimize
	if (n < bit_left) {
		bit_buf = (bit_buf << n) | value;
		bit_left -= n;

	} else {
		bit_buf <<= bit_left;
		bit_buf |= value >> (n - bit_left);

		*(unsigned long *)s->buf_ptr = bswap(bit_buf);
		s->buf_ptr += 4;
		bit_left += 32 - n;
		bit_buf = value;
	}

	s->bit_buf = bit_buf;
	s->bit_left = bit_left;
}

static inline unsigned char* pbBufPtr(PutBitContext *s)
{
	return s->buf_ptr;
}

static inline int calc_lowcomp1(int a, int b0, int b1)
{
	if ((b0 + 256) == b1) {
		a = 384;
	} else if (b0 > b1) { 
		a = a - 64;
		if (a < 0) a = 0;
	}
	return a;
}

static inline int calc_lowcomp(int a, int b0, int b1, int bin)
{
	if (bin < 7) {
		if ((b0 + 256) == b1) {
			a = 384;
		} else if (b0 > b1) { 
			a = a - 64;
			if (a < 0) a = 0;
		}
	} else if (bin < 20) {
		if ((b0 + 256) == b1) {
			a = 320;
		} else if (b0 > b1) {
			a = a - 64;
			if (a < 0) a = 0;
		}
	} else {
		a = a - 128;
		if (a < 0) a = 0;
	}
	return a;
}

// AC3 bit allocation. The algorithm
// is the one described in the AC3 spec.

static void ac3_parametric_bit_allocation(
	AC3BitAllocParameters *s, unsigned char *bap,
	signed char *exp, int start, int end,
	int snroffset, int fgain, int is_lfe,
	int deltbae, int deltnseg, 
	unsigned char *deltoffst, unsigned char *deltlen,
	unsigned char *deltba)
{

	int bin,i,j,k,end1,v,v1,bndstrt,bndend,lowcomp,begin;
	int fastleak,slowleak,address,tmp;

	short psd[256];		// scaled exponents
	short bndpsd[50];	// interpolated exponents
	short excite[50];	// excitation
	short mask[50];		// masking value

	// exponent mapping to PSD
	for(bin = start; bin < end; bin++) {
		psd[bin] = (3072 - (exp[bin] << 7));
	}

	// PSD integration
	j = start;
	k = masktab[start];

	do {
		v = psd[j];
		j++;

		end1 = bndtab[k+1];
		if (end1 > end) end1 = end;

		for(i = j; i < end1; i++) {
			int c, adr;

			// logadd
			v1 = psd[j];
			c = v - v1;

			if (c >= 0) {
				adr = c >> 1;
				if (adr > 255) adr = 255;
				v = v + latab[adr];
			} else {
				adr = (-c) >> 1;
				if (adr > 255) adr = 255;
				v = v1 + latab[adr];
			}

			j++;
		}

		bndpsd[k] = v;
		k++;

	} while (end > bndtab[k]);

	// excitation function
	bndstrt = masktab[start];
	bndend = masktab[end-1] + 1;

	if (bndstrt == 0) {
		lowcomp = 0;

		lowcomp = calc_lowcomp1(lowcomp, bndpsd[0], bndpsd[1]);
		excite[0] = bndpsd[0] - fgain - lowcomp;

		lowcomp = calc_lowcomp1(lowcomp, bndpsd[1], bndpsd[2]);
		excite[1] = bndpsd[1] - fgain - lowcomp;

		begin = 7;
		for (bin = 2; bin < 7; bin++) {

			if (!(is_lfe && bin == 6)) {
				lowcomp = calc_lowcomp1(lowcomp, bndpsd[bin], bndpsd[bin+1]);
			}

			fastleak = bndpsd[bin] - fgain;
			slowleak = bndpsd[bin] - s->sgain;
			excite[bin] = fastleak - lowcomp;

			if (!(is_lfe && bin == 6)) {
				if (bndpsd[bin] <= bndpsd[bin+1]) {
					begin = bin + 1;
					break;
				}
			}
		}
    
		end1 = bndend;
		if (end1 > 22) end1 = 22;

		for (bin = begin; bin < end1; bin++) {
			if (!(is_lfe && bin == 6)) {
				lowcomp = calc_lowcomp(lowcomp, bndpsd[bin], bndpsd[bin+1], bin);
			}

			fastleak -= s->fdecay;
			v = bndpsd[bin] - fgain;
			if (fastleak < v) fastleak = v;

			slowleak -= s->sdecay;
			v = bndpsd[bin] - s->sgain;
			if (slowleak < v) slowleak = v;

			v=fastleak - lowcomp;
			if (slowleak > v) v=slowleak;

			excite[bin] = v;
		}
		begin = 22;

	} else {
		// coupling channel
		begin = bndstrt;

		fastleak = (s->cplfleak << 8) + 768;
		slowleak = (s->cplsleak << 8) + 768;
	}

	for (bin = begin; bin < bndend; bin++) {
		fastleak -= s->fdecay;
		v = bndpsd[bin] - fgain;
		if (fastleak < v) fastleak = v;

		slowleak -= s->sdecay;
		v = bndpsd[bin] - s->sgain;
		if (slowleak < v) slowleak = v;

		v = fastleak;
		if (slowleak > v) v = slowleak;
		excite[bin] = v;
	}

	// compute masking curve

	for (bin = bndstrt; bin < bndend; bin++) {
		v1 = excite[bin];

		tmp = s->dbknee - bndpsd[bin];
		if (tmp > 0) v1 += tmp >> 2;

		v = hth[bin >> s->halfratecod][s->fscod];
		if (v1 > v) v = v1;

		mask[bin] = v;
	}

	// delta bit allocation

	if (deltbae == 0 || deltbae == 1) {
		int seg, delta;
		int band = 0;

		for (seg = 0; seg < deltnseg; seg++) {
			band += deltoffst[seg];

			if (deltba[seg] >= 4) {
				delta = (deltba[seg] - 3) << 7;
			} else {
				delta = (deltba[seg] - 4) << 7;
			}

			for (k = 0; k < deltlen[seg]; k++) {
				mask[band] += delta;
				band++;
			}
		}
	}

	// compute bit allocation

	i = start;
	j = masktab[start];

	do {
		v = mask[j];
		v -= snroffset;
		v -= s->floor;

		if (v < 0) v = 0;
		v &= 0x1FE0;
		v += s->floor;

		end1 = bndtab[j] + bndsz[j];
		if (end1 > end) end1 = end;

		for (k = i; k < end1; k++) {
			address = (psd[i] - v) >> 5;
			
			if (address < 0) {
				address = 0;
			} else if (address > 63) {
				address = 63;
			}
		
			bap[i] = baptab[address];
			i++;
		}
	} while (end > bndtab[j++]);
}

typedef struct {
	short re;
	short im;
} IComplex;

static inline short fix15(float a)
{
	int v = (int)(a * (float)(1 << 15));

	if (v < -32767) {
		v = -32767;
	} else if (v > 32767) {
		v = 32767;
	}

	return (short)v;
}

static void fft_init(int ln)
{
	int i, m;
	int n = 1 << ln;

	for(i = 0; i < (n/2); i++) {
		float alpha = (float)(2 * M_PI * (float)i / (float)n);
		costab[i] = fix15((float)cos(alpha));
		sintab[i] = fix15((float)sin(alpha));
	}

	for(i = 0; i < n; i++) {
		m = 0;
		for(int j = 0; j < ln; j++) {
			m |= ((i >> j) & 1) << (ln-j-1);
		}
		fft_rev[i] = m;
	}
}

// butter fly op
#define BF(pre, pim, qre, qim, pre1, pim1, qre1, qim1) \
{\
	int ax, ay, bx, by;\
	bx = pre1;\
	by = pim1;\
	ax = qre1;\
	ay = qim1;\
	pre = (bx + ax) >> 1;\
	pim = (by + ay) >> 1;\
	qre = (bx - ax) >> 1;\
	qim = (by - ay) >> 1;\
}

#define MUL16(a,b) ((a) * (b))

#define CMUL(pre, pim, are, aim, bre, bim) \
{\
	pre = (MUL16(are, bre) - MUL16(aim, bim)) >> 15;\
	pim = (MUL16(are, bim) + MUL16(bre, aim)) >> 15;\
}


// do a 2^n point complex fft on 2^ln points.
static void fft(IComplex *z, int ln)
{
	int	j, l, np, np2;
	int	nblocks, nloops;
	register IComplex *p,*q;
	int tmp_re, tmp_im;

	np = 1 << ln;

	// reverse
	for(j = 0; j < np; j++) {
		int k = fft_rev[j];

		if (k < j) {
			IComplex tmp = z[k];
			z[k] = z[j];
			z[j] = tmp;
		}
	}

	// pass 0

	p = &z[0];
	j = (np >> 1);
	do {
		BF(p[0].re, p[0].im, p[1].re, p[1].im, 
			p[0].re, p[0].im, p[1].re, p[1].im);

		p += 2;
	} while (--j != 0);

	// pass 1

	p = &z[0];
	j = np >> 2;
	do {
		BF(p[0].re, p[0].im, p[2].re, p[2].im, 
			p[0].re, p[0].im, p[2].re, p[2].im);

		BF(p[1].re, p[1].im, p[3].re, p[3].im, 
			p[1].re, p[1].im, p[3].im, -p[3].re);

		p += 4;
	} while (--j != 0);

	/* pass 2 .. ln-1 */

	nblocks = np >> 3;
	nloops = 1 << 2;
	np2 = np >> 1;

	do {
		p = z;
		q = z + nloops;

		for (j = 0; j < nblocks; ++j) {

			BF(p->re, p->im, q->re, q->im,
				p->re, p->im, q->re, q->im);

			p++;
			q++;

			for(l = nblocks; l < np2; l += nblocks) {

				CMUL(tmp_re, tmp_im, costab[l],
					-sintab[l], q->re, q->im);

				BF(p->re, p->im, q->re, q->im,
					p->re, p->im, tmp_re, tmp_im);

				p++;
				q++;
			}

			p += nloops;
			q += nloops;
		}

		nblocks = nblocks >> 1;
		nloops = nloops << 1;

	} while (nblocks != 0);
}

// do a 512 point mdct
static void mdct512(long *out, short *in)
{
	int i, re, im, re1, im1;
	short rot[MDCT_N]; 
	IComplex x[MDCT_N/4];

	// shift to simplify computations
	for(i = 0; i < MDCT_N/4; i++) {
		rot[i] = -in[i + 3*MDCT_N/4];
	}

	for(i = MDCT_N/4; i < MDCT_N; i++) {
		rot[i] = in[i - MDCT_N/4];
	}
        
	// pre rotation
	for(i = 0; i < MDCT_N/4; i++) {
		re = ((int)rot[2*i] - (int)rot[MDCT_N-1-2*i]) >> 1;
		im = -((int)rot[MDCT_N/2+2*i] - (int)rot[MDCT_N/2-1-2*i]) >> 1;
		CMUL(x[i].re, x[i].im, re, im, -xcos1[i], xsin1[i]);
	}

	fft(x, MDCT_NBITS - 2);
  
	// post rotation
	for(i = 0; i < MDCT_N/4; i++) {
		re = x[i].re;
		im = x[i].im;
		CMUL(re1, im1, re, im, xsin1[i], xcos1[i]);
		out[2*i] = im1;
		out[MDCT_N/2-1-2*i] = re1;
	}
}

// XXX: use another norm ?
static int calc_exp_diff(unsigned char *exp1, unsigned char *exp2, int n)
{
	int i;
	int sum = 0;

	for(i = 0; i < n; i++) {
		sum += abs(exp1[i] - exp2[i]);
	}
	return sum;
}

static void compute_exp_strategy(
	unsigned char exp_strategy[NB_BLOCKS][AC3_MAX_CHANNELS],
	unsigned char exp[NB_BLOCKS][AC3_MAX_CHANNELS][MDCT_N/2],
	int ch, int is_lfe)
{
	int i, j;
	int exp_diff;

	// estimate if the exponent variation & decide
	// if they should be reused in the next frame
	exp_strategy[0][ch] = EXP_NEW;

	for(i = 1; i < NB_BLOCKS; i++) {
		exp_diff = calc_exp_diff(exp[i][ch], exp[i-1][ch], MDCT_N/2);

		if (exp_diff > EXP_DIFF_THRESHOLD) {
			exp_strategy[i][ch] = EXP_NEW;
		} else {
			exp_strategy[i][ch] = EXP_REUSE;
		}
	}

	if (is_lfe) return;

	// now select the encoding strategy type : if exponents
	// are often recoded, we use a coarse encoding
	i = 0;
	while (i < NB_BLOCKS) {
		j = i + 1;

		while (j < NB_BLOCKS && exp_strategy[j][ch] == EXP_REUSE) {
			j++;
		}

		switch(j - i) {

		case 1:
			exp_strategy[i][ch] = EXP_D45;
			break;

		case 2:
		case 3:
			exp_strategy[i][ch] = EXP_D25;
			break;

		default:
			exp_strategy[i][ch] = EXP_D15;
			break;
		}

		i = j;
	}
}

// set exp[i] to min(exp[i], exp1[i])
//static void exponent_min(uint8_t exp[MDCT_N/2], uint8_t exp1[MDCT_N/2], int n)
static void exponent_min(unsigned char *exp, unsigned char *exp1, int n)
{
	int i;

	for(i = 0; i < n; i++) {
		if (exp1[i] < exp[i]) exp[i] = exp1[i];
	}
}
                                 
// update the exponents so that they are the ones the decoder will
// decode. Return the number of bits used to code the exponents
static int encode_exp(unsigned char encoded_exp[MDCT_N/2],
	unsigned char exp[MDCT_N/2], int nb_exps, int exp_strategy)
{
	int group_size, nb_groups, i, j, k, recurse, exp_min, delta;
	unsigned char exp1[MDCT_N/2];

	switch(exp_strategy) {

	case EXP_D15:
		group_size = 1;
		break;

	case EXP_D25:
		group_size = 2;
		break;

	default:
	case EXP_D45:
		group_size = 4;
		break;
	}

	nb_groups = ((nb_exps + (group_size * 3) - 4) / (3 * group_size)) * 3;

	// for each group, compute the minimum exponent
	exp1[0] = exp[0];	// DC exponent is handled separately

	k = 1;
	for(i = 1; i <= nb_groups; i++) {
		exp_min = exp[k];
		_ASSERT(exp_min >= 0 && exp_min <= 24);

		for(j = 1; j < group_size; j++) {
			if (exp[k+j] < exp_min) exp_min = exp[k+j];
		}

		exp1[i] = exp_min;
		k += group_size;
	}

	// constraint for DC exponent
	if (exp1[0] > 15) exp1[0] = 15;

	// Iterate until the delta constraints between each groups are
	// satisfyed. I'm sure it is possible to find a better algorithm,
	// but I am lazy
	do {
		recurse = 0;

		for(i = 1; i <= nb_groups; i++) {
			delta = exp1[i] - exp1[i-1];

			if (delta > 2) {
				// if delta too big, we encode a smaller exponent
				exp1[i] = exp1[i-1] + 2;

			} else if (delta < -2) {
				// if delta is too small, we must decrease the
				// previous exponent, which means we must recurse
				recurse = 1;
				exp1[i-1] = exp1[i] + 2;
			}
		}
	} while (recurse);
    
	// now we have the exponent values the decoder will see
	encoded_exp[0] = exp1[0];

	k = 1;
	for(i = 1; i <= nb_groups; i++) {
		for(j = 0; j < group_size; j++) {
			encoded_exp[k+j] = exp1[i];
		}
		k += group_size;
	}

	return 4 + (nb_groups / 3) * 7;
}

// return the size in bits taken by the mantissa
static int compute_mantissa_size(AC3EncodeContext *s, const unsigned char *m, int nb_coefs)
{
	int bits, mant, i;

	bits = 0;
	for(i = 0; i < nb_coefs; i++) {
		mant = m[i];

		switch(mant) {

		case 0:		// nothing
			break;

		case 1:		// 3 mantissa in 5 bits
			if (s->mant1_cnt == 0) bits += 5;
			if (++s->mant1_cnt == 3) s->mant1_cnt = 0;
			break;

		case 2:		// 3 mantissa in 7 bits
			if (s->mant2_cnt == 0) bits += 7;
			if (++s->mant2_cnt == 3) s->mant2_cnt = 0;
			break;

		case 3:
			bits += 3;
			break;

		case 4:		// 2 mantissa in 7 bits
			if (s->mant4_cnt == 0) bits += 7;
			if (++s->mant4_cnt == 2) s->mant4_cnt = 0;
			break;

		case 14:
			bits += 14;
			break;

		case 15:
			bits += 16;
			break;

		default:
			bits += mant - 1;
			break;
		}
	}
	return bits;
}


static int bit_alloc(AC3EncodeContext *s,
	unsigned char bap[NB_BLOCKS][AC3_MAX_CHANNELS][MDCT_N/2],
	unsigned char encoded_exp[NB_BLOCKS][AC3_MAX_CHANNELS][MDCT_N/2],
	unsigned char exp_strategy[NB_BLOCKS][AC3_MAX_CHANNELS],
	int frame_bits, int csnroffst, int fsnroffst)
{
	int i, ch;

	// compute size
	for(i = 0; i < NB_BLOCKS; i++) {
		s->mant1_cnt = 0;
		s->mant2_cnt = 0;
		s->mant4_cnt = 0;

		for(ch = 0; ch < s->nb_all_channels; ch++) {

			ac3_parametric_bit_allocation(
				&s->bit_alloc, bap[i][ch],
				(signed char *)encoded_exp[i][ch], 
				0, s->nb_coefs[ch], 
				(((csnroffst-15) << 4) + 
				fsnroffst) << 2, 
				fgaintab[s->fgaincod[ch]],
				ch == s->lfe_channel,
				2, 0, NULL, NULL, NULL);

			frame_bits += compute_mantissa_size(
				s, bap[i][ch], s->nb_coefs[ch]);
		}
	}

	return 16 * s->frame_size - frame_bits;
}

#define SNR_INC1 4

static int compute_bit_allocation(AC3EncodeContext *s,
	unsigned char bap[NB_BLOCKS][AC3_MAX_CHANNELS][MDCT_N/2],
	unsigned char encoded_exp[NB_BLOCKS][AC3_MAX_CHANNELS][MDCT_N/2],
	unsigned char exp_strategy[NB_BLOCKS][AC3_MAX_CHANNELS],
	int frame_bits)
{
	int i, ch;
	int csnroffst, fsnroffst;
	unsigned char bap1[NB_BLOCKS][AC3_MAX_CHANNELS][MDCT_N/2];
	static int frame_bits_inc[8] = { 0, 0, 2, 2, 2, 4, 2, 4 };

	// init default parameters
	s->sdecaycod = 2;
	s->fdecaycod = 1;
	s->sgaincod = 1;
	s->dbkneecod = 2;
	s->floorcod = 4;

	for(ch = 0; ch < s->nb_all_channels; ch++) {
		s->fgaincod[ch] = 4;
	}
    
	// compute real values
	s->bit_alloc.fscod = s->fscod;
	s->bit_alloc.halfratecod = s->halfratecod;
	s->bit_alloc.sdecay = sdecaytab[s->sdecaycod] >> s->halfratecod;
	s->bit_alloc.fdecay = fdecaytab[s->fdecaycod] >> s->halfratecod;
	s->bit_alloc.sgain = sgaintab[s->sgaincod];
	s->bit_alloc.dbknee = dbkneetab[s->dbkneecod];
	s->bit_alloc.floor = floortab[s->floorcod];
    
	// header size
	frame_bits += 65;
	// if (s->acmod == 2) frame_bits += 2;
	frame_bits += frame_bits_inc[s->acmod];

	// audio blocks
	for(i = 0; i < NB_BLOCKS; i++) {
		frame_bits += s->nb_channels * 2 + 2;	// blksw * c, dithflag * c, dynrnge, cplstre

		if (s->acmod == 2) frame_bits++;	// rematstr
		frame_bits += 2 * s->nb_channels;	// chexpstr[2] * c

		if (s->lfe) frame_bits++;			// lfeexpstr

		for(ch = 0; ch < s->nb_channels; ch++) {
			if (exp_strategy[i][ch] != EXP_REUSE) {
				frame_bits += 6 + 2;		// chbwcod[6], gainrng[2]
			}
		}

		frame_bits++;		// baie
		frame_bits++;		// snr
		frame_bits += 2;	// delta / skip
	}
	frame_bits++;			// cplinu for block 0

	// bit alloc info
	// sdcycod[2], fdcycod[2], sgaincod[2], dbpbcod[2], floorcod[3]
	// csnroffset[6]
	// (fsnoffset[4] + fgaincod[4]) * c
	frame_bits += 2*4 + 3 + 6 + s->nb_all_channels * (4 + 3);

	// auxdatae, crcrsv
	frame_bits += 2;

	// CRC
	frame_bits += 16;

	// now the big work begins : do the bit allocation. Modify the snr
	// offset until we can pack everything in the requested frame size

	csnroffst = s->csnroffst;

	while (csnroffst >= 0 &&
		bit_alloc(s, bap, encoded_exp, exp_strategy,
		frame_bits, csnroffst, 0) < 0) {

		csnroffst -= SNR_INC1;
	}

	if (csnroffst < 0) {
		fprintf(stderr, "Yack, Error !!!\n");
		return -1;
	}

	while ((csnroffst + SNR_INC1) <= 63 &&
		bit_alloc(s, bap1, encoded_exp, exp_strategy,
		frame_bits, csnroffst + SNR_INC1, 0) >= 0) {

		csnroffst += SNR_INC1;
		memcpy(bap, bap1, sizeof(bap1));
	}

	while ((csnroffst + 1) <= 63 &&
		bit_alloc(s, bap1, encoded_exp, exp_strategy,
		frame_bits, csnroffst + 1, 0) >= 0) {

		csnroffst++;
		memcpy(bap, bap1, sizeof(bap1));
	}

	fsnroffst = 0;

	while ((fsnroffst + SNR_INC1) <= 15 &&
		bit_alloc(s, bap1, encoded_exp, exp_strategy,
		frame_bits, csnroffst, fsnroffst + SNR_INC1) >= 0) {

		fsnroffst += SNR_INC1;
		memcpy(bap, bap1, sizeof(bap1));
	}

	while ((fsnroffst + 1) <= 15 &&
		bit_alloc(s, bap1, encoded_exp, exp_strategy,
		frame_bits, csnroffst, fsnroffst + 1) >= 0) {

		fsnroffst++;
		memcpy(bap, bap1, sizeof(bap1));
	}
    
	s->csnroffst = csnroffst;
	for(ch = 0; ch < s->nb_all_channels; ch++) {
		s->fsnroffst[ch] = fsnroffst;
	}

	return 0;
}

static void ac3_common_init(void)
{
	// compute bndtab and masktab from bandsz
	int i;
	int k = 0;
	int l = 0;

	for(i = 0; i < 50; i++) {
		bndtab[i] = l;
		int v = bndsz[i];
		for (int j = 0; j < v; j++) {
			masktab[k++] = i;
		}
		l += v;
	}
	bndtab[50] = 0;
}

// compute the ac3 crc
#define CRC16_POLY ((1 << 0) | (1 << 2) | (1 << 15) | (1 << 16))

static void ac3_crc_init(void)
{
	unsigned int n;

	for(n = 0; n < 256; n++) {
		unsigned int k;
		unsigned int c = n << 8;

		for (k = 0; k < 8; k++) {
			if (c & (1 << 15)) {
				c = ((c << 1) & 0xffff) ^ (CRC16_POLY & 0xffff);
			} else {
				c = c << 1;
			}
		}

		crc_table[n] = c;
	}
}


int AC3_encode_init(int freq, int bitrate, int channels)
{
//	int freq = avctx->sample_rate;
//	int bitrate = avctx->bit_rate;
//	int channels = avctx->channels;

	AC3EncodeContext *s = &ac3enc_state;
	int i, j, ch;
	float alpha;

	static const unsigned char acmod_defs[6] = {
		0x01,	// C
		0x02,	// L R
		0x03,	// L C R
		0x06,	// L R SL SR
		0x07,	// L C R SL SR
		0x07,	// L C R SL SR (+LFE)
	};

	// number of channels
	if (channels < 1 || channels > 6) return 0;

	s->acmod = acmod_defs[channels - 1];
	s->lfe = (channels == 6)? 1: 0;
	s->nb_all_channels = channels;
	s->nb_channels = (channels > 5)? 5: channels;
	s->lfe_channel = (s->lfe)? 5: -1;
	s->sample_rate = 0;

	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++) {
			if ((ac3_freqs[j] >> i) == freq) {
				s->sample_rate = freq;
				s->halfratecod = i;
				s->fscod = j;
				i = 2;
				break;
			}
		}
	}

	if (s->sample_rate == 0) return 0;

	s->bsid = 8 + s->halfratecod;
	s->bsmod = 0;	// complete main audio service

	// bitrate & frame size
	bitrate /= 1000;
	for(i = 0; i < 19; i++) {
		if ((ac3_bitratetab[i] >> s->halfratecod) == bitrate) break;
	}
	if (i == 19) return 0;

	s->bit_rate = bitrate;
	s->frmsizecod = i << 1;
	s->frame_size_min = (bitrate * 1000 * AC3_FRAME_SIZE) / (freq * 16);

	// for now we do not handle fractional sizes
	s->frame_size = s->frame_size_min;
    
	// bit allocation init
	for(ch = 0; ch < s->nb_channels; ch++) {
		// bandwidth for each channel
		// XXX: should compute the bandwidth according to the frame
		// size, so that we avoid anoying high freq artefacts
		s->chbwcod[ch] = 50;	// sample bandwidth as mpeg audio layer 2 table 0
		s->nb_coefs[ch] = ((s->chbwcod[ch] + 12) * 3) + 37;
	}
	if (s->lfe) {
		s->nb_coefs[s->lfe_channel] = 7;	// fixed
	}

	// initial snr offset
	s->csnroffst = 40;

	ac3_common_init();

	// mdct init
	fft_init(MDCT_NBITS - 2);
	for(i = 0; i < MDCT_N/4; i++) {
		alpha = (float)(2 * M_PI * (i + 1.0 / 8.0) / (float)MDCT_N);
		xcos1[i] = fix15((float)-cos(alpha));
		xsin1[i] = fix15((float)-sin(alpha));
	}

	ac3_crc_init();

//	avctx->coded_frame = avcodec_alloc_frame();
//	avctx->coded_frame->key_frame = 1;

	return s->frame_size * 2;
}

// output the AC3 frame header
static void output_frame_header(AC3EncodeContext *s, unsigned char *dst)
{
	init_put_bits(&s->pb, dst, AC3_MAX_CODED_FRAME_SIZE);

	put_bits(&s->pb, 16, 0x0b77);	// frame header
	put_bits(&s->pb, 16, 0);		// crc1: will be filled later
	put_bits(&s->pb, 2, s->fscod);
	put_bits(&s->pb, 6, s->frmsizecod + (s->frame_size - s->frame_size_min));
	put_bits(&s->pb, 5, s->bsid);
	put_bits(&s->pb, 3, s->bsmod);
	put_bits(&s->pb, 3, s->acmod);

	if ((s->acmod & 0x01) && s->acmod != 0x01) {
		put_bits(&s->pb, 2, 1);		// XXX -4.5 dB
	}

	if (s->acmod & 0x04) {
		put_bits(&s->pb, 2, 1);		// XXX -6 dB
	}

	if (s->acmod == 0x02) {
		put_bits(&s->pb, 2, 0);		// surround not indicated
	}

	put_bits(&s->pb, 1, s->lfe);	// LFE
	put_bits(&s->pb, 5, 31);		// dialog norm: -31 db
	put_bits(&s->pb, 1, 0);			// no compression control word
	put_bits(&s->pb, 1, 0);			// no lang code
	put_bits(&s->pb, 1, 0);			// no audio production info
	put_bits(&s->pb, 1, 0);			// no copyright
	put_bits(&s->pb, 1, 1);			// original bitstream
	put_bits(&s->pb, 1, 0);			// no time code 1
	put_bits(&s->pb, 1, 0);			// no time code 2
	put_bits(&s->pb, 1, 0);			// no addtional bit stream info
}

// symetric quantization on 'levels' levels
static inline int sym_quant(int c, int e, int levels)
{
	int v;

	if (c >= 0) {
		v = (levels * (c << e)) >> 24;
		v = (v + 1) >> 1;
		v = (levels >> 1) + v;
	} else {
		v = (levels * ((-c) << e)) >> 24;
		v = (v + 1) >> 1;
		v = (levels >> 1) - v;
	}

	_ASSERT(v >= 0 && v < levels);
	return v;
}

// asymetric quantization on 2^qbits levels
static inline int asym_quant(int c, int e, int qbits)
{
	int m, v;

	int lshift = e + qbits - 24;

	if (lshift >= 0) {
		v = c << lshift;
	} else {
		v = c >> (-lshift);
	}

	// rounding
	v = (v + 1) >> 1;
	m = (1 << (qbits-1));

	if (v >= m) v = m - 1;

	_ASSERT(v >= -m);

	return v & ((1 << qbits)-1);
}

// Output one audio block. There are NB_BLOCKS
// audio blocks in one AC3 frame
static void output_audio_block(AC3EncodeContext *s,
	unsigned char exp_strategy[AC3_MAX_CHANNELS],
	unsigned char encoded_exp[AC3_MAX_CHANNELS][MDCT_N/2],
	const unsigned char bap[AC3_MAX_CHANNELS][MDCT_N/2],
	long mdct_coefs[AC3_MAX_CHANNELS][MDCT_N/2],
	signed char global_exp[AC3_MAX_CHANNELS],
	int block_num)
{
	int ch, nb_groups, group_size, i, baie;
	unsigned char *p;
	unsigned short qmant[AC3_MAX_CHANNELS][MDCT_N/2];
	int exp0, exp1;
	int mant1_cnt, mant2_cnt, mant4_cnt;
	unsigned short *qmant1_ptr, *qmant2_ptr, *qmant4_ptr;
	int delta0, delta1, delta2;

	for(ch = 0; ch < s->nb_channels; ch++) {
		put_bits(&s->pb, 1, 0);	// 512 point MDCT
	}

	for(ch = 0; ch < s->nb_channels; ch++) {
		put_bits(&s->pb, 1, 1);	// no dither
	}

	put_bits(&s->pb, 1, 0);		// no dynamic range

	if (block_num == 0) {
		// for block 0, even if no coupling, we must say it.
		// This is a waste of bit :-)
		put_bits(&s->pb, 1, 1);	// coupling strategy present
		put_bits(&s->pb, 1, 0);	// no coupling strategy
	} else {
		put_bits(&s->pb, 1, 0);	// no new coupling strategy
	}

	if (s->acmod == 2) {
		if(block_num == 0) {
			int rbnd;
			// first block must define rematrixing (rematstr)
			put_bits(&s->pb, 1, 1); 

			// dummy rematrixing rematflg(1:4)=0
			for (rbnd = 0; rbnd < 4; rbnd++) {
				put_bits(&s->pb, 1, 0);
			}
		} else {
			// no matrixing (but should be used in the future)
			put_bits(&s->pb, 1, 0);
		} 
	}

	// exponent strategy
	for(ch = 0; ch < s->nb_channels; ch++) {
		put_bits(&s->pb, 2, exp_strategy[ch]);
	}

	if (s->lfe) {
		put_bits(&s->pb, 1, exp_strategy[s->lfe_channel]);
	}

	for(ch = 0; ch < s->nb_channels; ch++) {
		if (exp_strategy[ch] != EXP_REUSE) {
			put_bits(&s->pb, 6, s->chbwcod[ch]);
		}
	}
    
	// exponents
	for (ch = 0; ch < s->nb_all_channels; ch++) {
	
		switch(exp_strategy[ch]) {
		
		case EXP_REUSE:
			continue;
	
		case EXP_D15:
			group_size = 1;
			break;
	
		case EXP_D25:
			group_size = 2;
			break;
	
		default:
			case EXP_D45:
			group_size = 4;
			break;
		}
	
		nb_groups = (s->nb_coefs[ch] + (group_size * 3) - 4) / (3 * group_size);
		p = encoded_exp[ch];

		// first exponent
		exp1 = *p++;
		put_bits(&s->pb, 4, exp1);

		// next ones are delta encoded
		for(i = 0; i < nb_groups; i++) {

			// merge three delta in one code
			exp0 = exp1;
			exp1 = p[0];
			p += group_size;
			delta0 = exp1 - exp0 + 2;

			exp0 = exp1;
			exp1 = p[0];
			p += group_size;
			delta1 = exp1 - exp0 + 2;

			exp0 = exp1;
			exp1 = p[0];
			p += group_size;
			delta2 = exp1 - exp0 + 2;

			put_bits(&s->pb, 7, ((delta0 * 5 + delta1) * 5) + delta2);
		}

		if (ch != s->lfe_channel) {
			put_bits(&s->pb, 2, 0);	// no gain range info
		}
    }

	// bit allocation info
	baie = (block_num == 0);
	put_bits(&s->pb, 1, baie);
	if (baie) {
		put_bits(&s->pb, 2, s->sdecaycod);
		put_bits(&s->pb, 2, s->fdecaycod);
		put_bits(&s->pb, 2, s->sgaincod);
		put_bits(&s->pb, 2, s->dbkneecod);
		put_bits(&s->pb, 3, s->floorcod);
	}

	// snr offset
	put_bits(&s->pb, 1, baie);	// always present with bai

	if (baie) {
		put_bits(&s->pb, 6, s->csnroffst);

		for(ch = 0; ch < s->nb_all_channels; ch++) {
			put_bits(&s->pb, 4, s->fsnroffst[ch]);
			put_bits(&s->pb, 3, s->fgaincod[ch]);
		}
	}

	put_bits(&s->pb, 1, 0);	// no delta bit allocation
	put_bits(&s->pb, 1, 0);	// no data to skip

	// mantissa encoding : we use two passes to handle the grouping
	// one pass method may be faster, but it would necessitate to
	// modify the output stream.

	// first pass: quantize
	mant1_cnt = mant2_cnt = mant4_cnt = 0;
	qmant1_ptr = qmant2_ptr = qmant4_ptr = NULL;

	for (ch = 0; ch < s->nb_all_channels; ch++) {
		int b, c, e, v;

		for(i = 0; i < s->nb_coefs[ch]; i++) {

			c = mdct_coefs[ch][i];
			e = encoded_exp[ch][i] - global_exp[ch];
			b = bap[ch][i];

            switch(b) {
	
			case 0:
				v = 0;
				break;

			case 1:
				v = sym_quant(c, e, 3);

				switch(mant1_cnt) {
				
				case 0:
					qmant1_ptr = &qmant[ch][i];
					v = 9 * v;
					mant1_cnt = 1;
					break;
			
				case 1:
					*qmant1_ptr += 3 * v;
					mant1_cnt = 2;
					v = 128;
					break;
				
				default:
					*qmant1_ptr += v;
					mant1_cnt = 0;
					v = 128;
					break;
				}
				break;

			case 2:
				v = sym_quant(c, e, 5);
			
				switch(mant2_cnt) {
			
				case 0:
					qmant2_ptr = &qmant[ch][i];
					v = 25 * v;
					mant2_cnt = 1;
					break;
				
				case 1:
					*qmant2_ptr += 5 * v;
					mant2_cnt = 2;
					v = 128;
					break;
			
				default:
					*qmant2_ptr += v;
					mant2_cnt = 0;
					v = 128;
					break;
				}
				break;

			case 3:
				v = sym_quant(c, e, 7);
				break;

			case 4:
				v = sym_quant(c, e, 11);
			
				switch(mant4_cnt) {
			
				case 0:
					qmant4_ptr = &qmant[ch][i];
					v = 11 * v;
					mant4_cnt = 1;
					break;
			
				default:
					*qmant4_ptr += v;
					mant4_cnt = 0;
					v = 128;
					break;
				}
				break;

			case 5:
				v = sym_quant(c, e, 15);
				break;
			
			case 14:
				v = asym_quant(c, e, 14);
				break;
			
			case 15:
				v = asym_quant(c, e, 16);
				break;
			
			default:
				v = asym_quant(c, e, b - 1);
				break;
			}

			qmant[ch][i] = v;
		}
	}

	// second pass : output the values
	for (ch = 0; ch < s->nb_all_channels; ch++) {
		int b, q;

		for(i = 0; i < s->nb_coefs[ch]; i++) {
			q = qmant[ch][i];
			b = bap[ch][i];

			switch(b) {

			case 0:
				break;

			case 1:
				if (q != 128) put_bits(&s->pb, 5, q);
				break;

			case 2:
				if (q != 128) put_bits(&s->pb, 7, q);
				break;

			case 3:
				put_bits(&s->pb, 3, q);
				break;
			
			case 4:
				if (q != 128) put_bits(&s->pb, 7, q);
				break;

			case 14:
				put_bits(&s->pb, 14, q);
				break;
			
			case 15:
				put_bits(&s->pb, 16, q);
				break;
			
			default:
				put_bits(&s->pb, b - 1, q);
				break;
			}
		}
	}
}

static unsigned int ac3_crc(unsigned char *data, int n, unsigned int crc)
{
	int i;
	for(i = 0; i < n; i++) {
		crc = (crc_table[data[i] ^ (crc >> 8)] ^ (crc << 8)) & 0xffff;
	}
	return crc;
}

static unsigned int mul_poly(unsigned int a, unsigned int b, unsigned int poly)
{
	unsigned int c = 0;

	while (a) {
		if (a & 1) c ^= b;
		a = a >> 1;
		b = b << 1;
		if (b & (1 << 16)) b ^= poly;
	}
	return c;
}

static unsigned int pow_poly(unsigned int a, unsigned int n, unsigned int poly)
{
	unsigned int r = 1;

	while (n) {
		if (n & 1) r = mul_poly(r, a, poly);
		a = mul_poly(a, a, poly);
		n >>= 1;
	}
	return r;
}


static inline int av_log2(unsigned int v)
{
	static const unsigned char ff_log2_tab[256] = {
		0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
		6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
		6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
		7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
		7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
		7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
		7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
	};

	int n = 0;

	if (v & 0xffff0000) {
		v >>= 16;
		n += 16;
	}

	if (v & 0xff00) {
		v >>= 8;
		n += 8;
	}

	n += ff_log2_tab[v];

	return n;
}

// compute log2(max(abs(tab[])))
static int log2_tab(short *tab, int n)
{
	int i;
	int v = 0;

	for(i = 0; i < n; i++) {
		v |= abs(tab[i]);
	}

	return av_log2(v);
}

static void lshift_tab(short *tab, int n, int lshift)
{
	int i;

	if (lshift > 0) {
		for(i = 0; i < n; i++) {
			tab[i] <<= lshift;
		}
	} else if (lshift < 0) {
		lshift = -lshift;
		for(i = 0; i < n; i++) {
			tab[i] >>= lshift;
		}
	}
}

// fill the end of the frame and compute the two crcs
static int output_frame_end(AC3EncodeContext *s)
{
	int frame_size, frame_size_58, n, crc1, crc2, crc_inv;
	unsigned char *frame, *frame_end;

	frame_size = s->frame_size;	// frame size in words

	// align to 8 bits
	flush_put_bits(&s->pb);

	// AC3ACM: Getting an assert below while compressing to stereo
	// n = -1
	// s->frame_size = 384 (192 kbit/sec)
	// frame     = 0x09914830
	// frame_end = 0x09914B2F (diff. of 767)

	// add zero bytes to reach the frame size
	frame = s->pb.buf;
	frame_end = pbBufPtr(&s->pb);
	n = 2 * s->frame_size - (int)(frame_end - frame) - 2;
	_ASSERT(n >= 0);
	if (n > 0) memset(frame_end, 0, n);
    
	// Now we must compute both crcs : this is not so easy
	// for crc1 because it is at the beginning of the data...
	frame_size_58 = (frame_size >> 1) + (frame_size >> 3);
	crc1 = ac3_crc(frame + 4, (2 * frame_size_58) - 4, 0);

	// XXX: could precompute crc_inv
	crc_inv = pow_poly((CRC16_POLY >> 1), (16 * frame_size_58) - 16, CRC16_POLY);
	crc1 = mul_poly(crc_inv, crc1, CRC16_POLY);
	frame[2] = crc1 >> 8;
	frame[3] = crc1;

	crc2 = ac3_crc(frame + 2 * frame_size_58, (frame_size - frame_size_58) * 2 - 2, 0);
	frame[2*frame_size - 2] = crc2 >> 8;
	frame[2*frame_size - 1] = crc2;

	return frame_size * 2;
}

int AC3_encode_frame(unsigned char *dst, short *samples, unsigned char *chmap)
{
	AC3EncodeContext *s = &ac3enc_state;
	int i, j, k, v, ch;

	// input_samples[]		512 * 2 bytes
	// mdct_coef[]			6 * 6 * 256 * 4 bytes
	// exp[]				6 * 6 * 256 bytes
	// exp_strategy[]		6 * 6 bytes
	// encoded_exp[]		6 * 6 * 256 bytes
	// bap[]				6 * 6 * 256 bytes
	// exp_samples[]		6 * 6 bytes

	// 1024 + 36864 + 9216 + 36 + 9216 + 9216 + 36 = 65608 bytes!

//	short input_samples[MDCT_N];
//	long mdct_coef[NB_BLOCKS][AC3_MAX_CHANNELS][MDCT_N/2];
//	unsigned char exp[NB_BLOCKS][AC3_MAX_CHANNELS][MDCT_N/2];
//	unsigned char exp_strategy[NB_BLOCKS][AC3_MAX_CHANNELS];
//	unsigned char encoded_exp[NB_BLOCKS][AC3_MAX_CHANNELS][MDCT_N/2];
//	unsigned char bap[NB_BLOCKS][AC3_MAX_CHANNELS][MDCT_N/2];
//	signed char exp_samples[NB_BLOCKS][AC3_MAX_CHANNELS];

	int frame_bits = 0;

	for(ch = 0; ch < s->nb_all_channels; ch++) {

		// fixed mdct to the six sub blocks & exponent computation
		for(i = 0; i < NB_BLOCKS; i++) {
			short *sptr;
			int sinc;

			// compute input samples
			memcpy(input_samples, s->last_samples[ch], MDCT_N/2 * sizeof(short));
			sinc = s->nb_all_channels;
			//sptr = samples + (sinc * (MDCT_N/2) * i) + ch;
			sptr = samples + (sinc * (MDCT_N/2) * i) + chmap[ch];

			for(j = 0; j < MDCT_N/2; j++) {
				v = *sptr;
				input_samples[j + MDCT_N/2] = v;
				s->last_samples[ch][j] = v; 
				sptr += sinc;
			}

			// apply the MDCT window
			for(j = 0; j < MDCT_N/2; j++) {

				input_samples[j] = MUL16(
					input_samples[j], ac3_window[j]) >> 15;

				input_samples[MDCT_N-j-1] = MUL16(
					input_samples[MDCT_N-j-1], ac3_window[j]) >> 15;
			}
        
			// Normalize the samples to use the
			// maximum available precision
			v = 14 - log2_tab(input_samples, MDCT_N);
			if (v < 0) v = 0;
			exp_samples[i][ch] = v - 9;
			lshift_tab(input_samples, MDCT_N, v);

			// do the MDCT
			mdct512(mdct_coef[i][ch], input_samples);
            
			// compute "exponents". We take into
			// account the normalization there
			for(j = 0; j < MDCT_N/2; j++) {
				int e;
				v = abs(mdct_coef[i][ch][j]);

				if (v == 0) {
					e = 24;
				} else {
					e = 23 - av_log2(v) + exp_samples[i][ch];
					if (e >= 24) {
						e = 24;
						mdct_coef[i][ch][j] = 0;
					}
				}

				exponent[i][ch][j] = e;
			}
		}
        
		compute_exp_strategy(exp_strategy, exponent, ch, ch == s->lfe_channel);

		// compute the exponents as the decoder will see them.
		// The EXP_REUSE case must be handled carefully : we
		// select the min of the exponents
		i = 0;
		while (i < NB_BLOCKS) {
			j = i + 1;
	
			while (j < NB_BLOCKS && exp_strategy[j][ch] == EXP_REUSE) {
				exponent_min(exponent[i][ch], exponent[j][ch], s->nb_coefs[ch]);
				j++;
			}
			
			frame_bits += encode_exp(encoded_exp[i][ch],
				exponent[i][ch], s->nb_coefs[ch], exp_strategy[i][ch]);

			// copy encoded exponents for reuse case
			for(k = i + 1; k < j; k++) {
				memcpy(encoded_exp[k][ch], encoded_exp[i][ch], 
					s->nb_coefs[ch] * sizeof(unsigned char));
			}

			i = j;
		}
	}

	compute_bit_allocation(s, bap, encoded_exp, exp_strategy, frame_bits);

	// everything is known... let's output the frame
	output_frame_header(s, dst);
        
	for(i = 0; i < NB_BLOCKS; i++) {
		output_audio_block(s, exp_strategy[i], encoded_exp[i], 
			bap[i], mdct_coef[i], exp_samples[i], i);
	}

	return output_frame_end(s);
}
