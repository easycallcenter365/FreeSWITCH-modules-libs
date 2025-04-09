/*
 *  funasr module for freeswitch
 *
 * easycallcenter365@126.com
 *
 * 
 * compile:

 g++  -shared -o mod_funasr.so -fPIC -g -O -ggdb -std=c++11 -Wall   mod_funasr.cpp \
 -I../../../../libs/libhv-master/master/libhv/include/hv/  \
 -I../../../../libs/libteletone/src/  \
 -I../../../../src/include/  \
 -lpthread  -L/usr/local/freeswitchvideo/lib/  -lhv -lfreeswitch

 
usage:
 start asr:
<action application="start_asr" data="hello"/>

pause asr:
<action application="pause_asr" data="1"/>
resume asr:
<action application="pause_asr" data="0"/>

stop asr: 
<action application="stop_asr" />	

 */

#include <condition_variable>
#include "hlog.h"
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>  
#include <string.h>
#include "WebSocketClient.h"
#include <iostream>
#include <vector>
#include <chrono>
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

extern "C" {
	#include <switch.h>
	#include <EventLoopThreadPool.h>
	#include <audio_queue.h>
}

using namespace hv;
using namespace std;
using namespace rapidjson;

#define THREAD_POOL_SIZE 20
#define PCM_8k_FRAME_SIZE 160
#define PCM_16k_FRAME_SIZE 320

SWITCH_MODULE_LOAD_FUNCTION(mod_funasr_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_funasr_shutdown);
extern "C" {
   SWITCH_MODULE_DEFINITION(mod_funasr, mod_funasr_load, mod_funasr_shutdown, NULL);
}

/**
* funasr server config info
**/
struct mod_funasr_globals {
	char *server_url;
	char silent_data[320];
	int ws_conn_timeout_ms;
	int sample_rate;
	char *asr_mode;
	int log_asr_response;
	int parse_json;
	int write_pcm_enable;
	int ws_conn_timeout_warn_ms;
	int ws_conn_retry_max;
	switch_memory_pool_t *pool;
 };
typedef struct mod_funasr_globals mod_funasr_globals_t;

 /** Module global variables */
static mod_funasr_globals_t funasr_globals;

static string ONLINE =  string("2pass-online");
static string OFFLINE = string("2pass-offline");


static switch_bool_t funasr_mod_mediabug_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type);
void stop_funasr(switch_core_session_t *session);
 
 struct switch_da {
	switch_media_bug_t *bug; 	 
	switch_mutex_t *write_mutex; 
 	switch_thread_cond_t *cond;
	switch_frame_t frame; 
	volatile bool hangup;
	volatile bool ws_connected;
	volatile bool ws_closed;
	volatile int paused;	
 	volatile int ws_conn_retry;
	char *uuid;
	/** FreeSWITCH audio buffer */
	audio_queue_t *audio_queue;
	switch_thread_t *wsThread;
	switch_memory_pool_t *pool;
 } ;
typedef struct switch_da switch_da_t;

static void destroy(switch_da_t **pvtArg)
{
	if (!pvtArg || !*pvtArg) return;

	switch_da_t *pvt = *pvtArg;

	if (!pvt->hangup) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "wait for session hangup, uuid=%s\n", pvt->uuid);
		switch_thread_cond_wait(pvt->cond, pvt->write_mutex);
	}
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "session is hangup, uuid=%s\n", pvt->uuid);

	audio_queue_clear(pvt->audio_queue);
	audio_queue_destroy(pvt->audio_queue);
	switch_mutex_destroy(pvt->write_mutex);
	switch_thread_cond_destroy(pvt->cond);

	pvt->bug = NULL;
	pvt->audio_queue = NULL;
	pvt->wsThread = NULL;
	pvt->write_mutex = NULL;
	pvt->cond = NULL; 

	if (pvt->pool) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
						  "switch_core_destroy_memory_pool, pvt->pool memory address=%p, uuid=%s\n",
						  (void *)(pvt->pool), pvt->uuid);
		switch_core_destroy_memory_pool(&pvt->pool);
	}
	pvt->uuid = NULL;
	pvt->pool = NULL;
	pvt = NULL;
}

