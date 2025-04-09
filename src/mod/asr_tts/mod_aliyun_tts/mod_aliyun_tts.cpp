/*
 * tts engine based on alibaba cloud tts
 *
 * easycallcenter365@126.com
 *
 * mod_aliyun_tts.cpp -- Alibaba Cloud TTS module for freeswitch
 * 
 * compile:
 * os:  debian-12.5
 * apt-get install libcurl4-openssl-dev  libssl-dev 

 g++ -shared -o mod_aliyun_tts.so -fPIC -g -O -ggdb -std=c++11 -Wall  \
 -I../../../../libs/libhv/include/hv/  \
 -I../../../../libs/cpputils/ -I../../../../src/include/  \
 -I../../../../libs/libteletone/src/   mod_aliyun_tts.cpp   \
 -L/usr/local/freeswitch/lib/  -lfreeswitch  \
 -L/usr/local/lib/  -lhv    -lcurl  -lpthread   -lssl -lcrypto

 
usage:
<action application="speak" data="<engine>|<voice>|<text>|[timer_name]"/>

Example:

first request:
<action application="speak" data="aliyun_tts|aixia|FreeSWITCH is awesome"/>

resume session based on the last request:
<action application="aliyuntts_resume" data="FreeSWITCH is powerful!"/>

once speak completed, the session will be closed automatically.
you need to use speak to reopen the session again if you want to speak again.
 

 */

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <cstdio>
#include <atomic>
#include "WebSocketClient.h"
#include <EventLoopThreadPool.h>
#include <condition_variable>
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include <iostream>
#include <memory> 
#include "AccessToken.hpp" 


using namespace hv;
using namespace std;
using namespace rapidjson;

#define THREAD_POOL_SIZE 20

SWITCH_MODULE_LOAD_FUNCTION(mod_aliyun_tts_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_aliyun_tts_shutdown);
extern "C" {
  SWITCH_MODULE_DEFINITION(mod_aliyun_tts, mod_aliyun_tts_load, mod_aliyun_tts_shutdown, NULL);
}
 
static EventLoopThreadPool loop_threads_net(THREAD_POOL_SIZE);

static struct {
	char *server_url;
	int ws_conn_timeout_ms;
	int sample_rate;
	char *access_key_id;
	char *access_key_secret;
	char *app_key;
	char *voice_name;
	int voice_volume;
	int write_pcm_enable; 
	int buffer_size;
	int buffer_max_size;
	string token;
	long token_expire_time;
	switch_mutex_t *MUTEX;
	switch_memory_pool_t *pool;
	char silent_data[640];
} globals;

typedef struct aliyuntts {
	char *voice;
	char *task_uuid;
	char *uuid;
	FILE *pcm_file; 
	std::shared_ptr<WebSocketClient> ws;
	std::atomic<int> ws_connected;
	std::atomic<int> ws_closed;
	std::atomic<int> synthesisCompleted;
	std::atomic<int> tts_closed;	
	std::atomic<long> last_request_time_ms;
	std::atomic<int> conn_finished;
	std::atomic<int> read_audio_data;
	std::atomic<int> play_back_stopped;
	std::atomic<int> tts_in_progress;
	std::atomic<int> audio_data_available;
	std::atomic<int> play_back_started;

    std::mutex *mtx;
	// for ws_close and ws_open events we need seperate 'condition_variable' and  mutex vars
	std::condition_variable *cv;
	std::mutex *mtx_for_close;
	std::condition_variable *cv_for_close;
	 
	switch_memory_pool_t *pool;
	switch_buffer_t *audio_buffer;
	switch_core_session_t *session;

	// tts-text queue
	switch_queue_t *tts_text_queue;

} aliyuntts_t;

static std::string prepare_tts_request(const char *text, aliyuntts_t *pvt);

/// <summary>
/// Get token
/// </summary>
/// <param name="accessKeyId"></param>
/// <param name="accessKeySecret"></param>
/// <returns>Returns token string if successful, otherwise returns empty string</returns>
string getToken(std::string &accessKeyId, std::string &accessKeySecret, aliyuntts_t *pvt)
{ 
	long now = switch_time_now() / 1000 /1000;
	if (globals.token_expire_time - now > 3600 ) { return  globals.token; }
	 
	switch_mutex_lock(globals.MUTEX);

	if (globals.token_expire_time - now < 3600 ) { 
		if (globals.token_expire_time > 0L) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "Try to create token. Token is going to be expired, now:%ld, expire:%ld, pvt->uuid=%s.\n",
							  now, globals.token_expire_time, pvt->uuid);
		} 
		AccessToken accessToken(accessKeyId, accessKeySecret);
		try { 
			accessToken.apply(); 
			long expireTime = accessToken.getExpireTime(); 
			if (expireTime > 0) {
				globals.token = accessToken.getToken(); 
				globals.token_expire_time = expireTime;				
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
								  " getToken successfully, token=%s, expireTime=%ld, pvt->uuid=%s.\n",
								  globals.token.c_str(), expireTime, pvt->uuid);
			}
		} catch (const std::exception &e) { 
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, " getToken error, msg = %s, pvt->uuid=%s. \n",
							  e.what(), pvt->uuid);
		}
	}

	switch_mutex_unlock(globals.MUTEX);
	 
	return globals.token;
}


string genUUID()
{
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
	switch_uuid_str(uuid_str, sizeof(uuid_str)); 

	string uuid_str_tmp(uuid_str);
	uuid_str_tmp.erase(std::remove(uuid_str_tmp.begin(), uuid_str_tmp.end(), '-'), uuid_str_tmp.end());
	return uuid_str_tmp;
}

void customDeleter(WebSocketClient *wsClient)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Custom deleter called for WebSocketClient.\n");
	delete wsClient;
};

