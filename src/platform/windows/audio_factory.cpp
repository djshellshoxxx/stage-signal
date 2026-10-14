#include "audio_backends.h"
namespace ss { namespace win {
// Backend choice is made once, here, from the explicit argument (Auto probes the OS capability only).
std::unique_ptr<IAudioCapture> makeAudioCapture(AudioBackend b) {
  if (b == AudioBackend::Wasapi) return makeWasapiCapture();
  if (b == AudioBackend::WinMM) return makeWinmmCapture();
  return wasapiAvailable() ? makeWasapiCapture() : makeWinmmCapture();
}
}}
