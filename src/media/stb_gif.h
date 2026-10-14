// Internal: safe multi-frame GIF decode built on stb_image's compositor (see stb_impl.cpp for why).
#pragma once
unsigned char* ssGifDecode(const unsigned char* buf, int len, int expectFrames, int* delaysMs, int* outW, int* outH);