/// <summary>
/// connect to cosv tts server
/// </summary>
/// <returns>if connected return 1, otherwise return 0</returns>
static int connect_ws(aliyuntts_t *pvt)
{  
	string key = string(globals.access_key_id);
	string secret = string(globals.access_key_secret);
	string token = getToken(key, secret, pvt);
	if (token.length() == 0) { 
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "tts exception, getToken got error!\n");
		return 0;
	}

	// Add the token as a query parameter in the URL as per Alibaba Cloud TTS API requirements
	string url = string(globals.server_url);
	if (url.find("?") == string::npos) {
		url += "?token=" + token;
	} else {
		url += "&token=" + token;
	}

	string task_uuid_str = genUUID();
	pvt->task_uuid = switch_core_strdup(pvt->pool, task_uuid_str.c_str()); 

	if (globals.write_pcm_enable) {
		char pcm_path[100] = {0};
		sprintf(pcm_path, "/tmp/aliyun_tts_pcm_file_%s", task_uuid_str.c_str());
		pvt->pcm_file = fopen(pcm_path, "wb");
		if (!pvt->pcm_file) { 
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "FAILED to open file=%s \n", pcm_path);
		} else { 
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "open file=%s successfully \n", pcm_path);
		}
	}
 
	std::shared_ptr<WebSocketClient> ws_client(
		new WebSocketClient(loop_threads_net.nextLoop()), customDeleter
	);
	pvt->ws = ws_client;

	// Using token in URL instead of headers as per Alibaba Cloud TTS API requirements
	// Format: wss://nls-gateway-cn-beijing.aliyuncs.com/ws/v1?token=<your token>
	
	pvt->ws->onopen = [pvt]() { 
		pvt->ws_connected.store(1);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
			"TTS Server is ready, pvt->uuid=%s \n", pvt->uuid);		

		if (!pvt->conn_finished.load()) {
			std::unique_lock<std::mutex> lock(*pvt->mtx);
			pvt->conn_finished.store(1);
			pvt->cv->notify_one();
		}
	};

	pvt->ws->onclose = [pvt]() {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Received TTS server disconnect message, pvt->uuid=%s \n",
						  pvt->uuid);
		// If TTS task is in progress, mark as completed to avoid blocking queue
		if (pvt->tts_in_progress.load() && !pvt->synthesisCompleted.load()) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, 
				"TTS connection closed while task is in progress, forcing mark as completed, pvt->uuid=%s \n",
				pvt->uuid);
			pvt->synthesisCompleted.store(1);
			pvt->tts_in_progress.store(0);
		}
		
		if (!pvt->conn_finished.load()) {
			std::unique_lock<std::mutex> lock(*pvt->mtx);
			pvt->conn_finished.store(1);
			pvt->cv->notify_one();
		}

		std::unique_lock<std::mutex> lock(*pvt->mtx_for_close); 
		pvt->ws_closed.store(1); 		
		// send signal to main thread
		pvt->cv_for_close->notify_one();
	};

	pvt->ws->onmessage = [pvt](int opcode, const std::string &msg) {
		if (opcode == 1) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
				"Received TTS Server text data: %s, pvt->uuid=%s \n", msg.c_str(), pvt->uuid);

			Document document;
			document.Parse(msg.c_str());

			if (!document.IsObject()) { return; }
			if (document.HasParseError()) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "parse json error, code = %d, pvt->uuid=%s \n",
								  document.GetParseError(), pvt->uuid);
				return;
			}

			// Check if "header" field exists
			if (document.HasMember("header") && document["header"].IsObject()) {
				const rapidjson::Value &header = document["header"];

				// Check if "status" field exists and if it is, check if it is a successful response
				if (header.HasMember("status") && header["status"].IsInt()) {
					int status = header["status"].GetInt(); 	
					
					// Check if it is a successful response
					if (status == 20000000) {
					    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
							"TTS Server response status code 20000000, pvt->uuid=%s.\n",
							pvt->uuid);
					} else {
						// Handle error status
						std::string status_text = "";
						if (header.HasMember("status_text") && header["status_text"].IsString()) {
							status_text = header["status_text"].GetString();
						}
						
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, 
							"TTS Server returned error, status code=%d, error message=%s, pvt->uuid=%s\n",
							status, status_text.c_str(), pvt->uuid);
						
						// Special handling for task status error (started task received start command again)
						if (status == 40010005) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, 
								"Detected task status error(40010005), marking current task as completed to handle next request, pvt->uuid=%s\n",
								pvt->uuid);
							
							// Reset synthesis status, allow processing other requests in queue
							pvt->synthesisCompleted.store(1);
							pvt->tts_in_progress.store(0);
						}
					}  
				}

				if (header.HasMember("name") && header["name"].IsString()) {
					std::string name = header["name"].GetString();
					if (name == "SynthesisCompleted") {
						// Record synthesis completion time
						long completion_time = switch_time_now() / 1000;
						long request_time = pvt->last_request_time_ms.load();
						long time_taken = 0;
						
						// Ensure time stamp calculation is reasonable
						if (request_time > 0 && completion_time > request_time) {
							time_taken = completion_time - request_time;
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
								"Received SynthesisCompleted event, time spent %ld ms.，pvt->uuid=%s\n",
								time_taken, pvt->uuid);
						} else {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, 
								"Received synthesis completion notification, time calculation exception(completion time=%ld, request time=%ld)，pvt->uuid=%s\n",
								completion_time, request_time, pvt->uuid);
						}
						
						// Get current buffer size
						size_t buffer_size = switch_buffer_inuse(pvt->audio_buffer);
						
						// Delay marking completed, give more time to receive data
						std::thread([pvt, buffer_size]() {
							// Wait for enough time for all audio data to arrive
							std::this_thread::sleep_for(std::chrono::milliseconds(500));
							
							// Recheck buffer size, see if new data has arrived
							size_t new_buffer_size = switch_buffer_inuse(pvt->audio_buffer);
							bool more_data_arrived = new_buffer_size > buffer_size;
							
							// Update status
							std::unique_lock<std::mutex> lock(*pvt->mtx);
							pvt->synthesisCompleted.store(1);
							
							// Only mark processing completed when enough data is received
							if (new_buffer_size > 1000 || more_data_arrived) {
								pvt->tts_in_progress.store(0);
								pvt->cv->notify_one();
								switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
									"TTS SynthesisCompleted, total recv  %zu bytes.，pvt->uuid=%s\n", 
									new_buffer_size, pvt->uuid);
							} else if (new_buffer_size > 0) {
								// Small amount of data, continue waiting for more data
								pvt->tts_in_progress.store(0);
								pvt->cv->notify_one();
								switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, 
									"TTS synthesis completed but data is very small(%zu bytes)，pvt->uuid=%s\n", 
									new_buffer_size, pvt->uuid);
							} else {
								// No data, mark error status
								switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, 
									"TTS synthesis completed but no data received, pvt->uuid=%s\n", pvt->uuid);
							}
						}).detach();
					}
				}
			}   

		} else if (opcode == 2) {
			// Record received binary data time
			long recv_time = switch_time_now() / 1000;
			long request_time = pvt->last_request_time_ms.load();
			long time_since_request = 0;
			
			// Ensure time stamp calculation is reasonable
			if (request_time > 0 && recv_time > request_time) {
				time_since_request = recv_time - request_time;
			}
			
			// Is this the first audio data packet?
			bool is_first_data = !pvt->audio_data_available.load();
			
			if (is_first_data) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
					"Received first TTS audio data, size=%ld bytes, delay=%ld ms, pvt->uuid=%s \n", 
					msg.size(), time_since_request, pvt->uuid);
				// here, we skip the first audio data, it is 76 bytes, not audio data.
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, 
					"Received tts audio data, size=%ld bytes, pvt->uuid=%s \n", 
					msg.size(), pvt->uuid);

				// Write audio data to buffer
			    switch_buffer_write(pvt->audio_buffer, msg.c_str(), msg.size());	
			}

			// Mark audio data available
			pvt->audio_data_available.store(1);
			 
			if (!pvt->read_audio_data.load()) {
				std::unique_lock<std::mutex> lock(*pvt->mtx);
				pvt->read_audio_data.store(1);
				pvt->cv->notify_one(); 
			}

		    if (pvt->play_back_stopped.load()) {
				pvt->play_back_stopped.store(0);
				switch_event_t *event = NULL;
				switch_channel_t *channel = switch_core_session_get_channel(pvt->session);
				if (switch_event_create(&event, SWITCH_EVENT_PLAYBACK_START) == SWITCH_STATUS_SUCCESS) {
					switch_channel_event_set_data(channel, event);
					switch_event_fire(&event);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
									  "Fire PLAYBACK_START event, pvt->uuid=%s \n", pvt->uuid);
				}
				if (event) { switch_event_destroy(&event); }
			}

			if (pvt->pcm_file) { fwrite(msg.c_str(), sizeof(char), msg.size(), pvt->pcm_file); }
		}
	};

	reconn_setting_t reconn;
	reconn.max_retry_cnt = 0; // disable reconnect
	pvt->ws->connect_timeout = globals.ws_conn_timeout_ms;
	chrono::steady_clock::time_point start = chrono::steady_clock::now();
	chrono::steady_clock::time_point end;
	pvt->ws->setReconnect(&reconn);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
		"Try to connect tts server=%s , task_uuid=%s, pvt->uuid=%s \n",
		url.c_str(), task_uuid_str.c_str(), pvt->uuid);
	pvt->ws->open(url.c_str());

	// wait for connection finished
	{
		std::unique_lock<std::mutex> lock(*pvt->mtx);
		pvt->cv->wait_for(lock, std::chrono::milliseconds(pvt->ws->connect_timeout),
						 [pvt] { return pvt->conn_finished.load(); });
	}

	end = chrono::steady_clock::now();
	auto duration = chrono::duration_cast<chrono::microseconds>(end - start).count();
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "tts-server connect cost time = %Lf microseconds,pvt->uuid=%s \n",
					  duration < 0.0L ? 0.0L : duration, pvt->uuid);

	if (!pvt->ws_connected.load())  {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "TTS Server connected failed. pvt->uuid=%s\n",
						  pvt->uuid);
	}

	return pvt->ws_connected.load();
}

