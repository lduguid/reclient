#include "FrameXml.hpp"

#include "Mpq.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

extern "C" void reclientBudgetHook(lua_State* L, lua_Debug*) { luaL_error(L, "instruction budget"); }

namespace reclient {
namespace {

struct XmlNode {
    std::string name;
    std::string text;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> kids;
    const std::string* attr(const char* key) const {
        for (const auto& [k, v] : attrs)
            if (k == key) return &v;
        return nullptr;
    }
};

void skipSpace(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
}

bool parseNode(const std::string& s, size_t& i, XmlNode& node);

bool parseAttrs(const std::string& s, size_t& i, XmlNode& node, bool& selfClose) {
    selfClose = false;
    while (i < s.size()) {
        skipSpace(s, i);
        if (i >= s.size()) return false;
        if (s[i] == '>') {
            ++i;
            return true;
        }
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '>') {
            selfClose = true;
            i += 2;
            return true;
        }
        const size_t key = i;
        while (i < s.size() && s[i] != '=' && s[i] != ' ' && s[i] != '\t' && s[i] != '>' && s[i] != '/') ++i;
        std::string name = s.substr(key, i - key);
        skipSpace(s, i);
        if (i >= s.size() || s[i] != '=') {
            if (i == key) ++i;
            continue;
        }
        ++i;
        skipSpace(s, i);
        if (i >= s.size()) return false;
        const char quote = s[i];
        if (quote != '"' && quote != '\'') return false;
        ++i;
        const size_t val = i;
        while (i < s.size() && s[i] != quote) ++i;
        node.attrs.emplace_back(std::move(name), s.substr(val, i - val));
        if (i < s.size()) ++i;
    }
    return false;
}

bool parseNode(const std::string& s, size_t& i, XmlNode& node) {
    skipSpace(s, i);
    if (i + 1 >= s.size() || s[i] != '<') return false;
    if (s.compare(i, 4, "<!--") == 0) {
        const size_t end = s.find("-->", i + 4);
        i = end == std::string::npos ? s.size() : end + 3;
        return true;
    }
    if (s.compare(i, 2, "<?") == 0 || s.compare(i, 2, "<!") == 0) {
        const size_t end = s.find('>', i + 2);
        i = end == std::string::npos ? s.size() : end + 1;
        return true;
    }
    ++i;
    const size_t nameAt = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '>' && s[i] != '/') ++i;
    node.name = s.substr(nameAt, i - nameAt);
    bool selfClose = false;
    if (!parseAttrs(s, i, node, selfClose)) return false;
    if (selfClose) return true;
    if (node.name.size() >= 2 && node.name[0] == 'O' && node.name[1] == 'n') {
        const std::string close = "</" + node.name;
        const size_t end = s.find(close, i);
        if (end == std::string::npos) return false;
        node.text = s.substr(i, end - i);
        const size_t gt = s.find('>', end);
        i = gt == std::string::npos ? s.size() : gt + 1;
        return true;
    }
    std::string text;
    while (i < s.size()) {
        if (s.compare(i, 2, "</") == 0) {
            const size_t end = s.find('>', i + 2);
            i = end == std::string::npos ? s.size() : end + 1;
            node.text = std::move(text);
            return true;
        }
        if (s.compare(i, 9, "<![CDATA[") == 0) {
            const size_t end = s.find("]]>", i + 9);
            if (end == std::string::npos) return false;
            text.append(s, i + 9, end - (i + 9));
            i = end + 3;
            continue;
        }
        if (s[i] == '<') {
            XmlNode child;
            if (!parseNode(s, i, child)) return false;
            if (!child.name.empty()) node.kids.push_back(std::move(child));
            continue;
        }
        text.push_back(s[i++]);
    }
    return false;
}

std::string stripXmlComments(std::string text) {
    size_t at = 0;
    while (at < text.size()) {
        const size_t start = text.find("<!--", at);
        if (start == std::string::npos) break;
        const size_t end = text.find("-->", start + 4);
        if (end == std::string::npos) break;
        text.erase(start, end + 3 - start);
        at = start;
    }
    return text;
}

std::string trimCopy(std::string s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\n' || s[a] == '\r' || s[a] == '\t')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\n' || s[b - 1] == '\r' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

void mergeNode(XmlNode& dst, const XmlNode& src) {
    if (dst.name.empty()) dst.name = src.name;
    for (const auto& [key, value] : src.attrs) {
        if (key == "virtual" || key == "inherits") continue;
        bool found = false;
        for (auto& [have, current] : dst.attrs) {
            if (have == key) {
                current = value;
                found = true;
                break;
            }
        }
        if (!found) dst.attrs.emplace_back(key, value);
    }
    auto take = [&](const char* childName) {
        const XmlNode* incoming = nullptr;
        for (const XmlNode& kid : src.kids)
            if (kid.name == childName) incoming = &kid;
        if (!incoming) return;
        for (XmlNode& kid : dst.kids) {
            if (kid.name == childName) {
                kid = *incoming;
                return;
            }
        }
        dst.kids.push_back(*incoming);
    };
    take("Size");
    take("Anchors");
    take("NormalTexture");
    take("PushedTexture");
    take("DisabledTexture");
    take("HighlightTexture");
    take("Backdrop");
    const XmlNode* scripts = nullptr;
    for (const XmlNode& kid : src.kids)
        if (kid.name == "Scripts") scripts = &kid;
    if (scripts) {
        XmlNode* dest = nullptr;
        for (XmlNode& kid : dst.kids)
            if (kid.name == "Scripts") dest = &kid;
        if (!dest) dst.kids.push_back(*scripts);
        else {
            for (const XmlNode& script : scripts->kids) {
                bool replaced = false;
                for (XmlNode& have : dest->kids) {
                    if (have.name == script.name) {
                        have = script;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) dest->kids.push_back(script);
            }
        }
    }
    for (const XmlNode& kid : src.kids) {
        if (kid.name == "Size" || kid.name == "Anchors" || kid.name == "Scripts" || kid.name == "NormalTexture" ||
            kid.name == "PushedTexture" || kid.name == "DisabledTexture" || kid.name == "HighlightTexture" ||
            kid.name == "Backdrop")
            continue;
        dst.kids.push_back(kid);
    }
}

XmlNode flatten(const XmlNode& el, const std::unordered_map<std::string, XmlNode>& templates, int depth) {
    XmlNode out;
    out.name = el.name;
    if (depth < 6) {
        if (const std::string* inherits = el.attr("inherits")) {
            size_t i = 0;
            while (i < inherits->size()) {
                while (i < inherits->size() && ((*inherits)[i] == ' ' || (*inherits)[i] == ',')) ++i;
                if (i >= inherits->size()) break;
                size_t j = i;
                while (j < inherits->size() && (*inherits)[j] != ',') ++j;
                const std::string name = trimCopy(inherits->substr(i, j - i));
                i = j + 1;
                const auto found = templates.find(name);
                if (found == templates.end()) continue;
                mergeNode(out, flatten(found->second, templates, depth + 1));
            }
        }
    }
    mergeNode(out, el);
    return out;
}

}  // namespace

struct FrameXml::Widget {
    int id = 0;
    int parent = 0;
    std::string type;
    std::string name;
    std::string text;
    std::string art;
    // One attachment of this frame's point to another frame's point.
    // Offsets are the interface's own axes: +x right, +y up.
    struct Anchor {
        std::string point = "TOPLEFT";
        std::string relPoint;
        std::string relName;
        int relId = -2;
        float x = 0, y = 0;
    };
    std::vector<Anchor> anchors;
    std::string parentName;
    float w = 0, h = 0;
    float x = 0, y = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    bool shown = true;
    bool mouse = false;
    bool abs = false;
    int scriptId = 0;
    unsigned color = 0xE0181612;
    std::vector<std::string> events;
    int onClick = LUA_NOREF;
    int onMouseDown = LUA_NOREF;
    int onMouseUp = LUA_NOREF;
    int onEvent = LUA_NOREF;
    int onLoad = LUA_NOREF;
};

struct FrameXml::Body {
    lua_State* L = nullptr;
    std::vector<Widget> widgets{Widget{}};
    std::unordered_map<std::string, int> names;
    std::unordered_map<std::string, XmlNode> templates;
    float screenW = 1280, screenH = 720;
    bool layoutDirty = true;
    bool textDirty = false;
    bool keepArgs = false;
    bool argsLive = false;
    struct FiredArg {
        bool number = false;
        double n = 0;
        std::string text;
    };
    FiredArg fired[11];
    std::string gossipText;
    std::string gossipNpc;
    std::vector<std::pair<std::string, std::string>> gossipOptions;
    std::vector<std::string> channels;
    std::string scriptError;
    FrameXml* owner = nullptr;
};

namespace {

FrameXml::Body* bodyOf(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "reclient.ui");
    auto* body = static_cast<FrameXml::Body*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return body;
}

int idOf(lua_State* L, int index) {
    if (!lua_isuserdata(L, index)) return 0;
    return *static_cast<int*>(lua_touserdata(L, index));
}

void widgetFields(lua_State* L, int id) {
    lua_getfield(L, LUA_REGISTRYINDEX, "reclient.fields");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "reclient.fields");
    }
    lua_rawgeti(L, -1, id);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, id);
    }
    lua_remove(L, -2);
}

int lNewIndex(lua_State* L) {
    const int id = idOf(L, 1);
    if (id <= 0) return 0;
    widgetFields(L, id);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 3);
    lua_settable(L, -3);
    lua_pop(L, 1);
    return 0;
}

void pushWidget(lua_State* L, int id) {
    auto* ud = static_cast<int*>(lua_newuserdata(L, sizeof(int)));
    *ud = id;
    luaL_getmetatable(L, "Widget");
    lua_setmetatable(L, -2);
    if (id > 0) {
        FrameXml::Body* body = bodyOf(L);
        if (body && id < int(body->widgets.size()) && !body->widgets[id].name.empty())
            lua_setglobal(L, body->widgets[id].name.c_str());
        else
            lua_pop(L, 1);
    }
}

void pushWidgetRet(lua_State* L, int id) {
    auto* ud = static_cast<int*>(lua_newuserdata(L, sizeof(int)));
    *ud = id;
    luaL_getmetatable(L, "Widget");
    lua_setmetatable(L, -2);
}

