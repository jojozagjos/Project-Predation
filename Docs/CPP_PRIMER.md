# C++ orientation for Project Predation

Written for someone new to C++ who is comfortable with programming generally. This is not a language
tutorial. It explains the specific things that will confuse you in *this* codebase, and points at real
files where each idea lives.

---

## 1. The build pipeline, and why C++ has two kinds of errors

Most languages have one step: run the code. C++ has three, and knowing which one failed tells you what
kind of mistake you made.

```
  .cpp file  ──►  PREPROCESSOR  ──►  COMPILER  ──►  .obj file  ──┐
  (+ its .h files)   pastes in         checks syntax,            │
                     #include text     generates machine code    ├──►  LINKER  ──►  .exe
                                                                 │     joins .obj
  another .cpp  ──►  ...  ──►  another .obj  ─────────────────────┘     files together
```

Each `.cpp` file is compiled **completely separately** into an `.obj` file. It knows nothing about any
other `.cpp`. This one fact explains most of C++'s strange design.

**Compiler errors** look like `error C2065: 'Foo': undeclared identifier`. They mean: while reading one
file top to bottom, I hit a name I have never heard of. Usually a missing `#include`, or a typo.

**Linker errors** look like `error LNK2019: unresolved external symbol`. They mean: the compiler believed
you when you promised a function exists, but when I went to glue everything together, nobody actually
wrote it. Usually a missing library in `CMakeLists.txt`, or a declared-but-never-defined function.

The distinction matters because *the fix is in a different place*. Compiler error → fix an include or a
typo. Linker error → fix your CMake wiring, or write the missing function body.

---

## 2. Headers and sources: promises and deliveries

Look at [`Engine/Core/Log.h`](../Engine/Core/Log.h) and [`Engine/Core/Log.cpp`](../Engine/Core/Log.cpp).

- The **header** (`.h`) is a list of *promises*: "there exists a class called `Log`, and it has a function
  `Init` that takes these arguments." It is mostly declarations, no bodies.
- The **source** (`.cpp`) *delivers* on those promises: the actual function bodies.

When another file writes `#include "Engine/Core/Log.h"`, the preprocessor literally pastes the header's
text into that file. Now the compiler knows `Log::Init` exists and can generate a call to it. The linker
later finds the real body in `Log.obj`.

Why bother with the split? Two reasons:

1. **Compile speed.** If everything were in headers, changing one line would rebuild the whole project.
2. **You can use a thing without seeing how it works** — the same reason interfaces exist everywhere.

**`#pragma once`** at the top of every header stops it being pasted in twice when two different files both
include it. Without it, you get "class already defined" errors. Just always put it there; every header in
this repo has it.

---

## 3. RAII: the single most important idea in C++

C++ has no garbage collector. But it has something that, once it clicks, is arguably better.

**When an object goes out of scope, its destructor runs. Guaranteed. Immediately. Even if an exception is
thrown.** A destructor is a function named `~ClassName()`.

So the C++ way to manage *any* resource — memory, a file handle, a GPU buffer, a lock, a window — is to
wrap it in an object whose constructor acquires it and whose destructor releases it. The language's scoping
rules then do the cleanup for you, correctly, with no `finally` blocks and nothing to forget.

This is called RAII (Resource Acquisition Is Initialization — an infamously bad name for a great idea).

Real example, [`Engine/Debug/FrameStats.h`](../Engine/Debug/FrameStats.h):

```cpp
class ScopedTimer
{
public:
    explicit ScopedTimer(const char* name) : m_name(name), m_start(now()) {}   // starts timing
    ~ScopedTimer() { record(m_name, now() - m_start); }                        // stops, always
};
```

Used via the macro `PRED_PROFILE_SCOPE("Render")` in [`Application.cpp`](../Engine/Application.cpp). You
write one line at the top of a block, and the timing is recorded when the block exits — through a normal
exit, an early `return`, or an exception. You cannot forget to stop the timer.

`Window`, `Renderer`, and `Application` all follow the same pattern: their destructors call `Destroy()` or
`Shutdown()`. When `main()` ends, everything unwinds in reverse order automatically.

---

## 4. Values, references, and pointers

This trips up everyone coming from Python, C#, Java, or JavaScript, where variables are almost always
references to objects. In C++, **a variable *is* the object** unless you say otherwise.

