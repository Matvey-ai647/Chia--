# Chia

Chia is a small Java-inspired scripting language for learning and individual
program development. `Chia.cpp` contains the Chia Swing Compilator: a
standalone C++17 interpreter with a command-line interface.

## Build

With GCC or Clang:

```sh
g++ -std=c++17 -O2 -o chia Chia.cpp
```

With Visual C++:

```bat
cl /std:c++17 /EHsc /O2 /utf-8 Chia.cpp
```

## Windows installer

Run `dist\ChiaSetup.exe` and Chia will be installed for the current user under
`%LOCALAPPDATA%\Programs\Chia`; no administrator rights are required. The setup
adds that folder to the user `PATH`. Open a new terminal to use `chia`, and run
`uninstall.ps1` from the installation folder to remove Chia. User-created files
in the install folder are preserved.

To build the installer from source, compile `Chia.cpp` to `dist\chia.exe` with
Visual C++ and run:

```powershell
.\build-installer.ps1
```

The packaging script uses the IExpress tool included with Windows. The language
smoke tests can be run with:

```powershell
.\tests\Test-Chia.ps1
```

## Create and run a program

```text
chia new Hello.chia
chia run Hello.chia
chia check Hello.chia
chia translate Hello.chia Hello.cpp
chia spell Hello.chia
chia correct Hello.chia Hello.corrected.chia
chia game engines
chia game new raylib 2d MyGame
chia game new unity 3d MyUnityGame
chia game new unreal 3d MyUnrealGame
chia game new godot 2d MyGodotGame
chia game open MyUnityGame
chia game build MyUnityGame
```

`chia new` writes a runnable `Main` class demonstrating text output and a
dictionary. If no extension is provided, `.chia` is added. Existing files are
never overwritten. A source file can also be run directly as `chia Hello.chia`.

Example:

```java
import Chia.Biblion;

public class Main {
    public static void main(String[] args) {
        String message = "Привет, Chia!";
        Chia.println(message);

        int answer = 20 + 22;
        Chia.println("Ответ: " + answer);
    }
}
```

## Language features

- `String`, `int`, `double`, `boolean`, `var`, `Object`, and generic
  `Dictionary<Key, Value>` declarations.
- Variables, reassignment with type checks, string concatenation, arithmetic,
  comparisons, boolean operators, and parentheses.
- `//` and `/* ... */` comments, quoted strings, and common backslash escapes.
- `Dictionary`/`Object` methods: `put`/`set`, `get`, `containsKey`/`contains`,
  `remove`/`delete`, and `size`.
- Built-in Biblion helpers: `Chia.println`, `Chia.print`, `Chia.typeOf`, and
  `Chia.length`.
- A local key-value database: `Chia.Database.put`, `get`, `contains`, and
  `delete`. It is stored beside the script in `.chia_database`; supported
  persistent values are strings, numbers, and booleans.
- Local username registry: `Chia.User.create(name)` and `Chia.User.exists(name)`.
  This registry stores names only; it is not an authentication system.
- Source diagnostics include line and column; `chia check` does not print
  program output or persist database/user changes.
- `chia translate input.chia output.cpp` translates a `Main.main` program to
  standalone C++17. The translator supports imports, typed local variables,
  assignments, arithmetic/boolean expressions, and `print`/`println`.
  Dictionary/Object/List operations, database and user-registry calls,
  `for`/`switch`, and custom methods are not supported by the C++ translator;
  unsupported constructs produce a source diagnostic instead of partial output.
- `chia game engines` searches for installed engines. `chia game new
  <raylib|unity|unreal|godot> <2d|3d> <folder>` generates a starter game
  project. If the selected editor is found, Chia opens the newly created
  project automatically. `chia game open <folder>` opens an existing project;
  `chia game build <folder>` launches its native build/export pipeline.
- Unity projects include a bootstrapped playable scene and a Windows player
  build method. Godot projects include a 2D or 3D starter scene and a Windows
  export preset. Unreal projects include C++ game-mode and controllable
  character sources; UnrealBuildTool builds them when found. Raylib projects
  use the Chia-to-C++ translator and CMake.
- Chia checks the Windows `PATH`, common installation directories, and
  engine-specific settings: `UNITY_EDITOR` (path to `Unity.exe`), `GODOT_EXE`
  (path to the Godot executable), and `UNREAL_ENGINE_ROOT` (Unreal Engine
  installation root). Raylib building uses `VCPKG_ROOT` when set. If an editor
  is not installed, project scaffolding still works; opening/building reports
  the missing dependency. Godot export additionally requires installed export
  templates.
- Unity, Unreal, and Godot projects are native engine projects with starter
  scripts, not a general transpilation of arbitrary Chia programs into
  C#/Unreal C++/GDScript. Use each engine's editor and scripting APIs for game
  logic; Chia manages project creation, detection, opening, and build launch.
- Import `Chia.Game` to use the Raylib game API: `window`, `frameRate`,
  `shouldClose`, `beginFrame`/`endFrame`, `clear`, `drawText`, `drawCircle`,
  `drawRectangle`, cached `drawTexture`, keyboard (`keyDown`/`keyPressed`) and
  mouse input, frame timing, rectangle/circle collision checks, audio
  (`initAudio`, `playSound`), and 3D camera/cube/grid/model operations
  (`begin3D`, `end3D`, `drawCube`, `drawCubeWires`, `drawGrid`, `drawModel`).
  Cached textures, sounds and models are released when `closeWindow` runs.
  `if`, `else`, and `while` are supported in translated programs.
  Asset paths are resolved relative to the game's working directory.
