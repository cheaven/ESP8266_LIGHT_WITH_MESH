#ifndef __USER_SIMPLEPAIR_H__
#define __USER_SIMPLEPAIR_H__


#include "espnow.h"
#include "user_config.h"

#if ESP_NOW_SUPPORT
#define MAX_BUTTON_NUM 10
#define SP_PARAM_MAGIC 0x5c5caacc
#define DEFAULT_CHANNEL 1


typedef struct {
uint8 mac_t[DEV_MAC_LEN];
uint8 key_t[ESPNOW_KEY_LEN];
uint8 channel_t;
}PairedSingleDev;

typedef struct {
	uint8 csum;
	uint32 magic;
	PairedSingleDev PairedList[MAX_BUTTON_NUM];
	uint8 PairedNum;
	uint8 MaxPairedDevNum;
	
}PairedButtonParam;
#endif

#if ESP_SIMPLE_PAIR_SUPPORT





typedef enum{
SP_ST_STA_FINISH=0,   //success when sta finish the neg
SP_ST_AP_FINISH=0,    // success when ap finish the neg
SP_ST_AP_RECV_NEG,    // ap recv sta's neg request
SP_ST_STA_AP_REFUSE_NEG,    // sta recv ap's neg refuse
SP_ST_WAIT_TIMEOUT,
SP_ST_SEND_ERR,
SP_ST_KEY_INSTALL_ERR,
Sp_ST_KEY_OVERLAP_ERR,
SP_ST_UNKOWN_ERR,
SP_ST_MAX,
}SP_ST_t;

typedef void (*simple_pair_status_cb_t) (uint8* sa,uint8 status);
int register_simple_pair_status_cb(simple_pair_status_cb_t);
void unregister_simple_pair_status_cb(void);
int simple_pair_init();
void simple_pair_deinit();
void simple_pair_ap_enter_scan_mode(void);
void simple_pair_sta_enter_scan_mode(void);
void simple_pair_sta_start_negotiate(void);
void simple_pair_ap_start_negotiate(void);
void simple_pair_ap_refuse_negotiate(void);
int simple_pair_set_peer_ref(uint8 *peer_mac,uint8* tmpkey,uint8*espnow_key);
int simple_pair_get_peer_ref(uint8 *peer_mac,uint8* tmpkey,uint8*espnow_key);

enum SimplePairStatus{
SP_LIGHT_IDLE,//at the sta recv Button Info->light_simplepair_init then wait button nagatiate request
SP_LIGHT_WAIT_BUTTON_REQUEST,
SP_LIGHT_WAIT_USER_INDICATE_PERMIT,//PERMIT OR REFUSE
SP_LIGHT_WAIT_SIMPLE_PAIR_RESULT,
SP_LIGHT_WAIT_SIMPLE_PAIR_NEXT,//CONTINUE OR BREAK
SP_LIGHT_END,
SP_LIGHT_ERROR_HANDLE

};


#define PAIR_GET_BUTTON_LIST "/device/button/devices"
#define PAIR_START_PATH "/device/button/configure"
#define PAIR_KEEP_ALIVE  "/device/ping"
#define PAIR_FOUND_REQUEST  "/device/button/pair/request"
#define PAIR_RESULT "/device/button/pair/result"
#define PAIR_BUTTONS_MAC   "/device/button/pair/device"
#define PAIR_DELET_BUTTONS "/device/button/devices/action=delete"

#define DELET_BUTTON_LEN_TAG "\"mac_len\""
#define DELET_BUTTON_MAC_TAG "\"mac\""
extern PairedButtonParam PairedDev;

void light_simple_pair_enable(void);
void simple_pair_enable();
void sp_MacInit();
void sp_DispPairedDev(PairedButtonParam* buttonParam);
bool sp_AddPairedDev(PairedButtonParam* buttonParam,uint8* button_mac ,uint8* mac,uint8 channel);
void sp_LightPairState(void);

#endif

#endif
