#ifndef OMPTARGET_OMPT_H
#define OMPTARGET_OMPT_H

#include <ompt.h>

#define FOREACH_OMPT_TARGET_FN(macro)		 \
  macro (libomp_callback_device_initialize)	 \
  macro (libomp_callback_target)		 \
  macro (libomp_set_frame_reenter)      


typedef ompt_callback_device_initialize_t libomp_callback_device_initialize_t;

typedef ompt_callback_target_t libomp_callback_target_t;

typedef void (*libomp_set_frame_reenter_t)
(
 void *addr
);



#endif /* OMPTARGET_OMPT_H */