/**
 * @file    voice_cmds.h
 * @brief   阶段一命令执行头文件
 * @version LN-B-4.3.0.0
 */

#ifndef VOICE_VOICE_CMDS_H
#define VOICE_VOICE_CMDS_H

int voice_cmd_execute(const char *cmd);
int voice_cmd_parse(const char *text);

#endif /* VOICE_VOICE_CMDS_H */