static bool send_tts_request(aliyuntts_t *pvt, const char *text)
{
	if (!pvt || !text) {
		return false;
	}

	// Check WebSocket connection status
	if (!pvt->ws_connected.load()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"WebSocket not connected, cannot send TTS request, pvt->uuid=%s \n", pvt->uuid);
		return false;
	}

	// Check if there is an ongoing TTS request
	if (pvt->tts_in_progress.load()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"An ongoing TTS request is in progress, cannot send new request, pvt->uuid=%s \n", pvt->uuid);
		return false;
	}

	// Reset status
	pvt->synthesisCompleted.store(0);
	pvt->audio_data_available.store(0);
	pvt->tts_in_progress.store(1);
	pvt->last_request_time_ms.store(switch_time_now() / 1000);

	// Prepare TTS request data
	std::string request_data = prepare_tts_request(text, pvt);
	if (request_data.empty()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Failed to prepare TTS request data, pvt->uuid=%s \n", pvt->uuid);
		pvt->tts_in_progress.store(0);
		return false;
	}

	// Send TTS request
	try {
		pvt->ws->send(request_data);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
			"TTS request sent, text length=%zu, pvt->uuid=%s \n", 
			strlen(text), pvt->uuid);
		return true;
	} catch (const std::exception& e) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"TTS request sending failed: %s, pvt->uuid=%s \n", e.what(), pvt->uuid);
		pvt->tts_in_progress.store(0);
		return false;
	}
}

