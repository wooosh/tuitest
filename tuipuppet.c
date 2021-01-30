#include "keymap.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pty.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <termios.h>
#include <poll.h>

#include <vterm.h>
#include <openssl/sha.h>

void die(const char* msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

static struct termios origTermios;
void restore_termios() {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &origTermios);
}

void setup_termios() {
  if (tcgetattr(STDIN_FILENO, &origTermios) == -1)
    die("tcgetattr");
  atexit(restore_termios);

  struct termios raw = origTermios;
  // set up the terminal to not print characters we type or response to escape
  // codes
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

pid_t exec_in_pty(int master, int slave, char** argv) {
  pid_t pid = fork();
  
  if (pid == 0) {
    close(master);
    
    // redirect process io to terminal device
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
    
    close(slave);

    execvp(argv[0], argv);
  }

  close(slave);

  return pid;
}

int main(int argc, char** argv) {
  // parse arguments
  bool show_terminal = false;
  if (argc > 2 && strcmp(argv[1], "-r") == 0) {
    show_terminal = true;
    argv++;
    argc--;
  }

  if (argc != 3) {
    fprintf(stderr, "Usage: %s [-r] <commandfile> <executable> <arguments to executable>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  FILE* stream = fopen(argv[1], "r");
  if (stream == NULL) {
    perror("fopen");
    exit(EXIT_FAILURE);
  }

  // vterm initialization
  struct winsize term_size = {43, 132};
  VTerm *vt = vterm_new(term_size.ws_col, term_size.ws_row);
  vterm_set_utf8(vt, 1);

  VTermScreen *vts = vterm_obtain_screen(vt);
  vterm_screen_reset(vts, 1);

  // resize host terminal and set termios properly
  if (show_terminal) {
    fprintf(stderr, "\x1b[8;%zu;%zut", term_size.ws_row, term_size.ws_col);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &origTermios);
  }

  // TODO: split into own function and have master side return
  // master side accepts output from process and provides input to it
  int master, slave;
  openpty(&master, &slave, NULL, NULL, &term_size);

  // shift the first two arguments, so that only the executable name and its arguments remain
  argv += 2;
  pid_t process = exec_in_pty(master, slave, argv);

  struct pollfd pollfds[] = {
    {STDIN_FILENO, POLLIN},
    {master, POLLIN}
  };

  short *master_events = &pollfds[0].revents;
  short *stdin_events = &pollfds[1].revents;

  while (1) {
    // TODO: move waitpid and select into their own functions
    // check if process is still alive
    if (waitpid(process, NULL, WNOHANG) == process) {
      // TODO: how to handle early exits?
      break;
    }

    // TODO: configurable timeout
    // check if any terminal output or stdin input is available for 500ms
    int ready = poll(pollfds, 2, 500);
    
    if (ready == -1) {
      die("poll");
    }

    // check the terminal for process output
    if (*master_events != 0) {
      if (*master_events & POLLIN) { // input available
        char buf[4096];
        ssize_t size = read(master, buf, 4096);
        if (size == -1) {
          die("read");
        }
        vterm_input_write(vt, buf, size);
      
        if (show_terminal) {
          // TODO: error handling on everything
          write(STDERR_FILENO, buf, size);
        }

        if (vterm_output_get_buffer_current(vt) > 0) {
          size = vterm_output_read(vt, buf, 4096);
          write(master, buf, size);
        }
      } else { // POLLERR | POLLHUP
        break;
      }
    }


    // TODO: put command handler into it's own function
    // handle command
    char* line = NULL;
    size_t len = 0;

    // TODO: handle getline returning error properly
    if (getline(&line, &len, stream) == -1) continue;

    char* cmd = strtok(line, " \n");
    if (strcmp(cmd, "keystream") == 0) {
      char* keyname;
      while (1) {
        keyname = strtok(NULL, " \n");
        if (keyname == NULL) break;

        // check if control key
        // TODO: proper keybind parser
        if (strncmp(keyname, "<ctrl-", strlen("<ctrl-")) == 0) {
          char key = keyname[strlen("<ctrl-")];
          // apply control modifier
          key &= 0x1f;
          write(master, &key, 1);
          goto found_key;
        }

        // translate key if applicable
        struct key_pair *kp = keymap;
        for (struct key_pair *kp = keymap; kp->key != NULL; kp++) {
          if (strcmp(kp->key, keyname) == 0) {
            write(master, kp->val, strlen(kp->val));
            goto found_key;
          }
        }
        // if key not in keymap
        write(master, keyname, strlen(keyname));
        found_key:;
      }
    } else if (strcmp(cmd, "hash") == 0) {
      SHA_CTX ctx;
      SHA1_Init(&ctx);

      //VTermPos cursorpos;
      //vterm_state_get_cursorpos(vt_state, &cursorpos);
      //SHA1_Update(&ctx, &cursorpos, sizeof(VTermPos));

      vts = vterm_obtain_screen(vt);
      VTermState *vt_state = vterm_obtain_state(vt);

      for (int x = 0; x < term_size.ws_col; x++) {
        for (int y = 0; y < term_size.ws_row; y++) {
          VTermScreenCell cell;
          vterm_screen_get_cell(vts, (VTermPos){y, x},&cell);
          SHA1_Update(&ctx, cell.chars, cell.width*4);
        }
      }

      unsigned char hash[SHA_DIGEST_LENGTH];
      SHA1_Final(hash, &ctx);
      write(STDOUT_FILENO, hash, SHA_DIGEST_LENGTH);
    }
    // TODO: handle unknown command
  }

  close(master);

  // cleanup
  fclose(stream);
  vterm_free(vt);

  return 0;
}