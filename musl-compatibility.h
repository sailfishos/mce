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

#endif /* MUSL_COMPATIBILITY_H_ */