static void destroy(aliyuntts_t **ppvt)
{
	aliyuntts_t *pvt = *ppvt;
	if (!pvt) {
		return;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"Starting to clean up TTS resources, pvt->uuid=%s \n", pvt->uuid);

	// Delete mutex and condition variables
	if (pvt->mtx) {
		delete pvt->mtx;
		pvt->mtx = nullptr;
	}
	if (pvt->cv) {
		delete pvt->cv;
		pvt->cv = nullptr;
	}
	if (pvt->mtx_for_close) {
		delete pvt->mtx_for_close;
		pvt->mtx_for_close = nullptr;
	}
	if (pvt->cv_for_close) {
		delete pvt->cv_for_close;
		pvt->cv_for_close = nullptr;
	}

	// Close WebSocket connection
	if (pvt->ws) {
		pvt->ws = nullptr;  // This will trigger WebSocket destructor
	}

	// Destroy audio buffer
	if (pvt->audio_buffer) {
		switch_buffer_destroy(&pvt->audio_buffer);
		pvt->audio_buffer = nullptr;
	}

	// Close PCM file
	if (pvt->pcm_file) {
		fclose(pvt->pcm_file);
		pvt->pcm_file = nullptr;
	}

	// Destroy TTS text queue
	if (pvt->tts_text_queue) {
		switch_queue_term(pvt->tts_text_queue);
		pvt->tts_text_queue = nullptr;
	}

	// Destroy memory pool
	if (pvt->pool) {
		switch_core_destroy_memory_pool(&pvt->pool);
		pvt->pool = nullptr;
	}

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"TTS resources cleaned up, pvt->uuid=%s \n", pvt->uuid);

	// Clear UUID
	pvt->uuid = nullptr;

	*ppvt = nullptr;
}

