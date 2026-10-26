#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace reclient {

class ArchiveSet;

struct UiWidget {
    int id = 0;
    int parent = 0;
    std::string type;
    std::string name;
    std::string text;
    std::string art;
    float x = 0, y = 0, w = 160, h = 28;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    bool shown = true;
    bool mouse = true;
    bool sized = false;
    unsigned color = 0xE0181612;
};

// Lua 5.1 FrameXML subset. Stock TOC files are loaded from the user's MPQs.
// Unknown Lua calls are skipped per file so one missing API does not stop the rest.
class FrameXml {
public:
    FrameXml();
    ~FrameXml();
    FrameXml(const FrameXml&) = delete;
    FrameXml& operator=(const FrameXml&) = delete;

    using Chatter = std::function<void(const std::string&)>;
    void setChatter(Chatter chatter);
    void setPicker(std::function<void(int)> picker);
    using Sounder = std::function<void(const std::string&)>;
    void setSounder(Sounder sounder);
    void play(const std::string& name);
    void setPlayerName(std::string name);
    void say(const std::string& text);
    void chooseGossip(int index);

    bool loadXml(const std::string& xml, const std::string& label);
    bool loadLua(const std::string& source, const std::string& label);
    int loadToc(const ArchiveSet& archives, const std::string& tocPath);
    // Reads the stock interface from the client archives and from Interface\AddOns.
    // Blizzard_* folders on disk hold signatures; the Lua itself is loaded from the archives.
    // AddOns marked LoadOnDemand stay unloaded until a script calls LoadAddOn.
    void useClient(const ArchiveSet* archives, std::string root);
    int loadStockUi();
    int loadAddOn(const std::string& name);
    int numAddOns() const { return int(addons_.size()); }
    bool addOnInfo(int index, std::string& name, std::string& title, bool& loaded, bool& demand) const;
    bool isAddOnLoaded(const std::string& name) const;

    void layout(float screenW, float screenH);
    bool layoutPending() const;
    int widgetCount() const;
    void fire(const std::string& event);
    // Stock GossipFrame and ChatFrame. The scripts are the client's; these fill the
    // globals and widget methods those scripts already call.
    void showGossip(const std::string& npc, const std::string& greeting,
                    const std::vector<std::pair<std::string, std::string>>& options);
    void hideGossip();
    void chatLine(uint8_t type, const std::string& sender, const std::string& text, const std::string& channel,
                  uint32_t language);
    void setChannels(const std::vector<std::string>& names);
    void setNamedText(const std::string& name, const std::string& text);
    std::string prepareDialogs();
    std::string script(const std::string& source);
    std::string lastError() const;
    void setShown(const std::string& name, bool shown);
    // Left button down and up. Positions are screen pixels, origin at the top left.
    bool click(float x, float y);
    bool release(float x, float y);

    const std::vector<UiWidget>& widgets() const { return draw_; }
    uint32_t revision() const { return revision_; }
    const std::vector<std::string>& errors() const { return errors_; }
    std::string textOf(const std::string& name) const;

    struct Widget;
    struct Body;

private:
    struct AddonRec {
        std::string name;
        std::string title;
        bool demand = false;
        bool loaded = false;
    };
    Body* body_ = nullptr;
    const ArchiveSet* archives_ = nullptr;
    std::string root_;
    std::vector<AddonRec> addons_;
    std::unordered_set<std::string> loadedFiles_;
    Chatter chatter_;
    std::function<void(int)> picker_;
    Sounder sounder_;
    std::string player_;
    int pressed_ = 0;
    std::vector<UiWidget> draw_;
    std::vector<std::string> errors_;
    uint32_t revision_ = 1;
    void publish();
    void note(const std::string& label, const std::string& error);
    bool readUi(const std::string& path, std::vector<uint8_t>& out) const;
    int loadListed(const std::string& tocPath);
    void scanAddons();
    void rememberAddon(const std::string& name, const std::string& tocText);
};

}  // namespace reclient
