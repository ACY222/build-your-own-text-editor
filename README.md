# Kilo

A minimal, lightweight, and dependency-free terminal text editor written entirely in standard C.

This project is an implementation and personal adaptation of the famous [Build Your Own Text Editor](https://viewsourcecode.org/snaptoken/kilo/) tutorial, originally inspired by antirez's `kilo`. It bypasses standard buffered I/O, interacting directly with the terminal via VT100/ANSI escape sequences to render text, handle cursor movement, and manage state.

## Features

* **Dependency-Free:** Built strictly with the C Standard Library and POSIX APIs (`<termios.h>`, `<unistd.h>`).
* **Raw Terminal I/O:** Manually handles terminal canonical mode switching and escape sequence parsing.
* **Incremental Search:** Real-time forward and backward string matching across the file buffer.
* **Syntax Highlighting:** Context-aware coloring for C/C++ keywords, numbers, strings, single and multi-line comments.

## File Structure

* `kilo.c`: The monolithic source code containing the core editor logic, state machine, and rendering engine.
* `Makefile`: Build configuration for compiling the editor with standard optimizations.
* `notes.typ`: New knowledge learned while completing the project.