static switch_status_t aliyuntts_speech_open(switch_speech_handle_t *sh, const char *voice_name, int rate, int channels,
										 switch_speech_flag_t *flags)
{
	// Create memory pool
	switch_memory_pool_t *pool;
	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Failed to create memory pool \n");
		return SWITCH_STATUS_FALSE;
	}

	// Allocate TTS structure
	aliyuntts_t *pvt = (aliyuntts_t *)switch_core_alloc(pool, sizeof(aliyuntts_t));
	if (!pvt) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Failed to allocate TTS structure \n");
		switch_core_destroy_memory_pool(&pool);
		return SWITCH_STATUS_FALSE;
	}

	// Set sample rate
	sh->native_rate = globals.sample_rate;

	// Set voice name
	if (voice_name) {
		pvt->voice = switch_core_strdup(pool, voice_name);
	} else {
		pvt->voice = switch_core_strdup(pool, globals.voice_name);
	}

	// Set session information
	pvt->session = sh->session;
	pvt->uuid = switch_core_strdup(pool, switch_core_session_get_uuid(sh->session));
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"Starting to initialize TTS, pvt->uuid=%s \n", pvt->uuid);

	// Initialize atomic variables
	new(&pvt->last_request_time_ms) std::atomic<long>(0);
	new(&pvt->ws_connected) std::atomic<int>(0);
	new(&pvt->ws_closed) std::atomic<int>(0);
	new(&pvt->tts_closed) std::atomic<int>(0);
	new(&pvt->synthesisCompleted) std::atomic<int>(1);
	new(&pvt->conn_finished) std::atomic<int>(0);
	new(&pvt->read_audio_data) std::atomic<int>(0);
	new(&pvt->play_back_stopped) std::atomic<int>(1);
	new(&pvt->tts_in_progress) std::atomic<int>(0);
	new(&pvt->audio_data_available) std::atomic<int>(0);
	new(&pvt->play_back_started) std::atomic<int>(0);

	// Create mutex and condition variables
	pvt->mtx = new std::mutex();
	pvt->cv = new std::condition_variable();
	pvt->mtx_for_close = new std::mutex();
	pvt->cv_for_close = new std::condition_variable();

	// Create audio buffer
	if (switch_buffer_create_dynamic(&pvt->audio_buffer, globals.buffer_size, globals.buffer_size,
								 globals.buffer_max_size) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Failed to create audio buffer, pvt->uuid=%s \n", pvt->uuid);
		destroy(&pvt);
		return SWITCH_STATUS_FALSE;
	}

	// Create TTS text queue
	pvt->pool = pool;
	if (switch_queue_create(&pvt->tts_text_queue, 100, pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Failed to create TTS text queue, pvt->uuid=%s \n", pvt->uuid);
		destroy(&pvt);
		return SWITCH_STATUS_FALSE;
	}

	// Set channel private data
	switch_channel_t *channel = switch_core_session_get_channel(sh->session);
	switch_channel_set_private(channel, "aliyuntts_mod", pvt);

	// Connect WebSocket
	if (!connect_ws(pvt)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Failed to connect WebSocket, pvt->uuid=%s \n", pvt->uuid);
		destroy(&pvt);
		return SWITCH_STATUS_FALSE;
	}

	// Start TTS processing thread
	std::thread([pvt] {
		// Record thread start time
		long thread_start_time = switch_time_now() / 1000;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
			"TTS processing thread started, timestamp=%ld, pvt->uuid=%s \n",
			thread_start_time, pvt->uuid);

		// Mark for preventing repeated sending
		bool pending_request = false;
		// Save last text unable to process, avoid losing
		char* pending_text = nullptr;

		while (!pvt->tts_closed.load()) {
			// Check if TTS is completed
			if (pvt->synthesisCompleted.load()) {
				// Reset TTS processing status
				pvt->tts_in_progress.store(0);
				pending_request = false;

				// Check if need to exit loop
				if (pvt->tts_closed.load()) {
					break;
				}

				// First try to process last request unable to send (if any)
				if (pending_text != nullptr) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
						"Reattempt to process last TTS request unable to send: %s, pvt->uuid=%s \n",
						pending_text, pvt->uuid);

					// Try to send TTS request
					if (send_tts_request(pvt, pending_text)) {
						pending_text = nullptr;
						pending_request = true;
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							"Reattempt to send TTS request failed, continue waiting, pvt->uuid=%s \n", pvt->uuid);
					}
				}
				// If there is no request to process or request sending failed, try to get from queue
				else {
					// Get next TTS text from queue
					void *pop;
					if (switch_queue_trypop(pvt->tts_text_queue, &pop) == SWITCH_STATUS_SUCCESS && pop &&
						!pvt->tts_closed.load()) {
						char* tts_text = (char *)pop;
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
							"Get TTS text from queue, prepare synthesis: %s, pvt->uuid=%s \n",
							tts_text, pvt->uuid);

						// Try to send TTS request
						if (send_tts_request(pvt, tts_text)) {
							pending_request = true;
						} else {
							// If request not successfully sent (possibly because previous request is still in progress)
							// Save text for later retry
							pending_text = tts_text;
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								"TTS request not sent, save text for later processing: %s, pvt->uuid=%s \n",
								pending_text, pvt->uuid);
						}
					}
				}

				// If just sent TTS request, give extra time for data to return
				// Avoid closing connection immediately after TTS request is sent
				if (pvt->tts_closed.load() && pvt->tts_in_progress.load()) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						"TTS connection marked as closed, wait for a while to receive data, pvt->uuid=%s \n", pvt->uuid);
					// Wait briefly, see if data can be received
					for (int i = 0; i < 10 && !pvt->audio_data_available.load(); i++) {
						std::this_thread::sleep_for(std::chrono::milliseconds(100));
						if (pvt->audio_data_available.load()) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
								"Wait %d milliseconds to receive audio data, pvt->uuid=%s \n",
								i * 100, pvt->uuid);
							break;
						}
					}
					break;
				}
			} else {
				// TTS synthesis is still in progress, wait for a while
				std::this_thread::sleep_for(std::chrono::milliseconds(50));

				// Check if TTS request is timed out
				if (pvt->tts_in_progress.load() && !pvt->audio_data_available.load() && pending_request) {
					long now = switch_time_now() / 1000;
					long request_time = pvt->last_request_time_ms.load();

					// Ensure request_time is valid
					if (request_time > 0 && now > request_time) {
						long time_passed = now - request_time;

						// If more than 3 seconds have passed without receiving data, it might be a request problem
						if (time_passed > 3000) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								"TTS request timed out, waited %ld milliseconds still no data received, timestamp(%ld -> %ld), pvt->uuid=%s \n",
								time_passed, request_time, now, pvt->uuid);
							// Reset status, allow processing next request
							pvt->synthesisCompleted.store(1);
							pvt->tts_in_progress.store(0);
							pending_request = false;
						}
					} else if (request_time > 0) {
						// Time stamp exception
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							"TTS time stamp exception: current=%ld, request=%ld, pvt->uuid=%s \n",
							now, request_time, pvt->uuid);
					}
				}
			}
		}

		// Check if stopped in processing
		if (pvt->tts_in_progress.load()) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"TTS processing thread about to stop, but synthesis task still in progress, pvt->uuid=%s \n", pvt->uuid);
		}

		// Check if there is any request unable to send
		if (pending_text != nullptr) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"TTS processing thread about to stop, but there is an unable to send request: %s, pvt->uuid=%s \n",
				pending_text, pvt->uuid);
		}

		// Check if there is any request unable to process in queue
		int queue_size = switch_queue_size(pvt->tts_text_queue);
		if (queue_size > 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"TTS processing thread about to stop, queue has %d unprocessed requests, pvt->uuid=%s \n",
				queue_size, pvt->uuid);
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
			"TTS processing thread ended, pvt->uuid=%s \n", pvt->uuid);

		{
			std::unique_lock<std::mutex> lock(*pvt->mtx_for_close);
			pvt->cv_for_close->wait_for(lock, std::chrono::milliseconds(3500),
				[pvt] { return pvt->ws_closed.load(); });
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
			"WebSocket connection closed, pvt->uuid=%s \n", pvt->uuid);

		aliyuntts_t *pvtArg = (aliyuntts_t *)pvt;
		destroy(&pvtArg);
	}).detach();

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t aliyuntts_speech_close(switch_speech_handle_t *sh, switch_speech_flag_t *flags)
{
	aliyuntts_t *pvt;
	switch_channel_t *channel = switch_core_session_get_channel(sh->session);
	pvt = (aliyuntts_t *)switch_channel_get_private(channel, "aliyuntts_mod");

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "try to shutdown TTS connection...pvt->uuid=%s. \n", pvt->uuid);

	// Check if there is an ongoing TTS request
	if (pvt->tts_in_progress.load()) {
		long wait_start = switch_time_now() / 1000;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
			"Detected TTS request in progress, wait for completion or timeout, start time=%ld, pvt->uuid=%s \n", 
			wait_start, pvt->uuid);
		 
		bool data_received = false;
		bool tts_completed = false;
		
		// Loop wait until data is received, TTS completed or timeout
		for (int i = 0; i < 50; i++) {  // 5 seconds = 50 times * 100 milliseconds
			// Check if data is received
			if (pvt->audio_data_available.load()) {
				data_received = true;
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
					"TTS request received data, continue waiting for completion, pvt->uuid=%s \n", pvt->uuid);
			}
			
			// Check if TTS is completed
			if (pvt->synthesisCompleted.load()) {
				tts_completed = true;
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
					"TTS marked as completed, pvt->uuid=%s \n", pvt->uuid);
				// If data is already received, no longer wait
				if (data_received) break;
			}
			
			// If already closed, exit wait
			if (pvt->ws_closed.load()) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, 
					"WebSocket closed, stop waiting, pvt->uuid=%s \n", pvt->uuid);
				break;
			}
			
			// Wait 100ms
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		
		long wait_end = switch_time_now() / 1000;
		long wait_duration = wait_end - wait_start;
		
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
			"TTS request wait ended: wait duration=%ld milliseconds, data received=%s, TTS completed=%s, pvt->uuid=%s \n", 
			wait_duration, data_received ? "Yes" : "No", tts_completed ? "Yes" : "No", pvt->uuid);
	}
	
	// Close WebSocket connection
	if (pvt->ws && !pvt->ws_closed.load()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
			"shutdown WebSocket connection, pvt->uuid=%s \n", pvt->uuid);
		pvt->ws->close(); 
	}
	
	// Mark TTS closed
	pvt->tts_closed.store(1);

	// Send custom event
	switch_event_t *event = NULL;
	if (switch_event_create(&event, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS) {
		event->subclass_name = strdup("TtsEvent");
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Tts-Event-Detail", strdup("Speech-Closed"));
		switch_event_fire(&event);
	}
	if (event) { switch_event_destroy(&event); } 

	return SWITCH_STATUS_SUCCESS;
}
 

