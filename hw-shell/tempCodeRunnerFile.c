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