void takeTemplateSize(FrameXml::Body* body, FrameXml::Widget& widget, const XmlNode& el, int depth) {
    if (depth < 6) {
        if (const std::string* inherits = el.attr("inherits")) {
            size_t i = 0;
            while (i < inherits->size()) {
                while (i < inherits->size() && ((*inherits)[i] == ' ' || (*inherits)[i] == ',')) ++i;
                if (i >= inherits->size()) break;
                size_t j = i;
                while (j < inherits->size() && (*inherits)[j] != ',') ++j;
                const std::string name = trimCopy(inherits->substr(i, j - i));
                i = j + 1;
                const auto found = body->templates.find(name);
                if (found != body->templates.end()) takeTemplateSize(body, widget, found->second, depth + 1);
            }
        }
    }
    for (const XmlNode& child : el.kids) {
        if (child.name != "Size") continue;
        for (const XmlNode& dim : child.kids) {
            if (dim.name != "AbsDimension") continue;
            if (const std::string* x = dim.attr("x")) widget.w = std::strtof(x->c_str(), nullptr);
            if (const std::string* y = dim.attr("y")) widget.h = std::strtof(y->c_str(), nullptr);
            widget.abs = true;
        }
    }
}

void applyCreateTemplate(FrameXml::Body* body, FrameXml::Widget& widget, const char* inherits) {
    if (!inherits || !inherits[0]) return;
    size_t i = 0;
    const std::string spec = inherits;
    while (i < spec.size()) {
        while (i < spec.size() && (spec[i] == ' ' || spec[i] == ',')) ++i;
        if (i >= spec.size()) break;
        size_t j = i;
        while (j < spec.size() && spec[j] != ',') ++j;
        const std::string name = trimCopy(spec.substr(i, j - i));
        i = j + 1;
        const auto found = body->templates.find(name);
        if (found == body->templates.end()) continue;
        takeTemplateSize(body, widget, found->second, 0);
    }
}

int lCreateFrame(lua_State* L) {
    FrameXml::Body* body = bodyOf(L);
    if (!body) return 0;
    FrameXml::Widget widget;
    widget.id = int(body->widgets.size());
    widget.type = lua_tostring(L, 1) ? lua_tostring(L, 1) : "Frame";
    if (lua_tostring(L, 2)) widget.name = lua_tostring(L, 2);
    if (lua_type(L, 3) == LUA_TUSERDATA) widget.parent = idOf(L, 3);
    widget.mouse = widget.type == "Button" || widget.type == "CheckButton" || widget.type == "EditBox";
    if (lua_isstring(L, 4)) applyCreateTemplate(body, widget, lua_tostring(L, 4));
    body->widgets.push_back(std::move(widget));
    const int id = body->widgets.back().id;
    if (!body->widgets[id].name.empty()) body->names[body->widgets[id].name] = id;
    body->layoutDirty = true;
    pushWidget(L, id);
    pushWidgetRet(L, id);
    return 1;
}

int lSendChat(lua_State* L) {
    FrameXml::Body* body = bodyOf(L);
    const char* text = lua_tostring(L, 1);
    const char* kind = lua_tostring(L, 2);
    const char* channel = lua_isstring(L, 4) ? lua_tostring(L, 4) : "";
    if (body && body->owner && text) {
        if (text[0] == '/') {
            body->owner->say(text);
            return 0;
        }
        std::string line = text;
        if (kind && std::string(kind) == "CHANNEL") {
            std::string name = channel ? channel : "";
            char* end = nullptr;
            const long n = std::strtol(name.c_str(), &end, 10);
            if (end != name.c_str() && end && *end == '\0' && n >= 1 && size_t(n) <= body->channels.size())
                name = body->channels[size_t(n - 1)];
            line = std::string("channel\t") + name + "|" + text;
        } else if (kind && std::string(kind) == "YELL")
            line = std::string("yell\t") + text;
        else if (kind)
            line = std::string("say\t") + text;
        body->owner->say(line);
    }
    return 0;
}

int lPlaySound(lua_State* L) {
    FrameXml::Body* body = bodyOf(L);
    const char* name = lua_tostring(L, 1);
    if (body && body->owner && name) body->owner->play(name);
    return 0;
}

unsigned luaBits(lua_State* L, int index) { return static_cast<unsigned>(lua_tonumber(L, index)); }

int lBand(lua_State* L) {
    unsigned value = lua_gettop(L) > 0 ? luaBits(L, 1) : 0;
    for (int i = 2; i <= lua_gettop(L); ++i) value &= luaBits(L, i);
    lua_pushnumber(L, static_cast<lua_Number>(value));
    return 1;
}

int lBor(lua_State* L) {
    unsigned value = 0;
    for (int i = 1; i <= lua_gettop(L); ++i) value |= luaBits(L, i);
    lua_pushnumber(L, static_cast<lua_Number>(value));
    return 1;
}

int lBxor(lua_State* L) {
    unsigned value = 0;
    for (int i = 1; i <= lua_gettop(L); ++i) value ^= luaBits(L, i);
    lua_pushnumber(L, static_cast<lua_Number>(value));
    return 1;
}

int lBnot(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(static_cast<uint32_t>(~luaBits(L, 1))));
    return 1;
}

int lLshift(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(luaBits(L, 1) << (luaBits(L, 2) & 31)));
    return 1;
}

int lRshift(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(luaBits(L, 1) >> (luaBits(L, 2) & 31)));
    return 1;
}

int lLoadAddOn(lua_State* L) {
    FrameXml::Body* body = bodyOf(L);
    const char* name = lua_tostring(L, 1);
    if (!body || !body->owner || !name) {
        lua_pushnil(L);
        lua_pushstring(L, "MISSING");
        return 2;
    }
    const int files = body->owner->loadAddOn(name);
    if (files < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "MISSING");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int lIsAddOnLoaded(lua_State* L) {
    FrameXml::Body* body = bodyOf(L);
    const char* name = lua_tostring(L, 1);
    lua_pushboolean(L, body && body->owner && name && body->owner->isAddOnLoaded(name));
    return 1;
}

int lGetNumAddOns(lua_State* L) {
    FrameXml::Body* body = bodyOf(L);
    lua_pushnumber(L, body && body->owner ? body->owner->numAddOns() : 0);
    return 1;
}

int lGetAddOnInfo(lua_State* L) {
    FrameXml::Body* body = bodyOf(L);
    std::string name;
    std::string title;
    bool loaded = false;
    bool demand = false;
    int index = 0;
    if (lua_isnumber(L, 1)) index = int(lua_tointeger(L, 1));
    const bool found = body && body->owner && index > 0 && body->owner->addOnInfo(index, name, title, loaded, demand);
    if (!found) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, name.c_str());
    lua_pushstring(L, title.c_str());
    lua_pushstring(L, "");
    lua_pushboolean(L, 1);
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    lua_pushstring(L, demand ? "DEMAND_LOADED" : "SECURE");
    return 7;
}

int lGetAddOnMetadata(lua_State* L) {
    FrameXml::Body* body = bodyOf(L);
    const char* field = lua_tostring(L, 2);
    std::string name = lua_tostring(L, 1) ? lua_tostring(L, 1) : "";
    std::string title;
    bool loaded = false;
    bool demand = false;
    bool found = false;
    if (body && body->owner) {
        for (int i = 1; i <= body->owner->numAddOns(); ++i) {
            std::string addon;
            if (!body->owner->addOnInfo(i, addon, title, loaded, demand)) continue;
            if (addon == name) {
                found = true;
                break;
            }
        }
    }
    if (!found || !field) {
        lua_pushnil(L);
        return 1;
    }
    if (std::strcmp(field, "Title") == 0) lua_pushstring(L, title.c_str());
    else if (std::strcmp(field, "X-LoadOnDemand") == 0) lua_pushstring(L, demand ? "1" : "0");
    else lua_pushnil(L);
    return 1;
}

int lUnitName(lua_State* L) {
    const char* who = lua_tostring(L, 1);
    FrameXml::Body* body = bodyOf(L);
    if (who && std::strcmp(who, "npc") == 0) {
        const char* name = body && !body->gossipNpc.empty() ? body->gossipNpc.c_str() : "npc";
        lua_pushstring(L, name);
        return 1;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, "reclient.player");
    if (lua_isstring(L, -1)) return 1;
    lua_pop(L, 1);
    lua_pushstring(L, "Player");
    return 1;
}

