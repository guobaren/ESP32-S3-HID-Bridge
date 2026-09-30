#include "dual_proxy_runtime_config.h"

static volatile bool s_periodic_stats_enabled;

bool dual_proxy_periodic_stats_enabled(void)
{
    return __atomic_load_n(&s_periodic_stats_enabled, __ATOMIC_ACQUIRE);
}

void dual_proxy_set_periodic_stats_enabled(bool enabled)
{
    __atomic_store_n(&s_periodic_stats_enabled, enabled, __ATOMIC_RELEASE);
}
