/* /features.json, for web/extras.html */

#include "events.h"
#include "extras.h"
#include "rtl_guard.h"
#include "safety.h"

size_t features_json(char *buf, size_t max)
{
	size_t n = app(buf, max, 0, "{\"reboot_left\":%lu,\"rollback_left\":%lu,\"guard\":",
		       (unsigned long)safety_reboot_left(), (unsigned long)safety_rollback_left());

	n += rtl_guard_json(buf + n, max - n);
	return app(buf, max, n, "}");
}
