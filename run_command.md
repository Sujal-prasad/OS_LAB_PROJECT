# Run

Open a terminal **inside the project folder** (the folder that has `os_project.cpp`).
In VS Code: **Terminal > New Terminal** opens one there automatically.

## Windows (PowerShell)

Terminal 1 (build and run):

```
g++ -std=c++17 -pthread os_project.cpp -o os_project.exe; .\os_project.exe
```

Terminal 2 and more (run only):

```
.\os_project.exe
```

## Linux

Terminal 1:

```
g++ -std=c++17 -pthread os_project.cpp -o os_project -lrt && ./os_project
```

Terminal 2 and more:

```
./os_project
```

## macOS

Terminal 1:

```
g++ -std=c++17 -pthread os_project.cpp -o os_project && ./os_project
```

Terminal 2 and more:

```
./os_project
```

## If `g++` is not found

Windows (then close and reopen the terminal):

```
winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT
```

Linux (Ubuntu/Debian):

```
sudo apt install -y g++
```

macOS:

```
xcode-select --install
```