void fireAsrEvent(switch_da_t *pvt, string &asrText, int vad)
{
	if (pvt && pvt->pool && !pvt->ws_closed) {
		switch_event_t *fsEvent = NULL;

		if (switch_event_create(&fsEvent, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS) {
			fsEvent->subclass_name = strdup("AsrEvent");
			// event plain CUSTOM AsrEvent
			switch_time_t now = switch_time_now() / 1000 ;
			switch_event_add_header_string(fsEvent, SWITCH_STACK_BOTTOM, "Unique-ID", strdup(pvt->uuid));
			switch_event_add_header_string(fsEvent, SWITCH_STACK_BOTTOM, "Detect-Speech-Result",
										   strdup(asrText.c_str()));
			switch_event_add_header_string(fsEvent, SWITCH_STACK_BOTTOM, "ASR-Event-Time-Ms",
										   strdup(to_string(now).c_str()));

			if (vad == 1) {
				switch_event_add_header_string(fsEvent, SWITCH_STACK_BOTTOM, "ASR-Event-Detail", strdup("Vad"));
			} else {
				switch_event_add_header_string(fsEvent, SWITCH_STACK_BOTTOM, "ASR-Event-Detail", strdup("Middle"));
			}
			switch_event_fire(&fsEvent);
		}

		if (fsEvent) { switch_event_destroy(&fsEvent); }
	}
};


static void *send_audio_to_asr_server(switch_thread_t *thread, void *user_data)
{
	pthread_detach(pthread_self());
	switch_da_t *pvt = (switch_da_t *)user_data;
	char *uuid = pvt->uuid;
	if (pvt->hangup) { 
		destroy(&pvt);
		return NULL;
	}

	// for ws_close and ws_open events  we need seperate 'condition_variable' and  mutex vars
	std::mutex mtx;
	std::condition_variable cv;
	std::mutex mtx_for_close;
	std::condition_variable cv_for_close;
	volatile bool connected = false;
	volatile bool closed = false;

	WebSocketClient ws;
	ws.onopen = [&ws, &pvt, &mtx, &cv, &connected]() { 
		std::string asr_mode = funasr_globals.asr_mode;
		std::string pram_begin = "{\"chunk_size\":[5,10,5], \"audio_fs\" : 16000, \"wav_name\":\"";
		std::string pram_end = "\",\"is_speaking\":true, \"wav_format\":\"pcm\", \"mode\":\""+ asr_mode +"\"}";
		std::string uuid = pvt->uuid;
		std::string pramStr = pram_begin + uuid + pram_end;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
						  "FunAsr connection ready, try to send init parameter,  uuid=%s\n", pvt->uuid);
		ws.send(pramStr);		 
		std::unique_lock<std::mutex> lock(mtx);
		connected = true;
		pvt->ws_connected = true;		
		// send signal to main thread
		cv.notify_one(); 
	};
	ws.onclose = [&closed, &uuid, &mtx_for_close, &cv_for_close]() {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "recv FunAsr disconnected msg, uuid=%s\n", uuid);		
		std::unique_lock<std::mutex> lock(mtx_for_close);  
		closed = true;
		// send signal to main thread
		cv_for_close.notify_one();
	};
	ws.onmessage = [&pvt](int opcode, const std::string &msg) {
		if (msg.empty()) { return; } 

		if (funasr_globals.log_asr_response) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "ws onmessage, %s, uuid=%s\n", 
							msg.c_str(), pvt->uuid);
		} 
		  
		if (pvt && pvt->pool) {

			if (!funasr_globals.parse_json) {
				const char *asr_result = strdup(msg.c_str());
				if (asr_result && strlen(asr_result) > 0) {
					switch_event_t *fsEvent = NULL;
					if (switch_event_create(&fsEvent, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS) {
						fsEvent->subclass_name = strdup("AsrEvent");
						switch_event_add_header_string(fsEvent, SWITCH_STACK_BOTTOM, "Unique-ID", strdup(pvt->uuid));
						switch_event_add_header_string(fsEvent, SWITCH_STACK_BOTTOM, "AsrResult", asr_result);
						switch_time_t now = switch_time_now() / 1000;
						switch_event_add_header_string(fsEvent, SWITCH_STACK_BOTTOM, "ASR-Event-Time-Ms",
													   strdup(to_string(now).c_str()));
						switch_event_fire(&fsEvent);
					}
					if (fsEvent) { switch_event_destroy(&fsEvent); } 
				}
				return;
			}

			// need to parse_json
			Document json;
			json.Parse(msg.c_str());
			if (!json.IsObject()) { return; }

		    if (json.HasParseError()) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "parse json error, code = %d,  uuid=%s\n",
								  json.GetParseError(), pvt->uuid);
				return;
			}

			//  {"is_final":false,"mode":"2pass-online","text":"ι","wav_name":"2412301349030110002"}

		    if (json.HasMember("mode") && json.HasMember("text")) {
			 
				const rapidjson::Value &text = json["text"]; 
				const rapidjson::Value &mode = json["mode"]; 

				if (text.IsString() && mode.IsString()) {
					int vad = 0;
					if (mode.GetString() == OFFLINE) {
						vad = 1;
					}  
					string asr_result = string(text.GetString());
					fireAsrEvent(pvt, asr_result, vad);
				}
			} 
		}
	};
	 
	reconn_setting_t reconn;
	reconn.max_retry_cnt = 0; // disable reconnect	
	ws.connect_timeout = funasr_globals.ws_conn_timeout_ms;	 
	chrono::steady_clock::time_point start = chrono::steady_clock::now();
	chrono::steady_clock::time_point end;
	ws.setReconnect(&reconn);		
	char server_url_uuid[256] = {0}; 
	switch_snprintf(server_url_uuid, sizeof(server_url_uuid), "%s/%s", funasr_globals.server_url, uuid);	
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
					  "Try to connect funAsr server=%s, conn_timeout=%d,  uuid=%s\n", server_url_uuid,
					  ws.connect_timeout, pvt->uuid);
	ws.open(server_url_uuid);

	// wait for connection ready
	{
		std::unique_lock<std::mutex> lock(mtx);
		cv.wait_for(lock, std::chrono::milliseconds(ws.connect_timeout), [&connected] { return connected; });
	}

	if (pvt->ws_connected) {
		end = chrono::steady_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() * 0.001f;
		if (duration > funasr_globals.ws_conn_timeout_warn_ms) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "FunAsr connected successfully, BUT SPENT TOO LONG TIME,  time spent=%f ms, uuid=%s\n",
							  duration, pvt->uuid);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
							  "FunAsr connected successfully, time spent = %f ms, uuid=%s\n", duration, pvt->uuid);
		}

		switch_audio_resampler_t *resampler;
		if (funasr_globals.sample_rate == 16000) {
			if (switch_resample_create(&resampler, 8000, 16000, (uint32_t)640, SWITCH_RESAMPLE_QUALITY, 1) !=
				SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Unable to create resampler!\n");
				ws.close();
				destroy(&pvt);
				return NULL;
			}
		}

		int16_t data[PCM_8k_FRAME_SIZE] = {0};
		int16_t data_resample[PCM_16k_FRAME_SIZE] = {0}; 
		 
		FILE *pcm_file = NULL;

		if (funasr_globals.write_pcm_enable) { 			
			char pcm_path[256] = {0};
			sprintf(pcm_path, "/tmp/16k_pcm_%s", uuid);
			pcm_file = fopen(pcm_path, "wb");
			if (!pcm_file) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "FAILED to open file=%s, uuid=%s \n", 
					pcm_path, uuid);
			}
		}

		while (!pvt->hangup && !closed) {

			switch_size_t len = PCM_8k_FRAME_SIZE * 2;
			audio_queue_read(pvt->audio_queue, data, &len, 1);

			if (len == PCM_8k_FRAME_SIZE * 2) {
				/* one frame read */ 

				if (funasr_globals.sample_rate == 16000) {					  
				   
					// convert 8K to 16K
					switch_resample_process(resampler, data, PCM_8k_FRAME_SIZE);
					memcpy(data_resample, resampler->to, resampler->to_len * 2);
					  
					ws.send((char*)data_resample, PCM_16k_FRAME_SIZE * 2  , WS_OPCODE_BINARY);
					if (pcm_file) {
						fwrite(data_resample, sizeof(int16_t), PCM_16k_FRAME_SIZE, pcm_file);
					}
				} 

				if (funasr_globals.sample_rate == 8000) { 
					ws.send((char*)data, len, WS_OPCODE_BINARY);
				} 
			} 
		}
		if (resampler && funasr_globals.sample_rate == 16000) { switch_resample_destroy(&resampler); }
		 
		if (pcm_file) { fclose(pcm_file); }

	}else{ 
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "WEBSOCKET CONNECT-FAILED, uuid=%s\n", pvt->uuid);
	} 
	 
	if (!closed) {
		ws.send("{\"is_speaking\":false}");
		// wait  3 seconds for asr result
		std::this_thread::sleep_for(std::chrono::milliseconds(1000)); 
	}

	ws.close();
	// wait for connection to be closed.
	{
		std::unique_lock<std::mutex> lock(mtx_for_close);
		cv_for_close.wait_for(lock, std::chrono::milliseconds(3000), [&closed] { return closed; });
	}

	// close is async
	if (closed) {
		pvt->ws_closed = true;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Successfully close websocket ws object %s\n", uuid);
	}

 
	pvt->ws_conn_retry += 1;
	if (!pvt->hangup) {
		if (pvt->ws_conn_retry <= funasr_globals.ws_conn_retry_max) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1000));
			if (!pvt->hangup) {
				// add function to reconnect...
				switch_threadattr_t *thd_attr = NULL;
				switch_threadattr_create(&thd_attr, pvt->pool);
				switch_threadattr_detach_set(thd_attr, 1);
				switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Try to create ws-reconnect-Thread uuid=%s\n",
								  pvt->uuid);
				if (switch_thread_create(&pvt->wsThread, thd_attr, send_audio_to_asr_server, (void *)pvt, pvt->pool) !=
					SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
									  "failed to create  ws-reconnect-Thread  for  session %s.", pvt->uuid);
				} else {
				   return NULL;				
				}
			}
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "websocket retry connection reach max limitation %d , abandon. uuid=%s\n",
							  funasr_globals.ws_conn_retry_max, pvt->uuid);
		}
	} 

	destroy(&pvt);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Destroy funasr object for session %s . \n", uuid);
	pthread_exit(0);
	return NULL;
};


