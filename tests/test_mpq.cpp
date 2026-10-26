#include "Mpq.hpp"

#include <cstdio>
#include <vector>

int main() {
    reclient::ArchiveSet archives(R"(D:\home\World of Warcraft)");
    const char* names[] = {
        "Interface\\FrameXML\\FrameXML.toc",
        "DBFilesClient\\AreaTable.dbc",
        "World\\Maps\\Azeroth\\Azeroth.wdt",
        "Interface\\Glues\\Models\\UI_MainMenu\\UI_MainMenu.m2",
    };
    int found = 0;
    for (const char* name : names) {
        std::vector<uint8_t> data;
        if (archives.read(name, data) && !data.empty()) ++found;
    }
    std::printf("found %d\n", found);
    return found >= 2 ? 0 : 1;
}
