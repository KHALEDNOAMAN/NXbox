# NXbox - Build & Compatibility Guide

## What is NXbox?
A Nintendo Switch emulator running natively on Xbox Series X as a UWP app.

## Architecture
```
Switch ROM --> NXbox Core --> OpenGL (Mesa) --> Direct3D 12 --> Xbox GPU
                 |
          Eden Emulator Port
```

## Build Requirements
| Requirement | Version |
|-------------|---------|
| Visual Studio | 2022+ |
| Windows SDK | 10.0.22621+ |
| CMake | 3.25+ |
| Xbox Dev Mode | Enabled |

## Build Steps
```bash
git clone --recursive https://github.com/NspxMiguel/NXbox.git
cd NXbox
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_SYSTEM_NAME=WindowsStore
cmake --build build --config Release
```

## Status
| Component | Status |
|-----------|--------|
| CPU Emulation | Working |
| GPU (Mesa/D3D12) | Experimental |
| Audio | Partial |
| Input | Working |
| Games | Not fully playable |