/******************************************************************************
 * Copyright 2013-2014 Espressif Systems (Wuxi)
 *
 * FileName: user_webserver.c
 *
 * Description: The web server mode configration.
 *              Check your hardware connection with the host while use this mode.
 * Modification history:
 *     2014/3/12, v1.0 create this file.
*******************************************************************************/
#include "ets_sys.h"
#include "os_type.h"
#include "osapi.h"
#include "mem.h"
#include "user_interface.h"
#include "ip_addr.h"

#include "user_iot_version.h"
#include "espconn.h"
#include "user_json.h"
#include "user_webserver.h"
#include "esp_send.h"
#include "driver/uart.h"

#include "upgrade.h"
//csc
#include "user_simplepair.h"
#include"user_espnow_action.h"
#include "user_light_mesh.h"

#if ESP_PLATFORM
#include "user_esp_platform.h"
#endif

#if ESP_DEBUG_MODE
#include "user_debug.h"
#endif


#if LIGHT_DEVICE
#include "user_light.h"
#include "user_light_hint.h"
#endif

LOCAL struct station_config *sta_conf;
LOCAL struct softap_config *ap_conf;
LOCAL scaninfo *pscaninfo;
LOCAL uint32 PostCmdNeeRsp = 1;
LOCAL uint8 token_update = 0;
LOCAL os_timer_t app_upgrade_10s;
LOCAL os_timer_t upgrade_check_timer;
LOCAL struct espconn esp_conn;
LOCAL esp_tcp esptcp;

extern u16 scannum;
uint8 upgrade_lock = 0;

#if ESP_MESH_SUPPORT 
#include "mesh.h"
char *sip = NULL, *sport = NULL, *smac = NULL;
char pair_sip[1+ESP_MESH_JSON_IP_ELEM_LEN];
char pair_sport[1+ESP_MESH_JSON_PORT_ELEM_LEN];
#endif

/*
 csc add
*/
Button_info buttonPairingInfo;

/******************************************************************************
 * FunctionName : device_get
 * Description  : set up the device information parmer as a JSON format
 * Parameters   : js_ctx -- A pointer to a JSON set up
 * Returns      : result
*******************************************************************************/
LOCAL int ICACHE_FLASH_ATTR
device_get(struct jsontree_context *js_ctx)
{
    const char *path = jsontree_path_name(js_ctx, js_ctx->depth - 1);
    
    if (os_strncmp(path, "manufacture", 11) == 0) {
        jsontree_write_string(js_ctx, "Espressif Systems");
    } else if (os_strncmp(path, "product", 7) == 0) {
        #if SENSOR_DEVICE
            #if HUMITURE_SUB_DEVICE
                jsontree_write_string(js_ctx, "Humiture");
            #elif FLAMMABLE_GAS_SUB_DEVICE
                jsontree_write_string(js_ctx, "Flammable Gas");
            #endif
        #endif
        #if PLUG_DEVICE
            jsontree_write_string(js_ctx, "Plug");
        #endif
        #if LIGHT_DEVICE
            jsontree_write_string(js_ctx, "Light");
        #endif
    }
    return 0;
}

LOCAL struct jsontree_callback device_callback =JSONTREE_CALLBACK(device_get, NULL);
/******************************************************************************
 * FunctionName : userbin_get
 * Description  : get up the user bin paramer as a JSON format
 * Parameters   : js_ctx -- A pointer to a JSON set up
 * Returns      : result
*******************************************************************************/
LOCAL int ICACHE_FLASH_ATTR
userbin_get(struct jsontree_context *js_ctx)
{
    const char *path = jsontree_path_name(js_ctx, js_ctx->depth - 1);
    char string[32];
    if (os_strncmp(path, "status", 8) == 0) {
        os_sprintf(string, "200");
    } else if (os_strncmp(path, "user_bin", 8) == 0) {
        if (system_upgrade_userbin_check() == 0x00) {
            os_sprintf(string, "user1.bin");
        } else if (system_upgrade_userbin_check() == 0x01) {
            os_sprintf(string, "user2.bin");
        } else{
            return 0;
        }
    }
    jsontree_write_string(js_ctx, string);
    return 0;
}

LOCAL struct jsontree_callback userbin_callback = JSONTREE_CALLBACK(userbin_get, NULL);

JSONTREE_OBJECT(userbin_tree,
                JSONTREE_PAIR("status", &userbin_callback),
                JSONTREE_PAIR("user_bin", &userbin_callback));
JSONTREE_OBJECT(userinfo_tree,JSONTREE_PAIR("user_info",&userbin_tree));
/******************************************************************************
 * FunctionName : version_get
 * Description  : set up the device version paramer as a JSON format
 * Parameters   : js_ctx -- A pointer to a JSON set up
 * Returns      : result
*******************************************************************************/
LOCAL int ICACHE_FLASH_ATTR
version_get(struct jsontree_context *js_ctx)
{
    const char *path = jsontree_path_name(js_ctx, js_ctx->depth - 1);
    char string[32];
    if (os_strncmp(path, "hardware", 8) == 0) {
        #if SENSOR_DEVICE
            os_sprintf(string, "0.3");
        #else
            os_sprintf(string, "0.1");
        #endif
    } else if (os_strncmp(path, "sdk_version", 11) == 0) {
        os_sprintf(string, "%s", system_get_sdk_version());
    } else if (os_strncmp(path, "iot_version", 11) == 0) {
        os_sprintf(string,"%s%d.%d.%dt%d(%s)",VERSION_TYPE,IOT_VERSION_MAJOR,\
        IOT_VERSION_MINOR,IOT_VERSION_REVISION,device_type,UPGRADE_FALG);
    }
    jsontree_write_string(js_ctx, string);
    return 0;
}

LOCAL struct jsontree_callback version_callback =
    JSONTREE_CALLBACK(version_get, NULL);
JSONTREE_OBJECT(device_tree,
                JSONTREE_PAIR("product", &device_callback),
                JSONTREE_PAIR("manufacturer", &device_callback));
JSONTREE_OBJECT(version_tree,
                JSONTREE_PAIR("hardware", &version_callback),
                JSONTREE_PAIR("sdk_version", &version_callback),
                JSONTREE_PAIR("iot_version", &version_callback),
                );
JSONTREE_OBJECT(info_tree,
                JSONTREE_PAIR("Version", &version_tree),
                JSONTREE_PAIR("Device", &device_tree));

JSONTREE_OBJECT(INFOTree,
                JSONTREE_PAIR("info", &info_tree));

LOCAL int ICACHE_FLASH_ATTR
connect_status_get(struct jsontree_context *js_ctx)
{
    const char *path = jsontree_path_name(js_ctx, js_ctx->depth - 1);
    if (os_strncmp(path, "status", 8) == 0) {
        jsontree_write_int(js_ctx, user_esp_platform_get_connect_status());
    }
    return 0;
}

LOCAL struct jsontree_callback connect_status_callback =
    JSONTREE_CALLBACK(connect_status_get, NULL);

JSONTREE_OBJECT(status_sub_tree,
                JSONTREE_PAIR("status", &connect_status_callback));

JSONTREE_OBJECT(connect_status_tree,
                JSONTREE_PAIR("Status", &status_sub_tree));

JSONTREE_OBJECT(con_status_tree,
                JSONTREE_PAIR("info", &connect_status_tree));

#if PLUG_DEVICE
/******************************************************************************
 * FunctionName : status_get
 * Description  : set up the device status as a JSON format
 * Parameters   : js_ctx -- A pointer to a JSON set up
 * Returns      : result
*******************************************************************************/
LOCAL int ICACHE_FLASH_ATTR
status_get(struct jsontree_context *js_ctx)
{
    if (user_plug_get_status() == 1) {
        jsontree_write_int(js_ctx, 1);
    } else {
        jsontree_write_int(js_ctx, 0);
    }
    return 0;
}

/******************************************************************************
 * FunctionName : status_set
 * Description  : parse the device status parmer as a JSON format
 * Parameters   : js_ctx -- A pointer to a JSON set up
 *                parser -- A pointer to a JSON parser state
 * Returns      : result
*******************************************************************************/
LOCAL int ICACHE_FLASH_ATTR
status_set(struct jsontree_context *js_ctx, struct jsonparse_state *parser)
{
    int type;
    while ((type = jsonparse_next(parser)) != 0) {
        if (type == JSON_TYPE_PAIR_NAME) {
            if (jsonparse_strcmp_value(parser, "status") == 0) {
                uint8 status;
                jsonparse_next(parser);
                jsonparse_next(parser);
                status = jsonparse_get_value_as_int(parser);
                user_plug_set_status(status);
            }
        }
    }
    return 0;
}

LOCAL struct jsontree_callback status_callback =
    JSONTREE_CALLBACK(status_get, status_set);

JSONTREE_OBJECT(status_tree,
                JSONTREE_PAIR("status", &status_callback));
JSONTREE_OBJECT(response_tree,
                JSONTREE_PAIR("Response", &status_tree));
JSONTREE_OBJECT(StatusTree,
                JSONTREE_PAIR("switch", &response_tree));
#endif

#if LIGHT_DEVICE
LOCAL int ICACHE_FLASH_ATTR
light_status_get(struct jsontree_context *js_ctx)
{
    const char *path = jsontree_path_name(js_ctx, js_ctx->depth - 1);

    if (os_strncmp(path, "red", 3) == 0) {
        jsontree_write_int(js_ctx, light_param_target.pwm_duty[0]);
    } else if (os_strncmp(path, "green", 5) == 0) {
        jsontree_write_int(js_ctx, light_param_target.pwm_duty[1]);
    } else if (os_strncmp(path, "blue", 4) == 0) {
        jsontree_write_int(js_ctx, light_param_target.pwm_duty[2]);
    } else if (os_strncmp(path, "wwhite", 6) == 0) {
        if(PWM_CHANNEL>LIGHT_WARM_WHITE){
            jsontree_write_int(js_ctx, light_param_target.pwm_duty[3]);
        }else{
            jsontree_write_int(js_ctx, 0);
        }
    } else if (os_strncmp(path, "cwhite", 6) == 0) {
        if(PWM_CHANNEL>LIGHT_COLD_WHITE){
            jsontree_write_int(js_ctx, light_param_target.pwm_duty[4]);
        }else{
            jsontree_write_int(js_ctx, 0);
        }
    } else if (os_strncmp(path, "period", 6) == 0) {
        jsontree_write_int(js_ctx, light_param_target.pwm_period);
    }
    return 0;
}

LOCAL int ICACHE_FLASH_ATTR
light_status_set(struct jsontree_context *js_ctx, struct jsonparse_state *parser)
{
    int type;
    static uint32 r,g,b,cw,ww,period;
    static uint8 ctrl_mode = 0;
    period = 1000;
    cw=0;
    ww=0;
    extern uint8 light_sleep_flg;
    while ((type = jsonparse_next(parser)) != 0) {
        if (type == JSON_TYPE_PAIR_NAME) {
            if (jsonparse_strcmp_value(parser, "red") == 0) {
                uint32 status;
                jsonparse_next(parser);
                jsonparse_next(parser);
                status = jsonparse_get_value_as_int(parser);
                r=status;
                WEB_DBG("R: %d \n",status);
            } else if (jsonparse_strcmp_value(parser, "green") == 0) {
                uint32 status;
                jsonparse_next(parser);
                jsonparse_next(parser);
                status = jsonparse_get_value_as_int(parser);
                g=status;
                WEB_DBG("G: %d \n",status);
            } else if (jsonparse_strcmp_value(parser, "blue") == 0) {
                uint32 status;
                jsonparse_next(parser);
                jsonparse_next(parser);
                status = jsonparse_get_value_as_int(parser);
                b=status;
                WEB_DBG("B: %d \n",status);
            } else if (jsonparse_strcmp_value(parser, "cwhite") == 0) {
                uint32 status;
                jsonparse_next(parser);
                jsonparse_next(parser);
                status = jsonparse_get_value_as_int(parser);
                cw=status;
                WEB_DBG("CW: %d \n",status);
            } else if (jsonparse_strcmp_value(parser, "wwhite") == 0) {
                uint32 status;
                jsonparse_next(parser);
                jsonparse_next(parser);
                status = jsonparse_get_value_as_int(parser);
                ww=status;
                WEB_DBG("WW: %d \n",status);
            } else if (jsonparse_strcmp_value(parser, "period") == 0) {
                uint32 status;
                jsonparse_next(parser);
                jsonparse_next(parser);
                status = jsonparse_get_value_as_int(parser);
                WEB_DBG("PERIOD: %d \n",status);
                period=status;
            }else if (jsonparse_strcmp_value(parser, "response") == 0) {
                uint32 status;
                jsonparse_next(parser);
                jsonparse_next(parser);
                status = jsonparse_get_value_as_int(parser);
                WEB_DBG("rspneed: %d \n",status);
                PostCmdNeeRsp = status;
            }
        }
    }

    if((r|g|b|ww|cw) == 0){

    }else{
        if(light_sleep_flg==1){
            WEB_DBG("modem sleep en\r\n");
            wifi_set_sleep_type(MODEM_SLEEP_T);
            light_sleep_flg =0;
        }
    }
   light_set_color(r,g,b,cw,ww,period);
    return 0;
}

static int ICACHE_FLASH_ATTR light_switchstatus_get(struct jsontree_context *js_ctx) 
{
    char buf[24];
    char mac[6]={0,0,0,0,0,0};
    int status=0, mv=0;
    const char *path=jsontree_path_name(js_ctx, js_ctx->depth-1);
    int idx=js_ctx->index[js_ctx->depth-2];
    #if ESP_NOW_SUPPORT&&LIGHT_DEVICE
    light_EspnowGetBatteryStatus(idx, mac, &status, &mv);
    if (os_strcmp(path, "mac")==0) {
        os_sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X",mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        jsontree_write_string(js_ctx, buf);
    } else if (os_strcmp(path, "status")==0) {
        if (status==0) {
            jsontree_write_string(js_ctx, "NA");
        } else if (status==1) {
            jsontree_write_string(js_ctx, "OK");
        } else if (status==2) {
            jsontree_write_string(js_ctx, "EMPTY");
        } else {
            jsontree_write_string(js_ctx, "UNK");
        }
    } else if (os_strcmp(path, "voltagemv")==0) {
        jsontree_write_int(js_ctx, mv);
    }
    #else
    if (os_strcmp(path, "mac")==0) {
        jsontree_write_string(js_ctx, "");
    }else if (os_strcmp(path, "status")==0) {
        jsontree_write_string(js_ctx, "UNK");
    }else if (os_strcmp(path, "voltagemv")==0) {
        jsontree_write_int(js_ctx, -1);
    }
    #endif
    return 0;
}


LOCAL struct jsontree_callback light_callback =
    JSONTREE_CALLBACK(light_status_get, light_status_set);
LOCAL struct jsontree_callback switchstatus_callback =
    JSONTREE_CALLBACK(light_switchstatus_get, NULL);

JSONTREE_OBJECT(rgb_tree,
                JSONTREE_PAIR("red", &light_callback),
                JSONTREE_PAIR("green", &light_callback),
                JSONTREE_PAIR("blue", &light_callback),
                JSONTREE_PAIR("cwhite", &light_callback),
                JSONTREE_PAIR("wwhite", &light_callback),
                );
JSONTREE_OBJECT(sta_tree,
                JSONTREE_PAIR("period", &light_callback),
                JSONTREE_PAIR("rgb", &rgb_tree),
                //JSONTREE_PAIR("switches", &switch_tree)
                );
JSONTREE_OBJECT(PwmTree,
                JSONTREE_PAIR("light", &sta_tree));
/*
csc add
*/
#if ESP_NOW_SUPPORT
int ICACHE_FLASH_ATTR
button_status_set(struct jsontree_context *js_ctx, struct jsonparse_state *parser)
{
    WEB_DBG("---button set----\n");
    int type;
    uint8 mdev_mac_address[13]={0};
    uint32 replace=0;
    uint8 tempkey[33]={0};
    uint8 button_address[13]={0};
    uint8 mac_len=0;
    uint8 mac[13]={0};
    mac[12]=0;
    mdev_mac_address[12]=0;
    tempkey[32]=0;
    button_address [12]=0; 
    while ((type = jsonparse_next(parser)) != 0) {
        if (type == JSON_TYPE_PAIR_NAME) {
            if(jsonparse_strcmp_value(parser, "mdev_mac") == 0){
                jsonparse_next(parser);
                jsonparse_next(parser);
                jsonparse_copy_value(parser, mdev_mac_address, sizeof(mdev_mac_address));
                WEB_DBG("mdev_mac_address: %s \n",mdev_mac_address);
            } 
            else if (jsonparse_strcmp_value(parser, "temp_key") == 0) {
                jsonparse_next(parser);
                jsonparse_next(parser);
                jsonparse_copy_value(parser, tempkey, sizeof(tempkey));
                int j;
                char tmp[17];
                char* ptmp = tempkey;
                for(j=0;j<16;j++){
                    os_bzero(tmp,sizeof(tmp));
                    os_memcpy(tmp,ptmp,2);
                    uint8 val = strtol(tmp,NULL,16);
                    WEB_DBG("val[%d]: %02x \r\n",j,val);
                    buttonPairingInfo.tempkey[j] = val;
                    ptmp+=2;
                }
                WEB_DBG("tempkey: %s\n",tempkey,js_ctx->depth);
            } 
            else if (jsonparse_strcmp_value(parser, "button_mac") == 0) {
                jsonparse_next(parser);
                jsonparse_next(parser);
                jsonparse_copy_value(parser, button_address, sizeof(button_address));
                WEB_DBG("button_mac: %s \n",button_address);
                //os_memcpy(buttonPairingInfo.button_mac_p,button_address,sizeof(button_address));
                int j;
                char tmp[4];
                char* ptmp = button_address;
                for(j=0;j<6;j++){
                    os_bzero(tmp,sizeof(tmp));
                    os_memcpy(tmp,ptmp,2);
                    uint8 val = strtol(tmp,NULL,16);
                    WEB_DBG("val[%d]: %02x \r\n",j,val);
                    buttonPairingInfo.button_mac[j] = val;
                    ptmp+=2;
                }
            }
            else if  (jsonparse_strcmp_value(parser, "replace") == 0) {
                jsonparse_next(parser);
                jsonparse_next(parser);
                replace = jsonparse_get_value_as_int(parser);
                WEB_DBG("replace=%d\n",replace);
            }
            else if (jsonparse_strcmp_value(parser, "mac_len") == 0) {
                jsonparse_next(parser);
                jsonparse_next(parser);
                mac_len = jsonparse_get_value_as_int(parser);
                WEB_DBG("mac_len=%d\n",mac_len);
            }else if(jsonparse_strcmp_value(parser, "test") == 0){
                jsonparse_next(parser);
                jsonparse_next(parser);
                jsonparse_copy_value(parser, mac, sizeof(mac));
                WEB_DBG("test: %s \n",mac);
            }
        
        }
    }
    WEB_DBG("Type=%d\n",type);
    return 1;
}