static switch_status_t aliyuntts_speech_feed_tts(switch_speech_handle_t *sh, char *text, switch_speech_flag_t *flags)
{
	switch_channel_t *channel = switch_core_session_get_channel(sh->session);
	aliyuntts_t *pvt = (aliyuntts_t *)switch_channel_get_private(channel, "aliyuntts_mod");
	if (!pvt) {
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
		"Starting to process TTS request, text: %s, pvt->uuid=%s \n", text, pvt->uuid);

	// Check TTS status
	if (pvt->tts_closed.load()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"TTS closed, cannot process request, pvt->uuid=%s \n", pvt->uuid);
		return SWITCH_STATUS_FALSE;
	}

	// Check WebSocket connection status
	if (!pvt->ws_connected.load()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"WebSocket not connected, cannot process request, pvt->uuid=%s \n", pvt->uuid);
		return SWITCH_STATUS_FALSE;
	}

	// Check if there is an ongoing TTS request
	if (pvt->tts_in_progress.load()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
			"Current ongoing TTS request, add new request to queue, pvt->uuid=%s \n", pvt->uuid);
		// Add request to queue
		if (switch_queue_push(pvt->tts_text_queue, text) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				"Failed to add TTS request to queue, pvt->uuid=%s \n", pvt->uuid);
			return SWITCH_STATUS_FALSE;
		}
		return SWITCH_STATUS_SUCCESS;
	}

	// Try to send TTS request directly
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"Try to send TTS request directly, pvt->uuid=%s \n", pvt->uuid);
	
	if (!send_tts_request(pvt, text)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"Direct TTS request sending failed, add request to queue, pvt->uuid=%s \n", pvt->uuid);
		// Send failed, add request to queue
		if (switch_queue_push(pvt->tts_text_queue, text) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				"Failed to add TTS request to queue, pvt->uuid=%s \n", pvt->uuid);
			return SWITCH_STATUS_FALSE;
		}
		return SWITCH_STATUS_SUCCESS;
	}

	// Wait for a while, see if data can be received
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"Wait to receive TTS data, pvt->uuid=%s \n", pvt->uuid);
	
	for (int i = 0; i < 20 && !pvt->audio_data_available.load(); i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (pvt->audio_data_available.load()) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
				"Wait %d milliseconds to receive audio data, pvt->uuid=%s \n", 
				i * 100, pvt->uuid);
			break;
		}
	}

	return SWITCH_STATUS_SUCCESS;
}

static void aliyuntts_speech_flush_tts(switch_speech_handle_t *sh)
{
	aliyuntts_t *pvt;
	switch_channel_t *channel = switch_core_session_get_channel(sh->session);
	pvt = (aliyuntts_t *)switch_channel_get_private(channel, "aliyuntts_mod");
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "on aliyuntts_speech_flush_tts event...pvt->uuid=%s \n",
					  pvt->uuid);
	 
	if (pvt->audio_buffer) { switch_buffer_zero(pvt->audio_buffer); }

}