static switch_bool_t funasr_mod_mediabug_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_da_t *pvt = (switch_da_t *)user_data;
	switch (type) {
	case SWITCH_ABC_TYPE_INIT: {
		if (!pvt->hangup) {
			switch_threadattr_t *thd_attr = NULL;
			switch_threadattr_create(&thd_attr, pvt->pool);
			switch_threadattr_detach_set(thd_attr, 1);
			switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Try to create FunAsr-connect-Thread uuid=%s\n",
							  pvt->uuid);
			if (switch_thread_create(&pvt->wsThread, thd_attr, send_audio_to_asr_server, (void *)pvt, pvt->pool) !=
				SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
								  "failed to create wsThread thread for session %s.", pvt->uuid);
			}
		}
	} break;
	case SWITCH_ABC_TYPE_CLOSE: {		
		if (pvt && pvt->pool && !pvt->hangup) {
			pvt->hangup = true;

			switch_mutex_lock(pvt->write_mutex);
			switch_thread_cond_signal(pvt->cond);
			switch_mutex_unlock(pvt->write_mutex);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "FunAsr stopped.\n");
		}
	} break;
	case SWITCH_ABC_TYPE_READ: {
		if (pvt->hangup || !pvt->ws_connected || pvt->ws_closed) { return SWITCH_TRUE; }				 
	    switch_mutex_lock(pvt->write_mutex);
		if (switch_core_media_bug_read(bug, &pvt->frame, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) { 
			switch_size_t frame_len = pvt->frame.datalen; 
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "read frame from switch, frame_len=%ld, uuid=%s\n",
							  frame_len, pvt->uuid);
			// send audio data to switch_buffer
			if (frame_len > 0) { 
				if (pvt->paused){ 
					audio_queue_write(pvt->audio_queue, funasr_globals.silent_data, &frame_len); 	
				} else {
					audio_queue_write(pvt->audio_queue, pvt->frame.data, &frame_len); 				
				}				
				audio_queue_signal(pvt->audio_queue);
			}
		}
		switch_mutex_unlock(pvt->write_mutex);

	} break;
	default:
		break;
	}
	return SWITCH_TRUE;
}

