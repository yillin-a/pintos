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

/* 忽略编译器关于未使用参数的警告 */
#define UNUSED_PARAM __attribute__((unused))

/* shell是否连接到真实终端 */
bool is_interactive_mode;

/* shell输入的文件描述符 */
int terminal_fd;

/* shell的终端模式设置 */
struct termios original_terminal_settings;

/* shell的进程组ID */
pid_t shell_process_group;

int execute_exit(struct tokens *tokens);
int execute_help(struct tokens *tokens);
int execute_pwd(struct tokens *tokens);
int execute_cd(struct tokens *tokens);
int execute_external_program(struct tokens *tokens);

/* 内置命令函数接受token数组并返回int */
typedef int command_func_t(struct tokens *tokens);

/* 内置命令结构体和查找表 */
typedef struct command_descriptor
{
  command_func_t *function_ptr;
  char *command_name;
  char *description;
} command_descriptor_t;

command_descriptor_t builtin_commands[] = {
    {execute_help, "?", "show this help menu"},
    {execute_exit, "exit", "exit the command shell"},
    {execute_pwd, "pwd", "prints the current working directory"},
    {execute_cd, "cd", "change derectory to the specific one"},
    {execute_external_program, "run_prog", "execute programs entered"}
};

/* 打印给定命令的帮助描述 */
int execute_help(UNUSED_PARAM struct tokens *tokens)
{
  unsigned int idx;
  for (idx = 0; idx < sizeof(builtin_commands) / sizeof(command_descriptor_t); idx++)
    printf("%s - %s\n", builtin_commands[idx].command_name, builtin_commands[idx].description);
  return 1;
}

/* 退出当前shell */
int execute_exit(UNUSED_PARAM struct tokens *tokens) { exit(0); }

/* 打印当前工作目录 */
int execute_pwd(UNUSED_PARAM struct tokens *tokens)
{
  char working_dir[4096];
  if (getcwd(working_dir, sizeof(working_dir)) != NULL) {
    printf("%s\n", working_dir);
  } else {
    perror("Error! Cannot acquire current directory.\nPossibly since...");
  }
  return 0;
}

/* 切换当前工作目录到指定目录 */
int execute_cd(struct tokens *tokens)
{
  // 验证输入参数数量是否正确
  if (tokens_get_length(tokens) != 2){
    printf("cd: Error! Expected 1 params!\n");
    return 1;
  }

  // 执行目录切换并验证是否成功
  char *target_path = tokens_get_token(tokens, 1);
  if (chdir(target_path) != 0){
    perror("Change error!");
    return 1;
  }
  return 0;
}

// 根据输入字符串返回完整路径，确保路径存在，返回NULL表示路径无效
char *resolve_program_path(const char *program_name)
{
  // 输入路径为空则直接返回NULL
  if (program_name == NULL) {
    return NULL;
  }

  // 如果输入以斜杠开头，这是绝对路径
  if (program_name[0] == '/') {
    // 验证路径是否存在
    if (access(program_name, F_OK) != -1) {
      // 返回路径副本
      return strdup(program_name);
    } else {
      return NULL;
    }
  }

  // 获取PATH环境变量
  char *path_env = getenv("PATH");
  // PATH环境变量为空则返回NULL
  if (path_env == NULL) {
    return NULL;
  }

  // 复制PATH环境变量用于后续处理
  char *path_copy = strdup(path_env);
  if (path_copy == NULL) {
    perror("Memory allocation failed");
    exit(EXIT_FAILURE);
  }

  // 用冒号作为分隔符分割PATH环境变量
  char *directory = strtok(path_copy, ":");
  while (directory != NULL) {
    // 计算完整路径长度
    size_t full_length = strlen(directory) + strlen(program_name) + 2;
    // 分配内存保存完整路径
    char *candidate_path = (char *)malloc(full_length);
    if (candidate_path == NULL) {
      perror("Memory allocation failed");
      free(path_copy);
      exit(EXIT_FAILURE);
    }

    // 拼接完整路径
    snprintf(candidate_path, full_length, "%s/%s", directory, program_name);

    // 检查完整路径是否存在
    if (access(candidate_path, F_OK) != -1) {
      // 释放环境变量副本
      free(path_copy);
      // 返回完整路径副本
      char *final_result = strdup(candidate_path);
      if (final_result == NULL) {
        perror("Memory allocation failed");
        free(candidate_path);
        exit(EXIT_FAILURE);
      }
      // 释放完整路径
      free(candidate_path);
      return final_result;
    }

    // 释放当前完整路径
    free(candidate_path);
    // 获取下一个路径
    directory = strtok(NULL, ":");
  }

  // 未找到有效路径，释放环境变量副本并返回NULL
  free(path_copy);
  return NULL;
}

