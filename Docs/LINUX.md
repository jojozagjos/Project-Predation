# Linux

The port is prepared but not yet built. Everything below is groundwork done on a Windows machine
with no Linux compiler on it, which means it is reasoned rather than proved, and the first real
build will find things. That is expected and it is why this file exists.

## What is already there

- **Presets.** `linux-debug` and `linux-release`, on the `x64-linux` triplet, using the same vcpkg
  manifest as Windows. They are hidden on a Windows host, the way the Windows ones are on Linux.
- **Scripts.** `play.sh` at the top, and `build.sh`, `test.sh`, `run.sh` and `clean.sh` beside their
  `.cmd` equivalents. They take the same arguments and print the same things.
- **The window.** A renderer on X11 or Wayland needs the display connection as well as the window,
  and the engine only ever handed over the window. `Window::NativeDisplay()` is what was missing;
  without it a first port comes up to a black screen and no error at all.
- **System information.** The CPU name, the memory in use and the memory installed now come from
  `/proc` and `sysconf` rather than returning "unknown" and zero.
- **Everything else in the engine** already had its POSIX branch: paths, sockets, the port mapper,
  the timestamp in the log.

## What is known to be left

- **Shaders.** The build compiles them per backend, and the profiles for Vulkan and OpenGL are not
  the ones Direct3D uses. This is the largest remaining piece and it cannot be checked from here.
- **The first compile.** A codebase that has only ever been built with one compiler always has a
  handful of things the others object to: missing includes that MSVC supplies through another
  header, narrowing conversions, and the order of declarations in templates. None of them are
  interesting and all of them have to be found by compiling.
- **The audio device.** SDL3 opens ALSA or PipeWire here rather than WASAPI. The engine asks SDL
  for a default playback device and mixes into it, so there is nothing platform-specific to write,
  but it has not been heard.
- **Packaging.** `package.cmd` lays out a folder and zips it. There is no `package.sh` yet.

## Building it

```
git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg
./play.sh
```

The dependencies want the usual development packages for X11, Wayland, ALSA and OpenGL; vcpkg says
which when it stops.
