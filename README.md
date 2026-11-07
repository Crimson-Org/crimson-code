<p align="center">
  <img src="./assets/icons/scalable/apps/io.github.CrimsonOrg.CrimsonCode.svg" alt="Crimson Code logo" width="128" height="128">
</p>

<h1 align="center">Crimson Code</h1>
<p align="center">A code editor for Linux, with language servers, a terminal and tasks built in.<br>
Write code the easy way, or switch to a Vim inspired mode with one click.</p>

<p align="center">
  <a href="https://gtk.org">
    <img src="https://img.shields.io/badge/Toolkit-GTK4-4a86cf?logo=gtk&logoColor=white" alt="GTK4">
  </a>
  <a href="https://gnome.pages.gitlab.gnome.org/libadwaita/">
    <img src="https://img.shields.io/badge/Style-libadwaita-3584e4?logo=gnome&logoColor=white" alt="libadwaita">
  </a>
  <a href="https://wayland.freedesktop.org/">
    <img src="https://img.shields.io/badge/Session-Wayland-ffbc00?logo=wayland&logoColor=white" alt="Wayland">
  </a>
</p>

<p align="center">
  <a href="https://github.com/Crimson-Org/crimson-code/releases/latest"><b>Download Here!</b></a>
</p>

<p align="center">
  <img src="./demo1.png" alt="Crimson Code with a C project open">
</p>

## Why

Crimson Code opens in an instant, even with large files, and keeps every tool out of the way until needed.

## About

### Language servers

Opening a file starts the matching language server, such as [clangd](https://clangd.llvm.org/) for C or [Pyright](https://microsoft.github.io/pyright/) for Python.\
Errors and warnings appear beside the line numbers, and hovering shows the details.\
Add any other server's command in Settings.

### Plans

TODO and plan files show as tickable lists, next to the TODO and FIXME notes in your code.

### Tasks

Tasks build, run and test your project.\
Each task is also saved as a script, which runs without Crimson Code.

### Hex editor

Binary files open in a hex editor.\
Search for text or bytes, replace them, jump to an offset, and read or change the value under the cursor as a number or text.

### Vim mode

One click switches to a Vim inspired mode, with commands like `:w`, `:q` and `:e`.

## Building

You need a [C compiler](https://gcc.gnu.org/), [pkg-config](https://www.freedesktop.org/wiki/Software/pkg-config/) and the development files for [GTK4](https://gtk.org), [libadwaita](https://gnome.pages.gitlab.gnome.org/libadwaita/) and [GtkSourceView](https://gitlab.gnome.org/GNOME/gtksourceview).\
On [openSUSE](https://www.opensuse.org/) these come from `sudo zypper install gcc pkg-config gtk4-devel libadwaita-devel gtksourceview5-devel`.

Steps:
 1. Clone using `git clone --recurse-submodules https://github.com/Crimson-Org/crimson-code`
 2. Run `sh .crimson-build/install.sh`

`sh .crimson-build/uninstall.sh` uninstalls Crimson Code.
