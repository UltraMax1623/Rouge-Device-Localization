// Upload to the root ESP32 board. Select its actual board model in the IDE.
// S3 RGB only: uncomment with the actual data GPIO if the board variant differs.
// #define MODE_RGB_PIN 38  // DevKitC-1 v1.1; initial revision uses 48.
#define ROOT_FIRMWARE 1
#define BOARD_NUMBER 0x01
static_assert(BOARD_NUMBER == 0x01, "Root station identity must always be 01");
#define TEST_BOARD_COUNT 2  // Root 01 plus normal nodes 02 and 03.
static_assert(TEST_BOARD_COUNT >= 2 && TEST_BOARD_COUNT <= 15,
              "Test board count must be 2..15");
#include "../shared/node_firmware.h"
