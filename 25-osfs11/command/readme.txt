noxisOS - a tiny 32-bit operating system for the emulator.

This text file was shipped inside cmd.tar and extracted into
the root directory by INIT at boot, so the desktop file
explorer and the TTY "cat" command have something to show.

Files usually found in /:
  kernel.bin   the 32-bit kernel
  cmd.tar      the install archive (emptied after boot)
  echo         tiny user program
  pwd          tiny user program
  demo         graphics demo (TASK_GFX)
  desktop      starts the GUI desktop (TASK_DESKTOP)
  hdldr.bin    hard-disk boot helper
  dev_tty0..2  terminal device files
  readme.txt   this file

Desktop tips:
  * Files window: Up/Down to pick, Enter to view, Enter or q to go back.
  * TTY window:   Up/Down recalls the last 8 commands; Down restores a draft.
  * TTY commands: help, clear, echo, ver, uptime, ls, cat <file>, end.
  * end (or poweroff) really turns the machine off.
  * ESC closes the desktop and returns to the text shell.
