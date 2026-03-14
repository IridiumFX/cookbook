# now — Complete Implementation Specification

**Version**: 1.1  
**Status**: Implementation Ready — v1 Scoped  
**Generated**: 2026-03-06  

This document is the authoritative merged specification for `now`, a
native-language build tool and package manager. It consolidates all
working documents into a single implementation reference, with all
chapter numbering reconciled and v1 scope annotations applied.

Cross-references are to chapters and sections within this document.

### v1 Scope Summary

The initial open-source release targets:

- **Languages**: C and C++ (gcc/clang toolchains, MSVC on Windows)
- **Platforms**: Linux, macOS, Windows (x86-64 and arm64)
- **Registry**: path-based and git-based dependencies; hosted registry
  via the companion `cookbook` project (in development)

Features marked **Post-v1** are fully specified and will be implemented
in subsequent releases. Do not remove them from this document.

---

## Table of Contents

| Chapter | Title | v1? |
|---------|-------|-----|
| 1 | Project Descriptor (`now.pasta`) | ✓ |
| 2 | Lifecycle and Phases | ✓ |
| 3 | Directory Layout | ✓ |
| 4 | Source Classification and Language Type System | ✓ (C/C++ only) |
| 5 | Artifact Model and Installed Layout | ✓ |
| 6 | Dependency Resolution and Versioning | ✓ |
| 7 | Toolchain Configuration — GCC / Clang | ✓ |
| 8 | Toolchain Configuration — MSVC | ✓ |
| 9 | Testing | ✓ |
| 10 | Plugins, Tools, Code Generation, and Plugin Protocol | ✓ |
| 11 | Multi-Architecture and Platform Triples | ✓ (Linux/macOS/Win) |
| 12 | Offline Mode and Cache | ✓ |
| 13 | Build Graph, Parallel Execution, and Module Pre-Scan | ✓ |
| 14 | Packaging and Assembly | ✓ |
| 15 | Language Directories and Per-Module Language Declaration | ✓ |
| 16 | CI Integration | ✓ |
| 17 | Signing and Trust | ✓ |
| 18 | Module System and Build Order | ✓ |
| 19 | IDE Integration and `now stay` Daemon | **Post-v1** |
| 20 | Serialisation: Pasta, JSON, and JSON5 | ✓ |
| 21 | Dependency Confusion Protection | ✓ |
| 22 | Glob Pattern Dialect | ✓ |
| 23 | Embedded and Freestanding Platforms | **Post-v1** |
| 24 | Reproducible Builds and Security Advisory Integration | ✓ |
| 25 | Cascading Configuration Layers | ✓ |
| 26 | Schema Reference | ✓ |
| 27 | Error Catalogue | ✓ |

---

# Chapter 1 — Project Descriptor (`now.pasta`)

The project descriptor is a file named `now.pasta` at the root of every
`now` project. It is the single authoritative source of project identity,
dependencies, build configuration, and lifecycle customisation. Everything
`now` does derives from this file.

The descriptor root is always a map. All fields are optional unless marked
**required**. Fields from the layer system (Chapter 25) are applied before
the descriptor is read by the build, giving a merged effective configuration.

---

## 1.1 Identity

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `group` | string | yes | Reverse-domain namespace: `"org.example"` |
| `artifact` | string | yes | Lowercase, hyphens allowed: `"my-lib"` |
| `version` | string | yes | Semantic version: `"MAJOR.MINOR.PATCH[-qualifier]"` |
| `name` | string | no | Display name; defaults to `artifact` |
| `description` | string | no | One-line summary |
| `url` | string | no | Project homepage |
| `license` | string | no | SPDX identifier: `"MIT"`, `"Apache-2.0"` |

The **coordinate** is `group:artifact:version` — the identifier used in
`deps` arrays, the registry, and lock files.

---

## 1.2 Language

```pasta
langs: ["c"],    ; array of language identifiers
std:   "c11",    ; primary language standard
```

`langs` accepts any language identifier registered in the language type
system (Chapter 4). Common values: `c`, `c++`, `asm`, `modula2`, `ada`,
`fortran`, `pascal`. A project with multiple languages in `langs` compiles
each source file with the tool appropriate for its type.

`std` applies to the primary language. Per-language standard overrides are
declared in the language type definition. Valid `std` values by language:

| `lang` | Allowed `std` values |
|--------|---------------------|
| `c` | `c89`, `c99`, `c11`, `c17`, `c23` |
| `c++` | `c++11`, `c++14`, `c++17`, `c++20`, `c++23` |
| `ada` | `ada83`, `ada95`, `ada2005`, `ada2012`, `ada2022` |
| others | as defined by their language type |

The legacy scalar `lang: "c"` is accepted as shorthand for `langs: ["c"]`.
`lang: "mixed"` is accepted as shorthand for `langs: ["c", "c++"]`.

---

## 1.3 Source Layout

```pasta
sources: {
  dir:     "src/main/c",
  headers: "src/main/include",   ; public headers — installed and added to -I
  private: "src/main/h/impl",    ; private headers — not installed
  pattern: "**.c",               ; glob (Chapter 19)

  include: ["vendor/sqlite/sqlite3.c"],   ; explicit additions after pattern
  exclude: ["vendor/sqlite/**.c"],        ; glob exclusions applied last

  overrides: [
    {
      file:    "src/vendor/stb_image.c",
      compile: { warnings: [], defines: ["STB_IMAGE_IMPLEMENTATION"] }
    }
  ]
}
```

Resolution order: collect `pattern` matches → add `include` entries
(de-duplicated) → remove `exclude` matches. `include` paths that do not
exist are hard errors. `exclude` patterns that match nothing warn in
verbose mode only.

Per-file `overrides` merge with the project compile block: scalars replace,
arrays append. Use `!`-prefix on a key to force array replacement:
`!warnings: []` replaces the warning list entirely.

Default source roots when `sources` is omitted:

| `langs` primary | `sources.dir` | `sources.headers` |
|----------------|--------------|------------------|
| `c` | `src/main/c` | `src/main/include` |
| `c++` | `src/main/cpp` | `src/main/include` |
| `mixed` / multiple | `src/main` | `src/main/include` |
| `asm` | `src/main/asm` | *(none)* |

`tests` follows the same structure, defaulting to `src/test/`.

---

## 1.4 Output

```pasta
output: {
  type: "executable",   ; executable | static | shared | header-only
  name: "myapp",
  dir:  "target/bin"
}
```

Platform filename derivation:

| Type | Linux | macOS | Windows |
|------|-------|-------|---------|
| `executable` | `myapp` | `myapp` | `myapp.exe` |
| `static` | `libmyapp.a` | `libmyapp.a` | `myapp.lib` |
| `shared` | `libmyapp.so` | `libmyapp.dylib` | `myapp.dll` |
| `header-only` | *(no compiled output)* | | |

---

## 1.5 Compile and Link Blocks

```pasta
compile: {
  flags:    ["-O2"],
  warnings: ["Wall", "Wextra"],   ; -W prepended automatically
  defines:  ["NDEBUG"],           ; -D prepended automatically
  includes: ["vendor/include"],   ; -I prepended automatically
  std:      "c11",                ; override root std for this block
  opt:      "speed"               ; none | debug | size | speed | lto
},

link: {
  flags:   ["-pthread"],
  libs:    ["m", "dl"],           ; -l prepended automatically
  libdirs: ["/usr/local/lib"],    ; -L prepended automatically
  script:  "link/memory.ld",      ; linker script path
  script_body: """
    MEMORY { RAM : ORIGIN = 0x20000000, LENGTH = 256K }
    SECTIONS { .text : { *(.text) } > RAM }
  """                             ; inline linker script (multiline string)
}
```

`opt` level mapping:

| `opt` | GCC/Clang | MSVC |
|-------|-----------|------|
| `none` | `-O0` | `/Od` |
| `debug` | `-Og` | `/Od` |
| `size` | `-Os` | `/O1` |
| `speed` | `-O2` | `/O2` |
| `lto` | `-O2 -flto` | `/O2 /GL` |

### Array merge and `!`-prefix replacement

In profiles, per-file overrides, and module inheritance, arrays accumulate
by default (profile array appended to base). Prefix a key with `!` to
force replacement instead:

```pasta
profiles: {
  strict: { compile: { !warnings: ["Wall", "Wextra", "Wpedantic"] } }
}
```

`!` is a valid Pasta label character. `now` strips it during merge and
treats the remainder as the field name. `!`-prefixed keys are only valid
in merge contexts — not at the root level of `now.pasta`.

---

## 1.6 Dependencies

```pasta
deps: [
  { id: "org.acme:core:1.5.0",   scope: "compile"  },
  { id: "zlib:zlib:^1.3",        scope: "compile"  },
  { id: "unity:unity:2.5.2",     scope: "test"     },
  { id: "org.acme:volatile-mod:*", scope: "compile", volatile: true }
]
```

| Field | Type | Notes |
|-------|------|-------|
| `id` | string! | Coordinate: `group:artifact:version-or-range` |
| `scope` | enum | `compile` (default), `test`, `provided`, `runtime` |
| `volatile` | bool? | Acknowledges dependency on a volatile module (§1.12) |

Version range syntax is defined in Chapter 6.

---

## 1.7 Repositories

```pasta
repos: [
  "https://registry.now.build/central",                   ; string shorthand
  { url: "https://pkg.acme.org/now", id: "acme",          ; map form
    release: true, snapshot: false, auth: "token" }
]
```

A plain string is shorthand for `{url: "...", release: true, snapshot: false, auth: "none"}`.
The local repo (`~/.now/repo`) is always checked first regardless of
declaration order.

---

## 1.8 Profiles

Profiles are named overlays activated with `-p name`. Multiple profiles
may be active simultaneously, applied left-to-right.

```pasta
profiles: {
  debug:   { compile: { flags: ["-g", "-O0"], defines: ["DEBUG"] } },
  release: { compile: { flags: ["-O3", "-flto"] }, link: { flags: ["-s"] } },
  asan:    {
    compile: { flags: ["-fsanitize=address"] },
    link:    { flags: ["-fsanitize=address"] }
  }
}
```

Profile merge rules: scalars replace, arrays append, maps deep-merge.
Use `!` prefix on array key to force replacement (§1.5).

---

## 1.9 Properties and Interpolation

```pasta
properties: {
  api_version:   "3",
  install_prefix: "/usr/local",
  ci_build_id:   { value: "${env.CI_BUILD_ID}", volatile: true }
},

compile: { defines: ["API_VERSION=${api_version}"] },
output:  { dir: "${install_prefix}/bin" }
```

Built-in properties:

| Property | Value |
|----------|-------|
| `${now.group}` | `group` field |
| `${now.artifact}` | `artifact` field |
| `${now.version}` | `version` field |
| `${now.basedir}` | Absolute path to project root |
| `${now.target}` | Absolute path to `target/` |
| `${now.triple}` | Active platform triple, e.g. `linux:x86_64:gnu` |
| `${env.NAME}` | Value of environment variable `NAME` |

Properties marked `volatile: true` do not flow to dependent modules or
downstream consumers (§1.11).

---

## 1.10 Plugins

```pasta
plugins: [
  { id: "org.protobuf:protoc-now:3.21.0", hooks: ["generate"] }
]
```

Plugins are declared as dep-like coordinates and procured from the
registry. Full plugin manifest and protocol specification in Chapter 10.

---

## 1.11 Workspace Fields

These fields are only meaningful in a workspace root `now.pasta`:

```pasta
modules: ["core", "net", "tls", "cli"],   ; sub-module directories

inherit_defaults: {
  version:    true,    ; modules inherit root version
  deps:       true,    ; root deps prepended to module deps
  compile:    false,   ; modules own compile config
  link:       false,
  profiles:   true,
  properties: true
}
```

`inherit_defaults` declares per-field inheritance policy for all modules.
Built-in defaults (when `inherit_defaults` is absent): all fields `true`.

Each module may override with its own `inherit` block:

```pasta
; module/now.pasta
{
  inherit: {
    compile: { warnings: true, flags: false, defines: false }
  }
}
```

Shorthand: `inherit: *` (inherit everything), `inherit: null` (inherit
nothing).

Fields never inherited regardless of policy: `group`, `artifact`,
`version`, `modules`, `output`, `sources`, `tests`, `langs`, `assembly`,
`publish`.

### Property volatility

A property with `volatile: true` does not flow to any module or consumer.
Both producer and consumer gates must be open for a property to reach a
module: `property.volatile != true` AND `module.inherit.properties == true`.

### Workspace mode

Inferred from `inherit_defaults` presence: if present → `monorepo`; if
absent → `aggregate`. Override explicitly:

```pasta
{
  workspace_mode: "aggregate",   ; "monorepo" | "aggregate" | "inferred"
  walk_boundary:  "continue",    ; "stop" | "continue" (default: "continue")
  modules: [...]
}
```

Relevant for the layer filesystem walk (Chapter 25 §25.13).

---

## 1.12 Volatile Modules

```pasta
; scheduler/now.pasta
{
  volatile: true,    ; design not stable — cannot be published or externally procured
  version:  "0.1.0-incubating",
  ...
}
```

A volatile module builds and tests normally within a workspace but cannot
be published to a registry or resolved by external projects. Sibling
dependencies on volatile modules require explicit acknowledgement
(`volatile: true` on the dep entry). Volatility is transitive for
publication: a module depending on a volatile sibling cannot itself be
published.

`now volatile:promote :module` removes the flag, removes sibling
acknowledgements, and runs `now version:check`.

---

## 1.13 Additional Top-Level Fields

| Field | Type | Notes |
|-------|------|-------|
| `build_options` | map? | Advanced build phase options (Chapter 8 §8.9) |
| `assembly` | assembly[]? | Multi-artifact packaging (Chapter 13) |
| `publish` | map? | Registry publication config |
| `reproducible` | map? | Reproducible build settings (Chapter 22) |
| `target` / `targets` | map? | Per-triple compile overrides (Chapter 12) |
| `toolchain` | map? | Toolchain selection and config (Chapter 7) |
| `private_groups` | string[]? | Private group prefixes (Chapter 24) |
| `parent` | string? | Published parent descriptor coordinate |

---

## 1.14 Minimal and Rich Examples

```pasta
; Minimal
{
  group:    "io.example",
  artifact: "hello",
  version:  "1.0.0",
  langs:    ["c"],
  std:      "c11",
  output:   { type: "executable", name: "hello" },
  compile:  { warnings: ["Wall", "Wextra"] }
}
```

```pasta
; Rich
{
  group:       "org.acme",
  artifact:    "rocketlib",
  version:     "3.0.0-beta.1",
  name:        "Rocket Library",
  license:     "Apache-2.0",
  langs:       ["c", "c++"],
  std:         "c17",

  sources: {
    dir:     "src/main",
    headers: "include",
    pattern: ["**.c", "**.cpp", "**.s"]
  },

  output:  { type: "shared", name: "rocket" },

  compile: {
    flags:    ["-fvisibility=hidden"],
    warnings: ["Wall", "Wextra", "Werror"],
    defines:  ["ROCKET_BUILDING"]
  },

  link: { flags: ["-pthread"], libs: ["m"] },

  deps: [
    { id: "org.acme:acme-core:^1.5", scope: "compile" },
    { id: "unity:unity:2.5.2",       scope: "test"    }
  ],

  profiles: {
    debug:   { compile: { flags: ["-g", "-O0"], defines: ["DEBUG"] } },
    release: { compile: { flags: ["-O3", "-flto"] } }
  },

  properties: { soversion: "3" }
}
```



---



---

# Chapter 2 — Lifecycle and Phases

`now` defines two independent lifecycles: **default** and **clean**.

## 2.1 Default Lifecycle

```
procure → generate → build → link → test → package → install → publish
```

| Phase | Description |
|-------|-------------|
| `procure` | Resolve dependency graph. Download missing artifacts. Install to local repo. Write/verify `now.lock.pasta`. |
| `generate` | Run registered code-generation plugins. Outputs go to `target/generated/`. |
| `build` | Unified source→object phase: compile, assemble, and all other language transformations. Replaces old `compile` + `assemble` phases. |
| `link` | Invoke linker or archiver on all object files. Produce final output in `target/bin/`. |
| `test` | Compile test sources and execute test binary. Fails build on any test failure. |
| `package` | Bundle output artifact, headers, and descriptor into a tarball in `target/pkg/`. |
| `install` | Extract package into `~/.now/repo/{group}/{artifact}/{version}/`. |
| `publish` | Upload package archive and SHA-256 to configured remote registry. |

Invoking a phase invokes all prior phases in its lifecycle first.

## 2.2 Clean Lifecycle

```
clean → vacate
```

| Phase | Description |
|-------|-------------|
| `clean` | Delete `target/`. Forces full rebuild. |
| `vacate` | Remove installed dep artifacts from `~/.now/repo/` (and optionally cache). |

`vacate` is never run automatically — it must be invoked explicitly.

## 2.3 The `build` Phase in Detail

`build` is a fixpoint loop, not a single pass:

```
for each source file S in the resolved source set:
  T    = classify(S)                    ; language type (Chapter 4)
  tool = T.tool
  flags = T.flags(project, profile)
  intermediates = tool.invoke(S, flags)
  if intermediates produce further source files:
    re-classify each → re-enter build loop
  emit object file(s) to target/{triple}/obj/
```

This handles multi-step transforms: Fortran `.f` → preprocessed `.f90` →
object; Modula-2 `.m` → symbol table `.sym` + object `.o`; C++ module
interface units → `.pcm` + object. The build phase also runs the
module pre-scan sub-phase (Chapter 8 §8.2) for languages that declare it.

## 2.4 `link` as a Standalone Phase

`now link` does not imply `build`. It links existing object files.
Useful when only link flags or the dep set has changed.

```sh
now link
now link --target linux:amd64:musl
now link :scheduler
now procure link        ; re-procure deps then re-link (no rebuild)
```

## 2.5 CLI Invocation

```sh
now build                       ; procure → generate → build → link
now build :scheduler            ; named target only
now build --target linux:*:musl ; fan out across triples (Chapter 12)
now link                        ; link only
now test                        ; procure → generate → build → link → test
now test -p debug -p asan       ; with profiles
now clean build                 ; clean then full build
now procure                     ; dep resolution only
now generate                    ; procure → generate
now publish --repo https://pkg.acme.org/now
now vacate org.acme:core:1.5.0
now vacate --all --purge
now compile-db                  ; emit compile_commands.json (IDE, Chapter 17)
```

## 2.6 Incremental Builds

`now` maintains a per-project build manifest at `target/.now-manifest` (a
Pasta file). Each entry records: source path, last-modified timestamp,
source SHA-256, header SHA-256s, compiler flags fingerprint, tool binary
SHA-256, target triple, and output `.o` path and SHA-256.

A source file is recompiled when any tracked input changes. The link phase
re-runs when any `.o` changes or link flags change. All source types —
C, C++, assembly, Modula-2, Fortran — use the same incremental tracking;
no per-language special cases.

## 2.7 Parallelism

The build phase runs source files in parallel. Default job count: number
of logical CPU cores. Override:

```sh
now build -j 4
```

Or permanently in `~/.now/config.pasta`:

```pasta
{ jobs: 4 }
```

Phases are always sequential — `build` never begins until `generate`
completes.

## 2.8 Exit Codes

| Code | Meaning |
|------|---------|
| `0` | Success. |
| `1` | One or more errors produced. |
| `2` | Internal `now` failure (bug). |
| `3` | Input not found (no `now.pasta`, missing file). |
| `4` | Network error during `procure`. |
| `5` | Compiler or tool subprocess failed. |
| `10` | Test failures (all tests ran, some failed). |
| `11` | Test execution error (tests could not run). |

Full error code catalogue in Chapter 27.



---



---

# Chapter 3 — Directory Layout

`now` follows a conventional layout derived from Maven's src/main and
src/test separation, adapted for native languages. Every path is
configurable in `now.pasta`; the convention minimises configuration.

## 3.1 Standard Project Tree

```
myproject/
├── now.pasta                   Project descriptor
├── now.lock.pasta              Locked dependency graph (commit to VCS)
├── layers.lock.pasta           Locked layer versions for repo/url sources (commit to VCS)
│
├── src/
│   ├── main/
│   │   ├── c/                  C production sources       (.c)
│   │   ├── cpp/                C++ production sources     (.cpp)
│   │   ├── asm/                Assembly sources            (.s .asm)
│   │   └── include/            Public headers              (.h .hpp)
│   │       └── myproject/      Namespaced header subdir
│   └── test/
│       ├── c/                  C test sources
│       ├── cpp/                C++ test sources
│       └── include/            Test-only headers
│
└── target/                     All generated output (gitignored)
    ├── obj/
    │   ├── main/               Object files (mirrors src/main tree)
    │   └── test/               Object files (mirrors src/test tree)
    ├── bin/                    Final linked output
    ├── pkg/                    Packaged artifact tarball
    ├── generated/              Code-gen plugin output
    ├── .now-manifest           Incremental build state (Pasta)
    └── .now-prescan/           Module pre-scan dependency cache (Chapter 8)
```

`target/` is always gitignored. `now.lock.pasta` and `layers.lock.pasta`
are always committed to VCS.

## 3.2 Object File Naming

Object files mirror the source tree. Extension is appended, not replaced:

```
src/main/c/net/http/parser.c   →   target/obj/main/net/http/parser.c.o
src/main/cpp/core/engine.cpp   →   target/obj/main/core/engine.cpp.o
```

The `.c.o` double-extension convention avoids collisions between `foo.c`
and `foo.cpp` in different subdirectories.

## 3.3 Multi-Module Projects

```
acme-platform/
├── now.pasta           workspace root (declares modules:)
├── core/
│   ├── now.pasta
│   └── src/...
├── net/
│   ├── now.pasta       deps: [{id: "org.acme:core:*"}]
│   └── src/...
└── cli/
    ├── now.pasta       deps: [{id: "org.acme:net:*"}]
    └── src/...
```

Modules are built in dependency order derived from their `deps[]` lists.
Sibling references are resolved from the local module tree before the
registry.

## 3.4 Local Repo Layout (`~/.now/repo/`)

```
~/.now/
├── repo/
│   └── {group}/{artifact}/{version}/
│       ├── {artifact}-{version}.tar.gz
│       ├── {artifact}-{version}.sha256
│       ├── now.pasta
│       ├── h/                      Public headers
│       ├── lib/                    Libraries
│       │   └── {os}-{arch}-{abi}/  Platform triple directory
│       ├── mod/                    Compiled module interfaces (Chapter 8)
│       └── asm/                    Assembly includes
│
├── cache/
│   └── {group}/{artifact}/{version}/
│       └── {artifact}-{version}-{triple}.tar.gz
│
└── config.pasta        Global user configuration
```

Platform directories use the full platform triple with `:` replaced by `-`
for filesystem compatibility:

| Triple | Directory |
|--------|-----------|
| `linux:x86_64:gnu` | `linux-x86_64-gnu` |
| `linux:aarch64:gnu` | `linux-aarch64-gnu` |
| `linux:x86_64:musl` | `linux-x86_64-musl` |
| `macos:x86_64:none` | `macos-x86_64-none` |
| `windows:x86_64:msvc` | `windows-x86_64-msvc` |

## 3.5 Credentials

Authentication credentials are never stored in `now.pasta`. They live in:

```
~/.now/credentials.pasta    ; chmod 600 — now refuses to read without this
```

```pasta
{
  registries: [
    { url: "https://pkg.acme.org/now", username: "alice", token: "ghp_xxx" }
  ]
}
```

`now publish` matches credentials by registry URL prefix.



---



---

# Chapter 4 — Source Classification and Language Type System

> **Scope note — v1: C and C++ only.** The language type system supports
> many native languages, but the v1 release targets **C and C++** exclusively.
> Sections covering Ada, Fortran, Pascal, Modula-2, and other languages are
> fully specified for future releases; implementers should skip them during
> the initial build and return when the C/C++ core is stable.


The insight: a **language** is not a string tag but a structured
declaration — a named registry of **defined types**, where each type maps
a file pattern to a tool invocation and declares what that invocation
produces.

This model is open: `now` ships built-in language definitions, but projects
can declare new languages or extend existing ones in `now.pasta`.

---

## 4.0 Source File Taxonomy

| Class | Definition | Examples |
|-------|------------|---------|
| **Generator** | Produces other source files when processed | `.proto`, `.idl`, `.y`, `.l`, `.x` |
| **Generated** | Produced by processing a generator | `.pb.c`, `.pb.h`, `y.tab.c` |
| **Derived include** | Header filtered for another language's FFI | Pascal `.pas`, Modula-2 `.def` from `.h` |
| **Compiled source** | Compiled directly into an object | `.c`, `.cpp`, `.s`, `.asm` |
| **Pure include** | Never compiled alone; included by others | `.h`, `.hpp`, `.inc` |

Generators drive additional `generate`-phase work. Generated files are
tracked in VCS-ignored paths. Derived includes rebuild when their parent
changes. Incremental builds track generator → generated → compiled chains.


## 4.1 Language as a Structured Declaration

A language definition has the form:

```pasta
{
  id:          "c",
  name:        "C",
  std_flag:    "-std=${std}",     ; how to pass the standard to the compiler
  types: [
    { ... },   ; one entry per file type this language handles
    { ... }
  ]
}
```

A **type** within a language defines:

```pasta
{
  id:        "c-source",          ; unique within this language
  pattern:   ["**.c"],            ; glob(s) that identify files of this type
  role:      "source",            ; source | header | intermediate | generated
  tool:      "${cc}",             ; tool variable to invoke
  args:      [                    ; argument template
    "${std_flag}",
    "${warning_flags}",
    "${define_flags}",
    "${include_flags}",
    "${compile_flags}",
    "-c", "${input}",
    "-o", "${output}",
    "-MMD", "-MF", "${depfile}"
  ],
  produces:  "object",            ; object | intermediate | none
  output_ext: ".c.o"              ; output filename = input basename + this ext
}
```

### `role` values

| Role | Meaning |
|------|---------|
| `source` | Compiled/processed into an object or intermediate |
| `header` | Never compiled alone; tracked for dependency changes |
| `intermediate` | Output of one tool step, input to another (re-enters build loop) |
| `generated` | Produced by the generate phase; classified on re-entry |

### `produces` values

| Value | Meaning |
|-------|---------|
| `object` | Produces a linkable `.o` / `.obj` — build loop terminates for this file |
| `intermediate` | Produces another source file — re-enters build loop with new classification |
| `none` | Tool runs for side effects only (e.g. linting, formatting check) |

---

## 4.2 Built-In Language Definitions

`now` ships the following language definitions. They are equivalent to what
a project would declare, but built-in so nothing needs to be written for
common cases.

### Language: `c`

```pasta
{
  id: "c",
  std_flag: "-std=${std}",
  types: [
    {
      id:         "c-source",
      pattern:    ["**.c"],
      role:       "source",
      tool:       "${cc}",
      args:       ["${std_flag}", "${warning_flags}", "${define_flags}",
                   "${include_flags}", "${compile_flags}",
                   "-c", "${input}", "-o", "${output}",
                   "-MMD", "-MF", "${depfile}"],
      produces:   "object",
      output_ext: ".c.o"
    },
    {
      id:         "c-preprocessed",
      pattern:    ["**.i"],
      role:       "source",
      tool:       "${cc}",
      args:       ["-x", "cpp-output", "${compile_flags}",
                   "-c", "${input}", "-o", "${output}"],
      produces:   "object",
      output_ext: ".i.o"
    },
    {
      id:      "c-header",
      pattern: ["**.h"],
      role:    "header"
      ; no tool, no args — tracked only
    }
  ]
}
```

### Language: `c++`

```pasta
{
  id: "c++",
  std_flag: "-std=${std}",
  types: [
    {
      id:         "cxx-source",
      pattern:    ["**.cpp", "**.cxx", "**.cc", "**.C", "**.c++"],
      role:       "source",
      tool:       "${cxx}",
      args:       ["${std_flag}", "${warning_flags}", "${define_flags}",
                   "${include_flags}", "${compile_flags}",
                   "-c", "${input}", "-o", "${output}",
                   "-MMD", "-MF", "${depfile}"],
      produces:   "object",
      output_ext: ".cpp.o"
    },
    {
      id:      "cxx-header",
      pattern: ["**.hpp", "**.hh", "**.hxx", "**.H"],
      role:    "header"
    }
  ]
}
```

### Language: `asm-gas`

```pasta
{
  id: "asm-gas",
  types: [
    {
      id:         "gas-source",
      pattern:    ["**.s"],
      role:       "source",
      tool:       "${as}",
      args:       ["${asm_flags}", "${input}", "-o", "${output}"],
      produces:   "object",
      output_ext: ".s.o"
    },
    {
      id:         "gas-cpp-source",
      pattern:    ["**.S"],
      role:       "source",
      tool:       "${cc}",
      args:       ["-x", "assembler-with-cpp",
                   "${define_flags}", "${include_flags}",
                   "-c", "${input}", "-o", "${output}"],
      produces:   "object",
      output_ext: ".S.o"
    },
    {
      id:      "gas-include",
      pattern: ["**.inc"],
      role:    "header"
    }
  ]
}
```

### Language: `asm-nasm`

```pasta
{
  id: "asm-nasm",
  types: [
    {
      id:         "nasm-source",
      pattern:    ["**.asm"],
      role:       "source",
      tool:       "${asm}",
      args:       ["-f", "${nasm_format}", "${nasm_include_flags}",
                   "${input}", "-o", "${output}"],
      produces:   "object",
      output_ext: ".asm.o"
    },
    {
      id:      "nasm-include",
      pattern: ["**.inc", "**.asm.h"],
      role:    "header"
    }
  ]
}
```

### Language: `objc`

```pasta
{
  id: "objc",
  std_flag: "-std=${std}",
  types: [
    {
      id:         "objc-source",
      pattern:    ["**.m"],
      role:       "source",
      tool:       "${cc}",
      args:       ["-x", "objective-c", "${std_flag}",
                   "${warning_flags}", "${define_flags}", "${include_flags}",
                   "${compile_flags}", "-c", "${input}", "-o", "${output}",
                   "-MMD", "-MF", "${depfile}"],
      produces:   "object",
      output_ext: ".m.o"
    },
    {
      id:         "objcxx-source",
      pattern:    ["**.mm"],
      role:       "source",
      tool:       "${cxx}",
      args:       ["-x", "objective-c++", "${std_flag}",
                   "${warning_flags}", "${define_flags}", "${include_flags}",
                   "${compile_flags}", "-c", "${input}", "-o", "${output}",
                   "-MMD", "-MF", "${depfile}"],
      produces:   "object",
      output_ext: ".mm.o"
    }
  ]
}
```

### Language: `modula2`

```pasta
{
  id: "modula2",
  types: [
    {
      id:         "mod-implementation",
      pattern:    ["**.mod"],
      role:       "source",
      tool:       "${cc}",          ; gm2 is invoked as a gcc frontend
      args:       ["-fmodula-2", "${std_flag}", "${compile_flags}",
                   "-c", "${input}", "-o", "${output}"],
      produces:   "object",
      output_ext: ".mod.o"
    },
    {
      id:         "mod-definition",
      pattern:    ["**.def"],
      role:       "header"          ; .def files are module interfaces, not compiled
    },
    {
      id:         "mod-m-source",
      pattern:    ["**.m"],
      role:       "source",
      tool:       "${cc}",
      args:       ["-fmodula-2", "${compile_flags}",
                   "-c", "${input}", "-o", "${output}"],
      produces:   "object",
      output_ext: ".m.o"
    }
  ]
}
```

Note: `.m` is claimed by `modula2` only when `lang: "modula2"` is active.
The ambiguity with Objective-C is resolved at the language level, not the
type level — exactly one language is active for `.m` resolution.

### Language: `fortran`

```pasta
{
  id: "fortran",
  types: [
    {
      id:         "f90-source",
      pattern:    ["**.f90", "**.f95", "**.f03", "**.f08"],
      role:       "source",
      tool:       "${fc}",
      args:       ["${std_flag}", "${compile_flags}",
                   "-c", "${input}", "-o", "${output}"],
      produces:   "object",
      output_ext: ".f90.o"
    },
    {
      ; Fixed-form Fortran — preprocessor produces free-form intermediate
      id:         "f77-source",
      pattern:    ["**.f", "**.for", "**.ftn"],
      role:       "source",
      tool:       "${fc}",
      args:       ["-ffixed-form", "${compile_flags}",
                   "-c", "${input}", "-o", "${output}"],
      produces:   "object",
      output_ext: ".f.o"
    }
  ]
}
```

---

## 4.3 Intermediate-Producing Types

When `produces: "intermediate"`, the tool's output is a new source file
that re-enters the build loop. The intermediate is classified by the build
loop as a new type and compiled again.

### Example: Pascal with pre-processing

Free Pascal can emit preprocessed `.pp` files before final compilation.
A project might want to run a custom preprocessor first:

```pasta
; In a custom language definition
{
  id: "pascal-with-macros",
  types: [
    {
      id:         "ppm-source",
      pattern:    ["**.ppm"],         ; custom macro-pascal
      role:       "source",
      tool:       "ppm-expand",       ; custom macro expander
      args:       ["${input}", "-o", "${output}"],
      produces:   "intermediate",
      output_ext: ".pp",              ; output is a .pp file
      output_dir: "target/generated/pascal"
    },
    {
      id:         "pascal-source",
      pattern:    ["**.pp", "**.pas"],
      role:       "source",
      tool:       "${fpc}",
      args:       ["${compile_flags}", "${include_flags}", "${input}"],
      produces:   "object",
      output_ext: ".pp.o"
    }
  ]
}
```

Build loop for `src/main/pascal/screen.ppm`:

```
1. classify screen.ppm        → type: ppm-source (produces: intermediate)
2. invoke ppm-expand          → target/generated/pascal/screen.pp
3. classify screen.pp         → type: pascal-source (produces: object)
4. invoke fpc                 → target/obj/main/pascal/screen.pp.o
5. loop terminates (object)
```

The manifest tracks the full chain: `screen.ppm → screen.pp → screen.pp.o`.
Any change to `screen.ppm` invalidates both the intermediate and the object.

---

## 4.4 Declaring Languages in `now.pasta`

Projects declare which languages are active and can extend or override
built-in language definitions:

```pasta
{
  ; Simple: single language by id
  lang: "c",
  std:  "c11",

  ; OR: multiple active languages
  langs: ["c", "asm-nasm"],

  ; OR: extend a built-in language
  langs: [
    "c",
    "asm-nasm",
    {
      ; Override the nasm format for this project
      extends: "asm-nasm",
      types: [
        {
          id:   "nasm-source",          ; override this type in the parent
          args: ["-f", "elf64", "-g",   ; always elf64 debug
                 "${nasm_include_flags}",
                 "${input}", "-o", "${output}"]
        }
      ]
    }
  ],

  ; OR: define a fully custom language
  langs: [
    "c",
    {
      id: "my-dsl",
      types: [
        {
          id:         "dsl-source",
          pattern:    ["**.mydsl"],
          role:       "source",
          tool:       "mydsl-compiler",
          args:       ["--target", "${arch}", "${input}", "-o", "${output}"],
          produces:   "object",
          output_ext: ".mydsl.o"
        }
      ]
    }
  ]
}
```

---

## 4.5 Type Variable Reference

Variables available in `args` templates:

| Variable | Value |
|----------|-------|
| `${input}` | Absolute path to the input source file |
| `${output}` | Absolute path to the output file |
| `${depfile}` | Absolute path to the dependency tracking file |
| `${std_flag}` | The language's `std_flag` with `${std}` resolved |
| `${std}` | The `std` value from `now.pasta` |
| `${cc}` | C compiler executable |
| `${cxx}` | C++ compiler executable |
| `${fc}` | Fortran compiler executable |
| `${as}` | AT&T assembler |
| `${asm}` | NASM/MASM assembler |
| `${fpc}` | Free Pascal compiler |
| `${warning_flags}` | Expanded from `compile.warnings[]` |
| `${define_flags}` | Expanded from `compile.defines[]` |
| `${include_flags}` | Expanded from `compile.includes[]` and dep includes |
| `${compile_flags}` | Expanded from `compile.flags[]` |
| `${nasm_format}` | Platform-derived NASM output format |
| `${nasm_include_flags}` | NASM-style `-I` flags (trailing slash convention) |
| `${asm_flags}` | Raw assembler flags from `compile.asm_flags[]` |
| `${arch}` | Architecture component of active target triple |
| `${os}` | OS component of active target triple |
| `${variant}` | Variant component of active target triple |
| `${basedir}` | Project root absolute path |
| `${target}` | `target/` directory absolute path |

---

## 4.6 Conflict Resolution Between Languages

When multiple active languages claim the same extension, the resolution
order is:

1. Explicit `sources.classify` glob override (always wins).
2. The first language in `langs[]` that claims the extension wins.
3. Built-in languages are ordered after user-declared ones.

```pasta
; .m is claimed by both objc and modula2 in the built-in definitions.
; Declaring modula2 first gives it priority for .m:
langs: ["modula2", "c"]

; Declaring objc first gives it priority for .m:
langs: ["objc", "c"]
```

If two languages in `langs[]` both claim the same extension and neither
is ordered before the other, `now` emits an error at startup listing the
conflict, not at build time.



---



---

# Chapter 5 — Artifact Model and Installed Layout

An artifact is anything `now` produces that can be stored, versioned,
depended upon, and reproduced. This document defines what an artifact is,
what it contains, how it is identified, and how it is consumed downstream.

---

## 5.1 Artifact Identity

Every artifact is uniquely identified by its **coordinate**:

```
group:artifact:version
```

Examples:
```
org.acme:rocketlib:3.0.0
iridiumfx.pasta:libpasta:1.2.0
zlib:zlib:1.3.1
```

- `group`: reverse-domain namespace. Dots are separators. No slashes.
- `artifact`: short project name. Lowercase, hyphens allowed, no dots.
- `version`: semantic version. Full SemVer 2.0 including pre-release and
  build metadata: `1.0.0-beta.2+build.42`.

The coordinate is the only thing another project's `deps[]` entry needs.
Platform, variant, and scope are separate concerns.

---

## 5.2 Artifact Types

| Type          | Output              | Installed as               |
|---------------|---------------------|----------------------------|
| `executable`  | Binary program      | bin/ in repo (not linkable)|
| `static`      | `.a` / `.lib`       | lib/ in repo               |
| `shared`      | `.so` / `.dylib` / `.dll` | lib/ in repo         |
| `header-only` | No compiled output  | include/ only in repo      |
| `package`     | Collection of other artifacts (multi-output) | each sub-artifact installed separately |

### Static vs Shared

A project may produce both a static and shared variant in one build:

```pasta
output: [
  { type: "static", name: "rocket" },
  { type: "shared", name: "rocket", soversion: "3" }
]
```

When `output` is an array, both are built. The `package` phase bundles all
outputs into one archive. Downstream deps declare which variant they want
via the `link` scope modifier (see document 06).

### Header-Only

No compilation occurs. The `package` phase bundles only the public headers
and `now.pasta`. Useful for template libraries or pure-macro libraries.

```pasta
{
  group:    "org.acme",
  artifact: "acme-concepts",
  version:  "1.0.0",
  lang:     "c",
  output:   { type: "header-only" }
}
```

---

## 5.3 The Package Archive

The canonical artifact on disk and in transit is a `.tar.gz` archive.

### Archive Name

```
{artifact}-{version}-{platform}.tar.gz
```

For header-only artifacts (no platform dependency):
```
{artifact}-{version}-noarch.tar.gz
```

### Archive Contents

```
{artifact}-{version}/
├── now.pasta                     Descriptor (required)
├── now.lock.pasta                Locked dep graph (required)
├── LICENSE                       License file (if present)
├── include/                      Public headers
│   └── myproject/
│       └── api.h
├── lib/
│   ├── linux-x86_64/
│   │   ├── libmyproject.a
│   │   └── libmyproject.so.3
│   └── macos-arm64/
│       ├── libmyproject.a
│       └── libmyproject.3.dylib
└── bin/                          Only for type: executable
    └── linux-x86_64/
        └── myproject
```

Multi-platform archives are produced by the `package` phase when building
on a system with cross-compilation configured. Single-platform builds
produce archives with only one platform directory.

### Checksum File

Every archive is accompanied by a `.sha256` file:

```
{artifact}-{version}-{platform}.sha256
```

Contents: single line, hex SHA-256 of the archive file. Used by `procure`
to verify integrity and by `now.lock.pasta` for pinning.

---

---

The project source tree never contains third-party code. Dependencies are
installed into a canonical layout under `~/.now/repo/` and referenced from
there at build time.

---

## 5.4 The Principle

Maven's local repository (`~/.m2/repository/`) is one of its best ideas.
Dependencies are not vendored, not copied, not subtree-merged. They are
installed once, referenced by coordinate, and shared across all projects
on the machine.

`now` adopts this fully. A dependency is never "inside" your project.
The project source tree contains only what a human wrote for this project.

This has a corollary: the installed layout of a dependency must be
predictable from its coordinate alone. Given `amiga.os:intuition:1.3.0`,
you can derive exactly where every file lives without reading any manifest.

---

## 5.5 The Installed Layout

Every artifact is installed under:

```
~/.now/repo/{group-path}/{artifact}/{version}/
```

where `{group-path}` is the group with dots replaced by directory
separators: `amiga.os` → `amiga/os`.

Within that root, the layout mirrors the logical structure of the artifact:

```
~/.now/repo/{group-path}/{artifact}/{version}/
├── h/                    Public headers (all languages)
│   ├── {artifact}/       Namespaced subdirectory (conventional, not enforced)
│   │   └── **.h
│   └── **.h              Flat headers if the project chose not to namespace
├── c/                    C source files (if artifact ships source)
│   └── **.c
├── cpp/                  C++ source files (if artifact ships source)
│   └── **.cpp
├── lib/                  Compiled libraries
│   ├── {triple}/         Per-platform compiled libraries
│   │   ├── lib{artifact}.a
│   │   └── lib{artifact}.so   (or .dylib / .dll)
│   └── noarch/           Platform-independent (e.g. linker scripts)
│       └── {artifact}.ld
├── pascal/               Pascal units (if artifact ships Pascal FFI)
│   └── **.pas
├── modula2/              Modula-2 definition modules
│   └── **.def
├── asm/                  Assembly includes
├── mod/                  Compiled module interfaces (.pcm, .bmi) for C++20/Ada/Modula-2 (doc 33 §33.6)
│   └── **.inc
├── bin/                  Executables (if artifact is executable type)
│   └── {triple}/
│       └── {artifact}
├── now.pasta             Installed descriptor
└── now.lock.pasta        Locked dep graph of this artifact
```

The subdirectory names (`h`, `c`, `lib`, `pascal`, `modula2`, `asm`)
are fixed by convention. They map directly to the language IDs in the
type system (document 04c), with `h` as the canonical alias for headers
regardless of language.

---

## 5.6 The Intuition Example

```
~/.now/repo/amiga/os/intuition/1.3.0/
├── h/
│   └── intuition/
│       ├── intuition.h
│       ├── screens.h
│       ├── gadgets.h
│       └── classes/
│           └── image.h
├── lib/
│   └── amiga:m68k:none/
│       └── libintuition.a
├── asm/
│   └── intuition/
│       ├── intuition.inc       ; struct offsets for m68k assembly
│       └── intuition_lvo.inc   ; library vector offsets
└── now.pasta
```

A project using this dep gets:

- `-I~/.now/repo/amiga/os/intuition/1.3.0/h` added to compile flags.
- `-L~/.now/repo/amiga/os/intuition/1.3.0/lib/amiga:m68k:none/` added to link flags.
- `-lintuition` added to link flags.
- For assembly: `${asm_include_flags}` includes
  `~/.now/repo/amiga/os/intuition/1.3.0/asm`.

The consuming project does not need to know any of this. It declares
the dep and `now procure` configures all paths automatically.

---

## 5.7 Header Namespacing Convention

The layout does not enforce namespacing inside `h/`, but convention
strongly recommends it: place headers in a subdirectory named after the
artifact. This means:

```c
#include "intuition/intuition.h"    /* good: unambiguous */
#include "intuition.h"              /* fragile: could collide */
```

`now` does not add `-I~/.now/repo/.../h/intuition/` — it adds
`-I~/.now/repo/.../h/` and the project uses the subdirectory in the
`#include` path. This is exactly how system headers work on POSIX (`/usr/include/`
contains `sys/`, `linux/`, `net/`, etc.).

For artifacts that ship flat (no subdirectory), the installed layout is
still flat under `h/`, and the consumer must use unqualified names.
This is noted in the artifact's `now.pasta` as `header_style: "flat"` vs
`header_style: "namespaced"` (informational, not enforced by `now`).

---

## 5.8 Language-Scoped Include Routing

When `now` adds include paths for a dep, it filters by the consuming
project's active languages:

| Consumer `langs` | Paths added |
|-----------------|-------------|
| `["c"]` | `h/` |
| `["c", "asm-nasm"]` | `h/`, `asm/` |
| `["c", "pascal"]` | `h/`, `pascal/` |
| `["modula2"]` | `h/`, `modula2/` |

A Pascal project consuming `intuition` gets `pascal/` on its include path.
An assembly project gets `asm/`. A C project gets only `h/`. No consumer
sees paths irrelevant to its language set.

The mapping from language ID to subdirectory:

| Language ID | Include subdir |
|-------------|---------------|
| `c` | `h/` |
| `c++` | `h/` (same; C++ consumes C headers) |
| `objc` | `h/` |
| `asm-gas` | `asm/` |
| `asm-nasm` | `asm/` |
| `pascal` | `pascal/` |
| `modula2` | `modula2/` |
| `ada` | `ada/` |
| `fortran` | `h/` (Fortran uses C headers for interop via `ISO_C_BINDING`) |

Custom languages declare their include subdir in the language definition:

```pasta
{
  id:          "my-dsl",
  include_dir: "mydsl",    ; uses ~/.now/repo/.../mydsl/ for includes
  types:       [ ... ]
}
```

---

## 5.9 Source Artifacts

Some dependencies ship C source directly rather than (or in addition to)
compiled libraries. This is common for:

- Tiny, single-file libraries (stb-style).
- Libraries that must be compiled with the consumer's exact flags
  (e.g. a library that uses `NDEBUG` to change struct layout).
- Header-only libraries that have optional `.c` implementation files.

```
~/.now/repo/stb/image/2.29.0/
├── h/
│   └── stb/
│       └── stb_image.h
├── c/
│   └── stb/
│       └── stb_image.c     ; consumer compiles this themselves
└── now.pasta
```

The installed `now.pasta` for a source artifact declares:

```pasta
{
  output: { type: "source" },

  consumer_compile: {
    includes: ["h"],
    sources:  ["c"]   ; these source files are added to the consumer's build
  }
}
```

When `now procure` processes a `source`-type dep, it adds the dep's `c/`
directory to the consuming project's source set. The dep's `.c` files are
compiled as part of the consumer's `build` phase, with the consumer's
flags. They appear in `target/obj/` alongside the project's own objects.

This is a deliberate trade-off: the consumer gets full control over
compilation, but the dep is not pre-compiled. The build manifest tracks
these source-type dep files like any other source.

---

## 5.10 The Installed Descriptor (`now.pasta` in the Repo)

The `now.pasta` stored in `~/.now/repo/` is the **installed descriptor** —
a curated subset of the project's build-time descriptor. It tells
consumers what they need to know, nothing more.

```pasta
; ~/.now/repo/amiga/os/intuition/1.3.0/now.pasta
{
  group:    "amiga.os",
  artifact: "intuition",
  version:  "1.3.0",
  lang:     "c",

  output: { type: "static" },

  ; What consumers need (generated by `now package`, not hand-written)
  consumer_compile: {
    includes: ["h"],          ; relative to this artifact's install root
    defines:  ["INTUITION_LIB"]
  },

  consumer_link: {
    libs:    ["intuition"],
    libdirs: ["lib/${now.triple}"]
  },

  consumer_asm: {
    includes: ["asm"]         ; for assembly consumers
  },

  ; Transitive deps consumers must also procure
  deps: [
    { id: "amiga.os:exec:1.3.0",     scope: "compile" },
    { id: "amiga.os:graphics:1.3.0", scope: "compile" }
  ]
}
```

`${now.triple}` in `consumer_link.libdirs` is resolved at procure time
to the consuming project's target triple. This is the only interpolation
performed in installed descriptors — everything else is literal.

---

## 5.11 Shared Machine, Multiple Projects

Because `~/.now/repo/` is shared across all projects on a machine, a dep
installed for project A is immediately available to project B without
re-downloading. The sha256 in the lock file is the integrity guarantee —
if a file in the repo is corrupted or replaced, `now procure` detects the
mismatch and re-fetches.

Two projects depending on different versions of the same artifact coexist
cleanly because the version is part of the path:

```
~/.now/repo/zlib/zlib/1.3.0/
~/.now/repo/zlib/zlib/1.3.1/
~/.now/repo/zlib/zlib/1.4.0-rc1/
```

There is no "installed version wins" ambiguity. The lock file pins the
exact version; the repo path is derived from the pinned version.

---

## 5.12 Vacate and the Shared Repo

Because the repo is shared, `now vacate` must be conservative:

```
vacate policy:
  a dep is vacated only if no other project's now.lock.pasta on this
  machine references it.
```

`now` maintains a reference count file at
`~/.now/repo/{group-path}/{artifact}/{version}/.refcount`. Each
`now procure` increments it; each `now vacate` decrements it. When the
count reaches zero, the files are removed.

```sh
now vacate --force    ; ignore refcount, remove regardless
now vacate --gc       ; scan all known projects and remove unreferenced deps
```

`now vacate --gc` is the safe way to clean up after projects are deleted
or their deps change — it rebuilds refcounts from scratch by scanning
all `now.lock.pasta` files it can find under common project roots, then
removes anything with a zero count.



---



---

# Chapter 6 — Dependency Resolution and Versioning

Dependency resolution is the process of taking the `deps[]` list in
`now.pasta`, expanding transitive dependencies, resolving version conflicts,
and producing a complete, reproducible, locked dependency graph.

---

## 6.1 Dependency Declaration

```pasta
deps: [
  { id: "org.acme:core:^4.0.0",    scope: "compile"  },
  { id: "zlib:zlib:~1.3.0",        scope: "compile"  },
  { id: "unity:unity:2.5.2",       scope: "test"     },
  { id: "org.acme:mock-io:1.0.0",  scope: "test"     },
  { id: "org.acme:concepts:3.0.0", scope: "provided" }
]
```

### Scopes

| Scope      | Compile | Link | Test compile | Test link | Installed descriptor |
|------------|---------|------|--------------|-----------|----------------------|
| `compile`  | yes     | yes  | yes          | yes       | yes                  |
| `provided` | yes     | no   | yes          | no        | yes                  |
| `test`     | no      | no   | yes          | yes       | no                   |
| `runtime`  | no      | yes  | no           | yes       | yes                  |

- **`compile`**: the normal case. Headers available at compile time, lib linked.
- **`provided`**: headers available, but the lib is expected to be supplied
  by the runtime environment (e.g. libc, OS frameworks). Not linked.
- **`test`**: only visible to the test build. Not included in the installed
  descriptor, not transitively propagated.
- **`runtime`**: not compiled against (no headers exposed), but linked. Used
  for plugin-style dependencies loaded at startup.

### Optional Attributes

```pasta
deps: [
  {
    id:      "org.acme:gui:2.0.0",
    scope:   "compile",
    variant: "static",           ; prefer static variant (see doc 05)
    optional: true               ; project builds even if this dep is absent
  }
]
```

`optional: true` deps are included only if resolvable. If absent, `now`
defines a preprocessor symbol `NOW_DEP_{GROUP}_{ARTIFACT}_ABSENT` so code
can `#ifdef` around the missing dependency.

---

## 6.2 Resolution Algorithm

Resolution is a classic PubGrub-style constraint solver operating on the
full dependency graph.

### Step 1: Collect root constraints

Parse all `deps[]` from `now.pasta`. Each entry contributes a version
constraint for a coordinate.

### Step 2: Expand transitively

For each dependency, fetch its installed descriptor (`now.pasta` from the
repo). Collect its `deps[]` (only `compile`, `provided`, `runtime` scopes —
never `test`). Add those constraints to the working set.

Repeat until no new coordinates are encountered. This is a BFS over the
dependency graph.

### Step 3: Resolve conflicts

When two constraints on the same coordinate conflict, apply the **minimum
upper bound wins** rule:

- `^1.2.0` from project A and `^1.3.0` from project B → resolved to
  `>=1.3.0 <2.0.0` (intersection of both ranges).
- If the intersection is empty → **resolution failure**. `now` reports the
  conflict chain and exits.

`now` does **not** silently select one version and ignore another (Maven's
nearest-wins). Ambiguity is always an error. The user must add an explicit
dep entry to pin the version.

### Step 4: Select concrete versions

For each coordinate with a resolved range, query the configured repos
(in declaration order) for available versions. Select the highest version
within the range.

### Step 5: Write lock file

Record every resolved coordinate with its exact version and sha256. This
becomes `now.lock.pasta`.

---

## 6.3 The Lock File (`now.lock.pasta`)

`now.lock.pasta` is generated by `procure` and must be committed to VCS.
It is the single source of truth for what gets downloaded and used.

### Entry Schema

The logical primary key per entry is `(id, triple)`. A project targeting
multiple platform triples has one entry per coordinate per triple, each
with a fully resolved concrete `url`. This ensures the integrity guarantee
is complete across the full declared target matrix — a developer on one
platform has pinned hashes for all other platforms committed to VCS.

Fields per resolved entry:

| Field | Type | Description |
|-------|------|-------------|
| `id` | string | Coordinate: `group:artifact:version` |
| `triple` | string | Platform triple (colons, e.g. `linux:amd64:gnu`) or `"noarch"` |
| `url` | string | Fully resolved archive URL — no template substitution |
| `sha256` | string | Hex SHA-256 of the `.tar.gz` archive |
| `descriptor_sha256` | string | Hex SHA-256 of the `now.pasta` installed descriptor — per-coordinate, same across all triples |
| `scope` | string | `compile`, `provided`, `runtime`, or `test` |
| `deps` | string[] | Direct dep coordinates of this artifact |

### Descriptor URL Derivation

The `now.pasta` descriptor URL for a given lock entry is derived
normatively: take the entry's `url`, replace the filename component
(everything after the last `/`) with `now.pasta`. This rule is
authoritative — implementations must not use any other derivation.

```
url:             .../core/4.2.1/core-4.2.1-linux-amd64-gnu.tar.gz
descriptor url:  .../core/4.2.1/now.pasta
```

`now procure` fetches and verifies the descriptor against
`descriptor_sha256` on every fresh install.

### Example

```pasta
; Auto-generated by now procure. Commit this file. Do not edit manually.
{
  now_version: "1.1.0",    ; version of now that wrote this file
  generated:   "2026-03-05T14:22:00Z",

  resolved: [
    {
      id:                "org.acme:core:4.2.1",
      triple:            "linux:amd64:gnu",
      url:               "https://registry.now.build/central/org/acme/core/4.2.1/core-4.2.1-linux-amd64-gnu.tar.gz",
      sha256:            "a3f8c2d1...",
      descriptor_sha256: "d4e5f6...",   ; same value for all triples of this coordinate
      scope:             "compile",
      deps:              ["zlib:zlib:1.3.1"]
    },
    {
      id:                "org.acme:core:4.2.1",
      triple:            "macos:arm64:none",
      url:               "https://registry.now.build/central/org/acme/core/4.2.1/core-4.2.1-macos-arm64-none.tar.gz",
      sha256:            "b82e14f9...",
      descriptor_sha256: "d4e5f6...",   ; same descriptor, different archive
      scope:             "compile",
      deps:              ["zlib:zlib:1.3.1"]
    },
    {
      id:                "zlib:zlib:1.3.1",
      triple:            "linux:amd64:gnu",
      url:               "https://registry.now.build/central/zlib/zlib/1.3.1/zlib-1.3.1-linux-amd64-gnu.tar.gz",
      sha256:            "b91d449f...",
      descriptor_sha256: "e7f8a9...",
      scope:             "compile",
      deps:              []
    },
    {
      id:                "unity:unity:2.5.2",
      triple:            "noarch",
      url:               "https://registry.now.build/central/unity/unity/2.5.2/unity-2.5.2-noarch.tar.gz",
      sha256:            "c44e201a...",
      descriptor_sha256: "f1a2b3...",
      scope:             "test",
      deps:              []
    }
  ]
}
```

When `now.lock.pasta` exists and is consistent with `now.pasta` (all
declared deps are present in the lock for all declared target triples),
`procure` uses locked coordinates directly — no resolution step. This
guarantees reproducibility across machines and time.

When to re-resolve (regenerate the lock):
- `now.pasta` deps change (addition, removal, version bump).
- `now.pasta` declared target triples change.
- `now procure --refresh` is run explicitly.
- A dep's `sha256` in the lock doesn't match the downloaded archive.
- A dep's `descriptor_sha256` in the lock doesn't match the fetched `now.pasta`.

---

## 6.4 Conflict Reporting

When resolution fails, `now` emits a structured error showing the conflict
chain:

```
Resolution failure: org.acme:core

  Your project requires:
    org.acme:core ^4.0.0

  Transitively, via org.acme:net:2.1.0:
    org.acme:core ^4.5.0

  Transitively, via org.acme:tls:1.0.0:
    org.acme:core ^3.0.0 <4.0.0

  These constraints have no common version.

  To resolve: add an explicit pin to your now.pasta:
    { id: "org.acme:core:4.5.0", scope: "compile" }
  and verify org.acme:tls compatibility with org.acme:core 4.x.
```

---

## 6.5 Repositories

Repos are tried in declaration order. The first repo that has a matching
artifact wins. Repos are declared in `now.pasta` and in
`~/.now/config.pasta`. The user-level list is appended after the
project-level list.

```pasta
repos: [
  "https://registry.now.build/central",    ; public central registry
  "https://pkg.acme.org/now",              ; private corporate registry
  "file://~/.now/repo"                     ; local repo (always appended last implicitly)
]
```

The local repo (`~/.now/repo`) is always checked **first**, regardless of
declaration order. This ensures that `now install` of a local build is
always preferred over remote artifacts with the same coordinate.

### Repository Protocol

Repos expose a simple HTTP API:

```
GET /resolve/{group}/{artifact}/{version-range}
→ JSON or Pasta list of available versions matching the range

GET /artifact/{group}/{artifact}/{version}/{filename}
→ Binary artifact (tar.gz or sha256 file)

PUT /artifact/{group}/{artifact}/{version}/{filename}
→ Publish artifact (authenticated)
```

`file://` repos are directory trees in the local repo layout (see doc 03).

### Authentication — Token Exchange

HTTP registries that require authentication use a two-step credential
flow. Credentials are stored in `~/.now/credentials.pasta` (mode 600):

```pasta
{
  registries: [
    { url: "https://registry.now.build/central", username: "alice", token: "ghp_xxx" }
  ]
}
```

The `token` field is a long-lived credential. At the start of a
`now procure` or `now publish` session, `now` exchanges it for a
short-lived JWT:

```
POST /auth/token
Authorization: Basic base64({username}:{token})

→ { "access_token": "eyJ...", "expires_in": 3600 }
```

All subsequent requests in the session use the JWT as a Bearer token:

```
Authorization: Bearer eyJ...
```

`now` caches the JWT for its declared lifetime and re-exchanges only when
it expires. The long-lived `token` is never sent on artifact or resolve
requests. Registries that do not require authentication ignore the
`Authorization` header. `file://` repos never perform token exchange.

---

## 6.6 Dependency Graph Visualisation

```sh
now dep:tree
```

Prints the full resolved dependency tree:

```
org.acme:myapp:1.0.0
├── compile: org.acme:core:4.2.1
│   └── compile: zlib:zlib:1.3.1
├── compile: org.acme:net:2.1.0
│   ├── compile: org.acme:core:4.2.1  (already listed)
│   └── provided: openssl:openssl:3.2.0
└── test: unity:unity:2.5.2
```

```sh
now dep:why zlib:zlib        ; explain why zlib is in the graph
now dep:check                ; check for known vulnerabilities (if registry supports)
now dep:updates              ; show which deps have newer versions available
```

---

## 6.7 Local Dependency Override

During development of multiple related projects, you may want to consume a
local build of a sibling project instead of the published version:

```sh
now procure --override org.acme:core=../core
```

This substitutes the `../core` local project for `org.acme:core` at any
version. `now` compiles `../core` first (running `now install` there), then
uses the result. The override is ephemeral — it does not modify
`now.lock.pasta`.

For persistent overrides during development, use the `overrides` block in
`~/.now/config.pasta`:

```pasta
{
  overrides: [
    { id: "org.acme:core", path: "~/dev/acme/core" }
  ]
}
```

---

## 6.9 Version Syntax and Ranges

Version management in `now` follows semantic versioning strictly, uses
Maven-style range syntax, and adds an explicit **convergence policy** that
controls how conflicting transitive version constraints are resolved. The
policy is declared in `now.pasta` and applied deterministically — no
resolver surprises.

---

## 6.9 Version Syntax

All versions are SemVer 2.0: `MAJOR.MINOR.PATCH[-pre][+build]`.

| Version | Meaning |
|---------|---------|
| `1.2.3` | Exact version |
| `1.2.3-beta.1` | Pre-release |
| `1.2.3+build.42` | Build metadata (ignored in comparisons) |
| `0.9.0` | Pre-1.0 — minor is breaking by convention |

Pre-release versions (`-alpha`, `-beta`, `-rc`) sort below their release:
`1.0.0-rc.1 < 1.0.0 < 1.0.1`.

---

## 6.10 Version Range Syntax

Ranges in `deps[]` entries express constraints on acceptable versions.

| Syntax | Name | Meaning |
|--------|------|---------|
| `1.2.3` | Exact | Only `1.2.3` |
| `^1.2.3` | Caret / compatible | `>=1.2.3 <2.0.0` |
| `^0.9.3` | Caret (pre-1.0) | `>=0.9.3 <0.10.0` (minor is breaking) |
| `~1.2.3` | Tilde / patch | `>=1.2.3 <1.3.0` |
| `~1.2` | Tilde / minor | `>=1.2.0 <1.3.0` |
| `>=1.2.0` | Floor | Any version at or above `1.2.0` |
| `>=1.2.0 <2.0.0` | Explicit range | Intersection (space = AND) |
| `*` | Any | Any version — forbidden in published artifacts |

### Range Examples

```pasta
deps: [
  { id: "zlib:zlib:^1.3.0",       scope: "compile" },  ; >=1.3.0 <2.0.0
  { id: "org.acme:core:~4.2.0",   scope: "compile" },  ; >=4.2.0 <4.3.0
  { id: "unity:unity:2.5.2",      scope: "test"    },  ; exactly 2.5.2
  { id: "org.acme:util:>=3.0.0",  scope: "compile" }   ; any 3.x or later
]
```

---

## 6.11 Convergence Policy

When two dependencies transitively require the same artifact at different
(but compatible) versions, the resolver must pick one. This is the
**convergence problem**. `now` makes the policy explicit via a `convergence`
field in `now.pasta`.

```pasta
{
  convergence: "lowest"   ; lowest | highest | exact
}
```

Default: `"lowest"`.

### `lowest` — Conservative Convergence

Select the lowest version that satisfies all constraints in the graph.

```
A requires zlib ^1.3.0    → accepts 1.3.0, 1.3.1, 1.4.0, ...
B requires zlib ^1.2.0    → accepts 1.2.0, 1.3.0, 1.4.0, ...
intersection: >=1.3.0 <2.0.0
lowest satisfying: 1.3.0
```

**Rationale**: Minimises surprise. You get the oldest version that makes
everyone happy. Useful when stability matters more than features. Matches
the principle of least surprise for library consumers.

**Risk**: `1.3.0` may have known bugs fixed in `1.3.5`. Mitigated by
explicit floor pinning: `>=1.3.5`.

### `highest` — Aggressive Convergence

Select the highest available version that satisfies all constraints.

```
A requires zlib ^1.3.0
B requires zlib ^1.2.0
intersection: >=1.3.0 <2.0.0
highest available: 1.4.2
```

**Rationale**: Gets security fixes and bug fixes automatically within the
safe range. Useful for applications (not libraries). Similar to Maven's
default nearest-wins but without the non-determinism — `highest` is always
deterministic given a fixed registry state.

**Risk**: Higher versions may introduce subtle behavioural changes even
within a compatible range. Mitigated by the lock file — `highest` is
resolved once and locked; it does not silently upgrade on subsequent builds.

### `exact` — Strict Convergence

All constraints on a given artifact must agree on exactly one version, or
resolution fails.

```
A requires zlib 1.3.1     ; exact
B requires zlib ^1.3.0    ; range
→ FAIL unless B's range contains exactly 1.3.1 and no other version
  is selected by the resolver

; Or equivalently: both must pin the same exact version:
A requires zlib 1.3.1
B requires zlib 1.3.1
→ OK: 1.3.1
```

**Rationale**: Guarantees byte-for-bit identical dependency sets across
all contributors. Used in safety-critical or compliance contexts where
every version change must be deliberate and audited.

**Risk**: Any transitive dep that uses ranges causes an immediate conflict.
Requires all deps in the graph to use exact pinning or compatible exact
pinning. Works best in a closed dependency ecosystem.

---

## 6.12 Convergence Failure Reporting

When convergence fails under any policy, `now` reports the full conflict
chain with the policy applied:

```
Convergence failure (policy: lowest):

  org.acme:core

  Constraints:
    your project         requires  ^4.0.0      (floor: 4.0.0)
    org.acme:net:2.1.0   requires  ^4.5.0      (floor: 4.5.0)
    org.acme:tls:1.0.0   requires  >=3.0.0 <4.0.0

  The constraint from org.acme:tls requires a version below 4.0.0.
  The other constraints require 4.0.0 or above.
  No version satisfies all constraints simultaneously.

  Options:
    1. Upgrade org.acme:tls to a version that supports org.acme:core 4.x.
    2. Add an explicit exclusion:
         { id: "org.acme:core:4.5.0", scope: "compile", override: true }
       and verify org.acme:tls compatibility manually.
    3. Change convergence policy to "highest" to see if a higher version
       resolves the conflict (it will not in this case — the range is disjoint).
```

---

## 6.13 Per-Dependency Overrides

For cases where the global convergence policy is not sufficient, individual
deps can be pinned with `override: true`. An override forces that exact
version regardless of what transitively required versions say.

```pasta
deps: [
  { id: "zlib:zlib:1.3.5", scope: "compile", override: true }
]
```

An override:
- Silences convergence failure for that coordinate.
- Forces the exact version specified.
- Emits a warning at procure time if the overridden version does not
  satisfy some transitive constraint (so you know you are taking a risk).
- Is recorded in `now.lock.pasta` with `overridden: true`.

Overrides should be rare and documented with a comment in `now.pasta`
explaining why the override is necessary.

---

## 6.14 Exclusions

Exclude a transitive dependency entirely — tell `now` to not include it
in the graph even if something requires it:

```pasta
deps: [
  {
    id:    "org.acme:framework:2.0.0",
    scope: "compile",
    exclude: ["org.acme:legacy-logging"]   ; don't pull this transitive dep
  }
]
```

Exclusions apply only to the transitive graph of that specific dep, not
globally. If another direct dep also pulls in `legacy-logging`, it is
still included from that path.

---

## 6.15 Lock File Entries Under Convergence

`now.lock.pasta` records the convergence policy that was used and the
resolved version for each coordinate:

```pasta
{
  now_version:  "1.0.0",
  convergence:  "lowest",
  generated:    "2026-03-05T14:22:00Z",

  resolved: [
    {
      id:          "zlib:zlib:1.3.0",
      url:         "https://registry.now.build/.../zlib-1.3.0-linux-amd64-gnu.tar.gz",
      sha256:      "a3f8c2...",
      scope:       "compile",
      deps:        [],
      converged_from: ["^1.3.0", "^1.2.0"],  ; constraints that were merged
      overridden:  false
    }
  ]
}
```

`converged_from` records the raw constraints that led to the selected
version. This makes it auditable: you can see exactly why `1.3.0` was
chosen and which deps contributed constraints.

---

## 6.16 Pre-release and Snapshot Policy

By default, pre-release versions (`-alpha`, `-beta`, `-rc`) are excluded
from range resolution. A range `^1.0.0` will not resolve to `1.1.0-beta.1`
even if it is the highest available version under `1.1.0`.

To allow pre-releases for a specific dep:

```pasta
{ id: "org.acme:new-thing:^2.0.0", scope: "compile", allow_prerelease: true }
```

To allow pre-releases globally (not recommended for production):

```pasta
{ convergence: "highest", allow_prerelease: true }
```

Snapshot versions (`-SNAPSHOT`) are never resolved by range — they must be
pinned exactly. A range constraint will never select a snapshot version.



---



---

# Chapter 7 — Toolchain Configuration — GCC / Clang

`now` is a direct build tool — it invokes the compiler, assembler, archiver,
and linker itself. It does not generate Makefiles or Ninja files. This
document defines how toolchains are selected, configured, and invoked.

---

## 7.1 Toolchain Resolution Order

For each tool, `now` resolves the executable in this order:

1. `compile.cc` / `compile.cxx` / etc. in `now.pasta` (project-level)
2. Active profile overrides to the above
3. `~/.now/config.pasta` global tool settings
4. `CC`, `CXX`, `AR`, `AS`, `LD` environment variables
5. Named toolchain preset (if `toolchain:` is set)
6. Platform default (`cc`/`c++` on POSIX, `cl.exe` on Windows)

Once resolved, `now` verifies the tool exists and is executable. If not,
it reports the resolution chain and fails with a clear message.

---

## 7.2 Tool Variables

| Variable | Role | Default (POSIX) | Default (Windows) |
|----------|------|-----------------|-------------------|
| `cc`     | C compiler | `cc` | `cl.exe` |
| `cxx`    | C++ compiler | `c++` | `cl.exe` |
| `as`     | Assembler (AT&T syntax) | `as` | `ml64.exe` |
| `asm`    | Assembler (NASM/MASM) | `nasm` | `nasm.exe` |
| `ar`     | Static lib archiver | `ar` | `lib.exe` |
| `ld`     | Explicit linker (optional) | *(use compiler driver)* | *(use compiler driver)* |
| `strip`  | Symbol stripper | `strip` | *(n/a)* |
| `objcopy`| Object manipulation | `objcopy` | *(n/a)* |

By default, `now` links by invoking the compiler driver (`cc` or `cxx`),
not `ld` directly. Set `link.ld: "lld"` to force a specific linker via
`-fuse-ld=lld`.

---

## 7.3 Named Toolchain Presets

```pasta
{
  toolchain: "llvm",   ; or "gcc", "msvc", "cross:arm-linux-gnueabihf"
  lang:      "c",
  std:       "c11"
}
```

Built-in presets:

| Preset | cc | cxx | ar | as |
|--------|----|-----|----|----|
| `gcc`  | `gcc` | `g++` | `ar` | `as` |
| `llvm` | `clang` | `clang++` | `llvm-ar` | `llvm-as` |
| `msvc` | `cl.exe` | `cl.exe` | `lib.exe` | `ml64.exe` |
| `tcc`  | `tcc` | *(n/a)* | `ar` | `as` |

For cross-compilation, prefix with `cross:` followed by the GNU target
triple:

```pasta
toolchain: "cross:aarch64-linux-gnu"
```

This sets `cc: "aarch64-linux-gnu-gcc"`, `cxx: "aarch64-linux-gnu-g++"`,
etc., and sets the sysroot if `compile.sysroot` is specified.

---

## 7.5 Assembler Invocation

### AT&T syntax (`.s`, via `as`):
```
$(as) $(asm_flags) $(source) -o $(output)
```

### AT&T syntax with CPP (`.S`, via compiler driver):
```
$(cc) -x assembler-with-cpp $(include_flags) $(define_flags) -c $(source) -o $(output)
```

### NASM syntax (`.asm`, via `nasm`):
```
$(asm) $(nasm_flags) -f $(nasm_format) $(include_flags_nasm) $(source) -o $(output)
```

`nasm_format` is derived from the target platform:
- Linux x86-64 → `elf64`
- macOS x86-64 → `macho64`
- Windows x86-64 → `win64`

`include_flags_nasm` uses `-I` with a trailing slash (NASM convention).

---

## 7.6 Linker Invocation

### Executable or shared library:
```
$(cc_or_cxx) $(link_flags) $(obj_files) $(dep_lib_flags) $(lib_flags) -o $(output)
```

Where `cc_or_cxx` is `cxx` if any C++ objects are present, else `cc`.

| Component | Source | Example |
|-----------|--------|---------|
| `link_flags` | `link.flags[]` verbatim | `-pthread -shared` |
| `obj_files` | All `.o` from `target/obj/main/` | |
| `dep_lib_flags` | From procured deps' `consumer_link` | `-L~/.now/repo/... -lzlib` |
| `lib_flags` | `link.libs[]` prepended with `-l`, `link.libdirs[]` with `-L` | `-lm -ldl` |

Dep libs are linked in reverse topological order (leaves first, same order
as `procure` installation).

### Static library:
```
$(ar) rcs $(output) $(obj_files)
```

`ranlib` is run if the archiver does not index automatically (detected from
`ar --version` output).

---

## 7.7 Parallel Compilation

`now` maintains a job pool of size `jobs` (default: logical CPU count).
Each source file is an independent job. Jobs are submitted in dependency
order where known (files that include many headers compile first so their
`.d` files are available for manifest updates).

No make or Ninja process pool is used — `now` manages processes directly
via `fork/exec` (POSIX) or `CreateProcess` (Windows).

Compilation errors are buffered and printed atomically after a job
completes, preventing interleaved output. The build continues until the
pool is drained, then exits with the first non-zero code (fail-fast mode
is available with `-f`).

---

## 7.8 Sysroot and Cross Compilation

```pasta
{
  toolchain: "cross:aarch64-linux-gnu",

  compile: {
    sysroot: "/opt/sysroots/aarch64-linux-gnu",
    flags:   ["--sysroot=${compile.sysroot}"]
  }
}
```

`${compile.sysroot}` is available as a property for use in flags. The
`procure` phase selects the `aarch64-linux` platform variant of all deps
when cross-compiling. If no such variant exists in the repo, `procure`
fails with a clear message rather than silently using the wrong binary.

---

## 7.9 Compiler Feature Detection

`now` does not run CMake-style `try_compile` tests during the build.
Feature detection, if needed, is the responsibility of a `generate`-phase
plugin. Plugins may write a `target/generated/config.h` with detected
feature macros, following the established autoconf-style pattern.

`now` itself uses no feature detection — it targets a defined standard and
trusts the compiler to enforce it.

---

## 7.10 Verbose and Dry Run Modes

```sh
now compile -v        ; print each compiler invocation as it runs
now compile -vv       ; print full command lines including all resolved flags
now compile --dry-run ; print what would be run, execute nothing
```

In dry-run mode, `now.lock.pasta` is not written or modified.

---


---

# Chapter 8 — Toolchain Configuration — MSVC

This document resolves GAP 11. It extends document 07's MSVC stub (§7.4)
into a complete specification covering: the full flag translation surface,
Program Database (PDB) debug info, DLL export management (`__declspec` and
`.def` files), response files for long command lines, static runtime vs
dynamic runtime (`/MT` vs `/MD`), and MSVC-specific `now.pasta` fields.

`now.pasta` always uses GCC/Clang conventions. The MSVC toolchain layer
translates everything. A project that builds on MSVC should require no
MSVC-specific content in `now.pasta` for the common case — only for
features that have no GCC/Clang equivalent (PDB paths, DEF files,
manifestation) does MSVC-specific configuration appear.

---

## 7.8 Full Flag Translation Table

`now` translates all `now.pasta` compile and link flags to MSVC
equivalents automatically. The table below is the authoritative mapping.

### Compile Flags

| `now.pasta` / GCC form | MSVC equivalent | Notes |
|------------------------|-----------------|-------|
| `-std=c11` | `/std:c11` | C11, C17, C23 supported |
| `-std=c++17` | `/std:c++17` | C++14, C++17, C++20, C++23 |
| `-std=c++20` | `/std:c++20` | |
| `-Wall` | `/W4` | Closest practical equivalent |
| `-Wextra` | `/W4` | Already included in /W4 |
| `-Werror` | `/WX` | Warnings as errors |
| `-Wpedantic` | `/permissive-` | Strict conformance |
| `-Wno-unused` | `/wd4101 /wd4189` | Suppress specific MSVC warnings |
| `-O0` | `/Od` | No optimisation |
| `-O1` | `/O1` | Minimise size |
| `-O2` | `/O2` | Maximise speed |
| `-O3` | `/Ox` | Full optimisation |
| `-Os` | `/Os` | Favour size |
| `-g` | `/Zi` | Debug info → PDB |
| `-g0` | *(omit /Zi)* | No debug info |
| `-DFOO` | `/DFOO` | Identical |
| `-DFOO=bar` | `/DFOO=bar` | Identical |
| `-Ipath` | `/Ipath` | Identical |
| `-isystem path` | `/external:Ipath /external:W0` | System include (suppress warnings) |
| `-c` | `/c` | Compile only |
| `-o file.o` | `/Fo:file.obj` | Output object file |
| `-fvisibility=hidden` | *(no equivalent)* | DLL exports managed via DEF or `__declspec` |
| `-fPIC` | *(no equivalent — MSVC always position-independent)* | Silently ignored |
| `-pthread` | *(no equivalent — MSVC always thread-safe runtime)* | Silently ignored |
| `-fstack-protector` | `/GS` | Stack buffer security check |
| `-fno-stack-protector` | `/GS-` | |
| `-fomit-frame-pointer` | `/Oy` | |
| `-march=native` | `/favor:blend` | Approximate equivalent |
| `-ffreestanding` | `/kernel` | Approximate — kernel mode code |
| `-nostdinc` | `/X` | Ignore standard include dirs |

### Flags with No MSVC Equivalent

When a flag has no MSVC equivalent, `now` applies one of three policies:

| Policy | Meaning | Applied to |
|--------|---------|-----------|
| `ignore` | Silently dropped | `-fPIC`, `-pthread` |
| `warn` | Dropped with a warning | `-fvisibility=hidden` (use DEF/`__declspec` instead) |
| `error` | Build fails | Flags that change semantics fundamentally with no safe equivalent |

Unknown flags in `compile.flags[]` that begin with `-f` or `-m` are
dropped with a warning on MSVC. Flags beginning with `/` are passed
through verbatim (they are MSVC-native and `now` does not translate them).

### Link Flags

| `now.pasta` / GCC form | MSVC equivalent | Notes |
|------------------------|-----------------|-------|
| `-o output` | `/Fe:output.exe` | Executable output |
| `-shared` | `/DLL` | Build DLL |
| `-static` | *(n/a — MSVC links statically to CRT via /MT)* | See §7.11 |
| `-lname` | `name.lib` | Library — added to link inputs |
| `-Lpath` | `/LIBPATH:path` | Library search path |
| `-Wl,--subsystem,console` | `/SUBSYSTEM:CONSOLE` | Console application |
| `-Wl,--subsystem,windows` | `/SUBSYSTEM:WINDOWS` | GUI application |
| `-Wl,-Map,file.map` | `/MAP:file.map` | Linker map |
| `-Wl,--stack,size` | `/STACK:size` | Stack size |
| `-Wl,--heap,size` | `/HEAP:size` | Heap size |
| `-Wl,--entry,sym` | `/ENTRY:sym` | Entry point |
| `-Wl,--version-script` | *(no equivalent — use DEF file)* | See §7.10 |

---

## 7.9 Program Database (PDB) Files

MSVC debug information is written to a separate `.pdb` file — it is not
embedded in the object or binary as with DWARF. `now` manages PDB files
as first-class build outputs.

### PDB Generation

When the active profile includes debug info (`-g` / `/Zi`), `now`
automatically passes PDB-related flags:

```sh
; Per-object PDB (compile step) — /Fd sets PDB path for this object
cl.exe /c /Zi /Fdsrc\main\c\parser.c.pdb src\main\c\parser.c /Fosrc\main\c\parser.c.obj

; Final PDB (link step) — /PDB sets the output PDB
link.exe /PDB:target\windows-amd64-msvc\bin\myapp.pdb /DEBUG ...
```

### PDB Location

`now` places PDB files alongside their corresponding outputs:

```
target/windows-amd64-msvc/
├── bin/
│   ├── myapp.exe
│   └── myapp.pdb          ← linked PDB (full debug info)
└── obj/main/
    └── src/main/c/
        ├── parser.c.obj
        └── parser.c.pdb   ← per-object PDB (intermediate)
```

Per-object PDBs are intermediate build artifacts — they are used to
produce the final linked PDB and are then redundant but retained in
`target/` for incremental builds.

### PDB in Installed Artifacts

When `output.type` is `shared` (DLL), the PDB is included in the
installed artifact alongside the DLL for consumer debugging:

```
~/.now/repo/org/acme/core/4.2.1/lib/windows-amd64-msvc/
├── acme-core.dll
├── acme-core.lib          ← import library
└── acme-core.pdb          ← debug symbols
```

PDBs are not included in `header-only` or `static` library artifacts
by default (static lib debugging uses the intermediate per-object PDBs).
Override via:

```pasta
output: {
  type:        "static",
  include_pdb: true          ; include merged PDB with static lib
}
```

### PDB Path Configuration

```pasta
; now.pasta — MSVC-specific PDB configuration
{
  msvc: {
    pdb: {
      compile_pdb_dir: "target/${triple}/pdb/obj",   ; per-object PDBs
      link_pdb:        "target/${triple}/bin/${now.artifact}.pdb",
      embed_source:    false,    ; /SOURCELINK — embed source server info
      strip_private:   true      ; /PDBSTRIPPED — public PDB (no private symbols)
    }
  }
}
```

`strip_private: true` produces a second PDB with private symbols removed,
suitable for distribution. The full PDB is kept in `target/`; the stripped
PDB is what gets installed and packaged.

---

## 7.10 DLL Export Management

Windows DLLs require explicit export declarations. GCC/Clang's
`-fvisibility=hidden` convention (default-hidden, explicitly export) has
no direct MSVC equivalent. `now` supports both `__declspec(dllexport)`
and `.def` file approaches, with cross-platform compatibility in mind.

### Approach 1: `__declspec` with Cross-Platform Macro

The recommended approach for cross-platform libraries. The project
defines a visibility macro that expands correctly on each platform:

```pasta
; now.pasta — inject the macro via defines
compile: {
  defines: ["MYLIB_EXPORTS"]    ; when building the DLL itself
}
```

```c
/* src/main/h/mylib/export.h */
#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef MYLIB_EXPORTS
    #define MYLIB_API __declspec(dllexport)
  #else
    #define MYLIB_API __declspec(dllimport)
  #endif
#else
  #define MYLIB_API __attribute__((visibility("default")))
#endif
```

`now` generates this header automatically when `output.type: "shared"` and
`msvc.exports: "macro"` (the default for shared libraries):

```pasta
output: {
  type: "shared",
  name: "mylib"
},
msvc: {
  exports: "macro",                    ; generate export header
  export_macro: "MYLIB_API",          ; macro name
  export_header: "src/main/h/mylib/export.h"  ; output path
}
```

`now` writes the header during the `generate` phase if it does not exist,
and updates it if `now.pasta` changes. The header is then available to all
source files via the normal include path.

### Approach 2: Module Definition File (`.def`)

For libraries with stable ABIs or legacy exports, a `.def` file gives
complete control over the export table:

```
; mylib.def
LIBRARY mylib
EXPORTS
  mylib_init          @1
  mylib_process       @2
  mylib_shutdown      @3
  mylib_get_version   @4 NONAME
```

```pasta
; now.pasta
link: {
  def_file: "mylib.def"    ; passed as /DEF:mylib.def to the linker
}
```

When a `.def` file is declared, `now` does not generate an export header —
the `.def` file is the authoritative export list. The linker generates the
import library (`.lib`) automatically.

### Import Library

When building a DLL, MSVC generates both the DLL and an import library
(`.lib`) that consumers link against. `now` places both in the output:

```
target/windows-amd64-msvc/bin/
├── mylib.dll          ← the DLL
└── mylib.lib          ← import library (linker input for consumers)
```

The installed artifact layout (doc 05b) for a shared library on Windows:

```
~/.now/repo/org/acme/mylib/1.0.0/
├── h/mylib/           ← headers
├── lib/windows-amd64-msvc/
│   ├── mylib.dll      ← runtime DLL
│   ├── mylib.lib      ← import library
│   └── mylib.pdb      ← debug symbols
└── now.pasta
```

Consumer `now.pasta` gets the import library automatically via
`consumer_link` in the installed descriptor — no manual `/LIBPATH` or
`.lib` name needed.

---

## 7.11 Runtime Library Selection (`/MT` vs `/MD`)

MSVC has two C runtime models:

| Flag | Runtime | Meaning |
|------|---------|---------|
| `/MD` | Dynamic (MSVCRT.dll) | Default. Smaller binary, requires DLL at runtime. |
| `/MDd` | Dynamic Debug | Debug build with dynamic CRT. |
| `/MT` | Static | CRT linked into binary. Larger, fully standalone. |
| `/MTd` | Static Debug | Debug build with static CRT. |

The runtime must be consistent across all translation units and all deps.
A mismatch (`/MT` in one object, `/MD` in another) causes linker errors
or subtle runtime failures.

### Declaration

```pasta
; now.pasta
{
  msvc: {
    runtime: "dynamic"    ; dynamic | dynamic-debug | static | static-debug
  }
}
```

Default: `dynamic` (matches most Windows development and CI environments).

### Profiles for Debug/Release

```pasta
profiles: {
  debug: {
    msvc: { runtime: "dynamic-debug" },
    compile: { flags: ["-g"] }
  },
  release: {
    msvc: { runtime: "dynamic" },
    compile: { flags: ["-O2"] }
  }
}
```

### Runtime Consistency Check

`now` validates that all dep artifacts were built with a compatible
runtime. When installing a dep that was built with `/MT` into a project
using `/MD`, `now procure` warns:

```
Warning: org.acme:core:4.2.1 (windows-amd64-msvc) was built with
  runtime: static (/MT)
  Your project uses: dynamic (/MD)
  CRT mismatch may cause heap corruption or link errors.
  Rebuild org.acme:core with runtime: dynamic, or set msvc.runtime: static.
```

---

## 7.12 Response Files

Windows has a maximum command line length of approximately 32,767
characters. Large projects with many source files, include paths, or
long dep chains easily exceed this. `now` uses response files
transparently when the command line would exceed a safe threshold
(8,192 characters — conservative to allow for shell and process overhead).

### How Response Files Work

Instead of:
```
cl.exe /c /std:c11 /W4 /Isrc\h /I...300 more include paths... src\main\c\parser.c /Fo:...
```

`now` writes a response file and passes it:
```
cl.exe @target\windows-amd64-msvc\rsp\parser.c.rsp
```

Contents of `parser.c.rsp`:
```
/c /std:c11 /W4
/Isrc\main\h
/I"C:\Users\alice\.now\repo\org\acme\core\4.2.1\h"
/I"C:\Users\alice\.now\repo\zlib\zlib\1.3.0\h"
/DNDEBUG /DACME_INTERNAL
src\main\c\parser.c
/Fo:target\windows-amd64-msvc\obj\main\src\main\c\parser.c.obj
```

Response files are written to `target/${triple}/rsp/` and are retained
for build inspection and debugging. `now build -vv` shows the response
file path rather than the expanded contents, with `--expand-rsp` to
print the full contents.

### Forced Response Files

```pasta
; now.pasta — always use response files (useful for debugging)
{
  msvc: {
    always_use_rsp: true
  }
}
```

---

## 7.13 MSVC-Specific `now.pasta` Fields

All MSVC-specific configuration lives under the `msvc:` key. It is
ignored by non-MSVC toolchains.

```pasta
{
  msvc: {
    ; Runtime library
    runtime: "dynamic",          ; dynamic | dynamic-debug | static | static-debug

    ; PDB configuration
    pdb: {
      link_pdb:      "target/${triple}/bin/${now.artifact}.pdb",
      strip_private: true,       ; produce stripped public PDB
      embed_source:  false       ; /SOURCELINK
    },

    ; DLL export management
    exports: "macro",            ; macro | def | none
    export_macro:  "MYLIB_API",
    export_header: "src/main/h/mylib/export.h",

    ; Or: .def file
    def_file: "mylib.def",

    ; Response files
    always_use_rsp: false,
    rsp_dir: "target/${triple}/rsp",

    ; Subsystem (executables only)
    subsystem: "console",        ; console | windows | native | efi

    ; Manifest embedding (Windows application manifest)
    manifest: {
      embed:   true,
      file:    "myapp.manifest",
      level:   "asInvoker"       ; asInvoker | highestAvailable | requireAdministrator
    },

    ; Whole-program optimisation (link-time)
    lto: false,                  ; /GL (compile) + /LTCG (link)

    ; Control Flow Guard
    cfg: true,                   ; /guard:cf — enabled by default on Windows targets

    ; Address Sanitizer (MSVC 2019+)
    asan: false                  ; /fsanitize=address
  }
}
```

---

## 7.14 MSVC Invocation Sequences

### Complete Compile Invocation

```
cl.exe
  /c                           ; compile only
  /std:c11                     ; translated from std: "c11"
  /W4 /WX                      ; translated from warnings: ["Wall", "Werror"]
  /Od /Zi                      ; translated from compile.flags: ["-O0", "-g"]
  /DNDEBUG /DACME_INTERNAL     ; translated from defines: [...]
  /Isrc\main\h                 ; translated from compile.includes: [...]
  /I"C:\...\.now\repo\...\h"   ; dep include paths
  /MDd                         ; runtime: dynamic-debug
  /guard:cf                    ; cfg: true (default)
  /Fd"target\...\parser.c.pdb" ; per-object PDB
  /Fo"target\...\parser.c.obj" ; output object
  @target\...\rsp\parser.c.rsp ; (if response file used)
  src\main\c\parser.c
```

### Complete Link Invocation (Executable)

```
link.exe
  /SUBSYSTEM:CONSOLE
  /PDB:"target\windows-amd64-msvc\bin\myapp.pdb"
  /DEBUG
  /MACHINE:X64
  /OUT:"target\windows-amd64-msvc\bin\myapp.exe"
  /LIBPATH:"C:\...\.now\repo\org\acme\core\4.2.1\lib\windows-amd64-msvc"
  acme-core.lib zlib.lib
  target\...\obj\main\src\main\c\parser.c.obj
  target\...\obj\main\src\main\c\writer.c.obj
  @target\...\rsp\link.rsp    ; (if response file used)
```

### Complete Link Invocation (DLL)

```
link.exe
  /DLL
  /DEF:mylib.def               ; if def_file declared
  /PDB:"target\...\mylib.pdb"
  /DEBUG
  /IMPLIB:"target\...\mylib.lib"   ; import library output
  /OUT:"target\...\mylib.dll"
  ...objects and libs...
```

### Static Library

```
lib.exe
  /OUT:"target\...\mylib.lib"
  target\...\obj\main\*.obj
```

---

## 7.15 Visual Studio Integration

`now` does not generate `.vcxproj` or `.sln` files. However, since
`now compile-db` produces `compile_commands.json` (doc 22), and Visual
Studio 2019+ supports `compile_commands.json` via the "Open Folder"
workflow, integration is available without project file generation.

For teams that require `.sln`/`.vcxproj`, a plugin can generate them
from the build graph as a `generate`-phase side effect. This is not a
built-in `now` feature — it is an explicit plugin that reads the build
graph and emits VS project files.

```pasta
plugins: [
  {
    id:    "org.now.plugins:vs-project-gen:1.0.0",
    type:  "plugin",
    phase: "generate",
    when:  "windows:*:msvc",
    config: {
      solution: "myapp.sln",
      vs_version: "2022"
    }
  }
]
```

---

## 7.16 Supersession Note

This document supersedes doc 07 §7.4 (the MSVC translation stub). The
full flag translation table in §7.8 is now authoritative. All other
sections of doc 07 remain valid and apply to MSVC where applicable
(toolchain resolution order, named presets, parallel compilation, verbose
and dry-run modes).



---



---

# Chapter 9 — Testing

The `test` phase compiles and runs the project's test suite. `now` treats
testing as a first-class lifecycle concern, not an afterthought. Tests are
always compiled against project sources (not the linked artifact), so
internal linkage is visible to tests.

---

## 8.1 Test Configuration

```pasta
tests: {
  dir:     "src/test/c",
  headers: "src/test/include",
  pattern: "**_test.c",          ; glob for test source files
  runner:  "unity",              ; test framework adapter
  timeout: 30,                   ; seconds per test binary (default: 30)
  jobs:    4                     ; parallel test execution
}
```

---

## 8.2 Test Build Model

The test binary is linked from:

1. **All production object files** from `target/obj/main/` — the same `.o`
   files produced by the `compile` phase. Not the linked library. This
   gives tests access to `static` functions and internal symbols.
2. **Test object files** from `target/obj/test/`.
3. **Test-scoped deps** (scope: `test` in `now.pasta`).
4. **Compile-scoped deps** (also linked into test binary).
5. **The test runner framework** (resolved from `tests.runner`).

The test binary is written to `target/bin/{artifact}-test`.

This model means test code can reach every symbol in the production code,
including internal helpers not exposed via public headers. This is
intentional — unit testing in C benefits from whitebox access.

---

## 8.3 Test Runner Adapters

`now` ships built-in adapters for common C test frameworks. The adapter
knows how to:

- Add the framework as an implicit test-scope dependency (if not already
  declared).
- Synthesise a `main()` entry point if the framework requires it.
- Parse test output and report pass/fail/skip counts.
- Map test failures to source locations.

### Built-in Adapters

| Runner | Framework | Notes |
|--------|-----------|-------|
| `unity` | ThrowTheSwitch Unity | Single `.c` + `.h`. Auto-procured. |
| `criterion` | Criterion | Uses `criterion:criterion:^2.4`. Auto-procured. |
| `check` | Check (libcheck) | Links `-lcheck`. Must be system-installed or declared. |
| `minunit` | MinUnit | Header-only. Auto-procured. |
| `custom` | User-defined | See 8.5. |
| `none` | No framework | `main()` must be in test sources. `now` runs the binary and checks exit code. |

### Unity Example

```pasta
tests: {
  dir:    "src/test/c",
  runner: "unity"
}
```

`now` auto-adds `{ id: "unity:unity:^2.5", scope: "test" }` to the dep
graph (unless already declared). It generates a `target/generated/test_runner_main.c`
containing:

```c
/* Auto-generated by now — do not edit */
#include "unity.h"

/* Forward declarations of all test functions found by scanning test sources */
void setUp(void);
void tearDown(void);
/* ... per-test setUp/tearDown if test file defines them ... */

int main(void) {
    UNITY_BEGIN();
    /* RUN_TEST calls inserted for each void test_*() function found */
    RUN_TEST(test_parser_empty_input);
    RUN_TEST(test_parser_single_value);
    /* ... */
    return UNITY_END();
}
```

`now` scans test source files for functions matching `void test_*()` and
generates the runner automatically. No registration boilerplate in test files.

### Criterion Example

Criterion uses a self-registering macro system — no `main()` needed. `now`
links against libcriterion and the framework's own entry point:

```pasta
tests: {
  runner: "criterion"
}

deps: [
  { id: "criterion:criterion:^2.4.2", scope: "test" }
]
```

---

## 8.4 Multiple Test Binaries

By default, all test sources are linked into a single binary. For large
projects, you can split into multiple test binaries:

```pasta
tests: [
  {
    name:    "unit",
    dir:     "src/test/unit",
    pattern: "**_test.c",
    runner:  "unity"
  },
  {
    name:    "integration",
    dir:     "src/test/integration",
    pattern: "**_test.c",
    runner:  "none",
    timeout: 120
  }
]
```

Each produces `target/bin/{artifact}-test-{name}`. All are run during the
`test` phase. A failure in any binary fails the build.

Run a specific suite:

```sh
now test --suite unit
now test --suite integration
```

---

## 8.5 Custom Test Runner

When `runner: "custom"`, you provide the runner binary and output format:

```pasta
tests: {
  runner:  "custom",
  command: "target/bin/${now.artifact}-test",   ; how to invoke the binary
  report:  "tap"                                ; output format: tap | junit | now
}
```

Output formats:

| Format | Description |
|--------|-------------|
| `tap`  | TAP (Test Anything Protocol). `now` parses TAP output. |
| `junit` | JUnit XML. `now` reads `target/test-results/*.xml`. |
| `now`  | `now`'s native simple format: `PASS: name`, `FAIL: name`, `SKIP: name` per line. |

Exit code of the test binary is always checked regardless of format.

---

## 8.6 Test Output and Reporting

During `now test`, output is:

```
[test] Building test binary...
[test] Running unity suite (12 tests)
  PASS  test_parser_empty_input       (0.001s)
  PASS  test_parser_single_value      (0.001s)
  PASS  test_parser_nested_map        (0.002s)
  FAIL  test_parser_string_escape     (0.001s)
         src/test/c/parser_test.c:47: Expected 5 but was 3
  PASS  test_writer_compact           (0.001s)
  ...

Results: 11 passed, 1 failed, 0 skipped  (0.018s total)
[test] FAILED
```

On failure, `now` exits with code 2. The full test log is written to
`target/test-results/{suite}.log`.

---

## 8.7 Test-Only Defines and Flags

Test sources are compiled with all production flags plus any test-specific
additions:

```pasta
tests: {
  dir:    "src/test/c",
  runner: "unity",

  compile: {
    defines: ["TESTING", "MOCK_FILESYSTEM"],
    flags:   ["-g", "-O0"]         ; always debug in tests
  }
}
```

Test compile flags are additive on top of the production `compile` block.
They do not affect production object files.

---

## 8.8 Code Coverage

```sh
now test --coverage
```

Activates coverage instrumentation:

1. Adds `-fprofile-arcs -ftest-coverage` (GCC) or
   `-fprofile-instr-generate -fcoverage-mapping` (Clang) to compile flags.
2. Runs the test binary.
3. Invokes `gcov`/`llvm-cov` to produce a coverage report in
   `target/coverage/`.
4. Prints a summary:

```
Coverage summary:
  src/main/c/parser.c       87.3%  (line)
  src/main/c/writer.c       94.1%  (line)
  src/main/c/value.c        91.8%  (line)
  Overall:                  90.4%
```

Coverage reports are not part of the standard lifecycle — they are
opt-in via the flag.



---



---

# Chapter 10 — Plugins, Tools, Code Generation, and Plugin Protocol


`now` is intentionally minimal in its core. Extensibility comes through
plugins — standalone executables or shared libraries that `now` invokes at
defined lifecycle hooks. Plugins are declared in `now.pasta`, procured like
dependencies, and run by `now` in process isolation.

---

## 10.1 Plugin Declaration

```pasta
plugins: [
  {
    id:    "org.now.plugins:protobuf-c:1.0.0",
    phase: "generate",
    config: {
      proto_src: "src/proto",
      out:       "target/generated/proto"
    }
  },
  {
    id:    "org.now.plugins:embed:0.3.0",
    phase: "generate",
    config: {
      src: "assets/",
      out: "target/generated/assets.c",
      sym: "asset_"
    }
  },
  {
    id:    "org.now.plugins:clang-format:1.0.0",
    phase: "pre-compile",
    config: {
      style: "file"    ; use .clang-format in project root
    }
  }
]
```

Plugins are procured from the same repos as regular dependencies. They are
installed to `~/.now/repo/` like any other artifact, but their `output.type`
is `executable` — they are programs, not libraries.

---

## 10.2 Lifecycle Hooks

| Hook | Runs | Typical use |
|------|------|-------------|
| `pre-procure` | Before dependency resolution | Inject synthetic deps |
| `post-procure` | After deps installed | Validate dep graph |
| `generate` | Before compile, after procure | Code generation |
| `pre-compile` | After generate, before compile | Formatting, linting |
| `post-compile` | After compile, before link | Static analysis on objects |
| `pre-link` | Before link | Modify object list |
| `post-link` | After link, before test | Binary patching, signing |
| `pre-test` | Before test | Set up fixtures, mocks |
| `post-test` | After test | Coverage, reports |
| `pre-package` | Before package | Add extra files to archive |
| `post-package` | After package | Notarisation, signing |
| `pre-publish` | Before publish | Final validation |

Multiple plugins may register the same hook. They run in declaration order.

---

## 10.3 Plugin Invocation Protocol

`now` invokes plugins as child processes. Communication uses stdin/stdout
with a Pasta-based protocol.

### Input (written to plugin stdin)

```pasta
{
  hook:       "generate",
  project:    "/home/alice/myproject",
  basedir:    "/home/alice/myproject",
  target:     "/home/alice/myproject/target",
  now_pasta:  { ; ... full parsed now.pasta content ... },
  config:     { ; ... this plugin's config block from now.pasta ... },
  platform:   "linux-x86_64",
  toolchain:  { cc: "/usr/bin/clang", cxx: "/usr/bin/clang++", ... },
  deps:       [ ; ... resolved dep list from lock file ... ]
}
```

### Output (plugin writes to stdout)

```pasta
{
  status: "ok",       ; ok | warn | error

  ; Optional: additional source files to compile
  sources: [
    "target/generated/proto/schema.pb.c"
  ],

  ; Optional: additional include paths to add to compile phase
  includes: [
    "target/generated/proto"
  ],

  ; Optional: additional defines
  defines: ["HAS_PROTO_SCHEMA"],

  ; Optional: human-readable messages
  messages: [
    { level: "info", text: "Generated schema.pb.c from src/proto/schema.proto" }
  ]
}
```

If `status: "error"`, `now` stops the build immediately and prints any
messages from the plugin. If `status: "warn"`, the build continues and
messages are shown.

### Error Handling

If the plugin process exits non-zero, `now` treats it as an error regardless
of the status field. If the plugin output is not valid Pasta, `now` emits a
parse error and fails the build.

---

## 10.4 Built-In Pseudo-Plugins

`now` ships several built-in capabilities that behave like plugins but
require no separate executable:

### `now:embed`

Embeds binary files as C arrays. Files in `embed.src` are compiled into
`target/generated/_now_embed.c` with symbols named `{prefix}{filename}` and
corresponding size variables.

```pasta
plugins: [
  {
    id:    "now:embed",
    phase: "generate",
    config: {
      src:    "assets/",
      prefix: "asset_"
    }
  }
]
```

Generated:
```c
/* target/generated/_now_embed.c */
const unsigned char asset_logo_png[] = { 0x89, 0x50, ... };
const size_t asset_logo_png_size = 1234;
```

### `now:version`

Generates a `target/generated/_now_version.c` with version constants:

```pasta
plugins: [
  { id: "now:version", phase: "generate" }
]
```

Generated:
```c
const char NOW_VERSION[]      = "2.1.0";
const char NOW_GROUP[]        = "org.acme";
const char NOW_ARTIFACT[]     = "myapp";
const int  NOW_VERSION_MAJOR  = 2;
const int  NOW_VERSION_MINOR  = 1;
const int  NOW_VERSION_PATCH  = 0;
```

### `now:header-guard`

Enforces `#pragma once` or consistent header guards across all headers
during `pre-compile`. Configurable: warn or error on violation.

---

## 10.5 Writing a Plugin

A plugin is any executable that reads Pasta from stdin and writes Pasta to
stdout. Language is irrelevant — C, Python, shell, Go — as long as it
speaks the protocol.

Minimal plugin in C using libpasta:

```c
#include "pasta.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    /* Read all of stdin */
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, stdin);
    buf[n] = '\0';

    /* Parse the input */
    PastaResult r;
    PastaValue *input = pasta_parse_cstr(buf, &r);
    if (!input) {
        fprintf(stderr, "Plugin: failed to parse input: %s\n", r.message);
        return 1;
    }

    /* Read config */
    const PastaValue *cfg    = pasta_map_get(input, "config");
    const char       *src    = pasta_get_string(pasta_map_get(cfg, "src"));
    const char       *outdir = pasta_get_string(pasta_map_get(cfg, "out"));

    /* ... do work, generate files ... */

    /* Write output */
    PastaValue *out = pasta_new_map();
    pasta_set(out, "status", pasta_new_string("ok"));

    PastaValue *sources = pasta_new_array();
    pasta_push(sources, pasta_new_string("target/generated/output.c"));
    pasta_set(out, "sources", sources);

    char *text = pasta_write(out, PASTA_COMPACT);
    puts(text);
    free(text);

    pasta_free(input);
    pasta_free(out);
    return 0;
}
```

---

## 10.6 Plugin Security

Plugins are executed as child processes with the same privileges as `now`.
This is intentional — code generation often needs full filesystem access.

Trust model:
- Plugins from the public central registry are signed. `now` verifies the
  signature before first execution and caches the verification result.
- Plugins from private registries are trusted by the user's registry
  configuration.
- Local plugins (path-based, not coordinate-based) are always trusted.
- `now --strict` refuses to run unsigned plugins from any registry.

Plugins do **not** have network access by default. If a plugin needs
network access (e.g. fetching a schema from a remote service), it must
declare `network: true` in its own `now.pasta`, and `now` prompts the user
for confirmation on first run.

---


## 10.A — Tools and Scripts


`now` supports three levels of extensibility, in ascending order of
capability and complexity: **scripted tools**, **external tools**, and
**compiled plugins**. Understanding when to use each, and how they
compose, is central to keeping `now.pasta` maintainable.

---

## 10.8 The Three Levels

| Level | Form | Packaged as | Invocation | Use when |
|-------|------|-------------|------------|----------|
| Scripted tool | Shell/script in the project | Not packaged; lives in the repo | `now tool:run name` | One-off tasks, project-local automation |
| External tool | Any executable, wrapped by a `now.pasta` entry | `executable` artifact in the registry | Invoked by `now` at a lifecycle hook | Reusable across projects, no Pasta-specific API needed |
| Compiled plugin | `now`-aware program speaking the Pasta protocol | `plugin` artifact in the registry | Invoked by `now` at a lifecycle hook with full context | Deep integration: code generation, source manipulation, dep injection |

---

## 10.9 Scripted Tools

Scripted tools are declared in `now.pasta` under `tools:`. They are simple
named commands — wrappers around shell invocations, scripts in the project,
or external executables. They do not hook into the lifecycle automatically;
they are invoked explicitly.

```pasta
tools: {
  fmt: {
    description: "Format all C sources with clang-format",
    run: "clang-format -i ${sources.dir}/**.c"
  },

  lint: {
    description: "Run cppcheck on sources",
    run: "cppcheck --enable=all ${sources.dir}"
  },

  genkeys: {
    description: "Generate test key material",
    run: "scripts/genkeys.sh ${target}/test-keys",
    env: { KEY_BITS: "2048" }
  },

  docker: {
    description: "Build a container image",
    run: "docker build -t ${now.artifact}:${now.version} ."
  }
}
```

```sh
now tool:run fmt
now tool:run lint
now tool:run genkeys
now tool:list          ; show all declared tools with descriptions
```

### Tool Variable Expansion

All `now` properties and phase variables are available in `run`:

| Variable | Value |
|----------|-------|
| `${now.artifact}` | artifact id |
| `${now.version}` | project version |
| `${now.basedir}` | project root |
| `${target}` | `target/` directory |
| `${sources.dir}` | main sources directory |
| `${triple}` | active target triple |
| Any `properties:` key | its declared value |

### Tool Hooks

A tool can be attached to a lifecycle phase as a pre- or post-action:

```pasta
tools: {
  check-format: {
    run:  "clang-format --dry-run --Werror ${sources.dir}/**.c",
    hook: "pre-build"    ; run before the build phase, fail build if non-zero exit
  },
  strip-binary: {
    run:  "strip ${target}/bin/${now.artifact}",
    hook: "post-link"
  }
}
```

Hook values: `pre-procure`, `post-procure`, `pre-generate`, `post-generate`,
`pre-build`, `post-build`, `pre-link`, `post-link`, `pre-test`, `post-test`,
`pre-package`, `post-package`, `pre-install`, `post-install`,
`pre-publish`, `post-publish`.

A non-zero exit from a hooked tool fails the build at that phase.

---

## 10.10 External Tools (Packaged)

An external tool is any executable published as a `now` artifact with
`output.type: "executable"`. It does not speak the Pasta protocol — it is
just a program. `now` invokes it with configured arguments at a lifecycle
hook.

```pasta
plugins: [
  {
    ; clang-format as an external tool — just runs the binary
    id:    "llvm:clang-format:18.0.0",
    type:  "external",           ; no Pasta protocol
    phase: "pre-build",
    run:   "${tool} -i ${sources.dir}/**.c",
    ; ${tool} is the path to the installed executable
  },
  {
    ; A code formatter from the registry
    id:    "org.now.tools:uncrustify:0.78.0",
    type:  "external",
    phase: "pre-build",
    run:   "${tool} -c .uncrustify.cfg -l C --replace ${sources.dir}/**.c"
  }
]
```

External tools are procured like any other dep — downloaded, cached,
installed to `~/.now/repo/`, executable placed in `bin/{triple}/`.
The `${tool}` variable in `run` is the absolute path to the installed
binary for the current target triple.

External tools are the right choice for wrapping existing ecosystem tools
(formatters, linters, code generators with their own CLI) without writing
a `now`-specific adapter.

---

## 10.11 Compiled Plugins (Maven-Style)

A compiled plugin is a `now`-aware program that speaks the Pasta-in /
Pasta-out protocol defined in document 09. It is the equivalent of a
Maven mojo — a unit of build logic with full access to the project model.

```pasta
plugins: [
  {
    id:     "org.now.plugins:protobuf-c:1.0.0",
    type:   "plugin",            ; speaks Pasta protocol
    phase:  "generate",
    config: {
      proto_src: "src/proto",
      out:       "target/generated/proto"
    }
  },
  {
    id:     "org.now.plugins:header-guard:1.0.0",
    type:   "plugin",
    phase:  "pre-build",
    config: { style: "pragma-once", enforce: "error" }
  }
]
```

Plugins receive the full project model on stdin (as Pasta) and return
actions on stdout (as Pasta). They can:
- Add sources to the build.
- Add include paths or defines.
- Inject additional deps into the graph (via the `pre-procure` hook).
- Fail the build with structured diagnostics.
- Emit warnings associated with specific source locations.

External tools cannot do any of these — they are fire-and-forget.

---

## 10.12 Phase Chaining: Multiple Actions per Phase

Multiple tools and plugins may be registered to the same phase. They are
declared as an ordered list and executed sequentially within that phase.
Execution order within a phase follows declaration order in `now.pasta`.

```pasta
plugins: [
  ; generate phase — three actions, run in order
  { id: "org.now.plugins:protobuf-c:1.0.0",  type: "plugin",   phase: "generate" },
  { id: "org.now.plugins:ragel:7.0.0",        type: "plugin",   phase: "generate" },
  { id: "org.now.plugins:embed:0.3.0",        type: "plugin",   phase: "generate" },

  ; pre-build — format check then lint
  { id: "org.now.tools:clang-format:18.0.0",  type: "external", phase: "pre-build",
    run: "${tool} --dry-run --Werror ${sources.dir}/**.c" },
  { id: "org.now.tools:cppcheck:2.13.0",      type: "external", phase: "pre-build",
    run: "${tool} --enable=warning ${sources.dir}" },

  ; post-link — strip then sign
  { id: "org.now.tools:strip:2.41.0",         type: "external", phase: "post-link",
    run: "${tool} ${target}/bin/${now.artifact}" },
  { id: "org.now.plugins:codesign:1.0.0",     type: "plugin",   phase: "post-link" }
]
```

### Chain Failure Semantics

- If any action in a phase chain fails (non-zero exit for external, `status: "error"` for plugin), execution stops at that action and the build fails.
- Subsequent actions in the same phase do not run after a failure.
- Actions in earlier phases that completed successfully are not rolled back.
- With `continue_on_error: true` on an action, `now` records the failure but
  continues the chain and the build, reporting at the end.

```pasta
plugins: [
  { id: "org.now.tools:cppcheck:2.13.0", type: "external", phase: "pre-build",
    run: "${tool} ${sources.dir}", continue_on_error: true }
]
```

### Chain Parallelism

Actions within the same phase that declare `parallel: true` may run
concurrently. `now` runs all `parallel: true` actions in a phase as a
concurrent group, waits for all to complete, then runs the next
non-parallel action.

```pasta
plugins: [
  ; These two run concurrently
  { id: "org.now.plugins:protobuf-c:1.0.0", type: "plugin",   phase: "generate", parallel: true },
  { id: "org.now.plugins:ragel:7.0.0",       type: "plugin",   phase: "generate", parallel: true },

  ; This runs after both above complete
  { id: "org.now.plugins:embed:0.3.0",       type: "plugin",   phase: "generate" }
]
```

---

## 10.13 Built-In Tool Interface (`now tool:*`)

`now` exposes its internal subsystems as tool-callable subcommands,
enabling external scripts to integrate with `now` without writing a plugin:

```sh
; Query the project model
now tool:query sources.dir
now tool:query deps
now tool:query --target linux:amd64:musl compile.flags

; Query resolved dep paths
now tool:dep-path zlib:zlib:1.3.0 h
now tool:dep-path zlib:zlib:1.3.0 lib linux:amd64:gnu

; Emit compile flags for a specific source file (for IDE integration)
now tool:compile-flags src/main/c/parser.c

; Emit the full compiler invocation for a file (dry-run one file)
now tool:compile-cmd src/main/c/parser.c

; List all source files that would be built
now tool:sources
now tool:sources --target linux:arm64:musl

; Check if a dep is satisfied
now tool:dep-check org.acme:core:^4.0.0
```

Output is always valid Pasta (or plain text with `--text`). This makes
`now tool:*` commands composable with shell scripts and IDE integrations
without requiring a full plugin.

---

## 10.14 Plugin and Tool Authoring Guide

### Scripted Tool

The simplest possible extensibility. Add to `now.pasta`:
```pasta
tools: { mytool: { run: "python3 scripts/mytool.py ${target}" } }
```
No packaging, no protocol, no registry.

### External Tool

Write a standard command-line program. Package it with its own `now.pasta`
declaring `output: { type: "executable" }`. Publish it. Reference by
coordinate. `now` installs and invokes it.

### Compiled Plugin

Write a program that reads Pasta from stdin and writes Pasta to stdout
(see document 09, section 9.5 for the C implementation template). Package
with `output: { type: "plugin" }`. The `plugin` type is like `executable`
but `now` verifies the Pasta protocol is spoken at install time by running
a quick handshake: `echo '{ hook: "ping" }' | myplugin` must return
`{ status: "ok" }`.


---


## 10.B — Plugin Manifest, Protocol Versioning, and Concurrency Contracts


All protocol interfaces defined here are version **1.0.0** — a working label
that will be audited and formally stamped during the final specification
distillation pass.

---

## 10.15 The Plugin Manifest

Every plugin artifact ships a `plugin.pasta` alongside its binary. This is
the plugin's gestalt — its identity, protocol contract, capability set, and
concurrency requirements. `now` reads it at procure time, validates it, and
incorporates its constraints into the build graph before any hook is invoked.

```pasta
; ~/.now/repo/org.now.plugins/codesign/1.0.0/plugin.pasta
{
  ; Identity
  id:       "org.now.plugins:codesign:1.0.0",
  name:     "Code Signing Plugin",
  protocol: "1.0.0",

  ; Concurrency constraints (see §10.18)
  concurrency: {
    mode:    "exclusive",
    scope:   "host",
    lock_id: "macos-keychain"
  },

  ; Capabilities this plugin requires from now (see §10.17)
  requires: ["source-inject", "fail-build"],

  ; Capabilities this plugin optionally uses if available
  optional: ["diagnostic-location"],

  ; Input fields this plugin reads from the hook payload
  accepts: [
    "hook", "project", "basedir", "target",
    "config", "platform", "toolchain", "deps"
  ],

  ; Output fields this plugin may emit in its response
  emits: ["status", "messages", "diagnostics"],

  ; Network access declaration (requires user consent at install)
  network: {
    required: true,
    hosts:    ["timestamp.digicert.com", "api.apple.com"],
    reason:   "Timestamp authority and Apple notarisation API"
  },

  ; Filesystem locks this plugin acquires (informational — for deadlock detection)
  fs_locks: [
    { path: "${HOME}/Library/Keychains", mode: "exclusive" }
  ],

  ; Hooks this plugin handles
  hooks: ["post-link", "pre-publish"],

  ; Minimum now version required
  requires_now: ">=1.0.0"
}
```

The manifest is written by the plugin author and shipped in the artifact.
It is not generated by `now` — it is a declaration of intent and contract.

---

## 10.16 Protocol Version Handshake

Before invoking any hook, `now` performs a handshake with the plugin.
The handshake is a single stdin/stdout exchange that establishes protocol
compatibility and negotiates capabilities.

### Handshake Request (`now` → plugin)

```pasta
{
  hook:         "handshake",
  now_version:  "1.0.0",
  now_protocol: "1.0.0",

  ; Capabilities now supports in this build
  capabilities: [
    "source-inject",
    "dep-inject",
    "fail-build",
    "diagnostic-location",
    "parallel-invocation",
    "cancel-signal"
  ]
}
```

### Handshake Response (plugin → `now`)

```pasta
{
  status:       "ok",
  protocol:     "1.0.0",

  ; Capabilities this plugin actually needs (subset of now's offered set)
  requires:     ["source-inject", "fail-build"],

  ; Capabilities this plugin will use if present
  optional:     ["diagnostic-location"],

  ; Concurrency self-declaration (may refine or confirm manifest)
  concurrency: {
    mode:    "exclusive",
    scope:   "host",
    lock_id: "macos-keychain"
  }
}
```

### Handshake Failure Cases

**Plugin requires capability `now` does not support:**
```pasta
{
  status:  "error",
  code:    "CAPABILITY_MISSING",
  message: "Requires 'source-inject' which now 1.0.0 does not support.",
  hint:    "Upgrade now to >= 1.1.0 to use this plugin."
}
```
`now` aborts the build with a clear message. This is a hard failure — a
plugin that declares a required capability and doesn't get it cannot be
trusted to behave correctly.

**Protocol version incompatibility:**
```pasta
{
  status:  "error",
  code:    "PROTOCOL_MISMATCH",
  message: "Plugin speaks protocol 2.0.0; now supports up to 1.0.0.",
  hint:    "Upgrade now or use an older version of this plugin."
}
```

**Plugin responds with unknown protocol version:**
`now` treats any unknown `protocol` field as potentially incompatible and
fails with a diagnostic rather than proceeding blindly.

### Handshake Caching

The handshake result is cached in the compiled build graph
(`target/.now-graph`) keyed by the plugin binary hash. A plugin that has
not changed since the last build skips the handshake and uses the cached
negotiation result. If the plugin binary hash changes (upgrade), the
handshake is re-run.

---

## 10.17 Capability Definitions

Capabilities are named strings declared in the handshake. They describe
what `now` offers to plugins and what plugins need from `now`.

### `now`-Side Capabilities (offered to plugins)

| Capability | Meaning |
|------------|---------|
| `source-inject` | Plugin may add source files to the build via its response |
| `dep-inject` | Plugin may inject additional deps during `pre-procure` |
| `fail-build` | Plugin may hard-fail the build with structured diagnostics |
| `diagnostic-location` | `now` will annotate diagnostics with source file/line/column |
| `parallel-invocation` | `now` may invoke this plugin concurrently for multiple files |
| `cancel-signal` | `now` will send a cancel signal on timeout or user interrupt |
| `stdin-streaming` | Plugin may receive streaming input rather than a single payload |
| `graph-query` | Plugin may query the build graph via a back-channel |

### Plugin-Side Capabilities (declared by plugins, informational to `now`)

| Capability | Meaning |
|------------|---------|
| `idempotent` | Plugin invocation is idempotent — safe to retry on failure |
| `deterministic` | Same inputs always produce same outputs — safe for caching |
| `stateless` | Plugin holds no state between invocations — safe to parallelise |
| `fast` | Plugin completes in < 100ms — safe to run synchronously in graph |

Plugin-side capabilities are informational — `now` may use them to
optimise scheduling but does not enforce them. A plugin that claims
`stateless` but holds state will misbehave; that is the plugin author's
responsibility.

---

## 10.18 Concurrency Constraints

Plugins declare their concurrency requirements so `now` can schedule them
correctly in the build graph. The constraint model has three components:
**mode**, **scope**, and **lock_id**.

### Mode

| Mode | Meaning |
|------|---------|
| `parallel` | Default. Can run concurrently with any other `parallel` plugin. |
| `serial` | Must not run concurrently with other `serial` plugins in the same scope. May run concurrently with `parallel` plugins. |
| `exclusive` | Must not run concurrently with anything — `parallel` or `serial` — in the same scope. Complete mutual exclusion. |

### Scope

| Scope | Mutual exclusion boundary |
|-------|--------------------------|
| `phase` | Within the same lifecycle phase only. Other phases are unaffected. |
| `project` | Across all phases of this project build. |
| `host` | Machine-wide. All `now` processes on this host respect the constraint. |
| `global` | All `now` processes on this host, plus any remote build workers. |

`host` and `global` use a filesystem-based lock at a well-known path
(`~/.now/locks/{lock_id}.lock`) using advisory locking (`flock` on POSIX,
`LockFileEx` on Windows). This allows coordination across independent
`now` processes without a central daemon.

### `lock_id`

The `lock_id` is a shared namespace for resource contention. Two plugins
from different vendors that contend on the same resource declare the same
`lock_id`. `now` serialises all plugins with the same `lock_id` and scope
against each other, regardless of which project or phase they belong to.

```pasta
; Plugin A — Apple code signing
concurrency: { mode: "exclusive", scope: "host", lock_id: "macos-keychain" }

; Plugin B — Certificate pinning tool (different vendor, same resource)
concurrency: { mode: "exclusive", scope: "host", lock_id: "macos-keychain" }
```

Even in a workspace building multiple modules simultaneously, plugins A
and B will never run at the same time on the same machine.

### Well-Known `lock_id` Values

`now` publishes a registry of well-known lock IDs to prevent accidental
collisions between plugins from different ecosystems:

| `lock_id` | Resource |
|-----------|---------|
| `macos-keychain` | macOS Keychain exclusive access |
| `windows-certstore` | Windows Certificate Store |
| `hsm-pkcs11` | PKCS#11 hardware security module |
| `amiga-flasher` | Amiga hardware flash programmer |
| `jtag-openocd` | JTAG/OpenOCD hardware debugger interface |
| `loopback-port-{n}` | A specific localhost port number |
| `display-{n}` | A specific display or framebuffer device |

Projects may define custom lock IDs with a namespaced prefix:
`org.acme:my-resource`.

### Concurrency in the Build Graph

When `now` constructs the build graph, plugin step nodes are annotated
with their concurrency constraints. The parallel execution engine (doc 16)
respects these annotations:

```
Parallel set 3 (post-link hooks):
  [step: strip-binary]        mode: parallel  → schedule freely
  [step: codesign]            mode: exclusive, scope: host, lock: macos-keychain
  [step: notarise]            mode: exclusive, scope: host, lock: macos-keychain

  Resolution:
    strip-binary runs freely.
    codesign and notarise share lock_id → serialised.
    strip-binary may run concurrently with codesign or notarise.
    codesign and notarise run sequentially in declaration order.
```

`now graph:show` annotates plugin steps with their concurrency mode:

```
post-link:
  [parallel]   step:strip-binary
  [exclusive/host:macos-keychain] step:codesign
  [exclusive/host:macos-keychain] step:notarise  (serialised after codesign)
```

---

## 10.19 Timeout and Cancellation

`now` enforces timeouts on plugin invocations. If a plugin exceeds its
timeout, `now` sends a cancel signal (if the `cancel-signal` capability
was negotiated) and then terminates the process.

```pasta
; now.pasta — per-plugin timeout override
plugins: [
  {
    id:      "org.now.plugins:codesign:1.0.0",
    type:    "plugin",
    phase:   "post-link",
    timeout: "120s"    ; override default (30s) for slow notarisation
  }
]
```

Default timeout: `30s`. Configurable globally in `~/.now/config.pasta`:

```pasta
{ plugins: { default_timeout: "30s" } }
```

### Cancel Signal Protocol

When `now` needs to cancel a plugin (timeout or user Ctrl+C), it sends
a cancel notification on stdin before terminating the process:

```pasta
{ hook: "cancel", reason: "timeout" }
```

The plugin has 5 seconds to respond and clean up. If it does not exit
within 5 seconds, `now` sends SIGTERM, then SIGKILL after another second.
Plugins that hold filesystem locks or network connections should handle
`cancel` by releasing them before exiting.

---

## 10.20 Plugin Verification at Procure Time

When `now procure` installs a plugin, it validates the manifest before
adding the plugin to the build:

1. **Schema validation** — `plugin.pasta` conforms to the manifest schema.
2. **Protocol compatibility** — `protocol` field is parseable and within
   supported range.
3. **Capability existence** — all `requires` capabilities are known to
   this version of `now`. Unknown required capabilities fail with a
   clear upgrade message.
4. **Network consent** — if `network.required: true`, the user is
   prompted to review and approve the declared hosts. Stored in
   `~/.now/trust.pasta` under `plugin_network_consent`.
5. **Signature verification** — if `require_signatures: true` in the
   project trust config, the plugin binary and `plugin.pasta` are
   verified against the publisher's key.
6. **Handshake dry-run** — `now` runs the handshake exchange once to
   confirm the binary speaks the declared protocol. A plugin whose binary
   does not respond correctly to `{ hook: "handshake" }` fails installation.

---

## 10.21 `plugin.pasta` Minimal Form

A simple plugin with no special requirements needs only a handful of fields:

```pasta
; Minimal plugin.pasta for a stateless code generator
{
  id:       "org.acme.plugins:my-generator:1.0.0",
  protocol: "1.0.0",
  hooks:    ["generate"],
  emits:    ["status", "sources", "messages"]
}
```

Defaults for omitted fields:

| Field | Default |
|-------|---------|
| `concurrency.mode` | `parallel` |
| `concurrency.scope` | `phase` |
| `requires` | `[]` |
| `optional` | `[]` |
| `network.required` | `false` |
| `requires_now` | `>=1.0.0` |
| `timeout` (in `now.pasta`) | `30s` |

---

## 10.22 Resolving GAP 8

| Item | Resolution |
|------|-----------|
| Protocol versioning | `protocol: "1.0.0"` field in `plugin.pasta`; handshake exchange before first hook |
| Capability negotiation | `requires` / `optional` in manifest and handshake; `now` offers its capabilities; plugin declares what it needs |
| Serial plugins | `concurrency.mode: "serial"` — serialised with other serial plugins in scope |
| Exclusive plugins | `concurrency.mode: "exclusive"` — complete mutual exclusion in scope |
| Cross-vendor resource contention | `lock_id` shared namespace; well-known IDs registered centrally |
| Host-wide locks | `scope: "host"` with `flock`-based filesystem advisory locks |
| Build graph integration | Concurrency constraints annotate plugin step nodes; parallel executor respects them |
| Timeout and cancellation | Per-plugin `timeout` field; `{ hook: "cancel" }` signal; graceful teardown window |
| Verification at procure | Schema, protocol, capability, network consent, signature, handshake dry-run |



---



---

# Chapter 11 — Multi-Architecture and Platform Triples

`now` treats multi-architecture and multi-platform builds as a first-class
concern, not a bolt-on. A single invocation can fan out across a matrix of
OS, architecture, and variant combinations. This document defines the
platform triple system, fan-out syntax, and how artifacts for multiple
targets are produced, named, and published.

---

## 11.1 The Platform Triple

Every `now` build targets a single **platform triple**:

```
os:arch:variant
```

| Component | Meaning | Examples |
|-----------|---------|---------|
| `os` | Operating system / kernel | `linux`, `macos`, `windows`, `freebsd`, `openbsd`, `freestanding` |
| `arch` | CPU architecture | `amd64`, `arm64`, `arm32`, `riscv64`, `riscv32`, `x86`, `mips64`, `wasm32` |
| `variant` | ABI / runtime / libc variant | `gnu`, `musl`, `msvc`, `mingw`, `uclibc`, `none` |

The full triple uniquely identifies a build target:

```
linux:amd64:gnu      Standard Linux x86-64 (glibc)
linux:amd64:musl     Linux x86-64 with musl libc
linux:arm64:gnu      Linux AArch64 (glibc)
linux:arm64:musl     Linux AArch64 with musl libc
linux:riscv64:gnu    Linux RISC-V 64-bit
macos:arm64:none     macOS Apple Silicon (uses system libc)
macos:amd64:none     macOS Intel
windows:amd64:msvc   Windows x86-64, MSVC ABI
windows:amd64:mingw  Windows x86-64, MinGW/GCC ABI
freestanding:arm32:none  Bare metal ARM Cortex-M
wasm32:wasm:none     WebAssembly
```

`variant: none` means the OS provides the runtime — no libc selection.
`os: freestanding` means no OS at all; `link.libs` must not include libc.

---

## 11.2 Host vs Target

`now` distinguishes two triples at all times:

- **Host triple**: the machine `now` is running on. Used for plugins,
  test execution, and tool selection.
- **Target triple**: the machine the output artifact will run on.

When host == target, it is a **native build**. When host != target, it is
a **cross build**. `now` detects this automatically and selects a
cross-compiler toolchain if configured.

The host triple is detected at startup. The target triple defaults to the
host but can be overridden:

```sh
now compile --target linux:arm64:musl
```

Or in `now.pasta`:

```pasta
{ target: "linux:arm64:musl" }
```

---

## 11.3 Specifying the Target on the CLI

```sh
; Native build (target = host, detected automatically)
now build

; Single explicit target
now build --target linux:amd64:musl

; Shorthand: omit leading components (filled left-to-right from host)
now build --target :amd64:musl    ; same os as host, amd64, musl
now build --target ::musl         ; same os and arch as host, musl variant
```

---

## 11.4 Fan-Out with Wildcards

`*` in any triple position means "all known values for this position". `now`
expands the wildcard into a set of concrete triples and builds each in
sequence (or in parallel with `--parallel`).

```sh
; All Linux architectures, musl variant
now build --target "linux:*:musl"

; All Linux architectures, all variants
now build --target "linux:*:*"

; All targets for all OSes (full matrix)
now build --target "*:*:*"

; All architectures on the host OS
now build --target ":*:"

; All musl targets across all OSes
now build --target "*:*:musl"
```

### What "all known values" means

The universe of values for each component is not unbounded. `now` knows a
closed set per project, derived from:

1. **Explicit matrix declaration** in `now.pasta` (authoritative).
2. **Toolchain availability** — if no cross-compiler for `riscv64` is
   installed, `riscv64` is not in the expansion.
3. **Dep compatibility** — if a dep has no artifact for a given triple,
   that triple is skipped with a warning.

```pasta
; Declare the target matrix this project supports
targets: [
  "linux:amd64:gnu",
  "linux:amd64:musl",
  "linux:arm64:gnu",
  "linux:arm64:musl",
  "linux:riscv64:gnu",
  "macos:arm64:none",
  "macos:amd64:none",
  "windows:amd64:msvc"
]
```

With this declared, `now build --target "linux:*:musl"` expands to exactly:
`linux:amd64:musl`, `linux:arm64:musl`.

Without a `targets` declaration, `now` expands wildcards only against
installed cross-toolchains and available dep variants.

---

## 11.5 Named Build Targets (Subcommand Syntax)

Alongside the platform triple, projects may declare **named build targets**
— logical build configurations. The `now build :name` syntax selects one.

```pasta
builds: {
  scheduler: {
    sources: { dir: "src/scheduler" },
    output:  { type: "executable", name: "scheduler" },
    compile: { defines: ["COMPONENT_SCHEDULER"] }
  },
  agent: {
    sources: { dir: "src/agent" },
    output:  { type: "executable", name: "agent" }
  },
  libcore: {
    sources: { dir: "src/core" },
    output:  { type: "static", name: "core" }
  }
}
```

```sh
; Build the scheduler component for linux:amd64:musl
now build :scheduler --target linux:amd64:musl

; Build all named targets for linux:amd64:musl
now build :* --target linux:amd64:musl

; Build the scheduler for all linux musl targets
now build :scheduler --target "linux:*:musl"

; Full fan-out: all named targets × all declared platform triples
now build :* --target "*:*:*"
```

The `:name` syntax (leading colon) unambiguously distinguishes named build
targets from platform triples.

---

## 11.6 Fan-Out Execution Model

When a wildcard or multi-target build is requested, `now` materialises the
full expansion first, then executes:

```
1. Expand --target wildcards → list of concrete triples T₁, T₂, …, Tₙ
2. Expand :name wildcards → list of named targets B₁, B₂, …, Bₘ
3. Form cartesian product: (Bᵢ, Tⱼ) for all i, j
4. For each (Bᵢ, Tⱼ):
     - Check toolchain availability for Tⱼ
     - Check dep availability for Tⱼ
     - If either missing: record as SKIP with reason
5. Execute remaining pairs:
     - Sequential: one at a time (default)
     - Parallel:   --parallel N, up to N concurrent builds
6. Report matrix:

  Build matrix results:

  Target               | scheduler | agent  | libcore
  ---------------------|-----------|--------|--------
  linux:amd64:gnu      | OK        | OK     | OK
  linux:amd64:musl     | OK        | OK     | OK
  linux:arm64:gnu      | OK        | SKIP*  | OK
  linux:arm64:musl     | OK        | SKIP*  | OK
  macos:arm64:none     | OK        | OK     | OK
  windows:amd64:msvc   | FAIL      | OK     | OK

  * agent uses POSIX-specific APIs not available on arm64 variant (see dep warning)
  FAIL: scheduler link error on windows:amd64:msvc — see target/windows-amd64-msvc/scheduler/build.log
```

---

## 11.7 Per-Target Output Directories

When building multiple targets, output is namespaced by triple to avoid
collision:

```
target/
├── linux-amd64-gnu/
│   ├── obj/
│   ├── bin/
│   └── pkg/
├── linux-amd64-musl/
│   ├── obj/
│   ├── bin/
│   └── pkg/
├── linux-arm64-gnu/
│   └── ...
└── macos-arm64-none/
    └── ...
```

The dashes in directory names replace colons (colons are not valid in
filesystem paths on all platforms).

For native single-target builds, `target/` is used directly without a
triple subdirectory (backward-compatible with the default layout).

---

## 11.8 Toolchain Selection per Triple

`now` maps each target triple to a toolchain. Resolution:

1. Explicit `toolchains` map in `now.pasta`.
2. Explicit `toolchains` map in `~/.now/config.pasta`.
3. Well-known triple → toolchain prefix conventions.
4. Fail with diagnostic if no toolchain found.

```pasta
; now.pasta — per-triple toolchain overrides
toolchains: {
  "linux:amd64:gnu":    { cc: "gcc",                   cxx: "g++"               },
  "linux:amd64:musl":   { cc: "musl-gcc",              cxx: "musl-g++"          },
  "linux:arm64:gnu":    { cc: "aarch64-linux-gnu-gcc",  cxx: "aarch64-linux-gnu-g++" },
  "linux:arm64:musl":   { cc: "aarch64-linux-musl-gcc", cxx: "aarch64-linux-musl-g++" },
  "macos:arm64:none":   { cc: "clang",                  cxx: "clang++"           },
  "windows:amd64:msvc": { cc: "cl.exe",                 cxx: "cl.exe"            }
}
```

Wildcards are allowed on the left side:

```pasta
toolchains: {
  "linux:*:musl":  { cc: "${arch}-linux-musl-gcc", cxx: "${arch}-linux-musl-g++" },
  "linux:*:gnu":   { cc: "${arch}-linux-gnu-gcc",  cxx: "${arch}-linux-gnu-g++"  }
}
```

`${arch}` interpolates to the architecture component of the matched triple.
Available interpolation variables in toolchain values: `${os}`, `${arch}`,
`${variant}`.

---

## 11.9 Per-Triple Compile Flags and Defines

Some flags only make sense for certain targets. Declare them under
`target_flags`:

```pasta
target_flags: {
  "linux:*:musl": {
    compile: {
      defines: ["_MUSL", "STATIC_LIBC"],
      flags:   ["-static"]
    },
    link: {
      flags: ["-static"]
    }
  },
  "freestanding:*:*": {
    compile: {
      flags:   ["-ffreestanding", "-fno-builtin", "-nostdlib"],
      defines: ["FREESTANDING"]
    },
    link: {
      flags: ["-nostdlib", "-nostartfiles"]
    }
  },
  "windows:*:msvc": {
    compile: {
      defines: ["_WIN32", "WIN32_LEAN_AND_MEAN"]
    }
  }
}
```

`target_flags` entries are merged with the base `compile` and `link` blocks
using the same rules as profiles (scalars replace, arrays append).
Multiple entries may match a triple; they are applied in declaration order.

---

## 11.10 Dependency Resolution Across Triples

When `now procure` runs for a specific target triple, it fetches the
variant of each dep matching that triple. The resolution:

1. Look for `{artifact}-{version}-{os}-{arch}-{variant}.tar.gz` in the repo.
2. If not found, look for `{artifact}-{version}-{os}-{arch}.tar.gz`
   (arch-specific, no variant).
3. If not found, look for `{artifact}-{version}-noarch.tar.gz`
   (header-only or pure-source artifact).
4. If not found, fail with:
   ```
   Error: dep org.acme:core:4.2.1 has no artifact for linux:arm64:musl
   Available: linux:amd64:gnu, linux:amd64:musl, macos:arm64:none
   ```

Source-only deps (header-only artifacts) are always compatible with any
triple.

---

## 11.11 Fat Archives

A single archive may contain binaries for multiple triples (fat archive).
Fat archives are identified by the platform string `fat` in the filename:

```
rocketlib-3.0.0-fat.tar.gz
```

Contents:
```
rocketlib-3.0.0/
├── now.pasta
├── include/
├── lib/
│   ├── linux-amd64-gnu/    libmyrocket.a
│   ├── linux-amd64-musl/   libmyrocket.a
│   ├── linux-arm64-gnu/    libmyrocket.a
│   └── macos-arm64-none/   libmyrocket.a
```

`now procure` accepts fat archives and extracts only the relevant platform
directory. On macOS, universal binaries (combining `amd64` and `arm64`) are
produced when `macos:*:none` is in the target matrix — `lipo` is invoked
post-link to produce a single `libfoo.a` or executable containing both
architectures.

---

## 11.12 The `now.pasta` `target` and `targets` Fields

```pasta
{
  ; Single default target (used when --target not specified)
  ; If omitted, defaults to host triple
  target: "linux:amd64:musl",

  ; Declared target matrix (governs wildcard expansion)
  targets: [
    "linux:amd64:gnu",
    "linux:amd64:musl",
    "linux:arm64:gnu",
    "linux:arm64:musl",
    "macos:arm64:none",
    "windows:amd64:msvc"
  ],

  ; Per-triple toolchain selection
  toolchains: { ... },

  ; Per-triple flag overlays
  target_flags: { ... }
}
```

If `target` is set but `targets` is not, `now build --target "*"` expands
only to the single declared `target`. If neither is set, the host triple is
used for all builds and wildcard expansion only covers it.

---

## 11.13 Cross-Compilation Prerequisites

`now` does not install cross-toolchains. It discovers what is available.
For each target triple in the matrix, `now` checks at startup whether a
suitable compiler exists. Missing toolchains cause SKIPs, not FAILs, in
fan-out builds.

To check what triples are buildable from the current host:

```sh
now toolchain:list
```

```
Available toolchains:
  linux:amd64:gnu      ✓  gcc 13.2  /usr/bin/gcc
  linux:amd64:musl     ✓  musl-gcc 13.2  /usr/local/musl/bin/musl-gcc
  linux:arm64:gnu      ✓  aarch64-linux-gnu-gcc 12.3  /usr/bin/aarch64-linux-gnu-gcc
  linux:arm64:musl     ✗  (no compiler found for aarch64-linux-musl)
  macos:arm64:none     ✗  (host is linux — cannot build for macos without SDK)
  windows:amd64:msvc   ✗  (no cl.exe found; try: now toolchain:install mingw-w64)
  windows:amd64:mingw  ✓  x86_64-w64-mingw32-gcc 12.2  /usr/bin/x86_64-w64-mingw32-gcc
```



---



---

# Chapter 12 — Offline Mode and Cache

`now` is designed to work correctly without network access, provided the
cache has been populated. This document defines offline behaviour, cache
structure, cache warming strategies, and the interaction between offline
mode and the lock file.

---

## 13.1 The `--offline` Flag

```sh
now procure --offline       ; use only cache and local repo, fail if anything missing
now build --offline         ; implies --offline for the procure sub-phase
now test --offline          ; all phases use offline mode
```

When `--offline` is active:
- No HTTP requests are made. Ever. Not even for version resolution.
- `now.lock.pasta` must exist and be complete. If it is absent or
  inconsistent, `now` fails immediately with a clear message.
- For each dep in the lock, `now` checks `~/.now/repo/` first,
  then `~/.now/cache/`. If found in cache but not repo, it installs from
  cache. If missing from both, it fails with a diagnostic.
- The lock file is never modified in offline mode.

```
Offline mode: dep org.acme:core:4.2.1 not in cache
  Expected: ~/.now/cache/org/acme/core/4.2.1/core-4.2.1-linux-amd64-gnu.tar.gz
  Run 'now procure' with network access to populate the cache.
```

---

## 13.2 Online Modes

`now` has three distinct online/offline modes:

| Mode | Flag | Behaviour |
|------|------|-----------|
| `online` | *(default)* | Use lock if present and consistent; fetch missing deps |
| `refresh` | `--refresh` | Ignore lock; re-resolve and re-fetch everything |
| `offline` | `--offline` | Never touch network; fail if cache incomplete |

A fourth mode exists for CI environments:

| Mode | Flag | Behaviour |
|------|------|-----------|
| `locked` | `--locked` | Use lock file strictly; fail if `now.pasta` and lock are inconsistent rather than re-resolving |

`--locked` is recommended for CI: it enforces that the committed lock file
is up to date and that no developer forgot to run `now procure` after
editing `now.pasta`.

---

## 13.3 Cache Structure

```
~/.now/cache/
└── {group-path}/{artifact}/{version}/
    ├── {artifact}-{version}-{triple}.tar.gz      Downloaded archive
    ├── {artifact}-{version}-{triple}.sha256      Expected hash (from registry)
    └── {artifact}-{version}-noarch.tar.gz        Header-only variant
```

The cache holds raw downloaded archives. It is never modified by the build
— only by `procure` (populating) and `vacate --purge` (clearing). The
installed repo (`~/.now/repo/`) is the extracted, ready-to-use form.

Cache entries are immutable once written. If a download is interrupted,
the partial file is written to a `.tmp` path and only moved to the final
cache path after sha256 verification. A partial or corrupt cache entry
cannot exist at the final path.

---

## 13.4 Cache Warming

Cache warming is the process of pre-populating the cache before going
offline. Several strategies:

### Manual warm: single project

```sh
; Download everything needed for this project into cache
now procure --warm

; Warm for all declared target triples
now procure --warm --target "*:*:*"
```

`--warm` fetches all archives into `~/.now/cache/` without installing to
`~/.now/repo/`. The project is not built. This is the pre-flight step
before air-gapped or intermittently connected builds.

### Export cache for transfer

```sh
; Bundle the cache for this project into a transferable archive
now cache:export --output now-cache-bundle.tar.gz

; Import on the target machine
now cache:import now-cache-bundle.tar.gz
```

The bundle contains all cache entries referenced by `now.lock.pasta`.
On import, entries are verified by sha256 and placed into `~/.now/cache/`.

### Mirror a registry

```sh
; Download all versions of a dep and its transitive closure
now cache:mirror org.acme:core --all-versions

; Mirror all deps for this project's entire declared target matrix
now cache:mirror --project --target "*:*:*"
```

Mirroring is useful for setting up an internal registry proxy or for
creating fully self-contained build environments.

---

## 13.5 Cache Validity

A cache entry is valid if:
1. The file exists at the expected path.
2. Its sha256 matches the value recorded in `now.lock.pasta`.

`now` does not check modification times or expiry — content hash is the
only validity criterion. This means the cache never silently degrades:
a valid cache entry is valid forever.

To verify the entire cache for a project:

```sh
now cache:verify
```

Output:
```
Cache verification:
  org.acme:core:4.2.1         OK      (sha256 match)
  zlib:zlib:1.3.0             OK      (sha256 match)
  unity:unity:2.5.2           MISSING (~/.now/cache/unity/unity/2.5.2/...)
  org.acme:util:3.1.0         CORRUPT (sha256 mismatch — expected a3f8..., got b2c9...)

2 OK, 1 missing, 1 corrupt
Run 'now procure' to repair.
```

---

## 13.6 Snapshot Cache Behaviour

Snapshots (`-SNAPSHOT` versions) bypass normal cache validity:

- Always re-fetched when online (no sha256 pinning, no lock pinning).
- In offline mode, the cached snapshot is used if present, with a warning:
  ```
  Warning: using cached snapshot org.acme:core:1.0.0-SNAPSHOT
  Snapshot may be outdated. Run 'now procure' with network to refresh.
  ```
- Snapshot cache entries are stored with a timestamp and optionally
  expire based on `snapshot_ttl` in `~/.now/config.pasta`.

---

## 13.7 Offline and the Build Graph

The build graph (document 15) is also cacheable. A compiled build graph
for a given `now.pasta` + `now.lock.pasta` + source tree state can be
stored in `target/.now-graph` and reused without recomputation.

In offline mode, if `target/.now-graph` exists and is valid (verified by
hashing the inputs that determine it), `now` uses it directly — no dep
resolution or graph construction occurs. This is the fastest possible start
to a build in an airgapped environment.

---

## 13.8 `~/.now/config.pasta` Offline Settings

```pasta
{
  ; Automatically fall back to offline if network is unavailable
  ; rather than failing immediately
  offline_fallback: true,

  ; Timeout before declaring network unavailable (seconds)
  network_timeout: 10,

  ; Snapshot TTL in offline_fallback mode (seconds, 0 = always warn)
  snapshot_ttl: 3600,

  ; Proxy
  http_proxy: "http://proxy.internal:8080",
  no_proxy:   ["localhost", "*.internal"]
}
```

With `offline_fallback: true`, `now` attempts each network request with
a `network_timeout` deadline. If the request times out or fails with a
connection error, it falls back to the cache silently. If the cache has
the artifact, the build continues. If not, it fails with a clear message
indicating both the network failure and the cache miss.



---



---

# Chapter 13 — Build Graph, Parallel Execution, and Module Pre-Scan

The build graph is the directed acyclic graph (DAG) of all build steps and
their dependencies. `now` constructs the graph before any compilation
begins, uses it to drive parallel execution and incremental builds, and
can compile it to a binary form that is cached and reused. This document
defines the graph model, construction algorithm, serialisation format,
and caching strategy.

---

## 14.1 Graph Nodes

Every entity in the build is a node. Nodes have types:

| Node Type | Represents | Inputs | Outputs |
|-----------|------------|--------|---------|
| `source` | A source file on disk | *(filesystem)* | `file` nodes |
| `file` | A file path with a content hash | *(filesystem)* | consumed by `step` nodes |
| `step` | One tool invocation | `file` nodes | `file` nodes |
| `group` | A named collection of `step` outputs | `file` nodes | `file` nodes (aggregated) |
| `link` | The link/archive step | object `file` nodes | binary `file` node |
| `phase` | A lifecycle phase boundary | `step`/`group` nodes | signals to next phase |

---

## 14.2 Graph Construction

Graph construction happens after `procure` and `generate`, before `build`.
It is a pure function of:

1. The resolved source file set (from source scanning).
2. The language type system (document 04c) — maps each file to a tool invocation.
3. The dep graph — contributes `file` nodes for installed headers and libs.
4. The phase chain (document 14) — contributes `step` nodes for tools and plugins.
5. The active profiles and target triple.

### Construction Algorithm

```
1. Scan source roots → collect all source files S₁, S₂, …, Sₙ
2. For each Sᵢ:
     classify Sᵢ → language type T
     create step node: step(T.tool, T.args, input=Sᵢ, output=obj(Sᵢ))
     if T.produces == "intermediate":
       create file node for intermediate output
       classify intermediate → new type T'
       create another step node: step(T'.tool, T'.args, input=intermediate)
       chain: Sᵢ → step₁ → intermediate → step₂ → obj
     else:
       Sᵢ → step → obj(Sᵢ)
3. Add dep header file nodes → edges into steps that transitively include them
4. Collect all obj nodes → create link step node
5. Add phase hook steps at phase boundaries
6. Topological sort → validate DAG (detect cycles, report error)
7. Assign each node a content hash (see 15.4)
```

### Example Graph Fragment

```
src/main/c/parser.c ──→ [cc step] ──→ target/obj/main/parser.c.o ──┐
src/main/c/writer.c ──→ [cc step] ──→ target/obj/main/writer.c.o ──┤
src/main/c/value.c  ──→ [cc step] ──→ target/obj/main/value.c.o  ──┤
src/main/asm/boot.s ──→ [as step] ──→ target/obj/main/boot.s.o   ──┤
                                                                      ├→ [link step] → target/bin/myapp
dep: zlib headers ──→ (included by parser.c step)                    │
dep: zlib lib ─────────────────────────────────────────────────────→ ┘
```

---

## 14.3 The Compiled Graph

After construction, `now` serialises the graph to `target/.now-graph`
as a binary Pasta document. This is the **compiled graph** — a complete,
pre-computed build plan that `now` can load and execute without
re-running the construction algorithm.

### What the compiled graph contains

```pasta
; target/.now-graph (binary Pasta, not human-edited)
{
  version:    "1",
  created:    "2026-03-05T14:22:00Z",

  ; Input fingerprint — if any of these change, graph is stale
  fingerprint: {
    now_pasta:       "sha256:a3f8...",
    now_lock:        "sha256:b91d...",
    source_tree:     "sha256:c44e...",   ; hash of all source file paths+mtimes
    langs:           "sha256:d55f...",   ; hash of resolved language definitions
    triple:          "linux:amd64:gnu",
    profile:         "release"
  },

  nodes: [
    {
      id:      "step:cc:src/main/c/parser.c",
      type:    "step",
      tool:    "/usr/bin/gcc",
      tool_hash: "sha256:e66a...",       ; hash of the tool binary itself
      args:    ["-std=c11", "-Wall", "-O2", "-c",
                "/home/alice/myapp/src/main/c/parser.c",
                "-o", "/home/alice/myapp/target/obj/main/parser.c.o",
                "-MMD", "-MF", ".../parser.c.d"],
      inputs:  ["file:src/main/c/parser.c", "file:dep:zlib:h/zlib.h", ...],
      outputs: ["file:target/obj/main/parser.c.o"]
    },
    ; ... more nodes
    {
      id:      "step:link",
      type:    "link",
      tool:    "/usr/bin/gcc",
      args:    ["-O2", "target/obj/main/parser.c.o", "...", "-lzlib", "-o", "target/bin/myapp"],
      inputs:  ["file:target/obj/main/parser.c.o", "..."],
      outputs: ["file:target/bin/myapp"]
    }
  ],

  ; Topological order for sequential execution
  topo_order: ["step:cc:parser.c", "step:cc:writer.c", "...", "step:link"],

  ; Independent sets for parallel execution
  parallel_sets: [
    ["step:cc:parser.c", "step:cc:writer.c", "step:cc:value.c", "step:as:boot.s"],
    ["step:link"]
  ]
}
```

---

## 14.4 Graph Node Content Hashing

Each node in the graph has a **content hash** — a hash of everything that
determines whether its output is still valid:

```
node_hash = sha256(
  tool_path,
  tool_binary_hash,
  sorted(input_file_hashes),
  canonical_args_string,
  target_triple,
  active_profiles
)
```

The **input file hash** of a source file is:

```
file_hash = sha256(
  file_content,
  sorted(transitive_header_hashes)    ; for C/C++ — from .d file
)
```

This means a node's hash changes if:
- The source file changes.
- Any header it includes changes (transitively).
- The tool binary changes (compiler update).
- The argument set changes (flag, define, include path added or removed).
- The target triple changes.

The hash is computed lazily — only for nodes that are candidates for
execution in the current incremental build.

---

## 14.5 Graph Staleness and Reuse

Before executing a build, `now` checks whether the compiled graph is still
valid by comparing the fingerprint in `target/.now-graph` against current
state:

```
graph is valid if:
  fingerprint.now_pasta    == sha256(current now.pasta)
  fingerprint.now_lock     == sha256(current now.lock.pasta)
  fingerprint.source_tree  == sha256(current source file listing + mtimes)
  fingerprint.langs        == sha256(resolved language definitions)
  fingerprint.triple       == active triple
  fingerprint.profile      == active profile set
```

If the graph is valid, `now` skips construction entirely and proceeds
directly to execution. Graph construction is typically fast (milliseconds)
but for large projects with thousands of source files and a complex dep
graph it can be noticeable. The compiled graph eliminates this overhead
for the common case of "nothing structural has changed."

If the graph is stale, `now` rebuilds it, re-serialises it, and overwrites
`target/.now-graph`.

---

## 14.6 Graph Cache Beyond `target/`

The compiled graph can be stored and shared beyond the local `target/`
directory. This is useful for:

- **CI caching**: cache `target/.now-graph` between runs. If the fingerprint
  matches, the graph is instantly available.
- **Shared build farms**: a central cache server holds compiled graphs
  keyed by fingerprint. Workers fetch the graph rather than recomputing it.
- **Reproducible builds**: the graph is a complete, content-addressed build
  plan. Storing it by its own hash creates a permanent record of exactly
  how a build was executed.

### Remote Graph Cache

```pasta
; ~/.now/config.pasta
{
  graph_cache: {
    url:   "https://build-cache.internal/graphs",
    token: "${env.NOW_CACHE_TOKEN}"
  }
}
```

Protocol:
```
GET  /graphs/{fingerprint-hash}    → compiled graph binary or 404
PUT  /graphs/{fingerprint-hash}    → store compiled graph
```

The fingerprint hash is `sha256(fingerprint map serialised as canonical Pasta)`.
If the remote cache has the graph, `now` downloads it (~milliseconds for a
typical graph). If not, `now` constructs it and uploads it for future use.

---

## 14.7 Graph as a First-Class Output

The graph can be exported in human-readable form for inspection:

```sh
; Print the full graph in Pasta format
now graph:show

; Print only the build steps for a specific file
now graph:show src/main/c/parser.c

; Print the parallel execution sets
now graph:parallel-sets

; Print the critical path (longest dependency chain — determines minimum build time)
now graph:critical-path

; Export as DOT for Graphviz rendering
now graph:dot > build-graph.dot
dot -Tsvg build-graph.dot > build-graph.svg

; Count nodes
now graph:stats
```

`now graph:stats` output:
```
Build graph statistics:
  Source nodes:      47
  Step nodes:        47   (44 compile, 2 assemble, 1 link)
  File nodes:        142  (47 sources, 47 objects, 48 dep headers)
  Phase hook steps:  3
  Critical path:     6 steps (limited by header chain depth)
  Parallel width:    44 steps (max concurrent in first parallel set)
  Estimated time:    ~12s (44-way parallel) vs ~180s (sequential)
```

---

## 14.10 Parallel Execution and Selective Rebuild

Given the build graph, two capabilities follow naturally: parallel execution of independent nodes, and selective rebuild
of only those nodes whose inputs have changed. This document defines both,
including the feasibility constraints and the content-addressed output
cache that makes selective rebuild precise.

---

## 14.10 Parallel Execution Model

The build graph partitions naturally into **parallel sets**: groups of
nodes with no dependency relationship between them. Within a parallel set,
all nodes can execute concurrently. Sets must execute sequentially — no
node in set N can begin until all nodes in set N-1 are complete.

This is a standard level-synchronised BFS execution model. It is correct
for any DAG and requires no locking between workers.

### Why This is Feasible

Each `step` node in the build graph is:
- **Stateless**: reads fixed input files, writes to a fixed output path.
- **Non-communicating**: no step shares memory or file handles with another.
- **Idempotent**: running the same step twice with the same inputs produces
  the same output.
- **Failure-isolated**: a failing step does not corrupt other steps' outputs.

These properties make naive `fork/exec` parallelism correct without any
synchronisation primitives between worker processes.

---

## 14.11 Execution Engine

`now` manages a process pool directly — no make, no Ninja, no external
job server.

```
pool size = min(jobs, width of current parallel set)

algorithm:
  for each parallel_set Pₙ in topo order:
    ready_queue = all nodes in Pₙ
    running = {}
    while ready_queue or running:
      while |running| < pool_size and ready_queue:
        node = dequeue(ready_queue)
        if node.inputs_changed():       ; incremental check
          proc = spawn(node.tool, node.args)
          running[proc.pid] = node
        else:
          mark node as SKIP (cached)
      pid, status = wait_any()
      node = running.pop(pid)
      if status != 0:
        if fail_fast:
          kill all running
          report_failure(node)
          exit(1)
        else:
          record_failure(node)
          drain remaining in Pₙ (skip nodes that depend on failed node)
    if any failures in Pₙ:
      stop; report all failures; exit(1)
```

### Output Buffering

Each worker's stdout and stderr are captured to a per-node buffer.
Output is printed atomically when the node completes — never interleaved.
Failures are printed immediately; successes are suppressed unless `-v`.

```
[build] parser.c           (0.31s)
[build] writer.c           (0.28s)
[build] value.c            FAILED (0.12s)
        src/main/c/value.c:42:5: error: use of undeclared identifier 'buf_size'
[build] lexer.c            (0.44s)
[build] 3 remaining steps cancelled (fail-fast)
```

With `--no-fail-fast`, all steps in the current parallel set complete
before reporting failures and stopping.

---

## 14.12 Incremental Rebuild: The Input Change Check

Before spawning a worker for a node, `now` checks whether the node's
output is still valid. This is the `node.inputs_changed()` check.

### The Check

```
node is UP-TO-DATE if:
  1. All output files exist.
  2. For each output file: stored_hash(output) == sha256(output_file_on_disk)
  3. current_node_hash == stored_node_hash

where:
  current_node_hash = sha256(tool_hash, input_hashes, args)
  stored_node_hash  = value from target/.now-manifest for this node
```

If the node is up-to-date, it is marked SKIP and its output files are
treated as valid inputs for downstream nodes. If not, it is executed.

### What Triggers a Rebuild

| Change | Nodes invalidated |
|--------|-------------------|
| Source file content | That file's compile step + all steps downstream |
| Header file content | All compile steps that transitively include it |
| Compiler binary replaced | All compile steps using that compiler |
| Compile flag added/removed | All compile steps affected |
| Define added/removed | All compile steps affected |
| New source file added | New step created; link step invalidated |
| Source file deleted | Old step removed; link step invalidated |
| Dep version changed | All steps using headers/libs from that dep |
| Link flag changed | Link step only |
| New dep added | Link step + any steps using dep headers |

### What Does NOT Trigger a Rebuild

| Non-change | Result |
|------------|--------|
| Comment-only edit to source | sha256 changes → rebuild. Comments are not stripped. |
| File mtime change without content change | sha256 same → skip. `now` uses content, not mtime. |
| Unrelated source file changes | Only that file's step; nothing else. |
| Profile activation that doesn't affect this file's flags | Skip. |

Using content hashes rather than mtimes means `now` is correct across
filesystem operations that touch mtimes without changing content (checkout,
touch, rsync with --times, etc.).

---

## 14.13 Content-Addressed Output Cache

Beyond the local incremental build (which only avoids recompiling locally
unchanged files), `now` supports a **content-addressed output cache** (CAC):
a store where object files are keyed by their node hash. If the node hash
matches a cached entry, the object file is fetched from the cache rather
than compiled. This is the key to fast builds across machines and clean
checkouts.

### How It Works

```
before spawning a compile step for node N:
  hash = current_node_hash(N)
  if cache.has(hash):
    fetch cache entry → write to N's output path
    mark N as CACHE-HIT
    skip compilation
  else:
    compile N
    store output in cache under hash
    mark N as COMPILED
```

### Cache Entry

Each cache entry stores:
- The object file (`.o`).
- The dependency file (`.d`) — the transitive header list.
- The node hash that produced it.
- The tool invocation that produced it (for audit).

### Local CAC

By default, `now` maintains a local CAC at `~/.now/object-cache/`:

```
~/.now/object-cache/
└── {node-hash-prefix}/{node-hash}/
    ├── output.o         The object file
    └── output.d         The dep file
```

The local CAC is shared across all projects on the machine. A clean
checkout of a project whose objects are in the CAC rebuilds in seconds —
no compilation required for any file that was previously built with the
same inputs.

### Remote CAC

```pasta
; ~/.now/config.pasta
{
  object_cache: {
    url:   "https://build-cache.internal/objects",
    token: "${env.NOW_CACHE_TOKEN}",
    push:  true    ; upload compiled objects (not just download)
  }
}
```

With a remote CAC, a CI build populates the cache and subsequent developer
builds fetch objects rather than compiling. For a project with 500 source
files and a well-populated remote CAC, a clean checkout might require
zero compilations — only linking.

---

## 14.14 The Critical Path

The critical path is the longest chain of dependent steps in the build
graph. It determines the theoretical minimum build time regardless of
parallelism.

```sh
now graph:critical-path
```

```
Critical path (6 steps, ~2.1s minimum):
  src/main/c/core/alloc.c          (0.31s)  ← compiled early, included by many
  src/main/c/core/pool.c           (0.38s)  ← depends on alloc.h
  src/main/c/io/stream.c           (0.44s)  ← depends on pool.h
  src/main/c/net/http/parser.c     (0.61s)  ← depends on stream.h (longest compile)
  src/main/c/net/http/server.c     (0.29s)  ← depends on parser.h
  [link step]                      (0.08s)
  
  Total critical path: 2.11s
  Estimated wall time (44-way parallel): ~2.4s
  Sequential time: ~47s
  Parallelism speedup: ~20x
```

The critical path reveals which files to optimise for compile time when
reducing total build time — not the slowest individual file, but the
slowest chain.

---

## 14.15 Selective Rebuild in Practice

### Typical developer workflow

```
Edit src/main/c/parser.c
  → parser.c hash changed
  → parser.c.o is stale
  → link step is stale (input changed)
  → all other .o files: UP-TO-DATE

now build:
  SKIP  alloc.c     (up-to-date)
  SKIP  pool.c      (up-to-date)
  COMPILE parser.c  (0.31s)
  SKIP  writer.c    (up-to-date)
  ...
  LINK  myapp       (0.08s)
  
  1 compiled, 43 skipped, 1 linked  (0.39s total)
```

### Header change propagation

```
Edit src/main/include/mylib/types.h
  → header hash changed
  → all .o files whose .d includes types.h are stale
  → suppose 12 of 44 source files include types.h transitively

now build:
  COMPILE (12 files in parallel)
  SKIP    (32 files)
  LINK    myapp
  
  12 compiled, 32 skipped, 1 linked  (0.61s total)
```

### CAC-assisted clean build

```
git clean -fdx target/      (wipe all objects)
now build:
  CACHE-HIT  alloc.c     (fetched from ~/.now/object-cache/)
  CACHE-HIT  pool.c
  CACHE-HIT  parser.c    (fetched — hash matches previous build)
  ...
  CACHE-HIT  (44 of 44 files)
  LINK  myapp  (0.08s)
  
  0 compiled, 44 cache-hits, 1 linked  (0.51s total)
```

---

## 14.16 Feasibility Notes and Limits

### What parallelism cannot help with

- The **link step** is always sequential — the linker processes all objects
  in one invocation. For very large projects, link time dominates. Mitigation:
  use LTO thin-LTO (`-flto=thin`) which parallelises within the linker.
- **Sequential phase hooks** (tools and plugins at phase boundaries) are not
  parallelised — they run in declaration order.
- **Module-based languages** (Modula-2, Fortran with modules, C++ modules)
  have inter-file dependencies within the build phase. `now` detects these
  from the compiler's module dependency output and adjusts the parallel sets
  accordingly — module interface compilations are serialised before their
  consumers.

### C++ module support

C++ modules (`.cppm`, `.ixx`) produce a compiled module interface (`.pcm`
or `.ifc`) that must exist before dependent translation units can compile.
`now` handles this by:

1. Running a module scan pass (`--precompile-module-interface`) on all
   module interface files.
2. Adding the resulting CMI files as `file` nodes with edges to all
   dependent translation units.
3. The graph construction algorithm ensures CMIs are in an earlier parallel
   set than their consumers.

This is transparent — the user declares `lang: "c++"` with `std: "c++20"`
and `now` handles the module dependency topology automatically.

### Distributed builds

The content-addressed output cache is the foundation for distributed builds.
Each node is a pure function of its inputs; any machine with access to the
same inputs and the same tool can produce the same output (content-identical
under the same compiler version). A distributed executor (beyond the scope
of this document) can dispatch node executions to remote workers using
the same node hash as the cache key.

---


## 14.20 Module-Aware Pre-Scan Protocol

*(resolves GAP 6)*

Languages with compile-time inter-file dependencies require a dependency
discovery step before the build graph can be fully constructed. This
document specifies the pre-scan protocol: when it runs, how `now` invokes
it, what it produces, and how the results are integrated into the build graph.

Affected languages: C++ (modules), Ada (with-clauses), Modula-2 (IMPORT),
Fortran (USE/modules), and any custom language whose type definition
declares `prescan: true`.

Doc 16 §16.3 describes C++ module handling at a high level; this document
is the authoritative and complete specification. It supersedes doc 16
for the pre-scan mechanism.

---

## 14.20 The Problem

For C and assembly, `now` constructs the full build graph before any
compilation begins:

1. Enumerate source files matching `sources.pattern`.
2. For each source file, add a compile node with edges to its `.o` output.
3. Header dependencies (via `-MMD`) are discovered *during* compilation and
   incorporated into fingerprint tracking for the *next* build.

For module-aware languages, step 2 is not possible independently: whether
file A can be compiled before file B depends on the module interface graph,
which is embedded in the source text itself. The solution is a lightweight
pre-scan pass that runs before graph construction and provides the
inter-file dependency edges.

---

## 14.21 Pre-Scan Phase Placement

Pre-scan runs as a sub-phase of `build`, after source enumeration and
before graph construction:

```
build phase:
  1. source-enumerate     — match source files per sources.pattern
  2. pre-scan             — discover inter-file module dependencies (this doc)
  3. graph-construct      — build the node/edge DAG (doc 15)
  4. parallel-execute     — compile in topological order (doc 16)
```

Pre-scan only runs for languages whose type definition includes
`prescan: true` (§14.24). For all other languages, step 2 is a no-op.

Pre-scan results are cached. If the source files have not changed since
the last pre-scan, `now` uses the cached dependency graph without
re-scanning.

---

## 14.22 Pre-Scan Invocation

`now` invokes a pre-scan tool for each file in the source set that
belongs to a prescan-enabled language type. Invocation is fully parallel
— all source files of a given language are scanned concurrently using the
standard process pool.

### 14.22.1 Built-In Pre-Scanners

`now` provides built-in pre-scan implementations for the supported languages:

| Language | Built-in scanner mechanism |
|----------|---------------------------|
| C++ modules | Compiler-driven: `clang -E --preprocess-dependency-directives` or GCC `g++ -fmodules-ts -MF <depfile>` |
| Ada | `gcc -fdump-ada-spec-slim` or `gnatmake -M` |
| Modula-2 | Regex scan: `IMPORT <ModuleName> ;` patterns (lightweight, no compiler needed) |
| Fortran | Regex scan: `USE <ModuleName>` patterns |

Built-in scanners are selected automatically based on the active language
and toolchain. They do not require `prescan:` configuration in `now.pasta`
beyond enabling the language.

### 14.22.2 Custom Pre-Scanner

A custom language type may declare a pre-scanner as part of its type
definition in `now.pasta`:

```pasta
{
  languages: [
    {
      id: "mylang",
      types: [
        {
          pattern: "**.ml",
          produces: "object",
          tool: "mlc",
          prescan: {
            tool: "ml-deps",
            args: ["--format", "now-deps", "${source}"],
            format: "now-deps"
          }
        }
      ]
    }
  ]
}
```

#### `prescan` sub-map fields

| Field | Type | Notes |
|-------|------|-------|
| `tool` | `string!` | Executable name or path. Resolved via PATH and `toolchain.tools`. |
| `args` | `string[]?` | Argument list. Variable substitutions: `${source}`, `${source_dir}`, `${project_root}`, `${target_dir}`. |
| `format` | `enum(now-deps, make, json)?` | Output format the tool produces (see §14.23). Default: `now-deps`. |
| `timeout_ms` | `number?` | Per-file timeout. Default: 10 000 ms. |

---

## 14.23 Pre-Scan Output Formats

### 14.23.1 `now-deps` Format (canonical)

A Pasta map with a single key `deps` containing an array of dependency
entries:

```pasta
{
  deps: [
    {
      importer: "src/main/cpp/app.cppm",
      imports:  ["src/main/cpp/utils.cppm", "src/main/cpp/io.cppm"]
    },
    {
      importer: "src/main/cpp/utils.cppm",
      imports:  []
    }
  ]
}
```

All paths are relative to `sources.dir`. `now` validates this on parse:

- `importer` must be a file that exists in the source set.
- Each path in `imports` must also exist in the source set, *or* be a
  path to an installed module interface from a dependency (see §14.25).
- Cycles produce `NOW-E0307`.

### 14.23.2 `make` Format

Classic Makefile dependency format (produced by most compilers with `-M`):

```
app.o: src/main/cpp/app.cppm src/main/cpp/utils.cppm
utils.o: src/main/cpp/utils.cppm
```

`now` parses this and maps targets to source paths using the source set.
This format is supported for maximum compiler compatibility.

### 14.23.3 `json` Format

A JSON object:

```json
{
  "deps": [
    {
      "importer": "src/main/cpp/app.cppm",
      "imports":  ["src/main/cpp/utils.cppm"]
    }
  ]
}
```

Identical semantics to `now-deps`; JSON for tools that produce it natively.

---

## 14.24 Language Type `prescan` Field

To declare that a built-in or custom language requires pre-scanning, its
type entry in the language type system (doc 04c) includes `prescan: true`
(uses the built-in scanner for the language) or a `prescan` sub-map
(uses a custom scanner per §14.22.2):

```pasta
; Built-in prescan for C++ modules
{
  id: "c++",
  types: [
    {
      pattern: "**.cppm",
      produces: "object",
      prescan: true    ; use built-in C++ module scanner
    },
    {
      pattern: "**.cpp",
      produces: "object"
      ; no prescan: headers tracked via -MMD as usual
    }
  ]
}
```

Files matching types without `prescan` are not scanned. This means `.cpp`
implementation files that only `import` and do not `export module` can
rely on the compiled module interface (`.pcm`) being available as a file
node dependency — they do not need to be pre-scanned themselves, because
their dependency is on the `.pcm` output, not the source `.cppm`.

---

## 14.25 Installed Module Interfaces (Cross-Package)

When a dependency provides C++ module interfaces or Modula-2 definition
modules, `now` installs them into the standard installed layout (doc 05b):

```
~/.now/repo/<group>/<artifact>/<version>/
  h/          ; C/C++ headers
  mod/        ; Compiled Module Interfaces (.pcm, .bmi) and source module
               ; interfaces (.cppm, .ixx) for consumers that need to
               ; recompile them (compiler-version compatibility)
  lib/        ; Static and shared libraries
```

The `mod/` directory is a new sub-directory in the installed layout,
added by this document, alongside the existing `h/`, `c/`, `lib/` etc.

Pre-scan tool output may reference module paths outside the project's own
source set. `now` resolves such references by looking them up in the
installed `mod/` directories of the project's transitive dependencies.

If a reference cannot be resolved, `now` reports `NOW-E0306` (pre-scan
failure) with the detail: "unresolved module import `<name>`".

---

## 14.26 Pre-Scan Result Caching

Pre-scan results are stored in `target/.now-prescan/`:

```
target/
  .now-prescan/
    <lang>/
      <source-file-hash>.deps   ; now-deps format, one per scanned file
      prescan.graph             ; merged dependency graph (Pasta map)
```

Cache invalidation:
- If the source file content hash changes, its `.deps` entry is invalid
  and the file is re-scanned.
- If the installed `mod/` content of any transitive dep changes (dep
  upgrade), the entire prescan cache is invalidated.
- The merged `prescan.graph` is regenerated any time any `.deps` entry
  changes.

Pre-scan runs incrementally: only changed or new source files are
re-scanned. Unchanged files reuse their cached `.deps`.

---

## 14.27 Build Graph Integration

After pre-scan completes, the build graph constructor (doc 15) uses the
`prescan.graph` to add module-order edges:

1. For each `(importer, importee)` pair in `prescan.graph`:
   - If `importee` is in the project's source set: add a `file` node
     for the importee's `.pcm`/`.bmi` output, and an edge from the
     importer's compile node to that `file` node.
   - If `importee` is an installed dep module: add a `file` node
     pointing to the installed `.pcm` in `mod/`, with no build edge
     (it is pre-built).

2. The graph partitions into parallel sets (doc 16 §16.2) respecting
   these module edges: all module interface compilations that have no
   unbuilt imports come first (parallel set 0), then those depending on
   set 0, etc.

3. The source-level cycle check (doc 16 and §14.23.1 above) runs on the
   module dependency graph. A cycle at this level is `NOW-E0307`.

---

## 14.28 Language-Specific Notes

### 14.28.1 C++ Modules

C++20 modules introduce `export module <name>` and `import <name>`.
`now` distinguishes:

- **Module interface units** (`.cppm`, `.ixx`, or `.cpp` with
  `export module`): must be pre-scanned; produce a compiled module
  interface (`.pcm` for Clang, `.bmi` for GCC).
- **Module implementation units** (`.cpp` with `module <name>`): depend
  on the interface `.pcm`; do not themselves need to be pre-scanned
  (their interface dependency is known after the interface is compiled).
- **Non-module translation units** (`.cpp` with neither): standard header
  dependency tracking via `-MMD`; no pre-scan.

`now` detects the first line `export module` or `module <name>;` to
classify each `.cpp` automatically. Files without either are treated as
non-module translation units. This classification is done as part of
pre-scan without a full compiler invocation.

MSVC uses `/interface` and `/implementation` flags; doc 30 §30.10
extends the MSVC flag table for C++ modules.

### 14.28.2 Ada

Ada has a strict one-spec-per-package rule. `now` uses `gnatmake -M` or
`gcc -fdump-ada-spec-slim` to discover `with` dependencies. Ada source
classification (doc 04c) matches `.ads` (spec) and `.adb` (body) files;
pre-scan runs on both, but body files are only compiled after their spec.

Ada support is provisional: the built-in prescan is tested against GNAT.
Other Ada compilers may need a custom `prescan` map.

### 14.28.3 Modula-2

Modula-2 definition modules (`.def`) and implementation modules (`.mod`)
are handled similarly to Ada. The built-in pre-scanner uses a regex pass
over `IMPORT` and `FROM ... IMPORT` statements — no compiler invocation
needed, since Modula-2 imports are syntactically simple.

Definition modules must be compiled before implementation modules that
import them. `now` adds this ordering automatically from the pre-scan graph.

### 14.28.4 Fortran

Fortran modules (`MODULE <name>` / `USE <name>`) have the same structure.
The built-in pre-scanner uses a case-insensitive regex over `USE` statements.
Fortran module files (`.mod`) are compiler-generated rather than
source-written; `now` treats them as intermediate outputs similar to `.pcm`,
produced by compiling the `MODULE` definition and consumed by `USE`rs.

---

## 14.29 `now.pasta` Configuration

Pre-scan does not require explicit configuration for built-in languages.
Advanced options are available under `build_options`:

```pasta
{
  build_options: {
    prescan: {
      timeout_ms: 15000,         ; per-file timeout (default: 10000)
      parallel:   true,          ; scan files in parallel (default: true)
      cache:      true           ; cache results in target/.now-prescan (default: true)
    }
  }
}
```

`build_options` is a new top-level map in the `now.pasta` schema (see
doc 31 §31.3 for the schema entry, which lists it as `build_options: build-options-block?`).

---

## 14.30 `now schema:check` Integration

`now schema:check` (doc 31 §31.19) validates `prescan` maps in language
type declarations using the schema in §14.22.2. The `format` field must
be one of the three defined values; unknown formats produce `NOW-E0102`.

---

## 14.31 Interaction with Distributed Builds

Pre-scan results are content-addressed by source file hash (§14.26). In a
distributed build scenario (doc 16 §16.7), the pre-scan cache is shared
via the same content-addressed store as object files. A worker that
receives a compile task can verify its expected module interfaces are
present before beginning compilation — the build graph already encodes
these as explicit `file` node dependencies.

---

## 14.32 Supersession

This document supersedes doc 16 §16.3 ("C++ module support") for the
pre-scan mechanism. Doc 16 §16.3 may be read for background but this
document is authoritative for all pre-scan protocol details.

The `mod/` sub-directory in the installed layout (§14.25) is added to
the installed artifact layout defined in doc 05b. Doc 05b's table of
installed layout sub-directories is extended by this document.



---



---

# Chapter 14 — Packaging and Assembly

The `package` phase produces distributable artifacts. Like Maven assemblies,
`now` separates the *what* (the assembly descriptor) from the *how* (the
packaging tool). A project declares what format it wants to produce and what
goes into it. `now` drives the appropriate tool.

---

## 17.1 The Assembly Descriptor

Packaging is declared under `assembly:` in `now.pasta`. Multiple assemblies
may be declared — each produces a separate artifact.

```pasta
assembly: [
  {
    id:      "dist",
    format:  "tar.gz",           ; the output format
    out:     "target/pkg",
    include: [
      { src: "target/bin/${now.artifact}", dest: "bin/" },
      { src: "src/main/include/**",        dest: "include/" },
      { src: "LICENSE",                    dest: "/" },
      { src: "README.md",                  dest: "/" }
    ]
  },
  {
    id:      "amiga-lha",
    format:  "lha",
    out:     "target/pkg",
    include: [
      { src: "target/bin/${now.artifact}", dest: "C/" },
      { src: "assets/icons/*.info",        dest: "Prefs/Env-Archive/" },
      { src: "src/main/include/**",        dest: "Developer/Include/C/" }
    ]
  }
]
```

---

## 17.2 Built-In Formats

| Format ID | Extension | Tool | Notes |
|-----------|-----------|------|-------|
| `tar.gz` | `.tar.gz` | `tar` + `gzip` | Default. Universal. |
| `tar.bz2` | `.tar.bz2` | `tar` + `bzip2` | Better compression. |
| `tar.xz` | `.tar.xz` | `tar` + `xz` | Best compression. |
| `tar.zst` | `.tar.zst` | `tar` + `zstd` | Fast + good ratio. |
| `zip` | `.zip` | `zip` or `libzip` | Windows-friendly. |
| `lha` | `.lha` | `lha` | Amiga. Requires `lha` installed. |
| `lzx` | `.lzx` | `unlzx` / custom | Amiga. |
| `deb` | `.deb` | `dpkg-deb` | Debian/Ubuntu. |
| `rpm` | `.rpm` | `rpmbuild` | Red Hat/Fedora. |
| `dmg` | `.dmg` | `hdiutil` | macOS. Host must be macOS. |
| `ipa` | `.ipa` | `zip` (structured) | iOS. |
| `nsis` | `.exe` | `makensis` | Windows installer. |
| `flat` | *(dir)* | *(copy)* | Not archived; just a directory tree. |
| `custom` | *(any)* | *(declared)* | See 17.4. |

---

## 17.3 Include Directives

Each `include` entry within an assembly specifies what to put where.

```pasta
include: [
  ; Single file → destination directory
  { src: "target/bin/myapp",      dest: "bin/" },

  ; Glob → destination directory (preserves relative structure)
  { src: "src/main/include/**",   dest: "include/" },

  ; Glob with flatten (no subdir preservation)
  { src: "assets/icons/*.png",    dest: "icons/", flatten: true },

  ; Dep artifact files
  { dep: "zlib:zlib:1.3.0",       from: "h/**", dest: "include/" },

  ; Rename on copy
  { src: "README.md",             dest: "/", as: "README" },

  ; Conditional on platform
  { src: "target/bin/myapp.exe",  dest: "bin/", when: "windows:*:*" },
  { src: "target/bin/myapp",      dest: "bin/", when: "linux:*:*"   },

  ; Set file permissions in archive (unix-aware formats)
  { src: "target/bin/myapp",      dest: "bin/", mode: "0755" },

  ; Exclude pattern within a glob
  { src: "src/**",  dest: "src/", exclude: ["**/*.o", "**/.git*"] }
]
```

---

## 17.4 Custom Format: Packaging Tool as External Tool

When the built-in formats are insufficient, declare a `custom` format
with an external tool invocation:

```pasta
assembly: [
  {
    id:     "amiga-lha",
    format: "custom",
    out:    "target/pkg",

    ; Staging: now copies files to a temp directory first
    stage:  "target/stage/amiga",

    ; Then invokes this command with the staged directory
    tool: {
      run: "lha -ao6 ${output} ${stage}",
      ; ${output} = target/pkg/myapp-1.0.0.lha
      ; ${stage}  = target/stage/amiga/
    },

    ; Output filename template
    filename: "${now.artifact}-${now.version}.lha",

    include: [
      { src: "target/bin/${now.artifact}", dest: "C/",         mode: "0755" },
      { src: "docs/guide",                 dest: "HELP/",                   },
      { src: "assets/icons/*.info",        dest: "Prefs/",                  }
    ]
  }
]
```

`now` always stages before invoking a custom tool: it copies the included
files to `stage/` according to the `include` directives, then calls the
tool. The tool sees a clean, pre-organised directory — it doesn't need
to know about `now`'s source layout.

---

## 17.5 Assembly and the Installed Layout

Assemblies for library artifacts follow the installed layout convention
from document 05b. The canonical library assembly:

```pasta
assembly: [
  {
    id:     "sdk",
    format: "tar.gz",
    include: [
      { src: "src/main/include/**",                  dest: "h/" },
      { src: "target/${triple}/bin/lib${artifact}.a", dest: "lib/${triple}/" },
      { src: "target/${triple}/bin/lib${artifact}.so",dest: "lib/${triple}/" },
      { src: "now.pasta",                            dest: "/" },
      { src: "now.lock.pasta",                       dest: "/" },
      { src: "LICENSE",                              dest: "/" }
    ]
  }
]
```

When the format is `tar.gz` and `now` detects the layout matches the
installed layout convention (has `h/`, `lib/`, `now.pasta`), it marks the
archive as a `now`-native artifact installable by `now procure`.

---

## 17.6 Multi-Platform Assembly

When building for multiple triples, assemblies may collect all platform
outputs into one fat archive:

```pasta
assembly: [
  {
    id:     "fat-sdk",
    format: "tar.gz",
    targets: "*:*:*",        ; collect from all built triples
    include: [
      { src: "src/main/include/**",                         dest: "h/" },
      { src: "target/${triple}/bin/lib${artifact}.a",       dest: "lib/${triple}/" }
      ; ${triple} expands once per built target when targets: is set
    ]
  }
]
```



---



---

# Chapter 15 — Language Directories and Per-Module Language Declaration

Languages are declared per module. Each module's declared language set
determines which source directories are created, which file types are
recognised, and which tools are invoked. This document defines how language
declarations map to directory structures and how `now sketch` uses that
mapping to generate project layouts declaratively.

---

## 18.1 Language-to-Directory Mapping

Each language definition (document 04c) declares the source subdirectories
it owns under `dirs:`. These are the directories that `now sketch` creates
and that `now` scans for sources.

### Built-In Language Directory Mappings

| Language ID | Source dirs | Header/include dirs | Notes |
|-------------|-------------|---------------------|-------|
| `c` | `c/` | `h/` | `.c` → `c/`, `.h` → `h/` |
| `c++` | `cpp/` | `h/` | `.cpp` → `cpp/`, `.hpp` → `h/` |
| `mixed` (c+c++) | `c/`, `cpp/` | `h/` | Both share `h/` |
| `asm-gas` | `s/` | `i/` | `.s`/`.S` → `s/`, `.inc` → `i/` |
| `asm-nasm` | `asm/` | `i/` | `.asm` → `asm/`, `.inc` → `i/` |
| `m68k-asm` | `s/` | `i/` | Motorola 68k dialect of GAS |
| `objc` | `m/` | `h/` | `.m` → `m/`, shares `h/` with C |
| `modula2` | `mod/` | `def/` | `.mod` → `mod/`, `.def` → `def/` |
| `pascal` | `pas/` | `units/` | `.pas` → `pas/`, `.pp` → `pas/`, `.ppu` → `units/` |
| `ada` | `ada/` | `ads/` | `.adb` → `ada/`, `.ads` → `ads/` |
| `fortran` | `f/` | `h/` | `.f90` → `f/`, uses C `h/` for interop |

The `i/` directory for assembly is the conventional name for assembly
include files — mirrors the C `h/` but for assembler.

### Resolving Multiple Languages

When `langs: ["c", "m68k-asm"]` is declared, the union of all language
directory mappings applies:

```
src/main/
├── c/        ← from "c"
├── h/        ← from "c"  
├── s/        ← from "m68k-asm"
└── i/        ← from "m68k-asm"
```

When `langs: ["c", "pascal"]`:

```
src/main/
├── c/        ← from "c"
├── h/        ← from "c"
├── pas/      ← from "pascal"
└── units/    ← from "pascal"
```

When `langs: ["c", "c++", "asm-nasm"]`:

```
src/main/
├── c/        ← from "c"
├── cpp/      ← from "c++"
├── h/        ← shared (c and c++ both declare h/)
├── asm/      ← from "asm-nasm"
└── i/        ← from "asm-nasm"
```

Shared directories (`h/` claimed by both `c` and `c++`) are merged — one
physical directory, both languages contribute to it.

---

## 18.2 Custom Language Directory Mapping

A custom language declaration includes its directory mapping:

```pasta
langs: [
  {
    id: "m68k-asm",
    extends: "asm-gas",
    dirs: {
      source:  "s",      ; maps to src/main/s/
      include: "i"       ; maps to src/main/i/
    },
    types: [
      {
        id:         "m68k-source",
        pattern:    ["**.s", "**.S"],
        role:       "source",
        tool:       "${as}",
        args:       ["-m68060", "${asm_flags}", "${include_flags}", "${input}", "-o", "${output}"],
        produces:   "object",
        output_ext: ".s.o"
      }
    ]
  }
]
```

---

## 18.3 Per-Module Language Declaration

In a multi-module project, each module declares its own `langs:`. Modules
may use entirely different language sets:

```pasta
; root now.pasta
{
  group:   "org.acme",
  artifact: "retro-platform",
  version:  "1.0.0",
  modules: ["kernel", "gui", "tools"]
}
```

```pasta
; kernel/now.pasta
{
  group:    "org.acme",
  artifact: "retro-kernel",
  version:  "1.0.0",
  langs:    ["c", "m68k-asm"],

  sources: { dir: "src/main" }
  ; creates: src/main/c/, src/main/h/, src/main/s/, src/main/i/
}
```

```pasta
; gui/now.pasta
{
  group:    "org.acme",
  artifact: "retro-gui",
  version:  "1.0.0",
  langs:    ["c", "pascal"],

  sources: { dir: "src/main" },
  deps: [{ id: "org.acme:retro-kernel:1.0.0", scope: "compile" }]
  ; creates: src/main/c/, src/main/h/, src/main/pas/, src/main/units/
}
```

```pasta
; tools/now.pasta
{
  group:    "org.acme",
  artifact: "retro-tools",
  version:  "1.0.0",
  langs:    ["c++"],

  sources: { dir: "src/main" }
  ; creates: src/main/cpp/, src/main/h/
}
```

The resulting project tree:

```
retro-platform/
├── now.pasta
├── kernel/
│   ├── now.pasta
│   └── src/main/
│       ├── c/
│       ├── h/
│       ├── s/           ← m68k assembly
│       └── i/           ← assembly includes
├── gui/
│   ├── now.pasta
│   └── src/main/
│       ├── c/
│       ├── h/
│       ├── pas/         ← Pascal sources
│       └── units/       ← Pascal unit outputs
└── tools/
    ├── now.pasta
    └── src/main/
        ├── cpp/
        └── h/
```

---

## 18.4 `now sketch` — Declarative Project Scaffolding

`now sketch` generates a project layout from either a language list or an
existing `now.pasta`. It is the `now init` equivalent — but declarative
and repeatable, not a one-shot wizard.

### From a Language List

```sh
; Minimal C project
now sketch c

; C with 68k assembly
now sketch c m68k-asm

; Mixed C, Pascal, and NASM
now sketch c pascal asm-nasm

; Full specification with group and artifact
now sketch --group org.acme --artifact retro-app --version 1.0.0 c m68k-asm
```

Each `now sketch` call:
1. Writes `now.pasta` (if absent; prompts before overwriting).
2. Creates `src/main/{dirs}` for each language.
3. Creates `src/test/{dirs}` for each language.
4. Writes `.gitignore` containing `target/`.
5. Writes a skeleton source file in each source directory as a comment-only
   placeholder.
6. Prints what was created.

### Generated `now.pasta` for `now sketch c m68k-asm`

```pasta
; Generated by: now sketch c m68k-asm
; Edit this file to configure your project.
{
  group:    "com.example",      ; TODO: set your group
  artifact: "myproject",        ; TODO: set your artifact name
  version:  "0.1.0",

  langs: ["c", "m68k-asm"],
  std:   "c11",

  output: { type: "executable", name: "myproject" },

  compile: {
    warnings: ["Wall", "Wextra"],
    defines:  []
  },

  deps: []
}
```

### Generated Directory Tree for `now sketch c m68k-asm`

```
myproject/
├── now.pasta
├── .gitignore
└── src/
    ├── main/
    │   ├── c/
    │   │   └── main.c          ; placeholder
    │   ├── h/
    │   │   └── myproject.h     ; placeholder
    │   ├── s/
    │   │   └── startup.s       ; placeholder
    │   └── i/
    │       └── myproject.inc   ; placeholder
    └── test/
        ├── c/
        │   └── main_test.c     ; placeholder
        └── i/
```

### `now sketch` on an Existing `now.pasta`

If `now.pasta` already exists, `now sketch` reconciles the filesystem with
the descriptor — creating missing directories, leaving existing ones alone.
This makes it idempotent and safe to re-run after adding a language to an
existing project:

```sh
; Add pascal to an existing c project
; 1. Edit now.pasta: langs: ["c", "pascal"]
; 2. Re-run sketch to create the new directories
now sketch
```

Output:
```
Reconciling with now.pasta...
  c/      already exists — OK
  h/      already exists — OK
  pas/    created
  units/  created
Done.
```

---

## 18.5 Gestalt: Default Target Triple

When no `--target` is specified and no `target:` is declared in `now.pasta`,
`now` detects the host triple and uses it as the build target. This is
the **gestalt** — the implicit "build for what I'm running on" default.

### Host Detection

```
os:
  uname -s → Linux → "linux"
              Darwin → "macos"
              FreeBSD → "freebsd"
              Windows_NT → "windows"

arch:
  uname -m → x86_64 → "amd64"
              aarch64 / arm64 → "arm64"
              armv7l → "arm32"
              riscv64 → "riscv64"
              i686 → "x86"

variant:
  Detect libc: ldd --version → glibc → "gnu"
               ldd → musl → "musl"
  macOS: always "none"
  Windows: detect cl.exe → "msvc", else → "mingw"
```

The detected host triple is printed at the start of any build in verbose
mode:

```
now build -v
Host: linux:amd64:gnu
Target: linux:amd64:gnu (gestalt — matches host)
```

When cross-compiling is configured but no `--target` given, `now` uses the
first entry in `targets:` from `now.pasta` if declared, otherwise falls
back to the host gestalt. The gestalt is never a surprise — it is always
reported and always overridable.



---



---

# Chapter 16 — CI Integration

`now` is designed to be driven by CI systems and to emit machine-readable
output they can consume. Since Pasta is JSON-adjacent (maps, arrays,
scalars), `now` can emit either Pasta or JSON from any query or build
command. This document defines the CI integration model, output formats,
and environment variable conventions.

---

## 19.1 Machine-Readable Output

Any `now` command that produces structured output accepts `--output`:

```sh
now build --output pasta    ; Pasta format (default for now commands)
now build --output json     ; JSON (for CI, jq, existing tooling)
now build --output text     ; Human-readable (default for terminals)
```

`--output` is auto-detected when stdout is not a TTY: if piped or
redirected, `now` defaults to `pasta`. Override explicitly when needed.

### Example: Build Result as JSON

```sh
now build --output json
```

```json
{
  "phase": "build",
  "status": "ok",
  "triple": "linux:amd64:gnu",
  "steps": {
    "total": 44,
    "compiled": 3,
    "cached": 41,
    "skipped": 0,
    "failed": 0
  },
  "duration_ms": 412,
  "outputs": [
    { "type": "executable", "path": "target/linux-amd64-gnu/bin/myapp" }
  ]
}
```

### Example: Test Result as JSON

```sh
now test --output json
```

```json
{
  "phase": "test",
  "status": "failed",
  "suite": "unity",
  "tests": {
    "total": 12,
    "passed": 11,
    "failed": 1,
    "skipped": 0
  },
  "failures": [
    {
      "name": "test_parser_string_escape",
      "file": "src/test/c/parser_test.c",
      "line": 47,
      "message": "Expected 5 but was 3"
    }
  ],
  "duration_ms": 18
}
```

---

## 19.2 Exit Codes (Reminder for CI)

| Code | Meaning | CI action |
|------|---------|-----------|
| 0 | Success | Pass |
| 1 | Build error | Fail, show compiler output |
| 2 | Test failure | Fail, show test report |
| 3 | Dep resolution failure | Fail, check network/registry |
| 4 | Config error | Fail, check `now.pasta` |
| 5 | I/O error | Fail, check disk/network |
| 6 | Auth failure | Fail, check credentials |

---

## 19.3 Environment Variable Conventions

`now` reads the following environment variables, which CI systems set
automatically or can be configured:

| Variable | Meaning | Default |
|----------|---------|---------|
| `NOW_HOME` | Override `~/.now` location | `~/.now` |
| `NOW_CACHE_DIR` | Override cache directory | `${NOW_HOME}/cache` |
| `NOW_REPO_DIR` | Override local repo | `${NOW_HOME}/repo` |
| `NOW_JOBS` | Parallel job count | CPU count |
| `NOW_TOKEN` | Auth token for default registry | *(none)* |
| `NOW_REGISTRY` | Override default registry URL | central registry |
| `NOW_OFFLINE` | If `1`, implies `--offline` | `0` |
| `NOW_LOCKED` | If `1`, implies `--locked` | `0` |
| `NOW_COLOR` | If `0`, disable ANSI colour | auto |
| `NOW_LOG_LEVEL` | `error`\|`warn`\|`info`\|`debug` | `info` |
| `CC` | C compiler override | *(toolchain resolution)* |
| `CXX` | C++ compiler override | *(toolchain resolution)* |
| `AR` | Archiver override | *(toolchain resolution)* |

CI-recommended environment:

```yaml
# GitHub Actions example
env:
  NOW_LOCKED: "1"          # fail if lock is inconsistent
  NOW_CACHE_DIR: "${{ runner.temp }}/now-cache"
  NOW_COLOR: "0"
```

---

## 19.4 CI Cache Integration

CI systems cache directories between runs. `now`'s cache layout is
designed for this:

### GitHub Actions

```yaml
- name: Cache now dependencies
  uses: actions/cache@v4
  with:
    path: ~/.now/cache
    key: now-${{ runner.os }}-${{ hashFiles('now.lock.pasta') }}
    restore-keys: |
      now-${{ runner.os }}-

- name: Build
  run: now build --locked
```

The cache key uses `now.lock.pasta` hash — a dep change invalidates the
cache cleanly. The `restore-keys` fallback allows partial cache hits when
only some deps changed.

### GitLab CI

```yaml
cache:
  key:
    files:
      - now.lock.pasta
  paths:
    - .now-cache/
variables:
  NOW_CACHE_DIR: "$CI_PROJECT_DIR/.now-cache"

build:
  script:
    - now build --locked --output json | tee build-result.json
  artifacts:
    reports:
      junit: target/test-results/*.xml
```

### Object Cache in CI

For large projects, caching compiled objects across runs eliminates
recompilation on CI:

```yaml
- name: Cache now objects
  uses: actions/cache@v4
  with:
    path: ~/.now/object-cache
    key: now-obj-${{ runner.os }}-${{ hashFiles('now.lock.pasta', 'src/**') }}
    restore-keys: |
      now-obj-${{ runner.os }}-${{ hashFiles('now.lock.pasta') }}-
      now-obj-${{ runner.os }}-
```

With a warm object cache and unchanged source files, CI builds skip all
compilation and only run the link step.

---

## 19.5 Build Matrix in CI

`now`'s `--target` fan-out maps naturally onto CI matrix strategies:

### GitHub Actions Matrix

```yaml
strategy:
  matrix:
    target:
      - linux:amd64:gnu
      - linux:amd64:musl
      - linux:arm64:gnu
      - macos:arm64:none
      - windows:amd64:msvc

jobs:
  build:
    runs-on: ${{ matrix.target == 'macos:arm64:none' && 'macos-14' || 'ubuntu-24.04' }}
    steps:
      - uses: actions/checkout@v4
      - run: now build --target ${{ matrix.target }} --locked --output json
```

Alternatively, let `now` own the matrix and run it in a single job
with `--parallel`:

```yaml
- run: now build --target "linux:*:*" --parallel 4 --output json
```

---

## 19.6 `now ci` Subcommand

`now ci` is a convenience command that runs the standard CI lifecycle with
CI-appropriate defaults:

```sh
now ci
```

Equivalent to:
```sh
now clean build test package --locked --output json --no-color
```

Additional CI-specific behaviours:
- Writes test results to `target/test-results/` in JUnit XML format.
- Writes a build summary to `target/now-ci-report.pasta` and
  `target/now-ci-report.json`.
- Sets `NOW_LOCKED=1` automatically.
- Exits non-zero on any failure, warning, or test failure.
- Emits GitHub Actions / GitLab annotations if the corresponding
  `CI` environment variable is detected.

### CI Report Format

```pasta
; target/now-ci-report.pasta
{
  project:    "org.acme:myapp:1.0.0",
  triple:     "linux:amd64:gnu",
  started:    "2026-03-05T14:00:00Z",
  finished:   "2026-03-05T14:00:42Z",
  duration_ms: 42000,
  status:     "ok",

  phases: [
    { phase: "procure", status: "ok",     duration_ms: 312,   deps_installed: 3 },
    { phase: "build",   status: "ok",     duration_ms: 38200, compiled: 44, cached: 0 },
    { phase: "test",    status: "ok",     duration_ms: 18,    passed: 12, failed: 0 },
    { phase: "package", status: "ok",     duration_ms: 204                          }
  ],

  artifacts: [
    { id: "dist", path: "target/pkg/myapp-1.0.0-linux-amd64-gnu.tar.gz",
      sha256: "a3f8c2..." }
  ]
}
```



---



---

# Chapter 17 — Signing, Integrity, and Trust

`now` integrates signing at three levels: artifact integrity (tamper
detection), publisher identity (who produced this artifact), and
platform-specific certification (e.g. Apple notarisation, Amiga RNC
signing, code signing certificates). Signing also participates in
dependency resolution — unsigned or untrusted packages can be rejected
before they influence a build.

---

## 20.1 Integrity: SHA-256 Checksums

Every artifact in the registry ships with a `.sha256` file. `now procure`
verifies every downloaded archive against its checksum before installation.
This is always on — there is no flag to disable it. A mismatch is a hard
error.

```
~/.now/cache/org/acme/core/4.2.1/
├── core-4.2.1-linux-amd64-gnu.tar.gz
└── core-4.2.1-linux-amd64-gnu.sha256    ← verified on every install
```

The sha256 is also recorded in `now.lock.pasta`. Even if the registry
serves a different file at the same URL (supply chain attack, storage
corruption), the lock file detects it.

---

## 20.2 Publisher Signing: Detached Signatures

Beyond content integrity, artifacts may be signed by their publisher.
`now` supports detached signature files alongside archives:

```
core-4.2.1-linux-amd64-gnu.tar.gz
core-4.2.1-linux-amd64-gnu.tar.gz.sha256
core-4.2.1-linux-amd64-gnu.tar.gz.sig      ← detached signature
```

### Signature Format

`now` uses **minisign** (a modern, small, Ed25519-based signing tool) as
the default signature format. It is simpler than GPG, has no web-of-trust
complexity, and produces small signatures. The public key is a single line
stored in the artifact's registry metadata.

```sh
; Publisher signs before uploading
minisign -Sm core-4.2.1-linux-amd64-gnu.tar.gz -s ~/.minisign/mykey.key
now publish    ; uploads .tar.gz, .sha256, and .sig
```

### Verification During `procure`

```pasta
; now.pasta — require signatures from all deps
{
  trust: {
    require_signatures: true,         ; reject unsigned packages
    require_known_keys: true          ; reject packages with unknown publisher keys
  }
}
```

When `require_signatures: true`, `procure` fetches the `.sig` file for
every dep and verifies it against the publisher's registered public key.
A missing or invalid signature aborts the build.

```
Signature verification:
  org.acme:core:4.2.1       OK      (Ed25519, key: RWT...)
  zlib:zlib:1.3.0           OK      (Ed25519, key: RWT...)
  org.acme:unknown:1.0.0    FAIL    (no signature found)
  → Aborting: unsigned package with require_signatures: true
```

---

## 20.3 Trust Store

The trust store maps group prefixes and artifact coordinates to trusted
public keys. It lives in `~/.now/trust.pasta`.

```pasta
; ~/.now/trust.pasta
{
  keys: [
    {
      ; Trust all artifacts from org.acme
      scope:  "org.acme",
      key:    "RWTnKf5...",           ; minisign public key
      comment: "ACME Corp build key"
    },
    {
      ; Trust a specific artifact at any version
      scope:  "zlib:zlib",
      key:    "RWTx8Qz...",
      comment: "zlib official"
    },
    {
      ; Trust the central registry's own signing key
      scope:  "*",
      key:    "RWTcentralkey...",
      comment: "now central registry"
    }
  ]
}
```

The central registry signs all artifacts it hosts with its own key.
Projects that trust the registry key transitively trust all packages
from the registry. Private registries sign with their own keys.

### Trust Levels

| Level | `require_signatures` | `require_known_keys` | Meaning |
|-------|---------------------|---------------------|---------|
| `none` | false | false | SHA-256 only (always on) |
| `signed` | true | false | Must be signed; key need not be known |
| `trusted` | true | true | Must be signed by a key in trust store |

Default: `none` (integrity only). Recommended for production: `trusted`.

---

## 20.4 Post-Build Signing Hooks

After the `link` phase produces a binary, a `post-link` hook may sign it.
This is separate from artifact signing (which happens during `publish`) and
addresses platform-specific binary signing requirements.

### Generic Binary Signing

```pasta
plugins: [
  {
    id:     "org.now.plugins:minisign:1.0.0",
    type:   "plugin",
    phase:  "post-link",
    config: {
      key:    "${env.SIGN_KEY_PATH}",
      output: "${target}/bin/${now.artifact}.sig"
    }
  }
]
```

### Apple Code Signing (macOS)

```pasta
plugins: [
  {
    id:    "org.now.plugins:apple-codesign:1.0.0",
    type:  "plugin",
    phase: "post-link",
    when:  "macos:*:*",           ; only on macOS targets
    config: {
      identity:   "${env.APPLE_SIGNING_IDENTITY}",
      entitlements: "entitlements.plist",
      notarise:   true,
      apple_id:   "${env.APPLE_ID}",
      team_id:    "${env.APPLE_TEAM_ID}"
    }
  }
]
```

### Windows Authenticode Signing

```pasta
plugins: [
  {
    id:    "org.now.plugins:authenticode:1.0.0",
    type:  "plugin",
    phase: "post-link",
    when:  "windows:*:*",
    config: {
      pfx:      "${env.CODESIGN_PFX_PATH}",
      password: "${env.CODESIGN_PFX_PASSWORD}",
      timestamp: "http://timestamp.digicert.com"
    }
  }
]
```

### Amiga / Platform-Specific Signing

Platforms with their own binary certification schemes (Amiga hunk signing,
console vendor certification) are handled the same way — a `post-link`
plugin that knows the platform's signing protocol:

```pasta
plugins: [
  {
    id:    "org.now.plugins:amiga-sign:1.0.0",
    type:  "plugin",
    phase: "post-link",
    when:  "amiga:*:*",
    config: {
      vendor_key: "${env.AMIGA_VENDOR_KEY}"
    }
  }
]
```

---

## 20.5 Signing During `publish`

The `publish` phase signs the artifact archive before uploading:

```pasta
{
  publish: {
    sign:    true,
    key:     "${env.PUBLISH_KEY_PATH}",    ; minisign private key
    ; OR: use a hardware key / HSM
    sign_via: "env.SIGNTOOL_CMD"           ; arbitrary signing command
  }
}
```

With `sign: true`, `now publish`:
1. Computes sha256 of the archive.
2. Signs the archive with the configured key.
3. Uploads: `.tar.gz`, `.sha256`, `.tar.gz.sig`.

The registry stores the publisher's public key against the group,
verified once at account registration. Consumers' `procure` can then
verify any artifact from that publisher against the registered key.

---

## 20.6 Supply Chain: What Signing Prevents

| Attack | Prevented by |
|--------|-------------|
| Registry serves wrong file | SHA-256 + lock file |
| DNS hijack / MITM on download | SHA-256 + lock file |
| Compromised registry storage | SHA-256 + lock file |
| Malicious package published by wrong person | Publisher signing + trust store |
| Compromised publisher account | Hardware key / HSM + `require_known_keys` |
| Dependency confusion attack | Explicit `repos:` ordering + private registry |
| Typosquatting | `require_known_keys` (unknown publisher → rejected) |
| Infected pre-built binary | SHA-256 of tool binaries in build manifest |

`now` does **not** prevent:
- A legitimate publisher publishing malicious code.
- A compromised private key.
- Social engineering of the publisher.

These require code review and organisational process, not build tooling.

---

## 20.7 Key Management Summary

```sh
; Generate a signing key pair
now keys:generate --name "My Build Key" --output ~/.now/keys/

; Export public key for registry registration
now keys:export --public ~/.now/keys/mykey.pub

; Add a trusted key to the trust store
now trust:add --scope "org.acme" --key "RWT..." --comment "ACME Corp"

; List trusted keys
now trust:list

; Verify a downloaded artifact manually
now verify target/pkg/myapp-1.0.0-linux-amd64-gnu.tar.gz

; Verify all cached artifacts against the trust store
now cache:verify --trust
```



---



---

# Chapter 18 — Module System and Build Order

---

## 21.1 The Module Graph

A workspace's modules form a directed graph where edges represent
dependency relationships. Unlike the artifact dependency graph (which
resolves across the registry), the module graph is resolved locally from
the workspace tree.

### Module Graph Construction

```
1. Parse root now.pasta → collect modules: []
2. For each module M:
     parse M/now.pasta → collect deps: []
     for each dep D in M.deps:
       if D.group:D.artifact matches any module in the workspace:
         add edge M → sibling(D)   ; M depends on the sibling
       else:
         add to external dep set   ; resolved from registry as normal
3. Result: directed graph G of workspace modules
```

A module is recognised as a workspace sibling if its `group:artifact`
coordinate matches a module's `group` and `artifact` fields, regardless
of the version specifier in the dep. Version specifiers on sibling deps
are validated (the sibling's declared version must satisfy the range) but
do not drive resolution — the local source is always used.

### Cycle Detection

`now` runs a standard DFS-based cycle detection on G immediately after
construction. A cycle is a hard error at startup — no build proceeds.

```
Cycle detected in workspace module graph:

  org.acme:kernel → org.acme:hal → org.acme:drivers → org.acme:kernel

  Module dependency cycles are not permitted.
  To resolve: extract the shared code into a new module with no
  workspace dependencies, or restructure the dependency direction.
```

The error report includes the full cycle path. `now graph:modules` can
be used to visualise the module graph before building.

### Topological Order

After cycle detection, `now` produces a topological ordering of modules.
This is the order in which modules are built — leaves (no workspace deps)
first, root last.

```
; workspace with: kernel ← hal ← drivers ← gui ← app
; and:            kernel ← net ← app

Topological build order:
  1. kernel        (no workspace deps)
  2. hal           (depends on: kernel)
  3. net           (depends on: kernel)
  4. drivers       (depends on: hal)
  5. gui           (depends on: drivers)
  6. app           (depends on: gui, net)
```

Modules at the same depth with no dependency between them (e.g. `hal` and
`net` both depend only on `kernel`) are independent and may be built in
parallel.

---

## 21.2 Building the Full Workspace

```sh
now build             ; build all modules in topological order
now test              ; test all modules (each after its build)
now package           ; package all modules
now install           ; install all modules to ~/.now/repo/
```

When run from the workspace root, `now` processes every module. For each
phase, all modules complete that phase before any module begins the next.
This ensures that when `gui` enters `build`, `kernel`, `hal`, and `drivers`
have all completed `install` and are available as local deps.

### Automatic Sibling Resolution

`now procure` for a module skips the registry for sibling deps and instead
uses the version already installed to `~/.now/repo/` by the earlier build
step. If a sibling has not been installed yet, `now` automatically runs
`now install` for that sibling first (in dependency order).

This means building from the workspace root always produces a consistent
state — you never get a mix of registry-fetched and locally-built siblings.

---

## 21.3 Targeted Module Builds

### Single Module

```sh
; Build only the scheduler module (and its workspace dependencies)
now build :scheduler
```

`:scheduler` selects the module whose `artifact` field is `"scheduler"`.
`now` computes the transitive workspace dependency closure of `scheduler`
and builds only those modules — nothing else.

```
Target: :scheduler
Dependency closure: [kernel, hal, drivers, scheduler]
Build order: kernel → hal → drivers → scheduler
Skipped: [net, gui, app]
```

### Multiple Modules

```sh
; Build kernel and gui (and their respective closures, merged)
now build [:kernel, :gui]
```

Array notation selects multiple named targets. `now` computes the union of
their transitive dependency closures and builds the minimal set.

```
Targets: [:kernel, :gui]
Closure of :kernel: [kernel]
Closure of :gui:    [kernel, hal, drivers, gui]
Union:              [kernel, hal, drivers, gui]
Build order:        kernel → hal → drivers → gui
Skipped:            [net, app]
```

### Excluding Modules

```sh
; Build everything except the app module
now build --exclude :app
```

### Without Closure (Explicit)

```sh
; Build only scheduler itself — assume deps are already installed
now build :scheduler --no-closure
```

`--no-closure` skips the dependency closure computation and builds only
the named module(s). If workspace deps are not installed, the build will
fail at compile time. This is the fast path for iterating on a single
module when you know deps haven't changed.

---

## 21.4 Detecting What Changed

When building from the workspace root without specifying targets, `now`
can use the build graph's content-hash manifest to detect which modules
have changed since the last build and build only those (plus their
dependents).

```sh
now build --changed          ; build modules that have changed + dependents
now build --changed :scheduler  ; build scheduler if it or its deps changed
```

Change detection is based on the same content-hash mechanism as file-level
incremental builds (document 16), applied at the module level. A module
is considered changed if any of its source files, `now.pasta`, or its
workspace dep outputs have changed.

---

## 21.5 Version Scheme

`now` uses an extended version scheme that supersedes the pure SemVer
description in document 12.

### Version Format

```
core.major.minor[-qualifier[-build.timestamp]]
```

| Component | Required | Type | Description |
|-----------|----------|------|-------------|
| `core` | yes | non-negative integer | Compatibility epoch. Increment signals fundamental breaking change beyond API — protocol, on-disk format, fundamental architectural shift. `0` during initial development. |
| `major` | yes | non-negative integer | API-breaking change within the compatibility epoch. |
| `minor` | yes | non-negative integer | Backward-compatible change: new features, no breaks. |
| `qualifier` | no | string | Pre-release label: `alpha`, `beta`, `rc`, `snapshot`, or any alphanumeric string. Multiple qualifiers separated by `.`: `beta.2`, `rc.1`. |
| `build` | no | literal string `build` | Separator keyword before the timestamp. Present only if timestamp is present. |
| `timestamp` | no | integer or date string | Build timestamp. Numeric (`20260305142200`) or ISO date (`2026-03-05`). Useful for snapshot tracing. |

### Valid Examples

```
1.0.0                        ; stable release
1.0.0-beta.1                 ; beta pre-release
1.0.0-rc.2                   ; release candidate
2.3.1-snapshot               ; snapshot (latest development)
2.3.1-snapshot-build.20260305142200  ; snapshot with build timestamp
0.9.0-alpha                  ; pre-1.0 alpha
3.0.0-beta.1-build.2026-03-05  ; beta with date-based build ID
```

### Comparison Rules

Version ordering (from lowest to highest):

```
0.9.0-alpha  <  0.9.0-beta.1  <  0.9.0-rc.1  <  0.9.0  <  0.9.1  <  1.0.0-alpha  <  1.0.0
```

1. Compare `core`, then `major`, then `minor` numerically.
2. A version with no qualifier is higher than any pre-release qualifier
   of the same `core.major.minor`.
3. Among pre-release qualifiers, order is: `alpha < beta < rc < snapshot`.
4. Qualifiers with numeric suffixes are ordered numerically: `beta.1 < beta.2`.
5. The `build.timestamp` component is ignored in comparisons (as in SemVer
   build metadata) — two versions differing only in timestamp are equal for
   resolution purposes.

### Range Syntax with Extended Versions

Range specifiers apply to `core.major.minor` only. Qualifier and build
components are never part of range resolution:

```
^1.2.0     ; >=1.2.0 <2.0.0 (major is the upper bound, not core)
~1.2.0     ; >=1.2.0 <1.3.0
^1.0.0     ; accepts 1.0.0, 1.0.1, 1.1.0, ... but not 2.0.0
```

Wait — what does `core` mean for ranges? The `core` component changes
meaning: bumping `core` is a signal of a deeper break than `major`. Range
resolution never crosses a `core` boundary:

```
^1.2.0     ; >=1.2.0, core must be 1, major >=2 (actually: same core, major >=2.0 wait...)
```

Actually the range semantics are:

| Range | Meaning |
|-------|---------|
| `^C.A.B` | `>=C.A.B`, same `C`, any `A` >= the given `A` |
| `~C.A.B` | `>=C.A.B`, same `C.A`, any `B` >= the given `B` |
| `C.A.B` | Exact `C.A.B` (any qualifier, build ignored) |
| `C.A.B-qualifier` | Exact `C.A.B-qualifier` |
| `>=C.A.B` | `core.major.minor >= C.A.B` (same core only) |

The `core` component is always pinned in range resolution: `^1.2.0` never
resolves to `2.0.0`. Crossing a `core` boundary always requires an explicit
version bump in `now.pasta`.

---

## 21.6 Module Version Management

### Default: Inherited Root Version

When a workspace root declares `version: "1.5.0"` and `inheritance: true`,
all modules default to `1.5.0` unless they declare their own version.
A module's `now.pasta` may omit the `version` field, in which case it
inherits the root version.

```pasta
; root: version: "1.5.0"
; kernel/now.pasta — inherits root version
{
  group:    "org.acme",
  artifact: "kernel"
  ; version omitted → inherits "1.5.0" from root
}
```

### Local Override

A module may declare its own version. This is allowed and expected for
hotfixes — a leaf module may be at a different version than the rest of
the workspace:

```pasta
; kernel/now.pasta — hotfix on this module only
{
  group:    "org.acme",
  artifact: "kernel",
  version:  "1.5.1"   ; hotfix; workspace root is still at 1.5.0
}
```

### Harmonisation Rules

Version overrides must not violate compatibility with the workspace root:

1. A module's `core` must equal the root's `core`. Cross-core module
   versions require the root to also bump `core` — you cannot have a
   `2.x.y` module in a `1.x.y` workspace.
2. A module's `major` may be higher than the root's (hotfix) but not lower
   (a module cannot be older than the workspace it lives in).
3. `minor` and qualifier are unconstrained.

These rules are checked by `now version:check` (see 21.7). They are
warnings, not errors, during normal builds — a mis-versioned module builds
but cannot be published to a registry that enforces harmonisation.

### Independent Workspace (`inheritance: false`)

When the root has `inheritance: false`, every module must declare its own
version. There is no default. Versions are unconstrained — modules in an
independent workspace may span multiple `core.major` values. This is the
right model for a monorepo containing unrelated projects.

---

## 21.7 `now version` Command Family

Version management is a first-class CLI concern.

### Query

```sh
now version:show                ; print workspace and all module versions
now version:check               ; validate version harmonisation rules
now version:graph               ; show version relationships as a tree
```

`now version:show` output:
```
Workspace: org.acme:acme-platform  1.5.0
  kernel     org.acme:kernel        1.5.1  (local override — hotfix)
  hal        org.acme:hal           1.5.0  (inherited)
  drivers    org.acme:drivers       1.5.0  (inherited)
  gui        org.acme:gui           1.5.0  (inherited)
  net        org.acme:net           1.5.0  (inherited)
  app        org.acme:app           1.5.0  (inherited)
```

### Absolute Set

```sh
; Set the workspace root and all inheriting modules to an exact version
now version:set 1.6.0

; Set the workspace root only (modules retain their current versions)
now version:set 1.6.0 --root-only

; Set a specific module's version
now version:set 1.5.2 :kernel

; Set all modules, including those with local overrides
now version:set 1.6.0 --all
```

`version:set` writes the new version to `now.pasta` for the affected
descriptors and reports what changed:

```
now version:set 1.6.0

  root         1.5.0 → 1.6.0
  kernel       1.5.1 → 1.6.0  (was locally overridden — resetting to inherited)
  hal          1.5.0 → 1.6.0  (inherited)
  drivers      1.5.0 → 1.6.0  (inherited)
  gui          1.5.0 → 1.6.0  (inherited)
  net          1.5.0 → 1.6.0  (inherited)
  app          1.5.0 → 1.6.0  (inherited)

  7 descriptors updated.
```

### Relative Increment

```sh
; Increment the minor component
now version:increment minor           ; 1.5.0 → 1.5.1
now version:increment major           ; 1.5.0 → 1.6.0
now version:increment core            ; 1.5.0 → 2.0.0  (resets major and minor to 0)

; Increment with qualifier
now version:increment minor --qualifier beta.1    ; 1.5.0 → 1.5.1-beta.1

; Increment a specific module
now version:increment minor :kernel   ; only kernel's version

; Increment and add build timestamp
now version:increment minor --timestamp           ; 1.5.0 → 1.5.1-build.20260305142200
```

### Qualifier Management

```sh
; Add or change qualifier
now version:qualify beta.2            ; 1.5.1 → 1.5.1-beta.2
now version:qualify rc.1              ; 1.5.1-beta.2 → 1.5.1-rc.1

; Remove qualifier (promote to release)
now version:release                   ; 1.5.1-rc.1 → 1.5.1

; Mark as snapshot
now version:snapshot                  ; 1.5.1 → 1.5.1-snapshot
```

### Validation

```sh
now version:check
```

Validates all module versions against harmonisation rules:

```
Version check:
  kernel  1.5.1   OK  (hotfix — major matches root 1.5.0)
  hal     1.5.0   OK  (matches root)
  drivers 1.5.0   OK  (matches root)
  net     1.5.0   OK  (matches root)
  gui     1.5.0   OK  (matches root)
  app     3.0.0   WARN  core mismatch — root core is 1, app core is 3
                        App cannot be published from this workspace.
                        Use 'now version:set 1.0.0 :app' or extract app
                        to an independent workspace.
```

---

## 21.8 The Module Graph as a Build Graph Input

The module graph feeds into the build graph (document 15) as a higher-level
structure. Each module is a subgraph within the overall workspace build
graph. Module boundaries are `phase` nodes in the graph — when all `link`
steps in module A complete, a `phase:module:A:done` node is marked, and
dependent module B's `procure` step (which installs A) is unblocked.

This means the workspace build graph supports the same parallel execution
model as the single-project build graph — independent modules build
concurrently, and within each module, source files compile concurrently.

```sh
now graph:modules           ; show the module dependency graph
now graph:modules --dot     ; DOT format for Graphviz
now graph:show --workspace  ; full workspace build graph (can be large)
```



---



---

# Chapter 19 — IDE Integration and `now stay` Daemon

> **Scope note — Post-v1 feature.** This chapter is fully specified but is
> **not targeted for the initial open-source release**. It should be
> implemented after the core build pipeline (Chapters 1–17) is stable and
> battle-tested. The specification is kept here so that v2 work can begin
> from a solid foundation without redesign.


---

## 22.1 `compile_commands.json` Generation

`compile_commands.json` is the de-facto standard for C/C++ tooling
integration. clangd, ccls, clang-tidy, VSCode, CLion, Neovim, Emacs, and
every other serious C/C++ editor or LSP client reads it. The schema is
fixed by the ecosystem — `now` must emit strict JSON, not Pasta, not JSON5.

### Generation

```sh
now compile-db                    ; writes compile_commands.json to project root
now compile-db --output pasta     ; same data as Pasta (for now tooling)
now compile-db --target linux:amd64:gnu   ; specific triple
now compile-db :scheduler         ; specific named target only
```

`now compile-db` walks the build graph and serialises each compile step
node into the standard record format:

```json
[
  {
    "directory": "/home/alice/acme-platform/kernel",
    "file": "/home/alice/acme-platform/kernel/src/main/c/sched.c",
    "arguments": [
      "gcc",
      "-std=c11", "-Wall", "-Wextra", "-Werror",
      "-DACME_INTERNAL",
      "-I/home/alice/acme-platform/kernel/src/main/h",
      "-I/home/alice/.now/repo/amiga/os/intuition/1.3.0/h",
      "-c", "/home/alice/acme-platform/kernel/src/main/c/sched.c",
      "-o", "/home/alice/acme-platform/kernel/target/obj/main/sched.c.o"
    ]
  },
  ...
]
```

`arguments` array form is preferred over the `command` string form —
it avoids shell quoting ambiguity and is what clangd recommends.

### Placement

By convention `compile_commands.json` lives at the project root. For
workspaces with multiple modules, `now compile-db` writes one file at
the workspace root covering all modules — clangd's project root detection
finds it automatically.

```sh
now compile-db --per-module       ; writes one file per module in module root
now compile-db --root             ; writes one file at workspace root (default)
```

### Freshness

`compile_commands.json` goes stale when `now.pasta` changes, deps change,
or profiles change. `now stay` (see §22.3) keeps it current automatically.
For manual workflows, add a `post-procure` hook:

```pasta
tools: {
  refresh-compile-db: {
    run:  "now compile-db",
    hook: "post-procure"
  }
}
```

### Generated Files

Generated sources (from the `generate` phase) are included in
`compile_commands.json` after the generate phase has run at least once.
Before that, only hand-written sources appear. `now stay` solves this by
running generate automatically when generator inputs change.

---

## 22.2 Single-File Compile Query

For editors that prefer to query per-file rather than load the whole
compilation database:

```sh
; Emit the full compiler invocation for one file (strict JSON)
now tool:compile-cmd src/main/c/parser.c
now tool:compile-cmd src/main/c/parser.c --output pasta

; Emit just the flags (no tool, no file paths)
now tool:compile-flags src/main/c/parser.c

; Emit include paths only (for jump-to-definition without full build)
now tool:includes src/main/c/parser.c
```

These commands are designed to be called by editor plugins on demand —
fast, no build required, just graph interrogation.

---

## 22.3 `now stay` — The Watch Verb

`stay` is a verb that can be prepended to any `now` command chain. It
transforms a one-shot command into a persistent watch loop: the command
runs once immediately, then re-runs automatically when the relevant inputs
change.

```sh
now stay build                    ; rebuild on source or header change
now stay build test               ; rebuild and retest on source change (TDD mode)
now stay build install            ; rebuild and reinstall on source change
now stay procure build            ; re-procure on lock change, rebuild on source change
now stay generate build           ; re-generate and rebuild on generator input change
now stay build --target linux:*:musl  ; watching fan-out across triples
```

### Trigger Model

Each phase in the chain watches the inputs that determine whether it needs
to re-run — derived directly from the build graph:

| Phase | Watches |
|-------|---------|
| `procure` | `now.pasta`, `now.lock.pasta` |
| `generate` | Generator input files (`.proto`, `.y`, `.l`, etc.) |
| `build` | Source files, headers, tool binaries, flag set |
| `link` | Object files, link flags, dep libraries |
| `test` | Test sources, test binary, test data |
| `install` | Package outputs |

When a file changes, `now stay` determines the minimal set of phases that
need to re-run and re-runs only those — not the full chain from the
beginning. A source file change triggers `build` and downstream phases;
a `now.pasta` change may trigger `procure` and then the full chain.

### Output Behaviour

```
[stay] Watching for changes. Press Ctrl+C to stop.

[14:22:01] src/main/c/parser.c changed
[14:22:01] build :kernel          ...
[14:22:01] COMPILE  parser.c      (0.31s)
[14:22:01] SKIP     writer.c      (up-to-date)
[14:22:01] LINK     kernel        (0.08s)
[14:22:01] build OK  (0.39s)

[14:22:45] src/main/include/types.h changed
[14:22:45] build :kernel          ...
[14:22:45] COMPILE  parser.c      (0.31s)   ; includes types.h
[14:22:45] COMPILE  writer.c      (0.29s)   ; includes types.h
[14:22:45] LINK     kernel        (0.08s)
[14:22:45] build OK  (0.68s)

[14:23:10] now.lock.pasta changed
[14:23:10] procure               ...
[14:23:10] INSTALL  zlib:1.3.1   (new version in lock)
[14:23:10] procure OK
[14:23:10] build :kernel         ...
[14:23:10] COMPILE  parser.c     (0.31s)   ; zlib headers changed
[14:23:10] LINK     kernel       (0.08s)
[14:23:10] build OK  (0.39s)
```

### `now stay` and Platform Fan-Out

With a target wildcard, `now stay` rebuilds only the triples whose inputs
changed. Since different triples have separate output directories and
separate build manifests, a change to a target-specific `#ifdef` block
that only affects `linux:arm64:musl` rebuilds only that triple — the others
are untouched.

### Debounce and Coalescing

Rapid successive changes (saving multiple files within a short window) are
debounced — `now stay` waits for a quiet period (default 50ms, configurable)
before triggering a rebuild. Multiple changes within the debounce window
are coalesced into a single rebuild pass.

```pasta
; ~/.now/config.pasta
{
  stay: {
    debounce_ms:  50,
    max_delay_ms: 500    ; force rebuild even if changes keep coming, after 500ms
  }
}
```

---

## 22.4 `now stay --daemon` — Background Mode

`now stay --daemon` detaches the watcher into a background process. It
performs the same watching and rebuilding as foreground `now stay`, plus
a set of **ancillary maintenance jobs** that only make sense in a
long-running process.

```sh
now stay build install --daemon       ; daemonise
now stay build test --daemon          ; TDD daemon — always-green
now daemon:status                     ; check if a daemon is running for this project
now daemon:stop                       ; graceful shutdown
now daemon:log                        ; tail the daemon log
now daemon:restart                    ; stop and restart with current config
```

### Daemon Lifecycle

```
now stay build --daemon

  Daemon started (PID 18432)
  Socket: /tmp/now-daemon-org.acme.kernel.sock
  Log:    ~/.now/daemon/org.acme.kernel.log
  Run 'now daemon:stop' to stop.
```

The daemon writes a PID file to `~/.now/daemon/{group}.{artifact}.pid`
and a Unix socket (POSIX) or named pipe (Windows) for IPC. `now daemon:*`
commands communicate via the socket — no signals, no polling.

On project root exit or reopen, `now` checks for a running daemon and
reconnects to it rather than starting a new one. If the daemon's PID is
stale (process dead), it cleans up and starts fresh.

### Daemon IPC Protocol

The IPC protocol is Pasta over the Unix socket — consistent with the rest
of `now`'s machine-readable interface:

```pasta
; Client → Daemon: request status
{ cmd: "status" }

; Daemon → Client: response
{
  status:    "idle",
  pid:       18432,
  project:   "org.acme:kernel:2.1.0",
  chain:     ["build", "install"],
  last_run:  { status: "ok", duration_ms: 390, at: "2026-03-05T14:22:01Z" },
  watching:  ["src/", "now.pasta", "now.lock.pasta"],
  jobs:      [
    { id: "dep-expiry-check", next_run: "2026-03-05T15:00:00Z" }
  ]
}
```

### Ancillary Maintenance Jobs

The daemon runs background jobs on configurable schedules:

```pasta
; ~/.now/config.pasta or now.pasta
{
  daemon: {
    jobs: [
      {
        id:       "dep-expiry",
        task:     "procure:check",       ; check for snapshot TTL expiry
        schedule: "every 1h",
        action:   "warn"                 ; warn | auto-refresh
      },
      {
        id:       "lock-drift",
        task:     "lock:check",          ; detect now.pasta / lock inconsistency
        schedule: "on-change:now.pasta",
        action:   "warn"
      },
      {
        id:       "cache-cleanup",
        task:     "cache:gc",            ; remove unreferenced objects from CAC
        schedule: "daily at 02:00",
        action:   "auto"
      },
      {
        id:       "compile-db",
        task:     "compile-db:refresh",  ; keep compile_commands.json current
        schedule: "on-change:now.pasta,now.lock.pasta",
        action:   "auto"
      },
      {
        id:       "speculative-build",
        task:     "build:warm",          ; pre-compile recently touched files
        schedule: "on-idle",             ; runs when no other build is active
        action:   "auto"
      }
    ]
  }
}
```

#### Built-In Job Tasks

| Task | Description |
|------|-------------|
| `procure:check` | Verify lock file consistency, check snapshot TTL |
| `procure:refresh` | Re-run procure for expired snapshots |
| `lock:check` | Detect drift between `now.pasta` and `now.lock.pasta` |
| `cache:gc` | Remove unreferenced entries from the object cache |
| `cache:verify` | Re-verify sha256 of cached archives |
| `compile-db:refresh` | Regenerate `compile_commands.json` |
| `build:warm` | Speculatively compile recently modified files into the CAC |
| `graph:rebuild` | Recompute and cache the compiled build graph |

### Speculative Build

The `build:warm` job is the daemon's most powerful feature. When the
daemon detects you have edited a file but not yet triggered a build, it
speculatively compiles that file into the content-addressed object cache
in the background. When you explicitly run `now build`, the object is
already in the CAC and the build completes near-instantly — only the link
step runs.

This turns `now stay build --daemon` into something close to an always-on
incremental compiler. The compilation happens during your thinking time,
not your waiting time.

### Daemon and Multiple Projects

One daemon per project (identified by `group:artifact`). A workspace with
multiple modules may run one daemon per module or one workspace-level
daemon. The workspace daemon coordinates module rebuild order:

```sh
; From workspace root
now stay build install --daemon    ; one daemon managing all modules in topo order
```

---

## 22.5 `now watch` — Event Emission Mode

Separate from `now stay` (which acts on changes), `now watch` is a pure
event emitter — it watches the project and emits change events on stdout
as Pasta, without triggering any build action. This is for IDE extensions
and other tools that want to manage the build themselves:

```sh
now watch
```

```pasta
{ event: "file-changed", path: "src/main/c/parser.c", at: "2026-03-05T14:22:01Z" }
{ event: "graph-stale",  reason: "source-changed",    affected: ["step:cc:parser.c", "step:link"] }
{ event: "file-changed", path: "src/main/include/types.h", at: "2026-03-05T14:22:45Z" }
{ event: "graph-stale",  reason: "header-changed",    affected: ["step:cc:parser.c", "step:cc:writer.c", "step:link"] }
{ event: "lock-changed", path: "now.lock.pasta",       at: "2026-03-05T14:23:10Z" }
{ event: "graph-stale",  reason: "deps-changed",       affected: ["step:cc:parser.c", "step:link"] }
```

An IDE extension consuming `now watch` knows exactly which graph nodes
are stale without running a build. It can show inline "stale" indicators,
trigger clangd re-indexing, or queue a background build — all driven by
the event stream.

`now watch` is also the daemon's internal event bus, exposed externally.
A daemon with `now watch --daemon` is equivalent to `now stay --daemon`
minus the automatic rebuild actions — just the event emission and
maintenance jobs.

---

## 22.6 Resolving GAP 7

| Item | Resolution |
|------|-----------|
| `compile_commands.json` | `now compile-db` — strict JSON output, arguments array form, workspace-root placement by default |
| Per-file query | `now tool:compile-cmd`, `now tool:compile-flags`, `now tool:includes` |
| File watching | `now stay` — foreground watcher with per-phase trigger model |
| Daemon mode | `now stay --daemon` — background process with ancillary jobs |
| IDE event stream | `now watch` — pure Pasta event emission, no build side effects |
| `compile_commands.json` freshness | Daemon `compile-db:refresh` job + `post-procure` hook option |
| Speculative compilation | Daemon `build:warm` job — compiles during idle time |



---



---

# Chapter 20 — Serialisation: Pasta, JSON, and JSON5

`now` uses a unified data model with three serialisation modes. Pasta is
the authoring format — expressive, human-friendly, comment-supporting.
JSON is the integration format — universally parseable, tooling-compatible,
required by some ecosystem contracts (compile_commands.json, JUnit XML
consumers, CI systems). JSON5 sits between them. All three are first-class
inputs and outputs of `now`.

The goal: zero onboarding friction for developers coming from any
ecosystem. A Cargo user, an npm user, and a Maven user can all write a
`now` project descriptor in the format they already know.

---

## 23.1 The Three Formats

| Format | File extension | Comment char | Unquoted keys | Trailing commas | `now` role |
|--------|---------------|--------------|---------------|-----------------|-----------|
| Pasta  | `.pasta`      | `;`          | ✓             | ✓               | Native authoring format |
| JSON5  | `.json5`      | `//` and `/* */` | ✓         | ✓               | Transition format |
| JSON   | `.json`       | ✗            | ✗             | ✗               | Integration and onboarding format |

All three map to identical in-memory data structures. The data model is
the same — only the surface syntax differs.

---

## 23.2 `now.pasta`, `now.json5`, `now.json`

`now` accepts any of these filenames as the project descriptor. Detection
is by extension:

```
now.pasta    → Pasta parser
now.json5    → JSON5 parser
now.json     → strict JSON parser
```

Only one descriptor file may exist per project directory. If multiple
are present, `now` fails with:

```
Error: ambiguous project descriptor — found now.pasta and now.json
Remove one or run 'now convert' to migrate.
```

### Format Detection Priority

When no descriptor is found under the standard names, `now` checks for
`now.pasta` first, then `now.json5`, then `now.json`. This priority
reflects authoring preference, not correctness — all are equally valid.

---

## 23.3 Structural Equivalence

The same project in all three formats:

### `now.pasta`

```pasta
; org.acme myapp — example project
{
  group:    "org.acme",
  artifact: "myapp",
  version:  "1.0.0",

  langs: ["c"],
  std:   "c11",

  output: { type: "executable", name: "myapp" },

  compile: {
    warnings: ["Wall", "Wextra"],
    defines:  ["NDEBUG"]
  },

  deps: [
    { id: "zlib:zlib:^1.3.0", scope: "compile" }
  ]
}
```

### `now.json5`

```json5
// org.acme myapp — example project
{
  group:    "org.acme",
  artifact: "myapp",
  version:  "1.0.0",

  langs: ["c"],
  std:   "c11",

  output: { type: "executable", name: "myapp" },

  compile: {
    warnings: ["Wall", "Wextra"],
    defines:  ["NDEBUG"],   // trailing comma OK
  },

  deps: [
    { id: "zlib:zlib:^1.3.0", scope: "compile" },
  ],
}
```

### `now.json`

```json
{
  "group":    "org.acme",
  "artifact": "myapp",
  "version":  "1.0.0",

  "langs": ["c"],
  "std":   "c11",

  "output": { "type": "executable", "name": "myapp" },

  "compile": {
    "warnings": ["Wall", "Wextra"],
    "defines":  ["NDEBUG"]
  },

  "deps": [
    { "id": "zlib:zlib:^1.3.0", "scope": "compile" }
  ]
}
```

All three are semantically identical. `now build` behaves the same
regardless of which file is present.

---

## 23.4 `now convert` — Format Migration

```sh
; Convert between formats — output to stdout by default
now convert now.pasta --to json         ; Pasta → strict JSON
now convert now.pasta --to json5        ; Pasta → JSON5
now convert now.json  --to pasta        ; JSON → Pasta
now convert now.json5 --to pasta        ; JSON5 → Pasta

; In-place conversion (replaces the file)
now convert now.pasta --to json --in-place    ; writes now.json, removes now.pasta
now convert now.json  --to pasta --in-place   ; writes now.pasta, removes now.json

; Auto-detect source format
now convert --to pasta           ; converts whatever descriptor exists
```

### Round-Trip Fidelity

| Direction | Fidelity |
|-----------|----------|
| Pasta → JSON | Lossy: comments stripped, keys quoted, trailing commas removed |
| Pasta → JSON5 | Near-lossless: Pasta `;` comments become `//` comments. Field order preserved. |
| JSON → Pasta | Lossless: all JSON is valid Pasta data. No comments to preserve. |
| JSON5 → Pasta | Near-lossless: `//` comments become `;` comments, `/* */` becomes `;` per line. |
| JSON → JSON5 | Lossless |
| JSON5 → JSON | Lossy: comments and trailing commas stripped |

`now convert` warns when a lossy conversion is about to be performed:

```
Warning: converting Pasta → JSON loses comments.
  14 comments found in now.pasta — they will not appear in now.json.
  Consider now.json5 to preserve comments.
Proceed? [y/N]
```

With `--force`, the warning is suppressed.

---

## 23.5 `--output` Flag Across All Commands

Every `now` command that produces structured output accepts `--output`:

```sh
now build --output pasta     ; Pasta (default when piped)
now build --output json5     ; JSON5
now build --output json      ; strict JSON (required for ecosystem consumers)
now build --output text      ; human-readable (default on TTY)
```

Auto-detection: if stdout is a TTY, default is `text`. If stdout is piped
or redirected, default is `pasta`. Override explicitly when needed.

### Fixed-Schema Outputs

Some outputs have a fixed schema required by external consumers and always
emit strict JSON regardless of `--output`:

| Command | Fixed format | Reason |
|---------|-------------|--------|
| `now compile-db` | `compile_commands.json` (JSON) | clangd/LSP ecosystem contract |
| `now test --junit` | JUnit XML | CI system contract |
| `now ci` report | JSON (also writes `.pasta` copy) | CI tooling |

These commands ignore `--output` for their primary output. They may
additionally write a `.pasta` sidecar when `--output pasta` is specified:

```sh
now compile-db --output pasta   ; writes compile_commands.json (JSON, fixed)
                                 ; AND compile_commands.pasta (Pasta, for now tooling)
```

---

## 23.6 JSON Schema for `now.json`

`now` ships a published JSON Schema for `now.json` descriptors. This
enables:
- IDE autocomplete and validation in VSCode, IntelliJ, Neovim + LSP.
- CI pre-validation before running `now`.
- Schema-driven documentation generation.

The schema is published at:
```
https://schema.now.build/now-1.0.json
```

Add to `now.json` for IDE integration:

```json
{
  "$schema": "https://schema.now.build/now-1.0.json",
  "group": "org.acme",
  ...
}
```

The JSON Schema covers all fields defined in documents 01, 01b, 01c, and
their extensions. It is generated from the canonical Pasta schema
definition (doc 31) and kept in sync
automatically.

---

## 23.7 `now.lock.pasta` is Always Pasta

The lock file is always Pasta format, never JSON. Rationale:
- It is machine-written (by `now procure`) and machine-read — not
  hand-edited.
- Comments in the lock file are used by `now` to annotate why specific
  versions were chosen, override reasons, and convergence sources.
- It is committed to VCS and reviewed by humans — Pasta readability
  (no quote noise) matters.
- No external tool needs to consume the lock file directly — if they do,
  `now convert now.lock.pasta --to json` produces a machine-readable form.

---

## 23.8 Pasta Language Features Used in `now`

The Pasta reference implementation (v0.1+) defines two features that `now`
relies on in its descriptors and output. This section documents how `now`
uses them.

### 23.8.1 Multiline Strings

Triple-quoted strings (`"""..."""`) allow multi-line values without escape
sequences. Everything between the opening `"""` and closing `"""` is taken
verbatim, including newlines, indentation, and regular `"` characters.

`now` uses multiline strings in two contexts:

**Tool and script bodies** — inline scripts in `tools` blocks no longer
require awkward single-line concatenation:

```pasta
{
  tools: [
    {
      name: "gen-version",
      type: "script",
      command: """
#!/bin/sh
git describe --tags --always --dirty > target/generated/version.h
echo "#define BUILD_DATE \"$(date -u +%Y-%m-%d)\"" >> target/generated/version.h
"""
    }
  ]
}
```

**Linker scripts embedded in descriptors** — `link.script_body` (an
extension to doc 27) accepts a multiline string for small, self-contained
linker scripts:

```pasta
{
  link: {
    script_body: """
MEMORY {
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}
SECTIONS {
  .text : { *(.text*) } > FLASH
  .data : { *(.data*) } > RAM AT > FLASH
  .bss  : { *(.bss*)  } > RAM
}
"""
  }
}
```

The Pasta writer automatically selects triple-quote form for any string
containing a newline character, so `now`-generated output (lock files,
`now convert` output) uses multiline strings naturally where appropriate.

**No escape sequences** — multiline strings have no escape mechanism.
The only value that cannot appear inside a multiline string is `"""`.
If a script body must contain `"""`, split it using an external file
and reference via `link.script` instead.

### 23.8.2 Sections (`@name`)

Pasta sections (`@name { ... }` or `@name [ ... ]`) allow a document
with multiple named root containers. A sectioned Pasta document parses
to a map with section names as keys.

`now` uses sections in two output contexts:

**Multi-module workspace descriptors** — `now-workspace.pasta` may use
sections to separate global defaults from per-module overrides:

```pasta
@workspace {
  modules: ["kernel", "drivers", "apps"]
}

@inherit_defaults {
  compile: {
    warnings: ["Wall", "Wextra", "Wpedantic"],
    defines:  ["NDEBUG"]
  }
}
```

**Machine output for multi-phase commands** — `now ci --output pasta`
produces a sectioned document, one section per lifecycle phase:

```pasta
@procure {
  status: "ok",
  elapsed_ms: 240
}

@build {
  status: "ok",
  files_compiled: 47,
  elapsed_ms: 3820
}

@test {
  status: "ok",
  passed: 112,
  failed: 0,
  elapsed_ms: 580
}
```

**Rules for `now` section documents:**
- If a document uses `@` sections, all root containers must be named
  (this is a Pasta grammar rule, not a `now` extension).
- `now convert` preserves sections when converting from `.pasta` to
  `.json5` or `.json` — JSON output uses a regular object with the
  section names as top-level keys.
- The daemon IPC protocol (doc 22 §22.5) uses sections to frame
  multi-part responses; each section is a logical message.
- `now.pasta` project descriptors do **not** use sections — the root
  is always a single flat map. Sections are for `now-workspace.pasta`
  and `now` output only.

---

## 23.9 Pasta as the Canonical Internal Format

`now`'s internal data model uses Pasta as its canonical representation.
When `now` reads a `now.json` or `now.json5`, it parses it into the same
in-memory structure as a `now.pasta`. No information is lost going JSON
→ internal model. The model is then available in all output formats.

This means:
- All `now tool:query` commands produce Pasta by default.
- The IPC protocol (daemon socket, plugin stdin/stdout) uses Pasta.
- Build graph nodes are serialised as Pasta.
- Error messages embed Pasta snippets showing the relevant config.

JSON is a surface format for input and for ecosystem integration output.
Pasta is the lingua franca of `now` internals.

---

## 23.10 HTTP Content Negotiation — `application/x-pasta`

### MIME Type Registration

| Field | Value |
|-------|-------|
| Type | `application/x-pasta` |
| Charset | `US-ASCII` |
| Extension | `.pasta` |

Pasta is strict ASCII by design. The grammar's `stringchar` production
is fully enumerated from ASCII printable characters; no byte above 0x7F
is valid in a Pasta document. A conforming parser must reject any input
containing bytes outside the US-ASCII range. Claiming UTF-8 would
overstate the format's scope — `charset=US-ASCII` is the precise and
correct declaration.

### Content Negotiation

Registry endpoints that return structured data (e.g. `GET /resolve`)
support Pasta responses in addition to JSON. The `now` client prefers
Pasta to eliminate a JSON parsing dependency.

**Request:** The client signals preference via the `Accept` header. No
`charset` parameter is needed on `Accept`:

```
Accept: application/x-pasta, application/json;q=0.9
```

**Response:** The server carries the actual content type in
`Content-Type`. Pasta responses must include `charset=US-ASCII`
explicitly:

```
Content-Type: application/x-pasta; charset=US-ASCII
```

JSON responses carry `charset=UTF-8`:

```
Content-Type: application/json; charset=UTF-8
```

### Conformance Requirements

A conforming registry **MUST** support `application/x-pasta` responses.
JSON (`application/json`) is the mandatory fallback when a client does
not send an `Accept` header.

Pasta and JSON responses for the same endpoint must be structurally
identical — same fields, same values, same semantics. A conforming
registry must not add fields present only in one format or omit fields
from one format that appear in the other.

### Equivalence Guarantee

The unified data model (§23.1) ensures round-trip fidelity. An
`application/x-pasta` response and its `application/json` equivalent
are semantically identical; format choice is a surface concern only.



---



---

# Chapter 21 — Dependency Confusion Protection

---

## 25.1 The Attack

Dependency confusion (also called namespace confusion or substitution
attack) works as follows:

1. An organisation uses a private registry for internal packages under
   a group namespace: `org.acme:core`, `org.acme:hal`, `org.acme:net`.
2. An attacker discovers these package names (from a leaked `now.pasta`,
   a public build log, or enumeration).
3. The attacker publishes packages with the same coordinates to the
   public registry — but at a higher version: `org.acme:core:999.0.0`.
4. If `now` checks the public registry and the convergence policy is
   `highest`, the public malicious package wins over the private
   legitimate one.
5. The malicious package executes arbitrary code during the build.

The attack is realistic. It has been demonstrated repeatedly against npm,
pip, and Maven ecosystems. It requires no credentials and no access to
the private registry — only knowledge of the package names.

---

## 25.2 `private_groups` — The Fence

`private_groups` declares a set of group prefixes that `now` will never
resolve from public registries, regardless of version, regardless of
convergence policy.

```pasta
; ~/.now/config.pasta — machine-level policy
{
  private_groups: [
    "org.acme",          ; exact group match
    "com.nova",          ; Nova OS internal packages
    "nova.os",
    "nova.grid",
    "amiga.os"           ; Amiga platform packages (not on public registry)
  ]
}
```

Or in `now.pasta` for project-level enforcement:

```pasta
; now.pasta
{
  resolve: {
    private_groups: [
      "org.acme",
      "com.nova"
    ]
  }
}
```

Both levels are checked. A group that appears in either the machine-level
or project-level `private_groups` list is always treated as private.

### What Private Group Resolution Means

When `now procure` encounters a dep whose group matches a private group
prefix:

1. Only repos declared before any public registry in `repos:` are tried.
2. The public `now` central registry is never consulted for this group,
   regardless of its position in `repos:`.
3. If the dep is not found in any private repo, `now` fails with:

```
Error: org.acme:core:^4.0.0 is in a private group (org.acme)
  and was not found in any private registry.

  Private registries configured:
    https://pkg.acme.internal/now   (tried — not found)

  Public registry was NOT consulted (private_groups policy).

  Options:
    1. Publish org.acme:core to your private registry.
    2. Remove "org.acme" from private_groups if this package
       should be resolved from the public registry.
```

This error is always a hard failure — there is no fallback, no warning,
no "try public anyway" escape hatch. The fence is absolute.

---

## 25.3 Prefix Matching Semantics

`private_groups` entries match by prefix on the dotted group name:

| Entry | Matches | Does not match |
|-------|---------|----------------|
| `"org.acme"` | `org.acme`, `org.acme.internal`, `org.acme.labs` | `org.acmecorp`, `com.acme` |
| `"nova"` | `nova`, `nova.os`, `nova.grid`, `nova.kernel` | `anova`, `com.nova` |
| `"com.nova"` | `com.nova`, `com.nova.hal`, `com.nova.grid` | `nova`, `org.nova` |

The matching rule: entry `E` matches group `G` if `G == E` or
`G` starts with `E + "."`. This prevents `org.acme` from accidentally
matching `org.acmecorp`.

---

## 25.4 Interaction with the Trust Store

`private_groups` and the signing trust store (doc 20) are complementary
defences at different layers:

| Layer | Mechanism | Stops |
|-------|-----------|-------|
| Resolution | `private_groups` | Package ever being fetched from wrong registry |
| Integrity | SHA-256 in lock file | Tampered archive after download |
| Identity | Publisher signing + trust store | Wrong publisher for a known-good registry |

For maximum security, combine all three:

```pasta
; ~/.now/config.pasta
{
  private_groups: ["org.acme", "com.nova"],

  trust: {
    require_signatures: true,
    require_known_keys: true
  }
}
```

With this configuration:
- `org.acme` packages are never fetched from public registries.
- Every package must be signed.
- Every signature must match a key in `~/.now/trust.pasta`.

An attacker who somehow bypasses the registry fence still fails at the
signature check — they cannot forge a signature for the legitimate
publisher's key.

---

## 25.5 `now resolve:check` — Audit Command

```sh
now resolve:check
```

Audits the current project's dep graph against the `private_groups` policy
and the trust store, without running a build:

```
Dependency resolution audit:

  org.acme:core:4.2.1       PRIVATE  resolved from pkg.acme.internal  ✓ signed
  org.acme:hal:2.1.0        PRIVATE  resolved from pkg.acme.internal  ✓ signed
  zlib:zlib:1.3.0           PUBLIC   resolved from registry.now.build  ✓ signed
  com.nova:kernel:0.9.0     PRIVATE  resolved from nova.internal       ✓ signed
  org.unknown:thing:1.0.0   PUBLIC   no private_groups match           ✗ unsigned
    WARNING: unsigned public package

  Policy: private_groups enforced, signatures required
  Result: 1 warning (unsigned public package)
```



---



---

# Chapter 22 — Glob Pattern Dialect

All glob fields in `now.pasta` use this dialect unless explicitly noted.

---

## 26.1 Base: gitglob Semantics

`now` adopts the gitglob pattern dialect as defined by the Git project
and widely implemented across the ecosystem (`.gitignore`, `glob` in
Cargo, `include`/`exclude` in many build tools). The core rules:

| Pattern | Meaning |
|---------|---------|
| `*` | Any sequence of characters except `/` |
| `**` | Any sequence of characters including `/` (recursive) |
| `?` | Any single character except `/` |
| `[abc]` | Character class — any one of `a`, `b`, `c` |
| `[a-z]` | Character range |
| `[!abc]` | Negated character class |
| `\` | Escape the next character literally |

A pattern without a `/` matches only the filename component (final
path segment). A pattern containing a `/` is matched against the full
path relative to the base directory.

---

## 26.2 Base Path Semantics

In `now`, all glob patterns are rooted at the `dir` field of the
enclosing `sources`, `tests`, or `assembly.include` block — not at
the project root. This is the first explicit deviation from bare gitglob,
which does not define a base path concept.

```pasta
sources: {
  dir:     "src/main/c",
  pattern: "**.c"        ; matches src/main/c/foo.c, src/main/c/sub/bar.c
                         ; does NOT match src/test/c/foo.c
}
```

Patterns in `assembly.include` `src:` fields are rooted at the project
root, not a `dir` field, because they reference files from anywhere in
the project tree.

Absolute paths (beginning with `/` or `~/`) are never glob-expanded —
they are treated as literal file references. If the file does not exist,
it is an error.

---

## 26.3 `**` Behaviour

`**` matches zero or more path segments, including zero:

| Pattern | Matches | Does not match |
|---------|---------|----------------|
| `**.c` | `foo.c`, `sub/foo.c`, `a/b/c/foo.c` | `foo.h` |
| `**/foo.c` | `foo.c`, `sub/foo.c`, `a/b/foo.c` | `foo.h` |
| `src/**/foo.c` | `src/foo.c`, `src/a/foo.c`, `src/a/b/foo.c` | `other/foo.c` |
| `**` | Every file in the tree | *(nothing excluded)* |

`**` in the middle of a pattern (`src/**/test/*.c`) matches zero or more
intermediate directories.

---

## 26.4 Deviation 1: Double-Extension Matching

`now` generates object files with double extensions: `parser.c.o`,
`startup.s.o`, `screen.asm.o`. These must be expressible in glob patterns,
particularly in assembly and include directives.

gitglob has no special treatment of dots in extensions — `*.c.o` simply
means "any filename ending in `.c.o`", which is correct. `now` makes this
explicit: **dots in extension patterns are literal, not special**. Multiple
dots in a pattern segment are valid and match literally.

```pasta
; Explicit: matches only .c.o object files (not .s.o or .asm.o)
exclude: ["target/obj/**.c.o"]

; Matches any object file regardless of source extension
exclude: ["target/obj/**/*.o"]
```

This is not a behavioural change from gitglob — it is a clarification
that `now` explicitly supports and documents this usage.

---

## 26.5 Deviation 2: Symlink Crossing

gitglob implementations vary on whether `**` crosses symlinks. `now`
takes an explicit position: **`**` does not cross symlinks by default**.

This is the safe default for build tools. A symlink cycle would otherwise
cause infinite traversal; and symlinked subtrees in build tools often
represent vendored or shared code that should be referenced explicitly,
not swept up by a glob.

To opt into symlink crossing for a specific pattern:

```pasta
sources: {
  dir:     "src",
  pattern: "**.c",
  follow_symlinks: true    ; ** crosses symlinks for this source set
}
```

`follow_symlinks` applies to the entire `sources` block, not to individual
patterns. Cycle detection is always active when `follow_symlinks: true` —
`now` tracks visited real paths and stops at a revisited directory.

---

## 26.6 Deviation 3: Case Sensitivity

gitglob on case-insensitive filesystems (macOS HFS+, Windows NTFS) is
implementation-defined. `now` takes an explicit position: **glob matching
is always case-sensitive, regardless of filesystem**.

This ensures consistent behaviour across platforms. A pattern `**.C`
matches files named `foo.C` but not `foo.c`, even on macOS.

On case-insensitive filesystems where two files would collide after
normalisation (e.g. `Foo.c` and `foo.c` in the same directory), `now`
emits an error at source scan time rather than silently including one
or both:

```
Error: case collision in source set:
  src/main/c/Foo.c
  src/main/c/foo.c
  These are distinct files but would produce the same object file
  on case-insensitive filesystems. Rename one.
```

---

## 26.7 Negation Patterns

Patterns prefixed with `!` in an array negate previously matched files.
This is the gitignore negation model, applied to `now`'s `pattern` arrays:

```pasta
sources: {
  dir:     "src",
  pattern: [
    "**.c",           ; include all .c files
    "!**_test.c",     ; exclude test files (handled separately)
    "!**/bench/*.c"   ; exclude benchmark files
  ]
}
```

Negation patterns are evaluated in order: a file matched by `**.c` and
then matched by `!**_test.c` is excluded. A file subsequently matched by
another positive pattern is re-included. This is identical to gitignore
semantics.

Negation in `pattern` arrays is a convenience shorthand. It is equivalent
to using `exclude:` with the same patterns (without the `!` prefix).
The two forms may be mixed — `pattern` negations are processed first,
then `exclude` patterns are applied.

---

## 26.8 Complete Pattern Reference

| Form | Example | Meaning |
|------|---------|---------|
| Bare name | `foo.c` | Exact filename in any subdirectory |
| Extension glob | `*.c` | Any `.c` file in the immediate directory |
| Recursive extension | `**.c` | Any `.c` file in any subdirectory |
| Path prefix | `src/main/*.c` | Any `.c` file directly in `src/main/` |
| Recursive path | `src/**/foo.c` | `foo.c` anywhere under `src/` |
| Double extension | `**.c.o` | Any `.c.o` file (object files) |
| Character class | `*.[ch]` | Any `.c` or `.h` file |
| Question mark | `foo?.c` | `foo1.c`, `fooX.c`, not `foo.c` |
| Negation | `!**_test.c` | Exclude files ending in `_test.c` |
| Literal dot | `**.min.js` | Any `.min.js` file |

---

## 26.9 Fields That Accept Globs

| Field | Base path | Notes |
|-------|-----------|-------|
| `sources.pattern` | `sources.dir` | Primary source selection |
| `sources.exclude` | `sources.dir` | Post-selection exclusion |
| `sources.include` | project root | Explicit additions — no glob expansion, literal paths only |
| `tests.pattern` | `tests.dir` | Test source selection |
| `assembly.include[].src` | project root | Assembly include source paths |
| `assembly.include[].exclude` | project root | Assembly include exclusions |
| `sources.generators[].pattern` | project root | Generator input files |
| `langs[].types[].pattern` | N/A | File type classification — matched against full relative path from source root |
| `private_groups` | N/A | Not a file glob — dotted prefix matching (doc 25) |



---



---

# Chapter 23 — Embedded and Freestanding Platforms

> **Scope note — Post-v1 platforms.** The Nova and Amiga platform targets
> are fully specified but are **not required for v1**. Focus on the Linux,
> macOS, and Windows triples first. Exotic targets can be enabled
> progressively once the cross-compilation infrastructure (§10) is solid.


Two platforms anchor the examples throughout: the **Amiga** (m68k, Hunk
format, AmigaOS) as the accessible tinkerer case, and **Nova** (RISC-V,
hexascale grid OS, Amiga-derivative architecture) as the advanced
production case. Both share the same `now` model — the only difference
is scale and complexity.

---

## 27.1 The Freestanding Triple

A freestanding target has no OS kernel, no libc, and no startup runtime
that `now` provides. The program is entirely responsible for its own
environment. This is expressed in the platform triple as:

```
freestanding:{arch}:none
```

| Triple | Platform |
|--------|---------|
| `freestanding:m68k:none` | Bare Amiga hardware (no AmigaOS) |
| `freestanding:arm32:none` | ARM Cortex-M microcontroller |
| `freestanding:riscv64:none` | Bare RISC-V 64-bit (Nova node, pre-OS) |
| `freestanding:riscv32:none` | Embedded RISC-V 32-bit |
| `freestanding:aarch64:none` | ARM64 bare metal |

OS-targeted triples use the OS name:

| Triple | Platform |
|--------|---------|
| `amigaos:m68k:none` | AmigaOS 3.x on Amiga hardware |
| `nova:riscv64:none` | Nova OS on RISC-V grid node |
| `nova:riscv64:smp` | Nova OS SMP variant |
| `nova:riscv64:grid` | Nova grid-aware runtime variant |

`now` does not need to know these triples in advance. They are declared
in the project and toolchain configuration. The triple system is open —
any `os:arch:variant` is valid if a toolchain can be found for it.

---

## 27.2 Declaring a New Platform

When `now` encounters a triple it has no built-in knowledge of, it
falls back entirely to project and global configuration. No built-in
defaults apply. This is the bootstrap path for new platforms.

```pasta
; ~/.now/config.pasta — Nova platform declaration
{
  platforms: {
    "nova:riscv64:none": {
      ; Toolchain
      cc:     "riscv64-nova-gcc",
      cxx:    "riscv64-nova-g++",
      as:     "riscv64-nova-as",
      ld:     "riscv64-nova-ld",
      ar:     "riscv64-nova-ar",
      objcopy: "riscv64-nova-objcopy",

      ; Platform compile flags always applied
      compile_flags: [
        "-march=rv64imafdc",
        "-mabi=lp64d",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector"
      ],

      ; Platform link flags always applied
      link_flags: [
        "-nostdlib",
        "-nostartfiles",
        "-static"
      ],

      ; Default output format for this platform
      output_format: "nova-elf",   ; registered format (see §27.5)

      ; Platform-specific defines injected automatically
      defines: ["__NOVA__", "__RISCV__", "__LP64__"]
    },

    "amigaos:m68k:none": {
      cc:      "m68k-amigaos-gcc",
      as:      "m68k-amigaos-as",
      ld:      "m68k-amigaos-ld",
      ar:      "m68k-amigaos-ar",
      objcopy: "m68k-amigaos-objcopy",

      compile_flags: [
        "-m68060",
        "-msoft-float",
        "-fno-builtin"
      ],

      link_flags: ["-nostdlib"],
      output_format: "amiga-hunk",
      defines: ["__AMIGA__", "__M68K__", "AMIGAOS"]
    }
  }
}
```

---

## 27.3 Linker Scripts

Linker scripts control memory layout — where code, data, BSS, and stack
land in the address space. They are essential for bare-metal targets and
custom OS kernels.

### Declaration

```pasta
; now.pasta — linker script for a freestanding target
{
  link: {
    script: "ld/nova-node.ld",          ; single script

    ; OR: per-triple script selection
    scripts: {
      "nova:riscv64:none":  "ld/nova-riscv64.ld",
      "nova:riscv64:smp":   "ld/nova-riscv64-smp.ld",
      "nova:riscv64:grid":  "ld/nova-riscv64-grid.ld",
      "freestanding:*:none": "ld/generic-freestanding.ld"
    }
  }
}
```

Wildcard matching in `scripts` keys follows the same triple-wildcard
rules as `toolchains` in doc 11 — the first matching key wins.

### Linker Script Variables

`now` makes project properties available as linker script symbols via
`--defsym` (GCC/Clang) or equivalent. This allows the linker script to
reference values from `now.pasta` without hardcoding them:

```pasta
; now.pasta
properties: {
  FLASH_ORIGIN: "0x20000000",
  FLASH_LENGTH: "0x00100000",   ; 1MB
  RAM_ORIGIN:   "0x80000000",
  RAM_LENGTH:   "0x00040000"    ; 256KB
}

link: {
  script: "ld/nova-node.ld",
  script_symbols: true    ; expose properties as --defsym NAME=VALUE
}
```

```ld
/* ld/nova-node.ld */
MEMORY {
  FLASH (rx)  : ORIGIN = FLASH_ORIGIN, LENGTH = FLASH_LENGTH
  RAM   (rwx) : ORIGIN = RAM_ORIGIN,   LENGTH = RAM_LENGTH
}
```

The `now` invocation becomes:
```sh
riscv64-nova-ld --defsym FLASH_ORIGIN=0x20000000 \
                --defsym FLASH_LENGTH=0x00100000  \
                --defsym RAM_ORIGIN=0x80000000    \
                --defsym RAM_LENGTH=0x00040000    \
                -T ld/nova-node.ld ...
```

### Memory Map Output

```pasta
link: {
  script:     "ld/nova-node.ld",
  map_output: "target/${triple}/nova-node.map"   ; linker map file
}
```

`now` passes `-Map ${map_output}` (GCC/Clang ld) or equivalent. The map
file is preserved in `target/` for analysis. `now link:map` provides
a human-readable summary:

```sh
now link:map --target nova:riscv64:none
```

```
Memory map: nova-node (nova:riscv64:none)

  Region   Used      Total     Free      %
  FLASH    87,432    1,048,576 961,144   8.3%
  RAM      12,288    262,144   249,856   4.7%

  Largest free FLASH block: 961,144 bytes
  Largest free RAM block:   249,856 bytes

  Sections:
    .text     FLASH  0x20000000  84,320 bytes
    .rodata   FLASH  0x20014960   3,112 bytes
    .data     RAM    0x80000000     892 bytes
    .bss      RAM    0x80000380  11,396 bytes
    .stack    RAM    0x80002D04   (not reserved — grows down)
```

### Memory Budget Enforcement

```pasta
link: {
  script: "ld/nova-node.ld",
  budget: {
    FLASH: { max: "90%",    error: true  },   ; fail build if >90% full
    RAM:   { max: "512KB",  error: false }    ; warn only
  }
}
```

`now` reads the linker map after link and checks budgets. Over-budget
with `error: true` fails the build before post-link hooks run.

---

## 27.4 Post-Link Binary Conversion

After linking produces an ELF (or equivalent), embedded targets often
need the binary converted to a different format for flashing, packaging,
or platform-specific loading.

### Declaration

```pasta
link: {
  script:  "ld/nova-node.ld",

  convert: [
    {
      ; Raw binary for flash programmer
      tool:   "objcopy",
      args:   ["-O", "binary", "${input}", "${output}"],
      output: "target/${triple}/nova-node.bin"
    },
    {
      ; Intel HEX for some flash tools
      tool:   "objcopy",
      args:   ["-O", "ihex", "${input}", "${output}"],
      output: "target/${triple}/nova-node.hex"
    },
    {
      ; Motorola S-record
      tool:   "srec_cat",
      args:   ["${input}", "-ELF", "-o", "${output}", "-Motorola"],
      output: "target/${triple}/nova-node.srec"
    }
  ]
}
```

`${input}` is the linked ELF/binary from the link step. `${output}` is
the declared output path. Conversions run in declaration order after
the link step, before any `post-link` hooks.

### Built-In Conversion Shortcuts

Common conversions have shorthand forms:

```pasta
link: {
  convert: [
    { format: "binary" },       ; objcopy -O binary
    { format: "ihex"   },       ; objcopy -O ihex
    { format: "srec"   },       ; srec_cat -Motorola
    { format: "uf2",            ; UF2 for RP2040 and similar
      family_id: "0xe48bff56"   ; RISC-V UF2 family
    }
  ]
}
```

Output filenames for shorthand forms use the artifact name with the
format's conventional extension: `nova-node.bin`, `nova-node.hex`, etc.

---

## 27.5 Custom Output Formats

Some platforms have proprietary executable formats that `objcopy` cannot
produce. These are handled as post-link plugins.

### Amiga Hunk Format

The Amiga uses the Hunk format — a segmented executable format predating
ELF. GCC targeting AmigaOS (via the `m68k-amigaos` toolchain) can produce
Hunk directly, but the format may need post-processing for specific Amiga
features (resident programs, Kickstart version tags, memory type hints).

```pasta
; now.pasta for an Amiga program
{
  group:    "amiga.demo",
  artifact: "bouncing-ball",
  version:  "1.0.0",

  langs:    ["c", "m68k-asm"],
  std:      "c11",

  targets:  ["amigaos:m68k:none"],

  output: { type: "executable", name: "BallDemo" },

  link: {
    scripts: { "amigaos:m68k:none": "ld/amiga.ld" },
    flags:   ["-Wl,--traditional-format"]
  },

  plugins: [
    {
      ; Post-process the Hunk binary: add version tag, set memory hints
      id:    "amiga.tools:hunk-postproc:1.0.0",
      type:  "plugin",
      phase: "post-link",
      when:  "amigaos:m68k:none",
      config: {
        kickstart_min: "3.1",
        chip_mem_only: false,
        version_tag:   "${now.version}"
      }
    }
  ],

  assembly: [
    {
      id:     "amiga-lha",
      format: "lha",
      include: [
        { src: "target/amigaos-m68k-none/bin/BallDemo", dest: "C/", mode: "0755" },
        { src: "assets/BallDemo.info",                  dest: "C/"               },
        { src: "README",                                 dest: "/"                }
      ]
    }
  ]
}
```

### Nova Executable Format

Nova uses an extended ELF format with Nova-specific section types for
grid capability declarations, memory topology hints, and inter-node
communication descriptors. The toolchain produces standard ELF; a
post-link plugin adds Nova-specific sections.

```pasta
; now.pasta for a Nova OS component
{
  group:    "nova.os",
  artifact: "nova-scheduler",
  version:  "0.1.0-incubating",

  volatile: true,    ; scheduler design still in flux

  langs:    ["c", "asm-gas"],
  std:      "c11",

  targets: [
    "nova:riscv64:none",
    "nova:riscv64:smp",
    "nova:riscv64:grid"
  ],

  output: { type: "executable", name: "nova-scheduler" },

  link: {
    scripts: {
      "nova:riscv64:none":  "ld/nova-single.ld",
      "nova:riscv64:smp":   "ld/nova-smp.ld",
      "nova:riscv64:grid":  "ld/nova-grid.ld"
    },
    map_output: "target/${triple}/nova-scheduler.map",
    budget: {
      TEXT: { max: "80%", error: true }
    }
  },

  plugins: [
    {
      ; Add Nova grid capability descriptors to ELF output
      id:    "nova.tools:nova-elf-annotate:0.1.0",
      type:  "plugin",
      phase: "post-link",
      config: {
        capabilities: ["scheduler", "ipc", "memory-manager"],
        grid_topology: "any",
        min_nodes:     1
      }
    },
    {
      ; Nova signing — required for grid deployment
      id:    "nova.tools:nova-sign:0.1.0",
      type:  "plugin",
      phase: "post-link",
      concurrency: { mode: "exclusive", scope: "host", lock_id: "nova-hsm" },
      config: {
        key:    "${env.NOVA_SIGNING_KEY}",
        policy: "grid-deployment"
      }
    }
  ]
}
```

---

## 27.6 Bootstrapping a Greenfield Platform

A university course, a research project, or a hardware startup may need
to build for a platform `now` has never seen. The bootstrap sequence:

### Step 1: Declare the Triple and Toolchain

```pasta
; ~/.now/config.pasta
{
  platforms: {
    "myos:myriscv:none": {
      cc:  "/opt/myriscv-toolchain/bin/myriscv-gcc",
      as:  "/opt/myriscv-toolchain/bin/myriscv-as",
      ld:  "/opt/myriscv-toolchain/bin/myriscv-ld",
      ar:  "/opt/myriscv-toolchain/bin/myriscv-ar",

      compile_flags: ["-march=rv32i", "-mabi=ilp32", "-ffreestanding"],
      link_flags:    ["-nostdlib", "-static"],
      defines:       ["__MYRISCV__"]
    }
  }
}
```

### Step 2: Sketch the Project

```sh
now sketch --group edu.myuni --artifact myos-hello --version 0.1.0 \
           --target myos:myriscv:none c asm-gas
```

Creates:
```
myos-hello/
├── now.pasta
├── .gitignore
└── src/main/
    ├── c/
    │   └── main.c
    ├── h/
    ├── s/
    │   └── startup.s
    └── i/
```

### Step 3: Add a Linker Script

```pasta
; now.pasta (generated, then edited)
{
  group:    "edu.myuni",
  artifact: "myos-hello",
  version:  "0.1.0",

  langs:    ["c", "asm-gas"],
  targets:  ["myos:myriscv:none"],

  output: { type: "executable", name: "hello" },

  link: {
    script: "ld/myos.ld",
    convert: [{ format: "binary" }]
  }
}
```

### Step 4: Build

```sh
now build --target myos:myriscv:none
```

`now` uses the declared toolchain, applies the platform flags, runs the
linker with the script, and converts to binary. No other configuration.

### Step 5: Register the Platform for the Community

When the platform matures, its toolchain and platform declaration can be
packaged as a `now` artifact and published:

```pasta
; myos-toolchain/now.pasta
{
  group:    "edu.myuni",
  artifact: "myos-toolchain-descriptor",
  version:  "0.1.0",

  output: { type: "platform-descriptor" },   ; new output type — see below
}
```

A `platform-descriptor` artifact, when procured, installs a
`platform.pasta` to `~/.now/platforms/` that `now` loads at startup —
making the platform available to any project without manual
`~/.now/config.pasta` editing:

```sh
; A new project consuming the platform descriptor
now procure edu.myuni:myos-toolchain-descriptor:^0.1.0
now sketch --target myos:myriscv:none c
now build
```

This is the path from greenfield experiment to shareable platform —
the same path Nova will take as it matures from volatile incubation to
a stable published platform.

---

## 27.7 Nova: The Primary Advanced Target

Nova is an Amiga-derivative OS targeting the hexascale RISC-V grid. It
is designed in parallel with `now` — `now` is Nova's build tool from day
one, and Nova's requirements have shaped `now`'s design throughout this
specification.

Key Nova-specific features that `now` supports:

| Nova requirement | `now` feature |
|-----------------|---------------|
| Multiple grid node variants | Platform triple variants: `nova:riscv64:none/smp/grid` |
| Per-node memory topology | Per-triple linker scripts + `script_symbols` |
| Grid capability declarations | Post-link ELF annotation plugin |
| Hardware signing for deployment | `exclusive/host` plugin with HSM `lock_id` |
| Volatile incubating components | `volatile: true` on modules and workspace |
| Hexascale parallel builds | Build graph + content-addressed object cache |
| Amiga API heritage | Same `h/`, `lib/`, `asm/` installed layout conventions |
| Multi-arch SDK distribution | Fat archive assembly descriptor |

Nova's `now.pasta` workspace root, when it matures, will be one of the
most complex descriptors in the ecosystem. The specification has been
designed so that complexity is expressible without escaping the model —
no shell scripts, no Makefiles, no bespoke tooling glue. Just `now.pasta`
all the way down.

---

## 27.8 The Amiga as Tinkerer Entry Point

The Amiga occupies a specific role in the `now` ecosystem: it is the
most feature-rich "unconsolidated" platform that tinkerers actually use.
It has:

- A custom executable format (Hunk) — tests the output format system
- A hand-written assembly tradition — tests the m68k-asm language type
- A custom packaging format (LHA) — tests the assembly descriptor
- Platform-specific signing (hardware dongle, Kickstart version tags) — tests the trust model
- A rich library ecosystem (intuition, graphics, exec) — tests the installed layout

A tinkerer who can build and package an Amiga program with `now` has
exercised every major system in the specification. That is intentional.

The Amiga example in §27.5 is complete and buildable. It is the
specification's proof of concept for the embedded and custom platform
story — and the lineage that Nova inherits and extends to grid scale.



---



---

# Chapter 24 — Reproducible Builds and Security Advisory Integration

---

## 26.1 What Reproducible Builds Mean in `now`

A **reproducible build** is one where identical inputs produce
bit-identical outputs regardless of when or where the build runs. "Inputs"
means: source files, dep versions, compiler version, flags, linker script.
"When or where" means: build machine, build user, build timestamp, absolute
paths, environment variables.

`now` targets **input reproducibility** as the primary guarantee: given
the same `now.lock.pasta` and source tree, every build produces the same
inputs to the compiler. **Output reproducibility** (bit-identical binaries)
additionally requires the compiler itself to be deterministic, which `now`
can encourage but not enforce.

```pasta
; now.pasta
{
  reproducible: true    ; enable all reproducibility measures
}
```

When `reproducible: true`, `now` activates the measures in §26.3.

---

## 26.2 Timebase — Declarative Timestamp Control

Timestamps embedded in build outputs (archive mtimes, `__DATE__` / `__TIME__`
macros, debug info) are the most common source of non-reproducibility.
`now` controls them through the `timebase` property.

### Declaration

```pasta
; now.pasta
{
  reproducible: true,

  timebase: "git-commit"    ; derive from HEAD commit timestamp
}
```

### Timebase Values

| Value | Meaning | Use case |
|-------|---------|---------|
| `"now"` | Current wall clock time | Default. Non-reproducible. Development builds. |
| `"git-commit"` | Timestamp of HEAD git commit (`git log -1 --format=%ct`) | Release builds. Same commit → same timestamp. |
| `"zero"` | Unix epoch: `1970-01-01T00:00:00Z` | Maximum reproducibility. Binary comparison across repos. |
| `"YYYY-MM-DDTHH:MM:SSZ"` | Absolute ISO 8601 timestamp | Pinned release date. Audit trail. |
| `"${property}"` | Value of a declared property | Flexible — set from CI environment or parent POM. |

### CLI Override

```sh
; Override timebase for this invocation only
now build --timebase 2026-03-05T00:00:00Z
now build --timebase git-commit
now build --timebase zero

; In CI — pin to the commit timestamp
now build --timebase "$(git log -1 --format=%cI)"
```

CLI `--timebase` overrides the `now.pasta` value. The active timebase
is recorded in the build manifest for auditability.

### How Timebase is Applied

The resolved timebase timestamp is applied consistently across all
outputs:

| Output | How applied |
|--------|-------------|
| `tar` archives | `--mtime` flag sets all member timestamps |
| `zip` archives | Entry timestamps set to timebase |
| Object files | Not directly — compiler flags control embedded timestamps |
| Debug info (`__DATE__`, `__TIME__`) | Injected as defines (see §26.3) |
| Linker map file | File mtime set to timebase |
| Build manifest | Records the timebase used |

### `git-commit` Timebase in Practice

```sh
; Developer build — uses wall clock (non-reproducible, fast feedback)
now build

; Release build — uses commit timestamp (reproducible)
now build --timebase git-commit

; Two developers, same commit:
;   Developer A builds at 09:00 → nova-scheduler-0.1.0.tar.gz
;   Developer B builds at 14:00 → nova-scheduler-0.1.0.tar.gz
;   sha256(A output) == sha256(B output)  ← reproducible
```

If `git-commit` is specified but the project is not a git repository,
`now` fails with a clear error rather than silently falling back to wall
clock — silent fallback would make builds appear reproducible when they
are not.

---

## 26.3 Reproducibility Measures

When `reproducible: true`, `now` activates the following measures.
Each can be individually controlled:

```pasta
{
  reproducible: {
    timebase:        "git-commit",      ; timestamp normalisation
    path_prefix_map: true,              ; strip absolute build paths
    sort_inputs:     true,              ; deterministic input ordering
    no_date_macros:  true,              ; neutralise __DATE__ / __TIME__
    strip_metadata:  true,              ; strip non-deterministic binary metadata
    verify:          true               ; post-build verification pass
  }
}
```

`reproducible: true` (boolean shorthand) enables all measures with their
defaults. `reproducible: { ... }` (map form) enables specific measures.

### `path_prefix_map`

Compilers embed the source file path in debug info and in `__FILE__`
macro expansions. Absolute paths make binaries non-reproducible across
machines with different home directories.

```sh
; GCC/Clang: -fdebug-prefix-map and -fmacro-prefix-map
-fdebug-prefix-map=/home/alice/nova-scheduler=.
-fmacro-prefix-map=/home/alice/nova-scheduler=.

; MSVC: /pathmap
/pathmap:C:\Users\Alice\nova-scheduler=.
```

`now` injects these flags automatically when `path_prefix_map: true`,
using `${now.basedir}` as the path to strip and `.` as the replacement.
The resulting debug info refers to `./src/main/c/sched.c` regardless of
the absolute build path.

For dep headers (which live in `~/.now/repo/`), the prefix map covers
each installed dep path:

```sh
-fdebug-prefix-map=/home/alice/.now/repo/nova/os/hal/1.0.0=nova/os/hal/1.0.0
```

### `no_date_macros`

`__DATE__` and `__TIME__` expand to the compilation time — non-reproducible
by definition. When `no_date_macros: true`, `now` injects defines that
override them with the resolved timebase:

```sh
-D__DATE__="2026-03-05"
-D__TIME__="00:00:00"
-D__TIMESTAMP__="2026-03-05T00:00:00Z"
```

This is technically a violation of the C standard (`__DATE__` is a
predefined macro), but GCC, Clang, and MSVC all allow it in practice.
`now` emits a warning if the compiler rejects the override:

```
Warning: compiler does not support __DATE__ override — date macros
remain non-reproducible. Consider removing __DATE__ usage from sources.
```

### `sort_inputs`

Some tools produce output that depends on the order in which input files
are presented. `now` sorts all input lists (source files passed to the
linker, object files in static archives) lexicographically by path.
This ensures consistent ordering regardless of filesystem enumeration
order, which varies by OS, filesystem type, and directory history.

### `strip_metadata`

Certain binary formats embed build metadata that varies between runs:
- ELF build IDs (random by default in some linkers)
- PE timestamps in Windows executables
- Mach-O UUIDs

```sh
; Disable random ELF build ID
-Wl,--build-id=none

; OR: use a deterministic build ID (hash of content)
-Wl,--build-id=sha1
```

`now` prefers `--build-id=sha1` over `--build-id=none` — a deterministic
content-based ID is reproducible and more useful for debugging than no ID.

### `verify`

When `verify: true`, `now` runs a second build pass immediately after
the first and compares output hashes. If the outputs differ, it reports
which files differ and why (using the diffoscope tool if available, or
a basic binary diff):

```
Reproducibility verification: FAILED

  target/nova-riscv64-none/bin/nova-scheduler:
    First build sha256:  a3f8c2...
    Second build sha256: b91d44...

  Difference detected in:
    .debug_info section (path strings differ — check path_prefix_map)

  Run 'now build --diffoscope' for detailed analysis.
```

A failed verify does not prevent the artifact from being produced, but
`now publish` refuses to publish an artifact that failed reproducibility
verification unless `--force-publish` is specified with an explicit reason.

---

## 26.4 Security Advisory Database

`now` maintains a native security advisory database in Pasta format.
Advisories cover known-vulnerable dep versions and are checked during
`procure`, `build`, and `publish`.

### Advisory Database Location

```
~/.now/advisories/
├── now-advisory-db.pasta          ; main database (pulled from advisory feed)
├── now-advisory-db.pasta.sig      ; signature (verified against advisory feed key)
├── local-overrides.pasta          ; project or org-level overrides
└── .last-updated                  ; timestamp of last pull
```

### Advisory Database Format

```pasta
; now-advisory-db.pasta
{
  version:   "1.0.0",
  updated:   "2026-03-05T00:00:00Z",
  source:    "https://advisories.now.build/db.pasta",

  advisories: [
    {
      id:          "NOW-SA-2026-0042",
      cve:         ["CVE-2026-1234"],
      cwe:         ["CWE-787"],          ; out-of-bounds write
      owasp:       ["A06:2021"],         ; vulnerable and outdated components
      severity:    "critical",           ; info | low | medium | high | critical
      title:       "Buffer overflow in zlib inflate()",

      affects: [
        { id: "zlib:zlib", versions: [">=1.2.0 <1.3.1"] }
      ],

      fixed_in: [
        { id: "zlib:zlib", version: "1.3.1" }
      ],

      description: "A heap buffer overflow in inflate() allows ...",
      references:  ["https://cve.mitre.org/...", "https://nvd.nist.gov/..."],
      published:   "2026-02-14T00:00:00Z",

      ; OWASP Top 10 mapping
      owasp_category: "Vulnerable and Outdated Components",

      ; Exploitability — affects now's default blocking behaviour
      exploitability: "remote-unauthenticated",
      affects_build_time: false,    ; does this vuln affect the build process?
      affects_runtime:    true      ; does this vuln affect the running binary?
    },
    {
      id:       "NOW-SA-2026-0043",
      severity: "high",
      title:    "Arbitrary code execution in now plugin protocol",

      affects: [
        { id: "org.now.plugins:evil-gen", versions: ["*"] }
      ],

      ; Blacklisted entirely — no version is safe
      blacklisted: true,

      affects_build_time: true,   ; this one executes during the build
      affects_runtime:    false
    }
  ]
}
```

### OWASP Integration

The advisory database maps to OWASP Top 10 categories natively. `now`
tracks the current OWASP Top 10 and flags advisories by category:

| OWASP Category | `now` coverage |
|----------------|---------------|
| A01: Broken Access Control | N/A (runtime) |
| A02: Cryptographic Failures | Advisory — weak crypto in dep |
| A03: Injection | Advisory — injection vulns in dep |
| A04: Insecure Design | Advisory — design flaws in dep |
| A05: Security Misconfiguration | Config linting (see §26.6) |
| A06: Vulnerable and Outdated Components | **Primary advisory target** |
| A07: Auth Failures | Advisory — auth vulns in dep |
| A08: Software Integrity Failures | Signing + `private_groups` (docs 20, 25) |
| A09: Logging Failures | N/A (runtime) |
| A10: SSRF | Advisory — server-side request forgery in dep |

A06 is `now`'s primary advisory concern — it is entirely about using
components with known vulnerabilities.

---

## 26.5 Advisory Checking During `procure`

`now procure` checks every resolved dep against the advisory database
before installation.

### Blocking Behaviour

| Severity | Default behaviour | Configurable? |
|----------|------------------|---------------|
| `blacklisted` | Hard error — cannot be overridden | No |
| `critical` | Hard error — blocks build | Yes: `allow` with justification |
| `high` | Hard error — blocks build | Yes: `allow` with justification |
| `medium` | Warning — build continues | Yes: escalate to error |
| `low` | Warning — build continues | Yes: escalate to error |
| `info` | Silent — recorded only | Yes: escalate to warning |

```
now procure

  Checking advisories...
  ✗ CRITICAL  zlib:zlib:1.3.0      NOW-SA-2026-0042  CVE-2026-1234
              Buffer overflow in inflate() — fixed in 1.3.1
              OWASP A06: Vulnerable and Outdated Components

  Build blocked. Update zlib:zlib to >=1.3.1 or add an advisory
  override with justification (see 'now advisory:allow --help').
```

### Advisory Override

When a known-vulnerable dep cannot be updated (compatibility constraint,
no fix available, vulnerability doesn't affect your usage), it can be
explicitly acknowledged:

```pasta
; now.pasta
{
  advisories: {
    allow: [
      {
        advisory: "NOW-SA-2026-0042",
        dep:      "zlib:zlib:1.3.0",
        reason:   "inflate() not called in our usage path — only deflate()",
        expires:  "2026-06-01",         ; mandatory — overrides must have expiry
        approved_by: "alice@acme.org"   ; audit trail
      }
    ]
  }
}
```

An override without `expires` is rejected — temporary acknowledgements
that never expire become permanent security debt silently. The expiry
date is checked at every `procure` run; an expired override fails the
build with a reminder to re-evaluate.

### Build-Time vs Runtime Advisories

Some vulnerabilities only matter at runtime (the vulnerable code path is
in the running binary). Others affect the build process itself (a
malicious code generator, a plugin with a CVE). `now` distinguishes:

- `affects_build_time: true` — blocked by default regardless of whether
  the dep appears in `compile`, `provided`, `runtime`, or `test` scope.
  A build-time vulnerability in a test dep is still a build-time risk.
- `affects_runtime: true` — blocked for `compile` and `runtime` scope
  deps. A runtime vulnerability in a `test`-only dep may be allowed with
  a warning (the vulnerable code is never shipped).

---

## 26.6 Advisory Database Maintenance

```sh
; Pull latest advisory database
now advisory:update

; Pull and verify signature
now advisory:update --verify

; Show all advisories affecting current project deps
now advisory:check

; Show advisory detail
now advisory:show NOW-SA-2026-0042

; Show all advisories by OWASP category
now advisory:check --owasp A06

; List active overrides and their expiry
now advisory:overrides

; Add a local advisory (for internal vulnerability tracking)
now advisory:add --file my-advisory.pasta
```

### Advisory Feed Configuration

```pasta
; ~/.now/config.pasta
{
  advisory: {
    feed:          "https://advisories.now.build/db.pasta",
    feed_key:      "RWT...",         ; signing key for the advisory feed
    update_policy: "on-procure",     ; always | on-procure | manual | daily
    severity_threshold: "medium",   ; block at medium and above (default: high)
    track_owasp:   true,
    local_db:      "~/.now/advisories/local-overrides.pasta"
  }
}
```

### Advisory Feed Integrity

The advisory database is itself a supply chain target. A compromised feed
could whitelist malicious packages or suppress legitimate advisories.
`now` verifies the feed's signature before applying any update:

```
now advisory:update

  Fetching: https://advisories.now.build/db.pasta
  Verifying signature against advisory feed key RWT...
  Signature: OK
  New advisories: 3
  Updated advisories: 1
  Database updated: 2026-03-05T14:22:00Z
```

If the signature fails, the update is rejected and the previous database
is retained:

```
  Signature: FAILED — advisory database not updated
  The feed may be compromised. Report to security@now.build
```

---

## 26.7 Reproducibility and Advisory Interaction

### Reproducing a Build with Known Vulnerabilities

When `now` reproduces a historical build (using a historical
`now.lock.pasta`) that contains deps with current advisories, it warns
clearly without blocking:

```
now build --reproduce 2026-01-15

  Reproducing build as of 2026-01-15T00:00:00Z
  Lock file: now.lock.pasta@2026-01-15

  Advisory warning (reproduction mode — not blocking):
  ⚠ zlib:zlib:1.3.0  NOW-SA-2026-0042 (published 2026-02-14)
    This advisory did not exist at the time of the original build.
    The original build was not knowingly vulnerable.
    The reproduced binary contains this vulnerability.

  Reproduction complete. Advisory report: target/reproduction-advisory.pasta
```

The advisory report is a Pasta file listing every advisory that applies
to the reproduced build, including whether the advisory existed at the
original build date. This is the audit trail that answers: "did we know
about this vulnerability when we shipped?"

### Publishing a Reproducible Build

`now publish` with `reproducible: true` attaches the reproducibility
verification result and the advisory state to the published artifact:

```pasta
; Published artifact metadata (in registry)
{
  reproducible:    true,
  timebase:        "git-commit",
  verified:        true,             ; second-pass verification passed
  build_date:      "2026-03-05T00:00:00Z",
  advisory_state:  "clean",          ; no advisories at publish time
  advisory_db_ver: "2026-03-05"      ; which advisory DB was current
}
```

A consumer of this artifact can see: it was reproducible, verified, and
clean of known advisories at publication time. If a new advisory is
published after this date, `now procure` will flag it — the advisory
state is a point-in-time snapshot, not a permanent guarantee.

---

## 26.10 Advisory Phase Guards, Forced Builds, and Receipt Trail

This document defines how security advisories gate lifecycle phases,
which phases require `--force` to proceed with known-vulnerable deps,
how forced builds leave auditable traces in the local repo receipt, and
how other projects encountering a force-installed artifact are protected
from silently inheriting the risk.

---

## 26.10 The Shared Trust Boundary

`now`'s local repository (`~/.now/repo/`) is a shared trust boundary.
Every project on the machine can reference any artifact installed there.
The refcount system (doc 05b) means that an artifact installed by one
project is immediately available to all others during their `procure`
phase — without any explicit action by those projects.

This creates a specific threat: a project performing security research
on a known-vulnerable artifact installs it to the shared repo, and a
concurrent build on the same machine picks it up silently. The researcher
accepted the risk consciously; the concurrent build did not.

**The rule:** building to `target/` is project-local and isolated.
Installing to `~/.now/repo/` or publishing to a registry crosses the
shared trust boundary and requires explicit force with justification when
known-vulnerable artifacts are involved.

---

## 26.11 Phase Blast Radius and Default Advisory Behaviour

Each lifecycle phase has a distinct blast radius — the scope of what
it can affect beyond the current project's `target/` directory.

```
Isolated (target/ only):          procure-cache, generate, build, link, package
Shared trust boundary crossed:    install (→ ~/.now/repo/)
Network boundary crossed:         publish (→ registry)
Local execution:                  test (→ runs binary on this machine)
Build-time execution:             generate (→ runs tools/plugins on this machine)
```

### Default Behaviour by Phase and Severity

| Phase | Blacklisted | Critical | High | Medium | Low | Info |
|-------|-------------|----------|------|--------|-----|------|
| `procure` (download) | Block | Warn | Warn | Warn | Log | Log |
| `generate` | Block | Block | Block | Warn | Log | Log |
| `build` | Warn¹ | Warn | Warn | Log | Log | Log |
| `link` | Warn¹ | Warn | Warn | Log | Log | Log |
| `test` | Block | Block | Block | Warn | Warn | Log |
| `package` | Warn¹ | Warn | Warn | Log | Log | Log |
| `install` | Block+Force | Block+Force | Block+Force | Warn+Force | Warn | Log |
| `publish` | Block+Force | Block+Force | Block+Force | Block+Force | Warn | Warn |

¹ Blacklisted deps are warned rather than blocked in `build`/`link`/`package`
  because isolated `target/` output has legitimate research and forensic value.
  The binary is produced but marked in the build manifest.

**Block** — phase refuses to run; build fails with advisory detail.
**Block+Force** — phase refuses to run; `--force` required to proceed.
**Warn** — phase runs; advisory is printed prominently.
**Log** — advisory is recorded in build manifest silently.

### Rationale for `generate` Blocking

`generate` executes tools and plugins that produce source code. A
code generator with `affects_build_time: true` in the advisory database
runs arbitrary code on the build machine — it is a direct execution risk,
not merely a shipped artifact risk. `now` blocks `generate` on
critical/high build-time advisories by the same logic as `install`.

### Rationale for `test` Blocking

Running a binary that links against a critically-vulnerable library
executes that library on the build machine. Even in an isolated test
environment, a vuln that allows arbitrary code execution at the library
level is a real local risk. Security researchers who deliberately test
vulnerable behaviour use `--force` with explicit justification.

---

## 26.12 The `--force` Switch

`--force` unlocks phases that would otherwise be blocked by advisory
policy. It requires a justification string and always writes a receipt.

```sh
; Force install of a project with a known-critical vulnerability
now install --force --reason "security research: analysing CVE-2026-1234 exploit path"

; Force the full lifecycle including install
now build install --force --reason "reproducing prod bug in vulnerable dep version"

; Force test execution with a known-critical dep
now test --force --reason "testing our mitigation against CVE-2026-1234"

; Force publish (requires additional --confirm flag for registry boundary)
now publish --force --confirm \
  --reason "patched binary — vuln in dep does not affect our call path, see ACME-SEC-042"
```

`--reason` is mandatory with `--force`. An empty or placeholder reason
("yes", "ok", "bypass") is rejected — `now` checks for minimum length
(20 characters) and refuses strings that appear to be filler.

`--force` without `--reason` produces:
```
Error: --force requires --reason "explanation"
Advisory enforcement exists to protect shared infrastructure.
Document why this risk is acceptable before proceeding.
```

### Force Scope

`--force` applies to the phases named in the command. It does not
propagate silently to implied phases:

```sh
; This forces install but NOT test — test still blocks
now test install --force --reason "..."

; This forces both test and install
now test install --force --reason "..."
; (--force applies to all named phases in the chain)
```

`--force` never applies to future invocations. Each `now` invocation
requires its own `--force` decision. There is no persistent "I have
accepted this risk for this project" flag — the receipt trail serves
that purpose with proper provenance, not a silent bypass.

---

## 26.13 The Forced Install Receipt

When `--force` causes `now install` to install an artifact with known
advisories to `~/.now/repo/`, a receipt is written into the installed
descriptor alongside the normal `now.pasta`.

### Receipt Location

```
~/.now/repo/{group-path}/{artifact}/{version}/
├── now.pasta            ; installed descriptor (normal)
├── now.lock.pasta       ; dep graph of this artifact
└── .install-receipt.pasta   ; forced install receipt (hidden file)
```

The receipt uses a hidden filename (`.install-receipt.pasta`) so normal
tools do not present it as part of the artifact, but `now` always checks
for it.

### Receipt Format

```pasta
; ~/.now/repo/zlib/zlib/1.3.0/.install-receipt.pasta
{
  installed_at:  "2026-03-05T14:22:00Z",
  installed_by:  "alice",
  host:          "nova-build-01.acme.org",
  project:       "org.acme:kernel:2.1.0",
  forced:        true,
  force_reason:  "security research: analysing CVE-2026-1234 exploit path in inflate()",

  advisories: [
    {
      id:          "NOW-SA-2026-0042",
      cve:         "CVE-2026-1234",
      severity:    "critical",
      title:       "Buffer overflow in zlib inflate()",
      state:       "force-installed",
      acknowledged_at: "2026-03-05T14:22:00Z",
      expires:     "2026-04-05"        ; 30 days from install — configurable
    }
  ],

  ; Refcount protection — other projects must re-acknowledge
  requires_reacknowledgement: true
}
```

### Receipt Expiry

Force receipts expire. By default, 30 days from forced install. After
expiry, the forced artifact behaves as if newly encountered — `now procure`
blocks again, requiring a fresh `--force` decision. This prevents research
installs from becoming permanent silent exceptions.

```pasta
; ~/.now/config.pasta
{
  advisory: {
    force_receipt_expiry: "30d"    ; default
  }
}
```

---

## 26.14 Cross-Project Receipt Propagation

When another project's `now procure` would install or reference an
artifact that has a forced install receipt in the local repo, `now`
does not silently accept it. It surfaces the receipt and requires
explicit acknowledgement from the new consumer.

### Scenario

```
Project A (security research):
  now install --force --reason "CVE-2026-1234 research"
  → zlib:zlib:1.3.0 installed with forced receipt

Project B (concurrent build, unrelated):
  now procure
  → zlib:zlib:1.3.0 in lock file, already in ~/.now/repo/
  → now detects .install-receipt.pasta
  → DOES NOT silently use it
```

### Cross-Project Advisory Prompt

```
now procure (project B)

  ⚠ Advisory: zlib:zlib:1.3.0 is present in the local repo but was
    force-installed by another project due to known vulnerabilities.

  Advisory:  NOW-SA-2026-0042 (CRITICAL)
             CVE-2026-1234 — Buffer overflow in zlib inflate()
  Forced by: alice on nova-build-01.acme.org (2026-03-05)
  Reason:    "security research: analysing CVE-2026-1234 exploit path"
  Expires:   2026-04-05

  This build would use the same vulnerable artifact.
  Options:
    1. Update now.lock.pasta to use zlib:zlib:>=1.3.1 (recommended)
    2. Acknowledge and proceed:
         now procure --acknowledge NOW-SA-2026-0042 \
                     --reason "same research context, controlled environment"
    3. Isolate: now procure --no-shared-repo
         (downloads fresh copy to project-local cache, not shared repo)
```

The new consumer must choose explicitly. There is no silent inheritance
of forced installs.

### `--no-shared-repo`

The `--no-shared-repo` flag tells `now procure` to ignore the shared
`~/.now/repo/` for this invocation and download all deps fresh to a
project-local cache at `target/.deps/`. This provides complete isolation:

```sh
now procure --no-shared-repo    ; fully isolated dep resolution
now build --no-shared-repo      ; isolated build — no shared repo interaction
```

A `--no-shared-repo` build never installs to `~/.now/repo/` and never
reads from it — it is entirely self-contained. Useful for:
- CI environments where machine state must not bleed between jobs
- Security research where you want complete isolation
- Auditing a dep without affecting the shared machine state

---

## 26.15 Advisory State in the Build Manifest

The build manifest (`target/.now-manifest`) records the advisory state
for every dep used in the build, regardless of whether advisories were
present:

```pasta
; target/.now-manifest (excerpt)
{
  deps: [
    {
      id:        "zlib:zlib:1.3.0",
      sha256:    "a3f8c2...",
      advisories: [
        {
          id:       "NOW-SA-2026-0042",
          severity: "critical",
          state:    "forced",        ; forced | allowed | clean | unknown
          db_version: "2026-03-05"  ; advisory DB version at build time
        }
      ]
    },
    {
      id:         "org.acme:core:4.2.1",
      sha256:     "b91d44...",
      advisories: []                 ; clean at build time
    }
  ],

  advisory_db_version: "2026-03-05T00:00:00Z",
  advisory_check_at:   "2026-03-05T14:22:01Z"
}
```

This manifest is the build-time evidence for compliance and audit
purposes — it proves which advisory state was current when the build ran
and whether any forced or allowed exceptions were in effect.

---

## 26.16 Phase Guard Configuration

The default phase guard table (§26.11) can be adjusted in `now.pasta`
or `~/.now/config.pasta`. Escalating guards (making them stricter) is
always permitted. Relaxing guards (making them more permissive) requires
`advisory.relax_guards: true` and produces a workspace-level warning.

```pasta
; now.pasta — escalate medium to block for install
{
  advisory: {
    phase_guards: {
      install: {
        medium: "block+force",   ; stricter than default
        low:    "warn+force"
      },
      test: {
        medium: "block"          ; no force option — must fix
      }
    }
  }
}
```

```pasta
; now.pasta — relax guards (requires explicit opt-in)
{
  advisory: {
    relax_guards:  true,         ; explicit acknowledgement that guards are relaxed
    relax_reason:  "air-gapped research network — no external risk",
    phase_guards: {
      install: {
        critical: "warn"         ; relaxed — warn only, no force required
      }
    }
  }
}
```

Relaxing critical guards on `install` or `publish` produces a permanent
warning on every `now` invocation for that project:

```
⚠ Advisory guards relaxed for install:critical — see now.pasta advisory.relax_reason
```

This warning cannot be suppressed. It is the price of relaxed guards —
the configuration is visible and auditable to anyone reading build output.

---

## 26.17 `now advisory:audit` — Full Project Audit

```sh
now advisory:audit
```

Produces a complete advisory audit report for the current project —
all deps, all phases, all advisory states, all forced/allowed exceptions,
all receipts in the shared repo, and all cross-project forced installs
that this project has acknowledged:

```
Advisory audit: org.acme:kernel:2.1.0
Advisory DB:    2026-03-05 (3 days old — consider 'now advisory:update')

Dependency advisory states:
  zlib:zlib:1.3.0           CRITICAL  NOW-SA-2026-0042  forced (expires 2026-04-05)
  org.acme:core:4.2.1       CLEAN
  org.acme:hal:2.0.0        CLEAN
  unity:unity:2.5.2         CLEAN (test-only — runtime risk not applicable)

Phase guard summary:
  install   critical→block+force  [1 forced exception active]
  publish   critical→block+force  [no exceptions]
  test      critical→block        [no exceptions]
  generate  critical→block        [no exceptions]

Shared repo forced installs affecting this project:
  zlib:zlib:1.3.0  forced by alice 2026-03-05  [acknowledged 2026-03-05]

Overall: 1 CRITICAL exception active — review before publish
```



---



---

# Chapter 25 — Cascading Configuration Layers

This document specifies the **layer model**: a mechanism for composing
`now` configuration from an ordered stack of Pasta documents, from
`now`'s own shipped baseline up through enterprise, department, team,
and project-specific layers. Each layer is a sectioned Pasta document.
Layers are merged top-down using per-section policy, with locking and
exclusion semantics that allow higher layers to protect critical settings
while still giving lower layers room to operate.

The layer model is an organisational and infrastructure boundary concept.
It sits above and is distinct from the project-level inheritance model
(doc 01c `inherit_defaults`, `parent:`), which concerns itself with
sharing build settings across modules within or between projects. Layers
are global infrastructure; inheritance is project structure.

---

## 34.1 Concepts

### Layer stack

A **layer** is a Pasta document that uses named sections (`@section-name`)
at its root. Layers are ordered from lowest priority to highest priority.
The full stack, from bottom to top:

```
now-baseline          ← shipped with now, always present
org-enterprise        ← organisation-wide mandate (optional)
org-department        ← department / business unit (optional)
org-team              ← team (optional)
project               ← now.pasta (always present, always top)
```

Any number of intermediate layers may exist between baseline and project.
The names `enterprise`, `department`, `team` are conventional, not reserved
— the actual layer names and their order are declared by the organisation
(§34.3).

### Section as the merge unit

Layers compose via **matching section names**. If layer A and layer B both
contain `@compile`, the two sections are merged according to the section's
declared policy. Sections that appear in only one layer pass through
unchanged.

### Section policy

Every section carries a **policy** that governs how lower layers may
interact with it. Policy is declared by the layer that introduces or
tightens it — a higher-priority layer may lock a section that a
lower-priority layer left unlocked, but never the reverse.

| Policy | Meaning |
|--------|---------|
| `open` | Lower layers may freely override or extend any field. Default. |
| `locked` | Lower layers may override, but each override produces `NOW-W0401` (advisory lock violation) in `now layers:audit`. Arrays accumulate-only. |
| `sealed` | Reserved for future use. Not yet defined. |

Advisory lock violations are warnings, not errors. A lower layer that
needs to override a locked section does so by adding a
`_override_reason: "..."` field in its own copy of the section, which
is recorded in the audit trail. The reason string follows the same
substance rules as `--force --reason` in doc 29 (minimum 20 characters,
no generic phrases).

### Merge semantics

Given two layers that both declare `@section-name`:

**Maps** are merged recursively. For each key:
- If the key appears only in one layer, it passes through.
- If the key appears in both, the lower-priority (deeper-in-stack) layer's
  value wins — the project is closer to the ground truth than the org.
  Exception: if the section is `locked`, the higher-priority layer's value
  wins and the override is flagged.

Wait — priority direction needs to be stated precisely. In this model:

> **Lower in the stack = higher authority for overriding.**
> The project layer is the most specific; `now-baseline` is the most general.
> A more-specific layer overrides a more-general layer for `open` sections.
> For `locked` sections, the locking layer's values are protected — the
> project may still override them, but the audit trail records it.

**Arrays** behave differently by section policy:
- `open` section: lower layer (project) may use `!exclude` entries to
  remove values contributed by higher layers (§34.6). Additions accumulate.
- `locked` section: accumulate-only. The project may add entries but
  cannot remove entries contributed by a locking layer. Attempted
  exclusions produce `NOW-W0402`.

**Scalars** in a `locked` section: project value wins but is flagged.
Scalars in an `open` section: project value wins silently.

---

## 34.2 The `now-baseline` Layer

`now` ships a built-in baseline layer as a Pasta document installed at:

```
~/.now/repo/now-baseline/now-baseline.pasta
```

On first install, `now` copies this file from its embedded resources into
the local repo. It is a real file — editable, inspectable, version-tracked
by `now` itself. The user (or org tooling) may replace it, but `now
layers:check` will note if it has diverged from the shipped version.

The baseline defines well-known section names with opinionated defaults.
Organisations may override every value. The baseline is the floor in the
sense that it defines the section vocabulary and sane starting points;
it is not a hard floor in the sense of being immutable.

### Well-known section names

| Section | Content |
|---------|---------|
| `@compile` | Default compile settings: `warnings: ["Wall", "Wextra"]`, `opt: debug`, `debug: true` |
| `@link` | Default link settings. Empty in baseline. |
| `@repos` | Default registry list. Points to the official `now` public registry. |
| `@toolchain` | Default toolchain: `preset: gcc`, platform-detected. |
| `@test` | Default test runner settings. |
| `@advisory` | Default advisory phase guards (mirrors doc 29 defaults). |
| `@reproducible` | Reproducible build defaults: `reproducible: false` in baseline. |
| `@private_groups` | Empty in baseline. Orgs populate this. |
| `@trust` | Default trust store config. |

The baseline is a normal Pasta document. Its shipped content:

```pasta
@compile {
  _policy: "open",
  warnings: ["Wall", "Wextra"],
  opt: "debug",
  debug: true
}

@link {
  _policy: "open"
}

@repos {
  _policy: "open",
  registries: [
    {url: "https://repo.now.build", id: "central", release: true, snapshot: false}
  ]
}

@toolchain {
  _policy: "open",
  preset: "gcc"
}

@advisory {
  _policy: "locked",
  phase_guards: {
    critical: "error",
    high:     "warn",
    medium:   "note",
    low:      "note"
  }
}

@reproducible {
  _policy: "open",
  enabled: false
}

@private_groups {
  _policy: "open",
  groups: []
}
```

Note that `@advisory` ships as `locked` — the baseline declares that
advisory guard configuration is a sensitive security control that should
not be silently weakened. An organisation that wants to change it must
do so explicitly in their own layer, which will trigger audit warnings
for any project that loosens the guards further.

---

## 34.3 Layer Discovery and Ordering

### Organisation-mandated layer list

The organisation declares the layer stack in their enterprise layer
document, or — if there is no enterprise layer — in the machine-level
`~/.now/config.pasta` under a `layers:` key:

```pasta
; ~/.now/config.pasta
{
  layers: [
    {id: "now-baseline",  source: "builtin"},
    {id: "enterprise",    source: "repo",  coordinate: "com.acme:now-config:^1.0"},
    {id: "department",    source: "file",  path: "~/.now/layers/backend-dept.pasta"},
    {id: "team",          source: "file",  path: "~/.now/layers/platform-team.pasta"}
  ]
}
```

The `project` layer (the `now.pasta` in the current project) is always
appended implicitly as the final, highest-specificity layer. It is never
listed in `layers:` — doing so produces `NOW-E1501`.

### Source types

| `source` | Meaning |
|----------|---------|
| `builtin` | The `now-baseline` embedded in the `now` installation. Only valid for `id: "now-baseline"`. |
| `repo` | A published artifact in the `now` registry. Resolved and cached via the standard procure mechanism (doc 10). Version range applies. |
| `file` | A local file path. `~` expands to the user home directory. |
| `url` | An HTTPS URL. Fetched and cached. Content-hash pinned via `layers.lock.pasta` (§34.8). |

### Filesystem walk (local layer discovery)

In addition to the declared `layers:` list, `now` performs a convention-
based filesystem walk for **file-based layers**: starting from the project
directory, `now` walks up the directory tree looking for `.now-layer.pasta`
files. Each found file is inserted into the stack above the declared layers
and below the project layer, ordered from farthest to nearest (so a layer
closer to the project has higher specificity).

```
/home/alice/work/acme/backend/services/auth/  ← project (now.pasta)
/home/alice/work/acme/backend/services/       ← .now-layer.pasta (if present)
/home/alice/work/acme/backend/               ← .now-layer.pasta (if present)
/home/alice/work/acme/                       ← .now-layer.pasta (if present)
/home/alice/work/                            ← walk stops at VCS root or home
```

Walk stops at:
- The user's home directory (never walks above `~`).
- A VCS root (directory containing `.git`, `.hg`, `.svn`).
- A directory containing `now-workspace.pasta` (workspace root is a
  natural organisational boundary).

This gives monorepo teams a natural mechanism: a `.now-layer.pasta` at
the monorepo root applies to all projects within it without each project
needing to declare anything.

### Final stack construction

```
now-baseline (builtin)
  ↓
layers[0] (from config.pasta layers list, e.g. enterprise repo artifact)
  ↓
layers[1..n] (from config.pasta layers list, e.g. department, team)
  ↓
.now-layer.pasta files (filesystem walk, farthest first)
  ↓
now.pasta (project, always last)
```

Duplicate layer IDs (same `id` field) produce `NOW-E1502` — each layer
must be distinct.

---

## 34.4 Layer Document Format

A layer document is a Pasta file using `@section` syntax at root. Every
section may contain a `_policy` field (stripped before merging — it is
metadata, not configuration):

```pasta
; acme-enterprise-layer.pasta
; Org-wide now configuration for Acme Corp

@compile {
  _policy: "locked",
  _description: "Corporate compiler standards. Override requires justification.",
  warnings: ["Wall", "Wextra", "Wpedantic", "Werror"],
  defines:  ["ACME_INTERNAL", "NDEBUG"]
}

@repos {
  _policy: "locked",
  _description: "All artifacts must come from internal registry first.",
  registries: [
    {url: "https://now.acme.internal", id: "acme-internal", release: true, snapshot: true},
    {url: "https://repo.now.build",    id: "central",       release: true, snapshot: false}
  ]
}

@private_groups {
  _policy: "locked",
  groups: ["com.acme", "acme.internal"]
}

@advisory {
  _policy: "locked",
  _override_reason: "Acme security policy requires high-severity advisories to block builds, not just warn.",
  phase_guards: {
    critical: "error",
    high:     "error",
    medium:   "warn",
    low:      "note"
  }
}

@reproducible {
  _policy: "locked",
  _description: "All Acme builds must be reproducible.",
  enabled: true,
  timebase: "git-commit"
}
```

### Reserved `_` prefixed fields in layers

| Field | Meaning |
|-------|---------|
| `_policy` | `"open"` or `"locked"`. Default: `"open"`. Stripped before merge. |
| `_description` | Human-readable explanation of the section's intent. Shown in `now layers:show`. |
| `_override_reason` | Required when this layer overrides a `locked` section from a lower layer. Recorded in audit trail. |
| `_min_now_version` | Minimum `now` version required to interpret this layer. `now` errors if its version is lower. |

Underscore-prefixed fields are layer metadata. They are never merged into
the effective configuration seen by the build.

---

## 34.5 Applying Layers to the Build

`now` computes the **effective configuration** once at startup, before any
lifecycle phase:

1. Load and validate all layers in stack order (§34.3).
2. For each well-known section name, merge layers bottom-up (baseline
   first, project last) according to the section policy (§34.1).
3. For custom sections (not in the well-known list), merge using `open`
   policy unless the introducing layer declared otherwise.
4. The resulting merged document is the effective configuration. It is
   available via `now layers:show --effective`.
5. Any advisory-lock violations discovered during merge are collected and
   reported. They do not stop the build unless `--strict-layers` is active.

### How effective configuration maps to the build model

The effective configuration is not a separate config file — it feeds
directly into the same fields as `now.pasta`. Specifically:

| Effective `@section` | Feeds into |
|---------------------|------------|
| `@compile` | Root `compile:` block, merged before profile application |
| `@link` | Root `link:` block |
| `@repos` | `repos:` array |
| `@toolchain` | `toolchain:` block |
| `@advisory` | `phase_guards:` in advisory config (doc 29) |
| `@reproducible` | `reproducible:` block (doc 28) |
| `@private_groups` | `private_groups:` in dep confusion config (doc 25) |
| `@trust` | Trust store config (doc 20) |

The project `now.pasta` fields take precedence over the effective layer
configuration for `open` sections, exactly as if the layer values were
defaults written in the baseline. For `locked` sections, the lock-declaring
layer's values win for arrays (accumulate-only) and scalars (project value
wins but is audited).

---

## 34.6 Array Exclusion Syntax

In `open` sections, a lower-priority layer (including the project) may
exclude array entries contributed by a higher layer. The mechanism differs
by element type but uses the same `_exclude` convention in both cases —
consistent with the `_`-prefixed metadata pattern used throughout layer
documents.

Exclusion entries are always stripped before the effective configuration
is materialised. The build never sees them.

In `locked` sections, any exclusion attempt produces `NOW-W0402`
(exclusion attempt in locked section) and is silently dropped — the
excluded value remains in the effective array.

### 34.6.1 String Array Exclusion

For arrays whose elements are plain strings, use the `!exclude:` prefix:

```pasta
; Team layer added "Wpedantic" to warnings (locked: false)
; This project has a documented reason to remove it
{
  compile: {
    warnings: ["!exclude:Wpedantic"]
  }
}
```

During merge, any accumulated string entry whose value exactly matches
the suffix of `!exclude:<value>` is removed. Exclusion of a value not
present is silently ignored — not an error, since the upstream layer
that added it may itself have been conditional.

### 34.6.2 Map Array Exclusion

For arrays whose elements are maps (registries, deps, plugins, tools,
assembly entries, and any custom map arrays), exclusion uses an
in-element `_exclude: true` field:

```pasta
; Lower layer wants to remove the public 'central' registry
; that the enterprise layer added
@repos {
  registries: [
    {id: "central", _exclude: true}
  ]
}
```

The `_exclude: true` entry is a **stub** — it carries only enough fields
to identify the target, plus the `_exclude` sentinel. During merge, `now`
matches stubs against accumulated entries and removes matched entries.
The stub itself is also removed. Neither appears in the effective
configuration.

**Matching rules** — applied in priority order:

1. **`id` field** — if the stub has an `id` field and any accumulated
   entry has the same `id`, that entry is removed. This is the preferred
   and most explicit form.
2. **`url` field** — if no `id` is present but the stub has a `url`,
   match by `url`.
3. **`coordinate` field** — for `deps` arrays, match by the `id`
   coordinate string (group:artifact, version ignored for matching).
4. **`name` field** — for `tools`, `plugins`, `assembly` arrays, match
   by `name`.
5. **No match** — if none of the above fields are present in the stub,
   `now` produces `NOW-W0408` (unresolvable exclusion stub — no identity
   field) and the stub is dropped without removing anything.

The matching fields by array type:

| Array | Identity field | Fallback |
|-------|---------------|---------|
| `repos.registries` | `id` | `url` |
| `deps` | `id` (coordinate prefix) | — |
| `plugins` | `id` (coordinate prefix) | — |
| `tools` | `name` | — |
| `assembly` | `id` | — |
| custom | `id` | `name`, then `url` |

**Multiple matches** — if a stub matches more than one accumulated entry
(e.g. two entries with the same `id`, which would itself be a validation
error), all matches are removed and `NOW-W0409` (ambiguous exclusion —
multiple entries matched) is emitted.

### 34.6.3 Full Exclusion Example

Enterprise layer (locked repos, open compile):

```pasta
@compile {
  _policy: "open",
  warnings: ["Wall", "Wextra", "Wpedantic", "Werror"],
  defines:  ["ACME_CORP"]
}

@repos {
  _policy: "locked",
  registries: [
    {id: "acme-internal", url: "https://now.acme.internal", release: true},
    {id: "central",       url: "https://repo.now.build",    release: true}
  ]
}
```

Project `now.pasta` (removes a warning from open section; cannot remove
a registry from the locked section):

```pasta
{
  compile: {
    warnings: ["!exclude:Wpedantic"],   ; open section — allowed, silent
    defines:  ["AUTH_SERVICE"]
  },

  ; Attempting to exclude from locked @repos:
  repos: {
    registries: [
      {id: "central", _exclude: true}   ; NOW-W0402 — locked, exclusion dropped
    ]
  }
}
```

Effective `@compile.warnings` after merge: `["Wall", "Wextra", "Werror"]`  
Effective `@repos.registries` after merge: both registries retained (exclusion blocked).

### 34.6.4 Composability

Both exclusion forms are composable within the same array in a single
layer — additions and exclusions may coexist:

```pasta
@compile {
  warnings: [
    "Wshadow",             ; add this
    "!exclude:Wpedantic"   ; remove this (from a higher layer)
  ]
}
```

Processing order: exclusions are applied first across the accumulated
array, then additions are appended. This ensures a layer cannot
simultaneously exclude and re-add the same value (the exclusion wins).

Map arrays follow the same ordering: `_exclude: true` stubs are matched
and removed first, then non-stub entries from the current layer are
appended.

---

## 34.7 Interaction with Existing Mechanisms

### `~/.now/config.pasta`

`config.pasta` gains a `layers:` key (§34.3). Its existing keys
(`offline_fallback`, `trust_store`, credential paths, etc.) remain
unchanged. Effectively, `config.pasta` becomes the layer registry for
the machine.

If `@repos`, `@trust`, or `@private_groups` are present in the layer
stack, they take precedence over the corresponding flat fields in
`config.pasta`. `config.pasta` flat fields are treated as if they were
in an implicit layer below `now-baseline`. This ensures machines without
a layer stack configured continue to work exactly as before.

### `inherit_defaults` and `parent:` (doc 01c)

Layer merge happens **before** project-level inheritance. The resolution
order is:

```
now-baseline
  ↓ layer merge
org/dept/team layers
  ↓ layer merge
filesystem .now-layer.pasta files
  ↓ layer merge
now.pasta (project)
  ↓ inherit / parent expansion (doc 01c)
  ↓ profile application (doc 01 §1.8)
  ↓ effective build configuration
```

`inherit_defaults` in `now-workspace.pasta` applies after all layer
merging. It is purely a workspace-internal mechanism and is not visible
to the layer system.

### Advisory phase guards (doc 29)

The `@advisory` section in layers maps directly onto the `phase_guards`
configuration. A locked `@advisory` section from the enterprise layer
means the project cannot silently weaken advisory enforcement — any
weakening goes through the `--force --reason` path and is recorded in
the forced-install receipt (doc 29 §29.4). `now advisory:audit` (doc 29
§29.7) includes layer-originated guard settings in its report.

---

## 34.8 Layer Locking (`layers.lock.pasta`)

Published (`repo` source) and URL-sourced layers are pinned in a
`layers.lock.pasta` file at the project root (alongside `now.lock.pasta`).
Format:

```pasta
; layers.lock.pasta — auto-generated, commit to VCS
{
  locked: [
    {
      id:         "enterprise",
      source:     "repo",
      coordinate: "com.acme:now-config:1.4.2",
      sha256:     "e3b0c44298fc1c149afb...",
      resolved:   "2026-03-06T10:37:00Z"
    }
  ]
}
```

`layers.lock.pasta` is always committed to VCS. It ensures all developers and
CI machines use the same layer version. Regenerated with `now layers:update`.

File-sourced and filesystem-walk layers are not locked (they are already
pinned by their filesystem path). `now layers:check` warns if a local
file layer has changed since last effective configuration was computed.

---

## 34.9 CLI: `now layers:*` Commands

### `now layers:show`

```
now layers:show [--effective] [--section <name>]
```

Without flags: shows the full layer stack, in order, with source and
policy for each section.

With `--effective`: shows the merged effective configuration as a Pasta
document (sections form).

With `--section compile`: shows only the `@compile` section across all
layers and the merged result.

### `now layers:audit`

```
now layers:audit [--format pasta|json|text]
```

Reports all advisory-lock violations in the current project: places where
a lower-priority layer overrides a `locked` section from a higher-priority
layer. Output includes the layer that set the lock, the layer that
overrode it, the field(s) affected, and the `_override_reason` if present.

Exit code `0` if no violations, `1` if violations exist.

Example output:
```
Advisory lock violations in effective configuration:

  Section: @compile
  Locked by:   enterprise (com.acme:now-config:1.4.2)
  Overridden by: project (now.pasta:14)
  Field: warnings — enterprise requires ["Wall","Wextra","Wpedantic","Werror"]
                    project removes    "Wpedantic" via !exclude
  Override reason: (none provided — add _override_reason to now.pasta @compile)
  Code: NOW-W0401

  1 violation(s). Run 'now layers:audit --format json' for machine-readable output.
```

### `now layers:update`

Refreshes `layers.lock.pasta` — resolves the latest versions of all `repo`-
sourced layers within their declared version ranges and rewrites the lock
file.

### `now layers:check`

Validates the layer stack without running a build:
- All declared layers are reachable.
- Layer documents are valid Pasta.
- No duplicate layer IDs.
- No unknown section names (warns `NOW-W0403`).
- `layers.lock.pasta` is up to date with `config.pasta` declarations.

### `now layers:init`

Scaffolds a `.now-layer.pasta` in the current directory with all
well-known sections present, each with `_policy: "open"` and commented-out
example values. Intended as a starting point for team or department layers.

---

## 34.10 Error and Warning Codes

New codes in the `E15xx` / `W04xx` ranges (reserved for layer system):

| Code | Level | Meaning |
|------|-------|---------|
| `NOW-E1501` | error | `project` listed in `layers:` — implicit, cannot be declared. |
| `NOW-E1502` | error | Duplicate layer ID in stack. |
| `NOW-E1503` | error | Layer document root is not a section document (must use `@name` sections). |
| `NOW-E1504` | error | Layer source unreachable. |
| `NOW-E1505` | error | Layer `_min_now_version` not satisfied. |
| `NOW-W0401` | warning | Advisory lock violation — lower layer overrides locked section. |
| `NOW-W0402` | warning | `_exclude` or `!exclude:` attempted in locked section; entry retained. |
| `NOW-W0403` | warning | Unknown section name in layer document (not a well-known section). |
| `NOW-W0404` | warning | Local file layer has changed since last `layers:check`. |
| `NOW-W0408` | warning | Unresolvable exclusion stub — no identity field (`id`, `url`, `name`, or `coordinate`) present. Stub dropped, nothing removed. |
| `NOW-W0409` | warning | Ambiguous exclusion — stub matched multiple accumulated entries. All matches removed. |

`NOW-W0401` warnings are shown after every build in which they are
present, not just during `now layers:audit`. They are shown as a
trailing block after the build summary, not inline with build output,
so they do not disrupt CI log parsing.

---

## 34.11 Security Considerations

The layer model introduces a new supply chain surface: a `repo`-sourced
layer is an artifact from the registry, resolved and applied to every
build on the machine. The following controls apply:

- `repo`-sourced layers are subject to the same integrity checks
  (SHA-256) and signing/trust rules as any other artifact (doc 20).
- A layer artifact with group prefix matching `private_groups` (doc 25)
  is never resolved from public registries.
- `layers.lock.pasta` pins exact versions and hashes; drift is detected by
  `now layers:check`.
- A layer document may not contain executable content — it is purely
  declarative Pasta configuration. Plugins and tools declared in a layer's
  `@plugins` or `@tools` section are subject to the same plugin security
  model as those declared in `now.pasta` (doc 24).

---

## 34.12 Example: Complete Three-Layer Stack

### `~/.now/config.pasta` (machine config, declares the stack)

```pasta
{
  layers: [
    {id: "now-baseline",  source: "builtin"},
    {id: "enterprise",    source: "repo", coordinate: "com.acme:now-config:^1.0"},
    {id: "backend-dept",  source: "file", path: "~/.now/layers/backend.pasta"}
  ]
}
```

### `com.acme:now-config:1.4.2` (enterprise layer, from registry)

```pasta
@compile {
  _policy: "locked",
  warnings: ["Wall", "Wextra", "Wpedantic", "Werror"],
  defines:  ["ACME_CORP"]
}

@repos {
  _policy: "locked",
  registries: [
    {url: "https://now.acme.internal", id: "acme", release: true, snapshot: true},
    {url: "https://repo.now.build",    id: "central", release: true, snapshot: false}
  ]
}

@private_groups {
  _policy: "locked",
  groups: ["com.acme"]
}

@reproducible {
  _policy: "locked",
  enabled: true,
  timebase: "git-commit"
}
```

### `~/.now/layers/backend.pasta` (department layer, local file)

```pasta
@compile {
  _policy: "open",
  _override_reason: "Backend team uses ASAN in all debug builds by org policy BE-SEC-04.",
  sanitizers: ["address", "undefined"]
}

@toolchain {
  _policy: "open",
  preset: "llvm"
}
```

### `now.pasta` (project layer)

```pasta
{
  group:    "com.acme",
  artifact: "auth-service",
  version:  "2.3.0",

  langs: ["c"],
  std:   "c11",

  output: {type: "executable", name: "auth"},

  compile: {
    defines: ["AUTH_SERVICE_BUILD"]
    ; warnings, Werror, ACME_CORP, sanitizers come from layers — not repeated here
  },

  deps: [
    {id: "com.acme:acme-crypto:^3.1", scope: "compile"}
  ]
}
```

### Effective configuration for `@compile` after merge

```pasta
@compile {
  warnings:   ["Wall", "Wextra", "Wpedantic", "Werror"],  ; enterprise (locked)
  defines:    ["ACME_CORP", "AUTH_SERVICE_BUILD"],         ; enterprise + project
  sanitizers: ["address", "undefined"],                    ; backend-dept
  opt:        "debug",                                     ; baseline
  debug:      true                                         ; baseline
}
```

`now layers:audit` for this project: no violations. The backend-dept
layer added to `@compile` but did not conflict with the locked fields.
The project added a define, which is accumulation, not exclusion.

---

## 34.13 Filesystem Walk in Workspace Builds

The single-project walk described in §34.3 is well-defined: `now` stands
in the project directory and walks upward. Workspace builds are more
complex because `now` is invoked at a root and builds downward across
multiple modules, each at a different filesystem depth.

### 34.13.1 Workspace Mode

A workspace is either a **monorepo** (modules share ownership, a unified
layer context is expected) or an **aggregation** (independent projects
co-located for build convenience, each with its own legitimate layer
context). The distinction determines how divergent per-module stacks are
treated.

`now` infers the workspace mode:

| Condition | Inferred mode |
|-----------|---------------|
| `now-workspace.pasta` contains `inherit_defaults` | `monorepo` |
| `now-workspace.pasta` contains no `inherit_defaults` | `aggregate` |

Inference can be overridden explicitly when it fails — symlinked module
trees, case-insensitive or network filesystems, and unusual repo layouts
can all produce unreliable path comparisons:

```pasta
; now-workspace.pasta
{
  workspace_mode: "aggregate",   ; "monorepo" | "aggregate" | inferred (default)
  modules: ["projectA", "projectB", "projectC"]
}
```

`workspace_mode` defaults to `"inferred"` when absent. An explicit value
suppresses the inference logic entirely and produces `NOW-W0405` if the
declared mode contradicts what inference would have concluded — a useful
signal that the repo layout is surprising.

### 34.13.2 Two-Phase Walk in Workspace Builds

When `now` is invoked at a workspace root, layer discovery proceeds in
two phases for each module:

**Phase 1 — Upward walk (shared, computed once)**

Starting from the workspace root directory, `now` walks upward to
discover layers that apply to the entire workspace. This establishes the
**workspace base stack**.

Walk terminates at the first of:
- The user home directory (`~`).
- A VCS root (directory containing `.git`, `.hg`, `.svn`).
- `walk_boundary: stop` declared in `now-workspace.pasta` (see §34.13.3).

The workspace base stack is computed once and shared by all modules. It
is the same regardless of which module is being built.

**Phase 2 — Downward walk (per-module)**

Starting from the workspace root, `now` walks *downward* toward each
module directory, collecting any `.now-layer.pasta` files found along
the path segments between the workspace root and the module. These
module-local layers are appended to the workspace base stack, above the
shared layers and below the module's own `now.pasta`.

For a module at `services/auth/`:

```
~/.now/config.pasta layers     ← declared layers (enterprise, dept, team)
  ↓
[upward walk from workspace root]
  /repo/.now-layer.pasta        ← workspace root local layer (if present)
  ↓
[downward walk to module]
  /repo/services/.now-layer.pasta       ← services subtree layer (if present)
  /repo/services/auth/.now-layer.pasta  ← auth-specific layer (if present)
  ↓
/repo/services/auth/now.pasta   ← module project layer
```

A sibling module at `services/billing/` shares everything above
`services/` but does not see `services/auth/.now-layer.pasta`.

### 34.13.3 `walk_boundary` in `now-workspace.pasta`

```pasta
{
  workspace_mode: "monorepo",
  walk_boundary:  "stop",     ; "stop" | "continue"  default: "continue"
  modules: [...]
}
```

| Value | Behaviour |
|-------|-----------|
| `continue` | Upward walk proceeds above the workspace root (default). Layers above the repo root — e.g. a department `.now-layer.pasta` sitting in a parent directory — are discovered. |
| `stop` | Upward walk halts at the workspace root. Only declared layers (from `config.pasta`) and downward-walk layers apply. Useful when the workspace root is itself an organisational boundary and nothing above it should influence the build. |

The default is `continue` because the common case — a department or team
layer sitting above the VCS root in a developer's home directory structure
— should be discovered automatically without requiring every workspace to
opt in. `stop` is the explicit hardening option for workspaces that want
to be fully self-contained.

### 34.13.4 Divergent Stacks and Audit

In `aggregate` mode, different modules having different effective layer
stacks is expected and not flagged. `now layers:audit` reports the stack
for each module separately.

In `monorepo` mode, `now layers:audit` additionally reports **stack
divergence**: modules whose effective stack differs from the workspace
root's base stack (i.e. modules that picked up extra downward-walk
layers). Divergence in monorepo mode produces `NOW-W0406` per divergent
module. It does not block builds — it is an audit signal for workspace
maintainers to review whether the divergence is intentional.

```
Stack divergence warnings (monorepo mode):

  Module: services/auth
  Extra layers vs workspace base:
    services/auth/.now-layer.pasta
  Sections affected: @compile (sanitizers added)
  Code: NOW-W0406

  1 module(s) with divergent stacks.
  Run 'now layers:audit --per-module' for full per-module effective configs.
```

### 34.13.5 Path Canonicalisation

To handle symlinks, case-insensitive filesystems, and network mounts
reliably, `now` canonicalises all paths before comparing them during walk
boundary detection and downward path segment computation:

- Symlinks are resolved to their real path (`realpath(3)`) before any
  comparison.
- On case-insensitive filesystems (detected via a probe at startup), path
  comparison is case-insensitive.
- Network filesystem paths (detected via `statfs` mount type on Linux,
  `GetDriveType` on Windows) produce `NOW-W0407` (network filesystem
  detected — walk results may be unreliable) and fall back to
  `walk_boundary: stop` behaviour unless `workspace_mode` is explicitly
  declared, in which case the explicit declaration is trusted.

This is why `workspace_mode: "inferred"` is unreliable on unusual
filesystems and the explicit override exists.

### 34.13.6 New Error and Warning Codes

Extending §34.10:

| Code | Level | Meaning |
|------|-------|---------|
| `NOW-W0405` | warning | `workspace_mode` explicit value contradicts inference. |
| `NOW-W0406` | warning | Module has divergent layer stack vs workspace base (monorepo mode only). |
| `NOW-W0407` | warning | Network filesystem detected; walk falling back to `stop` boundary. |

---

## 34.14 Relation to Doc 31 (Schema)

Doc 31 §31.3 (root map schema) is extended by this document:

- `~/.now/config.pasta` gains a `layers: layer-entry[]?` field.
- `now-workspace.pasta` gains `walk_boundary: enum(stop, continue)?` and
  `workspace_mode: enum(monorepo, aggregate, inferred)?` fields.
- Layer documents themselves are not validated by the `now.pasta` schema —
  they have their own structure (sections with `_policy` metadata).
- `now schema:check` does not validate layer documents. `now layers:check`
  (§34.9) is the validation command for the layer stack.

Doc 32 (error catalogue) is extended with the codes in §34.10 and §34.13.6.



---



---

# Chapter 26 — Schema Reference

This document defines the formal schema for the `now.pasta` project
descriptor and specifies the validation rules `now` applies at startup.
It supersedes the informal field tables scattered across documents 01,
01b, and 01c, which remain the narrative reference but this document is
authoritative for type and constraint rules.

---

## 31.1 Design Goals

1. **Field-level errors** — validation must report the exact field path
   and the constraint violated, not "bad config file".
2. **Forwards compatibility** — unknown fields warn, never fail. An old
   `now` binary must still load a `now.pasta` written for a newer one.
3. **No external schema language** — the schema is defined here in prose
   and normative tables. A companion JSON Schema is published at
   `https://schema.now.build/now-1.0.json` for editor integration; that
   file is generated from this document, not the other way around.
4. **Structural, not semantic** — validation checks types, enums, and
   required fields. Semantic checks (e.g. "the dep coordinates you listed
   actually resolve") happen later, at the appropriate lifecycle phase.

---

## 31.2 Notation

Schema tables use the following type column values:

| Notation | Meaning |
|----------|---------|
| `string` | A Pasta or JSON string value. |
| `bool` | `true` or `false`. |
| `number` | A numeric value (integer or decimal). |
| `string[]` | An array of strings. |
| `map` | A Pasta map `{ ... }`. |
| `T?` | Optional — field may be absent. |
| `T!` | Required — field must be present. |
| `enum(a,b,c)` | String restricted to one of the listed values. |
| `semver` | String matching `MAJOR.MINOR.PATCH[-qualifier[-build]]` per doc 21 §21.2. |
| `coordinate` | String of form `group:artifact:version-range`. |
| `triple` | String of form `os:arch:variant` (doc 11). |
| `glob` | String following glob dialect of doc 26. |

---

## 31.3 Root Map

```
now.pasta root  →  map
```

| Field | Type | Notes |
|-------|------|-------|
| `group` | `string!` | Reverse-domain: `"org.example"`. No whitespace. |
| `artifact` | `string!` | Lowercase, hyphens allowed: `"my-lib"`. Pattern: `[a-z0-9][a-z0-9\-]*`. |
| `version` | `semver!` | See doc 21 §21.2. |
| `name` | `string?` | Display name. Defaults to `artifact`. |
| `description` | `string?` | One-line summary. |
| `url` | `string?` | Project homepage URL. |
| `license` | `string?` | SPDX identifier: `"MIT"`, `"Apache-2.0"`, etc. |
| `lang` | `string?` | Shorthand — sets `langs: ["<value>"]`. |
| `langs` | `string[]?` | Language IDs (doc 04c). `lang` and `langs` are mutually exclusive. |
| `std` | `string?` | Language standard (see §31.4). |
| `sources` | `source-set?` | Production source configuration. |
| `tests` | `source-set?` | Test source configuration. |
| `compile` | `compile-block?` | Compiler flags (see §31.6). |
| `link` | `link-block?` | Linker flags (see §31.7). |
| `msvc` | `msvc-block?` | MSVC-specific settings (doc 30). |
| `deps` | `dep[]?` | Declared dependencies. |
| `repos` | `repo[]?` | Registry endpoints. |
| `toolchain` | `toolchain-block?` | Toolchain selection (doc 07). |
| `profiles` | `profiles-map?` | Named build profiles. |
| `plugins` | `plugin-ref[]?` | Lifecycle plugins. |
| `tools` | `tool[]?` | Scripted and external tools (doc 14). |
| `targets` | `target-map?` | Named build targets / platform fan-out. |
| `assembly` | `assembly[]?` | Packaging descriptors (doc 17). |
| `reproducible` | `bool | reproducible-block?` | Reproducible build settings (doc 28). |
| `parent` | `coordinate?` | Inherited organisation descriptor (doc 01c). |
| `inherit` | `inherit-block?` | Selective inheritance from workspace root (doc 01c). |
| `volatile` | `bool?` | Marks module as volatile (doc 01c). |
| `build_options` | `build-options-block?` | Advanced build phase options (see §31.17). |
| `languages` | `language-def[]?` | Custom language type definitions (doc 04c). |

Unknown fields at root level: **warn** (code `NOW-W0001`), do not fail.

### 31.3.1 `lang` vs `langs`

`lang` is syntactic sugar for `langs` with one element. If both appear in
the same map, `now` errors with `NOW-E0101` (mutually exclusive fields).

The allowed language IDs are defined by doc 04c §4c.2 (built-ins) plus
any custom language declared in `languages:`. Built-in IDs: `c`, `c++`,
`gas`, `nasm`, `objc`, `modula2`, `fortran`. Unknown language IDs are
not a schema error at load time — they may refer to a plugin-defined
language — but they produce `NOW-E0302` during the `build` phase if no
language definition is found.

---

## 31.4 Language Standard Enum

`std` is validated against the active `lang`/`langs`. If `langs` contains
multiple languages, `std` applies to the primary one; per-language
standards are set via the `languages:` map (doc 04c).

| lang | Allowed `std` values |
|------|----------------------|
| `c` | `c89`, `c99`, `c11`, `c17`, `c23` |
| `c++` | `c++11`, `c++14`, `c++17`, `c++20`, `c++23` |
| `objc` | `c99`, `c11` (Objective-C baseline) |
| `fortran` | `f77`, `f90`, `f95`, `f2003`, `f2008`, `f2018` |
| others | Unrestricted — passed through as-is to the toolchain. |

Error on invalid value: `NOW-E0102` — "std `c14` is not valid for lang
`c`; expected one of: c89 c99 c11 c17 c23".

---

## 31.5 Source Set (`source-set`)

Applies to both `sources` and `tests`.

| Field | Type | Notes |
|-------|------|-------|
| `dir` | `string?` | Root directory for sources. Defaults to `src/main/<lang>` (sources) or `src/test/<lang>` (tests). |
| `headers` | `string?` | Public header root. Defaults to `src/main/h`. |
| `private` | `string?` | Private headers, not installed. |
| `pattern` | `glob?` | Source file glob, relative to `dir`. Default `**.<ext>` for each active language extension. |
| `include` | `(string | include-entry)[]?` | Explicit include list — overrides `pattern`. |
| `exclude` | `string[]?` | Exclude globs, relative to `dir`. |
| `overrides` | `override[]?` | Per-file flag overrides (doc 01b). |
| `generator` | `bool?` | If `true`, treat all matched files as generator inputs (doc 04b). |

Unknown fields: **warn** `NOW-W0001`.

### 31.5.1 Include Entry

An element of `include` may be a plain string (a glob) or a map:

| Field | Type | Notes |
|-------|------|-------|
| `pattern` | `glob!` | File pattern to include. |
| `compile` | `compile-block?` | Overrides for matching files. |

---

## 31.6 Compile Block (`compile-block`)

| Field | Type | Notes |
|-------|------|-------|
| `warnings` | `string[]?` | Warning flags without leading `-W`: `["Wall", "Wextra"]`. |
| `defines` | `string[]?` | Preprocessor defines. `"NAME"` or `"NAME=value"`. |
| `flags` | `string[]?` | Raw flags, passed through verbatim. Use sparingly. |
| `includes` | `string[]?` | Additional `-I` paths, relative to project root. |
| `std` | `string?` | Override `std` for this compile block only. |
| `opt` | `enum(none, debug, size, speed, lto)?` | Optimisation level. |
| `debug` | `bool?` | Enable debug info (`-g`). Default: `true` in `debug` profile. |
| `sanitizers` | `string[]?` | Sanitizer names: `["address", "undefined"]`. |
| `coverage` | `bool?` | Instrument for coverage (`--coverage`). |

Unknown fields: **warn** `NOW-W0001`.

### 31.6.1 Opt Level Mapping

| `opt` value | GCC/Clang flag | MSVC flag |
|-------------|---------------|-----------|
| `none` | `-O0` | `/Od` |
| `debug` | `-Og` | `/Od` |
| `size` | `-Os` | `/O1` |
| `speed` | `-O2` | `/O2` |
| `lto` | `-O2 -flto` | `/O2 /GL` |

---

## 31.6.2 Array Replacement with `!`-Prefixed Keys

In profiles, per-file compile overrides, and module inheritance, array
fields may be **replaced** rather than appended by prefixing the field
key with `!`:

```pasta
profiles: {
  strict: {
    compile: {
      !warnings: ["Wall", "Wextra", "Wpedantic"]
      ; Replace the entire warnings array, not append to it
    }
  }
}
```

The `!` prefix is a `now` merge directive applied during profile and
inheritance resolution. It is not part of the Pasta format itself —
`now` strips the `!` and treats the remainder as the field name. `!`
is a valid Pasta label character (per the Pasta BNF `labelsymbol`
production), so `!warnings` parses as a valid Pasta key.

`!`-prefixed keys are valid in:
- Profile blocks (`profiles.<n>.compile`, `profiles.<n>.link`)
- Per-file compile overrides (`sources.overrides[].compile`)
- Module `inherit` block values (where inheritance produces arrays)

They are **not** valid at the root level of `now.pasta` or in layer
documents — only in merge contexts where array accumulation is the
default behaviour and replacement is the exception.

This mechanism is distinct from the `!exclude:` value prefix used in
layer array exclusion (doc 34 §34.6.1), which is a string value
convention applied inside array elements, not a map key prefix.



---

## 31.7 Link Block (`link-block`)

| Field | Type | Notes |
|-------|------|-------|
| `flags` | `string[]?` | Raw linker flags. |
| `libs` | `string[]?` | System library names: `["m", "pthread"]`. |
| `libdirs` | `string[]?` | Additional `-L` paths. |
| `rpath` | `string[]?` | Runtime library paths. |
| `script` | `string?` | Linker script path (freestanding targets, doc 27). |
| `script_symbols` | `map?` | Properties injected into the linker script (doc 27). |
| `map` | `string?` | Path to write the linker map file. |
| `strip` | `bool?` | Strip symbols from output. Default: `false`. |

---

## 31.8 Dependency Entry (`dep`)

| Field | Type | Notes |
|-------|------|-------|
| `id` | `coordinate!` | Dependency coordinate. The version part is a range. |
| `scope` | `enum(compile, provided, test, runtime)?` | Default: `compile`. |
| `optional` | `bool?` | If absent, failure is non-fatal. Default: `false`. |
| `exclude` | `coordinate[]?` | Transitive deps to suppress. |
| `override` | `semver?` | Force a specific resolved version. |

### 31.8.1 Coordinate Validation

The `id` field must match the pattern:
```
group:artifact:version-range
```
where:
- `group` matches `[a-zA-Z0-9][a-zA-Z0-9.\-]*`
- `artifact` matches `[a-z0-9][a-z0-9\-]*`
- `version-range` is a valid version range per doc 12 §12.3

Error on malformed coordinate: `NOW-E0201`.

---

## 31.9 Repository Entry (`repo`)

Each element of the `repos` array may be either a **plain string** (a URL
shorthand) or a **map** with the full set of fields. Both forms are valid:

```pasta
; String shorthand — url only, all other fields at their defaults
repos: ["https://repo.now.build"]

; Map form — full control
repos: [
  {url: "https://repo.now.build", id: "central", release: true, snapshot: false}
]
```

A plain string is equivalent to `{url: "<string>", release: true, snapshot: false, auth: "none"}`.

| Field | Type | Notes |
|-------|------|-------|
| `url` | `string!` | Registry base URL: `"https://repo.now.build"`. |
| `id` | `string?` | Short ID for error messages. Defaults to the URL host. |
| `release` | `bool?` | Whether this repo serves release versions. Default: `true`. |
| `snapshot` | `bool?` | Whether this repo serves snapshot versions. Default: `false`. |
| `auth` | `enum(none, basic, token)?` | Authentication type. Default: `none`. |

---

## 31.10 Profiles Map (`profiles-map`)

```pasta
profiles: {
  release: { ... },
  debug:   { ... },
  custom:  { ... }
}
```

Profile names must match `[a-zA-Z_][a-zA-Z0-9_]*`. Reserved names that
`now` pre-defines: `debug`, `release`, `test`. User-defined profiles
may use any other name.

Each profile value is a **profile-block**:

| Field | Type | Notes |
|-------|------|-------|
| `compile` | `compile-block?` | Merged over the root compile block. |
| `link` | `link-block?` | Merged over the root link block. |
| `defines` | `string[]?` | Profile-level defines (shorthand, added to compile.defines). |
| `deps` | `dep[]?` | Additional deps active only in this profile. |
| `active_when` | `string?` | Condition expression (doc 01 §1.8). |

---

## 31.11 Plugin Reference (`plugin-ref`)

| Field | Type | Notes |
|-------|------|-------|
| `id` | `coordinate!` | Plugin artifact coordinate. |
| `hooks` | `string[]?` | Lifecycle hooks this plugin attaches to. See doc 14 §14.3. |
| `config` | `map?` | Plugin-specific configuration passed verbatim. |

---

## 31.12 Tool Entry (`tool`)

See doc 14 §14.2 for full semantics. Schema:

| Field | Type | Notes |
|-------|------|-------|
| `name` | `string!` | Tool identifier, used in hook references. |
| `type` | `enum(script, external, plugin)?` | Default: `script`. |
| `command` | `string?` | Command line or script body. |
| `hooks` | `string[]?` | Lifecycle hooks. |
| `parallel` | `bool?` | May run in parallel with sibling hooks. Default: `false`. |
| `continue_on_error` | `bool?` | Default: `false`. |

---

## 31.13 MSVC Block (`msvc-block`)

Full schema in doc 30. Summary:

| Field | Type | Notes |
|-------|------|-------|
| `runtime` | `enum(static, dynamic)?` | CRT selection: `/MT` vs `/MD`. |
| `pdb` | `bool?` | Generate PDB files. Default: `true` on Windows. |
| `exports` | `enum(declspec, def)?` | DLL export strategy. |
| `def_file` | `string?` | Path to `.def` file when `exports: def`. |
| `subsystem` | `enum(console, windows, native, efi)?` | Linker `/SUBSYSTEM`. |

---

## 31.14 Reproducible Block (`reproducible-block`)

| Field | Type | Notes |
|-------|------|-------|
| `timebase` | `string?` | See doc 28 §28.3. One of: `now`, `git-commit`, `zero`, ISO-8601 timestamp, or `${property}`. |
| `path_prefix_map` | `map?` | Absolute path remapping for debug info. |
| `no_date_macros` | `bool?` | Suppress `__DATE__` / `__TIME__`. Default: `true` when `reproducible: true`. |
| `sorted_inputs` | `bool?` | Sort linker input order. Default: `true`. |
| `verify` | `bool?` | Enable second-pass verification mode. Default: `false`. |

---

## 31.15 Assembly Entry (`assembly`)

See doc 17 for full semantics. Key fields:

| Field | Type | Notes |
|-------|------|-------|
| `id` | `string!` | Assembly identifier. |
| `format` | `enum(tgz, zip, lha, lzx, deb, rpm, dmg, nsis, flat)?` | Package format. |
| `include` | `(string | assembly-include)[]?` | Files to package. |
| `output` | `string?` | Output filename template. |
| `platform` | `triple?` | Restrict to this platform triple. |

---

## 31.16 Workspace Descriptor (`now-workspace.pasta`)

A workspace root (doc 01c) uses `now-workspace.pasta` at the repo root.
Its schema is a strict subset of the module `now.pasta`:

| Field | Type | Notes |
|-------|------|-------|
| `modules` | `string[]!` | Relative paths to module directories. |
| `inherit_defaults` | `inherit-defaults-block?` | Default compile/link settings for all modules. |
| `repos` | `repo[]?` | Workspace-wide repository list. |
| `toolchain` | `toolchain-block?` | Workspace-wide toolchain. |
| `volatile_modules` | `string[]?` | Module paths treated as volatile. |

### `inherit-defaults-block`

| Field | Type | Notes |
|-------|------|-------|
| `compile` | `compile-block?` | Applied to all modules unless opted out. |
| `link` | `link-block?` | Applied to all modules unless opted out. |

---

## 31.17 Validation Phases

`now` applies validation in three ordered phases. A failure in an earlier
phase does not prevent later phases from running (all errors are
collected), but phases after the third are not entered until all errors
are resolved.

### Phase 1 — Parse (during file load)

- File must be valid Pasta/JSON5/JSON per the format grammar.
- Root must be a map, not an array or scalar.
- Errors here produce `NOW-E0001` (parse failure) with line/column.

### Phase 2 — Structural validation (immediately after parse)

- Required fields present (`group`, `artifact`, `version`).
- Field types match the schema tables above.
- Enum fields hold allowed values.
- Unknown fields produce `NOW-W0001` (unknown field warning).
- Mutually exclusive fields produce errors (`NOW-E0101`).
- Coordinate syntax (`NOW-E0201`).
- Profile names valid identifiers (`NOW-E0301`).

### Phase 3 — Contextual validation (after toolchain selection)

- `std` value valid for active `lang` (`NOW-E0102`).
- `langs` IDs resolve to known language definitions (`NOW-W0002` for
  unknown — plugin-defined languages may register during `generate`).
- `toolchain.preset` names a known preset (`NOW-E0401`).

Phases 1–3 all complete before any lifecycle phase begins. All
discovered errors are printed together.

---

## 31.18 Unknown Field Warning (`NOW-W0001`)

When `now` encounters a field name not in the schema:

```
Warning NOW-W0001: now.pasta:14: unknown field 'optmize' in compile block
  Did you mean 'opt'?
  Unknown fields are ignored. Use 'now schema:check --strict' to treat them as errors.
```

The "did you mean" suggestion applies Levenshtein distance ≤ 2 over the
known field names for the same map scope.

`--strict` mode (or `strict_schema: true` in `~/.now/config.pasta`)
promotes `NOW-W0001` to `NOW-E0001`.

---

## 31.19 `now schema:check`

```
now schema:check [--strict] [--format pasta|json|text]
```

Validates the project descriptor and exits. Exit code:

| Outcome | Exit code |
|---------|-----------|
| Valid, no warnings | `0` |
| Valid with warnings | `0` (unless `--strict`) |
| Invalid | `1` |
| File not found | `2` |

With `--format json`:
```json
{
  "valid": false,
  "errors": [
    {
      "code": "NOW-E0102",
      "path": "std",
      "line": 7,
      "col": 8,
      "message": "std 'c14' is not valid for lang 'c'",
      "hint": "Expected one of: c89 c99 c11 c17 c23"
    }
  ],
  "warnings": []
}
```

---

## 31.20 Published JSON Schema

A JSON Schema document is published at:

```
https://schema.now.build/now-1.0.json
```

This schema is generated from this document during the `now` release
process. It is intended for IDE integration (VS Code, JetBrains) and for
linting tooling. It is informative, not normative — this document takes
precedence in any conflict.

The JSON Schema uses `"additionalProperties": false` within each block
to flag unknown fields in IDE underlines, but `now` itself only warns
(§31.18) to maintain forwards compatibility.

---

## 31.17 Build Options Block (`build-options-block`)

Controls advanced behaviour of the `build` phase. All fields are optional.

| Field | Type | Notes |
|-------|------|-------|
| `prescan` | `prescan-options?` | Pre-scan configuration (doc 33 §33.10). |

### `prescan-options`

| Field | Type | Notes |
|-------|------|-------|
| `timeout_ms` | `number?` | Per-file pre-scan timeout in milliseconds. Default: `10000`. |
| `parallel` | `bool?` | Scan files in parallel. Default: `true`. |
| `cache` | `bool?` | Cache results in `target/.now-prescan/`. Default: `true`. |

---


## 31.21 Supersession

This document supersedes the field tables in:

- Doc 01 §1.2 through §1.9 (identity, language, sources, compile, link,
  deps, repos, profiles) — those sections remain narrative; this document
  is the type authority.
- Doc 01b §1b.1 (source-set include/exclude fields) — superseded by
  §31.5 above.
- Doc 01c §1c.3 (workspace descriptor schema) — superseded by §31.16.
- Doc 28 §28.2 (reproducible block fields) — superseded by §31.14.
- Doc 30 §30.9 (msvc block schema) — superseded by §31.13.



---



---

# Chapter 27 — Error Catalogue

This document defines the structured error and warning codes `now` emits.
Every diagnostic has a stable numeric ID, a human-readable message
template, the phase in which it can occur, and a suggested remediation.

Machine-readable output (`--output json`) wraps every diagnostic in a
structured object; `--output text` (default) formats it for terminal
display with colour coding and source location.

---

## 32.1 Error Output Schema

### Text Output (default)

```
Error NOW-E0102: now.pasta:7:8: std 'c14' is not valid for lang 'c'
  Expected one of: c89 c99 c11 c17 c23
  → Remove or correct the 'std' field.
```

Format:
```
<Level> <Code>: <file>:<line>:<col>: <message>
  <detail>
  → <remediation>
```

Where `<Level>` is one of: `Error`, `Warning`, `Note`.

### JSON Output

```json
{
  "diagnostics": [
    {
      "level":    "error",
      "code":     "NOW-E0102",
      "phase":    "validate",
      "file":     "now.pasta",
      "line":     7,
      "col":      8,
      "message":  "std 'c14' is not valid for lang 'c'",
      "detail":   "Expected one of: c89 c99 c11 c17 c23",
      "hint":     "Remove or correct the 'std' field.",
      "related":  []
    }
  ]
}
```

`related` is an array of secondary locations relevant to the error (e.g.
the conflicting dep that forced a version incompatibility).

### Exit Codes

| Code | Meaning |
|------|---------|
| `0` | Success. |
| `1` | One or more errors produced. |
| `2` | Internal `now` failure (bug). |
| `3` | Input not found (no `now.pasta`, missing file). |
| `4` | Network error during `procure`. |
| `5` | Compiler or tool subprocess failed (the compiler's own exit code is in the structured output). |
| `10` | Test failures (all tests ran, some failed). |
| `11` | Test execution error (tests could not run). |

---

## 32.2 Error Code Ranges

| Range | Domain |
|-------|--------|
| `E00xx` | File I/O and parse |
| `E01xx` | Descriptor schema validation |
| `E02xx` | Dependency and coordinate |
| `E03xx` | Build phase: source and language |
| `E04xx` | Toolchain |
| `E05xx` | Compilation subprocess |
| `E06xx` | Link subprocess |
| `E07xx` | Test |
| `E08xx` | Plugin and tool |
| `E09xx` | Packaging and assembly |
| `E10xx` | Registry, publish, procure |
| `E11xx` | Signing and trust |
| `E12xx` | Cache and offline |
| `E13xx` | Workspace and module |
| `E14xx` | Advisory and security |
| `E15xx` | Layer system |
| `E20xx` | Internal / unexpected (bugs) |
| `W00xx` | Schema warnings |
| `W01xx` | Dependency warnings |
| `W02xx` | Build warnings |
| `W03xx` | Security and advisory warnings |
| `N00xx` | Informational notes |

---

## 32.3 File I/O and Parse Errors (`E00xx`)

### NOW-E0001 — Parse failure

**Phase**: load  
**Message**: `<file>:<line>:<col>: <format> parse error: <detail>`  
**Detail**: The raw error from the Pasta/JSON5/JSON parser.  
**Remediation**: Fix the syntax error at the indicated location.

### NOW-E0002 — Descriptor not found

**Phase**: load  
**Message**: `No project descriptor found in <dir>`  
**Detail**: Looked for `now.pasta`, `now.json5`, `now.json`.  
**Remediation**: Run `now sketch` to create a starter `now.pasta`, or `cd` to the project root.

### NOW-E0003 — Ambiguous descriptor

**Phase**: load  
**Message**: `Ambiguous project descriptor: found <list> in <dir>`  
**Remediation**: Remove all but one, or run `now convert` to consolidate.

### NOW-E0004 — Root not a map

**Phase**: load  
**Message**: `<file>: root value must be a map, got <type>`  
**Remediation**: The outer structure must be `{ ... }`.

### NOW-E0005 — File unreadable

**Phase**: load  
**Message**: `Cannot read <file>: <os-error>`  
**Remediation**: Check file permissions.

---

## 32.4 Schema Validation Errors (`E01xx`)

### NOW-E0101 — Mutually exclusive fields

**Phase**: validate  
**Message**: `<file>:<line>: '<fieldA>' and '<fieldB>' are mutually exclusive`  
**Example**: `lang` and `langs` both present.  
**Remediation**: Remove one of the two fields.

### NOW-E0102 — Invalid enum value

**Phase**: validate  
**Message**: `<file>:<line>:<col>: '<value>' is not valid for field '<field>'`  
**Detail**: `Expected one of: <list>`  
**Remediation**: Use one of the listed values.

### NOW-E0103 — Wrong type

**Phase**: validate  
**Message**: `<file>:<line>:<col>: field '<field>' expects <expected-type>, got <actual-type>`  
**Remediation**: Supply the correct type.

### NOW-E0104 — Required field missing

**Phase**: validate  
**Message**: `<file>: required field '<field>' is missing`  
**Remediation**: Add the field.

### NOW-E0105 — Invalid artifact name

**Phase**: validate  
**Message**: `<file>:<line>: artifact '<value>' does not match pattern [a-z0-9][a-z0-9-]*`  
**Remediation**: Use lowercase letters, digits, and hyphens only.

### NOW-E0106 — Invalid group name

**Phase**: validate  
**Message**: `<file>:<line>: group '<value>' contains invalid characters`  
**Remediation**: Use reverse-domain format: `org.example`.

### NOW-E0107 — Profile name invalid

**Phase**: validate  
**Message**: `<file>:<line>: profile name '<value>' must match [a-zA-Z_][a-zA-Z0-9_]*`

### NOW-E0108 — Unknown `active_when` syntax

**Phase**: validate  
**Message**: `<file>:<line>: cannot parse condition '<expr>' in profile '<name>'`

---

## 32.5 Dependency and Coordinate Errors (`E02xx`)

### NOW-E0201 — Malformed coordinate

**Phase**: validate  
**Message**: `<file>:<line>: dependency id '<value>' is not a valid coordinate`  
**Detail**: Must be `group:artifact:version-range`.  
**Remediation**: Correct the coordinate syntax (see doc 12).

### NOW-E0202 — Version range parse failure

**Phase**: validate  
**Message**: `<file>:<line>: version range '<range>' is not valid`  
**Detail**: The specific parse error.  
**Remediation**: Use semver range syntax: `^1.0.0`, `>=2.0 <3.0`, exact `1.2.3`.

### NOW-E0203 — No version satisfies constraints

**Phase**: procure  
**Message**: `Cannot resolve <group>:<artifact>`: no version satisfies all constraints`  
**Detail**: Lists the conflicting constraints and their sources.  
**Related**: Each dep entry contributing a constraint.  
**Remediation**: Relax a constraint, or use `convergence: exact` with an explicit override.

### NOW-E0204 — Convergence conflict

**Phase**: procure  
**Message**: `Version convergence conflict for <group>:<artifact>`  
**Detail**: `Requires <v1> (from <source1>) and <v2> (from <source2>); policy is 'exact'`  
**Remediation**: Add a `convergence: highest` or explicit `override` field.

### NOW-E0205 — Yanked version selected

**Phase**: procure  
**Message**: `<group>:<artifact>:<version> has been yanked from the registry`  
**Detail**: Yanked reason if available.  
**Remediation**: Update the version constraint to exclude the yanked version.

### NOW-E0206 — Dependency cycle

**Phase**: procure  
**Message**: `Dependency cycle detected: <A> → <B> → ... → <A>`  
**Remediation**: Break the cycle — likely a structural problem in the dependency tree.

### NOW-E0207 — Private group violation

**Phase**: procure  
**Message**: `Dependency '<group>:<artifact>' matches private_group prefix '<prefix>' but was found on public registry '<url>'`  
**Remediation**: This is a potential dependency confusion attack. Add the correct private registry, or remove the package from the public registry.

---

## 32.6 Build Phase Errors (`E03xx`)

### NOW-E0301 — Language definition not found

**Phase**: build (graph construction)  
**Message**: `Language '<id>' declared in now.pasta has no definition`  
**Detail**: No built-in definition and no plugin registered one during `generate`.  
**Remediation**: Check the language ID spelling, or ensure the plugin defining it is listed in `plugins`.

### NOW-E0302 — Source directory not found

**Phase**: build  
**Message**: `Source directory '<dir>' does not exist`  
**Remediation**: Create the directory, or correct `sources.dir` in `now.pasta`.

### NOW-E0303 — No sources matched

**Phase**: build  
**Message**: `Pattern '<glob>' in sources matched no files in '<dir>'`  
**Remediation**: Check the pattern (doc 26) and the source directory.

### NOW-E0304 — Source file type unresolvable

**Phase**: build  
**Message**: `Cannot classify source file '<file>': no language type matches extension '<ext>'`  
**Remediation**: Add the extension to an existing language type, or add a custom language definition.

### NOW-E0305 — Generator output conflict

**Phase**: build (generate)  
**Message**: `Two generators both claim to produce '<output-file>'`  
**Remediation**: Check generator declarations for overlapping output paths.

### NOW-E0306 — Pre-scan failure

**Phase**: build (pre-scan)  
**Message**: `Pre-scan of '<file>' failed: <detail>`  
**Detail**: The raw pre-scan tool error.  
**Remediation**: Fix the source file or the pre-scan configuration (doc 33).

### NOW-E0307 — Module dependency cycle (source level)

**Phase**: build (pre-scan)  
**Message**: `Source-level module cycle: <A.cppm> → <B.cppm> → <A.cppm>`  
**Remediation**: Restructure the C++ module graph to eliminate the cycle.

---

## 32.7 Toolchain Errors (`E04xx`)

### NOW-E0401 — Toolchain preset not found

**Phase**: validate / toolchain init  
**Message**: `Toolchain preset '<name>' is not defined`  
**Detail**: Available presets: `gcc`, `llvm`, `msvc`, `tcc`.  
**Remediation**: Correct the preset name, or define a custom toolchain block (doc 07).

### NOW-E0402 — Compiler not found

**Phase**: toolchain init  
**Message**: `Compiler '<path>' not found on PATH or at explicit path`  
**Remediation**: Install the compiler, or set the toolchain path explicitly.

### NOW-E0403 — Target triple not supported

**Phase**: toolchain init  
**Message**: `Triple '<os:arch:variant>' is not supported by toolchain '<preset>'`  
**Remediation**: Install a cross-compiler, or declare the platform in `~/.now/config.pasta` (doc 27).

### NOW-E0404 — Sysroot not found

**Phase**: toolchain init  
**Message**: `Sysroot '<path>' does not exist`  
**Remediation**: Install the sysroot or correct the path.

---

## 32.8 Compilation Subprocess Errors (`E05xx`)

### NOW-E0501 — Compilation failed

**Phase**: build  
**Message**: `Compilation of '<file>' failed (exit code <n>)`  
**Detail**: Compiler stderr, presented with source location if parseable.  
**Note**: The compiler's own output appears verbatim below the `now` error
frame. `now` does not suppress or reformat compiler messages.

### NOW-E0502 — Assembler failed

**Phase**: build  
**Message**: `Assembly of '<file>' failed (exit code <n>)`

### NOW-E0503 — Generator tool failed

**Phase**: generate  
**Message**: `Generator '<tool>' failed on '<input>' (exit code <n>)`

### NOW-E0504 — Compilation timeout

**Phase**: build  
**Message**: `Compilation of '<file>' timed out after <n>s`  
**Remediation**: Increase `toolchain.compile_timeout`, or investigate why this file takes unusually long.

---

## 32.9 Link Errors (`E06xx`)

### NOW-E0601 — Link failed

**Phase**: link  
**Message**: `Link of '<output>' failed (exit code <n>)`  
**Detail**: Linker stderr verbatim.

### NOW-E0602 — Missing object file

**Phase**: link  
**Message**: `Expected object file '<path>' not produced by compile phase`  
**Remediation**: Usually a sign of a suppressed `E0501`; check compiler output.

### NOW-E0603 — Linker script not found

**Phase**: link  
**Message**: `Linker script '<path>' not found`  
**Remediation**: Correct the `link.script` path in `now.pasta`.

### NOW-E0604 — Memory budget exceeded

**Phase**: link  
**Message**: `Memory budget exceeded in region '<name>': used <n> bytes, limit <limit> bytes`  
**Detail**: Top contributors to the region by size.  
**Remediation**: Optimise for size (`opt: size`), or increase the budget in the platform descriptor (doc 27).

---

## 32.10 Test Errors (`E07xx`)

### NOW-E0701 — Test binary failed to start

**Phase**: test  
**Message**: `Test binary '<path>' could not be executed: <os-error>`

### NOW-E0702 — Test suite timed out

**Phase**: test  
**Message**: `Test suite '<name>' timed out after <n>s`

### NOW-E0703 — Test adapter mismatch

**Phase**: test  
**Message**: `Test adapter '<adapter>' could not parse output from '<binary>'`  
**Remediation**: Ensure the binary uses the matching test framework format (TAP/JUnit/Unity).

### NOW-E0704 — Coverage instrumentation failed

**Phase**: test  
**Message**: `Coverage data could not be collected from '<binary>'`

---

## 32.11 Plugin and Tool Errors (`E08xx`)

### NOW-E0801 — Plugin protocol version incompatible

**Phase**: generate (first hook invocation)  
**Message**: `Plugin '<id>' speaks protocol <plugin-version>, now supports up to <now-version>`  
**Remediation**: Update the plugin or downgrade `now`.

### NOW-E0802 — Required capability not available

**Phase**: generate  
**Message**: `Plugin '<id>' requires capability '<cap>' which is not available in this build of now`

### NOW-E0803 — Plugin hook timed out

**Phase**: any  
**Message**: `Plugin '<id>' hook '<hook>' timed out after <n>s`

### NOW-E0804 — Plugin output malformed

**Phase**: any  
**Message**: `Plugin '<id>' returned invalid Pasta: <detail>`

### NOW-E0805 — Tool command not found

**Phase**: any  
**Message**: `Tool '<name>' command '<cmd>' not found on PATH`

---

## 32.12 Packaging and Assembly Errors (`E09xx`)

### NOW-E0901 — Assembly include not found

**Phase**: package  
**Message**: `Assembly '<id>': pattern '<glob>' matched no files`

### NOW-E0902 — Assembly format tool not found

**Phase**: package  
**Message**: `Assembly format '<fmt>' requires external tool '<tool>' which was not found`  
**Remediation**: Install the tool (e.g. `lha`, `makensis`).

### NOW-E0903 — Duplicate assembly id

**Phase**: validate  
**Message**: `Two assembly entries share id '<id>'`

---

## 32.13 Registry and Procure Errors (`E10xx`)

### NOW-E1001 — Registry unreachable

**Phase**: procure  
**Message**: `Registry '<url>' is unreachable: <network-error>`  
**Remediation**: Check network connectivity, or use `--offline` mode.

### NOW-E1002 — Integrity check failed

**Phase**: procure  
**Message**: `Integrity check failed for '<group>:<artifact>:<version>': expected <hash>, got <hash>`  
**Remediation**: The download may be corrupt or tampered. Retry; if it persists, report to the registry operator.

### NOW-E1003 — Authentication failed

**Phase**: procure  
**Message**: `Authentication to '<url>' failed`  
**Remediation**: Check credentials in `~/.now/credentials.pasta`.

### NOW-E1004 — Publish pre-flight failed

**Phase**: publish  
**Message**: `Publish pre-flight check failed: <detail>`  
**Detail**: Specific check that failed (version already exists, missing signature, etc.).

---

## 32.14 Signing and Trust Errors (`E11xx`)

### NOW-E1101 — Signature verification failed

**Phase**: procure / install  
**Message**: `Signature verification failed for '<group>:<artifact>:<version>'`  
**Detail**: Key ID and trust level.  
**Remediation**: The artifact may be tampered. If expected, add the publisher's key to the trust store.

### NOW-E1102 — Trust level insufficient

**Phase**: procure  
**Message**: `<group>:<artifact>:<version> has trust level '<level>' but policy requires '<required>'`

### NOW-E1103 — Signing key not found

**Phase**: publish  
**Message**: `No signing key found for group '<group>'`  
**Remediation**: Run `now trust:key --generate` or `now trust:key --import`.

---

## 32.15 Cache and Offline Errors (`E12xx`)

### NOW-E1201 — Offline and not cached

**Phase**: procure  
**Message**: `'<group>:<artifact>:<version>' is not in the local cache and now is in offline mode`  
**Remediation**: Run `now procure` in online mode to warm the cache, then retry offline.

### NOW-E1202 — Lock drift in locked mode

**Phase**: procure  
**Message**: `Lock file drift detected in locked mode: '<dep>' has changed`  
**Remediation**: Do not modify `now.lock.pasta` manually. Run `now procure` in online mode to regenerate.

---

## 32.16 Workspace and Module Errors (`E13xx`)

### NOW-E1301 — Module cycle

**Phase**: workspace load  
**Message**: `Module dependency cycle: <A> → <B> → ... → <A>`  
**Remediation**: Break the intra-workspace module dependency cycle.

### NOW-E1302 — Module directory not found

**Phase**: workspace load  
**Message**: `Module directory '<path>' listed in now-workspace.pasta does not exist`

### NOW-E1303 — Module missing descriptor

**Phase**: workspace load  
**Message**: `Module directory '<path>' has no now.pasta`

### NOW-E1304 — Volatile promotion blocked

**Phase**: workspace load  
**Message**: `Cannot promote volatile module '<name>': it still has unresolved volatile deps`  
**Remediation**: Resolve all volatile transitive deps before running `now volatile:promote`.

---

## 32.17 Advisory and Security Errors (`E14xx`)

### NOW-E1401 — Advisory blocks phase

**Phase**: procure / generate / test  
**Message**: `Advisory <CVE-ID> (severity: <level>) blocks phase '<phase>' for '<dep>'`  
**Detail**: Advisory summary and affected version range.  
**Remediation**: Update the dep to a patched version, or use `--force --reason "<text>"` to override (doc 29).

### NOW-E1402 — Force reason insufficient

**Phase**: any (forced)  
**Message**: `--force reason '<text>' is too short or generic`  
**Detail**: Reason must be at least 20 characters and not one of the blocked generic phrases.  
**Remediation**: Provide a specific, substantive reason for the override.

### NOW-E1403 — Forced receipt expired

**Phase**: install  
**Message**: `Forced install receipt for '<dep>' expired on <date>`  
**Remediation**: Re-evaluate the advisory status and re-force if still intentional.

### NOW-E1404 — Advisory feed signature invalid

**Phase**: advisory update  
**Message**: `Advisory feed signature from '<url>' is invalid`  
**Remediation**: The feed may be compromised. Do not proceed without verifying the feed source.

---

## 32.18 Layer System Errors (`E15xx`)

### NOW-E1501 — Project listed in layers

**Phase**: validate / layer load
**Message**: `layers[] must not contain 'project' — the project layer is always implicit`
**Remediation**: Remove `project` from the `layers` list in `~/.now/config.pasta`.

### NOW-E1502 — Duplicate layer ID

**Phase**: validate / layer load
**Message**: `Duplicate layer id '<id>' in layer stack`
**Remediation**: Each layer must have a unique `id`.

### NOW-E1503 — Layer not a section document

**Phase**: layer load
**Message**: `Layer '<id>': document root must use @section syntax, not a plain map`
**Remediation**: Use `@section-name { ... }` at the root of the layer document.

### NOW-E1504 — Layer source unreachable

**Phase**: layer load
**Message**: `Layer '<id>' source '<source>' is unreachable: <detail>`
**Remediation**: Check network connectivity, file path, or registry availability.

### NOW-E1505 — Layer version requirement not met

**Phase**: layer load
**Message**: `Layer '<id>' requires now >= <version>, current is <current>`
**Remediation**: Upgrade `now`, or use an older version of the layer.

---

## 32.19 Internal Errors (`E20xx`)

### NOW-E2001 — Unexpected internal error

**Phase**: any  
**Message**: `Internal error in <component>: <detail>`  
**Detail**: Stack trace or panic information.  
**Remediation**: This is a `now` bug. Please report it with the full output of `now --verbose`.

### NOW-E2002 — Build graph invariant violated

**Phase**: build  
**Message**: `Build graph invariant violated: <detail>`  
**Remediation**: Report as a bug.

---

## 32.20 Warnings

### NOW-W0001 — Unknown field

**Phase**: validate  
**Message**: `<file>:<line>: unknown field '<name>' in <context>`  
**Detail**: Did-you-mean suggestion if Levenshtein distance ≤ 2.  
**Note**: Warning only. Unknown fields are ignored for forwards compatibility.

### NOW-W0002 — Unknown language ID

**Phase**: validate  
**Message**: `Lang '<id>' is not a built-in language and no plugin has registered it yet`  
**Note**: Downgraded from error because plugin-defined languages register at `generate` time.

### NOW-W0101 — Using snapshot dependency in release build

**Phase**: procure  
**Message**: `Dependency '<coord>' is a snapshot; release builds should use stable versions`

### NOW-W0102 — Dependency with no version constraint

**Phase**: procure  
**Message**: `Dependency '<group>:<artifact>:*' uses wildcard version — no stability guarantee`

### NOW-W0201 — Non-deterministic build option detected

**Phase**: build  
**Message**: `Compiler flag '<flag>' may produce non-deterministic output; incompatible with reproducible: true`

### NOW-W0301 — Advisory present but not blocking

**Phase**: procure  
**Message**: `Advisory <CVE-ID> (severity: <level>) affects '<dep>' but does not block at this severity`  
**Note**: Informational. Configure `phase_guards` in `now.pasta` to promote to error (doc 29).

---

## 32.21 Informational Notes

### NOW-N0001 — Using cached artifact

**Phase**: procure  
**Message**: `Using cached '<group>:<artifact>:<version>' (no network request)`

### NOW-N0002 — Selective rebuild

**Phase**: build  
**Message**: `Skipping <n> unchanged source files (incremental build)`

### NOW-N0003 — Profile active

**Phase**: any  
**Message**: `Active profile: '<name>'`

---

## 32.22 Layer System Warnings (`W04xx`)

These codes are defined by doc 34 (Cascading Configuration Layers) and
listed here for catalogue completeness.

### NOW-W0401 — Advisory lock violation

**Phase**: layer merge  
**Message**: `<layer>:<section>: overrides locked section from '<locking-layer>'`  
**Detail**: Field(s) affected and `_override_reason` if provided.  
**Note**: Warning only. Does not block builds. Appears in `now layers:audit` output and as a trailing block after each build summary.

### NOW-W0402 — Exclusion attempted in locked section

**Phase**: layer merge  
**Message**: `<layer>:<section>: exclusion of '<value>' ignored — section is locked by '<locking-layer>'`  
**Note**: The excluded value is retained.

### NOW-W0403 — Unknown section name in layer

**Phase**: layer load  
**Message**: `<layer>: section '@<name>' is not a well-known section name`  
**Note**: Custom sections are allowed; this is informational.

### NOW-W0404 — Local layer file changed

**Phase**: build startup / `layers:check`  
**Message**: `Layer file '<path>' has changed since last effective configuration was computed`  
**Remediation**: Run `now layers:check` to revalidate.

### NOW-W0405 — Workspace mode contradicts inference

**Phase**: workspace load  
**Message**: `workspace_mode: '<declared>' declared but inference would conclude '<inferred>'`  
**Note**: Explicit declaration is used. Warning exists to flag surprising repo layouts.

### NOW-W0406 — Divergent module stack (monorepo mode)

**Phase**: workspace load  
**Message**: `Module '<path>' has a divergent layer stack vs workspace base`  
**Detail**: Lists extra layers the module picks up via downward walk.  
**Note**: Informational in monorepo mode. Run `now layers:audit --per-module` for full detail.

### NOW-W0407 — Network filesystem detected

**Phase**: layer discovery  
**Message**: `Network filesystem detected at '<path>' — walk falling back to stop boundary`  
**Remediation**: Declare `workspace_mode` and `walk_boundary` explicitly in `now-workspace.pasta`.

### NOW-W0408 — Unresolvable exclusion stub

**Phase**: layer merge  
**Message**: `_exclude stub in '<layer>' has no identity field — nothing removed`  
**Detail**: Stub contents shown for diagnosis.  
**Remediation**: Add `id:`, `url:`, `name:`, or `coordinate:` to the stub to identify the target entry.

### NOW-W0409 — Ambiguous exclusion

**Phase**: layer merge  
**Message**: `_exclude stub in '<layer>' matched <n> accumulated entries — all removed`  
**Remediation**: Ensure array entries have unique identity field values.

---

## 32.23 Adding New Error Codes

When `now` is extended, new codes must:

1. Fall within an appropriate range (§32.2), or use a new range allocation.
2. Be documented here with message template, phase, and remediation.
3. Include at least one test case in the `now` test suite.
4. Not reuse a retired code number — retired codes are listed in appendix A (to be maintained as the implementation matures).

New warning codes follow the same process but are lighter-weight — a one-line
entry in the relevant `W0xxx` range suffices for initial introduction.



---

