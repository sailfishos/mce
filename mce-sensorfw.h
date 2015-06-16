#ifndef MCE_SENSORFW_H_
# define MCE_SENSORFW_H_

# include <stdbool.h>

# ifdef __cplusplus
extern "C" {
# elif 0
} /* fool JED indentation ... */
# endif

bool mce_sensorfw_init(void);
void mce_sensorfw_quit(void);

void mce_sensorfw_suspend(void);
void mce_sensorfw_resume(void);

void mce_sensorfw_als_attach(int fd);
void mce_sensorfw_als_set_notify(void (*cb)(int lux));
void mce_sensorfw_als_enable(void);
void mce_sensorfw_als_disable(void);

void mce_sensorfw_ps_attach(int fd);
void mce_sensorfw_ps_set_notify(void (*cb)(bool covered));
void mce_sensorfw_ps_enable(void);
void mce_sensorfw_ps_disable(void);

void mce_sensorfw_orient_set_notify(void (*cb)(int state));
void mce_sensorfw_orient_enable(void);
void mce_sensorfw_orient_disable(void);

# ifdef __cplusplus
};
# endif

#endif /* MCE_SENSORFW_H_ */