int ICACHE_FLASH_ATTR button_status_get(struct jsontree_context *js_ctx)
{
   WEB_DBG("button get\n");
   return 1;
}

LOCAL struct jsontree_callback button_callback =
    JSONTREE_CALLBACK(button_status_get, button_status_set);

JSONTREE_OBJECT(button_remove_tree,
              JSONTREE_PAIR("mac_len", &button_callback),
              JSONTREE_PAIR("test", &button_callback),
                 );

JSONTREE_OBJECT(button_new_tree,
              JSONTREE_PAIR("temp_key", &button_callback),
              JSONTREE_PAIR("button_mac", &button_callback),
                 );

JSONTREE_OBJECT(buttonPairingInfom,
                JSONTREE_PAIR("mdev_mac", &button_callback),
                JSONTREE_PAIR("button_new", &button_new_tree),
                 JSONTREE_PAIR("replace", &button_callback),
                 JSONTREE_PAIR("button_remove", &button_remove_tree),
                //JSONTREE_PAIR("switches", &switch_tree)
                );
JSONTREE_OBJECT(button_info_tree,
               JSONTREE_PAIR("button_info", &buttonPairingInfom));



int ICACHE_FLASH_ATTR
user_change_func_map(struct jsontree_context *js_ctx, struct jsonparse_state *parser)
{
    static uint8 cnt = 0;
    
    WEB_DBG("---user_change_func_map----cnt: %d \n",cnt++);
    int type;
    int fun_pos=0;
    uint8 func[13]={0};
    uint8 short_arg[20]={0};
    uint8 long_arg[20]={0};
    uint8 i=0;
    uint8 key_value=i+1;
    char tap[12];
    
    UserEspnowAction EspnowActionTmp;
    os_memset((uint8*)&EspnowActionTmp,0,sizeof(EspnowActionTmp));
    sint8 param_flg = -1;
    sint8 cmd_type = -1;
    int cmd_idx = -1;
    uint8 err_flg = 0;
    //char key_val_chr[MAX_COMMAND_COUNT]={'1','2','3','4','U','D','L','R'};
    while ((type = jsonparse_next(parser)) != 0) {
        if (type == JSON_TYPE_PAIR_NAME){
            if (jsonparse_strcmp_value(parser, "response") == 0) {
                jsonparse_next(parser);
                jsonparse_next(parser);
                PostCmdNeeRsp = jsonparse_get_value_as_int(parser);
                WEB_DBG("rspneed: %d \n",PostCmdNeeRsp);
            }
            
            for(i=0;i<MAX_COMMAND_COUNT;i++){
				
                key_value=i+1;
                os_bzero(tap,sizeof(tap));
                os_sprintf(tap,"tap%c",key_tag[i]);
                if(jsonparse_strcmp_value(parser, tap) == 0){//find the function
                    os_bzero(func,sizeof(func));
                    jsonparse_next(parser);
                    jsonparse_next(parser);
                    jsonparse_copy_value(parser, func, sizeof(func));
                    WEB_DBG("key %c shortpress: %s \n",key_tag[i],func);
                    cmd_type = SHORT_PRESS;
                    cmd_idx = i;
                    if(os_strcmp(func,"nil")==0){
                        EspnowActionTmp.user_short_press_func_pos= -1;
                    }
                    else if((fun_pos=UserGetCorrespondFunction(func))>=0){
                        EspnowActionTmp.user_short_press_func_pos=fun_pos;
                    }else{
                        WEB_DBG("err,in %d SHORT\r\n",i);
                        err_flg = 1;
                    }
					break;
                } 
                os_bzero(tap,sizeof(tap));
                os_sprintf(tap,"arg%ctap",key_tag[i]);
                if(jsonparse_strcmp_value(parser,tap) == 0){//find  tap param
                    os_bzero(short_arg,sizeof(short_arg));
                    jsonparse_next(parser);
                    jsonparse_next(parser);
                    jsonparse_copy_value(parser, short_arg, sizeof(short_arg));
                    WEB_DBG("arg%ctap param: %s \n",key_tag[i],short_arg);
                    param_flg = 1;
					break;
                } 
                os_bzero(tap,sizeof(tap));
                os_sprintf(tap,"press%c",key_tag[i]);
                if(jsonparse_strcmp_value(parser,tap) == 0){//find the function
                    os_bzero(func,sizeof(func));
                    jsonparse_next(parser);
                    jsonparse_next(parser);
                    jsonparse_copy_value(parser, func, sizeof(func));
                    WEB_DBG("key %c longpress: %s \n",key_tag[i],func);
                    cmd_type = LONG_PRESS;
                    cmd_idx = i;
                    if(os_strcmp(func,"nil")==0){
                        EspnowActionTmp.user_long_press_func_pos= -1;
                    }
                    else if((fun_pos=UserGetCorrespondFunction(func))>=0){
                        EspnowActionTmp.user_long_press_func_pos=fun_pos;
                    }else{
                        WEB_DBG("err,in %d LONG\r\n",i);
                        err_flg = 1;
                    }
					break;
                } 
                os_bzero(tap,sizeof(tap));
                os_sprintf(tap,"arg%cpress",key_tag[i]);
                if(jsonparse_strcmp_value(parser, tap) == 0){//find press param
                    os_bzero(long_arg,sizeof(long_arg));
                    jsonparse_next(parser);
                    jsonparse_next(parser);
                    jsonparse_copy_value(parser, long_arg, sizeof(long_arg));
                    WEB_DBG("arg%cpress param: %s \n",key_tag[i],long_arg);
                    param_flg = 1;
                    //if(fun_pos>=0)UserGetCmdParam(fun_pos,long_arg,&esp_now_cmd_param[i][LONG_PRESS]);
                    break;
                } 
            }
			
			if(err_flg){
				WEB_DBG("cmd error..\r\n");
				return -1;
			}

			
			if(cmd_idx>=0 && cmd_type>=0 && param_flg>=0){
        	   WEB_DBG("==============\r\n");
        	   WEB_DBG("cmd_type: %d \r\n",cmd_type);
        	   WEB_DBG("cmd_idx: %d \r\n",cmd_idx);

        	   if((uint32)cmd_type == LONG_PRESS) WEB_DBG("LONG param: %s \r\n",long_arg);
        	   if((uint32)cmd_type == SHORT_PRESS) WEB_DBG("SHORT param: %s \r\n",short_arg);
        	   WEB_DBG("==============\r\n");
        	   if((uint32)cmd_type == LONG_PRESS){
        		   if(UserGetCmdParam(EspnowActionTmp.user_long_press_func_pos,long_arg, &userActionParam.userAction[cmd_idx].press_arg[1])){
                       userActionParam.userAction[cmd_idx].user_long_press_func_pos= EspnowActionTmp.user_long_press_func_pos;
                       //user_EspnowActionParamSave(&userActionParam);
                       WEB_DBG("L:UserGetCmdParam saved....\r\n");
        		   }else{
                       WEB_DBG("L:UserGetCmdParam error....\r\n");
        		   	}
        	   }else if((uint32)cmd_type == SHORT_PRESS){
        		   if(UserGetCmdParam(EspnowActionTmp.user_short_press_func_pos,short_arg, &userActionParam.userAction[cmd_idx].press_arg[0])){
            		   userActionParam.userAction[cmd_idx].user_short_press_func_pos = EspnowActionTmp.user_short_press_func_pos;
            		   //user_EspnowActionParamSave(&userActionParam);
        			   WEB_DBG("S:UserGetCmdParam saved....\r\n");
        		   }else{
                       WEB_DBG("S:UserGetCmdParam error....\r\n");
        		   }
        	   }
       		   cmd_idx = -1;
			   cmd_type = -1;
			   param_flg = -1;
           }

			   
        }
    }
    // WEB_DBG("Type=%d\n",type);
   user_EspnowActionParamSave(&userActionParam);
   

    return 1;
}


LOCAL int ICACHE_FLASH_ATTR
user_get_func_map(struct jsontree_context *js_ctx)
{
	WEB_DBG("in user_get_func_map\r\n");
    const char *path = jsontree_path_name(js_ctx, js_ctx->depth - 1);

	char tag_tmp[20];
	char key_val_chr[8]={'1','2','3','4','U','D','L','R'};
    char str_tmp[40];
	os_bzero(str_tmp,sizeof(str_tmp));
	int idx = 0;
    int i;
    for(i=0;i<sizeof(key_val_chr);i++){
		
		os_bzero(tag_tmp,sizeof(tag_tmp));
		os_sprintf(tag_tmp,"tap%c",key_val_chr[i]);
        if(os_strncmp(path, tag_tmp, os_strlen(tag_tmp))==0){//find the function
            idx = userActionParam.userAction[i].user_short_press_func_pos;
            WEB_DBG("find tag_tmp: %s\r\n",tag_tmp);
			if(idx>=0) os_sprintf(str_tmp,"%s",fun_tag[idx].Tag);
			else os_sprintf(str_tmp,"nil");
            jsontree_write_string(js_ctx, str_tmp);
			break;
        } 
		os_bzero(tag_tmp,sizeof(tag_tmp));
		os_sprintf(tag_tmp,"arg%ctap",key_val_chr[i]);
        if(os_strncmp(path, tag_tmp, os_strlen(tag_tmp))==0){//find  tap param     
            idx = userActionParam.userAction[i].user_short_press_func_pos;
            WEB_DBG("find tag_tmp: %s\r\n",tag_tmp);
            os_sprintf(str_tmp,"%s",tag_tmp);
			jsontree_write_string(js_ctx, str_tmp);
			break;
        } 
		os_bzero(tag_tmp,sizeof(tag_tmp));
		os_sprintf(tag_tmp,"press%c",key_val_chr[i]);
        if(os_strncmp(path, tag_tmp, os_strlen(tag_tmp))==0){//find the function
            idx = userActionParam.userAction[i].user_long_press_func_pos;
            WEB_DBG("find tag_tmp: %s\r\n",tag_tmp);
			if(idx>=0) os_sprintf(str_tmp,"%s",fun_tag[idx].Tag);
			else os_sprintf(str_tmp,"nil");
        	jsontree_write_string(js_ctx, str_tmp);
			break;
        } 
		os_bzero(tag_tmp,sizeof(tag_tmp));
		os_sprintf(tag_tmp,"arg%cpress",key_val_chr[i]);
        if(os_strncmp(path, tag_tmp, os_strlen(tag_tmp))==0){//find press param
            idx = userActionParam.userAction[i].user_long_press_func_pos;
            WEB_DBG("find tag_tmp: %s\r\n",tag_tmp);
        	os_sprintf(str_tmp,"%s",tag_tmp);
			jsontree_write_string(js_ctx, str_tmp);
			break;
        } 
    }
    return 0;
}



LOCAL struct jsontree_callback user_change_func_map_cb =
    JSONTREE_CALLBACK(user_get_func_map, user_change_func_map);

JSONTREE_OBJECT(USER_CHANGE_CMD_MAP,
         JSONTREE_PAIR("tap1", &user_change_func_map_cb),/*tap 1*/
         JSONTREE_PAIR("arg1tap", &user_change_func_map_cb),
         JSONTREE_PAIR("press1", &user_change_func_map_cb),
         JSONTREE_PAIR("arg1press", &user_change_func_map_cb),

         JSONTREE_PAIR("tap2", &user_change_func_map_cb),
         JSONTREE_PAIR("arg2tap", &user_change_func_map_cb),
         JSONTREE_PAIR("press2", &user_change_func_map_cb),
         JSONTREE_PAIR("arg2press", &user_change_func_map_cb),

         JSONTREE_PAIR("tap3", &user_change_func_map_cb),
         JSONTREE_PAIR("arg3tap", &user_change_func_map_cb),
         JSONTREE_PAIR("press3", &user_change_func_map_cb),
         JSONTREE_PAIR("arg3press", &user_change_func_map_cb),

         JSONTREE_PAIR("tap4", &user_change_func_map_cb),
         JSONTREE_PAIR("arg4tap", &user_change_func_map_cb),
         JSONTREE_PAIR("press4", &user_change_func_map_cb),
         JSONTREE_PAIR("arg4press", &user_change_func_map_cb),

          JSONTREE_PAIR("tapU", &user_change_func_map_cb),
         JSONTREE_PAIR("argUtap", &user_change_func_map_cb),
          JSONTREE_PAIR("pressU", &user_change_func_map_cb),
         JSONTREE_PAIR("argUpress", &user_change_func_map_cb),

         JSONTREE_PAIR("tapD", &user_change_func_map_cb),
         JSONTREE_PAIR("argDtap", &user_change_func_map_cb),
         JSONTREE_PAIR("pressD", &user_change_func_map_cb),
         JSONTREE_PAIR("argDpress", &user_change_func_map_cb),

         JSONTREE_PAIR("tapL", &user_change_func_map_cb),
         JSONTREE_PAIR("argLtap", &user_change_func_map_cb),
         JSONTREE_PAIR("pressL", &user_change_func_map_cb),
         JSONTREE_PAIR("argLpress", &user_change_func_map_cb),
   
         JSONTREE_PAIR("tapR", &user_change_func_map_cb),
         JSONTREE_PAIR("argRtap", &user_change_func_map_cb),
          JSONTREE_PAIR("pressR", &user_change_func_map_cb),
         JSONTREE_PAIR("argRpress", &user_change_func_map_cb),
         
         JSONTREE_PAIR("response", &user_change_func_map_cb),

);
#endif
JSONTREE_OBJECT(switch_desc,
                JSONTREE_PAIR("mac", &switchstatus_callback),
                JSONTREE_PAIR("status", &switchstatus_callback),
                JSONTREE_PAIR("voltagemv", &switchstatus_callback));
JSONTREE_ARRAY(switch_tree,
                JSONTREE_PAIR_ARRAY(&switch_desc));
JSONTREE_OBJECT(bat_tree,
                JSONTREE_PAIR("switches", &switch_tree));
JSONTREE_OBJECT(BatteryTree,
                JSONTREE_PAIR("switches", &bat_tree));

#endif

#if ESP_MESH_SUPPORT
LOCAL uint8 *MeshChildInfo = NULL;
LOCAL uint8 *MeshParentInfo = NULL;
LOCAL uint16 MeshChildrenNum = -1; 
LOCAL uint16 MeshParentNum = -1; 
LOCAL uint16 MeshNodeNum = -1;

LOCAL void ICACHE_FLASH_ATTR
	mesh_clear_topology()
{
	MeshChildInfo = NULL;
	MeshParentInfo = NULL;
	MeshChildrenNum = -1; 
	MeshParentNum = -1; 
	MeshNodeNum = -1;
}

LOCAL void ICACHE_FLASH_ATTR
	mesh_update_topology(enum mesh_node_type type)
{
    int i;
	uint16 num_t;
	int8 rssi;
    if(type==MESH_NODE_CHILD){
        int i;
        if (espconn_mesh_get_node_info(MESH_NODE_CHILD, &MeshChildInfo, &MeshChildrenNum)) {
            WEB_DBG("get child info success\n");
            if (MeshChildrenNum == 0) {
                WEB_DBG("no child\n");
            } else {
               // children represents the count of children.
               // you can read the child-information from child_info.
               struct mesh_sub_node_info *p_child_tmp;
               for(i=0;i<MeshChildrenNum;i++){
				   p_child_tmp = (struct mesh_sub_node_info *)(MeshChildInfo+i*sizeof(struct mesh_sub_node_info));
                   WEB_DBG("MAC[%d]:"MACSTR" ;NODE NUM: %d \r\n",i,MAC2STR(p_child_tmp->mac),p_child_tmp->sub_count);
               }
            }
        } else{
            WEB_DBG("get child info fail\n");
        } 
    }else if(type==MESH_NODE_PARENT){
    
        if (espconn_mesh_get_node_info(MESH_NODE_PARENT, &MeshParentInfo, &num_t)) {
			MeshParentNum = num_t&0xff;
			rssi = (num_t>>8)&0xff;
			
            WEB_DBG("get parent info success\n");
            if (MeshParentNum == 0) {
                WEB_DBG("no parent\n");
            } else {
               // children represents the count of children.
               // you can read the child-information from child_info.
               for(i=0;i<MeshParentNum;i++){
                   WEB_DBG("MAC[%d]:"MACSTR"\r\n",i,MAC2STR(MeshParentInfo+i*DEV_MAC_LEN));
               }
            }
        } else {
            WEB_DBG("get parent info fail\n");
        } 
    }else if(type==MESH_NODE_ALL){
        if(espconn_mesh_get_node_info(MESH_NODE_ALL, NULL, &MeshNodeNum)){
            WEB_DBG("get sub node number success: %d \r\n",MeshNodeNum);
        }else{
            WEB_DBG("get sub node number fail\r\n");
        }

    }
}
//---------------------------------------------------------
bool ICACHE_FLASH_ATTR mesh_root_if()
{
    struct ip_info ipconfig;
    wifi_get_ip_info(STATION_IF, &ipconfig);
	WEB_DBG("=====================\r\n\n");
	WEB_DBG("in mesh_root_if: ip: "IPSTR" \r\n",IP2STR(&(ipconfig.ip)));
	WEB_DBG("in mesh_root_if: netmask: "IPSTR"\r\n",IP2STR(&ipconfig.netmask));
	WEB_DBG("in mesh_root_if: gateway: "IPSTR"\r\n",IP2STR(&ipconfig.gw));
	WEB_DBG("=====================\r\n\n");
	
    struct ip_info apconfig;
    wifi_get_ip_info(SOFTAP_IF, &apconfig);
	WEB_DBG("=====================\r\n\n");
	WEB_DBG("in mesh_root_if: ap ip: "IPSTR"\r\n",IP2STR(&apconfig.ip));
	WEB_DBG("in mesh_root_if: ap netmask: "IPSTR"\r\n",IP2STR(&apconfig.netmask));
	WEB_DBG("in mesh_root_if: ap gateway: "IPSTR"\r\n",IP2STR(&apconfig.gw));
	WEB_DBG("=====================\r\n\n");
    if(ipconfig.ip.addr == 0) return false;

    if(espconn_mesh_local_addr(&ipconfig.ip)) return false;
    else return true;
}