```cpp
void takesCopy(std::string s);         // COPIES the whole string. Caller unaffected by changes.
void takesRef(std::string& s);         // Refers to the caller's string. Changes are visible to caller.
void takesConstRef(const std::string& s);  // Refers to it, but promises not to change it. No copy.
void takesPointer(std::string* s);     // Same as a reference, but can be null, and can be reassigned.
```

The rules of thumb this codebase follows:

- **Small, cheap things** (`int`, `float`, `bool`, an enum): pass **by value**. Copying is free.
- **Big things you only need to read** (`std::string`, `std::vector`, structs): pass **`const T&`**.
  You avoid the copy, and `const` documents that you will not modify it.
- **Things you intend to modify**: pass `T&`.
- **Things that might legitimately be absent**: pass `T*`, and check for `nullptr`.

Look at [`DebugOverlay.h`](../Engine/Debug/DebugOverlay.h): `Draw(const Info& info)` takes a const
reference (read-only, no copy), and inside `Info` the members `renderer` and `stats` are pointers
precisely because they *can* be null before those systems exist.

**The one real danger:** a reference or pointer to an object that has been destroyed is a *dangling*
reference, and using it is undefined behavior (see §7). Never return a reference to a local variable.

---

## 5. Ownership: who is responsible for deleting this?

Every heap-allocated object needs exactly one owner responsible for its lifetime. Modern C++ expresses
that in the type system rather than in comments.

- **`std::unique_ptr<T>`** — exactly one owner. When it dies, the object is deleted. Cannot be copied,
  only moved. This is your default choice.
- **`std::shared_ptr<T>`** — reference counted, deleted when the last one goes away. Useful, but if you
  are reaching for it constantly, you probably haven't decided who owns what.
- **raw `T*`** — in this codebase, *always* means "I'm looking at something someone else owns." Never
  `delete` a raw pointer.

[`Renderer.h`](../Engine/Render/Renderer.h) uses `std::unique_ptr<Impl> m_impl;`. This is the **pimpl**
idiom ("pointer to implementation"): the header shows only that some `Impl` struct exists, while the real
members live in the `.cpp`. It means files including `Renderer.h` don't drag in all of bgfx's headers,
which keeps compiles fast and stops bgfx types leaking into game code.

---

## 6. Reading a class in this codebase

```cpp
namespace pred                          // everything is in namespace `pred` to avoid name collisions
{
class Window
{
public:                                 // the API other code may use
    bool Create(const WindowDesc& desc);
    void Destroy();
    SDL_Window* Handle() const;         // `const` = this function does not modify the object

private:                                // internals; nobody outside may touch these
    SDL_Window* m_window = nullptr;     // `m_` prefix marks a member variable
};
}
```

Conventions you'll see everywhere here:

| Thing | Convention | Example |
|---|---|---|
| Namespace | all lowercase | `pred` |
| Types, functions | PascalCase | `ShaderLibrary`, `BeginFrame()` |
| Member variables | `m_` + camelCase | `m_frameIndex` |
| Constants | `k` + PascalCase | `kViewMain` |
| Includes | repo-relative | `#include "Engine/Core/Log.h"` |

`const` after a member function (`int Width() const;`) is a promise that calling it will not change the
object. Get in the habit of adding it — it lets the function be called on `const` references, and the
compiler enforces the promise.

---

## 7. Undefined behavior: the thing that has no equivalent in safer languages

In most languages, a bug produces an exception or a wrong answer. In C++, certain mistakes produce
**undefined behavior**: the standard imposes no requirements at all. The program may crash, may silently
corrupt data, may work perfectly on your machine and fail on someone else's, or may work in Debug and
break in Release because the optimizer assumed you didn't do it.

The common ways to get it:

- Reading or writing past the end of an array or `std::vector`.
- Using a pointer or reference after the object it points to has been destroyed.
- Using a variable before assigning it a value.
- Signed integer overflow.
- Dereferencing `nullptr`.

**This is why "it runs" is not evidence of correctness in C++,** and why the codebase leans on
`std::vector` instead of raw arrays, `std::string` instead of `char*`, initializes members at their
declaration (`m_window = nullptr`), and checks handles with `bgfx::isValid()` before use.

