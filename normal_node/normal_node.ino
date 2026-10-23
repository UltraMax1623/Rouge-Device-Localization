// Upload to each normal board; change BOARD_NUMBER before each upload.
#define ROOT_FIRMWARE 0
#define BOARD_NUMBER 0x02  // Use 0x02, 0x03, 0x04 for the three normal boards.
static_assert(BOARD_NUMBER >= 2 && BOARD_NUMBER <= 15,
              "Normal board number must be 0x02..0x0F and unique");
#include "../shared/node_firmware.h"