- Game builds require CMake, the Chia compiler on `PATH`, and Raylib installed
  with vcpkg. For example, install Raylib with
  `vcpkg install raylib:x64-windows`, add `chia.exe` to `PATH`, then configure
  a generated game project with
  `cmake -S MyGame -B MyGame/build -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake`
  and build it with `cmake --build MyGame/build`. Editing `Game.chia` and
  rebuilding automatically retranslates the source.
- `chia spell file.chia` checks Russian words in string literals and comments
  against the built-in common-typo list. `chia correct input.chia output.chia`
  applies those known corrections to a new file and never overwrites the input.
  This offline helper is a compact correction list, not a complete Russian
  dictionary; unrecognized misspellings are not detected. The spelling command
  also scans syntax fingerprints, including unclosed delimiters, missing
  semicolons, likely missing `=`, and lexer errors, and prints a suggested fix.
  Syntax suggestions are diagnostic only and do not rewrite source code.

Supported imports are `Chia.Biblion`, `Chia.Database`, `Chia.User`,
`Chia.Collections`, `Chia.Diagnostics`, `Chia.Game`, `Chia.OS`, `Chia.App`,
`Chia.Project`, `Chia.Git`, `Chia.HttpExchange`, `Chia.Server`,
`Chia.Database.Java`, `Chia.Database.Oracle`, and
`com.sun.net.httpserver.HttpExchange`. `System.out.println` and
`System.out.print` are also accepted for Java familiarity.

The interpreter intentionally focuses on a compact runnable core. Game scripts
use the Raylib-backed C++ translation/build path; the interpreter does not
execute graphics APIs directly. Custom classes, user-defined methods, and
third-party libraries are not implemented yet.

## OS, application, Git, database, and HTTP project APIs

Project APIs are called from a `.chia` program and generate starter source files
in a new directory. They do not translate arbitrary Chia program logic into a
native platform app or kernel. `chia check` validates these calls without
creating files or running Git; `chia run` performs the requested side effects.
Existing directories are never overwritten.

```java
import Chia.OS;
import Chia.App;
import Chia.Git;
import Chia.Database.Java;
import Chia.Database.Oracle;
import Chia.HttpExchange;

public class Main {
    public static void main(String[] args) {
        Chia.OS.createKernel("TinyKernel", "out/TinyKernel");
        Chia.App.createDesktop("DeskApp", "out/DeskApp", "windows");
        Chia.App.createDesktop("DeskAppLinux", "out/DeskAppLinux", "linux");
        Chia.App.createDesktop("DeskAppMac", "out/DeskAppMac", "macos");
        Chia.App.createMobile("AndroidApp", "out/AndroidApp", "android");
        Chia.App.createMobile("IosApp", "out/IosApp", "ios");
        Chia.HttpExchange.create("WebServer", "out/WebServer");
        Chia.Database.Java.create("LocalDatabase", "out/LocalDatabase");
        Chia.Database.Oracle.create("OracleDatabase", "out/OracleDatabase");
        Chia.Git.init("out/DeskApp");
        Chia.Git.status("out/DeskApp");
        Chia.Git.add("out/DeskApp");
        Chia.Git.commit("out/DeskApp", "Initial Chia project");
    }
}
```

Supported project calls:

- `Chia.OS.createKernel(name, path)` creates a minimal freestanding 32-bit
  Multiboot kernel scaffold. It is not a complete OS; building requires an
  `i686-elf` GCC cross-toolchain and booting requires a compatible bootloader or
  emulator.
- `Chia.App.createDesktop(name, path, "windows"|"macos"|"linux")` generates a
  C++17/CMake desktop starter. Use the native toolchain or a configured
  cross-compiler for the target platform.
- `Chia.App.createMobile(name, path, "android"|"ios")` generates a native
  Android Gradle/Java or iOS SwiftUI/XcodeGen starter. Android requires the
  Android SDK, JDK 17 and Gradle; iOS building/signing requires macOS, Xcode and
  signing credentials.
- `Chia.HttpExchange.create(name, path)` (also
  `Chia.Server.createHttpExchange`) creates a Java 17 `com.sun.net.httpserver`
  server using `HttpExchange`. The sample listens on localhost only; add
  production-grade TLS, authentication and request handling before deployment.
- `Chia.Database.Java.create(name, path)` creates a Java JDBC project using a
  local H2 file database. `Chia.Database.Oracle.create(name, path)` creates an
  Oracle JDBC starter. Both require JDK 17 and Maven. Oracle additionally
  requires an Oracle server and `ORACLE_JDBC_URL`, `ORACLE_USER`, and
  `ORACLE_PASSWORD` environment variables; credentials are not written to
  generated source.
- `Chia.Git.init(path)`, `status(path)`, `add(path)` and `commit(path, message)`
  execute the installed Git CLI. Git must be on `PATH`; `add` stages all changes
  in that repository, and `commit` requires Git identity to be configured.

These are project generators and Git/JDBC/HTTP integration entry points, not
embedded Android/iOS SDKs, a native Chia-to-OS compiler, or direct Java database
bindings in the Chia interpreter. Generated Java projects are built and run
with Maven/JDK; generated platform projects use their own native toolchains.
