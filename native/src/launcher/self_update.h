#pragma once

/* Extract first-party components embedded in the launcher executable.
   Third-party runtime payloads are intentionally not embedded here. */
void sync_embedded_payloads(void);