SWITCH_STANDARD_APP(stop_funasr_session_function){ 
	stop_funasr(session);
};


SWITCH_STANDARD_APP(pause_funasr_session_function) { 
	char *arg_buf = NULL;
	if (!zstr(data) && (arg_buf = switch_core_session_strdup(session, data))) {
		char *argv[2] = {0};
		int argc = switch_separate_string(arg_buf, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
		if (argc >= 1) {
			switch_da_t *pvt;
			switch_channel_t *channel = switch_core_session_get_channel(session);
			if ((pvt = (switch_da_t *)switch_channel_get_private(channel, "funasr_mod"))) {				
				pvt->paused = atoi(argv[0]);
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "%s funasr paused = %d \n",
								  pvt->uuid, pvt->paused);
			}
		}
	}
};


void stop_funasr(switch_core_session_t *session){
	switch_da_t *pvt;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	if ((pvt = (switch_da_t *)switch_channel_get_private(channel, "funasr_mod"))) {
		switch_channel_set_private(channel, "funasr_mod", NULL);
		switch_core_media_bug_remove(session, &pvt->bug);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s stop asr executed.\n",
						  switch_channel_get_name(channel));
	}
}; 

SWITCH_STANDARD_APP(start_funasr_session_function)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_da_t *pvt = NULL;

	if (!zstr(data)) {

		switch_memory_pool_t *pool;
		if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
			status = SWITCH_STATUS_FALSE;
			goto done;
		}

		pvt = (switch_da_t *)switch_core_alloc(pool, sizeof(switch_da_t));
		memset(pvt, 0, sizeof(*pvt));
		if (!pvt) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to allocate memory for telecom_asr_mod\n");
			status = SWITCH_STATUS_FALSE;
			goto done;
		}

		pvt->pool = pool;
		char *uuid = switch_core_strdup(pool, switch_core_session_get_uuid(session));
		if (switch_mutex_init(&pvt->write_mutex, SWITCH_MUTEX_UNNESTED, pool) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "unable to create pvt->write_mutex, uuid=%s \n",
							  uuid);
			status = SWITCH_STATUS_FALSE;
			goto done;
		}

		if (audio_queue_create(&pvt->audio_queue, uuid, uuid, pvt->pool) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "unable to create pvt->audio_queue, uuid=%s \n",
							  uuid);
			status = SWITCH_STATUS_FALSE;
			goto done;
		}

		pvt->paused = 0;
		pvt->uuid = uuid;
		pvt->hangup = false;
		pvt->ws_connected = false;
		pvt->ws_conn_retry = 0;
		pvt->ws_closed = false;
		pvt->frame.buflen = 320;
		pvt->frame.data = (char *)switch_core_alloc(pool, 320);

		if (switch_thread_cond_create(&pvt->cond, pvt->pool) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "unable to create pvt condition variable");
			status = SWITCH_STATUS_FALSE;
			goto done;
		}		 
	  
		switch_channel_set_private(channel, "funasr_mod", pvt);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s recv start_funasr command. pvt->pool memory address=%p \n", uuid, (void *)pool);

		if (switch_core_media_bug_add(session, "funasr_mod", NULL, funasr_mod_mediabug_callback, pvt, 0,
												SWITCH_ABC_TYPE_READ, &(pvt->bug)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "switch_core_media_bug_add error :%s. \n",
							  pvt->uuid);
			goto done;
		}
	}
