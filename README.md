# reclient

Original C++ client for World of Warcraft 2.4.3 (build 8606). It speaks to a Burning Crusade server such as CMaNGOS and reads game data from a WoW installation you already own. The repository does not contain the game client, MPQ archives, or extracted assets.

## Build

CMake, Visual Studio 2022, and Ninja. Lua 5.1.5 and the SDL3 development kit are vendored under `third_party/` (Lua because FrameXML is 5.1, SDL3 for the window). Crypto and zlib are in-tree (`miniz`) so the login path does not need OpenSSL. `vcpkg.json` lists StormLib, zlib, OpenSSL, and SDL3 for a later manifest install; the current build does not run vcpkg.

```
cmake -S . -B build -G Ninja
cmake --build build
build\reclient_tests.exe
build\reclient_ui.exe
build\reclient_formats.exe
```

## Headless session

Copy `config.example.json` to `config.json` and set the account and game password. The game password can also be passed as `RECLIENT_GAME_PASSWORD`. Database passwords are not used.

```
build\reclient.exe --config config.json --serve --stdin
```

`--serve` listens on `127.0.0.1:8766` for one JSON command per line:

```
{"id":1,"method":"status"}
{"id":2,"method":"nearby"}
{"id":3,"method":"say","text":"hello"}
{"id":4,"method":"move","x":2,"y":0}
{"id":5,"method":"quit"}
```

`--stdin` reads the same commands as plain text: `status`, `chars`, `enter Name`, `nearby`, `pos`, `say hello`, `move 2 0`, `turn 0.2`, `create Name 1 1 0`, `wait 3`, `quit`.

A script file (`--script commands.txt`, one command per line) runs and then exits. If `character` is set and no script is given, the client logs in, enters that character, prints who is nearby, and quits.

## Window

```
build\reclient.exe --config config.json --window
```

The window is an SDL3 hardware view. Click the account, password, or character box and type. Tab moves between them, Enter connects. WASD moves and Q and E turn once a character is in the world; Enter opens chat. Stock frames stay off the login screen. `--shot` writes `build\northshire.bmp`, a frame of Elwynn terrain from the local install, without logging in.

The running CMaNGOS realm used for bring-up holds `SMSG_CHAR_ENUM` until its Warden module has read system info out of `Wow.exe`. This client decrypts the Warden handshake and does not execute the uploaded module. Turn Warden off on the server (`Warden.Enable=0`) to walk into the world.

## Tools

Node is at `D:\home\tools\node`. JDK 25 is the Eclipse Temurin 25 install. Ghidra 12.2 lives at `D:\home\tools\ghidra\ghidra_12.2_PUBLIC`, and the `Wow.exe` project is under `D:\home\tools\ghidra-projects` (not in this repo). REA is registered for Cursor via `npx rea-agents setup --yes --client cursor`. REA 6.7 accepts Ghidra 12.1.x as its native provider; 12.2 is installed because that is the version this project analyzes with. Set `JAVA_HOME` to the Temurin 25 JDK and `GHIDRA_INSTALL_DIR` to the Ghidra directory before launching the REA provider.

## Layout

`src/` is the session, the MPQ/DBC/M2/BLP/ADT/WMO readers, the SDL view, and the Lua 5.1 FrameXML loader. `docs/provenance.md` records what each piece was checked against. `docs/discrepancies.md` records where this client and CMaNGOS disagree; the game client wins.