int redirect_output_to_file(char *output_file)
{
  // 文件路径为空则打印错误并返回1
  if (output_file == NULL) {
    perror("No output file specified!");
    return 1;
  }

  // 以写模式打开文件，文件不存在则自动创建
  FILE *fp = fopen(output_file, "w");
  if (fp == NULL) {
    perror("Failed to open file: please check the path.");
    return 1;
  }

  // 重定向标准输出到指定文件
  if (freopen(output_file, "w", stdout) == NULL) {
    perror("Error redirecting output!");
    fclose(fp);
    return 1;
  }

  // 关闭原始文件指针
  fclose(fp);

  return 0;
}

int redirect_input_from_file(char *input_file)
{
  // 文件路径为空则打印错误并返回1
  if (input_file == NULL) {
    perror("No input file specified!");
    return 1;
  }

  // 以只读模式打开文件
  FILE *fp = fopen(input_file, "r");
  if (fp == NULL) {
    perror("Failed to open file: please check the path.");
    return 1;
  }

  // 重定向标准输入到指定文件
  if (freopen(input_file, "r", stdin) == NULL) {
    perror("Error redirecting input!");
    fclose(fp);
    return 1;
  }

  // 关闭原始文件指针
  fclose(fp);

  return 0;
}

int restore_standard_io()
{
  freopen("/dev/tty", "w", stdout);
  freopen("/dev/tty", "r", stdin);
  return 0;
}

/* 使shell支持运行外部程序 */
int execute_external_program(struct tokens *tokens)
{
  int num_tokens = tokens->tokens_length;
  // 检查是否输入了可执行文件路径
  if (num_tokens < 1){
    printf("run: Please enter an executable \n");
    return 1;
  }

  // 获取程序的绝对路径
  char *executable_path = resolve_program_path(tokens->tokens[0]);
  if (executable_path == NULL){
    printf("File not exists!\n");
    return 1;
  }
  
  // 判断是否需要重定向
  int has_redirection = 0;
  if (num_tokens >= 3){
    // 需要重定向输出到文件
    if (strcmp(tokens->tokens[num_tokens - 2], ">") == 0){
      has_redirection = 1;
      if (redirect_output_to_file(tokens->tokens[num_tokens - 1]) != 0)
        return 1;
    }
    // 需要重定向输入到文件
    else if (strcmp(tokens->tokens[num_tokens - 2], "<") == 0){
      has_redirection = 1;
      if (redirect_input_from_file(tokens->tokens[num_tokens - 1]) != 0)
        return 1;
    }
  }

  // 创建子进程
  pid_t child_pid = fork();
  if (child_pid == -1) {
    perror("Failed to create child process. Refer to...");
    return 1;
  } else if (child_pid == 0) { // 子进程执行的代码
    // 设置子进程的进程组，使其成为新进程组的领导者
    setpgid(0, 0);
    // 设置信号处理方式
    signal(SIGINT, SIG_DFL);  // 默认行为：终止进程
    signal(SIGTSTP, SIG_DFL); // 默认行为：暂停进程
    signal(SIGTTOU, SIG_DFL); // 默认行为：暂停进程

    // 创建新的字符串数组用于传递给execv
    int arg_count = tokens->tokens_length;
    if (has_redirection == 1) { // 使用输入输出重定向时不传递最后两个参数
      arg_count = arg_count - 2;
    }
    char **argument_vector = malloc((arg_count + 1) * sizeof(char *));
    if (argument_vector == NULL) {
      perror("Memory allocation error!");
      return 1;
    }

    // 复制tokens中的字符串
    for (size_t i = 0; i < arg_count; ++i) {
      argument_vector[i] = tokens->tokens[i];
    }
    argument_vector[arg_count] = NULL; // 数组结束标志

    // 在子进程中执行程序
    execv(executable_path, argument_vector);

    // 以下代码仅在execv失败时执行，用于退出子进程
    perror("Failed to execute child process. Refer to...");
    exit(EXIT_FAILURE);
  } else { // fork返回正数，表示父进程，child_pid为子进程ID
    // 忽略SIGINT信号，让子进程处理该信号
    signal(SIGINT, SIG_IGN);

    // 将子进程置于前台
    tcsetpgrp(terminal_fd, child_pid);

    // 等待子进程完成
    int exit_status;
    waitpid(child_pid, &exit_status, 0);

    // 恢复前台进程组为Shell的进程组
    tcsetpgrp(terminal_fd, shell_process_group);

    // 恢复SIGINT的默认行为
    signal(SIGINT, SIG_DFL);

    // 取消输入输出重定向
    restore_standard_io();
  }
  // tcsetpgrp(terminal_fd, getpid());
  restore_standard_io();
  return 0;
}

