# now — Implementation Guide

**Version**: 1.1 — v1 Scoped  
**A narrative guide to building the `now` native build tool from scratch**

---

This guide tells you what to build, in the order you should build it, and
why each decision was made the way it was. It is not a specification — the
specification is `now-spec.md`. This is the story of how the pieces fit
together, written for the engineer who is going to implement them.

Read this first. Keep the spec open beside it.

---

## Part I — What `now` Is

### The Core Idea

`now` is a build tool for native languages — C, C++, assembly, and their
companions — that borrows the best ideas from Maven and applies them to a
world that Maven was never designed for. Like Maven, it has a declarative
project descriptor, a lifecycle with well-defined phases, a dependency
resolution model, and a registry. Unlike Maven, it produces machine code,
not JVM bytecode.

The central file is `now.pasta`. It sits at the root of every project and
answers every question about the project: what language it is written in,
what it produces, what it depends on, and how to build it. The tooling
reads this file and derives everything else.

The format is Pasta — Plain And Simple Text Archive. It looks like a
relaxed JSON with semicolon comments and unquoted keys. You will learn to
love it because it is genuinely simple: maps, arrays, strings, numbers,
booleans, and null. No anchors, no tags, no special forms.

### Why Not Make? Why Not CMake?

Make is an assembly language for builds. It gives you maximum control and
maximum opportunity to get it wrong. The average C project's Makefile is
a custom language interpreter that someone rewrote badly. CMake is a
build system generator — it generates Makefiles or Ninja files, which
means you are one level removed from what actually runs. Both require
explicit dependency management: you tell the tool what depends on what.

`now` inverts this. You describe what you have (sources, headers, deps),
and `now` figures out what to do. The build graph is derived from the
project model, not written by hand. Incremental builds are correct by
construction, not by convention.

The cost is that `now` is opinionated. It has a conventional layout, a
defined lifecycle, and a registry model. If your project fits the model —
and most C projects do — the experience is dramatically simpler than Make
or CMake. If it doesn't fit, you use plugins and custom tools to bridge
the gap.

---

## Part II — Where to Start: The Foundation

### Build This First: Pasta Parsing

Before you write a single line of build logic, you need a Pasta parser.
Everything `now` reads and writes is Pasta: `now.pasta`, `now.lock.pasta`,
`layers.lock.pasta`, plugin manifests, daemon IPC, CI output. It is the
substrate of the entire system.

The Pasta reference implementation is available at
`https://github.com/IridiumFX/Pasta` and is written in C. Use it. It is
small, correct, and battle-tested. The grammar is deliberately minimal —
five value types, two containers, comments, whitespace. The C library
gives you `pasta_parse`, `pasta_write`, and a tree of `PastaValue*`
nodes. That is genuinely all you need.

Two features added specifically for `now` are worth understanding early:

**Multiline strings** (`"""..."""`) are used for embedded script bodies
and inline linker scripts. A string containing newlines is written with
triple quotes; the writer chooses this form automatically. You never need
to think about it — the parser and writer handle it.

**Sections** (`@name { ... }`) are used in workspace descriptors and
layer documents. A sectioned document has multiple named root containers
instead of one. The layer system (Chapter 25 of the spec) makes heavy use
of sections as the unit of per-layer configuration. When you reach that
chapter, sections will feel natural.

### The Project Object Model

Once you can parse Pasta, load a `now.pasta` into a Project Object Model
— an in-memory struct that represents the project. This is the heart of
`now`. Every command reads it first.

The POM has a natural hierarchy: identity at the top, then language, then
sources, then compile/link settings, then dependencies. The spec (Chapter 1)
lists every field with its type and default. Implement them in order of
frequency of use: identity, language, sources, compile, deps, output.
Profiles and properties come later.

A crucial early decision: field merge order. Profiles overlay the base
configuration. Layer-derived defaults underlie everything. Inheritance
flows from workspace root to module. The merge rules are consistent
throughout: scalars replace, arrays accumulate, maps deep-merge. The
`!`-prefix key mechanism lets any array field opt out of accumulation and
force replacement instead. Get this right early, because every subsequent
feature builds on it.

### The Directory Layout

Establish the conventional layout immediately. When a user runs `now init`,
they get a `src/main/c/`, `src/test/c/`, `src/main/include/`, and a
minimal `now.pasta`. When `now build` runs, it looks for sources in the
right place automatically. When it produces output, it goes to `target/`.

