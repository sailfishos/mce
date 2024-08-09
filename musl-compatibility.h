#ifndef MUSL_COMPATIBILITY_H_
# define MUSL_COMPATIBILITY_H_

/* Used to retry syscalls that can return EINTR. Taken from bionic unistd.h */
# ifndef TEMP_FAILURE_RETRY
#  define TEMP_FAILURE_RETRY(exp) ({       \
    __typeof__(exp) _rc;                   \
    do {                                   \
        _rc = (exp);                       \
    } while (_rc == -1 && errno == EINTR); \
    _rc; })
# endif

/* GNU basename() like functionality as a macro
 *
 * Used for gnu vs posix basename() disambiguation.
 */
#define simple_basename(PATH) ({\
    const char *_sb_file = (PATH);\
    char *_sb_slash = _sb_file ? strrchr(_sb_file, '/') : NULL;\
    _sb_slash ? (_sb_slash + 1) : (char *)_sb_file;\
})

#endif /* MUSL_COMPATIBILITY_H_ */
