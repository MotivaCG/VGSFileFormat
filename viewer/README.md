# VGS Viewer

Desktop player for `.vgs` and `.pgs` captures. The viewer lives beside `editor/`,
with its own CMake and qmake projects, deployment script and installer. It
compiles shared display sources from `../editor` with `VGS_VIEWER` defined and
links only the decoder; encoder, export and signing sources are excluded.

The viewer is supplied under the [VGS Decoder licence](../decoder/LICENSE.md).
Its version is defined in `viewerversion.h` and read by both build systems and
the installer.

## Build with CMake

From this directory:

```powershell
cmake -S . -B build/cmake-msvc -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/Qt6.8.1/6.8.0/msvc2022_64
cmake --build build/cmake-msvc --config Release
ctest --test-dir build/cmake-msvc -C Release --output-on-failure
```

The `keyleak_viewer` test checks the executable for the private signing key.
The checker is a test tool and is not included in deployment.

The repository's root CMake project can also build the viewer when configured
with `-DVGS_BUILD_VIEWER=ON` and the Qt prefix. This option is off by default, so
codec and plugin builds do not require Qt. The editor's CMake project builds
the editor separately.

## Build with Qt Creator

Open `VGSViewer.pro` and select the MSVC Qt kit. The usual Release build directory
is `build/Desktop_Qt_6_8_0_MSVC2022_64bit-Release`. Generated build files must be
regenerated with qmake after moving an existing checkout or build directory.

## Deploy and create the installer

`deploy.ps1` makes `<build>/deploy` containing `VGSViewer.exe`, Qt libraries and
plugins, the MSVC runtime and the decoder licence. Run it in the compiler
environment, as Qt Creator does, so `windeployqt` can find the runtime.

```powershell
.\deploy.ps1 -BuildDir build/Desktop_Qt_6_8_0_MSVC2022_64bit-Release
```

For Qt Creator's Custom Process Step under Deploy, use `powershell.exe` and:

```text
-NoProfile -ExecutionPolicy Bypass -File "<viewer>\deploy.ps1" -BuildDir "%{buildDir}"
```

Compile `installer/VGSViewer_InnoSetup.iss` with Inno Setup 6 after deployment.
It uses that qmake Release deploy directory by default and writes
`installer/Output/VGSViewerInstaller_<version>.exe`. To package a CMake build:

```powershell
.\deploy.ps1 -BuildDir build/cmake-msvc
& 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe' "/DMyAppPathToInclude=$PWD\build\cmake-msvc\deploy" installer/VGSViewer_InnoSetup.iss
```

The installer retains the viewer's existing AppId and `.vgs`/`.pgs` associations,
so installations continue to upgrade the same application.
