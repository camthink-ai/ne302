#ifndef __EG912U_GL_NETIF_H__
#define __EG912U_GL_NETIF_H__

#ifdef __cplusplus
    extern "C" {
#endif

#include "netif_manager.h"

int eg912u_netif_init(void);
int eg912u_netif_up(void);
int eg912u_netif_down(void);
void eg912u_netif_deinit(void);
int eg912u_netif_config(netif_config_t *netif_cfg);
int eg912u_netif_info(netif_info_t *netif_info);
netif_state_t eg912u_netif_state(void);
struct netif *eg912u_netif_ptr(void);

int eg912u_netif_ctrl(const char *if_name, netif_cmd_t cmd, void *param);

int eg912u_netif_enable_fast_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* __EG912U_GL_NETIF_H__ */