static switch_status_t aliyuntts_speech_read_tts(switch_speech_handle_t *sh, void *data, size_t *datalen,
											 switch_speech_flag_t *flags)
{
	size_t bytes_read = 0;
	aliyuntts_t *pvt;
	switch_channel_t *channel = switch_core_session_get_channel(sh->session);
	pvt = (aliyuntts_t *)switch_channel_get_private(channel, "aliyuntts_mod");
  
	// First call time record, used for total time calculation
	static long first_read_time = 0;
	if (first_read_time == 0) {
		first_read_time = switch_time_now() / 1000;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
			"First TTS data read request, timestamp=%ld, pvt->uuid=%s\n", 
			first_read_time, pvt->uuid);
	}
	
	// First try to read available data
	if ((bytes_read = switch_buffer_read(pvt->audio_buffer, data, *datalen))) { 
		*datalen = bytes_read;
		return SWITCH_STATUS_SUCCESS;
	}

	// Check if there is an ongoing TTS request
	bool tts_active = pvt->tts_in_progress.load() && 
					   pvt->ws_connected.load() && 
					   !pvt->ws_closed.load() && 
					   !pvt->tts_closed.load();
	
	// If there is an active TTS request but no data, wait for data to arrive
	if (tts_active && !pvt->audio_data_available.load()) {
		long now = switch_time_now() / 1000;
		long request_time = pvt->last_request_time_ms.load();
		
		// Ensure time stamp is valid
		if (request_time > 0 && now > request_time) {
			long time_since_request = now - request_time;
			
			// If request just sent (within 5 seconds), give enough wait time
			if (time_since_request < 5000) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
					"TTS request sent %ld milliseconds ago, waiting for audio data, pvt->uuid=%s\n", 
					time_since_request, pvt->uuid);
				
				// Wait up to 5 seconds, check if data has arrived
				bool got_data = false;
				for (int i = 0; i < 50 && !pvt->audio_data_available.load(); i++) {
					// Check every 100 milliseconds
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					
					// If data received, connection closed or TTS closed, exit wait
					if (!pvt->ws_connected.load() || pvt->ws_closed.load() || pvt->tts_closed.load()) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, 
							"Wait data time WebSocket connection status changed, exit wait, pvt->uuid=%s\n", pvt->uuid);
						break;
					}
					
					// If data received, exit wait
					if (pvt->audio_data_available.load()) {
						got_data = true;
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
							"Wait %d milliseconds to receive audio data, pvt->uuid=%s\n", 
							(i+1)*100, pvt->uuid);
						break;
					}
				}
				
				// If data received, try to read again
				if (got_data && (bytes_read = switch_buffer_read(pvt->audio_buffer, data, *datalen))) {
					*datalen = bytes_read;
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
						"Successfully read %zu bytes audio data, pvt->uuid=%s\n", bytes_read, pvt->uuid);
					return SWITCH_STATUS_SUCCESS;
				}
			}
		}
	}

	// No data, and need to block read, wait for data to arrive
	if (*flags == SWITCH_SPEECH_FLAG_BLOCKING && 
		(pvt->tts_in_progress.load() || pvt->audio_data_available.load())) {
		
		// Determine maximum wait time
		const int max_wait_ms = 2000;
		const int wait_step_ms = 20;  // Shorten each wait time, improve responsiveness
		int waited_ms = 0;
		
		while (waited_ms < max_wait_ms) {
			// Use condition variable wait for new data
			{
				std::unique_lock<std::mutex> lock(*pvt->mtx);
				pvt->cv->wait_for(lock, std::chrono::milliseconds(wait_step_ms), 
					[pvt] { return pvt->read_audio_data.load() || !pvt->tts_in_progress.load(); });
				
				// Reset flag, prepare for next notification
				if (pvt->read_audio_data.load()) {
					pvt->read_audio_data.store(0);
				}
			}
			
			// Try to read data
			if ((bytes_read = switch_buffer_read(pvt->audio_buffer, data, *datalen))) {
				*datalen = bytes_read;
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, 
					"Wait %d milliseconds to read %zu bytes data, pvt->uuid=%s\n", 
					waited_ms, bytes_read, pvt->uuid);
				return SWITCH_STATUS_SUCCESS;
			}
			
			waited_ms += wait_step_ms;
			
			// If connection is closed and no more data, stop waiting
			if (pvt->ws_closed.load() || pvt->tts_closed.load()) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, 
					"TTS connection closed, stop waiting for data, pvt->uuid=%s\n", pvt->uuid);
				break;
			}
		}
		
		// Try one last read
		if ((bytes_read = switch_buffer_read(pvt->audio_buffer, data, *datalen))) {
			*datalen = bytes_read;
			return SWITCH_STATUS_SUCCESS;
		}
	}
	
	// If no data read, and PLAYBACK_START event has been sent, send PLAYBACK_STOP event
	if (!bytes_read && !pvt->play_back_stopped.load()) {
		pvt->play_back_stopped.store(1);
		pvt->audio_data_available.store(0);
		
		switch_event_t *event = NULL;
		if (switch_event_create(&event, SWITCH_EVENT_PLAYBACK_STOP) == SWITCH_STATUS_SUCCESS) {
			switch_channel_event_set_data(channel, event);
			switch_event_fire(&event);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, 
				"Fire PLAYBACK_STOP event, pvt->uuid=%s \n", pvt->uuid);
		}
		if (event) { switch_event_destroy(&event); }
	}
	
	// If data read, return success; otherwise return failure
	return (bytes_read > 0) ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

static void aliyuntts_text_param_tts(switch_speech_handle_t *sh, char *param, const char *val) {}

static void aliyuntts_numeric_param_tts(switch_speech_handle_t *sh, char *param, int val) {}

static void aliyuntts_float_param_tts(switch_speech_handle_t *sh, char *param, double val) {}

/* internal function to load the aliyuntts configuration*/
static switch_status_t aliyuntts_load_config(void)
{
	char *cf = (char*)"aliyun_tts.conf";
	switch_xml_t cfg, xml = NULL, param, settings;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	// global default settings
	globals.server_url = (char *)"";
	globals.app_key = NULL;
	globals.voice_name = (char *)"";
	globals.buffer_size = 49152;
	globals.buffer_max_size = 4194304;
	globals.token_expire_time = 0;
	memset(globals.silent_data, 0, sizeof(globals.silent_data));

	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", cf);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			char *var = (char *)switch_xml_attr_soft(param, "name");
			char *val = (char *)switch_xml_attr_soft(param, "value");
			if (strcasecmp(var, "server_url") == 0) {
				globals.server_url = switch_core_strdup(globals.pool, val);
			} else if (strcasecmp(var, "app_key") == 0) {
				globals.app_key = switch_core_strdup(globals.pool, val);
			} else if (strcasecmp(var, "voice_name") == 0) {
				globals.voice_name = switch_core_strdup(globals.pool, val);
			} else if (strcasecmp(var, "buffer_size") == 0) {
				globals.buffer_size = atoi(val);
			} else if (strcasecmp(var, "buffer_max_size") == 0) {
				globals.buffer_max_size = atoi(val);
			} else if (strcasecmp(var, "access_key_id") == 0) {
				globals.access_key_id = switch_core_strdup(globals.pool, val);
			} else if (strcasecmp(var, "access_key_secret") == 0) {
				globals.access_key_secret = switch_core_strdup(globals.pool, val);
			} else if (strcasecmp(var, "ws_conn_timeout_ms") == 0) {
				globals.ws_conn_timeout_ms = atoi(val);
			} else if (strcasecmp(var, "sample_rate") == 0) {
				globals.sample_rate = atoi(val);
			} else if (strcasecmp(var, "write_pcm_enable") == 0) {
				globals.write_pcm_enable = switch_true(val);
			} else if (strcasecmp(var, "voice_volume") == 0) {
				globals.voice_volume = switch_safe_atoi(val, 50);
			}
		}
	}