int lUnitExists(lua_State* L) {
    const char* who = lua_tostring(L, 1);
    FrameXml::Body* body = bodyOf(L);
    if (who && std::strcmp(who, "npc") == 0) {
        lua_pushboolean(L, body && !body->gossipNpc.empty());
        return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int lGetGossipText(lua_State* L) {
    FrameXml::Body* body = bodyOf(L);
    lua_pushstring(L, body ? body->gossipText.c_str() : "");
    return 1;
}

int lGetNumGossipOptions(lua_State* L) {
    FrameXml::Body* body = bodyOf(L);
    lua_pushnumber(L, body ? int(body->gossipOptions.size()) : 0);
    return 1;
}

int lGetGossipOptions(lua_State* L) {
    FrameXml::Body* body = bodyOf(L);
    if (!body) return 0;
    for (const auto& option : body->gossipOptions) {
        lua_pushstring(L, option.first.c_str());
        lua_pushstring(L, option.second.c_str());
    }
    return int(body->gossipOptions.size() * 2);
}

int lNoGossip(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

int lNoList(lua_State*) { return 0; }

int lSelectGossip(lua_State* L) {
    FrameXml::Body* body = bodyOf(L);
    if (body && body->owner) body->owner->chooseGossip(int(lua_tointeger(L, 1)) - 1);
    return 0;
}

int lCloseGossip(lua_State* L) {
    FrameXml::Body* body = bodyOf(L);
    if (!body) return 0;
    const auto found = body->names.find("GossipFrame");
    if (found != body->names.end() && found->second > 0 && found->second < int(body->widgets.size())) {
        body->widgets[found->second].shown = false;
        body->layoutDirty = true;
    }
    return 0;
}

int lGetDefaultLanguage(lua_State* L) {
    lua_pushstring(L, "Common");
    return 1;
}

int lGetChatTypeIndex(lua_State* L) {
    lua_pushnumber(L, 1);
    return 1;
}

int lGetChatWindowInfo(lua_State* L) {
    const int id = int(lua_tointeger(L, 1));
    lua_pushstring(L, "Chat Frame");
    lua_pushnumber(L, 14);
    lua_pushnumber(L, 1);
    lua_pushnumber(L, 1);
    lua_pushnumber(L, 1);
    lua_pushnumber(L, 1);
    lua_pushnumber(L, id == 1 ? 1 : 0);
    lua_pushnumber(L, 1);
    return 8;
}

int lGetChatWindowMessages(lua_State* L) {
    if (int(lua_tointeger(L, 1)) != 1) return 0;
    const char* groups[] = {"SYSTEM", "SAY",     "EMOTE",    "YELL",          "WHISPER",
                            "PARTY",  "GUILD",   "MONSTER_SAY", "MONSTER_YELL", "MONSTER_EMOTE",
                            "MONSTER_WHISPER", "CHANNEL"};
    for (const char* group : groups) lua_pushstring(L, group);
    return 12;
}

int lGetChatWindowChannels(lua_State*) { return 0; }

std::string foldName(std::string value) {
    for (char& c : value) c = char(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

int lGetChannelName(lua_State* L) {
    FrameXml::Body* body = bodyOf(L);
    const char* which = lua_tostring(L, 1);
    if (!body || !which || body->channels.empty()) {
        lua_pushnumber(L, 0);
        return 1;
    }
    char* stop = nullptr;
    const long n = std::strtol(which, &stop, 10);
    if (stop != which && stop && *stop == '\0' && n >= 1 && size_t(n) <= body->channels.size()) {
        lua_pushnumber(L, n);
        lua_pushstring(L, body->channels[size_t(n - 1)].c_str());
        lua_pushnumber(L, 0);
        return 3;
    }
    const std::string want = foldName(which);
    for (size_t i = 0; i < body->channels.size(); ++i) {
        if (foldName(body->channels[i]) == want) {
            lua_pushnumber(L, int(i + 1));
            lua_pushstring(L, body->channels[i].c_str());
            lua_pushnumber(L, 0);
            return 3;
        }
    }
    lua_pushnumber(L, 0);
    return 1;
}

int lIndex(lua_State* L) {
    FrameXml::Body* body = bodyOf(L);
    const int id = idOf(L, 1);
    const char* key = lua_tostring(L, 2);
    if (!body || id <= 0 || id >= int(body->widgets.size()) || !key) return 0;
    widgetFields(L, id);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2);
        return 1;
    }
    lua_pop(L, 2);
    FrameXml::Widget& widget = body->widgets[id];
    if (std::strcmp(key, "SetText") == 0 || std::strcmp(key, "SetPoint") == 0 || std::strcmp(key, "SetWidth") == 0 ||
        std::strcmp(key, "SetHeight") == 0 || std::strcmp(key, "SetSize") == 0 || std::strcmp(key, "Show") == 0 ||
        std::strcmp(key, "Hide") == 0 || std::strcmp(key, "RegisterEvent") == 0 || std::strcmp(key, "SetScript") == 0 ||
        std::strcmp(key, "EnableMouse") == 0 || std::strcmp(key, "GetName") == 0 || std::strcmp(key, "SetParent") == 0 ||
        std::strcmp(key, "ClearAllPoints") == 0 || std::strcmp(key, "SetAllPoints") == 0 || std::strcmp(key, "GetWidth") == 0 ||
        std::strcmp(key, "GetHeight") == 0 || std::strcmp(key, "GetLeft") == 0 || std::strcmp(key, "GetRight") == 0 ||
        std::strcmp(key, "GetTop") == 0 || std::strcmp(key, "GetBottom") == 0 || std::strcmp(key, "GetParent") == 0 ||
        std::strcmp(key, "IsShown") == 0 || std::strcmp(key, "IsVisible") == 0 || std::strcmp(key, "GetText") == 0 ||
        std::strcmp(key, "SetFormattedText") == 0 || std::strcmp(key, "GetTextHeight") == 0 ||
        std::strcmp(key, "SetID") == 0 || std::strcmp(key, "GetID") == 0 || std::strcmp(key, "SetTexture") == 0 ||
        std::strcmp(key, "AddMessage") == 0 || std::strcmp(key, "GetFont") == 0 || std::strcmp(key, "GetFrameLevel") == 0 ||
        std::strcmp(key, "SetAttribute") == 0 || std::strcmp(key, "GetAttribute") == 0) {
        lua_pushvalue(L, 2);
        lua_pushcclosure(
            L,
            [](lua_State* state) -> int {
                FrameXml::Body* inner = bodyOf(state);
                const char* method = lua_tostring(state, lua_upvalueindex(1));
                const int self = idOf(state, 1);
                if (!inner || !method || self <= 0 || self >= int(inner->widgets.size())) return 0;
                FrameXml::Widget& w = inner->widgets[self];
                if (std::strcmp(method, "SetText") == 0) {
                    w.text = lua_tostring(state, 2) ? lua_tostring(state, 2) : "";
                    inner->textDirty = true;
                } else if (std::strcmp(method, "GetText") == 0) {
                    lua_pushstring(state, w.text.c_str());
                    return 1;
                } else if (std::strcmp(method, "SetFormattedText") == 0) {
                    const int top = lua_gettop(state);
                    lua_getglobal(state, "string");
                    lua_getfield(state, -1, "format");
                    lua_remove(state, -2);
                    int nargs = 0;
                    for (int i = 2; i <= top; ++i) {
                        lua_pushvalue(state, i);
                        ++nargs;
                    }
                    if (lua_pcall(state, nargs, 1, 0) == 0)
                        w.text = lua_tostring(state, -1) ? lua_tostring(state, -1) : "";
                    lua_settop(state, top);
                    inner->textDirty = true;
                } else if (std::strcmp(method, "GetTextHeight") == 0) {
                    lua_pushnumber(state, 14);
                    return 1;
                } else if (std::strcmp(method, "SetID") == 0) {
                    w.scriptId = int(lua_tointeger(state, 2));
                } else if (std::strcmp(method, "GetID") == 0) {
                    lua_pushnumber(state, w.scriptId);
                    return 1;
                } else if (std::strcmp(method, "SetTexture") == 0) {
                    if (lua_isstring(state, 2)) w.art = lua_tostring(state, 2);
                    inner->textDirty = true;
                } else if (std::strcmp(method, "AddMessage") == 0) {
                    std::string line = lua_tostring(state, 2) ? lua_tostring(state, 2) : "";
                    std::string clean;
                    for (size_t i = 0; i < line.size(); ++i) {
                        if (line[i] != '|' || i + 1 >= line.size()) {
                            clean.push_back(line[i]);
                            continue;
                        }
                        const char mark = line[i + 1];
                        if (mark == 'h' || mark == 'r') {
                            ++i;
                            continue;
                        }
                        if (mark == 'c' && i + 9 < line.size()) {
                            i += 9;
                            continue;
                        }
                        if (mark == 'H' || mark == 'T') {
                            const std::string closer = mark == 'H' ? "|h" : "|t";
                            const size_t end = line.find(closer, i + 2);
                            if (end == std::string::npos) continue;
                            i = end + 1;
                            continue;
                        }
                        clean.push_back(line[i]);
                    }
                    if (!w.text.empty()) w.text.push_back('\n');
                    w.text += clean;
                    int breaks = 0;
                    for (char c : w.text)
                        if (c == '\n') ++breaks;
                    while (breaks > 11) {
                        const size_t cut = w.text.find('\n');
                        if (cut == std::string::npos) break;
                        w.text.erase(0, cut + 1);
                        --breaks;
                    }
                    inner->textDirty = true;
                } else if (std::strcmp(method, "GetFont") == 0) {
                    lua_pushstring(state, "");
                    lua_pushnumber(state, 14);
                    lua_pushstring(state, "");
                    return 3;
                } else if (std::strcmp(method, "GetFrameLevel") == 0) {
                    lua_pushnumber(state, 1);
                    return 1;
                } else if (std::strcmp(method, "SetAttribute") == 0) {
                    widgetFields(state, self);
                    lua_pushvalue(state, 2);
                    lua_pushvalue(state, 3);
                    lua_rawset(state, -3);
                    lua_pop(state, 1);
                } else if (std::strcmp(method, "GetAttribute") == 0) {
                    widgetFields(state, self);
                    lua_pushvalue(state, 2);
                    lua_rawget(state, -2);
                    lua_remove(state, -2);
                    return 1;
                } else if (std::strcmp(method, "SetWidth") == 0) {
                    w.w = float(lua_tonumber(state, 2));
                    w.abs = true;
                    inner->layoutDirty = true;
                } else if (std::strcmp(method, "SetHeight") == 0) {
                    w.h = float(lua_tonumber(state, 2));
                    w.abs = true;
                    inner->layoutDirty = true;
                } else if (std::strcmp(method, "SetSize") == 0) {
                    w.w = float(lua_tonumber(state, 2));
                    w.h = float(lua_tonumber(state, 3));
                    w.abs = true;
                    inner->layoutDirty = true;
                } else if (std::strcmp(method, "SetPoint") == 0) {
                    FrameXml::Widget::Anchor anchor;
                    anchor.point = lua_tostring(state, 2) ? lua_tostring(state, 2) : "TOPLEFT";
                    anchor.relPoint = anchor.point;
                    const int n = lua_gettop(state);
                    if (n >= 4 && lua_isnumber(state, 3) && lua_isnumber(state, 4)) {
                        anchor.x = float(lua_tonumber(state, 3));
                        anchor.y = float(lua_tonumber(state, 4));
                    } else if (n >= 3) {
                        if (lua_isuserdata(state, 3)) anchor.relId = idOf(state, 3);
                        else if (lua_isstring(state, 3)) anchor.relName = lua_tostring(state, 3);
                        int numAt = 4;
                        if (n >= 4 && lua_isstring(state, 4) && !lua_isnumber(state, 4)) {
                            anchor.relPoint = lua_tostring(state, 4);
                            numAt = 5;
                        }
                        if (n >= numAt + 1 && lua_isnumber(state, numAt) && lua_isnumber(state, numAt + 1)) {
                            anchor.x = float(lua_tonumber(state, numAt));
                            anchor.y = float(lua_tonumber(state, numAt + 1));
                        }
                    }
                    bool replaced = false;
                    for (FrameXml::Widget::Anchor& have : w.anchors) {
                        if (have.point == anchor.point) {
                            have = anchor;
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced) w.anchors.push_back(anchor);
                    inner->layoutDirty = true;
                } else if (std::strcmp(method, "ClearAllPoints") == 0) {
                    w.anchors.clear();
                    inner->layoutDirty = true;
                } else if (std::strcmp(method, "SetAllPoints") == 0) {
                    int rel = w.parent;
                    if (lua_isuserdata(state, 2)) rel = idOf(state, 2);
                    else if (lua_isstring(state, 2)) {
                        const auto named = inner->names.find(lua_tostring(state, 2));
                        if (named != inner->names.end()) rel = named->second;
                    }
                    w.anchors.clear();
                    FrameXml::Widget::Anchor edge;
                    edge.relId = rel > 0 ? rel : -2;
                    edge.point = "TOPLEFT";
                    edge.relPoint = "TOPLEFT";
                    w.anchors.push_back(edge);
                    edge.point = "BOTTOMRIGHT";
                    edge.relPoint = "BOTTOMRIGHT";
                    w.anchors.push_back(edge);
                    inner->layoutDirty = true;
                } else if (std::strcmp(method, "GetWidth") == 0) {
                    lua_pushnumber(state, w.w);
                    return 1;
                } else if (std::strcmp(method, "GetHeight") == 0) {
                    lua_pushnumber(state, w.h);
                    return 1;
                } else if (std::strcmp(method, "GetLeft") == 0) {
                    lua_pushnumber(state, w.x);
                    return 1;
                } else if (std::strcmp(method, "GetRight") == 0) {
                    lua_pushnumber(state, w.x + w.w);
                    return 1;
                } else if (std::strcmp(method, "GetTop") == 0) {
                    lua_pushnumber(state, inner->screenH - w.y);
                    return 1;
                } else if (std::strcmp(method, "GetBottom") == 0) {
                    lua_pushnumber(state, inner->screenH - (w.y + w.h));
                    return 1;
                } else if (std::strcmp(method, "GetParent") == 0) {
                    if (w.parent <= 0) return 0;
                    pushWidgetRet(state, w.parent);
                    return 1;
                } else if (std::strcmp(method, "IsShown") == 0) {
                    lua_pushboolean(state, w.shown);
                    return 1;
                } else if (std::strcmp(method, "IsVisible") == 0) {
                    bool seen = w.shown;
                    int at = w.parent;
                    for (int guard = 0; seen && at > 0 && guard < 32; ++guard) {
                        if (at >= int(inner->widgets.size()) || !inner->widgets[at].shown) seen = false;
                        else at = inner->widgets[at].parent;
                    }
                    lua_pushboolean(state, seen);
                    return 1;
                } else if (std::strcmp(method, "Show") == 0) {
                    w.shown = true;
                    inner->layoutDirty = true;
                } else if (std::strcmp(method, "Hide") == 0) {
                    w.shown = false;
                    inner->layoutDirty = true;
                } else if (std::strcmp(method, "EnableMouse") == 0) {
                    w.mouse = lua_toboolean(state, 2) != 0;
                } else if (std::strcmp(method, "RegisterEvent") == 0) {
                    if (lua_tostring(state, 2)) w.events.emplace_back(lua_tostring(state, 2));
                } else if (std::strcmp(method, "SetScript") == 0) {
                    const char* which = lua_tostring(state, 2);
                    if (which && lua_isfunction(state, 3)) {
                        const int ref = luaL_ref(state, LUA_REGISTRYINDEX);
                        int* slot = nullptr;
                        if (std::strcmp(which, "OnClick") == 0) slot = &w.onClick;
                        else if (std::strcmp(which, "OnMouseDown") == 0) slot = &w.onMouseDown;
                        else if (std::strcmp(which, "OnMouseUp") == 0) slot = &w.onMouseUp;
                        else if (std::strcmp(which, "OnEvent") == 0) slot = &w.onEvent;
                        else if (std::strcmp(which, "OnLoad") == 0) slot = &w.onLoad;
                        if (!slot) luaL_unref(state, LUA_REGISTRYINDEX, ref);
                        else {
                            if (*slot != LUA_NOREF) luaL_unref(state, LUA_REGISTRYINDEX, *slot);
                            *slot = ref;
                        }
                    }
                } else if (std::strcmp(method, "GetName") == 0) {
                    lua_pushstring(state, w.name.c_str());
                    return 1;
                } else if (std::strcmp(method, "SetParent") == 0) {
                    w.parent = idOf(state, 2);
                    inner->layoutDirty = true;
                }
                return 0;
            },
            1);
        return 1;
    }
    if (std::strcmp(key, "GetName") == 0) return 0;
    lua_pushcfunction(L, [](lua_State*) { return 0; });
    return 1;
}

struct Budget {
    lua_State* L;
    explicit Budget(lua_State* state) : L(state) { lua_sethook(L, reclientBudgetHook, LUA_MASKCOUNT, 2000000); }
    ~Budget() { lua_sethook(L, nullptr, 0, 0); }
};

void callRef(lua_State* L, int ref, int id, const char* event) {
    if (ref == LUA_NOREF) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    pushWidgetRet(L, id);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "this");
    int args = 1;
    if (event) {
        lua_pushstring(L, event);
        lua_setglobal(L, "event");
        lua_pushstring(L, event);
        ++args;
    }
    FrameXml::Body* body = bodyOf(L);
    if (body) {
        for (int i = 0; i < 11; ++i) {
            if (!body->argsLive) lua_pushnil(L);
            else if (body->fired[i].number) lua_pushnumber(L, body->fired[i].n);
            else lua_pushstring(L, body->fired[i].text.c_str());
            char key[8];
            std::snprintf(key, sizeof key, "arg%d", i + 1);
            lua_setglobal(L, key);
        }
    }
    Budget budget(L);
    if (lua_pcall(L, args, 0, 0) != 0) {
        const char* msg = lua_tostring(L, -1);
        if (body && id > 0 && id < int(body->widgets.size())) {
            const std::string& who = body->widgets[id].name;
            const bool dialog = who.find("Gossip") != std::string::npos || who.find("Chat") != std::string::npos;
            if (dialog || body->scriptError.empty())
                body->scriptError = who + " " + (event ? event : "script") + ": " + (msg ? msg : "lua error");
        }
        lua_pop(L, 1);
    }
}

}  // namespace

FrameXml::FrameXml() : body_(new Body) {
    body_->owner = this;
    body_->L = luaL_newstate();
    luaL_openlibs(body_->L);
    lua_pushlightuserdata(body_->L, body_);
    lua_setfield(body_->L, LUA_REGISTRYINDEX, "reclient.ui");
    luaL_newmetatable(body_->L, "Widget");
    lua_pushcfunction(body_->L, lIndex);
    lua_setfield(body_->L, -2, "__index");
    lua_pushcfunction(body_->L, lNewIndex);
    lua_setfield(body_->L, -2, "__newindex");
    lua_pop(body_->L, 1);
    lua_register(body_->L, "CreateFrame", lCreateFrame);
    lua_register(body_->L, "SendChatMessage", lSendChat);
    lua_register(body_->L, "PlaySound", lPlaySound);
    lua_register(body_->L, "PlayMusic", lPlaySound);
    lua_register(body_->L, "UnitName", lUnitName);
    lua_register(body_->L, "UnitExists", lUnitExists);
    lua_register(body_->L, "GetGossipText", lGetGossipText);
    lua_register(body_->L, "GetNumGossipOptions", lGetNumGossipOptions);
    lua_register(body_->L, "GetGossipOptions", lGetGossipOptions);
    lua_register(body_->L, "GetNumGossipAvailableQuests", lNoGossip);
    lua_register(body_->L, "GetNumGossipActiveQuests", lNoGossip);
    lua_register(body_->L, "GetGossipAvailableQuests", lNoList);
    lua_register(body_->L, "GetGossipActiveQuests", lNoList);
    lua_register(body_->L, "SelectGossipOption", lSelectGossip);
    lua_register(body_->L, "SelectGossipAvailableQuest", lSelectGossip);
    lua_register(body_->L, "SelectGossipActiveQuest", lSelectGossip);
    lua_register(body_->L, "CloseGossip", lCloseGossip);
    lua_register(body_->L, "GetDefaultLanguage", lGetDefaultLanguage);
    lua_register(body_->L, "GetChatTypeIndex", lGetChatTypeIndex);
    lua_register(body_->L, "GetChatWindowInfo", lGetChatWindowInfo);
    lua_register(body_->L, "GetChatWindowMessages", lGetChatWindowMessages);
    lua_register(body_->L, "GetChatWindowChannels", lGetChatWindowChannels);
    lua_register(body_->L, "GetChannelName", lGetChannelName);
    lua_newtable(body_->L);
    lua_pushcfunction(body_->L, lBand);
    lua_setfield(body_->L, -2, "band");
    lua_pushcfunction(body_->L, lBor);
    lua_setfield(body_->L, -2, "bor");
    lua_pushcfunction(body_->L, lBxor);
    lua_setfield(body_->L, -2, "bxor");
    lua_pushcfunction(body_->L, lBnot);
    lua_setfield(body_->L, -2, "bnot");
    lua_pushcfunction(body_->L, lLshift);
    lua_setfield(body_->L, -2, "lshift");
    lua_pushcfunction(body_->L, lRshift);
    lua_setfield(body_->L, -2, "rshift");
    lua_pushcfunction(body_->L, lRshift);
    lua_setfield(body_->L, -2, "arshift");
    lua_setglobal(body_->L, "bit");
    lua_register(body_->L, "LoadAddOn", lLoadAddOn);
    lua_register(body_->L, "IsAddOnLoaded", lIsAddOnLoaded);
    lua_register(body_->L, "GetNumAddOns", lGetNumAddOns);
    lua_register(body_->L, "GetAddOnInfo", lGetAddOnInfo);
    lua_register(body_->L, "GetAddOnMetadata", lGetAddOnMetadata);
    lua_register(body_->L, "GetScreenWidth", [](lua_State* state) -> int {
        FrameXml::Body* body = bodyOf(state);
        lua_pushnumber(state, body ? body->screenW : 0);
        return 1;
    });
    lua_register(body_->L, "GetScreenHeight", [](lua_State* state) -> int {
        FrameXml::Body* body = bodyOf(state);
        lua_pushnumber(state, body ? body->screenH : 0);
        return 1;
    });
    luaL_dostring(body_->L,
                  "function getglobal(n) return _G[n] end\n"
                  "function setglobal(n,v) _G[n]=v end\n"
                  "function getn(t) return #t end\n"
                  "function hooksecurefunc() end\n"
                  "function StopMusic() end\n"
                  "function PlayGlueMusic() end\n"
                  "function StopGlueMusic() end\n"
                  "function SetCVar() end\n"
                  "function GetCVar() return '' end\n"
                  "function GetRealmName() return 'MaNGOS' end\n"
                  "function GetAccountName() return '' end\n"
                  "local _clock = 0\n"
                  "function GetTime() _clock = _clock + 0.05; return _clock end\n"
                  "tinsert = table.insert\n"
                  "tremove = table.remove\n"
                  "format = string.format\n"
                  "strlower = string.lower\n"
                  "strupper = string.upper\n"
                  "strsub = string.sub\n"
                  "strfind = string.find\n"
                  "strmatch = string.match\n"
                  "gsub = string.gsub\n"
                  "strlen = string.len\n"
                  "strrep = string.rep\n"
                  "strtrim = function(s) return (s:gsub('^%s*(.-)%s*$', '%1')) end\n"
                  "ceil = math.ceil\n"
                  "floor = math.floor\n"
                  "abs = math.abs\n"
                  "mod = math.fmod\n"
                  "max = math.max\n"
                  "min = math.min\n"
                  "function GetItemQualityColor() return 1, 1, 1 end\n"
                  "function message() end\n"
                  "function GetNumPartyMembers() return 0 end\n"
                  "function GetNumRaidMembers() return 0 end\n"
                  "function IsInGuild() return false end\n"
                  "function StaticPopup_Visible() return nil end\n"
                  "function StaticPopup_Hide() end\n"
                  "function SetPortraitTexture() end\n");
}

FrameXml::~FrameXml() {
    if (body_) {
        if (body_->L) lua_close(body_->L);
        delete body_;
    }
}

void FrameXml::setChatter(Chatter chatter) { chatter_ = std::move(chatter); }

void FrameXml::setSounder(Sounder sounder) { sounder_ = std::move(sounder); }

void FrameXml::play(const std::string& name) {
    if (sounder_) sounder_(name);
}

void FrameXml::setPlayerName(std::string name) {
    player_ = std::move(name);
    lua_pushstring(body_->L, player_.c_str());
    lua_setfield(body_->L, LUA_REGISTRYINDEX, "reclient.player");
}

void FrameXml::say(const std::string& text) {
    if (chatter_) chatter_(text);
}

void FrameXml::chooseGossip(int index) {
    if (picker_) picker_(index);
}

void FrameXml::setPicker(std::function<void(int)> picker) { picker_ = std::move(picker); }

void FrameXml::note(const std::string& label, const std::string& error) {
    if (errors_.size() < 40) errors_.push_back(label + ": " + error);
}

bool FrameXml::loadLua(const std::string& source, const std::string& label) {
    Budget budget(body_->L);
    if (luaL_loadbuffer(body_->L, source.data(), source.size(), label.c_str()) != 0 || lua_pcall(body_->L, 0, 0, 0) != 0) {
        note(label, lua_tostring(body_->L, -1) ? lua_tostring(body_->L, -1) : "lua error");
        lua_pop(body_->L, 1);
        return false;
    }
    publish();
    body_->layoutDirty = true;
    return true;
}

void pinDialogs(FrameXml::Body& body) {
    auto widget = [&](const std::string& name) -> FrameXml::Widget* {
        const auto found = body.names.find(name);
        if (found == body.names.end()) return nullptr;
        const int id = found->second;
        if (id <= 0 || id >= int(body.widgets.size())) return nullptr;
        return &body.widgets[id];
    };
    // One docked panel. Extra chat windows stay hidden; the original client would
    // show them as tabs on this same panel, not as a second window behind it.
    for (FrameXml::Widget& extra : body.widgets) {
        const bool otherLog = extra.type == "ScrollingMessageFrame" && extra.name != "ChatFrame1";
        const bool otherChat = extra.name.size() >= 10 && extra.name.compare(0, 9, "ChatFrame") == 0 && extra.name[9] >= '2' &&
                               extra.name[9] <= '9';
        if (otherLog || otherChat) extra.shown = false;
        if (extra.type == "Texture" && !extra.art.empty()) {
            std::string art = extra.art;
            for (char& c : art)
                if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            if (art.find("chatframebackground") != std::string::npos || art.find("chatbackground") != std::string::npos)
                extra.art.clear();
        }
    }
    // The main bar occupies the bottom of the screen. Keep the one chat window above it.
    if (body.screenH > 240.f) {
        const float margin = 84.f;
        const float editH = 28.f;
        if (FrameXml::Widget* chat = widget("ChatFrame1")) {
            const float nx = 16.f;
            const float nw = 430.f;
            const float nh = 120.f;
            const float ny = body.screenH - margin - editH - 6.f - nh;
            const float dx = nx - chat->x;
            const float dy = ny - chat->y;
            chat->x = nx;
            chat->y = ny;
            chat->w = nw;
            chat->h = nh;
            chat->shown = true;
            if (std::fabs(dx) + std::fabs(dy) > 0.5f) {
                std::vector<int> pending{chat->id};
                for (size_t n = 0; n < pending.size(); ++n) {
                    const int parent = pending[n];
                    for (FrameXml::Widget& child : body.widgets) {
                        if (child.parent != parent || !child.shown) continue;
                        child.x += dx;
                        child.y += dy;
                        pending.push_back(child.id);
                    }
                }
            }
            if (FrameXml::Widget* tab = widget("ChatFrame1Tab")) {
                tab->shown = true;
                tab->x = chat->x + 6.f;
                tab->y = chat->y - 18.f;
                tab->w = 78.f;
                tab->h = 20.f;
                if (tab->text.empty()) tab->text = "General";
            }
        }
        if (FrameXml::Widget* edit = widget("ChatFrame1EditBox")) {
            edit->x = 16.f;
            edit->w = 430.f;
            edit->h = editH;
            edit->y = body.screenH - margin - editH;
        }
    }
    FrameXml::Widget* gossip = widget("GossipFrame");
    if (!gossip || !gossip->shown) return;
    if (gossip->w < 200.f) gossip->w = 384.f;
    if (gossip->h < 200.f) gossip->h = 512.f;
    if (gossip->x < 0.f || gossip->y < 0.f || gossip->x + 80.f > body.screenW || gossip->y + 80.f > body.screenH) {
        gossip->x = 48.f;
        gossip->y = 64.f;
    }
    if (FrameXml::Widget* title = widget("GossipFrameNpcNameText")) {
        title->x = gossip->x + 80.f;
        title->y = gossip->y + 18.f;
        title->w = std::max(80.f, gossip->w - 150.f);
        title->h = 16.f;
        title->shown = true;
    }
    // The parchment writing area starts below the frame's top bevel. The scroll
    // child is inset 81px; another 30px clears the dark edge that was eating the line.
    const float x = gossip->x + 36.f;
    const float width = std::max(160.f, std::min(270.f, gossip->w - 90.f));
    float y = gossip->y + 112.f;
    const float limit = gossip->y + gossip->h - 120.f;
    if (FrameXml::Widget* greet = widget("GossipGreetingText")) {
        const bool filler = greet->text.empty() || greet->text == "..." || greet->text == "Filler Text";
        if (filler) {
            greet->shown = false;
        } else {
            int lines = 1;
            int col = 0;
            const int cols = std::max(8, int(width / 8.f));
            for (char c : greet->text) {
                if (c == '\n' || col >= cols) {
                    ++lines;
                    col = c == '\n' ? 0 : 1;
                } else {
                    ++col;
                }
            }
            greet->x = x;
            greet->y = y;
            greet->w = width;
            greet->h = float(lines * 10 + 6);
            greet->shown = true;
            y += greet->h + 8.f;
        }
    }
    for (int i = 1; i <= 32; ++i) {
        FrameXml::Widget* button = widget("GossipTitleButton" + std::to_string(i));
        if (!button || !button->shown || button->text.empty()) continue;
        if (y + 18.f > limit) {
            button->shown = false;
            continue;
        }
        button->x = x;
        button->y = y;
        button->w = width;
        button->h = 18.f;
        button->mouse = true;
        y += 20.f;
    }
    if (FrameXml::Widget* bye = widget("GossipFrameGreetingGoodbyeButton")) {
        bye->w = 78.f;
        bye->h = 22.f;
        bye->x = gossip->x + gossip->w - 117.f;
        bye->y = gossip->y + gossip->h - 95.f;
        bye->shown = true;
        bye->mouse = true;
        if (bye->text.empty()) bye->text = "Goodbye";
    }
    if (FrameXml::Widget* close = widget("GossipFrameCloseButton")) {
        close->w = 32.f;
        close->h = 32.f;
        close->x = gossip->x + gossip->w - 58.f;
        close->y = gossip->y + 15.f;
        close->shown = true;
        close->mouse = true;
    }
}

void FrameXml::publish() {
    pinDialogs(*body_);
    ++revision_;
    draw_.clear();
    auto visible = [&](auto&& self, int id, int guard) -> bool {
        if (id <= 0 || id >= int(body_->widgets.size()) || guard > 32) return true;
        const Widget& widget = body_->widgets[id];
        if (!widget.shown) return false;
        return self(self, widget.parent, guard + 1);
    };
    for (const Widget& widget : body_->widgets) {
        if (widget.id == 0 || !visible(visible, widget.id, 0)) continue;
        UiWidget view;
        view.id = widget.id;
        view.parent = widget.parent;
        view.type = widget.type;
        view.name = widget.name;
        view.text = widget.text;
        view.art = widget.art;
        view.x = widget.x;
        view.y = widget.y;
        view.w = widget.w;
        view.h = widget.h;
        view.u0 = widget.u0;
        view.v0 = widget.v0;
        view.u1 = widget.u1;
        view.v1 = widget.v1;
        view.shown = widget.shown;
        view.mouse = widget.mouse;
        view.sized = widget.abs || !widget.art.empty();
        // Interface art comes from the texture file. A solid fill is not a substitute.
        view.color = 0;
        draw_.push_back(view);
    }
}

int FrameXml::loadToc(const ArchiveSet& archives, const std::string& tocPath) {
    const ArchiveSet* previous = archives_;
    archives_ = &archives;
    const int loaded = loadListed(tocPath);
    archives_ = previous;
    return loaded;
}

bool FrameXml::readUi(const std::string& path, std::vector<uint8_t>& out) const {
    if (archives_ && archives_->read(path, out)) return true;
    if (root_.empty()) return false;
    std::string full = root_;
    if (!full.empty() && full.back() != '\\' && full.back() != '/') full.push_back('\\');
    full += path;
    std::ifstream in(full, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const auto bytes = in.tellg();
    if (bytes <= 0 || bytes > 8 * 1024 * 1024) return false;
    in.seekg(0);
    out.resize(static_cast<size_t>(bytes));
    in.read(reinterpret_cast<char*>(out.data()), bytes);
    return bool(in);
}

void FrameXml::useClient(const ArchiveSet* archives, std::string root) {
    archives_ = archives;
    root_ = std::move(root);
}

void FrameXml::rememberAddon(const std::string& name, const std::string& tocText) {
    for (const AddonRec& have : addons_)
        if (have.name == name) return;
    AddonRec rec;
    rec.name = name;
    size_t line = 0;
    while (line < tocText.size()) {
        size_t end = tocText.find('\n', line);
        if (end == std::string::npos) end = tocText.size();
        std::string row = trimCopy(tocText.substr(line, end - line));
        line = end + (end < tocText.size() ? 1 : 0);
        if (row.size() < 3 || row[0] != '#') continue;
        std::string lower = row;
        for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
        auto value = [&](const char* key) -> std::string {
            const std::string look = std::string("## ") + key;
            if (lower.compare(0, look.size(), look) != 0) return {};
            std::string rest = trimCopy(row.substr(look.size()));
            if (!rest.empty() && rest[0] == ':') rest = trimCopy(rest.substr(1));
            return rest;
        };
        if (std::string title = value("title"); !title.empty()) rec.title = std::move(title);
        if (std::string demand = value("loadondemand"); demand == "1") rec.demand = true;
    }
    if (rec.title.empty()) rec.title = name;
    addons_.push_back(std::move(rec));
}

void FrameXml::scanAddons() {
    if (root_.empty()) return;
    namespace fs = std::filesystem;
    std::error_code error;
    const fs::path folder = fs::path(root_) / "Interface" / "AddOns";
    if (!fs::is_directory(folder, error)) return;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, error)) {
        if (error || !entry.is_directory()) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() < 10 || name.compare(0, 9, "Blizzard_") != 0) continue;
        const std::string tocPath = "Interface\\AddOns\\" + name + "\\" + name + ".toc";
        std::vector<uint8_t> toc;
        if (!readUi(tocPath, toc)) continue;
        rememberAddon(name, std::string(reinterpret_cast<const char*>(toc.data()), toc.size()));
    }
}

