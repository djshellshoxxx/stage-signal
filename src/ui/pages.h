// Tab page factories (one translation unit each). Pages only talk to AppController through ui::A.ctl.
#pragma once
#include "ui/ui_page.h"
#include <memory>

namespace ss { namespace ui {
std::unique_ptr<Page> makeLivePage();
std::unique_ptr<Page> makeSetupPage();
std::unique_ptr<Page> makeMediaPage();
std::unique_ptr<Page> makeMidiLightPage();
std::unique_ptr<Page> makeRecordPage();
std::unique_ptr<Page> makeDiagPage();

// helpers shared by pages
std::string showOutputList(const std::vector<std::string>& v);
void applyDocEdit(const std::function<void(show::ShowDocument&)>& edit);   // copy document(), edit, applyDocument(), report errors
std::vector<std::string> sceneNames(const show::ShowDocument& d);
void showHelp(int topicIndex = -1);        // help.cpp
void showAbout(HWND owner);                // help.cpp
void showHelpTopic(const std::string& key);   // opens the help topic whose title contains key
}} // namespace ss::ui
