// Internal: safe multi-frame GIF decode built on stb_image's single-frame decoder (see stb_impl.cpp for why).
#pragma once
struct SsGifFrameInfo { int x, y, w, h, disposal; };   // from the bounds-checked pre-scan (disposal 0..3)
unsigned char* ssGifDecode(const unsigned char* buf, int len, int expectFrames, const SsGifFrameInfo* info, int screenW, int screenH,
                           int* delaysMs);
