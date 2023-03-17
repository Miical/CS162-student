#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

int cmd_exit(struct tokens* tokens);
int cmd_help(struct tokens* tokens);
int cmd_pwd(struct tokens* tokens);
int cmd_cd(struct tokens* tokens);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens* tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t* fun;
  char* cmd;
  char* doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
    {cmd_help, "?", "show this help menu"},
    {cmd_exit, "exit", "exit the command shell"},
    {cmd_pwd, "pwd", "print the current working directory"},
    {cmd_cd, "cd", "changes the current working directory"},
};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens* tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens* tokens) { exit(0); }

/* Prints the current working directory to standard output */
int cmd_pwd(unused struct tokens* tokens) {
  char *pwd = getcwd(NULL, 0);
  puts(pwd);
  free(pwd);
  return 0;
}

/* Changes the current working directory */
int cmd_cd(unused struct tokens* tokens) {
  if (tokens_get_length(tokens) == 1) return 0;
  if (tokens_get_length(tokens) > 2) return 1;
  const int MAX_SIZE = 4096;
  char *pwd = (char *)malloc(MAX_SIZE);
  getcwd(pwd, MAX_SIZE);
  strcat(pwd, "/");
  strcat(pwd, tokens_get_token(tokens, 1));
  if (chdir(pwd)) {
    fprintf(stderr, "cd: no such file or directory: %s\n",
      tokens_get_token(tokens, 1));
    return 1;
  }
  return 0;
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

int exec_subprogram_pipe(struct tokens* tokens, int token_start, int token_end) {
  /* Create arguments and redirect */
  int argcnt = 0, tokens_length = tokens_get_length(tokens);
  char *arg[4096];
  for (int i = token_start; i < token_end; i++) {
    char *token = tokens_get_token(tokens, i);
    if (!strcmp(token, "<") && i + 1 < tokens_length) {
      int fd = open(tokens_get_token(tokens, i+1), O_RDONLY);
      if (fd != -1) dup2(fd, STDIN_FILENO);
      else { printf("shell: can't open file: %s\n", tokens_get_token(tokens, i+1)); }
      i++;
    }
    else if (!strcmp(token, ">") && i + 1 < tokens_length) {
      int fd = open(tokens_get_token(tokens, i+1), O_CREAT|O_WRONLY|O_TRUNC, 0600);
      if (fd != -1) dup2(fd, STDOUT_FILENO);
      i++;
    }
    else {
      arg[argcnt++] = token;
    }
  }
  arg[tokens_length] = NULL;

  /* Retrieve the program and execute */
  char exe_path[4096], *word, *brkp;
  char *sep = ":";
  char *PATH = getenv("PATH");
  if (PATH == NULL) { return 1; }

  if (!execv(arg[0], arg)) return 0;
  for (word = strtok_r(PATH, sep, &brkp);
       word;
       word = strtok_r(NULL, sep, &brkp)) {
    strcpy(exe_path, word);
    strcat(exe_path, "/");
    strcat(exe_path, arg[0]);
    if (!execv(exe_path, arg)) return 0;
  }
  printf("shell: command not found: %s\n", arg[0]);
  return 1;
}

void exec_subprogram(struct tokens* tokens) {
  /* Calculate the total number of process */
  int total_proc = 1, tokens_length = tokens_get_length(tokens);
  for (int i = 0; i < tokens_length; i++) {
    if (!strcmp(tokens_get_token(tokens, i), "|")) {
      ++total_proc;
    }
  }

  /* Create pipes */
  int pipe_arr[total_proc][2];
  for (int i = 0; i < total_proc - 1; i++) {
    if (pipe(pipe_arr[i]) == -1) { exit(1); }
  }

  /* Disable some signals */
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGTTOU, &sa, NULL) == -1) { perror("sigaction error"); exit(EXIT_FAILURE); }
  if (sigaction(SIGTTIN, &sa, NULL) == -1) { perror("sigaction error"); exit(EXIT_FAILURE); }
  if (sigaction(SIGCONT, &sa, NULL) == -1) { perror("sigaction error"); exit(EXIT_FAILURE); }
  if (sigaction(SIGTSTP, &sa, NULL) == -1) { perror("sigaction error"); exit(EXIT_FAILURE); }
  if (sigaction(SIGTERM, &sa, NULL) == -1) { perror("sigaction error"); exit(EXIT_FAILURE); }
  if (sigaction(SIGINT,  &sa, NULL) == -1) { perror("sigaction error"); exit(EXIT_FAILURE); }
  if (sigaction(SIGQUIT, &sa, NULL) == -1) { perror("sigaction error"); exit(EXIT_FAILURE); }

  /* Run subtasks */
  int pid[total_proc];
  int token_start = 0, proc_cnt = 0;
  for (int i = 0; i <= tokens_length; i++) {
    if (i == tokens_length || !strcmp(tokens_get_token(tokens, i), "|")) {
      pid[proc_cnt] = fork();
      if (pid[proc_cnt] == 0) {
        /* Child Process */
        /* Set pipe */
        if (proc_cnt != 0) dup2(pipe_arr[proc_cnt - 1][0], STDIN_FILENO);
        if (proc_cnt != total_proc - 1)
          dup2(pipe_arr[proc_cnt][1], STDOUT_FILENO);
        for (int i = 0; i < total_proc - 1; i++) {
          close(pipe_arr[i][0]);
          close(pipe_arr[i][1]);
        }

        /* Enable some signals */
        sa.sa_handler = SIG_DFL;
        if (sigaction(SIGTTOU, &sa, NULL) == -1) { perror("sigaction error"); exit(EXIT_FAILURE); }
        if (sigaction(SIGTTIN, &sa, NULL) == -1) { perror("sigaction error"); exit(EXIT_FAILURE); }
        if (sigaction(SIGCONT, &sa, NULL) == -1) { perror("sigaction error"); exit(EXIT_FAILURE); }
        if (sigaction(SIGTSTP, &sa, NULL) == -1) { perror("sigaction error"); exit(EXIT_FAILURE); }
        if (sigaction(SIGTERM, &sa, NULL) == -1) { perror("sigaction error"); exit(EXIT_FAILURE); }
        if (sigaction(SIGINT,  &sa, NULL) == -1) { perror("sigaction error"); exit(EXIT_FAILURE); }
        if (sigaction(SIGQUIT, &sa, NULL) == -1) { perror("sigaction error"); exit(EXIT_FAILURE); }

        /* Set process group*/
        setpgid(0, 0);

        /* Run subprogram */
        int st = exec_subprogram_pipe(tokens, token_start, i);

        /* Close pipe and Exit*/
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        exit(st);
      } else {
        /* Parent Process */
        tcsetpgrp(STDIN_FILENO, pid[proc_cnt]);
        token_start = i + 1;
        ++proc_cnt;
      }
    }
  }

  /* Close pipes */
  for (int i = 0; i < total_proc - 1; i++) {
    close(pipe_arr[i][0]);
    close(pipe_arr[i][1]);
  }

  /* Wait subtasks */
  for (int i = 0; i < total_proc; i++) {
    wait(&pid[i]);
  }
  tcsetpgrp(STDIN_FILENO, getpgrp());
}


int main(unused int argc, unused char* argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens* tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      exec_subprogram(tokens);
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
