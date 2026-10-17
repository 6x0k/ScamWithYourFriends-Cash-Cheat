# Scam With Your Friends - External Trainer

An external C++ ImGui overlay for Scam With Your Friends. It uses a dynamic pointer chain to read and write the player's cash value, with a freeze feature to bypass network sync reverts.

## Table of Contents
- [Features](#features)
- [How to Use](#how-to-use)
- [Building from Source](#building-from-source)
- [Updating Offsets](#updating-offsets)
- [Disclaimer](#disclaimer)

## Features
* **ImGui Overlay:** Modern DirectX 11 menu (Toggle with `INSERT`).
* **Cash Reader:** Displays your current in-game cash in real-time.
* **Set & Freeze:** Writes your desired cash value every frame to prevent the server from reverting it.

## How to Use
1. Launch **Scam With Your Friends** and enter a match.
2. Run the compiled `.exe` (accept the admin prompt).
3. Press `INSERT` to open the menu.
4. Type your desired cash amount and click **Set & Freeze Money**.
5. Press `END` to close the trainer.

## Building from Source
The project uses CMake and requires the ImGui and MinHook (optional) dependencies.

1. Clone the repository.
2. Generate the CMake cache (ensure you target x64).
3. Build the solution in Visual Studio.

```bash
git clone https://github.com/6x0k/ScamWithYourFriends-Cash-Cheat.git
cd ScamWithYourFriends-Cash-Cheat
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

## Updating Offsets
i try to keep em up if i got time
