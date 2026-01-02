#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "tokenizer.h"

struct tokens;

static void *vector_push(char ***pointer, size_t *size, void *elem)
{
  *pointer = (char **)realloc(*pointer, sizeof(char *) * (*size + 1));
  (*pointer)[*size] = elem;
  *size += 1;
  return elem;
}

static void *copy_word(char *source, size_t n)
{
  source[n] = '\0';
  char *word = (char *)malloc(n + 1);
  strncpy(word, source, n + 1);
  return word;
}

struct tokens *tokenize(const char *line)
{
  // 如果输入的字符串为空，直接返回 NULL
  if (line == NULL) return NULL;

  // 定义一个静态字符数组用于存储当前解析的单词
  static char token[4096];
  size_t n = 0, n_max = 4096; // n 用于记录当前单词的长度，n_max 是 token 数组的最大长度

  // 分配内存用于存储解析后的 tokens
  struct tokens *tokens = (struct tokens *)malloc(sizeof(struct tokens));
  if (tokens == NULL) {
    perror("Memory allocation failed");
    return NULL;
  }

  // 初始化 tokens 结构体
  tokens->tokens_length = 0;
  tokens->tokens = NULL;
  tokens->buffers_length = 0;
  tokens->buffers = NULL;

  // 定义解析模式常量
  const int MODE_NORMAL = 0, MODE_SQUOTE = 1, MODE_DQUOTE = 2;
  int mode = MODE_NORMAL; // 初始模式为正常模式

  // 获取输入字符串的长度
  size_t line_length = strlen(line);

  // 遍历输入字符串的每个字符
  for (unsigned int i = 0; i < line_length; i++) {
    char c = line[i];

    if (mode == MODE_NORMAL) {
      // 处理单引号，进入单引号模式
      if (c == '\'') {
        mode = MODE_SQUOTE;
      }
      // 处理双引号，进入双引号模式
      else if (c == '"') {
        mode = MODE_DQUOTE;
      }
      // 处理转义字符
      else if (c == '\\') {
        if (i + 1 < line_length) {
          token[n++] = line[++i]; // 跳过转义字符，存储下一个字符
        }
      }
      // 处理空格，表示一个单词结束
      else if (isspace(c)) {
        if (n > 0) {
          void *word = copy_word(token, n); // 复制当前单词
          vector_push(&tokens->tokens, &tokens->tokens_length, word); // 将单词添加到 tokens 数组
          n = 0; // 重置当前单词长度
        }
      }
      // 处理普通字符，添加到当前单词
      else {
        token[n++] = c;
      }
    }
    else if (mode == MODE_SQUOTE) {
      // 处理单引号，退出单引号模式
      if (c == '\'') {
        mode = MODE_NORMAL;
      }
      // 处理转义字符
      else if (c == '\\') {
        if (i + 1 < line_length) {
          token[n++] = line[++i]; // 跳过转义字符，存储下一个字符
        }
      }
      // 处理普通字符，添加到当前单词
      else {
        token[n++] = c;
      }
    }
    else if (mode == MODE_DQUOTE) {
      // 处理双引号，退出双引号模式
      if (c == '"') {
        mode = MODE_NORMAL;
      }
      // 处理转义字符
      else if (c == '\\') {
        if (i + 1 < line_length) {
          token[n++] = line[++i]; // 跳过转义字符，存储下一个字符
        }
      }
      // 处理普通字符，添加到当前单词
      else {
        token[n++] = c;
      }
    }

    // 检查 token 数组是否已满，如果已满则中止程序
    if (n + 1 >= n_max) {
      abort();
    }
  }

  // 处理最后一个单词
  if (n > 0) {
    void *word = copy_word(token, n); // 复制当前单词
    vector_push(&tokens->tokens, &tokens->tokens_length, word); // 将单词添加到 tokens 数组
    n = 0; // 重置当前单词长度
  }

  return tokens; // 返回解析后的 tokens 结构体
}

size_t tokens_get_length(struct tokens *tokens)
{
  if (tokens == NULL)
  {
    return 0;
  }
  else
  {
    return tokens->tokens_length;
  }
}

char *tokens_get_token(struct tokens *tokens, size_t n)
{
  if (tokens == NULL || n >= tokens->tokens_length)
  {
    return NULL;
  }
  else
  {
    return tokens->tokens[n];
  }
}

void tokens_destroy(struct tokens *tokens)
{
  // printf("\nEnter tokens_destory.\n");
  if (tokens == NULL)
  {
    return;
  }
  // printf("Tokens is not NULL.\n");
  // printf("Tokens_length:%ld\n", tokens->tokens_length);
  for (int i = 0; i < tokens->tokens_length; i++)
  {
    // printf("Freeing tokens->tokens[%d]:%s\n", i, tokens->tokens[i]);
    if (tokens->tokens[i])
      free(tokens->tokens[i]);
    // printf("Freed tokens->tokens[%d]\n", i);
  }
  // printf("Freed all the tokens->tokens");
  // printf("Buffers_length:%ld\n", tokens->buffers_length);
  for (int i = 0; i < tokens->buffers_length; i++)
  {
    // printf("Freeing tokens->buffers[%d]\n", i);
    if (tokens->buffers[i])
      free(tokens->buffers[i]);
  }
  // printf("Freed all the tokens->buffers\n");
  if (tokens->tokens)
  {
    // printf("Freeing tokens->tokens.\n");
    free(tokens->tokens);
  }
  // printf("Freed tokens->tokens.\n");
  free(tokens);
  // printf("Done free.\n\n\n");
}
