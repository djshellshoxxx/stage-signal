// Contract: native asynchronous file dialogs (app-adapters 11, APP-AD-13). Owner: engine-adapters-desktop.
// Never modal. The callback runs on the message thread; the caller validates results (security core).
#pragma once
#include <functional>
#include <string>

namespace ss::app {

enum class DialogKind : unsigned char { OpenShow, SaveShow, ChooseFolder, OpenMedia, SaveReport };

class IFileDialogs {
public:
    virtual ~IFileDialogs() = default;
    // `path` is empty when the operator cancelled.
    virtual void launch(DialogKind kind, const std::string& startDir, std::function<void(const std::string& path)> done) = 0;
};

class ScriptedFileDialogs final : public IFileDialogs {
public:
    void launch(DialogKind, const std::string&, std::function<void(const std::string&)> done) override { done(nextResult); }
    std::string nextResult;
};

}  // namespace ss::app
