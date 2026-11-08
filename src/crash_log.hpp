#pragma once

// Logs access violations and similar faults (address, module + offset, and the call stack with module offsets) to the
// plugin log, then lets the exception continue as if we weren't there. Diagnostics only: it doesn't handle anything.
namespace crash_log {
void install();
} // namespace crash_log
