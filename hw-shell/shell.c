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
#include <sys/stat.h>
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

int cmd_exit(struct tokens *tokens);
int cmd_help(struct tokens *tokens);
int cmd_pwd(struct tokens *tokens);
int cmd_cd(struct tokens *tokens);
int cmd_run_prog(struct tokens *tokens);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens *tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc
{
  cmd_fun_t *fun;
  char *cmd;
  char *doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
    {cmd_help, "?", "show this help menu"},
    {cmd_exit, "exit", "exit the command shell"},
    {cmd_pwd, "pwd", "prints the current working directory"},
    {cmd_cd, "cd", "change derectory to the specific one"},
    {cmd_run_prog, "run_prog", "execute programs entered"}
};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens *tokens)
{
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens *tokens) { exit(0); }

/* 打印当前的工作路径*/
int cmd_pwd(unused struct tokens *tokens)
{
  char cwd[4096];
  if (getcwd(cwd, sizeof(cwd)) != NULL)printf("%s\n", cwd);
  else perror("Error! Cannot acquire current directory.\nPossibly since...");
  return 0;
}

/* 转换当前工作路径至指定的工作路径*/
int cmd_cd(struct tokens *tokens)
{
  // 检查输入格式是否正确
  if (tokens_get_length(tokens) != 2){
    printf("cd: Error! Expected 1 params!\n");
    return 1;
  }

  // 跳转路径并检查是否转换成功
  char *path = tokens_get_token(tokens, 1);
  if (chdir(path) != 0){
    perror("Change error!");
    return 1;
  }
  return 0;
}

// 根据输入的字符串返回完整路径，确保返回的路径是存在的，返回NULL表示不能根据输入找到存在的路径
char *get_path_real(const char *input_path)
{
  if (input_path == NULL) return NULL;
  // 若input_path以"/"开头，表示系统绝对路径，检查路径是否真实存在
  if (input_path[0] == '/'){
    // 路径存在，返回
    if (access(input_path, F_OK) != -1)
      return strdup(input_path);
    return NULL;
  }
  // 若为程序名称，则在环境变量中寻找路径
  else
  {
    char *env_path = getenv("PATH");// 获取系统环境变量
    // 如果环境变量为空，则直接返回
    if (env_path == NULL) return NULL;
    char *env_path_copy = strdup(env_path);// 复制一份环境变量
    if (env_path_copy == NULL){
      perror("Memory allocated error!");
      exit(EXIT_FAILURE);
    }
    
    char *path = strtok(env_path_copy, ":");// 将以:分隔环境变量的进行分割
    // 按环境变量path进行匹配
    while (path != NULL){
      // 为完整路径分配空间，长度为path长度 + 输入路径长度 + 斜杠 + 末尾\0
      size_t path_length = strlen(path) + strlen(input_path) + 2;
      char *full_path = (char *)malloc(path_length);
      if (full_path == NULL){
        perror("Memory allocated error!");
        exit(EXIT_FAILURE);
      }
      snprintf(full_path, path_length, "%s/%s", path, input_path);  // 拼接得到完整路径
      // 返回找到的第一个路径
      if (access(full_path, F_OK) != -1){
        free(env_path_copy);
        char *result = strdup(full_path);
        if (result == NULL){
          perror("Memory allocated error!");
          exit(EXIT_FAILURE);
        }
        free(full_path);
        return result;
      }
      free(full_path);// 释放完整路径
      path = strtok(NULL, ":");// 取下一条环境变量
    }
    //  如果没有在环境变量中找到相应路径，则返回空
    free(env_path_copy);
    return NULL;
  }
}

int O_redirect(char *file_path)
{
  if (file_path == NULL){
    perror("No inputFile derectory!");
    return 1;
  }
  // 打开文件，若文件不存在会自动创建
  FILE *file = fopen(file_path, "w");
  if (file == NULL){
    perror("Fail to open file:please check again.");
    return 1;
  }
  if (freopen(file_path, "w", stdout) == NULL){
    perror("Redirect error!");
    fclose(file);
    return 1;
  }
  return 0;
}

