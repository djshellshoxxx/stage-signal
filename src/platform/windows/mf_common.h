// Internal: Media Foundation includes + runtime-bound function typedefs (shared by camera and video decoder).
#pragma once
#include "win_internal.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

namespace ss { namespace win { namespace detail {
typedef HRESULT (WINAPI *PFN_MFCreateAttributes)(IMFAttributes**, UINT32);
typedef HRESULT (WINAPI *PFN_MFCreateMediaType)(IMFMediaType**);
typedef HRESULT (WINAPI *PFN_MFEnumDeviceSources)(IMFAttributes*, IMFActivate***, UINT32*);
typedef HRESULT (WINAPI *PFN_MFCreateSourceReaderFromMediaSource)(IMFMediaSource*, IMFAttributes*, IMFSourceReader**);
typedef HRESULT (WINAPI *PFN_MFCreateSourceReaderFromURL)(LPCWSTR, IMFAttributes*, IMFSourceReader**);

inline bool mtSize(IMFMediaType* t, UINT32& w, UINT32& h) {
  UINT64 v = 0; if (FAILED(t->GetUINT64(MF_MT_FRAME_SIZE, &v))) return false;
  w = (UINT32)(v >> 32); h = (UINT32)(v & 0xFFFFFFFFu); return true;
}
inline double mtFps(IMFMediaType* t) {
  UINT64 v = 0; if (FAILED(t->GetUINT64(MF_MT_FRAME_RATE, &v))) return 0;
  UINT32 n = (UINT32)(v >> 32), d = (UINT32)(v & 0xFFFFFFFFu); return d ? (double)n / d : 0;
}
inline bool isGuid(const GUID& a, const GUID& b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }
}}}