static int ICACHE_FLASH_ATTR mesh_topology_get(struct jsontree_context *js_ctx) {
    char buf[24];
    os_memset(buf,0,sizeof(buf));
    const char *path = jsontree_path_name(js_ctx, js_ctx->depth-1);
    int idx=js_ctx->index[js_ctx->depth-2];
	struct mesh_sub_node_info *p_node_info;

    //WEB_DBG("=================================\r\n");
    //WEB_DBG("mesh topology idx: %d \r\n",idx);
    //WEB_DBG("path: %s \r\n",path);
    //WEB_DBG("depth: %d \r\n",js_ctx->depth);
    //WEB_DBG("=================================\r\n");
    if (os_strcmp(path, "mac")==0 && js_ctx->depth == 3) {
        if(MeshParentInfo==NULL) mesh_update_topology(MESH_NODE_PARENT);
        if(MeshParentInfo){
            os_sprintf(buf, MACSTR, MAC2STR(MeshParentInfo+DEV_MAC_LEN*idx) );
            WEB_DBG("MAC[%d]:"MACSTR"\r\n",idx,MAC2STR(MeshParentInfo+idx*DEV_MAC_LEN));
            WEB_DBG("prnt mac: %s \r\n",buf);
            jsontree_write_string(js_ctx, buf);
        }else{
            jsontree_write_string(js_ctx, "None");
        }
        
    } else if (os_strcmp(path, "type")==0 && js_ctx->depth == 4) {
        if(MeshChildInfo==NULL) mesh_update_topology(MESH_NODE_CHILD);
        if(MeshChildInfo!=NULL && idx<MeshChildrenNum ){
            jsontree_write_string(js_ctx, "Light");//LIGHT ONLY , JUST FOR NOW
        }else{
            jsontree_write_string(js_ctx, "None");
        }

    }else if(os_strcmp(path, "type")==0 && js_ctx->depth == 2){
        jsontree_write_string(js_ctx, "Light");//LIGHT ONLY , JUST FOR NOW
    }else if (os_strcmp(path, "mac")==0 && js_ctx->depth == 4) {
        if(MeshChildInfo==NULL) mesh_update_topology(MESH_NODE_CHILD);
        if(MeshChildInfo!=NULL && idx<MeshChildrenNum ){
			p_node_info = (struct mesh_sub_node_info *)(MeshChildInfo+(sizeof(struct mesh_sub_node_info))*idx);
            os_sprintf(buf, MACSTR,MAC2STR(p_node_info->mac));
            WEB_DBG("MAC[%d]:"MACSTR"\r\n",idx,MAC2STR(p_node_info->mac));
            WEB_DBG("prnt mac: %s \r\n",buf);
            jsontree_write_string(js_ctx, buf);
        }else{
            jsontree_write_string(js_ctx, "None");
        }
    }else if (os_strcmp(path, "num")==0 && js_ctx->depth == 4) {
        if(MeshChildInfo==NULL) mesh_update_topology(MESH_NODE_CHILD);
        if(MeshChildInfo!=NULL && idx<MeshChildrenNum ){
    		p_node_info = (struct mesh_sub_node_info *)(MeshChildInfo+(sizeof(struct mesh_sub_node_info))*idx);
    		WEB_DBG("NODE NUM: %d \r\n",p_node_info->sub_count);
    		jsontree_write_int(js_ctx, p_node_info->sub_count);
        }else{
			jsontree_write_int(js_ctx, 0);
        }
	}else if(os_strcmp(path, "num")==0){
        espconn_mesh_get_node_info(MESH_NODE_ALL, NULL, &MeshNodeNum);
        jsontree_write_int(js_ctx, MeshNodeNum);
    }
    return 0;
}

LOCAL struct jsontree_callback mesh_topology_callback =
    JSONTREE_CALLBACK(mesh_topology_get, NULL);

JSONTREE_OBJECT(mesh_parent_tree,
                JSONTREE_PAIR("mac", &mesh_topology_callback));
JSONTREE_OBJECT(mesh_child_desc,
                JSONTREE_PAIR("type", &mesh_topology_callback),
                JSONTREE_PAIR("mac", &mesh_topology_callback),
                JSONTREE_PAIR("num", &mesh_topology_callback));
JSONTREE_ARRAY(mesh_child_tree,
                JSONTREE_PAIR_ARRAY(&mesh_child_desc),
                JSONTREE_PAIR_ARRAY(&mesh_child_desc),
                JSONTREE_PAIR_ARRAY(&mesh_child_desc),
                JSONTREE_PAIR_ARRAY(&mesh_child_desc),
                JSONTREE_PAIR_ARRAY(&mesh_child_desc),
                JSONTREE_PAIR_ARRAY(&mesh_child_desc),
                JSONTREE_PAIR_ARRAY(&mesh_child_desc),
                JSONTREE_PAIR_ARRAY(&mesh_child_desc));
JSONTREE_OBJECT(mesh_info_tree,
                JSONTREE_PAIR("parent", &mesh_parent_tree),
                JSONTREE_PAIR("type", &mesh_topology_callback),
                JSONTREE_PAIR("num", &mesh_topology_callback),
                JSONTREE_PAIR("children", &mesh_child_tree),);
JSONTREE_OBJECT(MeshInfoTree,
                JSONTREE_PAIR("mesh_info", &mesh_info_tree));



LOCAL int ICACHE_FLASH_ATTR
mesh_sengding_status_set(struct jsontree_context *js_ctx, struct jsonparse_state *parser)
{
    int type;
    while ((type = jsonparse_next(parser)) != 0) {
        if (type == JSON_TYPE_PAIR_NAME) {
            if (jsonparse_strcmp_value(parser, "mesh_send_sta") == 0) {
                uint8 status;
                jsonparse_next(parser);
                jsonparse_next(parser);
                status = jsonparse_get_value_as_int(parser);
 	            WEB_DBG("mesh sending update status: %d \r\n",status);
				if(status == ESP_LOCAL_ACTIVE){
                    WEB_DBG("status ESP_LOCAL_ACTIVE\r\n");
					espSendSetStatus(status);
					espconn_mesh_release_congest();
				}else if(status == ESP_SERVER_ACTIVE){
                    WEB_DBG("status ESP_SERVER_ACTIVE\r\n");
					espSendSetStatus(status);
				}else{
                    WEB_DBG("unknown status: %d \r\n",status);
				}
            }else if (jsonparse_strcmp_value(parser, "response") == 0) {
                uint32 resp;
                jsonparse_next(parser);
                jsonparse_next(parser);
                resp = jsonparse_get_value_as_int(parser);
                WEB_DBG("rspneed: %d \n",resp);
                PostCmdNeeRsp = resp;
            }
        }
    }
    return 0;
}

LOCAL struct jsontree_callback mesh_send_status_callback =
    JSONTREE_CALLBACK(NULL, mesh_sengding_status_set);

JSONTREE_OBJECT(mesh_send_status_tree,
                JSONTREE_PAIR("mesh_send_sta", &mesh_send_status_callback));
JSONTREE_OBJECT(MeshSendQueueStatusTree,
                JSONTREE_PAIR("meshSendStatus", &mesh_send_status_tree));

#endif
/******************************************************************************
 * FunctionName : wifi_station_get
 * Description  : set up the station paramer as a JSON format
 * Parameters   : js_ctx -- A pointer to a JSON set up
 * Returns      : result
*******************************************************************************/
LOCAL int ICACHE_FLASH_ATTR
wifi_station_get(struct jsontree_context *js_ctx)
{
    const char *path = jsontree_path_name(js_ctx, js_ctx->depth - 1);
    struct ip_info ipconfig;
    uint8 buf[20];
    os_bzero(buf, sizeof(buf));
    wifi_station_get_config(sta_conf);
    wifi_get_ip_info(STATION_IF, &ipconfig);

    if (os_strncmp(path, "ssid", 4) == 0) {
        jsontree_write_string(js_ctx, sta_conf->ssid);
    } else if (os_strncmp(path, "password", 8) == 0) {
        jsontree_write_string(js_ctx, sta_conf->password);
    } else if (os_strncmp(path, "ip", 2) == 0) {
        os_sprintf(buf, IPSTR, IP2STR(&ipconfig.ip));
        jsontree_write_string(js_ctx, buf);
    } else if (os_strncmp(path, "mask", 4) == 0) {
        os_sprintf(buf, IPSTR, IP2STR(&ipconfig.netmask));
        jsontree_write_string(js_ctx, buf);
    } else if (os_strncmp(path, "gw", 2) == 0) {
        os_sprintf(buf, IPSTR, IP2STR(&ipconfig.gw));
        jsontree_write_string(js_ctx, buf);
    }
    return 0;
}

/******************************************************************************
 * FunctionName : wifi_station_set
 * Description  : parse the station parmer as a JSON format
 * Parameters   : js_ctx -- A pointer to a JSON set up
 *                parser -- A pointer to a JSON parser state
 * Returns      : result
*******************************************************************************/
LOCAL int ICACHE_FLASH_ATTR
wifi_station_set(struct jsontree_context *js_ctx, struct jsonparse_state *parser)
{
    int type;
    uint8 station_tree;
    WEB_INFO("TEST IN WIFI STATION SET\r\n");
    while ((type = jsonparse_next(parser)) != 0) {
        if (type == JSON_TYPE_PAIR_NAME) {
            char buffer[64];
            os_bzero(buffer, 64);
            if (jsonparse_strcmp_value(parser, "Station") == 0) {
                WEB_INFO("PARSE STATION=============\r\n");
                station_tree = 1;
            } else if (jsonparse_strcmp_value(parser, "Softap") == 0) {
                station_tree = 0;
            }

            if (station_tree) {
                if (jsonparse_strcmp_value(parser, "ssid") == 0) {
                    jsonparse_next(parser);
                    jsonparse_next(parser);
                    jsonparse_copy_value(parser, buffer, sizeof(buffer));
                    os_memcpy(sta_conf->ssid, buffer, os_strlen(buffer));
                    WEB_INFO("PARSE SSID : %s \r\n",buffer);
                } else if (jsonparse_strcmp_value(parser, "password") == 0) {
                    jsonparse_next(parser);
                    jsonparse_next(parser);
                    jsonparse_copy_value(parser, buffer, sizeof(buffer));
                    os_memcpy(sta_conf->password, buffer, os_strlen(buffer));
                    WEB_INFO("PARSE PWD : %s \r\n",buffer);
                }

#if ESP_PLATFORM

                else if (jsonparse_strcmp_value(parser, "token") == 0) {
                    jsonparse_next(parser);
                    jsonparse_next(parser);
                    jsonparse_copy_value(parser, buffer, sizeof(buffer));
                    user_esp_platform_set_token(buffer);
                    token_update = 1;
                    WEB_INFO("GET TOKEN: %s \r\n",buffer);
                }
#endif
            }
        }
    }
    return 0;
}

LOCAL struct jsontree_callback wifi_station_callback =
    JSONTREE_CALLBACK(wifi_station_get, wifi_station_set);

JSONTREE_OBJECT(get_station_config_tree,
                JSONTREE_PAIR("ssid", &wifi_station_callback),
                JSONTREE_PAIR("password", &wifi_station_callback));
JSONTREE_OBJECT(set_station_config_tree,
                JSONTREE_PAIR("ssid", &wifi_station_callback),
                JSONTREE_PAIR("password", &wifi_station_callback),
                JSONTREE_PAIR("token", &wifi_station_callback));

JSONTREE_OBJECT(ip_tree,
                JSONTREE_PAIR("ip", &wifi_station_callback),
                JSONTREE_PAIR("mask", &wifi_station_callback),
                JSONTREE_PAIR("gw", &wifi_station_callback));
JSONTREE_OBJECT(get_station_tree,
                JSONTREE_PAIR("Connect_Station", &get_station_config_tree),
                JSONTREE_PAIR("Ipinfo_Station", &ip_tree));
JSONTREE_OBJECT(set_station_tree,
                JSONTREE_PAIR("Connect_Station", &set_station_config_tree));

/******************************************************************************
 * FunctionName : wifi_softap_get
 * Description  : set up the softap paramer as a JSON format
 * Parameters   : js_ctx -- A pointer to a JSON set up
 * Returns      : result
*******************************************************************************/
LOCAL int ICACHE_FLASH_ATTR
wifi_softap_get(struct jsontree_context *js_ctx)
{
    const char *path = jsontree_path_name(js_ctx, js_ctx->depth - 1);
    struct ip_info ipconfig;
    uint8 buf[20];
    os_bzero(buf, sizeof(buf));
    wifi_softap_get_config(ap_conf);
    wifi_get_ip_info(SOFTAP_IF, &ipconfig);

    if (os_strncmp(path, "ssid", 4) == 0) {
        jsontree_write_string(js_ctx, ap_conf->ssid);
    } else if (os_strncmp(path, "password", 8) == 0) {
        jsontree_write_string(js_ctx, ap_conf->password);
    } else if (os_strncmp(path, "channel", 7) == 0) {
        jsontree_write_int(js_ctx, ap_conf->channel);
    } else if (os_strncmp(path, "authmode", 8) == 0) {
        switch (ap_conf->authmode) {
            case AUTH_OPEN:
                jsontree_write_string(js_ctx, "OPEN");
                break;
            case AUTH_WEP:
                jsontree_write_string(js_ctx, "WEP");
                break;
            case AUTH_WPA_PSK:
                jsontree_write_string(js_ctx, "WPAPSK");
                break;
            case AUTH_WPA2_PSK:
                jsontree_write_string(js_ctx, "WPA2PSK");
                break;
            case AUTH_WPA_WPA2_PSK:
                jsontree_write_string(js_ctx, "WPAPSK/WPA2PSK");
                break;
            default :
                jsontree_write_int(js_ctx, ap_conf->authmode);
                break;
        }
    } else if (os_strncmp(path, "ip", 2) == 0) {
        os_sprintf(buf, IPSTR, IP2STR(&ipconfig.ip));
        jsontree_write_string(js_ctx, buf);
    } else if (os_strncmp(path, "mask", 4) == 0) {
        os_sprintf(buf, IPSTR, IP2STR(&ipconfig.netmask));
        jsontree_write_string(js_ctx, buf);
    } else if (os_strncmp(path, "gw", 2) == 0) {
        os_sprintf(buf, IPSTR, IP2STR(&ipconfig.gw));
        jsontree_write_string(js_ctx, buf);
    }
    return 0;
}

/******************************************************************************
 * FunctionName : wifi_softap_set
 * Description  : parse the softap parmer as a JSON format
 * Parameters   : js_ctx -- A pointer to a JSON set up
 *                parser -- A pointer to a JSON parser state
 * Returns      : result
*******************************************************************************/
LOCAL int ICACHE_FLASH_ATTR
wifi_softap_set(struct jsontree_context *js_ctx, struct jsonparse_state *parser)
{
    int type;
    uint8 softap_tree;
    while ((type = jsonparse_next(parser)) != 0) {
        if (type == JSON_TYPE_PAIR_NAME) {
            char buffer[64];
            os_bzero(buffer, 64);
            if (jsonparse_strcmp_value(parser, "Station") == 0) {
                softap_tree = 0;
            } else if (jsonparse_strcmp_value(parser, "Softap") == 0) {
                softap_tree = 1;
            }
            if (softap_tree) {
                if (jsonparse_strcmp_value(parser, "authmode") == 0) {
                    jsonparse_next(parser);
                    jsonparse_next(parser);
                    jsonparse_copy_value(parser, buffer, sizeof(buffer));
                    // other mode will be supported later...
                    if (os_strcmp(buffer, "OPEN") == 0) {
                        ap_conf->authmode = AUTH_OPEN;
                    } else if (os_strcmp(buffer, "WPAPSK") == 0) {
                        ap_conf->authmode = AUTH_WPA_PSK;
                        WEB_DBG("%d %s\n", ap_conf->authmode, buffer);
                    } else if (os_strcmp(buffer, "WPA2PSK") == 0) {
                        ap_conf->authmode = AUTH_WPA2_PSK;
                    } else if (os_strcmp(buffer, "WPAPSK/WPA2PSK") == 0) {
                        ap_conf->authmode = AUTH_WPA_WPA2_PSK;
                    } else {
                        ap_conf->authmode = AUTH_OPEN;
                        return 0;
                    }
                }

                if (jsonparse_strcmp_value(parser, "channel") == 0) {
                    jsonparse_next(parser);
                    jsonparse_next(parser);
                    ap_conf->channel = jsonparse_get_value_as_int(parser);
                } else if (jsonparse_strcmp_value(parser, "ssid") == 0) {
                    jsonparse_next(parser);
                    jsonparse_next(parser);
                    jsonparse_copy_value(parser, buffer, sizeof(buffer));
                    os_memcpy(ap_conf->ssid, buffer, os_strlen(buffer));
                } else if (jsonparse_strcmp_value(parser, "password") == 0) {
                    jsonparse_next(parser);
                    jsonparse_next(parser);
                    jsonparse_copy_value(parser, buffer, sizeof(buffer));
                    os_memcpy(ap_conf->password, buffer, os_strlen(buffer));
                }
            }
        }
    }

    return 0;
}

LOCAL struct jsontree_callback wifi_softap_callback =
    JSONTREE_CALLBACK(wifi_softap_get, wifi_softap_set);

