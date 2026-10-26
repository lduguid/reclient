# Client / server discrepancies

The game client is the authority. Rows here are places CMaNGOS does something the client must match in order to connect, or places that still need a Ghidra check against `Wow.exe` 2.4.3.8606.

| Topic | What this client does | Why |
| --- | --- | --- |
| SRP bignum hashing | SHA-1 inputs for `u`, `x`, M1, and M2 drop trailing zero bytes of little-endian numbers | CMaNGOS `BigNumber::AsByteArray()` omits high zero bytes. A retail client may hash the fixed-width wire bytes instead. Confirm in Ghidra; a mismatch only matters when a value's top byte is zero. |
| Header key | One HMAC-SHA1 of the session key, seed `38 A7 83 15 F8 92 25 30 71 98 67 B1 8C 04 E2 AA`, used for both directions | TBC CMaNGOS derives a single key. Later clients use separate send and receive seeds. |
| Movement packets the client sends | `MovementInfo` only, no leading guid | `WorldSession::HandleMovementOpcodes` reads the movement block directly. The server adds a packed guid when it relays the packet. |
| Forced speed change | Server packet is packed guid, counter, an extra `uint8` on run speed, then the float. The acknowledgement is a full guid, counter, `MovementInfo`, and the float | That is the pair of writers in `Unit::SetSpeedRate` and `HandleForceSpeedChangeAckOpcodes`. |
| Teleport | The server sends `MSG_MOVE_TELEPORT_ACK` (packed guid, counter, movement). The client replies with a full guid, the counter, and a client timestamp | `Unit::SendTeleportPacket` and `HandleMoveTeleportAckOpcode`. |
| Version check | The logon proof's client-checksum field is zeros | CMaNGOS skips it unless `StrictVersionCheck` is on. |
| Warden | The handshake is decrypted and `MODULE_MISSING` is answered. The uploaded module is not executed | Namreeb's Warden holds `SMSG_CHAR_ENUM` until a memory read of `Wow.exe` returns `SYSTEM_INFO`. Leave Warden off while the session is brought up. |
| ADT / WDT / WMO chunk ids | Compared as a little-endian fourcc, so the on-disk bytes read backwards from the documented name | `Azeroth.wdt` from this install begins `REVM`, which is `MVER` stored as a uint32. |

## Live session, 2026-10-24

Against `192.168.50.43:3724` with the default `PLAYER` account:

- Logon challenge, proof, and M2 matched. Session key is 40 bytes.
- Realm list returned `MaNGOS` at `38.141.210.85:8085`.
- World auth returned `SMSG_AUTH_RESPONSE` 0x0C after the addon info packet.
- `CMSG_CHAR_CREATE` for a level-1 human returned 0x2F. The character row is in `tbccharacters`.
- The same session then received a Warden module offer. The decrypted opcode was 0, the module transfer was 19027 bytes, and no `SMSG_CHAR_ENUM` followed. The character list stays on the server until Warden is disabled or a real module is running inside `Wow.exe`.
