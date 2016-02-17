// Encoder include file for AC3ACM

#ifndef AC3ENC_H
#define AC3ENC_H

int AC3_encode_init(int freq, int bitrate, int channels);
int AC3_encode_frame(unsigned char *dst, short *samples, unsigned char *chmap);

#endif