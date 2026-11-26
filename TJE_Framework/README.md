# TJE_Framework

## ARKFALL quick reference

Run everything from this folder (`TJE_Framework/`) so the relative `data/...` paths resolve.

```console
cmake -S . -B build
cmake --build build --config Debug --target TJE_Framework arkfall_tests
ctest --test-dir build -C Debug --output-on-failure
```

* `arkfall_tests` is a headless test executable (no window/GL context). New headless gameplay sources must be added to `ARKFALL_HEADLESS_SOURCES` in `CMakeLists.txt`; tests live in `tests/`.
* Controls (P04/P05): WASD/arrows pan, Q/E or middle-drag rotate, mouse wheel zoom, left click selects a colonist or a building, B build mode (R rotates the ghost), Space pause, -/+ game speed, Esc or right click cancels/closes. Escape does not quit: close the window. The colonist popup has Engineer/Miner/Farmer buttons (changing role frees the current work slot); the building popup shows workers present/reserved, the recipe, progress and what blocks production.
* Scenarios (P05): `TJE_Framework --scenario <path>` starts another scenario (default `data/arkfall/scenario.json`). `data/arkfall/dev/production.json` has a completed Iron Mine, two Storages and an Iron Processor with 6 Miners, 1 Engineer and 1 Farmer. In Visual Studio, configure with `cmake -S . -B build -DARKFALL_DEBUG_ARGS="--scenario data/arkfall/dev/production.json"` (or set Project > Properties > Debugging > Command Arguments).
* Debug keys: F3 navigation overlay, F4 obstacle ahead of the selected (or scripted) colonist, F6 clear obstacles, F7 open/close the Exit Door, F8 edge scrolling, F5 shader reload (Debug builds).
* `data/arkfall/buildings.json` holds every building value (13 types: footprint, cost, roles/slots, access cells, utilities, recipe, visuals); no building values live in C++. `data/arkfall/colonists.json` holds the role visuals and the 200-name pool.
* `data/arkfall/scenario.json` is the source of truth for the grid, base interior, Exit Door crossings, deposits, blockers and the start state (colonists, inventory, cargo capacity, initial buildings). `data/arkfall/scene.json` is generated from it: run `python tools/generate_arkfall_scene.py` after changing the scenario or `assets.json`, and `python tools/generate_arkfall_scene.py --check` to verify both files agree.

TJE Framework is a C++ layer on top of SDL and OpenGL to help create games or visual applications. It only provides the basic GPU abstraction (Meshes, Textures, Shaders, Application). It has been used for many years in the Videogame's development courses at the Universitat Pompeu Fabra.

It contains the basics to do something nice:

- Mesh, Texture, Shader and FBO classes
- Vector2,Vector3,Vector4,Matrix44 and Quaternion classes
- Meshes have a ray-mesh and sphere-mesh collision method (thanks to library Coldet)
- Parser to load OBJ, PNG and TGA. Re-stores meshes in binary for faster load.
- Supports skinned animated meshes using own format (you must use a web tool to convert them).

## Installation

Clone and initialize the repository:
```
git clone --recurse-submodules -j8 https://github.com/upf-gti/TJE_Framework.git
```

The framework provided is compatible with all operating systems.
The steps for each platform are mandatory to be able to build the framework.
The mandatory C++ IDEs are the following:

* MS Visual Studio Community (Windows)
* XCode (Mac)
* Visual Studio Code (Linux, optional for other platforms)

Detailed installation information can be seen below.

## Windows

MS Visual Studio Community can be downloaded from [here](https://visualstudio.microsoft.com/es/free-developer-offers/). Make sure you select **"Desktop Development with C++"**.

In addition you need to install *CMake* which can be downloaded from [here](https://cmake.org/download/). Select "Windows x64 Installer" for the last version. Remember to set **"Add CMake to PATH"** when asked while installing, otherwhise you won't be able to call CMake from the terminal.

Once you have all required open a Windows Terminal, go to the project folder and do this steps:
```console
mkdir build
cd build
cmake ..
```

This will generate a Visual Studio project inside the folder ``build/`` (.sln) that you can use to compile and debug the framework.

## Mac

You need XCode installed in your system (you may need to update MacOS version), Homebrew to install the missing libraries and also *CMake*. 

To install Homebrew open a terminal and run this command:
```console
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

Continue by installing *cmake* using this command:
```console
brew install cmake
```

Once you have all required open a MacOS Terminal, go to the project folder and do this steps:
```console
mkdir build && cd build
cmake -G "Xcode" ..
```

This will generate a XCode project inside the folder ``build/`` that you can use to compile and debug the framework.

If this process leads to cmake errors, run this command an try again:

```console
sudo xcode-select --reset
```

> [!CAUTION]
> For audio this project uses the C++ middleware BASS. The library is distributed as a .dylib file, and due to Mac OS security policy it will ask you to quarantine this file. It is important to go to "Privacy and security" and enable loading for "libbass.dylib" when promted in order to build this project. This should be necessary to do only once.

## Linux

Install *cmake* and needed libraries using this command:
```console
sudo apt install cmake
sudo apt install build-essential
sudo apt install -y libglu1-mesa-dev freeglut3-dev mesa-common-dev libgl1-mesa-dev
```

Once you have all required open a Linux Terminal, go to the project folder and do this steps:
```console
mkdir build && cd build
cmake ..
```

This will generate a Makefile inside the folder ``build/`` that you can use to compile framework.

Use ``make`` to compile. You can speed-up compilation using more threads with ``-j(num threads)``, for example: ``make -j8``.

> [!CAUTION]
> Note that when running the executable, in order for the paths to align, you will need to run it from the project directory, not from the build folder.

### Visual Studio Code (Linux, optional for other platforms)

After installing all the libs for your platform, if you need a more light weight IDEs which can be used in any platform (included Linux), this is your better option.

Visual Studio Code can be downloaded for each platform [here](https://code.visualstudio.com/download).

#### Configuring VSCode

After the VSCode and the requirements for each platform are installed **(the steps for each platform are mandatory!)**, the following extensions are needed to work with C++ code:

```
C/C++
C/C++ Extension Pack
C/C++ Themes
Cmake Tools
```

Check [this link](https://gourav.io/blog/setup-vscode-to-run-debug-c-cpp-code) to learn how to debug the framework in Visual Studio Code.

First, open the project folder where the CMakeLists.txt is located, then open the CMake tab on the left, configure and build the project.