Practical advice: develop in the **Debug** build. It initializes memory to recognizable patterns, enables
assertions, and catches many of these immediately instead of six months later.

---

## 8. CMake: what those files actually say

CMake is not a build system; it *generates* one (here, Ninja files). You describe **targets** and how they
relate, and it works out the commands.

The three targets in this project:

```
PredationEngine  (static library)  ─┐
                                    ├──►  ProjectPredation  (the .exe)
                                    └──►  PredationTests     (the test .exe)
```

In [`Engine/CMakeLists.txt`](../Engine/CMakeLists.txt), the important lines are:

```cmake
add_library(PredationEngine STATIC ${PRED_ENGINE_SOURCES})   # a bundle of .obj files
target_link_libraries(PredationEngine PUBLIC SDL3::SDL3 ...) # what it needs to link against
target_include_directories(PredationEngine PUBLIC ${CMAKE_SOURCE_DIR})  # where #includes resolve from
```

**`PUBLIC` vs `PRIVATE`** is the one bit worth understanding now. `PUBLIC` means "I need this, *and so does
anyone who links me*." `PRIVATE` means "I need this internally; it doesn't leak." SDL3 is `PUBLIC` because
engine headers mention SDL types, so the game needs SDL's headers too. `stb` is `PRIVATE` because only one
`.cpp` uses it.

A **static library** (`.lib`) is just a collection of `.obj` files bundled together and copied into the
final executable at link time. That's why we ship a single `.exe` with no DLLs beside it.

> **Gotcha worth knowing early.** The linker only pulls an `.obj` out of a static library if something
> actually *references a symbol* in it. A file that contains nothing but self-registering global objects —
> which is exactly how our cvars work, see the `CVar<bool> cv_ai{...}` definitions in
> [`DebugCategories.cpp`](../Engine/Debug/DebugCategories.cpp) — can be silently dropped, and those cvars
> then never appear. If you ever add a new engine file whose only job is to register things and they don't
> show up in the `cvars` console listing, this is why. The fix is to make sure something calls into that
> file, or to put the definitions in a file that is already referenced.

---

## 9. vcpkg: where the third-party code comes from

[`vcpkg.json`](../vcpkg.json) lists the libraries. On configure, vcpkg downloads and **compiles each one
from source** for your exact compiler and settings. That's why the first build takes 20+ minutes and later
ones are instant — results are cached.

The **triplet** `x64-windows-static-md` encodes three choices: 64-bit x86, Windows, statically linked
libraries but using the *dynamic* C runtime. That last part matters: every piece of a C++ program must use
the same C runtime, or you get bizarre crashes when memory allocated by one part is freed by another.
Picking one triplet and sticking to it prevents a whole category of pain.

---

## 10. Practical workflow

```bash
Scripts\build.cmd windows-debug
```

```bash
Scripts\test.cmd windows-debug
```

```bash
Scripts\run.cmd windows-debug
```

**In VS Code:** install the CMake Tools and C/C++ extensions (already recommended in
`.vscode/extensions.json`). Pick the "Windows Debug" preset in the status bar, press F7 to build, F5 to
debug. Set a breakpoint by clicking left of a line number; when it hits, hover a variable to see its value.
The debugger is far more useful than adding log lines, and stepping through
`Application::Run` once is the fastest way to understand the frame loop.

**Reading compiler errors:** always fix the **first** one. C++ errors cascade — one missing semicolon can
produce forty messages, and the other thirty-nine evaporate when you fix the first. Template errors are
famously enormous; the useful line is usually the first one mentioning *your* file.

**When something behaves strangely:** in this project you have the console (grave key `` ` ``) and the F3
overlay. `cvars` lists every tunable, and log output is mirrored into the console with category and level.
Use them before adding `printf`.

---

## 11. What you do *not* need to learn yet

C++ is enormous and most of it is optional. You can build this entire game without ever deeply learning:
template metaprogramming, custom allocators, `volatile`, multiple inheritance, exception hierarchies,
move-constructor edge cases, or the intricacies of `constexpr`. When one of them becomes genuinely useful
here, I'll explain it at the point of use rather than in the abstract.

The 20% that carries the other 80%: headers vs sources, RAII, `const T&` parameters, `unique_ptr`
ownership, `std::vector` and `std::string`, and reading linker errors.
