// Upload to the root ESP32 board. Select its actual board model in the IDE.
#define ROOT_FIRMWARE 1
#define BOARD_NUMBER 0x01
static_assert(BOARD_NUMBER == 0x01, "Root station identity must always be 01");
#define TEST_BOARD_COUNT 3  // Root 01 plus normal nodes 02 and 03.
static_assert(TEST_BOARD_COUNT >= 2 && TEST_BOARD_COUNT <= 15,
              "Test board count must be 2..15");
#include "../shared/node_firmware.h"