JSONTREE_OBJECT(softap_config_tree,
                JSONTREE_PAIR("authmode", &wifi_softap_callback),
                JSONTREE_PAIR("channel", &wifi_softap_callback),
                JSONTREE_PAIR("ssid", &wifi_softap_callback),
                JSONTREE_PAIR("password", &wifi_softap_callback));
JSONTREE_OBJECT(softap_ip_tree,
                JSONTREE_PAIR("ip", &wifi_softap_callback),
                JSONTREE_PAIR("mask", &wifi_softap_callback),
                JSONTREE_PAIR("gw", &wifi_softap_callback));
JSONTREE_OBJECT(get_softap_tree,
                JSONTREE_PAIR("Connect_Softap", &softap_config_tree),
                JSONTREE_PAIR("Ipinfo_Softap", &softap_ip_tree));
JSONTREE_OBJECT(set_softap_tree,
                JSONTREE_PAIR("Ipinfo_Softap", &softap_config_tree));

JSONTREE_OBJECT(get_wifi_tree,
                JSONTREE_PAIR("Station", &get_station_tree),
                JSONTREE_PAIR("Softap", &get_softap_tree));
JSONTREE_OBJECT(set_wifi_tree,
                JSONTREE_PAIR("Station", &set_station_tree),
                JSONTREE_PAIR("Softap", &set_softap_tree));

JSONTREE_OBJECT(wifi_response_tree,
                JSONTREE_PAIR("Response", &get_wifi_tree));
JSONTREE_OBJECT(wifi_request_tree,
                JSONTREE_PAIR("Request", &set_wifi_tree));

JSONTREE_OBJECT(wifi_info_tree,
                JSONTREE_PAIR("wifi", &wifi_response_tree));
JSONTREE_OBJECT(wifi_req_tree,
                JSONTREE_PAIR("wifi", &wifi_request_tree));


/******************************************************************************
 * FunctionName : scan_get
 * Description  : set up the scan data as a JSON format
 * Parameters   : js_ctx -- A pointer to a JSON set up
 * Returns      : result
*******************************************************************************/
LOCAL int ICACHE_FLASH_ATTR
scan_get(struct jsontree_context *js_ctx)
{
    const char *path = jsontree_path_name(js_ctx, js_ctx->depth - 1);
    LOCAL struct bss_info *bss;
    if (os_strncmp(path, "TotalPage", 9) == 0) {
        jsontree_write_int(js_ctx, pscaninfo->totalpage);
    } else if (os_strncmp(path, "PageNum", 7) == 0) {
        jsontree_write_int(js_ctx, pscaninfo->pagenum);
    } else if (os_strncmp(path, "bssid", 5) == 0) {
        bss = STAILQ_FIRST(pscaninfo->pbss);
        u8 buffer[32];
        os_memset(buffer, 0, sizeof(buffer));
        os_sprintf(buffer, MACSTR, MAC2STR(bss->bssid));
        jsontree_write_string(js_ctx, buffer);
    } else if (os_strncmp(path, "ssid", 4) == 0) {
        jsontree_write_string(js_ctx, bss->ssid);
    } else if (os_strncmp(path, "rssi", 4) == 0) {
        jsontree_write_int(js_ctx, -(bss->rssi));
    } else if (os_strncmp(path, "channel", 7) == 0) {
        jsontree_write_int(js_ctx, bss->channel);
    } else if (os_strncmp(path, "authmode", 8) == 0) {
        switch (bss->authmode) {
            case AUTH_OPEN:
                jsontree_write_string(js_ctx, "OPEN");
                break;
            case AUTH_WEP:
                jsontree_write_string(js_ctx, "WEP");
                break;
            case AUTH_WPA_PSK:
                jsontree_write_string(js_ctx, "WPAPSK");
                break;
            case AUTH_WPA2_PSK:
                jsontree_write_string(js_ctx, "WPA2PSK");
                break;
            case AUTH_WPA_WPA2_PSK:
                jsontree_write_string(js_ctx, "WPAPSK/WPA2PSK");
                break;
            default :
                jsontree_write_int(js_ctx, bss->authmode);
                break;
        }
        STAILQ_REMOVE_HEAD(pscaninfo->pbss, next);
        os_free(bss);
    }
    return 0;
}

LOCAL struct jsontree_callback scan_callback =
    JSONTREE_CALLBACK(scan_get, NULL);

JSONTREE_OBJECT(scaninfo_tree,
                JSONTREE_PAIR("bssid", &scan_callback),
                JSONTREE_PAIR("ssid", &scan_callback),
                JSONTREE_PAIR("rssi", &scan_callback),
                JSONTREE_PAIR("channel", &scan_callback),
                JSONTREE_PAIR("authmode", &scan_callback));
JSONTREE_ARRAY(scanrslt_tree,
               JSONTREE_PAIR_ARRAY(&scaninfo_tree),
               JSONTREE_PAIR_ARRAY(&scaninfo_tree),
               JSONTREE_PAIR_ARRAY(&scaninfo_tree),
               JSONTREE_PAIR_ARRAY(&scaninfo_tree),
               JSONTREE_PAIR_ARRAY(&scaninfo_tree),
               JSONTREE_PAIR_ARRAY(&scaninfo_tree),
               JSONTREE_PAIR_ARRAY(&scaninfo_tree),
               JSONTREE_PAIR_ARRAY(&scaninfo_tree));

JSONTREE_OBJECT(scantree,
                JSONTREE_PAIR("TotalPage", &scan_callback),
                JSONTREE_PAIR("PageNum", &scan_callback),
                JSONTREE_PAIR("ScanResult", &scanrslt_tree));
JSONTREE_OBJECT(scanres_tree,
                JSONTREE_PAIR("Response", &scantree));
JSONTREE_OBJECT(scan_tree,
                JSONTREE_PAIR("scan", &scanres_tree));

/******************************************************************************
 * FunctionName : parse_url
 * Description  : parse the received data from the server
 * Parameters   : precv -- the received data
 *                purl_frame -- the result of parsing the url
 * Returns      : none
*******************************************************************************/
LOCAL void ICACHE_FLASH_ATTR
parse_url(char *precv, URL_Frame *purl_frame)
{
    char *str = NULL;
    uint8 length = 0;
    char *pbuffer = NULL;
    char *pbufer = NULL;
    if (purl_frame == NULL || precv == NULL) {
        return;
    }

    pbuffer = (char *)os_strstr(precv, "Host:");
    if (pbuffer != NULL) {
        length = pbuffer - precv;
        pbufer = (char *)os_zalloc(length + 1);
        pbuffer = pbufer;
        os_memcpy(pbuffer, precv, length);
        os_memset(purl_frame->pSelect, 0, sizeof(purl_frame->pSelect));
        os_memset(purl_frame->pCommand, 0, sizeof(purl_frame->pCommand));
        os_memset(purl_frame->pFilename, 0, sizeof(purl_frame->pFilename));
        os_memset(purl_frame->pPath,0,sizeof(purl_frame->pPath));

        if (os_strncmp(pbuffer, "GET ", 4) == 0) {
            purl_frame->Type = GET;
            pbuffer += 4;
        } else if (os_strncmp(pbuffer, "POST ", 5) == 0) {
            purl_frame->Type = POST;
            pbuffer += 5;
        }

        //to be imporved
        str = (char *)os_strstr(pbuffer, "/");
        
        if(str){
            WEB_DBG("str,%p\r\n",str);
            char* str_tmp=str+1;
            char* str_limit = (char*)os_strstr(pbuffer," HTTP");
            if( str_limit && ((char*)os_strstr(pbuffer,"?")==NULL || (char*)os_strstr(pbuffer,"=")==NULL)){
                str = str_limit -1;
            }else{
                while(true){
                    str_tmp = (char *)os_strstr(str_tmp, "/");
                    WEB_DBG("i,%p\r\n",str_tmp);
                    if(str_tmp && str_limit && str_tmp<str_limit){
                        str = str_tmp;
                    }
                    else {
                        break;
                    }
                    str_tmp ++;
                }
            }
            WEB_DBG("o,str:%p\r\n",str);
            
            length = str - pbuffer + 1;
            os_memcpy(purl_frame->pPath,pbuffer,length);
            pbuffer = str + 1;
        }
        
        str = (char *)os_strstr(pbuffer, "?");
        if (str != NULL) {
            length = str - pbuffer;
            os_memcpy(purl_frame->pSelect, pbuffer, length);
            str ++;
            pbuffer = (char *)os_strstr(str, "=");

            if (pbuffer != NULL) {
                length = pbuffer - str;
                os_memcpy(purl_frame->pCommand, str, length);
                pbuffer ++;
                str = (char *)os_strstr(pbuffer, "&");

                if (str != NULL) {
                    length = str - pbuffer;
                    os_memcpy(purl_frame->pFilename, pbuffer, length);
                } else {
                    str = (char *)os_strstr(pbuffer, " HTTP");

                    if (str != NULL) {
                        length = str - pbuffer;
                        os_memcpy(purl_frame->pFilename, pbuffer, length);
                    }
                }
            }
        }

        os_free(pbufer);
    } else {
        return;
    }
}

LOCAL char *precvbuffer;
static uint32 dat_sumlength = 0;
LOCAL bool ICACHE_FLASH_ATTR
save_data(char *precv, uint16 length)
{
    bool flag = false;
    char length_buf[10] = {0};
    char *ptemp = NULL;
    char *pdata = NULL;
    uint16 headlength = 0;
    static uint32 totallength = 0;

    ptemp = (char *)os_strstr(precv, "\r\n\r\n");

    if (ptemp != NULL) {
        length -= ptemp - precv;
        length -= 4;
        totallength += length;
        headlength = ptemp - precv + 4;
        pdata = (char *)os_strstr(precv, "Content-Length: ");

        if (pdata != NULL) {
            pdata += 16;
            precvbuffer = (char *)os_strstr(pdata, "\r\n");

            if (precvbuffer != NULL) {
                os_memcpy(length_buf, pdata, precvbuffer - pdata);
                dat_sumlength = atoi(length_buf);
            }
        } else {
            if (totallength != 0x00){
                totallength = 0;
                dat_sumlength = 0;
                return false;
            }
        }
        if ((dat_sumlength + headlength) >= 1024) {
            precvbuffer = (char *)os_zalloc(headlength + 1);
            os_memcpy(precvbuffer, precv, headlength + 1);
        } else {
            precvbuffer = (char *)os_zalloc(dat_sumlength + headlength + 1);
            os_memcpy(precvbuffer, precv, os_strlen(precv));
        }
    } else {
        if (precvbuffer != NULL) {
            totallength += length;
            os_memcpy(precvbuffer + os_strlen(precvbuffer), precv, length);
        } else {
            totallength = 0;
            dat_sumlength = 0;
            return false;
        }
    }

    if (totallength == dat_sumlength) {
        totallength = 0;
        dat_sumlength = 0;
        return true;
    } else {
        return false;
    }
}

LOCAL bool ICACHE_FLASH_ATTR
check_data(char *precv, uint16 length)
{
    char length_buf[10] = {0};
    char *ptemp = NULL;
    char *pdata = NULL;
    char *tmp_precvbuffer;
    uint16 tmp_length = length;
    uint32 tmp_totallength = 0;
    
    ptemp = (char *)os_strstr(precv, "\r\n\r\n");
    
    if (ptemp != NULL) {
        tmp_length -= ptemp - precv;
        tmp_length -= 4;
        tmp_totallength += tmp_length;
        
        pdata = (char *)os_strstr(precv, "Content-Length: ");
        if (pdata != NULL){
            pdata += 16;
            tmp_precvbuffer = (char *)os_strstr(pdata, "\r\n");
            if (tmp_precvbuffer != NULL){
                os_memcpy(length_buf, pdata, tmp_precvbuffer - pdata);
                dat_sumlength = atoi(length_buf);
                WEB_DBG("A_dat:%u,tot:%u,lenght:%u\n",dat_sumlength,tmp_totallength,tmp_length);
                if(dat_sumlength != tmp_totallength){
                    return false;
                }
            }
        }
    }
    return true;
}

LOCAL os_timer_t *restart_10ms;
LOCAL rst_parm *rstparm;

/******************************************************************************
 * FunctionName : restart_10ms_cb
 * Description  : system restart or wifi reconnected after a certain time.
 * Parameters   : arg -- Additional argument to pass to the function
 * Returns      : none
*******************************************************************************/
LOCAL void ICACHE_FLASH_ATTR
restart_10ms_cb(void *arg)
{
    if (rstparm != NULL && rstparm->pespconn != NULL) {
        switch (rstparm->parmtype) {
            case WIFI:
                if (sta_conf->ssid[0] != 0x00) {
                    WEB_INFO("GET SSID: %s \r\n",sta_conf->ssid);
                    #if ESP_MESH_SUPPORT
                        WIFI_Connect(sta_conf->ssid, sta_conf->password,NULL);
                    #else
                        wifi_station_set_config(sta_conf);
                        wifi_station_disconnect();
                        wifi_station_connect();
                        user_esp_platform_check_ip(1);
                    #endif
                }else if(token_update == 1){
                    WEB_INFO("TOKEN SET , DO REGISTER \r\n");//reset register status here??
                    token_update = 0;
                    user_esp_platform_sent_data();
                    
                }

                if (ap_conf->ssid[0] != 0x00) {
                    wifi_softap_set_config(ap_conf);
                    system_restart();
                }

                os_free(ap_conf);
                ap_conf = NULL;
                os_free(sta_conf);
                sta_conf = NULL;
                os_free(rstparm);
                rstparm = NULL;
                os_free(restart_10ms);
                restart_10ms = NULL;
                break;
            case DEEP_SLEEP:
            case REBOOT:
                if (rstparm->pespconn->state == ESPCONN_CLOSE) {
                    wifi_set_opmode(STATION_MODE);
                    if (rstparm->parmtype == DEEP_SLEEP) {
#if SENSOR_DEVICE
                        system_deep_sleep(SENSOR_DEEP_SLEEP_TIME);
#endif
                    }
                } else {
                    os_timer_arm(restart_10ms, 10, 0);
                }
                break;
            default:
                break;
        }
    }
}

/******************************************************************************
 * FunctionName : data_send
 * Description  : processing the data as http format and send to the client or server
 * Parameters   : arg -- argument to set for client or server
 *                responseOK -- true or false
 *                psend -- The send data
 * Returns      :
*******************************************************************************/
LOCAL void ICACHE_FLASH_ATTR
data_send(void *arg, bool responseOK, char *psend)
{
#if 1
    WEB_DBG("data send.\r\n");
    struct espconn *ptrespconn = arg;
	WEB_DBG("1\r\n");
	#if MESH_HEADER_MODE
	if(espconn_mesh_get_status() == MESH_DISABLE 
		#if ESP_DEBUG_MODE
		&& DEBUG_FLG == true
		#endif
		){
        WEB_DBG("DEBUG MODE...\r\n");
		//res = espSendEnq(p_data_send, length, ptrespconn, ESP_DATA,TO_LOCAL,espSendGetRingbuf());
        data_send_buf(ptrespconn, responseOK, psend,(psend?os_strlen(psend):0),NULL,0,NULL,NULL,false);
	}else{
    	struct mesh_header_format* mesh_header = (struct mesh_header_format*)ptrespconn->reverse;
    	uint8* src_addr = NULL;//,*dst_addr = NULL;
    	bool res = espconn_mesh_get_src_addr(mesh_header,&src_addr);
    	//espconn_mesh_get_dst_addr(mesh_header,&dst_addr);	
    	WEB_DBG("dst: "MACSTR"\r\n",MAC2STR(src_addr));
    	WEB_DBG("src: "MACSTR"\r\n",MAC2STR(mesh_src_addr));
        data_send_buf(ptrespconn, responseOK, psend,(psend?os_strlen(psend):0),NULL,0,mesh_src_addr,src_addr,false);
	}
    #else
    data_send_buf(ptrespconn, responseOK, psend,os_strlen(psend),NULL,0,NULL,NULL,false);
	#endif



#else
    uint16 length = 0;
    char *pbuf = NULL;
    char httphead[256];
    struct espconn *ptrespconn = arg;
    os_memset(httphead, 0, 256);
    sint8 res;

    if (responseOK) {
        os_sprintf(httphead,
                   "HTTP/1.0 200 OK\r\nContent-Length: %d\r\nServer: lwIP/1.4.0\r\n",
                   psend ? os_strlen(psend) : 0);
        if (psend) {
            os_sprintf(httphead + os_strlen(httphead),
                       "Content-type: application/json\r\nExpires: Fri, 10 Apr 2008 14:00:00 GMT\r\nPragma: no-cache\r\n\r\n");
            length = os_strlen(httphead) + os_strlen(psend);
            pbuf = (char *)os_zalloc(length + 1);
            os_memcpy(pbuf, httphead, os_strlen(httphead));
            os_memcpy(pbuf + os_strlen(httphead), psend, os_strlen(psend));
        } else {
            os_sprintf(httphead + os_strlen(httphead), "\n");
            length = os_strlen(httphead);
        }
    } else {
        os_sprintf(httphead, "HTTP/1.0 400 BadRequest\r\n\
Content-Length: 0\r\nServer: lwIP/1.4.0\r\n\n");
        length = os_strlen(httphead);
    }

	bool queue_empty = espSendQueueIsEmpty(espSendGetRingbuf());
	WEB_DBG("send in data_send\r\n");
	uint8* p_data_send;
	if(psend){
		p_data_send = pbuf;
	}else{
		p_data_send = httphead;
	}


    #if MESH_HEADER_MODE
	if(espconn_mesh_get_status() == MESH_DISABLE && DEBUG_FLG == true){
        WEB_DBG("DEBUG MODE...\r\n");
		res = espSendEnq(p_data_send, length, ptrespconn, ESP_DATA,TO_LOCAL,espSendGetRingbuf());
	}else{
		struct mesh_header_format* mesh_header = (struct mesh_header_format*)ptrespconn->reverse;;
		uint8* src_addr = NULL;//,*dst_addr = NULL;
		espconn_mesh_get_src_addr(mesh_header,&src_addr);
		//espconn_mesh_get_dst_addr(mesh_header,&dst_addr);


		WEB_DBG("dst: "MACSTR"\r\n",MAC2STR(src_addr));
		WEB_DBG("src: "MACSTR"\r\n",MAC2STR(mesh_src_addr));
		
    	mesh_header = (struct mesh_header_format*)espconn_mesh_create_packet( src_addr,//uint8_t *dst_addr
			                                                                  mesh_src_addr,//uint8_t *src_addr,
			                                                                  false, //bool p2p,
			                                                                  true,  //bool piggyback_cr
			                                                                  M_PROTO_HTTP, //enum mesh_usr_proto_type proto
			                                                                  length, //uint16_t data_len
			                                                                  false,  //bool option
			                                                                  0, //uint16_t ot_len
			                                                                  false, //bool frag
			                                                                  0,  //enum mesh_option_type frag_type
			                                                                  false, //bool mf
			                                                                  0, //uint16_t frag_idx
			                                                                  0 //uint16_t frag_id
			                                                                  ); 
    	if(!mesh_header){ 
    		WEB_DBG("alloc resp packet fail,heap: %d\n",system_get_free_heap_size()); 
        	return; 
    	}
    	
    	if(!espconn_mesh_set_usr_data(mesh_header,p_data_send,length)){ 
    		WEB_DBG("setuserdatafail\n"); 
    		os_free(mesh_header); 
    		return; 
    	}
		
    	res = espSendEnq(mesh_header, mesh_header->len, ptrespconn, ESP_DATA,TO_LOCAL,espSendGetRingbuf());

		
		if(mesh_header) {
			os_free(mesh_header);
			mesh_header = NULL;
		}
	}
	    
	#else
    	res = espSendEnq(p_data_send, length, ptrespconn, ESP_DATA,TO_LOCAL,espSendGetRingbuf());
	#endif
	
	if(res==-1){
		WEB_DBG("espconn send error , no buf in app...\r\n");
	}
	
	/*send the packet if platform sending queue is empty*/
	/*if not, espconn sendcallback would post another sending event*/
	if(queue_empty){
		system_os_post(ESP_SEND_TASK_PRIO, 0, (os_param_t)espSendGetRingbuf());
	}
	


	#if 0
    if (psend) {
        bool queue_empty = espSendQueueIsEmpty(espSendGetRingbuf());
        WEB_DBG("send in data_send\r\n");
        res = espSendEnq(pbuf, length, ptrespconn, ESP_DATA,TO_LOCAL,espSendGetRingbuf());
        if(res==-1){
            WEB_DBG("espconn send error , no buf in app...\r\n");
        }
        
        /*send the packet if platform sending queue is empty*/
        /*if not, espconn sendcallback would post another sending event*/
        if(queue_empty){
            system_os_post(ESP_SEND_TASK_PRIO, 0, (os_param_t)espSendGetRingbuf());
        }



    } else {
        bool queue_empty = espSendQueueIsEmpty(espSendGetRingbuf());
        res = espSendEnq(httphead, length, ptrespconn, ESP_DATA,TO_LOCAL,espSendGetRingbuf());
        if(res==-1){
            WEB_DBG("espconn send error , no buf in app...\r\n");
        }
        
        /*send the packet if platform sending queue is empty*/
        /*if not, espconn sendcallback would post another sending event*/
        if(queue_empty){
            system_os_post(ESP_SEND_TASK_PRIO, 0, (os_param_t)espSendGetRingbuf());
        }
    }
	#endif
    if (pbuf) {
        os_free(pbuf);
        pbuf = NULL;
    }
	
#endif
}