/* 查找内置命令（如果存在） */
int find_builtin_command(char cmd[])
{
  unsigned int i;
  for (i = 0; i < sizeof(builtin_commands) / sizeof(command_descriptor_t); i++)
    if (cmd && (strcmp(builtin_commands[i].command_name, cmd) == 0))
      return i;
  return -1;
}

/* shell的初始化过程 */
void initialize_shell()
{
  /* shell连接到标准输入 */
  terminal_fd = STDIN_FILENO;

  signal(SIGINT, SIG_DFL);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);

  /* 检查是否以交互模式运行 */
  is_interactive_mode = isatty(terminal_fd);

  if (is_interactive_mode)
  {
    /* 如果shell当前不在前台，必须暂停shell直到它成为前台进程。
     * 使用SIGTTIN暂停shell。当shell移到前台时，会收到SIGCONT信号。 */
    while (tcgetpgrp(terminal_fd) != (shell_process_group = getpgrp()))
      kill(-shell_process_group, SIGTTIN);

    /* 保存shell的进程ID */
    shell_process_group = getpid();

    /* 控制终端 */
    tcsetpgrp(terminal_fd, shell_process_group);

    /* 保存当前termios到变量，以便稍后恢复 */
    tcgetattr(terminal_fd, &original_terminal_settings);
  }
}

int main(UNUSED_PARAM int argc, UNUSED_PARAM char *argv[])
{
  initialize_shell();

  static char input_buffer[4096];
  int prompt_number = 0;

  /* 仅当标准输入不是tty时打印shell提示符 */
  if (is_interactive_mode)
    fprintf(stdout, "%d: ", prompt_number);

  while (fgets(input_buffer, 4096, stdin)){
    /* 将输入行分割成单词 */
    struct tokens *parsed_tokens = tokenize(input_buffer);

    /* 查找要运行的内置函数 */
    int builtin_index = find_builtin_command(tokens_get_token(parsed_tokens, 0));

    if (builtin_index >= 0){
      builtin_commands[builtin_index].function_ptr(parsed_tokens);
    }
    else{
      /* 替换此部分以程序方式运行命令 */
      // fprintf(stdout, "This shell doesn't know how to run programs.\n");
      execute_external_program(parsed_tokens);
    }
    if (is_interactive_mode){
      /* 仅当标准输入不是tty时打印shell提示符 */
      fprintf(stdout, "%d: ", ++prompt_number);
    }
    /* 清理内存 */
    tokens_destroy(parsed_tokens);
  }

  return 0;
}