done:
	if (status != SWITCH_STATUS_SUCCESS) {
		if (pvt) destroy(&pvt);
	}
}

static switch_status_t funasr_load_config(switch_memory_pool_t *pool)
{
	char *cf = (char *)"funasr.conf";
	switch_xml_t cfg, xml = NULL, param, settings;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	funasr_globals.pool = pool;
	
	funasr_globals.asr_mode = switch_core_strdup(funasr_globals.pool, "online");
	funasr_globals.ws_conn_timeout_ms = 9000;
	funasr_globals.sample_rate = 16000;
	funasr_globals.log_asr_response = 1;
	funasr_globals.parse_json = 1;
	funasr_globals.write_pcm_enable = 0;
	funasr_globals.ws_conn_retry_max = 1;
	memset(funasr_globals.silent_data, 0, sizeof(funasr_globals.silent_data));


	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", cf);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			char *var = (char *)switch_xml_attr_soft(param, "name");
			char *val = (char *)switch_xml_attr_soft(param, "value");
			if (!strcasecmp(var, "server_url")) {
				funasr_globals.server_url = switch_core_strdup(funasr_globals.pool, val);
			} else if (!strcasecmp(var, "asr_mode")) {
				funasr_globals.asr_mode = switch_core_strdup(funasr_globals.pool, val);
			} else if (!strcasecmp(var, "ws_conn_timeout_ms")) {
				funasr_globals.ws_conn_timeout_ms = atoi(val);
			}else if (!strcasecmp(var, "sample_rate")) {
				funasr_globals.sample_rate = atoi(val);
			} else if (!strcasecmp(var, "log_asr_response")) {
				funasr_globals.log_asr_response = atoi(val);
			} else if (!strcasecmp(var, "Parse-Json")) {
				funasr_globals.parse_json = atoi(val);
			} else if (!strcasecmp(var, "write_pcm_enable")) {
				funasr_globals.write_pcm_enable = atoi(val);
			} else if (!strcasecmp(var, "ws_conn_retry_max")) {
				funasr_globals.ws_conn_retry_max = atoi(val);
			}
			
		}
	}

	if (funasr_globals.sample_rate != 8000 && funasr_globals.sample_rate != 16000) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "Unsupported sample_rate = %d, only 16000 or 8000 supported !  \n",
						  funasr_globals.sample_rate);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
		"funasr conf, server_url=%s, ws_conn_timeout_ms=%d, sample_rate=%d, log_asr_response=%d, asr_mode=%s \n",
		funasr_globals.server_url, funasr_globals.ws_conn_timeout_ms, funasr_globals.sample_rate,
	    funasr_globals.log_asr_response,  funasr_globals.asr_mode);

done:
	if (xml) { switch_xml_free(xml); }

	return status;
}

 
SWITCH_MODULE_LOAD_FUNCTION(mod_funasr_load){
	if (SWITCH_STATUS_SUCCESS != funasr_load_config(pool)){ 
		return SWITCH_STATUS_FALSE;
	}
    switch_application_interface_t *app_interface;
    *module_interface = switch_loadable_module_create_module_interface(pool, modname); 
    SWITCH_ADD_APP(app_interface, "start_asr", "funasr_mod", "funasr_mod",start_funasr_session_function, "", SAF_MEDIA_TAP);
	SWITCH_ADD_APP(app_interface, "pause_asr", "funasr_mod", "funasr_mod", pause_funasr_session_function, "", SAF_NONE); 
    SWITCH_ADD_APP(app_interface, "stop_asr", "funasr_mod", "funasr_mod", stop_funasr_session_function, "", SAF_NONE); 
	  
	funasr_globals.ws_conn_timeout_warn_ms = 5000;	

    return SWITCH_STATUS_SUCCESS;
}


 SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_funasr_shutdown){ 
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, " mod_funasr shutdown\n");
    return SWITCH_STATUS_SUCCESS;
}
