#ifndef __USER_DEVICE_H__
#define __USER_DEVICE_H__
#include "user_config.h"

#define IOT_BIN_VERSION_LEN (15)


enum{
    MODE_NORMAL,
    MODE_RESET,
    MODE_APMODE,
};

struct esp_platform_saved_param {
    uint8 devkey[40];
    uint8 token[40];
    uint8 activeflag;
    uint8 reset_flg;
    uint8 pad[2];
    uint32 ota_start_time;
    uint32 ota_finish_time;
	uint32 ota_flag_sv;//for debug mode
};

struct esp_platform_sec_flag_param {
    uint8 flag; 
    uint8 pad[3];
};

enum {
    DEVICE_CONNECTING = 40,
    DEVICE_ACTIVE_DONE,
    DEVICE_ACTIVE_FAIL,
    DEVICE_CONNECT_SERVER_FAIL
};
struct dhcp_client_info {
    ip_addr_t ip_addr;
    ip_addr_t netmask;
    ip_addr_t gw;
    uint8 flag;
    uint8 pad[3];
};


#if 0
enum {
    CMD_TYPE_MULITCAST,
    CMD_TYPE_UNICAST,
    CMD_TYPE_BROADCAST,
};
#endif

char * user_json_find_section(const char *pbuf, u16 len, const char *section);
int user_JsonGetValueInt(const char *pbuffer, uint16 buf_len, const uint8 *json_key);

void user_esp_platform_init(void);
void user_esp_platform_init_peripheral(void);
void user_esp_platform_connect_ap_cb();
void user_esp_platform_sent_data();
void user_mdns_conf(void);
void user_esp_platform_reset_default();
void user_esp_platform_set_reset_flg(uint32 rst);
void user_platform_send_sms();
void user_esp_platform_mesh_conn_init();

#if STRING_SV_IN_FLASH
void ICACHE_FLASH_ATTR load_string_from_flash(char* buf,uint16 buf_len,void* flash_str);
#endif


#endif
