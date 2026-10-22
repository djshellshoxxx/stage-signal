#include "Engine/Desktop/NativeDisplayIdentity.h"
#include <juce_core/juce_core.h>
#include "ss/base/sha256.hpp"
#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif JUCE_MAC
#include <CoreGraphics/CoreGraphics.h>
#elif JUCE_LINUX
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#endif
namespace ss::app {
const NativeDisplayRecord* uniqueNativeDisplay(const std::vector<NativeDisplayRecord>& records, int x, int y, int width, int height) {
    const NativeDisplayRecord* found = nullptr;
    for (const auto& record : records) {
        if (record.x != x || record.y != y || record.width != width || record.height != height || record.identity.empty()) continue;
        if (found) return nullptr;
        found = &record;
    }
    if (found) {
        for (const auto& record : records)
            if (&record != found && record.identity == found->identity) return nullptr;
    }
    return found;
}
std::vector<NativeDisplayRecord> nativeDisplayIdentities() {
    std::vector<NativeDisplayRecord> result;
#if JUCE_WINDOWS
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS || pathCount > 128 || modeCount > 512) return result;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) return result;
    for (UINT32 i = 0; i < pathCount; ++i) {
        const auto& path = paths[i];
        if (path.sourceInfo.modeInfoIdx >= modeCount) continue;
        const auto& mode = modes[path.sourceInfo.modeInfoIdx];
        if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME; target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId; target.header.id = path.targetInfo.id;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME; source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId; source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS || DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        const auto identity = juce::String(target.monitorDevicePath).toStdString();
        if (identity.empty()) continue;
        result.push_back({mode.sourceMode.position.x, mode.sourceMode.position.y, static_cast<int>(mode.sourceMode.width), static_cast<int>(mode.sourceMode.height),
                          "windows:" + identity, juce::String(source.viewGdiDeviceName).toStdString()});
    }
#elif JUCE_MAC
    CGDirectDisplayID displays[128]{}; uint32_t count = 0;
    if (CGGetActiveDisplayList(128, displays, &count) != kCGErrorSuccess) return result;
    for (uint32_t i = 0; i < count; ++i) {
        const auto display = displays[i];
        CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(display);
        if (!uuid) continue;
        CFStringRef text = CFUUIDCreateString(kCFAllocatorDefault, uuid);
        char buffer[256]{};
        const bool ok = text && CFStringGetCString(text, buffer, sizeof(buffer), kCFStringEncodingUTF8);
        if (text) CFRelease(text);
        CFRelease(uuid);
        if (!ok) continue;
        const auto bounds = CGDisplayBounds(display);
        result.push_back({static_cast<int>(bounds.origin.x), static_cast<int>(bounds.origin.y), static_cast<int>(bounds.size.width), static_cast<int>(bounds.size.height),
                          std::string("macos:") + buffer, "cg:" + std::to_string(display)});
    }
#elif JUCE_LINUX
    // Runtime-loaded X11 avoids opening a display or loading X11 on unsupported Wayland/headless paths.
    juce::DynamicLibrary x11, randr;
    if (!x11.open("libX11.so.6") || !randr.open("libXrandr.so.2")) return result;
#define SS_X_FN(library, name) const auto name##Fn = reinterpret_cast<decltype(&::name)>(library.getFunction(#name)); if (!name##Fn) return result
    SS_X_FN(x11, XOpenDisplay); SS_X_FN(x11, XCloseDisplay); SS_X_FN(x11, XDefaultRootWindow);
    SS_X_FN(x11, XInternAtom); SS_X_FN(x11, XFree);
    SS_X_FN(randr, XRRGetScreenResourcesCurrent); SS_X_FN(randr, XRRFreeScreenResources);
    SS_X_FN(randr, XRRGetOutputInfo); SS_X_FN(randr, XRRFreeOutputInfo);
    SS_X_FN(randr, XRRGetCrtcInfo); SS_X_FN(randr, XRRFreeCrtcInfo); SS_X_FN(randr, XRRGetOutputProperty);
#undef SS_X_FN
    Display* display = XOpenDisplayFn(nullptr);
    if (!display) return result;
    auto* resources = XRRGetScreenResourcesCurrentFn(display, XDefaultRootWindowFn(display));
    const Atom edid = XInternAtomFn(display, "EDID", True);
    if (resources && edid != None && resources->noutput <= 128) {
        for (int i = 0; i < resources->noutput; ++i) {
            auto* output = XRRGetOutputInfoFn(display, resources, resources->outputs[i]);
            if (!output) continue;
            if (output->connection == RR_Connected && output->crtc != None) {
                auto* crtc = XRRGetCrtcInfoFn(display, resources, output->crtc);
                Atom type = None; int format = 0; unsigned long count = 0, after = 0; unsigned char* bytes = nullptr;
                const int status = XRRGetOutputPropertyFn(display, resources->outputs[i], edid, 0, 256, False, False, AnyPropertyType,
                                                         &type, &format, &count, &after, &bytes);
                if (crtc && status == Success && bytes && format == 8 && count >= 128 && count <= 1024 && after == 0 && crtc->width && crtc->height) {
                    ss::Sha256 hash; hash.update(bytes, static_cast<std::size_t>(count));
                    result.push_back({crtc->x, crtc->y, static_cast<int>(crtc->width), static_cast<int>(crtc->height),
                                      "edid:" + ss::Sha256::hex(hash.finish()), std::string(output->name, static_cast<std::size_t>(output->nameLen))});
                }
                if (bytes) XFreeFn(bytes);
                if (crtc) XRRFreeCrtcInfoFn(crtc);
            }
            XRRFreeOutputInfoFn(output);
        }
    }
    if (resources) XRRFreeScreenResourcesFn(resources);
    XCloseDisplayFn(display);
#endif
    return result;
}
}
