/* host-test stub — not the real settings component */
#pragma once
#include "nvs_flash.h"

/* journal only needs the security-config accessor; tests return a
 * dummy non-NULL pointer so jrn_init proceeds */
const void *settings_sec_cfg(void);
