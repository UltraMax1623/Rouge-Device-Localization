#pragma once

// Keep firmware diagnostics compiled out while command events carry UI results.
#define FW_LOG(...) do {} while (0)