The `target/` directory is your workspace. Everything in it is derived and
can be deleted. The object file naming convention — `parser.c.o` rather
than `parser.o` — prevents collisions when different source files have the
same basename in different subdirectories. Use the full relative path.

---

## Part III — The Core Build Loop

### The Lifecycle

`now`'s lifecycle is: `procure → generate → build → link → test → package
→ install → publish`. Each phase is a verb. Each phase implies all prior
phases in its lifecycle. Running `now test` runs procure, generate, build,
link, and then test — in that order.

The old `compile` and `assemble` phases have been unified into `build`.
This unification matters conceptually: a `.c` file and a `.s` file both
go through the same `build` phase, dispatched to different tools by the
language type system. There is no special-casing by extension at the phase
level.

The `clean` lifecycle (`clean → vacate`) is independent. `now clean` deletes
`target/`. `now vacate` removes installed deps from the local repo. You
can run `now clean build` to run both lifecycles in sequence.

### The Language Type System

The language type system is how `now` knows what to do with each source
file. Every language is a named type definition that specifies file
extensions, the tool to invoke, and the flag mapping. `now` ships built-in
type definitions for C, C++, x86/ARM/RISC-V assembly, Ada, Fortran,
Modula-2, and Pascal. Projects can register custom types for languages `now`
doesn't know.

The classification algorithm is three-layer: explicit declaration in
`now.pasta` wins over project language and convention, which wins over
file-level override. When two languages claim the same extension (`.m` is
both Objective-C and Modula-2), the project `langs` field resolves the
ambiguity.

Build this type system as a registry. Each entry maps extensions to a tool
invocation spec. The `build` phase iterates the source set, classifies
each file through the registry, and dispatches. The type system also
declares which languages need the module pre-scan protocol (Chapter 13 of
the spec) — C++20 modules, Ada, Modula-2, and Fortran all need it.

### The Build Graph

Before any file is compiled, `now` constructs a directed acyclic graph of
compilation units. Nodes are source files; edges are dependencies —
includes, module imports, generated file relationships. The graph
determines parallel execution order: independent nodes run concurrently,
dependent nodes wait.

For languages with inter-file module dependencies (C++20 modules, Ada
packages, Modula-2 modules), the graph cannot be constructed from the
source set alone. This is where the **module pre-scan** runs: a fast
first pass that reads each source file just enough to discover `import`,
`USE`, or `with` declarations, then adds those edges to the graph before
compilation begins.

The pre-scan runs incrementally. Results are cached in
`target/.now-prescan/{lang}/{source-hash}.deps`. If the source hasn't
changed, the cached result is used. Only changed files are re-scanned.

The build graph is not a Makefile. It is an in-memory DAG that `now`
constructs fresh each run, validated against the incremental manifest to
determine which nodes need work. Nodes whose inputs match the manifest are
skipped. Nodes that need work are dispatched to a thread pool.

### Incremental Builds

The incremental manifest at `target/.now-manifest` is a Pasta file with
one entry per source file. Each entry records everything that can affect
the compiled output: the source SHA-256, the SHA-256 of every
transitively included header, the complete compiler flag string, the tool
binary SHA-256, and the output `.o` SHA-256.

When a source file is considered for (re)compilation, `now` computes the
current values of all inputs and compares them to the manifest. If
everything matches, the file is skipped. If anything differs, the file
is recompiled and the manifest entry is updated.

This gives you correct incremental builds without Make or Ninja. The
correctness invariant: if the manifest says a file is up to date, it
is up to date — no staleness is possible because you track every input
including the tool binary itself. A compiler upgrade forces a full rebuild
automatically.

---

## Part IV — Dependencies and the Registry

### Coordinates and the Lock File

Every artifact in the `now` ecosystem is identified by a coordinate:
`group:artifact:version`. Groups are reverse-domain names. Artifacts are
lowercase identifiers with hyphens. Versions are semantic versions with
optional qualifiers.

The lock file `now.lock.pasta` records the exact resolved coordinates,
SHA-256 hashes, and installed descriptor hashes of every dependency in
the build, per target triple. The logical primary key per entry is
`(id, triple)` — a project targeting multiple platform triples has one
entry per coordinate per triple, each with a fully resolved concrete URL.
The lock is committed to VCS and is the source of truth for reproducible
builds across all declared targets. On a machine where all deps are
cached, `now build --offline` uses the lock file without touching the
network.

Implement the lock file early. It is the dependency resolution output and
the procure phase input. Every dep operation — download, install, vacate
— operates on lock file entries, not `now.pasta` entries.

### Dependency Resolution

Resolution is a two-phase process. In the first phase, `now` traverses the
dependency graph breadth-first, collecting version constraints. Each dep
entry specifies a version range (`^1.3` means compatible with 1.3.x,
`>=1.0 <2.0` is explicit). When the same artifact appears multiple times
with different ranges, the ranges are intersected to find a compatible
version.

In the second phase, the resolved graph is checked for conflicts. Three
convergence policies control what happens when an exact version cannot be
agreed upon: `lowest` (take the minimum satisfying version), `highest`
(take the maximum), or `exact` (any conflict is a hard error). The default
is `highest`.

Once resolved, the complete concrete version set is written to
`now.lock.pasta`. Subsequent builds use the lock file directly, bypassing
the resolution algorithm.

### The Registry Protocol

The registry exposes a simple HTTP API: `GET /resolve/{group}/{artifact}/{range}`
returns available matching versions, and `GET /artifact/.../{filename}` fetches
the archive. Publication is `PUT /artifact/...` with authentication.

The local repo (`~/.now/repo/`) is always checked first. If a dep at the
locked exact version is already installed with matching SHA-256, `now`
skips the download entirely. This makes repeated builds fast and makes
offline operation possible for any project whose deps have ever been
installed on the machine.

### Dependency Confusion Protection

When enterprise organisations host private registries with packages named
under their own group prefixes, there is a risk that a public registry
could be used to serve a maliciously-named package with the same
coordinate. `now` prevents this through the `private_groups` mechanism:
group prefixes declared private are only ever resolved from configured
private registries, never from public ones.

This protection runs before network access. If a dep's group matches a
private prefix and only public registries are configured, the build fails
with a clear error rather than silently fetching from the wrong place.

---

## Part V — Configuration at Scale

### Toolchains

A toolchain is a named preset that specifies the compiler, assembler,
linker, and archiver for a particular platform and language family.
`now` ships presets for `gcc`, `llvm`, `arm-gcc`, `riscv-gcc`, and
`msvc`. Each preset knows the flag mapping: how `now`'s abstract
compile settings (warnings, defines, optimisation level) translate to
concrete compiler flags.

The MSVC case is worth special attention because MSVC's flag vocabulary
is different from GCC/Clang. The spec devotes an entire chapter to the
complete translation table. When implementing multi-platform support,
do not try to abstract over GCC and MSVC with a single flag set —
instead, implement the translation table faithfully. The flags that have
no equivalent are explicitly noted as such.

### Multi-Architecture Builds

`now` models build targets with platform triples: `{os}:{arch}:{abi}`.
For example, `linux:x86_64:gnu`, `linux:aarch64:musl`, `macos:aarch64:none`,
`windows:x86_64:msvc`. The triple determines which toolchain preset is
selected, which library directory is used from installed deps, and where
output goes within `target/`.

Cross-compilation is handled by selecting a non-host triple. `now` then
resolves all deps for the target triple (some deps ship pre-compiled for
multiple platforms in a single archive; `now` selects the right platform
directory from the installed layout). The toolchain for the target triple
is looked up from the configured cross-compilation presets.

Fan-out builds — `now build --target linux:*:musl` — run the build for
all known architectures matching the wildcard. This is how you build and
test a C library across your entire platform matrix in one command.

### The Cascading Layer System

The layer system is the organisational configuration mechanism. Where
`now.pasta` is per-project, layers are per-organisation, per-department,
or per-team. A layer is a Pasta document that uses `@section` names at its
root. Sections correspond to areas of `now.pasta` configuration: `@compile`,
`@repos`, `@toolchain`, `@advisory`, and so on.

Layers stack in priority order, from `now`'s own shipped baseline up
through enterprise, department, team, and filesystem-walk layers, to the
project's own `now.pasta` at the top. Each layer may declare its sections
as `open` (lower layers can override freely) or `locked` (overrides are
allowed but produce an audit warning). Arrays in locked sections accumulate
only — lower layers can add but not remove.

The filesystem walk discovers `.now-layer.pasta` files by walking up the
directory tree from the project. In workspace builds, this walk has two
phases: upward from the workspace root (establishing a shared base stack)
and downward to each module (allowing module-local extensions). The result
is that each module in a large aggregated workspace can have a different
effective layer configuration, reflecting the different teams that own
different parts of the codebase.

The exclusion mechanism for array layers uses two conventions: `!exclude:value`
for string arrays, and `_exclude: true` inside a map-element stub for
arrays of maps. Both are implemented in `now` at merge time; neither
requires any Pasta language changes. The `_exclude` sentinel is stripped
from the effective configuration before the build sees it.

---

## Part VI — Safety, Trust, and Security

### Signing and Trust

Every artifact published to the `now` registry is signed with the
publisher's key. Consumers verify the signature before installation.
The trust store maps group prefixes to trusted keys, so `com.acme:*`
can only be installed if the artifact is signed by Acme's registered key.

The trust model is hierarchical: the `now` central registry has a root
of trust, organisations register their keys with the registry, and
consumers trust the registry's key list by default. Organisations can also
distribute their key directly for air-gapped environments.

### Reproducible Builds

When `reproducible: true` is set in a project, `now` activates a set of
measures that eliminate non-determinism from the build output. This
includes replacing `__DATE__` and `__TIME__` macros with values derived
from the latest git commit timestamp, sorting file lists before passing
them to the linker, and stripping embedded build paths.

The reproducibility guarantee: given the same `now.lock.pasta` and source
tree, every build on every machine produces a byte-identical output.
This is verifiable with `now reproducible:check`, which builds twice and
compares checksums.

### Advisory Phase Guards

The advisory system integrates vulnerability information into the build
lifecycle. `now` checks each locked dependency against a security advisory
database during the `procure` phase. Advisories are classified by severity
and matched against installed version ranges.

The phase guard configuration controls what happens when an advisory is
found: `error` halts the build, `warn` continues with a warning, `note`
records it silently. The defaults — `critical: error`, `high: warn` — are
established in the shipped `now-baseline` layer and can be tightened by
enterprise layers. Any forced override of a locked advisory configuration
is recorded in an audit trail, visible via `now advisory:audit`.

---

## Part VII — The Extended Ecosystem

### Plugins

Plugins extend `now` at defined lifecycle hooks. They are standalone
executables that communicate with `now` via a Pasta-over-stdin/stdout
protocol. Declaring a plugin in `now.pasta` causes it to be procured from
the registry like a dependency.

Each plugin ships a `plugin.pasta` manifest that declares which hooks it
handles, what capabilities it needs (filesystem write, network access, etc.),
and its concurrency constraints (whether it can run alongside other plugins
or needs exclusive access). The capability model is what prevents a
malicious plugin from having more access than it declared.

### IDE Integration and the Stay Daemon

`now stay` is a long-running daemon that watches the project for changes
and rebuilds incrementally. It provides a structured event stream for IDEs:
build-started, source-changed, error, build-complete. The IDE adapter
(Language Server Protocol bridge) translates this event stream into LSP
notifications.

The daemon also exports the compile database — `compile_commands.json` —
used by `clangd` and other language servers for accurate symbol resolution
and semantic highlighting. Every time a source file's compile flags change,
the daemon regenerates the compile database automatically.

### Embedded and Freestanding Platforms

For targets without a standard operating system — microcontrollers, OS
kernels, bare-metal hardware — `now` supports freestanding output types
and custom memory models. A freestanding project specifies a custom linker
script (inline in `now.pasta` using a multiline string, or as a separate
file), a startup object, and typically disables the standard library.

Platform definitions for exotic targets (Amiga 68k, RISC-V Nova CPU) are
registered as custom platform entries in `now.pasta`. The platform entry
specifies the toolchain prefix, output format, entry point, and any
platform-specific link flags. Once registered, the platform is a first-class
build target, usable with `--target amiga:m68k:none` like any other.

---

## Part VIII — Implementation Order

If you are building `now` from scratch, here is the recommended order.
Steps marked **[Post-v1]** are fully specified but not required for the
initial release — complete the v1 core first.

**v1 Core (target C and C++ on Linux, macOS, Windows):**

1. **Pasta parser** — the foundation; nothing else works without it.
2. **POM loader** — parse `now.pasta` into a struct. Implement field merge (profiles, `!` prefix).
3. **Directory layout** — establish `target/`, `src/`, the object naming convention.
4. **Language type system** — the extension registry; dispatch to tools. Implement C and C++ only.
5. **Build phase** — invoke compiler, produce `.o` files. No incrementality yet.
6. **Incremental manifest** — add the `.now-manifest` check. Now builds are fast.
7. **Link phase** — archiver for static, linker for shared/executable.
8. **Dependency resolution** — version ranges, convergence, lock file.
9. **Procure phase** — download, verify SHA-256, install to local repo.
10. **Registry** — HTTP client for resolve and fetch (see `cookbook`). Publish comes later.
11. **Test phase** — compile test sources, link against project objects, execute.
12. **Build graph and parallelism** — DAG construction, thread pool, selective rebuild.
13. **Module pre-scan** — for C++20 modules; extend to Ada, Modula-2, Fortran post-v1.
14. **Plugins and generate phase** — plugin procure, manifest validation, IPC protocol.
15. **Packaging and install** — tarball assembly, local repo extraction.
16. **Multi-architecture** — triple syntax, toolchain selection, fan-out (Linux/macOS/Win triples).
17. **Workspace and modules** — root descriptor, inheritance, module graph.
18. **Layer system** — baseline, org/team layers, section merge, audit trail.
19. **Signing and trust** — key registration, signature verification, trust store.
20. **Reproducible builds** — determinism measures, `now reproducible:check`.
21. **Advisory guards** — vulnerability database integration, phase guard config.
22. **CI integration** — structured output, cache key helpers, `now ci` lifecycle.
23. **MSVC support** — flag translation table, Visual Studio toolchain preset.
24. **Dependency confusion protection** — private group enforcement, registry ordering.
25. **Layer audit and governance** — `now layers:audit`, per-module stack reporting.
26. **Publish** — registry HTTP PUT, authentication, pre-publish checks.

**Post-v1 extensions (fully specified, implement after v1 is stable):**

27. **IDE integration** *(Ch. 19)* — compile database, `now stay` daemon, LSP bridge.
28. **Embedded platforms** *(Ch. 23)* — freestanding output, custom linker scripts, platform registry.
29. **Additional language types** *(Ch. 4)* — Ada, Fortran, Modula-2, Pascal, and others.

Each v1 step is independently testable. After step 5 you can build a C file.
After step 9 you can build a project with dependencies. After step 12 you
have the full core build loop. Everything from step 13 onward is an
extension of a working system.

---

## Appendix: Key Design Decisions

**Why Pasta and not TOML or JSON?** Pasta is smaller, has semicolon
comments (important for config files), and has no schema gotchas like
TOML's datetime type or JSON's lack of trailing commas. The C library is
embeddable without dependencies. The Pasta team added multiline strings
and sections specifically to support `now`'s needs.

**Why `!`-prefix for array replacement instead of a separate syntax?**
Because `!` is already a valid Pasta label character. The merge directive
is a `now` convention layered on top of normal Pasta, not a Pasta extension.
This means layer documents and `now.pasta` are both valid Pasta without
any parser modifications.

**Why advisory lock violations are warnings, not errors?** Because an
organisation that deploys a locked advisory config cannot anticipate every
legitimate project exception. The governance model is transparency, not
prevention: the exception shows up in `now layers:audit` and must be
justified with an `_override_reason`, but it does not block the build. A
team that legitimately needs to use an older vulnerable version of a library
(while a fix is in progress) should not be blocked — they should be audited.

**Why does `build` subsume `compile` and `assemble`?** Because they were
never meaningfully different at the lifecycle level. Both phases transformed
source files into object files, dispatched by file extension. Unifying them
into `build` eliminates the split conceptually and removes a footgun:
previously, `now compile` would not process `.s` files, which confused
projects with mixed C and assembly. Now `now build` handles everything.

**Why does the pre-scan run inside the `build` phase rather than as its
own phase?** Because it is not a user-visible operation — it is an
implementation detail of how the build graph is constructed for languages
with inter-file module dependencies. Making it a separate phase would
require users to understand when to invoke it, which breaks the phase
abstraction. It runs automatically, transparently, and incrementally.