int FrameXml::loadAddOn(const std::string& name) {
    if (name.empty()) return -1;
    for (AddonRec& addon : addons_) {
        if (addon.name != name) continue;
        if (addon.loaded) return 0;
        addon.loaded = true;
        const int files = loadListed("Interface\\AddOns\\" + name + "\\" + name + ".toc");
        fire("ADDON_LOADED");
        return files > 0 ? files : 0;
    }
    const std::string tocPath = "Interface\\AddOns\\" + name + "\\" + name + ".toc";
    std::vector<uint8_t> toc;
    if (!readUi(tocPath, toc)) {
        note(tocPath, "not in the client archives");
        return -1;
    }
    rememberAddon(name, std::string(reinterpret_cast<const char*>(toc.data()), toc.size()));
    return loadAddOn(name);
}

int FrameXml::loadStockUi() {
    scanAddons();
    const int base = loadListed("Interface\\FrameXML\\FrameXML.toc");
    int extra = 0;
    for (const AddonRec& addon : addons_) {
        if (addon.demand || addon.loaded) continue;
        const int files = loadAddOn(addon.name);
        if (files > 0) extra += files;
    }
    fire("VARIABLES_LOADED");
    return base + extra;
}

int FrameXml::loadListed(const std::string& tocPath) {
    std::vector<uint8_t> toc;
    if (!readUi(tocPath, toc)) {
        note(tocPath, "not in the client archives");
        return 0;
    }
    std::string dir = tocPath;
    const size_t slash = dir.find_last_of("\\/");
    dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
    const std::string text(reinterpret_cast<const char*>(toc.data()), toc.size());
    int loaded = 0;
    size_t line = 0;
    while (line < text.size()) {
        size_t end = text.find('\n', line);
        if (end == std::string::npos) end = text.size();
        std::string row = trimCopy(text.substr(line, end - line));
        line = end + (end < text.size() ? 1 : 0);
        if (row.empty() || row[0] == '#') continue;
        for (char& c : row)
            if (c == '/') c = '\\';
        const std::string path = dir + row;
        if (std::getenv("RECLIENT_UI_TRACE")) {
            std::fprintf(stderr, "ui %s\n", path.c_str());
            std::fflush(stderr);
        }
        std::vector<uint8_t> file;
        if (!loadedFiles_.insert(path).second) continue;
        if (!readUi(path, file)) {
            loadedFiles_.erase(path);
            note(path, "missing");
            continue;
        }
        const std::string source(reinterpret_cast<const char*>(file.data()), file.size());
        const bool lua = row.size() >= 4 && row.substr(row.size() - 4) == ".lua";
        if (lua ? loadLua(source, path) : loadXml(source, path)) ++loaded;
    }
    return loaded;
}

