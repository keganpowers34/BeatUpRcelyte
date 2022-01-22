#include <stdint.h>
#include <mbedtls/ctr_drbg.h>
#include "../net.h"

_Bool instance_init(const char *domain, const char *domainIPv4);
void instance_cleanup();

_Bool instance_get_isopen(ServerCode code, struct String *managerId_out, struct GameplayServerConfiguration *configuration);
struct NetSession *instance_open(ServerCode *out, struct String managerId, struct GameplayServerConfiguration *configuration, struct SS addr, struct String secret, struct String userId, struct String userName);
struct NetContext *instance_get_net(ServerCode code);
struct NetSession *instance_resolve_session(ServerCode code, struct SS addr, struct String secret, struct String userId, struct String userName);
struct IPEndPoint instance_get_address(ServerCode code, _Bool ipv4);
