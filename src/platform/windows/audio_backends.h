// Internal: audio backend constructors (selection lives in audio_factory.cpp).
#pragma once
#include "platform.h"
namespace ss { namespace win {
std::unique_ptr<IAudioCapture> makeWasapiCapture();
std::unique_ptr<IAudioCapture> makeWinmmCapture();
}}
