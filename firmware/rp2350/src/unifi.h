#ifndef _UNIFI_H_
#define _UNIFI_H_

/*
 * The switch as a UniFi switch: it informs a UniFi Network controller over
 * HTTP, can be adopted there, reports its ports and takes VLANs, port VLAN IDs,
 * names and fixed speeds from the controller's configuration. It claims to be a
 * USW Flex 2.5G (5 or 8, by its ports) and refuses firmware upgrades.
 *
 * What the controller sets goes on top of the switch's own configuration, which
 * it never changes: it is kept in a flash sector of its own once the
 * controller has been reached after it, and applied again after each start
 * of the bridge. If the controller can't be reached for WATCH_S after a
 * change, the switch restarts with the configuration of before and refuses
 * that version from then on.
 *
 *   unifi                       status
 *   unifi on|off
 *   unifi inform <url>          http://<a.b.c.d>[:port]/inform, kept in flash
 *   unifi forget                back to the default key, to be adopted again
 */

#include <stdbool.h>
#include <stddef.h>

void unifi_boot(void);		/* after store_load() */
void unifi_bridge_up(void);	/* after rtl_bridge_up(), which replays the configuration */
void unifi_second(void);
bool unifi_url_ok(const char *url);
/* the commands' side is in plat.h */

#endif
