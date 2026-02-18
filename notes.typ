#set page(paper: "a4", margin: 2cm)
#set text(size: 16pt)

= new knowledge when building text editor

== ```c <unistd.h>``` (Unix Standard)

`unistd.h` is one of the most important and fundamental header files in
Unix/Linux system programming. As long as you want to make a system call, you
have to include it.

=== ```c read()```

- ```ssize_t read(int fildes, void *buf, size_t nbyte);```
  - `fildes`: File Descriptor, an index ID assigned by the operating system
    kernel to manage files opened by the currently running process.
- *read()* attempts to read _nbyte_ bytes of data from the object referenced by
  the descriptor _fildes_ into the buffer pointed to by _buf_.

==== File Descriptors

In Unix/Linux systems, the operating system tracks open files and resources
using integers called *File Descriptors*. When any program starts, the OS
automatically opens three descriptors for it:
#table(
  columns: (auto, auto, auto, auto),
  table.header([fd], [Macros], [Purpose], [Default Device]),
  [0], [`STDIN_FILENO`], [Standard Input], [Keyboard],
  [1], [`STDOUT_FILENO`], [Standard Output], [Screen (Terminal)],
  [2], [`STDERR_FILENO`], [Standard Error], [Screen (Terminal)],
)

== *modes*

In the world of Unix/Linux terminal programming, "modes" describe how the
*Terminal Driver* (the software between your keyboard and the priogram)
processes input before sending it to your application

You can set them by using the `<termios.h>` struct flags:
+ *Canonical*: Default state (Flag `ICANON` is on)
+ *Raw*: You must manually disable several flags:
  - Disable `ICANON` (turn off the buffering)
  - Disable `ECHO` (turn off printing)
  - Disable `ISIG` (turn off signals like `Ctrl+C`)
+ *Cbreak*: Disable `ICANON`, but keep `ISIG` enabled

=== Canonical Mode (Cooked Mode)

This is the *default* mode for most command-line applications.
- How it works: Input is *Line Buffered*
  - When you type, the data goes into a temporary buffer in the OS
  - The program (via `read()`) receives nothing until you press `Enter`
- Features:
  - *Line Editing*: The user can use `Backspace` to fix mistakes. The terminal
    driver handles this locally; the program never sees the deleted characters
  - *Signal Processing*: Special keys like `Ctrl+C` (SIGINT) or `Ctrl+Z` (SIGSTP)
    are intercepted by the system to terminate or suspend the program

=== Raw Mode

This is the mode used by text editors and full-screen applications
- How it works: Input is *Unbuffered*
  - Data is sent to the priogram byte-by-byte, immediately upon keypress
- Features:
  - *No Processing*: The terminal driver passes everything directly.
  - *No Automatic Echo*: In Canonical mode, typing 'a' displays 'a'. In Raw
    mode, you usually turn this off so the priogram controls exactly what is
    drawn on the screen.
  - *No Signals*: For example, `Ctrl+C` is no longer a "kill" command; it is
    just a byte. the priogram must decide what to do with it
  - *CR/LF*: Pressing Enter sends a Carriage Return (`\r`, 13). It does not
    automatically become a Newline (`\n`, 10)

=== Cbreak Mode (Rare Mode)

This is a hybrid mode, sometimes referred to as "Rare" mode (medium-well, as
opposed to raw or cooked)
- How it works: It is *Unbuffered*, but it keeps *Signal processing*
- Features:
  - the priogram gets input immediately (no waiting for Enter)
  - However, `Ctrl + C` will still kill the program instantly
- Use Case: Programs that need instant reaction but don't need full control over
  the keyboard, like `top`

== Escaping

In terminal programming, "Escaping" is the mechanism used to send *commands* to
the terminal screen through the same channel used for *text*.

Since we have only one "pipe" (the standard output) to send data to the screen,
we need a way to tell the terminal: "Don't print the next few characters; use
them to move the cursor or change the color instead"

=== The Escape Character `\x1b`

The Escape Character is the trigger that starts a command
- *ASCII Value*: `27` or `0x1b`
- *Representation*: In C, it is written as `\x1b`. In documentation, you often
  see it written as `^[`
- *Function*: When the terminal driver sees this byte, it stops printing
  characters to the screen and enters a "command state", waiting for a specific
  sequence of characters to follow
