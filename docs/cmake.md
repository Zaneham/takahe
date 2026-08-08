# Using Takahe from CMake

[← back to README](../README.md)

Takahe builds with its own Makefile; there is no CMake build of the tool
itself. This is the other direction, a CMake project consuming an installed
Takahe to synthesise designs as part of its build.

## Installing

```sh
make
sudo make install                 # /usr/local by default
make install PREFIX=$HOME/.local  # or wherever
```

That puts `takahe` in `<prefix>/bin`, the token and cell definitions in
`<prefix>/share/takahe/defs`, the message catalogues in
`<prefix>/share/takahe/lang`, and the package config in
`<prefix>/lib/cmake/Takahe`. `DESTDIR` is honoured for staged installs.

The PDK libraries under `lib/` are not installed. They are 78 MB of
third-party Liberty data and `--lib` takes an explicit path, so point at
wherever you keep them.

## Finding it

```cmake
find_package(Takahe 0.1 REQUIRED)
```

If the prefix is not one CMake already searches, point it there with
`-DCMAKE_PREFIX_PATH=<prefix>`.

While Takahe is on 0.x the minor version has to match, so asking for 0.1 is
satisfied by 0.1.2 but not by 0.2. That is deliberate: 0.x is where things are
still allowed to move. From 1.0 the usual same-major rule takes over.

You get `Takahe_VERSION`, `Takahe_EXECUTABLE`, `Takahe_DEFS_DIR`,
`Takahe_LANG_DIR`, and a `Takahe::takahe` imported target.

## Synthesising a design

```cmake
takahe_add_design(cpu_netlist
    SOURCE  rtl/cpu.sv
    FORMAT  yosys
    LIB     ${CMAKE_SOURCE_DIR}/pdk/sky130.lib)
```

That adds a target `cpu_netlist` that runs `takahe` at build time. The output
path comes back as a `TAKAHE_NETLIST` target property and as a
`cpu_netlist_OUTPUT` variable in the calling scope:

```cmake
add_custom_command(TARGET place_and_route PRE_BUILD
    COMMAND nextpnr-ice40 --json ${cpu_netlist_OUTPUT} ...)
```

### Arguments

| | |
|---|---|
| `SOURCE` | The input design, `.sv`, `.vhd` or `.abl`. |
| `FORMAT` | `yosys` (default), `blif`, `verilog`, or `fpga` for nextpnr iCE40 JSON. |
| `LIB` | Liberty `.lib` for technology mapping. |
| `RADIX` | 2 binary, 3 ternary, 12 dozenal, and the rest. |
| `TMR` / `TMR_FULL` | Radiation hardening, triplicated flops or everything. |
| `EQUIV` | Prove the optimised netlist equivalent to the unoptimised one. |
| `BUDGET` | Refuse to emit above a live cell count. |
| `OUTPUT` | Output path. Defaults to `<name>` in the build dir with a suffix from the format. |
| `OPTIONS` | Any other flags, handed to `takahe` untouched. |
| `DEPENDS` | Extra files to rebuild on. |
| `EXCLUDE_FROM_ALL` | Build only when something asks for it. |

### One thing to know

`takahe` writes no depfile, so editing a file the design includes will not
retrigger a build on its own. List those in `DEPENDS` if you need it.
