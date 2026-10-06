# Chia

Chia is a Java-inspired programming language and command-line interpreter for
Windows, implemented as a standalone C++17 program.

```java
import Chia.Biblion;

public class Main {
    public static void main(String[] args) {
        Chia.println("Hello, Chia!");
    }
}
```

## Get started

On Windows, run `dist\ChiaSetup.exe` to install Chia for the current user. The
installer adds Chia to the user `PATH`; open a new terminal and run:

```text
chia new Hello.chia
chia run Hello.chia
```

To build from source with Visual C++, open a Developer Command Prompt:

```bat
cl /std:c++17 /EHsc /O2 /utf-8 Chia.cpp
```

See [CHIA.md](CHIA.md) for language syntax, API reference, project generators,
and platform requirements. Run the Windows smoke tests with
`.\tests\Test-Chia.ps1`.

## Project status

Chia is an early-stage learning and individual-development language. Its
interpreter supports a compact Java-like subset; generated OS, desktop, mobile,
HTTP, database, and game projects are starter scaffolds that use external
toolchains. See the documentation for current limitations.