- *CSI*: `\x1b[` is the *Control Sequence Introducer*, which is the most common
  way to start an ANSI Escape Sequence.

=== The Escape Sequence

- An *Escape Sequence* is the full string of characters that follows the Escape
character to perform a specific task.
- Escape sequences always start with an escape character (`\x1b`) followed by a
`[`. Escape sequences instruct the terminal to do various text
formatting tasks, such as coloring text, moving the cursor around, and
clearing parts of the screen
- Escape sequence commands take arguments, which come before the command
hide the cursor before refreshing the screen

=== Common Examples

#table(
  columns: (auto, auto, auto),
  table.header([Sequence], [Name], [Effect]),
  [`\x1b[2J`], [Clear Screen], [Erases everything currently on the display],
  [`\x1b[?25l`], [Hide Cursor], [Makes the cursor invisible],
  [`\x1b[?25h`], [Show Cursor], [Makes the cursor visible again],
)

== ```C restrict```

In C programming, ```C restrict``` is a *type qualifier* (like ```C const``` or
```C volatile```) introduced in the C99 standard. It is used exclusively with
*pointers*

When you declare a pointer with ```C restrict```, you are giving a "solemn
promise" to the compiler: For the lifetime of this pointer, only this pointer
(or a value derived directly from it) will be used to access the data it points
to.

=== Example: ```C memcpy()``` vs ```C memmove()```

==== ```C memcpy()``` (Use ```C restrict```)

This function is designed for speed. It assumes the source and destination
buffers do not overlap. Because of ```C restrict```, the compiler can optimize
the copy process aggressively

```C
void *memcpy(void *restrict dest, const void *restrict src, size_t n);
```

==== ```C memmove()``` (Does Not use ```C restrict```)

If your buffers might overlap, you must use ```C memmove()```. Since
```C restrict``` is absent, the compiler takes extra precautions to ensure data
isn't corrupted during the shift, making it slightly slower than
```C memcpy()```

```C
void *memmove(void *dest, const void *src, size_t n);
```

== ```C realloc()```

```C realloc()``` *resizes a previously allocated memory block*. It is
essentially a "dynamic resizing" tool that tries to be as efficient as
possible by avoiding unnecessary data copying.
+ ```C realloc()``` check the memory immediately following the current block at
  first.
  - If there is enough space directly after current block, the allocator simply
    claims that extra space, updates its internal metadata, and returns the same
    pointer. No data is moved.
+ If there is no enough space behind current block, ```C realloc()``` must find
  a new home for the data
  - It looks for a completely new, large enough hole in the heap, and then
    allocates a new block. It copies the contents of the old block to the new
    one, and then automatically calls ```C free()``` on the old pointer.
    Finally, it returns the new pointer to the new memory location
+ If we call ```C realloc()``` with a smaller size, the allocator usually marks
  the "tail" of the old block as free so it can be used for future allocations

== ```C enum```

In C and C++, an enumeration is a user-defined type that assigns names to
integer constants, making the code more readable and easier to maintain.
For example:
```C
enum editorKey {
  ARROW_LEFT = 'h', // 104, the ASCII code for 'h'
  ARROW_RIGHT = 'l',
  ARROW_UP = 'k',
  ARROW_DOWN = 'j',
};
```

```C
enum editorKey {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,// 1001
  ARROW_UP,   // 1002
  ARROW_DOWN, // 1003
};
```

== Variadic Function

```C
// ... makes it a variadic function
// It accepts a *format string* followed by a variable number of arguments,
//  which mimics the behavior of standard functions like `printf`
//  - `fmt`: The fixed starting argument, like "saved %d lines"
//  - `...`: The variable arguments that fill the placeholders in `fmt`
void editorSetStatusMessage(const char *fmt, ...) {
    // declare a variable (argument pointer) to traverse the list of extra
    //  arguments
    va_list ap;
    // initialize `ap` to point to the first argument after `fmt`, this is why
    //  you must always have at least one named parameter
    va_start(ap, fmt);
    // int vsnprintf(char * restrict str, size_t size, const char * restrict
    //  format, va_list ap);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    // cleans up the memory associated with the argument list traversal
    va_end(ap);
}

// We can call it as follows:
editorSetStatusMessage("Hello, %s! This is %d.", name, year);
```
