// Help tab text (app-ui 5.7, topics from spec section 19). Owner: ui-other. Plain English, no em dashes, no
// banned copy words (app-ui 3.5). The keyboard map is generated from core/palette, so it cannot drift.
#pragma once
#include <string>
#include <vector>
#include "ss/palette/help.hpp"

#ifndef SS_APP_VERSION
#define SS_APP_VERSION "0.1.0"
#endif

namespace ss::app::help {

struct Topic {
    std::string title;
    std::string body;
};

inline std::vector<Topic> topics(const palette::KeyMap& keymap) {
    return {
        {"First show",
         "1. Open Setup and pick the audio device.\n"
         "2. Set each display's role. Controls should not have an audience route.\n"
         "3. Check Safety. Strobing is off for new shows.\n"
         "4. Open or build the show.\n"
         "5. Run Show Doctor in Diagnostics and fix any blockers.\n"
         "6. Arm lighting only when the room is ready."},
        {"Display roles",
         "Each display has one role: Controls, Audience, Duplicate of another output, Independent or Unused.\n"
         "Identical monitors can look the same to the computer. If the app says it cannot tell them apart, use "
         "Identify to see which is which before the show."},
        {"Gain staging",
         "Set the input trim so loud passages peak below the top of the meter.\n"
         "When the clipping warning shows, lower the trim until it stops. Clipping makes the analysis and the lights "
         "react wrongly."},
        {"BPM and key confidence",
         "Tempo and key are estimates, and the Live tab shows how sure the app is. When an estimate is wrong, tap the "
         "tempo or set a value by hand. Release the override to go back to the estimate."},
        {"Scene modes",
         "Auto picks scenes from the music. Programmed follows the cue list. Hybrid follows the cue list and lets you "
         "override a scene.\n"
         "Pressing a scene pad in Auto switches to Hybrid until you resume."},
        {"Live text",
         "Write the message in Media / Text and preview it. Audience outputs change only when you publish. Clear "
         "removes the published text."},
        {"Camera publication",
         "The camera reaches an audience output only when a scene shows it and the camera is open. Show Doctor warns "
         "when a camera is selected but not available."},
        {"MIDI learn and clock",
         "Learn maps a control on a MIDI device to an action. Clock output stays off until you turn it on. SysEx stays "
         "off in this build."},
        {"Lighting arm, blackout and timeouts",
         "Lighting sends nothing until you arm it, and Arm asks you to confirm.\n"
         "Blackout stops the lights and stays on until you restore them. After a fault, arm again."},
        {"Benchmark interpretation",
         "A benchmark runs for about 75 seconds and shows which features this computer can keep up with. Run it again "
         "after you change the setup. Results older than 30 days are marked old."},
        {"Recorder recovery",
         "If the app stops during a recording, the file is marked recoverable at the next start. Check the last file "
         "before you record again. Recording never starts on its own after a restart."},
        {"Broadcast protocols and credentials",
         "Use RTMPS or SRT when you can. RTMP sends the stream without encryption.\n"
         "The stream key is masked, is never written to logs, and is not shown again after you save it."},
        {"Portable export",
         "Export portable saves the show as one .stagesignal file for another computer. If an import fails, the message "
         "says why, and your current show is unchanged."},
        {"Keyboard map", palette::keyboardHelpText(keymap)},
        {"Sanitised diagnostics",
         "Diagnostics show what the app is doing without secrets, file paths or audio. Support bundles and Show Reports "
         "follow the same rule."},
        {"Troubleshooting",
         "No audio device: Audio device not found. The show keeps running without analysis. Pick a device in Setup.\n"
         "Lights do not respond: check the interface and destination in MIDI / Lighting, then run Show Doctor for the "
         "cause."},
        {"About",
         std::string("StageSignal ") + SS_APP_VERSION + "\n"
         "StageSignal is free software under AGPL-3.0-or-later. The source code is at "
         "https://github.com/djshellshoxxx/stage-signal. Font and asset licences are in the assets folder."},
    };
}

}  // namespace ss::app::help
