// Help tab (app-ui 5.7). Owner: ui-other. Topic list on the left, text on the right. Topics live in Help/HelpTopics.h.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "UI/Help/HelpTopics.h"
#include "UI/PanelBase.h"

namespace ss::app {

class HelpPanel final : public PanelBase {
public:
    explicit HelpPanel(UiModel& model);
    ~HelpPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // Topic titles, in display order (exposed for the self-tests).
    std::vector<std::string> titles() const;
    int topicCount() const { return static_cast<int>(topics_.size()); }

private:
    class TopicModel;
    void showTopic(int index);

    std::vector<help::Topic> topics_;
    std::unique_ptr<TopicModel> model_list_;
    juce::ListBox list_;
    juce::TextEditor text_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HelpPanel)
};

}  // namespace ss::app
