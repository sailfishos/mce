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

void mce_sensorfw_als_set_notify(void (*cb)(unsigned lux));
void mce_sensorfw_als_enable(void);
void mce_sensorfw_als_disable(void);

void mce_sensorfw_ps_set_notify(void (*cb)(bool covered));
void mce_sensorfw_ps_enable(void);
void mce_sensorfw_ps_disable(void);

/** These must match with what sensorfw uses */
typedef enum
{
	MCE_ORIENTATION_UNDEFINED   = 0,  /**< Orientation is unknown. */
	MCE_ORIENTATION_LEFT_UP     = 1,  /**< Device left side is up */
	MCE_ORIENTATION_RIGHT_UP    = 2,  /**< Device right side is up */
	MCE_ORIENTATION_BOTTOM_UP   = 3,  /**< Device bottom is up */
	MCE_ORIENTATION_BOTTOM_DOWN = 4,  /**< Device bottom is down */
	MCE_ORIENTATION_FACE_DOWN   = 5,  /**< Device face is down */
	MCE_ORIENTATION_FACE_UP     = 6,  /**< Device face is up */
} orientation_state_t;

void mce_sensorfw_orient_set_notify(void (*cb)(int state));
void mce_sensorfw_orient_enable(void);
void mce_sensorfw_orient_disable(void);

# ifdef __cplusplus
};
#endif

#endif /* MCE_SENSORFW_H_ */