int I_redirect(char *file_path)
{
  if (file_path == NULL)
  {
    perror("No inputFile derectory!");
    return 1;
  }
  // 打开文件，若文件不存在应该报错
  FILE *file = fopen(file_path, "r");
  if (file == NULL)
  {
    perror("Fail to open file:please check again.");
    return 1;
  }
  if (freopen(file_path, "r", stdin) == NULL)
  {
    perror("Redirect error!");
    fclose(file);
    return 1;
  }
  return 0;
}

int reset_stdio()
{
  freopen("/dev/tty", "w", stdout);
  freopen("/dev/tty", "r", stdin);
  return 0;
}

/*使shell支持运行外部程序*/
int cmd_run_prog(struct tokens *tokens)
{
  int tokens_length = tokens->tokens_length;
  // 检查是否输入了可执行文件路径
  if (tokens_length < 1){
    printf("run: Please enter an executable \n");
    return 1;
  }

  // 获取程序绝对路径，使
  char *program_path = get_path_real(tokens->tokens[0]);
  if (program_path == NULL){
    printf("File not exists!\n");
    return 1;
  }
  //  判断是否重定向
  int is_redirected = 0;
  if (tokens_length >= 3){
    // 需要重定向输出到文件
    if (strcmp(tokens->tokens[tokens_length - 2], ">") == 0){
      is_redirected = 1;
      if (O_redirect(tokens->tokens[tokens_length - 1]) != 0)
        return 1;
    }
    // 需要重定向输入到文件
    else if (strcmp(tokens->tokens[tokens_length - 2], "<") == 0){
      is_redirected = 1;
      if (I_redirect(tokens->tokens[tokens_length - 1]) != 0)
        return 1;
    }
  }
  // 创建子进程
  pid_t pid = fork();
  if (pid == -1){
    perror("Failed to create child process.Refer to...");
    return 1;
  }
  else if (pid == 0) {// 子进程会执行的代码
    // 设置子进程的进程组,两个参数都为0则将pgid设置成与其pid相同的值，确保子进程处于一个独立的进程组
    setpgid(0, 0);
    signal(SIGINT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    // 创建一个新的字符串数组，用于传递给 execv
    int copyied_argc = tokens->tokens_length;
    if (is_redirected == 1) // 若使用了输入输出重定向，则不传递最后两个参数
      copyied_argc = copyied_argc - 2;
    char **argv = malloc((copyied_argc + 1) * sizeof(char *));
    if (argv == NULL){
      perror("Memory allocated error!y\n");
      return 1;
    }
    // 复制 tokens 中的字符串
    for (size_t i = 0; i < copyied_argc; ++i){
      argv[i] = tokens->tokens[i];
    }
    argv[copyied_argc] = NULL;// 数组结束标志符
    // 在子进程中执行程序
    execv(program_path, argv);
    // 以下代码仅在execv执行失败时被执行，用于退出子进程
    perror("Execute child process error! Refer to...");
    exit(EXIT_FAILURE);
  }
  else {// fork返回的pid为正数，表示当前为父进程，pid为子进程的id
    signal(SIGINT, SIG_IGN);
    // 将子进程置于前台
    tcsetpgrp(shell_terminal, pid);
    int status;
    waitpid(pid, &status, 0);
    tcsetpgrp(shell_terminal, shell_pgid);
    signal(SIGINT, SIG_DFL);
    // 取消输入输出的重定向
    reset_stdio();
  }
  // tcsetpgrp(shell_terminal, getpid());
  reset_stdio();
  return 0;
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[])
{
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell()
{
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  signal(SIGINT, SIG_DFL);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive)
  {
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

int main(unused int argc, unused char *argv[])
{
  init_shell();

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)){
    /* Split our line into words. */
    struct tokens *tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0){
      cmd_table[fundex].fun(tokens);
    }
    else{
      /* REPLACE this to run commands as programs. */
      // fprintf(stdout, "This shell doesn't know how to run programs.\n");
      cmd_run_prog(tokens);
    }
    if (shell_is_interactive){
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);
    }
    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
