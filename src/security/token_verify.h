#ifndef SECURITY_TOKEN_VERIFY_H
#define SECURITY_TOKEN_VERIFY_H

#ifdef __cplusplus
extern "C" {
#endif

int token_generate(void);
int token_verify(const char *input);
int token_refresh(void);
int token_is_locked(void);

#ifdef __cplusplus
}
#endif

#endif