done:
	if (xml) { switch_xml_free(xml); }

	return status;
}

static void do_config_load(void)
{
	switch_mutex_lock(globals.MUTEX);
	aliyuntts_load_config();
	switch_mutex_unlock(globals.MUTEX);
}

SWITCH_STANDARD_APP(raliyuntts_resume_session_function)
{
	aliyuntts_t *pvt;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	pvt = (aliyuntts_t *)switch_channel_get_private(channel, "aliyuntts_mod");
	if (!pvt || !pvt->pool) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "%s resume_tts command failed to get private data.\n", switch_core_session_get_uuid(session));
		return;
	}

	if (!zstr(data)) {	 
		// Record TTS request text and current status
		bool is_busy = !pvt->synthesisCompleted.load();
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
						  "%s received resume_tts command, current TTS status=%s, text=%s \n", 
						  pvt->uuid, is_busy ? "busy" : "idle", data);

		// Add to TTS text queue
		char *tts_text = switch_core_strdup(pvt->pool, data);
		switch_queue_push(pvt->tts_text_queue, tts_text);

	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "%s resume_tts command requires 1 parameter: TTS text.\n", pvt->uuid);
	}
};

SWITCH_MODULE_LOAD_FUNCTION(mod_aliyun_tts_load)
{
	switch_speech_interface_t* tts_interface;

	switch_mutex_init(&globals.MUTEX, SWITCH_MUTEX_NESTED, pool);
	globals.pool = pool;

	do_config_load();

	/* connect my internal structure to the blank pointer passed to me */
	switch_application_interface_t *app_interface;
	*module_interface = switch_loadable_module_create_module_interface(pool, modname); 
   
	SWITCH_ADD_APP(app_interface, "aliyuntts_resume", "aliyuntts_mod", "aliyuntts_mod", raliyuntts_resume_session_function,
				   "",
				   SAF_NONE
	); 

	tts_interface = (switch_speech_interface_t *)switch_loadable_module_create_interface(*module_interface,
																						 SWITCH_SPEECH_INTERFACE);
	tts_interface->interface_name = "aliyuntts";
	tts_interface->speech_open = aliyuntts_speech_open;
	tts_interface->speech_close = aliyuntts_speech_close;
	tts_interface->speech_feed_tts = aliyuntts_speech_feed_tts;
	tts_interface->speech_read_tts = aliyuntts_speech_read_tts;
	tts_interface->speech_flush_tts = aliyuntts_speech_flush_tts;
	tts_interface->speech_text_param_tts = aliyuntts_text_param_tts;
	tts_interface->speech_numeric_param_tts = aliyuntts_numeric_param_tts;
	tts_interface->speech_float_param_tts = aliyuntts_float_param_tts;

	

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_aliyun_tts_shutdown)
{
	return SWITCH_STATUS_UNLOAD;
}

// Prepare TTS request data
static std::string prepare_tts_request(const char *text, aliyuntts_t *pvt)
{
    if (!text || !pvt) {
        return "";
    }
    
    // Generate unique message ID
    string msg_id_str = genUUID();
    
    // Create JSON document
    Document document;
    document.SetObject();
    rapidjson::Document::AllocatorType &allocator = document.GetAllocator();
    
    // Add header part
    rapidjson::Value header(rapidjson::kObjectType);
    header.AddMember("namespace", rapidjson::StringRef("SpeechSynthesizer"), allocator);
    header.AddMember("name", rapidjson::StringRef("StartSynthesis"), allocator);
    header.AddMember("message_id", rapidjson::StringRef(msg_id_str.c_str()), allocator);
    header.AddMember("appkey", rapidjson::StringRef(globals.app_key), allocator);
    header.AddMember("task_id", rapidjson::StringRef(pvt->task_uuid), allocator);
    document.AddMember("header", header, allocator);
    
    // Add payload part
    rapidjson::Value payload(rapidjson::kObjectType);
    payload.AddMember("text", rapidjson::StringRef(text), allocator);
    payload.AddMember("voice", rapidjson::StringRef(pvt->voice), allocator);
    payload.AddMember("format", rapidjson::StringRef("wav"), allocator);
    payload.AddMember("sample_rate", globals.sample_rate, allocator);
    payload.AddMember("volume", globals.voice_volume, allocator);
    payload.AddMember("speech_rate", -300, allocator);
    payload.AddMember("pitch_rate", 0, allocator);
    payload.AddMember("enable_subtitle", false, allocator);
    document.AddMember("payload", payload, allocator);
    
    // Add context part
    rapidjson::Value context(rapidjson::kObjectType);
    rapidjson::Value sdk(rapidjson::kObjectType);
    sdk.AddMember("name", rapidjson::StringRef("nls-sdk-cpp"), allocator);
    sdk.AddMember("version", rapidjson::StringRef("1.0.0"), allocator);
    context.AddMember("sdk", sdk, allocator);
    document.AddMember("context", context, allocator);
    
    // Convert to JSON string
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);
    
    return buffer.GetString();
}

 