void ICACHE_FLASH_ATTR
data_send_buf(void *arg, bool responseOK, char *psend,uint16 d_len,char* url,uint16 url_len,uint8* src,uint8* dst,bool p2p_msg)
{
    uint16 length = 0;
    char *pbuf = NULL;
    char* httphead = (char*)os_malloc(256+url_len);
    struct espconn *ptrespconn = arg;
    os_memset(httphead, 0, 256);
    sint8 res;

    if (responseOK) {
        if (psend) {
            if(url){
                os_sprintf(httphead,
                           "POST %s HTTP/1.0\r\nContent-Length: %d\r\nServer: lwIP/1.4.0\r\n",url,
                           psend ? d_len : 0);
            }else{
                os_sprintf(httphead,
                           "HTTP/1.0 200 OK\r\nContent-Length: %d\r\nServer: lwIP/1.4.0\r\n",
                           psend ? d_len : 0);
            }
            os_sprintf(httphead + os_strlen(httphead),
                       "Host: 0.0.0.0\r\nContent-type: application/json\r\nExpires: Fri, 10 Apr 2008 14:00:00 GMT\r\nPragma: no-cache\r\n\r\n");
            length = os_strlen(httphead) + d_len;//os_strlen(psend);
            pbuf = (char *)os_zalloc(length + 1);
            os_memcpy(pbuf, httphead, os_strlen(httphead));
            os_memcpy(pbuf + os_strlen(httphead), psend, d_len);
        } else {
            os_sprintf(httphead,
               "HTTP/1.0 200 OK\r\nContent-Length: %d\r\nServer: lwIP/1.4.0\r\n",
               psend ? d_len : 0);
            os_sprintf(httphead + os_strlen(httphead), "\n");
            length = os_strlen(httphead);
        }
    } else {
        length = os_sprintf(httphead, "HTTP/1.0 400 BadRequest\r\n\
Content-Length: 0\r\nServer: lwIP/1.4.0\r\n\n");
        //length = os_strlen(httphead);
    }


#if 1
	bool queue_empty = espSendQueueIsEmpty(espSendGetRingbuf());
	uint8* p_data_send;
	if(psend){
		p_data_send = pbuf;
	}else{
		p_data_send = httphead;
	}


    #if MESH_HEADER_MODE
	if(src==NULL && dst==NULL){
		res = espSendEnq(p_data_send, length, ptrespconn, ESP_DATA,TO_LOCAL,espSendGetRingbuf());
	}else{
		struct mesh_header_format* mesh_header = NULL;//(struct mesh_header_format*)espconn->reverse;;
		//uint8* src_addr = NULL,*dst_addr = NULL;
		//espconn_mesh_get_src_addr(mesh_header,&src_addr);
		//espconn_mesh_get_dst_addr(mesh_header,&dst_addr);


		WEB_DBG("debug src: "MACSTR"\r\n",MAC2STR(src));
		WEB_DBG("debug dst: "MACSTR"\r\n",MAC2STR(dst));
		WEB_DBG("--------create---------\r\n");
    	mesh_header = (struct mesh_header_format*)espconn_mesh_create_packet( dst,//uint8_t *dst_addr
			                                                                  src,//uint8_t *src_addr,
			                                                                  p2p_msg, //bool p2p,
			                                                                  true,  //bool piggyback_cr
			                                                                  M_PROTO_HTTP, //enum mesh_usr_proto_type proto
			                                                                  length, //uint16_t data_len
			                                                                  false,  //bool option
			                                                                  0, //uint16_t ot_len
			                                                                  false, //bool frag
			                                                                  0,  //enum mesh_option_type frag_type
			                                                                  false, //bool mf
			                                                                  0, //uint16_t frag_idx
			                                                                  0 //uint16_t frag_id
			                                                                  ); 
		WEB_DBG("----------------------\r\n");
    	if(!mesh_header){ 
    		WEB_DBG("alloc resp packet fail,heap: %d\n",system_get_free_heap_size()); 
        	return; 
    	}

		WEB_DBG("dbg send data: %d : %s\r\n",length,p_data_send);
    	if(!espconn_mesh_set_usr_data(mesh_header,p_data_send,length)){ 
    		WEB_DBG("setuserdatafail\n"); 
    		os_free(mesh_header); 
    		return; 
    	}
	    res = espSendEnq(mesh_header, mesh_header->len, ptrespconn, ESP_DATA,TO_LOCAL,espSendGetRingbuf());
		
		if(mesh_header){
            os_free(mesh_header);
			mesh_header = NULL;
		}
	}
	#else
	res = espSendEnq(p_data_send, length, ptrespconn, ESP_DATA,TO_LOCAL,espSendGetRingbuf());
	#endif

	
	
	if(res==-1){
		WEB_DBG("espconn send error , no buf in app...\r\n");
	}
	
	/*send the packet if platform sending queue is empty*/
	/*if not, espconn sendcallback would post another sending event*/
	if(queue_empty){
		system_os_post(ESP_SEND_TASK_PRIO, 0, (os_param_t)espSendGetRingbuf());
	}

#endif
    if (pbuf) {
        os_free(pbuf);
        pbuf = NULL;
    }
    if(httphead){
        os_free(httphead);
        httphead=NULL;
    }
}




void ICACHE_FLASH_ATTR json_build_packet(char *pbuf, ParmType ParmType)
{
    switch (ParmType) {
    #if LIGHT_DEVICE
        case LIGHT_STATUS:
            json_ws_send((struct jsontree_value *)&PwmTree, "light", pbuf);
            break;
        case BATTERY_STATUS:
            json_ws_send((struct jsontree_value *)&BatteryTree, "switches", pbuf);
            break;

    #elif PLUG_DEVICE
        case SWITCH_STATUS:
            json_ws_send((struct jsontree_value *)&StatusTree, "switch", pbuf);
            break;
    #endif
    #if ESP_MESH_SUPPORT
        case MESH_INFO:
            //mesh_update_topology(MESH_NODE_PARENT);
            json_ws_send((struct jsontree_value *)&MeshInfoTree, "mesh_info", pbuf);
            break;
    #endif
	#if ESP_NOW_SUPPORT
	    case ESPNOW_ACTION:
			json_ws_send((struct jsontree_value *)&USER_CHANGE_CMD_MAP, "action_info", pbuf);
			break;
	#endif
        case INFOMATION:
            json_ws_send((struct jsontree_value *)&INFOTree, "info", pbuf);
            break;

        case WIFI:
            json_ws_send((struct jsontree_value *)&wifi_info_tree, "wifi", pbuf);
            break;

        case CONNECT_STATUS:
            json_ws_send((struct jsontree_value *)&con_status_tree, "info", pbuf);
            break;

        case USER_BIN:
            json_ws_send((struct jsontree_value *)&userinfo_tree, "user_info", pbuf);
            break;
        case SCAN: {
            u8 i = 0;
            u8 scancount = 0;
            struct bss_info *bss = STAILQ_FIRST(pscaninfo->pbss);

            if (bss == NULL) {
                os_free(pscaninfo);
                pscaninfo = NULL;
                os_sprintf(pbuf, "{\n\"successful\": false,\n\"data\": null\n}");
            } else {
                    if (pscaninfo->page_sn == pscaninfo->pagenum) {
                        pscaninfo->page_sn = 0;
                        os_sprintf(pbuf, "{\n\"successful\": false,\n\"meessage\": \"repeated page\"\n}");
                        break;
                    }

                    scancount = scannum - (pscaninfo->pagenum - 1) * 8;

                    if (scancount >= 8) {
                        pscaninfo->data_cnt += 8;
                        pscaninfo->page_sn = pscaninfo->pagenum;

                        if (pscaninfo->data_cnt > scannum) {
                            pscaninfo->data_cnt -= 8;
                            os_sprintf(pbuf, "{\n\"successful\": false,\n\"meessage\": \"error page\"\n}");
                            break;
                        }

                        json_ws_send((struct jsontree_value *)&scan_tree, "scan", pbuf);
                    } else {
                        pscaninfo->data_cnt += scancount;
                        pscaninfo->page_sn = pscaninfo->pagenum;

                        if (pscaninfo->data_cnt > scannum) {
                            pscaninfo->data_cnt -= scancount;
                            os_sprintf(pbuf, "{\n\"successful\": false,\n\"meessage\": \"error page\"\n}");
                            break;
                        }
                        char ptrscanbuf[jsonSize];
                        char *pscanbuf = ptrscanbuf;
                        os_memset(ptrscanbuf, 0, jsonSize);
                        pscanbuf+=os_sprintf(pscanbuf, ",\n\"ScanResult\": [\n");
                        //pscanbuf += os_strlen(pscanbuf);

                        for (i = 0; i < scancount; i ++) {
                            bss = STAILQ_FIRST(pscaninfo->pbss);
                            if (os_memcmp(bss->ssid, "espressif", os_strlen("espressif"))) {
                                STAILQ_REMOVE_HEAD(pscaninfo->pbss, next);
                                os_free(bss);
                                i --;
                                continue;
                            }
                            JSONTREE_OBJECT(page_tree, JSONTREE_PAIR("page", &scaninfo_tree));
                            json_ws_send((struct jsontree_value *)&page_tree, "page", pscanbuf + os_strlen(pscanbuf));
                            os_sprintf(pscanbuf + os_strlen(pscanbuf), ",\n");
                            //pscanbuf += os_strlen(pscanbuf);
                        }
                        
                        pscanbuf += os_strlen(pscanbuf);
                        os_sprintf(pscanbuf - 2, "]\n");
                        JSONTREE_OBJECT(scantree, JSONTREE_PAIR("TotalPage", &scan_callback), JSONTREE_PAIR("PageNum", &scan_callback));
                        JSONTREE_OBJECT(scanres_tree, JSONTREE_PAIR("Response", &scantree));
                        JSONTREE_OBJECT(scan_tree, JSONTREE_PAIR("scan", &scanres_tree));
                        json_ws_send((struct jsontree_value *)&scan_tree, "scan", pbuf);
                        os_memcpy(pbuf + os_strlen(pbuf) - 4, ptrscanbuf, os_strlen(ptrscanbuf));
                        os_sprintf(pbuf + os_strlen(pbuf), "}\n}");
                    }
            }
            break;
        }
        default :
            break;
    }
}

#if ESP_MESH_SUPPORT
LOCAL uint16_t ICACHE_FLASH_ATTR mesh_json_copy_elem(char *dest, char *elem, uint16_t len)
{
    uint16_t j = 0;
    if (!dest || !elem || len == 0)
        return;
    for (j = 0; j < len;) {
        if (*elem == ' ' || *elem == '\t' || *elem == '\r' || *elem == '\n') {  //skip space and tab character
            elem ++;
            continue;
        }
        if (*elem == '\0')  // elem end character
            break;
        dest[j++] = *elem++;
    }
    return j;
}

bool ICACHE_FLASH_ATTR
mesh_json_add_elem(char *pbuf, size_t buf_size, char *elem, size_t elem_size)
{
    char *json_end = NULL;
    uint16_t i = 0, len = 0;
    uint8_t compensate = 1, k = 0;

    len = os_strlen(pbuf);
    if (len < 2) {
        return false;
    }

    for (i = 1; i < len; i ++)
        if (pbuf[len - i] == ESP_MESH_JSON_END_CHAR)
            break;
    if (i >= len) {
        return false;
    }

    while ((pbuf[len - i - 1] == '\n' || pbuf[len - i - 1] == '\r') && compensate < 3) {  // match format of json
        i ++;
        compensate ++ ;
    }

    if (pbuf[len - i - 1] == ESP_MESH_JSON_START_CHAR)  // the first elementary
        compensate --;

    if (len > buf_size - elem_size - compensate) {
        return false;
    }
    
    json_end = (char *)os_zalloc(i + 1);
    // copy json end string
    os_memcpy(json_end, pbuf + len - i, i + 1);  // include '\0'

    // add element
    if (pbuf[len - i - 1] != ESP_MESH_JSON_START_CHAR) {  // not the first elem
        pbuf[len - i] = ',';
        compensate --;
        len ++;
    }

    k = 0;
    while (compensate > 0) {
        pbuf[len++ - i] = json_end[k ++];
        compensate --;
    }

    len += mesh_json_copy_elem(pbuf + len - i, elem, elem_size);
    os_memcpy(pbuf + len - i, json_end, i + 1);  // include '\0'
    os_free(json_end);
    return true;
}

#endif


/******************************************************************************
 * FunctionName : json_send
 * Description  : processing the data as json format and send to the client or server
 * Parameters   : arg -- argument to set for client or server
 *                ParmType -- json format type
 * Returns      : none
*******************************************************************************/
void ICACHE_FLASH_ATTR
json_send(void *arg, ParmType ParmType)
{

#if ESP_MESH_SUPPORT
    char pbuf[jsonSize];
    struct espconn *ptrespconn = arg;
    char* dev_mac = (char*)mesh_GetMdevMac();
    os_memset((void *)pbuf, 0, sizeof(pbuf));

	#if 0
    if (!sip || !sport) {
        json_build_packet(pbuf, ParmType);
        WEB_INFO("NO SIP ,NO SPORT ... \r\n");
        WEB_INFO("DATA: \r\n%s \r\n",pbuf);
        data_send(ptrespconn, true, pbuf);
        return;
    }
    #endif
	
    json_build_packet(pbuf, ParmType);
    //if (!mesh_json_add_elem(pbuf, jsonSize, sip, ESP_MESH_JSON_IP_ELEM_LEN)) return;
    //if (!mesh_json_add_elem(pbuf, jsonSize, sport, ESP_MESH_JSON_PORT_ELEM_LEN)) return;
    if (!mesh_json_add_elem(pbuf, jsonSize, dev_mac, ESP_MESH_JSON_DEV_MAC_ELEM_LEN)) return;
    
    //if(sip) WEB_INFO("SIP: %s\r\n",sip);
    //if(sport) WEB_INFO("SPORT: %s\r\n",sport);
    WEB_INFO("dev_mac: %s\r\n",dev_mac);
    WEB_INFO("FIND SIP ,SPORT ... \r\n");
    WEB_INFO("DATA: \r\n%s \r\n",pbuf);
    int size_tmp = os_strlen(pbuf);
    pbuf[size_tmp]='\n';
    pbuf[size_tmp+1]='\0';
    data_send(ptrespconn, true, pbuf);

#else
    char *pbuf = NULL;
    pbuf = (char *)os_zalloc(jsonSize);
    struct espconn *ptrespconn = arg;
    json_build_packet(pbuf, ParmType);
    data_send(ptrespconn, true, pbuf);
    os_free(pbuf);
    pbuf = NULL;
#endif  //ESP_MESH_SUPPORT
}

