#include "FrameXml.hpp"
#include "Mpq.hpp"
#include "World.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

int main() {
    reclient::FrameXml ui;
    std::string said;
    ui.setChatter([&](const std::string& text) { said = text; });
    const char* xml =
        "<Ui>"
        "<Button name=\"Login\">"
        "<Size><AbsDimension x=\"200\" y=\"40\"/></Size>"
        "<Anchors><Anchor point=\"CENTER\"><Offset><AbsDimension x=\"0\" y=\"0\"/></Offset></Anchor></Anchors>"
        "<Layers><Layer><FontString name=\"$parentText\" text=\"Login\"/></Layer></Layers>"
        "<Scripts>"
        "<OnLoad>LoginText:SetText(\"Ready\")</OnLoad>"
        "<OnClick>SendChatMessage(\"hello\")</OnClick>"
        "</Scripts>"
        "</Button>"
        "<Button name=\"Bar\">"
        "<Size><AbsDimension x=\"100\" y=\"20\"/></Size>"
        "<Anchors><Anchor point=\"BOTTOMLEFT\" relativePoint=\"BOTTOMLEFT\">"
        "<Offset><AbsDimension x=\"8\" y=\"12\"/></Offset></Anchor></Anchors>"
        "</Button>"
        "<Button name=\"Tpl\" virtual=\"true\">"
        "<Size><AbsDimension x=\"80\" y=\"20\"/></Size>"
        "<Anchors><Anchor point=\"TOPLEFT\"><Offset><AbsDimension x=\"10\" y=\"-30\"/></Offset></Anchor></Anchors>"
        "<NormalTexture file=\"Interface\\Buttons\\UI-Panel-Button-Up\"/>"
        "</Button>"
        "<Button name=\"Use\" inherits=\"Tpl\"/>"
        "</Ui>";
    if (!ui.loadXml(xml, "fixture")) {
        std::printf("xml did not create a widget\n");
        return 1;
    }
    ui.layout(800, 600);
    if (ui.textOf("LoginText") != "Ready") {
        std::printf("onload text '%s'\n", ui.textOf("LoginText").c_str());
        return 1;
    }
    bool hit = false;
    for (const reclient::UiWidget& widget : ui.widgets()) {
        if (widget.name == "Login") {
            hit = ui.click(widget.x + 4, widget.y + 4);
        }
    }
    if (!hit || said != "hello") {
        std::printf("click %d said '%s'\n", hit ? 1 : 0, said.c_str());
        return 1;
    }
    bool placed = false;
    for (const reclient::UiWidget& widget : ui.widgets()) {
        if (widget.name == "Bar") {
            placed = std::fabs(widget.x - 8.f) < 0.1f && std::fabs(widget.y - 568.f) < 0.1f;
            if (!placed) std::printf("bar at %.1f %.1f\n", widget.x, widget.y);
        }
        if (widget.name == "Use") {
            const bool spot = std::fabs(widget.x - 10.f) < 0.1f && std::fabs(widget.y - 30.f) < 0.1f &&
                              std::fabs(widget.w - 80.f) < 0.1f && widget.art.find("UI-Panel-Button-Up") != std::string::npos;
            if (!spot) {
                std::printf("use at %.1f %.1f %.1f art '%s'\n", widget.x, widget.y, widget.w, widget.art.c_str());
                return 1;
            }
        }
    }
    if (!placed) return 1;
    ui.fire("PLAYER_LOGIN");
    std::printf("ui widgets %zu\n", ui.widgets().size());

    reclient::Movement move;
    move.flags = reclient::kMoveForward;
    move.x = -8949.95f;
    move.y = -132.49f;
    move.z = 83.5f;
    move.o = 0.4f;
    reclient::ByteBuffer encoded;
    reclient::writeMovement(encoded, move);
    reclient::ByteBuffer decoded(encoded.raw());
    reclient::Movement back;
    reclient::readMovement(decoded, back);
    if (std::fabs(back.x - move.x) > 0.001f || std::fabs(back.y - move.y) > 0.001f || back.flags != move.flags) {
        std::printf("movement roundtrip failed\n");
        return 1;
    }

    reclient::ByteBuffer chars;
    chars.write<uint8_t>(1);
    chars.write<uint64_t>(42);
    chars.writeCString("Reclient");
    for (int i = 0; i < 8; ++i) chars.write<uint8_t>(1);
    chars.write<uint8_t>(1);
    chars.write<uint32_t>(12);
    chars.write<uint32_t>(0);
    chars.write<float>(-8949.95f);
    chars.write<float>(-132.49f);
    chars.write<float>(83.5f);
    chars.write<uint32_t>(0);
    chars.write<uint32_t>(0);
    chars.write<uint8_t>(0);
    chars.write<uint32_t>(0);
    chars.write<uint32_t>(0);
    chars.write<uint32_t>(0);
    for (int slot = 0; slot < 20; ++slot) {
        chars.write<uint32_t>(0);
        chars.write<uint8_t>(0);
        chars.write<uint32_t>(0);
    }
    const std::vector<reclient::Character> list = reclient::parseCharEnum(chars);
    if (list.size() != 1 || list[0].name != "Reclient" || list[0].guid != 42) {
        std::printf("char enum failed\n");
        return 1;
    }
    std::printf("movement and character list ok\n");

    reclient::ArchiveSet archives(R"(D:\home\World of Warcraft)");
    reclient::FrameXml stock;
    stock.useClient(&archives, R"(D:\home\World of Warcraft)");
    const int files = stock.loadStockUi();
    const int auction = stock.loadAddOn("Blizzard_AuctionUI");
    const auto layoutStart = std::chrono::steady_clock::now();
    stock.layout(1280, 720);
    stock.layout(1280, 720);
    const auto layoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - layoutStart).count();
    for (const reclient::UiWidget& widget : stock.widgets()) {
        if (widget.name == "MinimapCluster" && (widget.x < 1000.f || widget.y > 40.f || widget.w < 180.f)) {
            std::printf("minimap cluster at %.0f,%.0f %.0fx%.0f\n", widget.x, widget.y, widget.w, widget.h);
            return 1;
        }
        if (widget.name == "ActionButton1" && widget.x < 100.f) {
            std::printf("action button at %.0f,%.0f\n", widget.x, widget.y);
            return 1;
        }
    }
    std::printf("framexml files %d widgets %zu slots %d layout %lld ms errors %zu addons %d auction %d loaded %d\n", files,
                stock.widgets().size(), stock.widgetCount(), static_cast<long long>(layoutMs), stock.errors().size(), stock.numAddOns(),
                auction, stock.isAddOnLoaded("Blizzard_AuctionUI") ? 1 : 0);
    if (stock.widgetCount() > 40000 || layoutMs > 400) {
        std::printf("interface list built too many frames\n");
        return 1;
    }
    int shown = 0;
    for (const std::string& error : stock.errors()) {
        if (shown++ >= 12) break;
        std::printf("framexml %s\n", error.c_str());
    }
    if (files <= 0 || auction < 0 || !stock.isAddOnLoaded("Blizzard_AuctionUI")) return 1;
    if (stock.loadAddOn("Blizzard_AuctionUI") != 0) {
        std::printf("addon loaded twice\n");
        return 1;
    }
    return 0;
}
