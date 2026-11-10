#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../include/http_security.h"

int main() {
  // 1. Safe path segment validation blocks path traversal
  assert(camsec::isValidSafeSegment("run_0001"));
  assert(camsec::isValidSafeSegment("frame_000123.jpg"));
  assert(camsec::isValidSafeSegment("readings.csv"));
  assert(!camsec::isValidSafeSegment(""));
  assert(!camsec::isValidSafeSegment("."));
  assert(!camsec::isValidSafeSegment(".."));
  assert(!camsec::isValidSafeSegment("../secret"));
  assert(!camsec::isValidSafeSegment("run_0001/../../etc"));
  assert(!camsec::isValidSafeSegment("run\\file.jpg"));

  // 2. Constant-time token comparison
  assert(camsec::constantTimeEquals("s3cr3t-token", "s3cr3t-token"));
  assert(!camsec::constantTimeEquals("s3cr3t-token", "s3cr3t-tokeN"));
  assert(!camsec::constantTimeEquals("short", "longer-token"));
  assert(!camsec::constantTimeEquals("", "nonempty"));

  // 3. HTML escaping prevents XSS in /config SSID fields
  char escaped[128];
  camsec::htmlEscapeToBuf("My\"AP'<script>", escaped, sizeof(escaped));
  assert(strcmp(escaped, "My&quot;AP&#39;&lt;script&gt;") == 0);

  // 4. Content-Type inference & pagination clamping
  assert(strcmp(camsec::contentTypeForFilename("frame_000001.jpg"), "image/jpeg") == 0);
  assert(strcmp(camsec::contentTypeForFilename("readings.csv"), "text/csv; charset=utf-8") == 0);
  assert(camsec::clampPage(-5) == 1);
  assert(camsec::clampPageSize(9999) == 100);

  printf("All C++ http_security unit tests passed.\n");
  return 0;
}