/******************************************************************************
 * FunctionName : response_send
 * Description  : processing the send result
 * Parameters   : arg -- argument to set for client or server
 *                responseOK --  true or false
 * Returns      : none
*******************************************************************************/
LOCAL void ICACHE_FLASH_ATTR
response_send(void *arg, bool responseOK )
{
    WEB_DBG("func response_send\r\n");
    #if ESP_MESH_SUPPORT
        
        struct espconn *ptrespconn = arg;
        char* dev_mac = (char*)mesh_GetMdevMac();
        //const size_t len = ESP_MESH_JSON_IP_ELEM_LEN + ESP_MESH_JSON_PORT_ELEM_LEN + ESP_MESH_JSON_ROUTER_ELEM_LEN + 5;
        const size_t len = ESP_MESH_JSON_IP_ELEM_LEN + ESP_MESH_JSON_PORT_ELEM_LEN + +ESP_MESH_JSON_DEV_MAC_ELEM_LEN + 5;
        char pbuf[len];
		#if 0
        if (!sip || !sport) {
            data_send(ptrespconn, responseOK, NULL);
            return;
        }
		#endif
        os_memset(pbuf, 0, sizeof(pbuf));
        pbuf[0] = '{';
        pbuf[1] = '}';
        //if (!mesh_json_add_elem(pbuf, sizeof(pbuf), sip, ESP_MESH_JSON_IP_ELEM_LEN)) return;
        //if (!mesh_json_add_elem(pbuf, sizeof(pbuf), sport, ESP_MESH_JSON_PORT_ELEM_LEN)) return;
        if (!mesh_json_add_elem(pbuf, sizeof(pbuf), dev_mac, ESP_MESH_JSON_DEV_MAC_ELEM_LEN)) return;
        uint32 dlen = os_strlen(pbuf);
        os_sprintf(pbuf+os_strlen(pbuf),"\r\n");
        WEB_INFO("PBUF: %p ,len: %d\r\n",pbuf,dlen);
        data_send(ptrespconn, responseOK, pbuf);
    #else
        struct espconn *ptrespconn = arg;
        data_send(ptrespconn, responseOK, NULL);
    #endif
}


void ICACHE_FLASH_ATTR
response_send_str(void *arg, bool responseOK,char* pJsonDataStr , int str_len,char* url,uint16 url_len,uint8 tag_if,uint8 http_header_if,uint8* src,uint8* dst)
{
    WEB_DBG("func response_send_str\r\n");
    #if ESP_MESH_SUPPORT
        struct espconn *ptrespconn = arg;
        char* dev_mac = (char*)mesh_GetMdevMac();
        const size_t len = ESP_MESH_JSON_IP_ELEM_LEN + ESP_MESH_JSON_PORT_ELEM_LEN + ESP_MESH_JSON_DEV_MAC_ELEM_LEN + 10 + str_len;
        if(len>1300) WEB_DBG("WARNNING!!! LEN > 1300\r\n");
        char* pbuf = (char*)os_zalloc(len);
        os_memset(pbuf, 0, sizeof(pbuf));
        if(pJsonDataStr){
            os_strncpy(pbuf,pJsonDataStr,str_len);
        }else{
            if(tag_if==1) os_sprintf(pbuf,"{}");
        }
        if(tag_if ==1){
            if (!mesh_json_add_elem(pbuf, sizeof(pbuf), dev_mac, ESP_MESH_JSON_DEV_MAC_ELEM_LEN)) return;
        }else{
        }
        WEB_DBG("pbuf: %s %d  %d \r\n",pbuf,os_strlen(pbuf),len);
        uint32 dlen = os_strlen(pbuf);
        os_sprintf(pbuf+os_strlen(pbuf),"\r\n");
        WEB_INFO("PBUF: %p\r\n",pbuf);
		
        if(http_header_if==1){
            WEB_DBG("response_send_str header_if: %d \r\n",http_header_if);
            data_send_buf(ptrespconn, responseOK, pbuf,os_strlen(pbuf),url,url_len,src,dst,false);//
        }else{
            WEB_DBG("response_send_str header_if: %d \r\n",http_header_if);
            user_JsonDataSend(ptrespconn, pbuf, os_strlen(pbuf),0,src,dst);//
        }
        os_free(pbuf);
        pbuf = NULL;
    #else
        struct espconn *ptrespconn = arg;
        data_send_buf(ptrespconn, responseOK, pJsonDataStr,str_len,url,url_len,NULL,NULL,false);
    #endif
}

/******************************************************************************
 * FunctionName : json_scan_cb
 * Description  : processing the scan result
 * Parameters   : arg -- Additional argument to pass to the callback function
 *                status -- scan status
 * Returns      : none
*******************************************************************************/
LOCAL void ICACHE_FLASH_ATTR json_scan_cb(void *arg, STATUS status)
{
    pscaninfo->pbss = arg;

    if (scannum % 8 == 0) {
        pscaninfo->totalpage = scannum / 8;
    } else {
        pscaninfo->totalpage = scannum / 8 + 1;
    }

    JSONTREE_OBJECT(totaltree,
                    JSONTREE_PAIR("TotalPage", &scan_callback));
    JSONTREE_OBJECT(totalres_tree,
                    JSONTREE_PAIR("Response", &totaltree));
    JSONTREE_OBJECT(total_tree,
                    JSONTREE_PAIR("total", &totalres_tree));

    char *pbuf = NULL;
    pbuf = (char *)os_zalloc(jsonSize);
    json_ws_send((struct jsontree_value *)&total_tree, "total", pbuf);
    data_send(pscaninfo->pespconn, true, pbuf);
    os_free(pbuf);
}

void ICACHE_FLASH_ATTR
upgrade_check_func(void *arg)
{
    struct espconn *ptrespconn = arg;
    os_timer_disarm(&upgrade_check_timer);
    if(system_upgrade_flag_check() == UPGRADE_FLAG_START) {
        response_send(ptrespconn, false);
        system_upgrade_deinit();
        system_upgrade_flag_set(UPGRADE_FLAG_IDLE);
        upgrade_lock = 0;
        WEB_DBG("local upgrade failed\n");
    } else if( system_upgrade_flag_check() == UPGRADE_FLAG_FINISH ) {
        WEB_DBG("local upgrade success\n");
        response_send(ptrespconn, true);
        upgrade_lock = 0;
    } else {

    }


}
/******************************************************************************
 * FunctionName : upgrade_deinit
 * Description  : disconnect the connection with the host
 * Parameters   : bin -- server number
 * Returns      : none
*******************************************************************************/
void ICACHE_FLASH_ATTR
LOCAL local_upgrade_deinit(void)
{
    if (system_upgrade_flag_check() != UPGRADE_FLAG_START) {
        WEB_DBG("system upgrade deinit\n");
        system_upgrade_deinit();
    }
}


/******************************************************************************
 * FunctionName : upgrade_download
 * Description  : Processing the upgrade data from the host
 * Parameters   : bin -- server number
 *                pusrdata -- The upgrade data (or NULL when the connection has been closed!)
 *                length -- The length of upgrade data
 * Returns      : none
*******************************************************************************/
LOCAL void ICACHE_FLASH_ATTR
local_upgrade_download(void * arg,char *pusrdata, unsigned short length)
{
    WEB_DBG("lud: ");
    char *ptr = NULL;
    char *ptmp2 = NULL;
    char lengthbuffer[32];
    static uint32 totallength = 0;
    static uint32 sumlength = 0;
    static uint32 erase_length = 0;
    char A_buf[2] = {0xE9 ,0x03}; char  B_buf[2] = {0xEA,0x04};
    struct espconn *pespconn = arg;
    if (totallength == 0 && (ptr = (char *)os_strstr(pusrdata, "\r\n\r\n")) != NULL &&
            (ptr = (char *)os_strstr(pusrdata, "Content-Length")) != NULL) {
        ptr = (char *)os_strstr(pusrdata, "Content-Length: ");
        if (ptr != NULL) {
            ptr += 16;
            ptmp2 = (char *)os_strstr(ptr, "\r\n");

            if (ptmp2 != NULL) {
                os_memset(lengthbuffer, 0, sizeof(lengthbuffer));
                os_memcpy(lengthbuffer, ptr, ptmp2 - ptr);
                sumlength = atoi(lengthbuffer);
                if (sumlength == 0) {
                    os_timer_disarm(&upgrade_check_timer);
                    os_timer_setfn(&upgrade_check_timer, (os_timer_func_t *)upgrade_check_func, pespconn);
                    os_timer_arm(&upgrade_check_timer, 10, 0);
                    return;
                }
            } else {
                WEB_DBG("sumlength failed\n");
            }
        } else {
            WEB_DBG("Content-Length: failed\n");
        }
        ptr = (char *)os_strstr(pusrdata, "\r\n\r\n");
        length -= ptr - pusrdata;
        length -= 4;
        totallength += length;
        WEB_DBG("upgrade file download start.\n");
        system_upgrade(ptr + 4, length);
    } else {
        totallength += length;
        system_upgrade(pusrdata, length);
    }

    if (totallength == sumlength) {
        WEB_DBG("upgrade file download finished.\n");
        system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
		user_esp_set_upgrade_finish_flag();//set flag wait for reboot
        totallength = 0;
        sumlength = 0;
        upgrade_check_func(pespconn);
        os_timer_disarm(&app_upgrade_10s);
        os_timer_setfn(&app_upgrade_10s, (os_timer_func_t *)local_upgrade_deinit, NULL);
        os_timer_arm(&app_upgrade_10s, 10, 0);
    }
}

