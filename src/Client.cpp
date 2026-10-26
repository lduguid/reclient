#include "Client.hpp"

#include "Diag.hpp"
#include "Opcodes.hpp"
#include "Srp6.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace reclient {
namespace {
const char* gossipKind(uint8_t icon) {
    switch (icon) {
    case 1: return "vendor";
    case 2: return "taxi";
    case 3: return "trainer";
    case 4:
    case 5: return "binder";
    case 6: return "banker";
    case 7: return "petition";
    case 8: return "tabard";
    case 9: return "battlemaster";
    case 10: return "auctioneer";
    default: return "gossip";
    }
}

void replaceToken(std::string& text, const std::string& token, const std::string& value) {
    for (size_t at = 0; (at = text.find(token, at)) != std::string::npos; at += value.size())
        text.replace(at, token.size(), value);
}

std::string jsonEscape(std::string_view text) {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string valueAfter(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const size_t at = json.find(needle);
    if (at == std::string::npos) return {};
    size_t colon = json.find(':', at + needle.size());
    if (colon == std::string::npos) return {};
    ++colon;
    while (colon < json.size() && std::isspace(static_cast<unsigned char>(json[colon]))) ++colon;
    if (colon >= json.size()) return {};
    if (json[colon] == '"') {
        std::string out;
        for (size_t i = colon + 1; i < json.size(); ++i) {
            if (json[i] == '\\' && i + 1 < json.size()) {
                out.push_back(json[++i]);
                continue;
            }
            if (json[i] == '"') break;
            out.push_back(json[i]);
        }
        return out;
    }
    size_t end = colon;
    while (end < json.size() && std::string(",}\n\r ").find(json[end]) == std::string::npos) ++end;
    return json.substr(colon, end - colon);
}

uint8_t readU8(Socket& sock) {
    uint8_t b = 0;
    if (!sock.recvExact(&b, 1, 20000)) throw std::runtime_error("auth server timed out");
    return b;
}

std::vector<uint8_t> readN(Socket& sock, size_t n) {
    std::vector<uint8_t> v(n);
    if (n && !sock.recvExact(v.data(), n, 20000)) throw std::runtime_error("auth server timed out");
    return v;
}

std::string opcodeName(uint16_t opcode) {
    if (opcode < kOpcodeCount) return kOpcodes[opcode].name;
    return "OPCODE_" + std::to_string(opcode);
}

bool asked(const std::vector<uint64_t>& list, uint64_t guid) {
    return std::find(list.begin(), list.end(), guid) != list.end();
}
bool asked32(const std::vector<uint32_t>& list, uint32_t id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}
}  // namespace

Config loadConfig(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("could not open config " + path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string json = buffer.str();
    Config cfg;
    if (auto host = valueAfter(json, "host"); !host.empty()) cfg.host = host;
    if (auto account = valueAfter(json, "account"); !account.empty()) cfg.account = account;
    if (auto password = valueAfter(json, "gamePassword"); !password.empty()) cfg.password = password;
    if (auto character = valueAfter(json, "character"); !character.empty()) cfg.character = character;
    if (auto client = valueAfter(json, "client"); !client.empty()) cfg.clientPath = client;
    if (auto port = valueAfter(json, "authPort"); !port.empty()) cfg.authPort = std::stoi(port);
    if (auto port = valueAfter(json, "controlPort"); !port.empty()) cfg.controlPort = std::stoi(port);
    auto flag = [](const std::string& value) { return value == "true" || value == "1" || value == "TRUE"; };
    if (auto diag = valueAfter(json, "diagnostics"); !diag.empty()) cfg.diagnostics = flag(diag);
    if (auto strict = valueAfter(json, "diagnosticsStrict"); !strict.empty()) cfg.diagnosticsStrict = flag(strict);
    if (const char* env = std::getenv("RECLIENT_DIAGNOSTICS")) cfg.diagnostics = std::string(env) != "0";
    if (const char* env = std::getenv("RECLIENT_GAME_PASSWORD")) {
        if (cfg.password.empty()) cfg.password = env;
    }
    return cfg;
}

GameClient::GameClient(Config config) : config_(std::move(config)) {}

void GameClient::log(const std::string& line) {
    if (logger_) logger_(line);
}

void GameClient::note(const std::string& line) {
    discrepancies_.push_back(line);
    log("discrepancy: " + line);
}

void GameClient::enqueue(std::string command) {
    std::lock_guard<std::mutex> lock(queueMu_);
    queue_.push_back(Job{std::move(command), nullptr});
}

std::string GameClient::submit(std::string command) {
    auto reply = std::make_shared<std::promise<std::string>>();
    auto future = reply->get_future();
    {
        std::lock_guard<std::mutex> lock(queueMu_);
        queue_.push_back(Job{std::move(command), reply});
    }
    if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) return "timeout";
    return future.get();
}

uint32_t GameClient::nowMs() const {
#ifdef _WIN32
    return GetTickCount();
#else
    using namespace std::chrono;
    return uint32_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
#endif
}

void GameClient::loginRealm() {
    if (config_.account.empty() || config_.password.empty())
        throw std::runtime_error("account and gamePassword are required (config or RECLIENT_GAME_PASSWORD)");
    log("auth connect " + config_.host + ":" + std::to_string(config_.authPort));
    auth_.connect(config_.host, config_.authPort);

    ByteBuffer challenge;
    challenge.write<uint8_t>(0x00);
    challenge.write<uint8_t>(0x00);
    const std::string account = [&] {
        std::string s = config_.account;
        for (char& c : s)
            if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        return s;
    }();
    config_.account = account;
    const uint16_t body = uint16_t(30 + account.size());
    challenge.write<uint16_t>(body);
    challenge.append(reinterpret_cast<const uint8_t*>("WoW"), 4);
    challenge.write<uint8_t>(2);
    challenge.write<uint8_t>(4);
    challenge.write<uint8_t>(3);
    challenge.write<uint16_t>(8606);
    challenge.append(reinterpret_cast<const uint8_t*>("68x"), 3);
    challenge.write<uint8_t>(0);
    challenge.append(reinterpret_cast<const uint8_t*>("niW"), 3);
    challenge.write<uint8_t>(0);
    challenge.append(reinterpret_cast<const uint8_t*>("SUne"), 4);
    challenge.write<uint32_t>(0);
    challenge.write<uint32_t>(0);
    challenge.write<uint8_t>(uint8_t(account.size()));
    challenge.append(reinterpret_cast<const uint8_t*>(account.data()), account.size());
    auth_.sendAll(challenge.raw());

    const uint8_t cmd = readU8(auth_);
    const uint8_t unk = readU8(auth_);
    const uint8_t result = readU8(auth_);
    (void)unk;
    if (cmd != 0x00 || result != 0)
        throw std::runtime_error("logon challenge rejected, result " + std::to_string(result));
    const auto B = readN(auth_, 32);
    const uint8_t gLen = readU8(auth_);
    const auto g = readN(auth_, gLen);
    const uint8_t nLen = readU8(auth_);
    const auto N = readN(auth_, nLen);
    const auto salt = readN(auth_, 32);
    (void)readN(auth_, 16);
    const uint8_t security = readU8(auth_);
    if (security & 0x04) {
        (void)readU8(auth_);
        throw std::runtime_error("this account has an authenticator; the headless client cannot answer it");
    }
    if (security & 0x01) throw std::runtime_error("PIN login is not implemented");

    const Srp6Session session = srp6Client(account, config_.password, B, g, N, salt);
    sessionKey_ = session.K;

    ByteBuffer proof;
    proof.write<uint8_t>(0x01);
    proof.append(session.A.data(), session.A.size());
    proof.append(session.M1.data(), session.M1.size());
    uint8_t zeros[20]{};
    proof.append(zeros, 20);
    proof.write<uint8_t>(0);
    proof.write<uint8_t>(0);
    auth_.sendAll(proof.raw());

    const uint8_t proofCmd = readU8(auth_);
    const uint8_t proofResult = readU8(auth_);
    if (proofCmd != 0x01 || proofResult != 0)
        throw std::runtime_error("logon proof rejected, result " + std::to_string(proofResult));
    const auto m2 = readN(auth_, 20);
    (void)readN(auth_, 4);
    (void)readN(auth_, 4);
    (void)readN(auth_, 2);
    if (!std::equal(m2.begin(), m2.end(), session.M2.begin()))
        note("server M2 does not match the locally derived session proof");
    log("auth ok, session key " + std::to_string(sessionKey_.size()) + " bytes");
    if (diag::enabled()) diag::event("session.auth", "realm accepted");

    ByteBuffer realmReq;
    realmReq.write<uint8_t>(0x10);
    realmReq.write<uint32_t>(0);
    auth_.sendAll(realmReq.raw());
    const uint8_t realmCmd = readU8(auth_);
    if (realmCmd != 0x10) throw std::runtime_error("unexpected realm list command");
    const auto sizeBytes = readN(auth_, 2);
    const uint16_t realmSize = uint16_t(sizeBytes[0] | (sizeBytes[1] << 8));
    auto realmBytes = readN(auth_, realmSize);
    ByteBuffer realmBuf(std::move(realmBytes));
    realms_ = parseRealmList(realmBuf);
    if (realms_.empty()) throw std::runtime_error("realm list was empty");
    for (const Realm& realm : realms_)
        log("realm " + realm.name + " @ " + realm.address + " characters " + std::to_string(realm.characters));
    auth_.close();
}

void GameClient::sendWorld(uint16_t opcode, const ByteBuffer& body) {
    uint8_t header[6];
    const uint16_t size = uint16_t(body.size() + 4);
    header[0] = uint8_t((size >> 8) & 0xff);
    header[1] = uint8_t(size & 0xff);
    const uint32_t opcode32 = opcode;
    header[2] = uint8_t(opcode32 & 0xff);
    header[3] = uint8_t((opcode32 >> 8) & 0xff);
    header[4] = uint8_t((opcode32 >> 16) & 0xff);
    header[5] = uint8_t((opcode32 >> 24) & 0xff);
    if (crypt_.ready()) crypt_.encryptSend(header, 6);
    world_.sendAll(header, 6);
    if (!body.raw().empty()) world_.sendAll(body.raw());
}

bool GameClient::pollWorld(int timeoutMs) {
    uint8_t header[4];
    if (!world_.recvExact(header, 4, timeoutMs)) return false;
    if (crypt_.ready()) crypt_.decryptRecv(header, 4);
    const uint16_t size = uint16_t((uint16_t(header[0]) << 8) | header[1]);
    const uint16_t opcode = uint16_t(header[2] | (uint16_t(header[3]) << 8));
    if (size < 2 || size > 0x7FFF) throw std::runtime_error("world packet size " + std::to_string(size));
    const size_t bodyLen = size_t(size - 2);
    std::vector<uint8_t> raw(bodyLen);
    if (bodyLen && !world_.recvExact(raw.data(), bodyLen, 20000))
        throw std::runtime_error("world packet body timed out");
    ByteBuffer body(std::move(raw));
    onWorld(opcode, body);
    return true;
}

void GameClient::sendMove(uint16_t opcode) {
    self_.time = nowMs();
    ByteBuffer body;
    writeMovement(body, self_);
    sendWorld(opcode, body);
}

void GameClient::requestNames() {
    for (const auto& [guid, object] : objects_.all()) {
        if (object.typeId == 4 && object.name.empty() && guid != selfGuid_ && !asked(nameAsked_, guid)) {
            ByteBuffer body;
            body.write<uint64_t>(guid);
            sendWorld(CMSG_NAME_QUERY, body);
            nameAsked_.push_back(guid);
        } else if (object.typeId == 3 && object.name.empty() && object.entry() != 0 &&
                   !asked32(creatureAsked_, object.entry())) {
            ByteBuffer body;
            body.write<uint32_t>(object.entry());
            body.write<uint64_t>(guid);
            sendWorld(CMSG_CREATURE_QUERY, body);
            creatureAsked_.push_back(object.entry());
        } else if (object.typeId == 5 && object.name.empty() && object.entry() != 0 &&
                   !asked32(objectAsked_, object.entry())) {
            ByteBuffer body;
            body.write<uint32_t>(object.entry());
            body.write<uint64_t>(guid);
            sendWorld(CMSG_GAMEOBJECT_QUERY, body);
            objectAsked_.push_back(object.entry());
        }
    }
}

void GameClient::onWorld(uint16_t opcode, ByteBuffer& body) {
    try {
        switch (opcode) {
            case SMSG_AUTH_CHALLENGE: {
                const uint32_t serverSeed = body.read<uint32_t>();
                std::random_device rd;
                clientSeed_ = rd();
                const Digest20 digest = worldAuthDigest(config_.account, clientSeed_, serverSeed, sessionKey_);
                ByteBuffer auth;
                auth.write<uint32_t>(8606);
                auth.write<uint32_t>(0);
                auth.writeCString(config_.account);
                auth.write<uint32_t>(clientSeed_);
                auth.append(digest.data(), digest.size());
                auth.append(buildAddonInfo());
                sendWorld(CMSG_AUTH_SESSION, auth);
                crypt_.init(sessionKey_);
                log("world auth session sent");
                break;
            }
            case SMSG_AUTH_RESPONSE: {
                const uint8_t code = body.read<uint8_t>();
                log("auth response " + std::to_string(code));
                if (diag::enabled())
                    diag::check("session.world-auth", code == 0x0C || code == 0x1B, "code " + std::to_string(code));
                if (code == 0x1B) {
                    log("server queued the login");
                    break;
                }
                if (code != 0x0C) throw std::runtime_error("world auth failed, code " + std::to_string(code));
                authed_ = true;
                ByteBuffer empty;
                sendWorld(CMSG_CHAR_ENUM, empty);
                break;
            }
            case SMSG_ADDON_INFO:
                log("addon info " + std::to_string(body.size()) + " bytes");
                break;
            case SMSG_CHAR_CREATE: {
                const uint8_t result = body.read<uint8_t>();
                log("char create result " + std::to_string(result));
                if (result == 0x2F) {
                    ByteBuffer again;
                    sendWorld(CMSG_CHAR_ENUM, again);
                } else {
                    note("character create failed with code " + std::to_string(result));
                }
                break;
            }
            case SMSG_WARDEN_DATA:
                onWarden(body);
                break;
            case SMSG_CHAR_ENUM: {
                characters_ = parseCharEnum(body);
                log("character list " + std::to_string(characters_.size()));
                if (diag::enabled()) {
                    diag::event("session.characters", std::to_string(characters_.size()));
                    diag::check("session.characters", !characters_.empty() || wardenSeen_,
                                "count " + std::to_string(characters_.size()));
                }
                for (const Character& c : characters_)
                    log("character " + c.name + " level " + std::to_string(c.level) + " map " +
                        std::to_string(c.map));
                if (!enterName_.empty() && !entered_) {
                    enterWorld(enterName_);
                    entered_ = true;
                }
                break;
            }
            case SMSG_LOGIN_VERIFY_WORLD: {
                mapId_ = body.read<uint32_t>();
                self_.x = body.read<float>();
                self_.y = body.read<float>();
                self_.z = body.read<float>();
                self_.o = body.read<float>();
                inWorld_ = true;
                log("in world map " + std::to_string(mapId_) + " at " + std::to_string(self_.x) + " " +
                    std::to_string(self_.y) + " " + std::to_string(self_.z));
                if (diag::enabled()) {
                    const bool finite = std::isfinite(self_.x) && std::isfinite(self_.y) && std::isfinite(self_.z);
                    diag::check("session.world", finite, "map " + std::to_string(mapId_));
                    diag::event("session.world", "map " + std::to_string(mapId_) + " " + std::to_string(self_.x) + " " +
                                                     std::to_string(self_.y) + " " + std::to_string(self_.z));
                }
                if (!moverSent_ && selfGuid_) {
                    ByteBuffer mover;
                    mover.write<uint64_t>(selfGuid_);
                    sendWorld(CMSG_SET_ACTIVE_MOVER, mover);
                    moverSent_ = true;
                }
                sendMove(MSG_MOVE_HEARTBEAT);
                break;
            }
            case SMSG_NEW_WORLD: {
                mapId_ = body.read<uint32_t>();
                self_.x = body.read<float>();
                self_.y = body.read<float>();
                self_.z = body.read<float>();
                self_.o = body.read<float>();
                ByteBuffer ack;
                sendWorld(MSG_MOVE_WORLDPORT_ACK, ack);
                log("new world map " + std::to_string(mapId_));
                break;
            }
            case SMSG_UPDATE_OBJECT: {
                std::string error;
                if (!objects_.applyUpdate(body.data(), body.size(), error))
                    note("update object parse: " + error);
                requestNames();
                break;
            }
            case SMSG_COMPRESSED_UPDATE_OBJECT: {
                const uint32_t full = body.read<uint32_t>();
                const auto inflated = inflateZlib(body.data() + (body.size() - body.remaining()), body.remaining(), full);
                std::string error;
                if (!objects_.applyUpdate(inflated.data(), inflated.size(), error))
                    note("compressed update parse: " + error);
                requestNames();
                break;
            }
            case SMSG_DESTROY_OBJECT: {
                objects_.destroy(body.read<uint64_t>());
                break;
            }
            case SMSG_MONSTER_MOVE:
            case SMSG_MONSTER_MOVE_TRANSPORT: {
                const uint64_t guid = body.readPackedGuid();
                if (opcode == SMSG_MONSTER_MOVE_TRANSPORT) (void)body.readPackedGuid();
                if (body.remaining() < 12) break;
                const float x = body.read<float>();
                const float y = body.read<float>();
                const float z = body.read<float>();
                WowObject* object = objects_.find(guid);
                if (!object) break;
                object->move.x = x;
                object->move.y = y;
                object->move.z = z;
                object->move.splineLive = false;
                if (body.remaining() < 5) break;
                const uint32_t splineId = body.read<uint32_t>();
                const uint8_t kind = body.read<uint8_t>();
                if (kind == 2 && body.remaining() >= 12) {
                    (void)body.read<float>();
                    (void)body.read<float>();
                    (void)body.read<float>();
                } else if (kind == 3 && body.remaining() >= 8) {
                    (void)body.read<uint64_t>();
                } else if (kind == 4 && body.remaining() >= 4) {
                    object->move.o = body.read<float>();
                }
                // A stop has no path. Anything else names how long the walk takes and where it ends.
                if (kind == 1 || body.remaining() < 8) {
                    object->move.splineId = splineId;
                    break;
                }
                const uint32_t splineFlags = body.read<uint32_t>();
                const uint32_t duration = body.read<uint32_t>();
                if (body.remaining() < 16 || duration == 0 || duration > 120000) {
                    object->move.splineId = splineId;
                    break;
                }
                (void)body.read<uint32_t>();
                object->move.splineX = body.read<float>();
                object->move.splineY = body.read<float>();
                object->move.splineZ = body.read<float>();
                object->move.splineMs = duration;
                object->move.splineFlags = splineFlags;
                object->move.splineId = splineId;
                object->move.splineLive = true;
                object->move.flags |= kMoveSplineEnabled;
                break;
            }
            case SMSG_NAME_QUERY_RESPONSE: {
                const uint64_t guid = body.read<uint64_t>();
                const std::string name = body.readCString();
                objects_.rename(guid, name);
                log("name " + name);
                break;
            }
            case SMSG_CREATURE_QUERY_RESPONSE: {
                const uint32_t entry = body.read<uint32_t>();
                if (entry & 0x80000000) break;
                const std::string name = body.readCString();
                objects_.renameEntry(3, entry, name);
                break;
            }
            case SMSG_GAMEOBJECT_QUERY_RESPONSE: {
                const uint32_t entry = body.read<uint32_t>();
                if (entry & 0x80000000) break;
                (void)body.read<uint32_t>();
                (void)body.read<uint32_t>();
                const std::string name = body.readCString();
                objects_.renameEntry(5, entry, name);
                break;
            }
            case SMSG_MESSAGECHAT:
            case SMSG_GM_MESSAGECHAT: {
                try {
                    ChatLine line = parseChat(body);
                    if (line.sender.empty()) {
                        if (line.senderGuid == selfGuid_ && !enterName_.empty()) line.sender = enterName_;
                        else if (const WowObject* who = objects_.find(line.senderGuid)) line.sender = who->name;
                    }
                    if (line.sender.empty()) line.sender = "someone";
                    line.seq = ++chatSeq_;
                    chat_.push_back(line);
                    if (chat_.size() > 40) chat_.erase(chat_.begin());
                    log("chat [" + line.sender + "] " + line.text);
                } catch (const std::exception& ex) {
                    note(std::string("chat parse: ") + ex.what());
                }
                break;
            }
            case SMSG_GOSSIP_MESSAGE: {
                const uint32_t serial = gossip_.serial + 1;
                gossip_ = GossipMenu{};
                gossip_.open = true;
                gossip_.serial = serial;
                gossip_.guid = body.read<uint64_t>();
                gossip_.menu = body.read<uint32_t>();
                gossip_.textId = body.read<uint32_t>();
                uint32_t count = body.read<uint32_t>();
                if (count > 32) count = 32;
                for (uint32_t i = 0; i < count; ++i) {
                    GossipChoice choice;
                    choice.id = body.read<uint32_t>();
                    choice.kind = gossipKind(body.read<uint8_t>());
                    choice.coded = body.read<uint8_t>() != 0;
                    (void)body.read<uint32_t>();
                    choice.text = body.readCString();
                    (void)body.readCString();
                    gossip_.choices.push_back(std::move(choice));
                }
                if (gossip_.textId != 0) {
                    ByteBuffer query;
                    query.write<uint32_t>(gossip_.textId);
                    query.write<uint64_t>(gossip_.guid);
                    sendWorld(CMSG_NPC_TEXT_QUERY, query);
                }
                log("gossip options " + std::to_string(gossip_.choices.size()));
                break;
            }
            case SMSG_NPC_TEXT_UPDATE: {
                const uint32_t textId = body.read<uint32_t>();
                std::string best;
                for (int i = 0; i < 8 && body.remaining() > 0; ++i) {
                    (void)body.read<float>();
                    const std::string male = body.readCString();
                    const std::string female = body.readCString();
                    (void)body.read<uint32_t>();
                    for (int emote = 0; emote < 6; ++emote) (void)body.read<uint32_t>();
                    if (best.empty()) best = !male.empty() ? male : female;
                }
                if (!gossip_.open || (gossip_.textId != 0 && textId != gossip_.textId)) break;
                const std::string who = enterName_.empty() ? "you" : enterName_;
                replaceToken(best, "$N", who);
                replaceToken(best, "$n", who);
                replaceToken(best, "$B", "\n");
                replaceToken(best, "$b", "\n");
                gossip_.text = std::move(best);
                ++gossip_.serial;
                break;
            }
            case SMSG_GOSSIP_COMPLETE:
                gossip_.open = false;
                gossip_.choices.clear();
                break;
            case SMSG_PONG:
                break;
            case SMSG_TIME_SYNC_REQ: {
                const uint32_t counter = body.read<uint32_t>();
                ByteBuffer resp;
                resp.write<uint32_t>(counter);
                resp.write<uint32_t>(nowMs());
                sendWorld(CMSG_TIME_SYNC_RESP, resp);
                break;
            }
            case MSG_MOVE_TELEPORT_ACK: {
                (void)body.readPackedGuid();
                const uint32_t counter = body.read<uint32_t>();
                Movement dest = self_;
                readMovement(body, dest);
                self_.x = dest.x;
                self_.y = dest.y;
                self_.z = dest.z;
                self_.o = dest.o;
                ByteBuffer ack;
                ack.write<uint64_t>(selfGuid_);
                ack.write<uint32_t>(counter);
                ack.write<uint32_t>(nowMs());
                sendWorld(MSG_MOVE_TELEPORT_ACK, ack);
                log("teleport ack to " + std::to_string(self_.x) + " " + std::to_string(self_.y));
                break;
            }
            case SMSG_FORCE_RUN_SPEED_CHANGE:
            case SMSG_FORCE_RUN_BACK_SPEED_CHANGE:
            case SMSG_FORCE_SWIM_SPEED_CHANGE:
            case SMSG_FORCE_WALK_SPEED_CHANGE:
            case SMSG_FORCE_SWIM_BACK_SPEED_CHANGE:
            case SMSG_FORCE_TURN_RATE_CHANGE:
            case SMSG_FORCE_FLIGHT_SPEED_CHANGE:
            case SMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE: {
                (void)body.readPackedGuid();
                const uint32_t counter = body.read<uint32_t>();
                if (opcode == SMSG_FORCE_RUN_SPEED_CHANGE && body.remaining() > 4) (void)body.read<uint8_t>();
                const float speed = body.read<float>();
                uint16_t ackOp = CMSG_FORCE_RUN_SPEED_CHANGE_ACK;
                if (opcode == SMSG_FORCE_RUN_BACK_SPEED_CHANGE) ackOp = CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK;
                if (opcode == SMSG_FORCE_SWIM_SPEED_CHANGE) ackOp = CMSG_FORCE_SWIM_SPEED_CHANGE_ACK;
                if (opcode == SMSG_FORCE_WALK_SPEED_CHANGE) ackOp = CMSG_FORCE_WALK_SPEED_CHANGE_ACK;
                if (opcode == SMSG_FORCE_SWIM_BACK_SPEED_CHANGE) ackOp = CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK;
                if (opcode == SMSG_FORCE_TURN_RATE_CHANGE) ackOp = CMSG_FORCE_TURN_RATE_CHANGE_ACK;
                if (opcode == SMSG_FORCE_FLIGHT_SPEED_CHANGE) ackOp = CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK;
                if (opcode == SMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE) ackOp = CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK;
                if (opcode == SMSG_FORCE_RUN_SPEED_CHANGE) self_.run = speed;
                ByteBuffer ack;
                ack.write<uint64_t>(selfGuid_);
                ack.write<uint32_t>(counter);
                writeMovement(ack, self_);
                ack.write<float>(speed);
                sendWorld(ackOp, ack);
                break;
            }
            default:
                break;
        }
        if (inWorld_ && selfGuid_ == 0) {
            for (const auto& [guid, object] : objects_.all()) {
                if (object.typeId == 4 && (object.move.x != 0 || object.move.y != 0)) {
                    // Prefer the player whose position matches the login point.
                    const float dx = object.move.x - self_.x;
                    const float dy = object.move.y - self_.y;
                    if (dx * dx + dy * dy < 4.0f) {
                        selfGuid_ = guid;
                        self_ = object.move;
                    }
                }
            }
        }
    } catch (const PacketError& ex) {
        note(opcodeName(opcode) + std::string(" parse: ") + ex.what());
    }
}

void GameClient::enterWorld(const std::string& name) {
    auto fold = [](std::string value) {
        for (char& c : value) c = char(std::tolower(static_cast<unsigned char>(c)));
        return value;
    };
    const std::string want = fold(name);
    const Character* found = nullptr;
    for (const Character& c : characters_) {
        if (fold(c.name) == want) found = &c;
    }
    if (!found) throw std::runtime_error("no character named " + name);
    selfGuid_ = found->guid;
    self_.x = found->x;
    self_.y = found->y;
    self_.z = found->z;
    ByteBuffer body;
    body.write<uint64_t>(found->guid);
    sendWorld(CMSG_PLAYER_LOGIN, body);
    log("entering " + name);
}

std::string GameClient::execute(const std::string& command) {
    const std::string line = trim(command);
    if (line.empty()) return "ok";
    std::istringstream in(line);
    std::string verb;
    in >> verb;
    for (char& c : verb) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (verb == "quit" || verb == "exit") {
        quit_ = true;
        return "bye";
    }
    if (verb == "wait") {
        double seconds = 1;
        in >> seconds;
        pendingWaitMs_ = int(seconds * 1000);
        return "waiting";
    }
    if (verb == "chars" || verb == "characters") {
        std::ostringstream out;
        out << "[";
        for (size_t i = 0; i < characters_.size(); ++i) {
            if (i) out << ",";
            out << "{\"name\":\"" << jsonEscape(characters_[i].name) << "\",\"level\":" << int(characters_[i].level)
                << ",\"map\":" << characters_[i].map << "}";
        }
        out << "]";
        log(out.str());
        return out.str();
    }
    if (verb == "create") {
        std::string name;
        int race = 1, klass = 1, gender = 0;
        in >> name >> race >> klass >> gender;
        ByteBuffer body;
        body.writeCString(name);
        body.write<uint8_t>(uint8_t(race));
        body.write<uint8_t>(uint8_t(klass));
        body.write<uint8_t>(uint8_t(gender));
        body.write<uint8_t>(0);
        body.write<uint8_t>(0);
        body.write<uint8_t>(0);
        body.write<uint8_t>(0);
        body.write<uint8_t>(0);
        body.write<uint8_t>(0);
        sendWorld(CMSG_CHAR_CREATE, body);
        enterName_ = name;
        entered_ = false;
        return "creating " + name;
    }
    if (verb == "enter") {
        std::string name;
        in >> name;
        if (name.empty()) name = config_.character;
        enterName_ = name;
        if (characters_.empty()) return "enter queued until the character list arrives";
        if (!entered_) {
            enterWorld(name);
            entered_ = true;
        }
        return "entering " + name;
    }
    if (verb == "nearby") {
        std::ostringstream out;
        out << "[";
        bool first = true;
        int shown = 0;
        for (const auto& [guid, object] : objects_.all()) {
            if (object.typeId != 3 && object.typeId != 4 && object.typeId != 5) continue;
            if (!first) out << ",";
            first = false;
            const char* kind = object.typeId == 4 ? "player" : object.typeId == 3 ? "creature" : "object";
            out << "{\"guid\":" << guid << ",\"kind\":\"" << kind << "\",\"entry\":" << object.entry()
                << ",\"name\":\"" << jsonEscape(object.name) << "\",\"x\":" << object.move.x << ",\"y\":" << object.move.y
                << ",\"z\":" << object.move.z << ",\"level\":" << object.level() << "}";
            if (++shown >= 40) break;
        }
        out << "]";
        log("nearby " + std::to_string(shown));
        log(out.str());
        return out.str();
    }
    if (verb == "pos") {
        std::ostringstream out;
        out << "{\"map\":" << mapId_ << ",\"x\":" << self_.x << ",\"y\":" << self_.y << ",\"z\":" << self_.z
            << ",\"o\":" << self_.o << "}";
        log(out.str());
        return out.str();
    }
    if (verb == "say" || verb == "yell" || verb == "channel" || verb == "join" || verb == "select" || verb == "hello" ||
        verb == "option") {
        auto language = [&]() {
            uint32_t race = 1;
            if (const WowObject* self = objects_.find(selfGuid_)) race = self->fields.size() > kFieldBytes0 ? (self->fields[kFieldBytes0] & 255u) : 1;
            if (race == 10) return 10u;
            if (race == 2 || race == 5 || race == 6 || race == 8) return 1u;
            return 7u;
        };
        if (verb == "join") {
            std::string name;
            std::getline(in, name);
            name = trim(name);
            if (name.empty()) return "join needs a channel";
            if (std::find(channels_.begin(), channels_.end(), name) == channels_.end()) channels_.push_back(name);
            ByteBuffer body;
            body.write<uint32_t>(0);
            body.write<uint8_t>(0);
            body.write<uint8_t>(0);
            body.writeCString(name);
            body.writeCString("");
            sendWorld(CMSG_JOIN_CHANNEL, body);
            log("join " + name);
            return "joined";
        }
        if (verb == "select") {
            uint64_t guid = 0;
            in >> guid;
            ByteBuffer body;
            body.write<uint64_t>(guid);
            sendWorld(CMSG_SET_SELECTION, body);
            return "selected";
        }
        if (verb == "hello") {
            uint64_t guid = 0;
            in >> guid;
            ByteBuffer body;
            body.write<uint64_t>(guid);
            sendWorld(CMSG_GOSSIP_HELLO, body);
            log("hello " + std::to_string(guid));
            return "hello";
        }
        if (verb == "option") {
            int index = -1;
            in >> index;
            if (!gossip_.open || index < 0 || index >= int(gossip_.choices.size())) return "no gossip";
            const GossipChoice& choice = gossip_.choices[size_t(index)];
            ByteBuffer body;
            body.write<uint64_t>(gossip_.guid);
            body.write<uint32_t>(gossip_.menu);
            body.write<uint32_t>(choice.id);
            if (choice.coded) body.writeCString("");
            sendWorld(CMSG_GOSSIP_SELECT_OPTION, body);
            return "option";
        }
        std::string text;
        std::getline(in, text);
        text = trim(text);
        ByteBuffer body;
        if (verb == "channel") {
            const size_t bar = text.find('|');
            const std::string name = bar == std::string::npos ? text : text.substr(0, bar);
            const std::string msg = bar == std::string::npos ? std::string() : trim(text.substr(bar + 1));
            if (name.empty() || msg.empty()) return "channel needs a name and a message";
            body.write<uint32_t>(0x11);
            body.write<uint32_t>(language());
            body.writeCString(name);
            body.writeCString(msg);
            log("channel " + name + " " + msg);
        } else {
            body.write<uint32_t>(verb == "yell" ? 6u : 1u);
            body.write<uint32_t>(language());
            body.writeCString(text);
            log(verb + " " + text);
        }
        sendWorld(CMSG_MESSAGECHAT, body);
        return verb;
    }
    if (verb == "turn") {
        float delta = 0;
        in >> delta;
        self_.o += delta;
        sendMove(MSG_MOVE_HEARTBEAT);
        return "turned";
    }
    if (verb == "pace") {
        int on = 0;
        in >> on;
        walking_ = on != 0;
        return walking_ ? "walk" : "run";
    }
    if (verb == "move") {
        float dx = 0, dy = 0, dz = 0;
        in >> dx >> dy >> dz;
        self_.x += dx;
        self_.y += dy;
        self_.z += dz;
        if (diag::enabled()) {
            const float len = std::sqrt(dx * dx + dy * dy);
            diag::check("move.packet", len < 1.5f, "delta " + std::to_string(len));
        }
        self_.flags = kMoveForward;
        if (walking_) self_.flags |= kMoveWalk;
        sendMove(MSG_MOVE_START_FORWARD);
        self_.flags = 0;
        sendMove(MSG_MOVE_STOP);
        log("moved to " + std::to_string(self_.x) + " " + std::to_string(self_.y));
        return "moved";
    }
    if (verb == "jump") {
        float zspeed = 7.96f, xy = 0, cosine = 1, sine = 0;
        in >> zspeed >> xy >> cosine >> sine;
        self_.flags = kMoveFalling | kMoveJumping;
        if (walking_) self_.flags |= kMoveWalk;
        self_.fallTime = 0;
        self_.jumpZ = zspeed;
        self_.jumpCos = cosine;
        self_.jumpSin = sine;
        self_.jumpXy = xy;
        sendMove(MSG_MOVE_JUMP);
        log("jump");
        return "jumped";
    }
    if (verb == "land") {
        float x = self_.x, y = self_.y, z = self_.z, facing = self_.o;
        in >> x >> y >> z >> facing;
        self_.x = x;
        self_.y = y;
        self_.z = z;
        self_.o = facing;
        self_.flags = 0;
        self_.fallTime = 0;
        sendMove(MSG_MOVE_FALL_LAND);
        log("landed");
        return "landed";
    }
    if (verb == "status") {
        std::ostringstream out;
        out << "{\"account\":\"" << jsonEscape(config_.account) << "\",\"inWorld\":" << (inWorld_ ? "true" : "false")
            << ",\"characters\":" << characters_.size() << ",\"objects\":" << objects_.all().size()
            << ",\"chat\":" << chat_.size() << "}";
        log(out.str());
        return out.str();
    }
    return "unknown command " + verb;
}

void GameClient::onWarden(ByteBuffer& body) {
    if (!warden_.ready()) warden_.init(sessionKey_);
    std::vector<uint8_t> payload(body.data(), body.data() + body.size());
    if (!payload.empty()) warden_.decrypt(payload.data(), payload.size());
    const uint8_t opcode = payload.empty() ? 0xFF : payload[0];
    wardenSeen_ = true;
    if (opcode == 0) {
        log("warden offered a module");
        if (!wardenModuleRequested_) {
            uint8_t reply = 0;
            warden_.encrypt(&reply, 1);
            ByteBuffer out;
            out.append(&reply, 1);
            sendWorld(CMSG_WARDEN_DATA, out);
            wardenModuleRequested_ = true;
            note("Warden holds SMSG_CHAR_ENUM until it has read system info out of Wow.exe. "
                 "This client does not load or execute the uploaded module, so the character "
                 "list stays on the server. Turn Warden off for bring-up.");
        }
    } else if (opcode == 1 && payload.size() >= 3) {
        const uint16_t len = uint16_t(payload[1] | (payload[2] << 8));
        wardenModuleBytes_ += len;
        if (len < 500) log("warden module " + std::to_string(wardenModuleBytes_) + " bytes, not executed");
    } else {
        log("warden opcode " + std::to_string(opcode) + " size " + std::to_string(payload.size()));
    }
}

void GameClient::connectWorld() {
    const std::string address = realms_.empty() ? std::string() : realms_.front().address;
    std::string host = address;
    int port = 8085;
    const size_t colon = address.rfind(':');
    if (colon != std::string::npos) {
        host = address.substr(0, colon);
        port = std::stoi(address.substr(colon + 1));
    }
    log("world connect " + host + ":" + std::to_string(port));
    world_.connect(host, port);
}

void GameClient::connectSession() {
    loginRealm();
    connectWorld();
    const auto now = std::chrono::steady_clock::now();
    lastBeat_ = now;
    lastPing_ = now;
    nextWait_ = now;
    if (!config_.character.empty()) enterName_ = config_.character;
}

GameClient::View GameClient::view() const {
    View out;
    out.connected = authed_ || !realms_.empty();
    out.authed = authed_;
    out.inWorld = inWorld_;
    out.warden = wardenSeen_;
    out.account = config_.account;
    out.error = lastError_;
    out.clientPath = config_.clientPath;
    out.map = mapId_;
    out.self = self_;
    out.selfGuid = selfGuid_;
    out.characters = characters_;
    out.chat = chat_;
    out.channels = channels_;
    out.gossip = gossip_;
    return out;
}

bool GameClient::pump(int timeoutMs) {
    if (quit_) return false;
    try {
        pollWorld(timeoutMs);
        const auto now = std::chrono::steady_clock::now();
        if (inWorld_ && now - lastBeat_ > std::chrono::milliseconds(1000)) {
            sendMove(MSG_MOVE_HEARTBEAT);
            lastBeat_ = now;
        }
        if (authed_ && now - lastPing_ > std::chrono::seconds(30)) {
            ByteBuffer ping;
            ping.write<uint32_t>(nowMs());
            ping.write<uint32_t>(50);
            sendWorld(CMSG_PING, ping);
            lastPing_ = now;
        }
        if (pendingWaitMs_ > 0) {
            if (now < nextWait_) return true;
            pendingWaitMs_ = 0;
        }
        std::string command;
        std::shared_ptr<std::promise<std::string>> reply;
        {
            std::lock_guard<std::mutex> lock(queueMu_);
            if (!queue_.empty()) {
                command = std::move(queue_.front().command);
                reply = std::move(queue_.front().reply);
                queue_.erase(queue_.begin());
            }
        }
        if (!command.empty()) {
            std::string result;
            try {
                result = execute(command);
            } catch (const std::exception& ex) {
                result = std::string("error: ") + ex.what();
                log(result);
            }
            if (pendingWaitMs_ > 0) nextWait_ = now + std::chrono::milliseconds(pendingWaitMs_);
            if (reply) reply->set_value(result);
        }
    } catch (const std::exception& ex) {
        lastError_ = ex.what();
        log(lastError_);
        quit_ = true;
    }
    return !quit_;
}

int GameClient::run(std::vector<std::string> script, bool serve, bool stdinMode) {
    connectSession();

    size_t scriptAt = 0;
    std::thread input;
    if (stdinMode) {
        input = std::thread([this] {
            std::string line;
            while (!quit_ && std::getline(std::cin, line)) enqueue(line);
        });
    }
    while (!quit_) {
        if (pendingWaitMs_ == 0 && scriptAt < script.size()) {
            enqueue(script[scriptAt++]);
        } else if (pendingWaitMs_ == 0 && scriptAt >= script.size() && !serve && !stdinMode) {
            std::lock_guard<std::mutex> lock(queueMu_);
            if (queue_.empty()) quit_ = true;
        }
        if (!quit_) pump(40);
    }
    world_.close();
    if (input.joinable()) {
        // The process is exiting; detach a blocked stdin reader.
        input.detach();
    }
    return 0;
}

}  // namespace reclient
