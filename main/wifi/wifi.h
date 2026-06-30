// wifi_module.h
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void wifi_init_sta(void);
void wifi_reconnect(void);
void print_ip(void);

#ifdef __cplusplus
}
#endif