/******************************************************************************
 * FunctionName : webserver_recv
 * Description  : Processing the received data from the server
 * Parameters   : arg -- Additional argument to pass to the callback function
 *                pusrdata -- The received data (or NULL when the connection has been closed!)
 *                length -- The length of received data
 * Returns      : none
*******************************************************************************/
void ICACHE_FLASH_ATTR
webserver_recv(void *arg, char *pusrdata, unsigned short length)
{
    //URL_Frame *pURL_Frame = NULL;
    char *pParseBuffer = NULL;
    bool parse_flag = false;
    #if ESP_DEBUG_MODE
    static uint8 debug_upgrade_flg = 0;
	PostCmdNeeRsp = 1;
	
    if(debug_upgrade_flg == 0){
        WEB_DBG("tcp data:80: \r\n%s\r\n",pusrdata);
    }
    WEB_DBG("tcp length: %d \r\n",length);
    #endif

    struct espconn *ptrespconn = arg;
    WEB_DBG("espconn in webrcv: %p \r\n",ptrespconn);
	
	uint8* src_addr = NULL,*dst_addr = NULL;
    #if MESH_HEADER_MODE
	if(espconn_mesh_get_status() == MESH_DISABLE 
		#if ESP_DEBUG_MODE
		&& DEBUG_FLG == true
		#endif
		){

	}else{
    	struct mesh_header_format* mesh_header = (struct mesh_header_format*)ptrespconn->reverse;;
    	espconn_mesh_get_src_addr(mesh_header,&src_addr);
    	espconn_mesh_get_dst_addr(mesh_header,&dst_addr);
		WEB_DBG("GET SRC/DST in webserver_recv\r\n");
		WEB_DBG("SRC: "MACSTR"\r\n",MAC2STR(src_addr));
		WEB_DBG("DST: "MACSTR"\r\n",MAC2STR(dst_addr));
	}
    #endif

    if(upgrade_lock == 0){
        WEB_DBG("len:%u\n",length);
        if(check_data(pusrdata, length) == false){
            WEB_DBG("goto\n");
             goto _temp_exit;
        }
        
        parse_flag = save_data(pusrdata, length);
        if (parse_flag == false) {
            WEB_INFO("-----------\r\nRESPONSE FALSE\r\n-------------\r\n");
            response_send(ptrespconn, false);
        }
        //pURL_Frame = (URL_Frame *)os_zalloc(sizeof(URL_Frame));
		URL_Frame URL_PARSE;
		URL_Frame* pURL_Frame = &URL_PARSE;
        parse_url(precvbuffer, pURL_Frame);

        WEB_DBG("================================\r\n");
        WEB_DBG("pURL_Frame->pPath: %s\r\n",pURL_Frame->pPath);
        WEB_DBG("pURL_Frame->pSelect: %s\r\n",pURL_Frame->pSelect);
        WEB_DBG("pURL_Frame->pCommand: %s\r\n",pURL_Frame->pCommand);
        WEB_DBG("pURL_Frame->pFilename: %s\r\n",pURL_Frame->pFilename);
        WEB_DBG("pURL_Frame->Type: %d\r\n",pURL_Frame->Type);
        WEB_DBG("================================\r\n");
            
        switch (pURL_Frame->Type) {
            case GET:
                WEB_DBG("We have a GET request.\n");
                 if (os_strcmp(pURL_Frame->pSelect, "client") == 0 &&
                        os_strcmp(pURL_Frame->pCommand, "command") == 0) {
                    if (os_strcmp(pURL_Frame->pFilename, "info") == 0) {
                        json_send(ptrespconn, INFOMATION);
                    }

                    if (os_strcmp(pURL_Frame->pFilename, "status") == 0) {
                        json_send(ptrespconn, CONNECT_STATUS);
                    } else if (os_strcmp(pURL_Frame->pFilename, "scan") == 0) {
                        char *strstr = NULL;
                        strstr = (char *)os_strstr(pusrdata, "&");

                        if (strstr == NULL) {
                            if (pscaninfo == NULL) {
                                pscaninfo = (scaninfo *)os_zalloc(sizeof(scaninfo));
                            }

                            pscaninfo->pespconn = ptrespconn;
                            pscaninfo->pagenum = 0;
                            pscaninfo->page_sn = 0;
                            pscaninfo->data_cnt = 0;
                            wifi_station_scan(NULL, json_scan_cb);
                        } else {
                            strstr ++;

                            if (os_strncmp(strstr, "page", 4) == 0) {
                                if (pscaninfo != NULL) {
                                    pscaninfo->pagenum = *(strstr + 5);
                                    pscaninfo->pagenum -= 0x30;

                                    if (pscaninfo->pagenum > pscaninfo->totalpage || pscaninfo->pagenum == 0) {
                                        response_send(ptrespconn, false);
                                    } else {
                                        json_send(ptrespconn, SCAN);
                                    }
                                } else {
                                    response_send(ptrespconn, false);
                                }
                            } else {
                                response_send(ptrespconn, false);
                            }
                        }
                    } else {
                        response_send(ptrespconn, false);
                    }
                } else if (os_strcmp(pURL_Frame->pSelect, "config") == 0 &&
                           os_strcmp(pURL_Frame->pCommand, "command") == 0) {
                    if (os_strcmp(pURL_Frame->pFilename, "wifi") == 0) {
                        ap_conf = (struct softap_config *)os_zalloc(sizeof(struct softap_config));
                        sta_conf = (struct station_config *)os_zalloc(sizeof(struct station_config));
                        json_send(ptrespconn, WIFI);
                        os_free(sta_conf);
                        os_free(ap_conf);
                        sta_conf = NULL;
                        ap_conf = NULL;
                    }

#if PLUG_DEVICE
                    else if (os_strcmp(pURL_Frame->pFilename, "switch") == 0) {
                        json_send(ptrespconn, SWITCH_STATUS);
                    }
#endif

#if LIGHT_DEVICE
                    else if (os_strcmp(pURL_Frame->pFilename, "light") == 0) {
                        json_send(ptrespconn, LIGHT_STATUS);
                    }
                    else if (os_strcmp(pURL_Frame->pFilename, "switches") == 0) {
                        json_send(ptrespconn, BATTERY_STATUS);
                    }

#endif
#if ESP_MESH_SUPPORT
                    else if (os_strcmp(pURL_Frame->pFilename, "mesh_info") == 0) {
						mesh_clear_topology();
                        json_send(ptrespconn, MESH_INFO);
						
                    }
#endif
                    else if (os_strcmp(pURL_Frame->pFilename, "reboot") == 0) {
                        json_send(ptrespconn, REBOOT);
                    } else {
                        response_send(ptrespconn, false);
                    }
                } 
                else if (os_strcmp(pURL_Frame->pSelect, "upgrade") == 0 &&
                          os_strcmp(pURL_Frame->pCommand, "command") == 0) {
                    if (os_strcmp(pURL_Frame->pFilename, "getuser") == 0) {
                        json_send(ptrespconn , USER_BIN);
                    }
                } 
#if ESP_SIMPLE_PAIR_SUPPORT
                else if(os_strcmp(pURL_Frame->pPath,PAIR_KEEP_ALIVE)==0){
                    WEB_DBG("PAIR KEEP ALIVE: \r\n");
                    sp_LightPairReplyKeepAlive(ptrespconn);
                    //return;
                }
#endif

#if ESP_NOW_SUPPORT
				else if(os_strcmp(pURL_Frame->pPath, USER_SET_CMD_ACTION) == 0){
					WEB_INFO("ESPNOW_ACTION:\r\n");
					//json_send(ptrespconn , ESPNOW_ACTION);
					char data_buf[500];
					UserResponseFuncMap(data_buf,sizeof(data_buf));
					response_send_str(ptrespconn, true,data_buf , os_strlen(data_buf),NULL,0,true,true,mesh_src_addr,src_addr);//swap dst and src
				}
#endif
				else {
                    response_send(ptrespconn, false);
                }
				

                break;

            case POST:
                WEB_DBG("We have a POST request.\n");
                pParseBuffer = (char *)os_strstr(precvbuffer, "\r\n\r\n");

                if (pParseBuffer == NULL) {
                    break;
                }

                pParseBuffer += 4;


                if (os_strcmp(pURL_Frame->pSelect, "config") == 0 &&
                        os_strcmp(pURL_Frame->pCommand, "command") == 0) {
#if SENSOR_DEVICE

                    //if (os_strcmp(pURL_Frame->pFilename, "sleep") == 0) {
#else

                    if (os_strcmp(pURL_Frame->pFilename, "reboot") == 0) {
#endif

                        if (pParseBuffer != NULL) {
                            if (restart_10ms != NULL) {
                                os_timer_disarm(restart_10ms);
                            }

                            if (rstparm == NULL) {
                                rstparm = (rst_parm *)os_zalloc(sizeof(rst_parm));
                            }

                            rstparm->pespconn = ptrespconn;
#if SENSOR_DEVICE
                            rstparm->parmtype = DEEP_SLEEP;
#else
                            rstparm->parmtype = REBOOT;
#endif

                            if (restart_10ms == NULL) {
                                restart_10ms = (os_timer_t *)os_malloc(sizeof(os_timer_t));
                            }

                            os_timer_setfn(restart_10ms, (os_timer_func_t *)restart_10ms_cb, NULL);
                            os_timer_arm(restart_10ms, 10, 0);  // delay 10ms, then do

                            response_send(ptrespconn, true);
                        } else {
                            response_send(ptrespconn, false);
                        }
                    } else if (os_strcmp(pURL_Frame->pFilename, "wifi") == 0) {
                        WEB_INFO("======IN WIFI CMD=====\r\n");
                        if (pParseBuffer != NULL) {
                            struct jsontree_context js;
                            user_esp_platform_set_connect_status(DEVICE_CONNECTING);

                            if (restart_10ms != NULL) {
                                os_timer_disarm(restart_10ms);
                            }

                            if (ap_conf == NULL) {
                                ap_conf = (struct softap_config *)os_zalloc(sizeof(struct softap_config));
                            }

                            if (sta_conf == NULL) {
                                sta_conf = (struct station_config *)os_zalloc(sizeof(struct station_config));
                            }

                            jsontree_setup(&js, (struct jsontree_value *)&wifi_req_tree, json_putchar);
                            json_parse(&js, pParseBuffer);

                            if (rstparm == NULL) {
                                rstparm = (rst_parm *)os_zalloc(sizeof(rst_parm));
                            }

                            rstparm->pespconn = ptrespconn;
                            rstparm->parmtype = WIFI;

                            if (sta_conf->ssid[0] != 0x00 || ap_conf->ssid[0] != 0x00 || token_update==1) {
                                ap_conf->ssid_hidden = 0;
                                ap_conf->max_connection = 4;

                                if (restart_10ms == NULL) {
                                    restart_10ms = (os_timer_t *)os_malloc(sizeof(os_timer_t));
                                }

                                os_timer_disarm(restart_10ms);
                                os_timer_setfn(restart_10ms, (os_timer_func_t *)restart_10ms_cb, NULL);
                                os_timer_arm(restart_10ms, 10, 0);  // delay 10ms, then do
                            } else {
                                os_free(ap_conf);
                                os_free(sta_conf);
                                os_free(rstparm);
                                sta_conf = NULL;
                                ap_conf = NULL;
                                rstparm =NULL;
                            }

                            response_send(ptrespconn, true);
                        } else {
                            response_send(ptrespconn, false);
                        }
                    }

#if PLUG_DEVICE
                    else if (os_strcmp(pURL_Frame->pFilename, "switch") == 0) {
                        if (pParseBuffer != NULL) {
                            struct jsontree_context js;
                            jsontree_setup(&js, (struct jsontree_value *)&StatusTree, json_putchar);
                            json_parse(&js, pParseBuffer);
                            response_send(ptrespconn, true);
                        } else {
                            response_send(ptrespconn, false);
                        }
                    }
#endif

#if LIGHT_DEVICE
                    else if (os_strcmp(pURL_Frame->pFilename, "light") == 0) {
                        if (pParseBuffer != NULL) {
                            struct jsontree_context js;
                            jsontree_setup(&js, (struct jsontree_value *)&PwmTree, json_putchar);
                            json_parse(&js, pParseBuffer);

                            WEB_DBG("rsp1:%u\n",PostCmdNeeRsp);
                            if(PostCmdNeeRsp == 0){
                                PostCmdNeeRsp = 1;
                            }else{
                                response_send(ptrespconn, true);
                            }
                        } else {
                            response_send(ptrespconn, false);
                        }
                    }
#if ESP_MESH_SUPPORT
					else if (os_strcmp(pURL_Frame->pFilename, "meshSendStatus")==0){
                        if (pParseBuffer != NULL) {
                            struct jsontree_context js;
                            jsontree_setup(&js, (struct jsontree_value *)&MeshSendQueueStatusTree, json_putchar);
                            json_parse(&js, pParseBuffer);

                            WEB_DBG("rsp1:%u\n",PostCmdNeeRsp);
                            if(PostCmdNeeRsp == 0){
                                PostCmdNeeRsp = 1;
                            }else{
                                response_send(ptrespconn, true);
                            }
                        } else {
                            response_send(ptrespconn, false);
                        }

                    }
#endif
                    else if(os_strcmp(pURL_Frame->pFilename, "switches")==0){
                        if (pParseBuffer != NULL) {
                            struct jsontree_context js;

                            jsontree_setup(&js, (struct jsontree_value *)&BatteryTree, json_putchar);
                            json_parse(&js, pParseBuffer);

                            if(PostCmdNeeRsp == 0){
                                PostCmdNeeRsp = 1;
                            }else{
                                response_send(ptrespconn, true);
                            }
                        } else {
                            response_send(ptrespconn, false);
                        }
                    }
                    else if (os_strcmp(pURL_Frame->pFilename, "reset") == 0) {
                            response_send(ptrespconn, true);
                            extern  struct esp_platform_saved_param esp_param;
                            esp_param.activeflag = 0;
                            //user_esp_platform_save_param(&esp_param);
                            system_param_save_with_protect(ESP_PARAM_START_SEC, &esp_param, sizeof(esp_param));
                            
                            system_restore();
                            system_restart();
                    }

#endif
                    else {
                        response_send(ptrespconn, false);
                    }
                }
                else if(os_strcmp(pURL_Frame->pSelect, "upgrade") == 0 &&
                        os_strcmp(pURL_Frame->pCommand, "command") == 0){
                    if (os_strcmp(pURL_Frame->pFilename, "start") == 0){
                        response_send(ptrespconn, true);
                        WEB_DBG("local upgrade start\n");
                        upgrade_lock = 1;
						user_esp_clr_upgrade_finish_flag();//clear flag to start ota
                        system_upgrade_init();
                        system_upgrade_flag_set(UPGRADE_FLAG_START);
                        os_timer_disarm(&upgrade_check_timer);
                        os_timer_setfn(&upgrade_check_timer, (os_timer_func_t *)upgrade_check_func, NULL);
                        os_timer_arm(&upgrade_check_timer, 120000, 0);
                    }
                    else if (os_strcmp(pURL_Frame->pFilename, "start2") == 0){
                        response_send(ptrespconn, true);
                        WEB_DBG("local upgrade start\n");
                        //upgrade_lock = 1;
						user_esp_clr_upgrade_finish_flag();//clear flag to start ota
                        system_upgrade_init();
                        system_upgrade_flag_set(UPGRADE_FLAG_START);
                        os_timer_disarm(&upgrade_check_timer);
                        os_timer_setfn(&upgrade_check_timer, (os_timer_func_t *)upgrade_check_func, NULL);
                        os_timer_arm(&upgrade_check_timer, 120000, 0);
                    }
                    else if(os_strcmp(pURL_Frame->pFilename, "erase")==0){
                        char* ptr = (char *)os_strstr(pusrdata, "\r\n\r\n");
                        ptr +=4;            
                        int len_tmp = (length - (ptr-pusrdata));
                        int bin_size = user_JsonGetValueInt(ptr,len_tmp,"\"size\"");
                        WEB_DBG("-----------\r\n");
                        WEB_DBG("erase size: %d\r\n",bin_size);
                        WEB_DBG("-----------\r\n");
                        response_send(ptrespconn, true);
                        system_upgrade_erase_flash(bin_size);
                        upgrade_lock = 1;
                    }else if (os_strcmp(pURL_Frame->pFilename, "reset") == 0) {
						if(system_upgrade_flag_check() == UPGRADE_FLAG_FINISH){
                            response_send(ptrespconn, true);
                            WEB_DBG("local upgrade restart\n");
                            UART_WaitTxFifoEmpty(UART0,50000);
                            #if ESP_MESH_SUPPORT
    							WEB_DBG("deinit for OTA\r\n");
        						espconn_mesh_disable(NULL);
    							wifi_station_disconnect();
    							wifi_set_opmode(NULL_MODE);
                            #endif
    						user_esp_clr_upgrade_finish_flag();
                            system_upgrade_reboot();
						}else{
							response_send(ptrespconn, false);
							WEB_DBG("OTA NOT FINISHED...\r\n");

    				    }
                    } else {
                        response_send(ptrespconn, false);
                    }
                }
				//==================================================
#if ESP_SIMPLE_PAIR_SUPPORT //LIGHT_DEVICE
				else if(0 == os_strcmp(pURL_Frame->pPath,PAIR_START_PATH)){
					WEB_DBG("~~~~~~~~~~\r\n");
					WEB_DBG("start pairing mode \r\n");
					WEB_DBG("~~~~~~~~~~\r\n");
					
                #if ESP_MESH_SUPPORT
                    #if MESH_HEADER_MODE
        				//extern uint8 pair_src_addr[ESP_MESH_ADDR_LEN];
        				extern uint8 pair_dst_addr[ESP_MESH_ADDR_LEN];
                        //if(src_addr) os_memcpy(mesh_src,src_addr,ESP_MESH_ADDR_LEN);
        				if(src_addr) os_memcpy(pair_dst_addr,src_addr,ESP_MESH_ADDR_LEN);
					#else
        				sip = (char *)os_strstr(pusrdata, ESP_MESH_SIP_STRING);
        				sport = (char *)os_strstr(pusrdata, ESP_MESH_SPORT_STRING);
    					if(sip) {
    						os_bzero(pair_sip,sizeof(pair_sip));
    						os_memcpy(pair_sip,sip,ESP_MESH_JSON_IP_ELEM_LEN);
    						WEB_DBG("pair_sip,%d: %s \r\n",os_strlen(pair_sip),pair_sip);
    					}
    					if(sport){
    						os_bzero(pair_sport,sizeof(pair_sport));
    						os_memcpy(pair_sport,sport,ESP_MESH_JSON_PORT_ELEM_LEN);
    						WEB_DBG("pair_sport,%d: %s \r\n",os_strlen(pair_sport),pair_sport);
    					}
					#endif
                #endif
					WEB_DBG("Recv Button inform\n");
					struct jsontree_context js;
					jsontree_setup(&js, (struct jsontree_value *)&button_info_tree, json_putchar);
					json_parse(&js, pParseBuffer);
					//response_send(ptrespconn, true);
					char data_resp[100];
					os_bzero(data_resp,sizeof(data_resp));
					os_sprintf(data_resp, "{\"status\":200,\"path\":\"/device/button/configure\"}");
					WEB_DBG("response send str...\r\n");
					response_send_str(ptrespconn, true, data_resp,os_strlen(data_resp),PAIR_START_PATH,os_strlen(PAIR_START_PATH),1,0,dst_addr,src_addr);
					buttonPairingInfo.simple_pair_state=USER_PBULIC_BUTTON_INFO;
					sp_LightPairState();
					//return;

				}
				else if(0 == os_strcmp(pURL_Frame->pPath,PAIR_FOUND_REQUEST)){
					WEB_DBG("PAIR_FOUND_REQUEST \r\n");
					if((char*)os_strstr(pParseBuffer,"200")!=NULL){
						WEB_DBG("device can pair\n");
						buttonPairingInfo.simple_pair_state=USER_PERMIT_SIMPLE_PAIR;
					}
					else if((char*)os_strstr(pParseBuffer,"403")){
						WEB_DBG("device cannot pair\n");
						buttonPairingInfo.simple_pair_state=USER_REFUSE_SIMPLE_PAIR;
					}
					//pairStatus=SP_LIGHT_PAIR_REQ_RES;
					sp_LightPairState();
					//return;
				}
				else if(0 == os_strcmp(pURL_Frame->pPath,PAIR_RESULT)){
					if((char*)os_strstr(pParseBuffer,"200")!=NULL){
						WEB_DBG("device pair stop\n");
						buttonPairingInfo.simple_pair_state=USER_CONFIG_STOP_SIMPLE_PAIR;
					}
					else if((char*)os_strstr(pParseBuffer,"100")){
						WEB_DBG("device pair continue\n");
						buttonPairingInfo.simple_pair_state=USER_CONFIG_CONTIUE_SIMPLE_PAIR;
					}
					response_send(ptrespconn, true);
					//return;
				}
				else if(0 == os_strcmp(pURL_Frame->pPath,PAIR_GET_BUTTON_LIST)){
					WEB_DBG("recv: %s \r\n",PAIR_GET_BUTTON_LIST);
					sp_LightReplyPairedDev();
					//return;
				}
				else if(0==os_strcmp(pURL_Frame->pPath,PAIR_DELET_BUTTONS)){
					WEB_DBG("delect paired buttonsn\n");
					char* HttpBodyData=pParseBuffer;
					uint8 cnt=0;
					int8 flag;
					cnt=sp_GetDeletButtonCnt(pParseBuffer);
					WEB_DBG("sp_GetDeletButtonCnt=%d\n",cnt);
					HttpBodyData+=11;//jump mac_len ,else it will be found by sp_GetDeletButtonsMAC function
					uint8 DeletMacList[MAX_BUTTON_NUM][DEV_MAC_LEN];
					os_bzero(DeletMacList,sizeof(DeletMacList));
					flag=sp_GetDeletButtonsMAC(HttpBodyData,DeletMacList,cnt);
					WEB_DBG("sp_GetDeletButtonsMAC flag=%d\n",flag);
					sp_DispDeleteMacList("Delet List\n",DeletMacList);
					uint8 i=0;
					for(i=0;i<cnt;i++){
						sp_DelPairedDev(&PairedDev,DeletMacList[i]);
					}
					//return;
				}
    			else if(0==os_strcmp(pURL_Frame->pPath,USER_SET_CMD_ACTION)){
    				   WEB_DBG("User Register Cmd Action\n");
    				struct jsontree_context js;
    					jsontree_setup(&js, (struct jsontree_value *)&USER_CHANGE_CMD_MAP, json_putchar);
    					json_parse(&js, pParseBuffer);
                        if(PostCmdNeeRsp == 1){
                            response_send(ptrespconn, true);
                            //button_SendKeyFunctionSync(pParseBuffer, os_strlen(pParseBuffer));
							user_esp_platform_button_function_status((struct espconn*)user_GetUserPConn(),NULL,MESH_BROADCAST);
                        }else{
                            PostCmdNeeRsp = 1;
                        }
    					 //return;
        		 }
				
#endif
				  //==================================================

#if ESP_DEBUG_MODE
                else if(os_strcmp(pURL_Frame->pSelect, "debug") == 0){
                    WEB_DBG("in debug\r\n");
                    if(os_strcmp(pURL_Frame->pCommand, "command") == 0){
                        WEB_DBG("in command\r\n");
                        WEB_DBG("pURL_Frame->pFilename: %s \r\n",pURL_Frame->pFilename);
                        if (os_strcmp(pURL_Frame->pFilename, "setdevkey") == 0) {
                            response_send(ptrespconn, true);
                            char* ptr = (char *)os_strstr(pusrdata, "\r\n\r\n");
                            ptr +=4;                        
                            WEB_DBG("devkey: %s\r\n",ptr);
                            extern struct esp_platform_saved_param esp_param;
                            os_memcpy(esp_param.devkey,ptr,40);
                            system_param_save_with_protect(ESP_PARAM_START_SEC, &esp_param, sizeof(esp_param));
                            WEB_DBG("set devkey ....\n");
                        }else if(os_strcmp(pURL_Frame->pFilename, "ota_time") == 0){
                            extern struct esp_platform_saved_param esp_param;
                            uint8 ota_time_resp[100];
                            os_bzero(ota_time_resp,sizeof(ota_time_resp));
                            os_sprintf(ota_time_resp,"{\"ota_stime\":%d,\"ota_ftime\":%d}",esp_param.ota_start_time,esp_param.ota_finish_time);
                            data_send_buf(ptrespconn, true, ota_time_resp,os_strlen(ota_time_resp),NULL,0,NULL,NULL,false);
                        }
                        else if(os_strcmp(pURL_Frame->pFilename, "flash_erase") == 0){
                            char* ptr = (char *)os_strstr(pusrdata, "\r\n\r\n");
                            ptr +=4;            
                            int len_tmp = (length - (ptr-pusrdata));
                            //int bin_size = user_JsonGetValueInt(ptr,len_tmp,"\"size\"");
                            int sec_num = user_JsonGetValueInt(ptr,len_tmp,"\"sector_num\"");
                            int sec_start = user_JsonGetValueInt(ptr,len_tmp,"\"sector_start\"");
                            WEB_DBG("-----------\r\n");
                            WEB_DBG("sec start: %d\r\n",sec_start);
                            WEB_DBG("sec num: %d\r\n",sec_num);
                            WEB_DBG("-----------\r\n");
                            response_send(ptrespconn, true);
                            //system_upgrade_erase_flash(bin_size);
                            int i;
                            for(i=0;i<sec_num;i++){
                                spi_flash_erase_sector(sec_start+i);
                                WEB_DBG("--------------\r\n");
                                WEB_DBG("erase sec: %d \r\n",sec_start+i);
                            }
                        }
                        else if(os_strcmp(pURL_Frame->pFilename, "flash_write") == 0){
                            debug_upgrade_flg = 1;
                            char* ptr = (char *)os_strstr(pusrdata, "\r\n\r\n");
                            ptr +=4;            
                            int len_tmp = (length - (ptr-pusrdata));
                            int addr = user_JsonGetValueInt(ptr,len_tmp,"\"addr\"");
                            ptr = (char *)os_strstr(pusrdata, "data:");
                            ptr += 5;
                            int data_len = (length - (ptr-pusrdata));
                            WEB_DBG("-------------\r\n");
                            WEB_DBG("write: %08x; len: %d\r\n",addr,data_len);
                            uint8* dtmp = (uint8*)os_zalloc(data_len);
                            os_memcpy(dtmp,ptr,data_len);
                            spi_flash_write(addr,(uint32*)dtmp,data_len);
                            response_send(ptrespconn, true);
                            os_free(dtmp);
                        }
						else if (os_strcmp(pURL_Frame->pFilename, "ota_dbg_stop") == 0){
							#if ESP_MESH_SUPPORT
							system_upgrade_flag_set(UPGRADE_FLAG_IDLE);
							extern os_timer_t g_mesh_upgrade_timer;
							os_timer_disarm(&g_mesh_upgrade_timer);
							struct espconn* pespconn = (struct espconn*)user_GetUserPConn();
							mesh_upgrade_check_func(pespconn);
							WEB_DBG("Upgrade stopped...\r\n");
							#endif
							response_send(ptrespconn, true);
						}
						else if (os_strcmp(pURL_Frame->pFilename, "ota_dbg_reboot") == 0){
						    sint8 ota_flg = system_upgrade_flag_check();
							if(ota_flg == UPGRADE_FLAG_FINISH || (1 ==user_esp_get_upgrade_finish_flag())){
								light_shadeStart(HINT_GREEN,500,0,0,NULL);
								debug_OtaRebootStart(10000);
							}else{
								WEB_DBG("UPGRADE NOT FINISHED...\r\n");
							}
						}else if(os_strcmp(pURL_Frame->pFilename, "force_reboot") == 0){
							WEB_DBG("force reboot,system restart ");
							#if ESP_MESH_SUPPORT
								WEB_DBG("deinit for OTA\r\n");
    							espconn_mesh_disable(NULL);
								wifi_station_disconnect();
								wifi_set_opmode(NULL_MODE);
                            #endif
							system_restart();

						}
                        else if(os_strcmp(pURL_Frame->pFilename, "upgrade_reboot") == 0){
                            debug_upgrade_flg = 0;
                            WEB_DBG("upgrade ...\r\n");
                            user_esp_platform_set_reset_flg(MODE_APMODE);
							
                            #if ESP_MESH_SUPPORT
								WEB_DBG("deinit for OTA\r\n");
    							espconn_mesh_disable(NULL);
								wifi_station_disconnect();
								wifi_set_opmode(NULL_MODE);
                            #endif
                            UART_WaitTxFifoEmpty(0,50000);
                            system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
							user_esp_clr_upgrade_finish_flag();//clear flag and reboot
                            system_upgrade_reboot();
                        }
                        else if(os_strcmp(pURL_Frame->pFilename, "flash_dbg_len") == 0) {
                            //uint8 dbg_len_buf[100];
                            uint8* dbg_len_buf = (uint8*)os_zalloc(100);
                            
                            //os_memset(dbg_len_buf,0,sizeof(dbg_len_buf));
                            os_sprintf(dbg_len_buf,"{\"debug_info_len\":%d}",debug_GetFlashDebugInfoLen(&FlashDebugBufParam));
                            data_send(ptrespconn, true, dbg_len_buf);
                            os_free(dbg_len_buf);
                        }else if(os_strcmp(pURL_Frame->pFilename, "dbg_toggle_bin") == 0) {
                            WEB_DBG("toggle_bin...");
                            WEB_DBG("current bin: %d \r\n",system_upgrade_userbin_check());
                            response_send(ptrespconn, true);
                            UART_WaitTxFifoEmpty(0,50000);
                            system_upgrade_flag_set(UPGRADE_FLAG_IDLE);
                            system_upgrade_flag_set(UPGRADE_FLAG_START);
                            system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
							
                            #if ESP_MESH_SUPPORT
								WEB_DBG("deinit for OTA\r\n");
    							espconn_mesh_disable(NULL);
								wifi_station_disconnect();
								wifi_set_opmode(NULL_MODE);
                            #endif
							user_esp_clr_upgrade_finish_flag();//clear flag and reboot
                            system_upgrade_reboot();

                        }
						else if (os_strcmp(pURL_Frame->pFilename, "flash_dbg_force") == 0) {
                            char* ptr = (char *)os_strstr(pusrdata, "\r\n\r\n");
                            ptr +=4;            
                            int len_tmp = (length - (ptr-pusrdata));
                            int offset = user_JsonGetValueInt(ptr,len_tmp,"\"offset\"");
                            int info_size = user_JsonGetValueInt(ptr,len_tmp,"\"size\"");
						
    						WEB_DBG("debug info offset: %d \r\n",offset);
    						WEB_DBG("debug info size: %d \r\n",info_size);

							uint8* debug_flash_info_force = (uint8*)os_zalloc(info_size+1);
							debug_GetFlashExceptInfoForce(debug_flash_info_force, info_size,offset);
                            data_send(ptrespconn, true, debug_flash_info_force);
                            os_free(debug_flash_info_force);
                        }	
						else if(os_strcmp(pURL_Frame->pFilename, "flash_print_reset")==0){
							debug_FlashPrintParamReset();
							response_send(ptrespconn, true);

						}
						else if (os_strcmp(pURL_Frame->pFilename, "flash_get") == 0) {
                            char* ptr = (char *)os_strstr(pusrdata, "\r\n\r\n");
                            ptr +=4;            
                            int len_tmp = (length - (ptr-pusrdata));
                            int offset = user_JsonGetValueInt(ptr,len_tmp,"\"offset\"");
                            int info_size = user_JsonGetValueInt(ptr,len_tmp,"\"size\"");
                            int addr = user_JsonGetValueInt(ptr,len_tmp,"\"addr\"");

							uint8* debug_flash_get_force = (uint8*)os_zalloc(info_size+1);
							debug_GetFlashDataForce(debug_flash_get_force, addr+offset, info_size);
                            data_send(ptrespconn, true, debug_flash_get_force);
                            os_free(debug_flash_get_force);
                        }


						else if (os_strcmp(pURL_Frame->pFilename, "flash_dbg_info") == 0) {
                            char* ptr = (char *)os_strstr(pusrdata, "\r\n\r\n");
                            ptr +=4;            
                            int len_tmp = (length - (ptr-pusrdata));
                            int offset = user_JsonGetValueInt(ptr,len_tmp,"\"offset\"");
                            int info_size = user_JsonGetValueInt(ptr,len_tmp,"\"size\"");
                            
                            WEB_DBG("debug info offset: %d \r\n",offset);
                            WEB_DBG("debug info size: %d \r\n",info_size);
                            //int info_size = 1000;//debug_GetFlashDebugInfoLen();
                            //if( offset>info_size) offset=info_size;

                            if(info_size > 0){
                                uint8* debug_flash_info = (uint8*)os_zalloc(info_size+1);
                                debug_GetFlashExceptInfo(debug_flash_info, info_size,offset);
                                data_send(ptrespconn, true, debug_flash_info);
                                os_free(debug_flash_info);
                            }else{
                                response_send(ptrespconn, true);
                            }
                        }
                        else if(os_strcmp(pURL_Frame->pFilename, "flash_dbg_reset") == 0) {
                            debug_FlashBufReset(&FlashDebugBufParam,Flash_DEBUG_INFO_ADDR);
                            response_send(ptrespconn, true);
                        }
                        else if(os_strcmp(pURL_Frame->pFilename, "dbg_set_ver") == 0){
                            char* ptr = (char *)os_strstr(pusrdata, "\r\n\r\n");
                            ptr +=4;            
                            int len_tmp = (length - (ptr-pusrdata));
                            
                            WEB_DBG("json data: %s\r\n",ptr);
                            int ver = user_JsonGetValueInt(ptr,len_tmp,"\"debug_version\"");
                            WEB_DBG("ver parse: %d \r\n",ver);
                            debug_SetDebugVersion(ver);
                            WEB_DBG("get version: %d \r\n",debug_GetDebugVersion());
                            response_send(ptrespconn, true);
                        }
                        else if(os_strcmp(pURL_Frame->pFilename, "get_test_ver") == 0){
                            uint8 resp_buf[100];
                            os_memset(resp_buf,0,100);
                            os_sprintf(resp_buf,"{\"test_version\":%d}\n",MESH_TEST_VERSION);
                            data_send(ptrespconn, true, resp_buf);
                        }

                        
                        else if(os_strcmp(pURL_Frame->pFilename, "dbg_get_ver") == 0){
                            uint8 resp_buf[100];
                            os_memset(resp_buf,0,100);
                            os_sprintf(resp_buf,"{\"debug_version\":%d}\n",debug_GetDebugVersion());
                            data_send(ptrespconn, true, resp_buf);
                        }
                        else if (os_strcmp(pURL_Frame->pFilename, "mac_info") == 0) {
                            uint32 MAC_FLG = READ_PERI_REG(0x3ff00054);
                            uint8 data_buf[32];
                            os_memset(data_buf,0,sizeof(data_buf));
                            MAC_FLG = ((MAC_FLG>>16)&0xff);
                            
                            struct station_config sta_conf;
                            uint8 mac_sta[6];
                            wifi_get_macaddr(STATION_IF, mac_sta);

                            os_sprintf(data_buf,"%02x%02x%02x%02x%02x%02x",MAC2STR(mac_sta));
                            data_send(ptrespconn, true, data_buf);
                            WEB_DBG("mac: %s\n",data_buf);
                            UART_WaitTxFifoEmpty(UART0,50000);
                            //system_upgrade_reboot();
                        }
                        else if (os_strcmp(pURL_Frame->pFilename, "key_info") == 0) {
                            uint32 MAC_FLG = READ_PERI_REG(0x3ff00054);
                            uint8 data_buf[41];
                            os_memset(data_buf,0,sizeof(data_buf));

                            extern struct esp_platform_saved_param esp_param;
                            os_memcpy(data_buf,esp_param.devkey,40);
                            data_send(ptrespconn, true, data_buf);
                            WEB_DBG("mac: %s\n",data_buf);
                            UART_WaitTxFifoEmpty(UART0,50000);
                            //system_upgrade_reboot();
                        }
                        else if (os_strcmp(pURL_Frame->pFilename, "reboot_to_ap") == 0) {
                            
                            user_esp_platform_set_reset_flg(MODE_APMODE);
                            response_send(ptrespconn, true);
                            WEB_DBG("local upgrade restart\n");
                            UART_WaitTxFifoEmpty(UART0,50000);
							
                            #if ESP_MESH_SUPPORT
								WEB_DBG("deinit for OTA\r\n");
    							espconn_mesh_disable(NULL);
								wifi_station_disconnect();
								wifi_set_opmode(NULL_MODE);
                            #endif
							user_esp_clr_upgrade_finish_flag();
                            system_upgrade_reboot();
                            WEB_DBG("call upgrade reboot ...\r\n");
                            UART_WaitTxFifoEmpty(UART0,50000);
                        }
                        else if(os_strcmp(pURL_Frame->pFilename, "set_switch_mac") == 0){
							//not used 
                            response_send(ptrespconn, false);
                        }
                        else if(os_strcmp(pURL_Frame->pFilename, "get_init_data") == 0){
                            //WEB_DBG("t1");
                            //uint8 init_data[256] = {0};
                            uint8* init_data = (uint8*)os_zalloc(256);
                            //WEB_DBG("t2");
                            spi_flash_read(0xfc000,(uint32*)init_data,128);
                            //WEB_DBG("t3");
                            data_send_buf(ptrespconn, true, init_data,128,NULL,0,NULL,NULL,false);
                            //WEB_DBG("t4");
                            os_free(init_data);
                            init_data = NULL;
                            //data_send(ptrespconn, true, init_data_resp);
                        }
                        else if(os_strcmp(pURL_Frame->pFilename, "set_init_data") == 0){
                            //uint8 init_data[128] = {0};
                            uint8* init_data = (uint8*)os_zalloc(256);
                            
                            char* ptr = (char *)os_strstr(pusrdata, "\r\n\r\n");
                            ptr +=4;            
                            int len_tmp = (length - (ptr-pusrdata));
                            if(len_tmp!=128){
                                response_send(ptrespconn, false);
                                //return;
                            }else{
                                os_memcpy(init_data,ptr,128);
                                spi_flash_erase_sector(0xfc);
                                spi_flash_write(0xfc000,(uint32*)init_data,128);
                                spi_flash_read(0xfc000,(uint32*)init_data,128);
                                data_send_buf(ptrespconn, true, init_data,128,NULL,0,NULL,NULL,false);
                            }
                        }
                        
                    }else if(os_strcmp(pURL_Frame->pCommand, "setboot") == 0){
                        if (os_strcmp(pURL_Frame->pFilename, "40m") == 0) {
                            uint8* data_bkp = (uint8*)os_zalloc(0x1000);
                            if( data_bkp ==NULL){
                                response_send(ptrespconn, false);
                                //return;
                            }
                            char* ptmp = pURL_Frame->pFilename;
                            spi_flash_read(0x0,(uint32*)data_bkp,0x1000);
                            WEB_DBG("data bkp: %02x %02x %02x %02x %02x %02x\r\n",data_bkp[0],data_bkp[1],data_bkp[2],data_bkp[3],data_bkp[4],data_bkp[5]); 
                            *(data_bkp+2) = 0x0;
                            *(data_bkp+3) = 0x20;
                            spi_flash_erase_sector(0);
                            spi_flash_write(0x0,(uint32 *)data_bkp,0x1000);
                            spi_flash_read(0x0,(uint32*)data_bkp,0x1000);
                            WEB_DBG("data bkp check: %02x %02x %02x %02x %02x %02x\r\n",data_bkp[0],data_bkp[1],data_bkp[2],data_bkp[3],data_bkp[4],data_bkp[5]); 
                            response_send(ptrespconn, true);
                            WEB_DBG("set devkey ....\n");
                            UART_WaitTxFifoEmpty(UART0,50000);
                            os_free(data_bkp);
                            data_bkp=NULL;
                        }
                        else if (os_strcmp(pURL_Frame->pFilename, "80m") == 0) {
                            uint8* data_bkp = (uint8*)os_zalloc(0x1000);
                            if( data_bkp ==NULL){
                                response_send(ptrespconn, false);
                                //return;
                            }
                            char* ptmp = pURL_Frame->pFilename;
                            spi_flash_read(0x0,(uint32*)data_bkp,0x1000);
                            WEB_DBG("data bkp: %02x %02x %02x %02x %02x %02x\r\n",data_bkp[0],data_bkp[1],data_bkp[2],data_bkp[3],data_bkp[4],data_bkp[5]); 
                            *(data_bkp+2) = 0x0;
                            *(data_bkp+3) = 0x2f;
                            spi_flash_erase_sector(0);
                            spi_flash_write(0x0,(uint32 *)data_bkp,0x1000);
                            spi_flash_read(0x0,(uint32*)data_bkp,0x1000);
                            WEB_DBG("data bkp check: %02x %02x %02x %02x %02x %02x\r\n",data_bkp[0],data_bkp[1],data_bkp[2],data_bkp[3],data_bkp[4],data_bkp[5]); 
                            response_send(ptrespconn, true);
                            WEB_DBG("set devkey ....\n");
                            UART_WaitTxFifoEmpty(UART0,50000);
                            os_free(data_bkp);
                            data_bkp=NULL;
                        }
                    }
                }
#endif
                else {
                    response_send(ptrespconn, false);
                }
                 break;
        }

        if (precvbuffer != NULL){
            os_free(precvbuffer);
            precvbuffer = NULL;
        }
        //os_free(pURL_Frame);
        //pURL_Frame = NULL;
        _temp_exit:
            ;
    }
    else if(upgrade_lock == 1){
        local_upgrade_download(ptrespconn,pusrdata, length);
        if(precvbuffer != NULL){
            WEB_DBG("free precvbuffer\r\n");
            os_free(precvbuffer);
            precvbuffer = NULL;
        }
    }
}

