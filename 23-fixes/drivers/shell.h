#ifndef SHELL_H
#define SHELL_H

/* Parse and dispatch a single line of user input (without a trailing newline).
 * Called by the keyboard driver when the user presses ENTER. */
void shell_run(char *input);

#endif
