#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "tokenizer.h"

struct tokens;

static void *expand_string_array(char ***array_ptr, size_t *current_size, void *new_element)
{
  *array_ptr = (char **)realloc(*array_ptr, sizeof(char *) * (*current_size + 1));
  (*array_ptr)[*current_size] = new_element;
  *current_size += 1;
  return new_element;
}

static void *duplicate_word(char *src, size_t length)
{
  src[length] = '\0';
  char *duplicated = (char *)malloc(length + 1);
  strncpy(duplicated, src, length + 1);
  return duplicated;
}

struct tokens *tokenize(const char *line)
{
  // 输入字符串为空则直接返回NULL
  if (line == NULL) return NULL;

  // 定义静态字符数组存储当前解析的词
  static char word_buffer[4096];
  size_t current_length = 0, max_length = 4096; // current_length记录当前词长度，max_length是缓冲区最大长度

  // 为解析后的tokens分配内存
  struct tokens *result_tokens = (struct tokens *)malloc(sizeof(struct tokens));
  if (result_tokens == NULL) {
    perror("Memory allocation failed");
    return NULL;
  }

  // 初始化tokens结构体
  result_tokens->tokens_length = 0;
  result_tokens->tokens = NULL;
  result_tokens->buffers_length = 0;
  result_tokens->buffers = NULL;

  // 定义解析状态常量
  const int STATE_REGULAR = 0, STATE_SINGLE_QUOTE = 1, STATE_DOUBLE_QUOTE = 2;
  int current_state = STATE_REGULAR; // 初始状态为常规状态

  // 获取输入字符串长度
  size_t input_length = strlen(line);

  // 遍历输入字符串的每个字符
  for (unsigned int idx = 0; idx < input_length; idx++) {
    char current_char = line[idx];

    if (current_state == STATE_REGULAR) {
      // 遇到单引号，切换到单引号状态
      if (current_char == '\'') {
        current_state = STATE_SINGLE_QUOTE;
      }
      // 遇到双引号，切换到双引号状态
      else if (current_char == '"') {
        current_state = STATE_DOUBLE_QUOTE;
      }
      // 处理转义符
      else if (current_char == '\\') {
        if (idx + 1 < input_length) {
          word_buffer[current_length++] = line[++idx]; // 跳过转义符，保存下一个字符
        }
      }
      // 遇到空白字符，表示词结束
      else if (isspace(current_char)) {
        if (current_length > 0) {
          void *completed_word = duplicate_word(word_buffer, current_length); // 复制当前词
          expand_string_array(&result_tokens->tokens, &result_tokens->tokens_length, completed_word); // 添加词到tokens数组
          current_length = 0; // 重置当前词长度
        }
      }
      // 普通字符，添加到当前词
      else {
        word_buffer[current_length++] = current_char;
      }
    }
    else if (current_state == STATE_SINGLE_QUOTE) {
      // 遇到单引号，退出单引号状态
      if (current_char == '\'') {
        current_state = STATE_REGULAR;
      }
      // 处理转义符
      else if (current_char == '\\') {
        if (idx + 1 < input_length) {
          word_buffer[current_length++] = line[++idx]; // 跳过转义符，保存下一个字符
        }
      }
      // 普通字符，添加到当前词
      else {
        word_buffer[current_length++] = current_char;
      }
    }
    else if (current_state == STATE_DOUBLE_QUOTE) {
      // 遇到双引号，退出双引号状态
      if (current_char == '"') {
        current_state = STATE_REGULAR;
      }
      // 处理转义符
      else if (current_char == '\\') {
        if (idx + 1 < input_length) {
          word_buffer[current_length++] = line[++idx]; // 跳过转义符，保存下一个字符
        }
      }
      // 普通字符，添加到当前词
      else {
        word_buffer[current_length++] = current_char;
      }
    }

    // 检查缓冲区是否已满，如果满则终止程序
    if (current_length + 1 >= max_length) {
      abort();
    }
  }

  // 处理最后一个词
  if (current_length > 0) {
    void *final_word = duplicate_word(word_buffer, current_length); // 复制当前词
    expand_string_array(&result_tokens->tokens, &result_tokens->tokens_length, final_word); // 添加词到tokens数组
    current_length = 0; // 重置当前词长度
  }

  return result_tokens; // 返回解析后的tokens结构体
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
  int idx;
  for (idx = 0; idx < tokens->tokens_length; idx++)
  {
    // printf("Freeing tokens->tokens[%d]:%s\n", idx, tokens->tokens[idx]);
    if (tokens->tokens[idx])
      free(tokens->tokens[idx]);
    // printf("Freed tokens->tokens[%d]\n", idx);
  }
  // printf("Freed all the tokens->tokens");
  // printf("Buffers_length:%ld\n", tokens->buffers_length);
  for (idx = 0; idx < tokens->buffers_length; idx++)
  {
    // printf("Freeing tokens->buffers[%d]\n", idx);
    if (tokens->buffers[idx])
      free(tokens->buffers[idx]);
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