bool FrameXml::loadXml(const std::string& xml, const std::string& label) {
    size_t i = 0;
    int made = 0;
    while (i < xml.size()) {
        skipSpace(xml, i);
        if (i >= xml.size()) break;
        if (xml.compare(i, 2, "</") == 0) break;
        XmlNode node;
        const size_t before = i;
        if (!parseNode(xml, i, node)) {
            const size_t span = std::min(size_t(60), xml.size() - std::min(i, xml.size()));
            std::string near = xml.substr(std::min(i, xml.size()), span);
            for (char& c : near)
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            note(label, "xml stopped early at " + std::to_string(i) + " near '" + near + "'");
            break;
        }
        if (i == before) break;
        auto pullFile = [&](const std::string& file) {
            std::string rel = file;
            for (char& c : rel)
                if (c == '/') c = '\\';
            std::string path = rel;
            const bool rooted = rel.size() >= 10 && (rel.compare(0, 10, "Interface\\") == 0);
            if (!rooted) {
                const size_t slash = label.find_last_of("\\/");
                path = (slash == std::string::npos ? std::string() : label.substr(0, slash + 1)) + rel;
            }
            if (!loadedFiles_.insert(path).second) return;
            std::vector<uint8_t> bytes;
            if (!readUi(path, bytes)) {
                loadedFiles_.erase(path);
                note(path, "missing");
                return;
            }
            const std::string source(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            const bool luaFile = path.size() >= 4 && path.compare(path.size() - 4, 4, ".lua") == 0;
            if (luaFile) loadLua(source, path);
            else loadXml(source, path);
        };
        std::function<int(const XmlNode&, int)> build = [&](const XmlNode& raw, int parent) -> int {
            if (const std::string* file = raw.attr("file");
                file && (raw.name == "Include" || raw.name == "Script")) {
                pullFile(*file);
                return parent;
            }
            if (const std::string* virt = raw.attr("virtual"); virt && *virt == "true") {
                if (const std::string* name = raw.attr("name"))
                    if (!name->empty()) body_->templates[*name] = raw;
                return parent;
            }
            const XmlNode el = flatten(raw, body_->templates, 0);
            const bool visual = el.name == "Frame" || el.name == "Button" || el.name == "FontString" ||
                                el.name == "Texture" || el.name == "EditBox" || el.name == "StatusBar" ||
                                el.name == "ScrollFrame" || el.name == "CheckButton" || el.name == "Slider" ||
                                el.name == "Minimap" || el.name == "Model" || el.name == "PlayerModel" ||
                                el.name == "DressUpModel" || el.name == "TabardModel" || el.name == "MessageFrame" ||
                                el.name == "ScrollingMessageFrame" || el.name == "SimpleHTML" || el.name == "Cooldown";
            int id = parent;
            const bool owned = visual;
            if (visual) {
                Widget widget;
                widget.id = int(body_->widgets.size());
                widget.parent = parent;
                widget.type = el.name;
                if (const std::string* name = el.attr("name")) {
                    widget.name = *name;
                    if (parent > 0 && widget.name.find("$parent") == 0)
                        widget.name = body_->widgets[parent].name + widget.name.substr(7);
                }
                if (const std::string* text = el.attr("text")) widget.text = *text;
                if (const std::string* file = el.attr("file")) widget.art = *file;
                if (const std::string* hidden = el.attr("hidden")) widget.shown = *hidden != "true";
                if (const std::string* namedParent = el.attr("parent")) {
                    if (*namedParent != "$parent") {
                        const auto found = body_->names.find(*namedParent);
                        if (found != body_->names.end()) widget.parent = found->second;
                        else widget.parentName = *namedParent;
                    }
                }
                widget.mouse = el.name == "Button" || el.name == "CheckButton" || el.name == "EditBox";
                if (const std::string* mouse = el.attr("enableMouse")) widget.mouse = *mouse == "true";
                if (const std::string* all = el.attr("setAllPoints"); all && *all == "true") {
                    Widget::Anchor edge;
                    edge.point = "TOPLEFT";
                    edge.relPoint = "TOPLEFT";
                    widget.anchors.push_back(edge);
                    edge.point = "BOTTOMRIGHT";
                    edge.relPoint = "BOTTOMRIGHT";
                    widget.anchors.push_back(edge);
                }
                if (el.name == "FontString") widget.color = 0x00000000;
                body_->widgets.push_back(widget);
                id = widget.id;
                if (!body_->widgets[id].name.empty()) body_->names[body_->widgets[id].name] = id;
                pushWidget(body_->L, id);
                ++made;
            }
            for (const XmlNode& child : el.kids) {
                if (!owned && (child.name == "Size" || child.name == "Anchors" || child.name == "TexCoords" ||
                               child.name == "Scripts"))
                    continue;
                if (child.name == "Size") {
                    for (const XmlNode& dim : child.kids) {
                        if (dim.name == "AbsDimension" && id > 0) {
                            if (const std::string* x = dim.attr("x")) body_->widgets[id].w = std::strtof(x->c_str(), nullptr);
                            if (const std::string* y = dim.attr("y")) body_->widgets[id].h = std::strtof(y->c_str(), nullptr);
                            body_->widgets[id].abs = true;
                        }
                    }
                } else if (child.name == "Anchors") {
                    if (id > 0) body_->widgets[id].anchors.clear();
                    for (const XmlNode& anchorEl : child.kids) {
                        if (anchorEl.name != "Anchor" || id <= 0) continue;
                        Widget::Anchor anchor;
                        if (const std::string* point = anchorEl.attr("point")) anchor.point = *point;
                        if (const std::string* rel = anchorEl.attr("relativePoint")) anchor.relPoint = *rel;
                        if (const std::string* relTo = anchorEl.attr("relativeTo")) anchor.relName = *relTo;
                        for (const XmlNode& off : anchorEl.kids) {
                            if (off.name != "Offset") continue;
                            if (const std::string* x = off.attr("x")) anchor.x = std::strtof(x->c_str(), nullptr);
                            if (const std::string* y = off.attr("y")) anchor.y = std::strtof(y->c_str(), nullptr);
                            for (const XmlNode& dim : off.kids) {
                                if (dim.name != "AbsDimension") continue;
                                if (const std::string* x = dim.attr("x")) anchor.x = std::strtof(x->c_str(), nullptr);
                                if (const std::string* y = dim.attr("y")) anchor.y = std::strtof(y->c_str(), nullptr);
                            }
                        }
                        body_->widgets[id].anchors.push_back(anchor);
                    }
                } else if (child.name == "TexCoords" && id > 0) {
                    auto num = [](const std::string* text, float fallback) {
                        return text ? std::strtof(text->c_str(), nullptr) : fallback;
                    };
                    body_->widgets[id].u0 = num(child.attr("left"), 0.f);
                    body_->widgets[id].v0 = num(child.attr("top"), 0.f);
                    body_->widgets[id].u1 = num(child.attr("right"), 1.f);
                    body_->widgets[id].v1 = num(child.attr("bottom"), 1.f);
                } else if (child.name == "Scripts" && id > 0) {
                    for (const XmlNode& script : child.kids) {
                        std::string scriptText = trimCopy(stripXmlComments(script.text));
                        if (scriptText.size() >= 12 && scriptText.compare(0, 9, "<![CDATA[") == 0 &&
                            scriptText.compare(scriptText.size() - 3, 3, "]]>") == 0)
                            scriptText = trimCopy(scriptText.substr(9, scriptText.size() - 12));
                        const std::string body = "local self = ...\n" + scriptText;
                        if (luaL_loadbuffer(body_->L, body.data(), body.size(), (label + ":" + script.name).c_str()) != 0) {
                            std::string sample = scriptText.substr(0, std::min(size_t(80), scriptText.size()));
                            for (char& c : sample)
                                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                            note(label, std::string(lua_tostring(body_->L, -1) ? lua_tostring(body_->L, -1) : "script") +
                                            " body '" + sample + "'");
                            lua_pop(body_->L, 1);
                            continue;
                        }
                        const int ref = luaL_ref(body_->L, LUA_REGISTRYINDEX);
                        int* slot = nullptr;
                        if (script.name == "OnClick") slot = &body_->widgets[id].onClick;
                        else if (script.name == "OnMouseDown") slot = &body_->widgets[id].onMouseDown;
                        else if (script.name == "OnMouseUp") slot = &body_->widgets[id].onMouseUp;
                        else if (script.name == "OnEvent") slot = &body_->widgets[id].onEvent;
                        else if (script.name == "OnLoad") slot = &body_->widgets[id].onLoad;
                        if (!slot) luaL_unref(body_->L, LUA_REGISTRYINDEX, ref);
                        else *slot = ref;
                    }
                } else if (!child.text.empty() && id > 0 && body_->widgets[id].text.empty() &&
                           (child.name == "FontString" || el.name == "FontString")) {
                    body_->widgets[id].text = trimCopy(child.text);
                } else {
                    if (const std::string* file = child.attr("file");
                        file && id > 0 && body_->widgets[id].art.empty() &&
                        (child.name == "NormalTexture" || child.name == "PushedTexture"))
                        body_->widgets[id].art = *file;
                    build(child, id);
                }
            }
            if (id > 0 && id != parent && body_->widgets[id].onLoad != LUA_NOREF)
                callRef(body_->L, body_->widgets[id].onLoad, id, nullptr);
            return id;
        };
        build(node, 0);
    }
    publish();
    body_->layoutDirty = true;
    return made > 0;
}

namespace {

void anchorFrac(const std::string& point, float& fx, float& fy) {
    const bool right = point.find("RIGHT") != std::string::npos;
    const bool left = point.find("LEFT") != std::string::npos;
    const bool bottom = point.find("BOTTOM") != std::string::npos;
    const bool top = point.find("TOP") != std::string::npos;
    fx = right ? 1.f : left ? 0.f : 0.5f;
    fy = bottom ? 1.f : top ? 0.f : 0.5f;
}

bool shownChain(const std::vector<FrameXml::Widget>& widgets, int id) {
    for (int guard = 0; id > 0 && guard < 32; ++guard) {
        if (id >= int(widgets.size()) || !widgets[id].shown) return false;
        id = widgets[id].parent;
    }
    return true;
}

}  // namespace

bool FrameXml::layoutPending() const { return body_ && (body_->layoutDirty || body_->textDirty); }

int FrameXml::widgetCount() const { return body_ ? int(body_->widgets.size()) : 0; }

void FrameXml::layout(float screenW, float screenH) {
    if (screenW < 1.f) screenW = 1.f;
    if (screenH < 1.f) screenH = 1.f;
    if (!body_->layoutDirty) {
        body_->textDirty = false;
        body_->screenW = screenW;
        body_->screenH = screenH;
        publish();
        return;
    }
    body_->textDirty = false;
    body_->layoutDirty = false;
    body_->screenW = screenW;
    body_->screenH = screenH;
    const int count = int(body_->widgets.size());
    std::vector<char> done(size_t(count), 0);
    std::function<void(int)> place = [&](int id) {
        if (id <= 0 || id >= count || done[size_t(id)] == 2) return;
        if (done[size_t(id)] == 1) return;
        done[size_t(id)] = 1;
        Widget& widget = body_->widgets[id];
        if (!widget.parentName.empty()) {
            const auto named = body_->names.find(widget.parentName);
            if (named != body_->names.end()) {
                widget.parent = named->second;
                widget.parentName.clear();
            }
        }
        if (widget.name == "UIParent" || widget.name == "WorldFrame") {
            widget.x = 0;
            widget.y = 0;
            widget.w = screenW;
            widget.h = screenH;
            done[size_t(id)] = 2;
            return;
        }
        struct Pin {
            float sx = 0, sy = 0, fx = 0, fy = 0;
        };
        std::vector<Pin> pins;
        const Widget::Anchor origin;
        const std::vector<Widget::Anchor> fallback{origin};
        const std::vector<Widget::Anchor>& anchors = widget.anchors.empty() ? fallback : widget.anchors;
        for (const Widget::Anchor& anchor : anchors) {
            int rel = anchor.relId;
            if (!anchor.relName.empty() && anchor.relName != "$parent") {
                const auto named = body_->names.find(anchor.relName);
                rel = named == body_->names.end() ? widget.parent : named->second;
            } else if (anchor.relName == "$parent" || rel == -2) {
                rel = widget.parent;
            }
            float px = 0, py = 0, pw = screenW, ph = screenH;
            if (rel > 0 && rel < count && rel != id) {
                place(rel);
                const Widget& other = body_->widgets[rel];
                px = other.x;
                py = other.y;
                pw = other.w;
                ph = other.h;
            } else if (widget.parent > 0 && widget.parent < count && anchor.relName.empty() && anchor.relId == -2) {
                place(widget.parent);
                const Widget& other = body_->widgets[widget.parent];
                px = other.x;
                py = other.y;
                pw = other.w;
                ph = other.h;
            }
            float rfx = 0, rfy = 0, fx = 0, fy = 0;
            anchorFrac(anchor.relPoint.empty() ? anchor.point : anchor.relPoint, rfx, rfy);
            anchorFrac(anchor.point, fx, fy);
            Pin pin;
            pin.sx = px + rfx * pw + anchor.x;
            pin.sy = py + rfy * ph - anchor.y;
            pin.fx = fx;
            pin.fy = fy;
            pins.push_back(pin);
        }
        float x = widget.x;
        float y = widget.y;
        float w = widget.w;
        float h = widget.h;
        bool gotW = false;
        bool gotH = false;
        for (size_t i = 0; i < pins.size() && (!gotW || !gotH); ++i) {
            for (size_t j = i + 1; j < pins.size() && (!gotW || !gotH); ++j) {
                if (!gotW && std::fabs(pins[j].fx - pins[i].fx) > 0.01f) {
                    w = (pins[j].sx - pins[i].sx) / (pins[j].fx - pins[i].fx);
                    x = pins[i].sx - pins[i].fx * w;
                    gotW = true;
                }
                if (!gotH && std::fabs(pins[j].fy - pins[i].fy) > 0.01f) {
                    h = (pins[j].sy - pins[i].sy) / (pins[j].fy - pins[i].fy);
                    y = pins[i].sy - pins[i].fy * h;
                    gotH = true;
                }
            }
        }
        if (!gotW && !pins.empty()) x = pins[0].sx - pins[0].fx * w;
        if (!gotH && !pins.empty()) y = pins[0].sy - pins[0].fy * h;
        if (w < 0.f) {
            x += w;
            w = -w;
        }
        if (h < 0.f) {
            y += h;
            h = -h;
        }
        widget.x = x;
        widget.y = y;
        widget.w = w;
        widget.h = h;
        done[size_t(id)] = 2;
    };
    for (int i = 1; i < count; ++i) place(i);
    publish();
}

void FrameXml::setShown(const std::string& name, bool shown) {
    const auto found = body_->names.find(name);
    if (found == body_->names.end()) return;
    body_->widgets[found->second].shown = shown;
    body_->layoutDirty = true;
}

void FrameXml::fire(const std::string& event) {
    body_->argsLive = body_->keepArgs;
    body_->keepArgs = false;
    for (Widget& widget : body_->widgets) {
        if (widget.onEvent == LUA_NOREF) continue;
        bool wanted = widget.events.empty();
        for (const std::string& name : widget.events)
            if (name == event) wanted = true;
        if (wanted) callRef(body_->L, widget.onEvent, widget.id, event.c_str());
    }
    body_->argsLive = false;
    publish();
}

void FrameXml::showGossip(const std::string& npc, const std::string& greeting,
                          const std::vector<std::pair<std::string, std::string>>& options) {
    body_->gossipNpc = npc;
    body_->gossipText = greeting;
    body_->gossipOptions = options;
    fire("GOSSIP_SHOW");
    auto widget = [&](const std::string& name) -> Widget* {
        const auto found = body_->names.find(name);
        if (found == body_->names.end()) return nullptr;
        const int id = found->second;
        if (id <= 0 || id >= int(body_->widgets.size())) return nullptr;
        return &body_->widgets[id];
    };
    if (Widget* frame = widget("GossipFrame")) frame->shown = true;
    if (Widget* greet = widget("GossipGreetingText")) {
        if (greet->text.empty() || greet->text == "Filler Text") greet->text = greeting.empty() ? "..." : greeting;
        greet->shown = true;
    }
    if (Widget* title = widget("GossipFrameNpcNameText")) {
        if (title->text.empty()) title->text = npc;
        title->shown = true;
    }
    for (int i = 1; i <= 32; ++i) {
        Widget* button = widget("GossipTitleButton" + std::to_string(i));
        if (!button) continue;
        if (i <= int(options.size())) {
            if (button->text.empty()) button->text = options[size_t(i - 1)].first;
            button->shown = !button->text.empty();
            button->mouse = true;
            button->scriptId = i;
        } else {
            button->shown = false;
        }
    }
    // The title buttons inherit their click from the virtual template. Bind it
    // again so each visible option reports its own index.
    script("for i = 1, 32 do local b = getglobal('GossipTitleButton'..i)\n"
           " if b and b:IsShown() then b.type = 'Gossip' b:SetID(i)\n"
           "  if GossipTitleButton_OnClick then b:SetScript('OnClick', GossipTitleButton_OnClick) end end end");
    publish();
}

void FrameXml::hideGossip() {
    body_->gossipOptions.clear();
    fire("GOSSIP_CLOSED");
}

void FrameXml::setChannels(const std::vector<std::string>& names) { body_->channels = names; }

void FrameXml::setNamedText(const std::string& name, const std::string& text) {
    const auto found = body_->names.find(name);
    if (found == body_->names.end()) return;
    body_->widgets[found->second].text = text;
    body_->textDirty = true;
}

std::string FrameXml::script(const std::string& source) {
    if (luaL_loadbuffer(body_->L, source.data(), source.size(), "dialog") != 0 || lua_pcall(body_->L, 0, 0, 0) != 0) {
        body_->scriptError = lua_tostring(body_->L, -1) ? lua_tostring(body_->L, -1) : "lua error";
        lua_pop(body_->L, 1);
        return body_->scriptError;
    }
    publish();
    return {};
}

std::string FrameXml::lastError() const { return body_ ? body_->scriptError : std::string(); }

std::string FrameXml::prepareDialogs() {
    const std::string problem = script(
        "function ShowUIPanel(frame)\n"
        "  if type(frame) == 'string' then frame = getglobal(frame) end\n"
        "  if frame and frame.Show then frame:Show() end\n"
        "end\n"
        "function HideUIPanel(frame)\n"
        "  if type(frame) == 'string' then frame = getglobal(frame) end\n"
        "  if frame and frame.Hide then frame:Hide() end\n"
        "end\n"
        "function HideParentPanel(frame)\n"
        "  if frame and frame.GetParent then\n"
        "    local parent = frame:GetParent()\n"
        "    if parent and parent.Hide then parent:Hide() end\n"
        "  end\n"
        "end\n"
        "if not ChatFrame1 and UIParent and ChatFrame_OnLoad then\n"
        "  ChatFrame1 = CreateFrame('ScrollingMessageFrame', 'ChatFrame1', UIParent)\n"
        "  this = ChatFrame1\n"
        "  ChatFrame_OnLoad()\n"
        "  ChatFrame1:SetScript('OnEvent', function() ChatFrame_OnEvent(event) end)\n"
        "end\n"
        "if ChatFrame1 and not ChatFrame1EditBox then\n"
        "  ChatFrame1EditBox = CreateFrame('EditBox', 'ChatFrame1EditBox', UIParent)\n"
        "end\n"
        "if ChatFrame1 then\n"
        "  DEFAULT_CHAT_FRAME = ChatFrame1\n"
        "  ChatFrame1:SetID(1)\n"
        "  ChatFrame1.defaultLanguage = 'Common'\n"
        "  if ChatFrame1EditBox then\n"
        "    ChatFrame1.editBox = ChatFrame1EditBox\n"
        "    ChatFrame1EditBox.chatFrame = ChatFrame1\n"
        "    ChatFrame1EditBox:SetAttribute('chatType', 'SAY')\n"
        "    ChatFrame1EditBox:SetAttribute('stickyType', 'SAY')\n"
        "    ChatFrame1EditBox.chatLanguage = 'Common'\n"
        "  end\n"
        "  ChatFrame1:ClearAllPoints()\n"
        "  ChatFrame1:SetPoint('BOTTOMLEFT', 'UIParent', 'BOTTOMLEFT', 16, 52)\n"
        "  ChatFrame1:SetWidth(460)\n"
        "  ChatFrame1:SetHeight(150)\n"
        "  ChatFrame1:Show()\n"
        "end\n"
        "if ChatFrame1EditBox then\n"
        "  ChatFrame1EditBox:ClearAllPoints()\n"
        "  ChatFrame1EditBox:SetPoint('BOTTOMLEFT', 'UIParent', 'BOTTOMLEFT', 16, 18)\n"
        "  ChatFrame1EditBox:SetWidth(460)\n"
        "  ChatFrame1EditBox:SetHeight(32)\n"
        "end\n");
    if (!problem.empty()) return problem;
    fire("UPDATE_CHAT_WINDOWS");
    return body_->scriptError;
}

void FrameXml::chatLine(uint8_t type, const std::string& sender, const std::string& text, const std::string& channel,
                        uint32_t language) {
    const char* event = nullptr;
    switch (type) {
    case 0x00: event = "CHAT_MSG_SYSTEM"; break;
    case 0x01: event = "CHAT_MSG_SAY"; break;
    case 0x02: event = "CHAT_MSG_PARTY"; break;
    case 0x03: event = "CHAT_MSG_RAID"; break;
    case 0x04: event = "CHAT_MSG_GUILD"; break;
    case 0x05: event = "CHAT_MSG_OFFICER"; break;
    case 0x06: event = "CHAT_MSG_YELL"; break;
    case 0x07: event = "CHAT_MSG_WHISPER"; break;
    case 0x09: event = "CHAT_MSG_WHISPER_INFORM"; break;
    case 0x0A: event = "CHAT_MSG_EMOTE"; break;
    case 0x0B: event = "CHAT_MSG_TEXT_EMOTE"; break;
    case 0x0C: event = "CHAT_MSG_MONSTER_SAY"; break;
    case 0x0D: event = "CHAT_MSG_MONSTER_PARTY"; break;
    case 0x0E: event = "CHAT_MSG_MONSTER_YELL"; break;
    case 0x0F: event = "CHAT_MSG_MONSTER_WHISPER"; break;
    case 0x10: event = "CHAT_MSG_MONSTER_EMOTE"; break;
    case 0x11: event = "CHAT_MSG_CHANNEL"; break;
    default: break;
    }
    if (!event) return;
    const char* tongue = "Universal";
    switch (language) {
    case 1: tongue = "Orcish"; break;
    case 2: tongue = "Darnassian"; break;
    case 3: tongue = "Taurahe"; break;
    case 6: tongue = "Dwarvish"; break;
    case 7: tongue = "Common"; break;
    case 8: tongue = "Demonic"; break;
    case 9: tongue = "Titan"; break;
    case 10: tongue = "Thalassian"; break;
    case 11: tongue = "Draconic"; break;
    case 12: tongue = "Kalimag"; break;
    case 13: tongue = "Gnomish"; break;
    case 14: tongue = "Troll"; break;
    case 33: tongue = "Gutterspeak"; break;
    case 35: tongue = "Draenei"; break;
    default: break;
    }
    if (type == 0x11 && !channel.empty()) {
        std::string quoted = "\"";
        for (char c : channel) {
            if (c == '\\' || c == '"') quoted.push_back('\\');
            if (c == '\n' || c == '\r') continue;
            quoted.push_back(c);
        }
        quoted.push_back('"');
        script(std::string("if ChatFrame1 and ChatFrame1.channelList then\n"
                           "  local name = ") +
               quoted +
               "\n  local have = false\n"
               "  for _, v in pairs(ChatFrame1.channelList) do\n"
               "    if strupper(v) == strupper(name) then have = true end\n"
               "  end\n"
               "  if not have then table.insert(ChatFrame1.channelList, name) end\n"
               "end\n");
    }
    for (auto& arg : body_->fired) {
        arg.number = false;
        arg.n = 0;
        arg.text.clear();
    }
    body_->fired[0].text = text;
    body_->fired[1].text = sender;
    body_->fired[2].text = tongue;
    body_->fired[3].text = type == 0x11 ? std::string("1. ") + channel : std::string();
    body_->fired[6].number = true;
    body_->fired[7].number = true;
    body_->fired[7].n = type == 0x11 ? 1 : 0;
    body_->fired[8].text = channel;
    body_->fired[9].number = true;
    body_->fired[10].number = true;
    body_->keepArgs = true;
    fire(event);
}

bool FrameXml::click(float x, float y) {
    pressed_ = 0;
    int blocker = 0;
    int best = 0;
    float bestArea = 0.f;
    for (int i = int(body_->widgets.size()) - 1; i >= 1; --i) {
        Widget& widget = body_->widgets[i];
        if (!widget.mouse || widget.w < 1.f || widget.h < 1.f || !shownChain(body_->widgets, i)) continue;
        if (x < widget.x || y < widget.y || x > widget.x + widget.w || y > widget.y + widget.h) continue;
        const bool script = widget.onClick != LUA_NOREF || widget.onMouseDown != LUA_NOREF;
        if (!script) {
            if (!blocker) blocker = i;
            continue;
        }
        const float area = widget.w * widget.h;
        if (!best || area < bestArea) {
            best = i;
            bestArea = area;
        }
    }
    if (best) {
        Widget& widget = body_->widgets[best];
        pressed_ = best;
        if (widget.onMouseDown != LUA_NOREF) callRef(body_->L, widget.onMouseDown, best, nullptr);
        if (widget.onClick != LUA_NOREF) callRef(body_->L, widget.onClick, best, nullptr);
        publish();
        return true;
    }
    if (blocker) {
        pressed_ = blocker;
        return true;
    }
    return false;
}

bool FrameXml::release(float x, float y) {
    const int id = pressed_;
    pressed_ = 0;
    if (id <= 0 || id >= int(body_->widgets.size())) return false;
    Widget& widget = body_->widgets[id];
    if (!widget.mouse || x < widget.x || y < widget.y || x > widget.x + widget.w || y > widget.y + widget.h) return false;
    if (widget.onMouseUp != LUA_NOREF) callRef(body_->L, widget.onMouseUp, id, nullptr);
    publish();
    return widget.onMouseUp != LUA_NOREF;
}

std::string FrameXml::textOf(const std::string& name) const {
    const auto it = body_->names.find(name);
    if (it == body_->names.end()) return {};
    return body_->widgets[it->second].text;
}

bool FrameXml::addOnInfo(int index, std::string& name, std::string& title, bool& loaded, bool& demand) const {
    if (index < 1 || index > int(addons_.size())) return false;
    const AddonRec& addon = addons_[size_t(index - 1)];
    name = addon.name;
    title = addon.title;
    loaded = addon.loaded;
    demand = addon.demand;
    return true;
}

bool FrameXml::isAddOnLoaded(const std::string& name) const {
    for (const AddonRec& addon : addons_)
        if (addon.name == name) return addon.loaded;
    return false;
}

}  // namespace reclient