/******************************************************************************
 * FunctionName : webserver_recon
 * Description  : the connection has been err, reconnection
 * Parameters   : arg -- Additional argument to pass to the callback function
 * Returns      : none
*******************************************************************************/
LOCAL ICACHE_FLASH_ATTR
void webserver_recon(void *arg, sint8 err)
{
    struct espconn *pesp_conn = arg;
    WEB_DBG("webserver's %d.%d.%d.%d:%d err %d reconnect\n", pesp_conn->proto.tcp->remote_ip[0],
            pesp_conn->proto.tcp->remote_ip[1],pesp_conn->proto.tcp->remote_ip[2],
            pesp_conn->proto.tcp->remote_ip[3],pesp_conn->proto.tcp->remote_port, err);
}

/******************************************************************************
 * FunctionName : webserver_recon
 * Description  : the connection has been err, reconnection
 * Parameters   : arg -- Additional argument to pass to the callback function
 * Returns      : none
*******************************************************************************/
LOCAL ICACHE_FLASH_ATTR
void webserver_discon(void *arg)
{
    struct espconn *pesp_conn = arg;

    WEB_DBG("webserver's %d.%d.%d.%d:%d disconnect\n", pesp_conn->proto.tcp->remote_ip[0],
                pesp_conn->proto.tcp->remote_ip[1],pesp_conn->proto.tcp->remote_ip[2],
                pesp_conn->proto.tcp->remote_ip[3],pesp_conn->proto.tcp->remote_port);
}
/******************************************************************************
 * FunctionName : user_accept_listen
 * Description  : server listened a connection successfully
 * Parameters   : arg -- Additional argument to pass to the callback function
 * Returns      : none
*******************************************************************************/
LOCAL void ICACHE_FLASH_ATTR
webserver_listen(void *arg)
{
    struct espconn *pesp_conn = arg;

    espconn_regist_recvcb(pesp_conn, webserver_recv);
    espconn_regist_reconcb(pesp_conn, webserver_recon);
    espconn_regist_disconcb(pesp_conn, webserver_discon);
}

/******************************************************************************
 * FunctionName : user_webserver_init
 * Description  : parameter initialize as a server
 * Parameters   : port -- server port
 * Returns      : none
*******************************************************************************/
void ICACHE_FLASH_ATTR
user_webserver_init(uint32 port)
{
    esp_conn.type = ESPCONN_TCP;
    esp_conn.state = ESPCONN_NONE;
    esp_conn.proto.tcp = &esptcp;
    esp_conn.proto.tcp->local_port = port;
	WEB_DBG("debug: esp_conn: %p \r\n",&esp_conn);
    espconn_regist_recvcb(&esp_conn, webserver_recv);
    espconn_regist_connectcb(&esp_conn, webserver_listen);

#ifdef SERVER_SSL_ENABLE
    espconn_secure_accept(&esp_conn);
#else
    espconn_accept(&esp_conn);
#endif
}

//get web conn param,not used in MESH mode
void* ICACHE_FLASH_ATTR user_GetWebPConn()
{
    return (&esp_conn);
}

