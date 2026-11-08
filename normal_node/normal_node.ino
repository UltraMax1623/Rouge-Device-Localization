// Upload to each normal board; change BOARD_NUMBER before each upload.

// S3 RGB only: uncomment with the actual data GPIO if the board variant differs.
// #define MODE_RGB_PIN 38  // DevKitC-1 v1.1; initial revision uses 48.
#define ROOT_FIRMWARE 0
#define FIRMWARE_DEBUG 1  // Serial connection and command logs at 115200 baud.
#define BOARD_NUMBER 0x04  // Use 0x02, 0x03, 0x04 for the three normal boards.
#define NORMAL_CHANNEL_DEBUG 1  // Print channel migration diagnostics at 115200 baud.

static_assert(BOARD_NUMBER >= 2 && BOARD_NUMBER <= 15,
              "Normal board number must be 0x02..0x0F and unique");
#include "../shared/node_firmware.h"
