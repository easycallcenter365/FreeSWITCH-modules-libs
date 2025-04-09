/*
 * websocket server for rnnoise
 * 
 * The workflow is:  Freeswitch mod_funasr send audio data  -> websocket server.
 *  websocket server do four things: 
    a. convert 8k audio to 16k audio;  
    b. denoise audio data;  
    c. send audio data to funasr server;
    d. recv asr result from funasr server and send to freeswitch.

    The  websocket server works on multiple worker_processes model, if one of the worker_processes crashed, 
    the master process will start a new one. It works like  nginx.
    For the known memory issue in rnnoise lib, we develop a proxy middle-ware to minimize the impact of process crashes.
    That's the reason why this program comes.

 *  @build
 *  before compile, we need to copy libfiles to dest directory /usr/local/lib: libfreeswitch.so  libhv.so  librnnoise.so
 *  and then change dir to /libs/libhv-master/master/libhv/examples/ :
 *   g++  -o rnnoise_server -fPIC -g -O -ggdb -std=c++11 -Wall  \
     -I../../../../../src/include/           \
     -I../../../../../libs/libteletone/src/  \
     -I../../../../../libs/spdlog/           \
     -I../include/hv/                        \
     rnnoise_server.cpp    -lpthread   -L/usr/local/lib/   -lhv -lfreeswitch -lrnnoise
 *
 *  @server  bin/websocket_server_test  /etc/rnnoise_server.conf
 */ 

#include "spdlog/spdlog.h"
#include "spdlog/async.h"
#include "spdlog/sinks/basic_file_sink.h"
#include <spdlog/details/registry.h>
#include "WebSocketServer.h"
#include "WebSocketClient.h"
#include "EventLoop.h"
#include "htime.h"
#include "audio_queue.h"
#include "hlog.h"
#include <EventLoopThreadPool.h>
#include <chrono>
#include <condition_variable>
#include "iniparser.h"
#include "hv.h"

extern "C" {
#include "rnnoise_16k.h"
}
using namespace std::chrono;
using namespace hv;

#define PCM_8k_FRAME_SIZE 160
#define PCM_16k_FRAME_SIZE 320
#define THREAD_POOL_SIZE 2
#define DEBUG_MODEL 0

std::shared_ptr<spdlog::logger> async_log_file;
#define spdlogflush()  async_log_file->flush()
#define spdlogflush_on_debug() if(DEBUG_MODEL)  async_log_file->flush()
#define spdlogi(fmt, ...) async_log_file->info(fmt " [{} {} {}]", ##__VA_ARGS__, __FILENAME__, __LINE__, __FUNCTION__); spdlogflush_on_debug()
#define spdlogw(fmt, ...) async_log_file->warn(fmt " [{} {} {}]", ##__VA_ARGS__, __FILENAME__, __LINE__, __FUNCTION__); spdlogflush_on_debug()
#define spdloge(fmt, ...) async_log_file->error(fmt " [{} {} {}]", ##__VA_ARGS__, __FILENAME__, __LINE__, __FUNCTION__); spdlogflush_on_debug()
#define spdlogf(fmt, ...) async_log_file->critical(fmt " [{} {} {}]", ##__VA_ARGS__, __FILENAME__, __LINE__, __FUNCTION__); spdlogflush_on_debug()

 struct rnnoise_data { 
    switch_mutex_t* write_mutex;
    volatile bool ws_client_closed;
    volatile bool asr_connected;
    volatile bool asr_closed;
    char* uuid;
    /** FreeSWITCH audio buffer */
    audio_queue_t* audio_queue;
    switch_thread_t* asr_thread;
    switch_memory_pool_t* pool;
    WebSocketChannelPtr ws_channel;
 };
 typedef struct rnnoise_data rnnoise_data_t;

 /**
  * funasr server config info
  **/
 struct rnnoise_server_config {
     IniParser* parser;
     char conf_file[MAX_PATH];

     char logfile[MAX_PATH];
     int loglevel;
     int log_remain_days;
     int log_filesize;
     int log_fsync;

     int worker_processes;
     int worker_threads;
     int port;

     char asr_server_url[MAX_PATH];
     int ws_conn_timeout_ms;
     int sample_rate;
     int log_asr_response;
     int rnnoise_enable;
     int write_pcm_enable;
     int ws_conn_timeout_warn_ms;    
 };
 typedef struct rnnoise_server_config rnnoise_server_config_t;

 /** Module global variables */
 static rnnoise_server_config_t rnnoise_globals;

 static EventLoopThreadPool ws_client_loop_threads(THREAD_POOL_SIZE);

 inline void conf_ctx_init(rnnoise_server_config_t* ctx) {
     ctx->parser = new IniParser;
     ctx->loglevel = LOG_LEVEL_INFO;
     ctx->worker_processes = 1;
     ctx->worker_threads = 1;
     ctx->port = 5090;
 }

 void init_sink() {     
     spdlog::init_thread_pool(32768, 1); // queue with max 32k items 1 backing thread. 
     async_log_file =  spdlog::create_async<spdlog::sinks::basic_file_sink_mt>("async_file_logger", rnnoise_globals.logfile);    
     async_log_file->set_pattern("[%Y-%m-%d %H:%M:%S] [%l] %v");      
     spdlogi("spdlog init ..."); 
 }

 int parse_confile(const char* confile) {
     int ret = rnnoise_globals.parser->LoadFromFile(confile);
     if (ret != 0) {
         printf("Load confile [%s] failed: %d\n", confile, ret);
         exit(-40);
     }

     // logfile
     std::string str = rnnoise_globals.parser->GetValue("logfile");
     if (!str.empty()) {
         strncpy(rnnoise_globals.logfile, str.c_str(), sizeof(rnnoise_globals.logfile));       
         char log_file_name[13];
         sprintf(log_file_name, ".%s.log", "libhv");
         // switch_string_replace return a new string.
         char* libhv_log_path = switch_string_replace(rnnoise_globals.logfile, ".log", log_file_name);    
         hlog_set_file(libhv_log_path);
     }
     // loglevel
     str = rnnoise_globals.parser->GetValue("loglevel");
     if (!str.empty()) {
         hlog_set_level_by_str(str.c_str());
     }
     // log_filesize
     str = rnnoise_globals.parser->GetValue("log_filesize");
     if (!str.empty()) {
        hlog_set_max_filesize_by_str(str.c_str());
     }
     // log_remain_days
     str = rnnoise_globals.parser->GetValue("log_remain_days");
     if (!str.empty()) {
         hlog_set_remain_days(atoi(str.c_str()));
     }

      // log_fsync
     str = rnnoise_globals.parser->GetValue("log_fsync");
     if (!str.empty()) {
         rnnoise_globals.log_fsync = atoi(str.c_str());
         logger_enable_fsync(hlog, rnnoise_globals.log_fsync);         
         hlogi("libhv logger_enable_fsync=%d", rnnoise_globals.log_fsync);
     }

     // worker_processes
     int worker_processes = 0;
     str = rnnoise_globals.parser->GetValue("worker_processes");
     if (str.size() != 0) {
         if (strcmp(str.c_str(), "auto") == 0) {
             worker_processes = get_ncpu();
             hlogi("worker_processes=ncpu=%d", worker_processes);
         }
         else {
             worker_processes = atoi(str.c_str());
         }
     }
     rnnoise_globals.worker_processes = LIMIT(0, worker_processes, 64);

     // worker_threads
     int worker_threads = 0;
     str = rnnoise_globals.parser->GetValue("worker_threads");
     if (str.size() != 0) {
         if (strcmp(str.c_str(), "auto") == 0) {
             worker_threads = get_ncpu();
             hlogi("worker_threads=ncpu=%d", worker_threads);
         }
         else {
             worker_threads = atoi(str.c_str());
         }
     }
     rnnoise_globals.worker_threads = LIMIT(0, worker_threads, 64);

     // port
     int port = rnnoise_globals.parser->Get<int>("port");
     if (port == 0) {
         printf("Please config listen port!\n");
         exit(-10);
     }
     rnnoise_globals.port = port;

     rnnoise_globals.ws_conn_timeout_ms = rnnoise_globals.parser->Get<int>("ws_conn_timeout_ms");
     rnnoise_globals.sample_rate = rnnoise_globals.parser->Get<int>("sample_rate");
     rnnoise_globals.log_asr_response = rnnoise_globals.parser->Get<int>("log_asr_response");
     rnnoise_globals.rnnoise_enable = rnnoise_globals.parser->Get<int>("rnnoise_enable");
     rnnoise_globals.write_pcm_enable = rnnoise_globals.parser->Get<int>("write_pcm_enable");
     rnnoise_globals.ws_conn_timeout_warn_ms = 5000;
     str = rnnoise_globals.parser->GetValue("asr_server_url");
     if (!str.empty()) {
         strncpy(rnnoise_globals.asr_server_url, str.c_str(), sizeof(rnnoise_globals.asr_server_url));
     }

     std::ostringstream oss;
     oss << " load config file:" << rnnoise_globals.conf_file << "! \n";
     oss << " port=" << rnnoise_globals.port;
     oss << "\n worker_processes = " << rnnoise_globals.worker_processes;
     oss << "\n worker_threads = " << rnnoise_globals.worker_threads;

     oss << "\n\n logfile = " << rnnoise_globals.logfile;
     oss << "\n loglevel = " << rnnoise_globals.loglevel;
     oss << "\n log_remain_days = " << rnnoise_globals.log_remain_days;
     oss << "\n log_filesize = " << rnnoise_globals.log_filesize;
     oss << "\n log_fsync = " << rnnoise_globals.log_fsync;
    
     oss << "\n\n asr_server_url = " << rnnoise_globals.asr_server_url;
     oss << "\n ws_conn_timeout_ms = " << rnnoise_globals.ws_conn_timeout_ms;
     oss << "\n sample_rate = " << rnnoise_globals.sample_rate;
     oss << "\n log_asr_response = " << rnnoise_globals.log_asr_response;
     oss << "\n rnnoise_enable = " << rnnoise_globals.rnnoise_enable;
     oss << "\n write_pcm_enable= " << rnnoise_globals.write_pcm_enable;

     std::cout << oss.str() << std::endl;

     printf("parse_confile('%s') OK \n\n", confile);
     return 0;
 }

 static void destroy(rnnoise_data** pvtArg) {
     rnnoise_data* pvt = *pvtArg;
     audio_queue_clear(pvt->audio_queue);
     audio_queue_destroy(pvt->audio_queue);
     switch_mutex_destroy(pvt->write_mutex);

     pvt->audio_queue = NULL;
     pvt->asr_thread = NULL;
     pvt->write_mutex = NULL;

     if (pvt->pool) {
         spdlogi("switch_core_destroy_memory_pool, pvt->pool memory address={}, uuid={}", (void*)(pvt->pool), pvt->uuid);
         switch_core_destroy_memory_pool(&pvt->pool);	
     }
     pvt->uuid = NULL;
     pvt->pool = NULL;
     pvt = NULL;
 }

class MyContext {
public:
    MyContext() { 
        timerID = INVALID_TIMER_ID;
    }
    ~MyContext() { 
    }

    int handleMessage(const std::string& msg, enum ws_opcode opcode) {
        spdlogi("onmessage(type={}, len={}): {} ", opcode == WS_OPCODE_TEXT ? "text" : "binary", (int)msg.size(), (int)msg.size(), msg.data());
        return msg.size();
    }
     
    TimerID timerID; 
    rnnoise_data_t* pvt;
    std::string ipport;
};

 static void* send_audio_to_asr_server(switch_thread_t* thread, void* user_data) {

     rnnoise_data_t* pvt = (rnnoise_data_t*)user_data;
     char* uuid = pvt->uuid;
     if (pvt->ws_client_closed) {
         destroy(&pvt);
         return NULL;
     }
      
     spdlogi("send_audio_to_asr_server started, pid={} tid={}", hv_getpid(), hv_gettid());

     // for ws_close and ws_open events， we need seperate 'condition_variable' and  mutex vars
     std::mutex mtx;
     std::condition_variable cv;
     std::mutex mtx_for_close;
     std::condition_variable cv_for_close;
     volatile bool connected = false;
     volatile bool closed = false;

     WebSocketClient ws(ws_client_loop_threads.nextLoop());
     ws.onopen = [&ws, &pvt, &mtx, &cv, &connected]() {
         steady_clock::time_point start_time = steady_clock::now();
         std::string pram_begin = "{\"chunk_size\":[5,10,5], \"audio_fs\" : 16000, \"wav_name\":\"";
         std::string pram_end = "\",\"is_peaking\":true, \"wav_format\":\"pcm\", \"mode\":\"2pass\"}";
         std::string uuid = pvt->uuid;
         std::string pramStr = pram_begin + uuid + pram_end;
         spdlogi("FunAsr connection ready, try to send init parameter,  uuid={}", pvt->uuid);
         ws.send(pramStr);
         std::unique_lock<std::mutex> lock(mtx);
         connected = true;
         pvt->asr_connected = true;
         // send signal to main thread
         cv.notify_one();
         auto duration = duration_cast<microseconds>(steady_clock::now() - start_time).count();
         spdlogi("threadpool-execute-time-cost1 = {} microseconds, uuid={}", duration < 0.0L ? 0.0L : duration, pvt->uuid);
     };
     ws.onclose = [&closed, &uuid, &mtx_for_close, &cv_for_close]() {
         spdlogi("recv FunAsr disconnected msg, uuid={}", uuid);
         std::unique_lock<std::mutex> lock(mtx_for_close);
         closed = true;
         // send signal to main thread
         cv_for_close.notify_one();
     };
     ws.onmessage = [&pvt](const std::string& msg) {
         if (msg.empty() || msg.c_str() == nullptr) {
             spdloge("INVALID asr response,ws onmessage,  uuid={}", pvt->uuid);
             return;
         }
         steady_clock::time_point start_time = steady_clock::now();

         if (pvt && pvt->pool && !pvt->ws_client_closed) {
             const char* asr_result = strdup(msg.c_str());
             if (asr_result && strlen(asr_result) > 0) {
                 // asr_result
                 pvt->ws_channel->send(asr_result);

                 if (rnnoise_globals.log_asr_response) {
                     spdlogi("ws onmessage, {}, uuid={}", asr_result, pvt->uuid);
                 }
             }
         }
         auto duration = duration_cast<microseconds>(steady_clock::now() - start_time).count();
         spdlogi("thread-pool-execute-time-cost2 = {} microseconds, uuid={}", duration < 0.0L ? 0.0L : duration, pvt->uuid);
     };

     reconn_setting_t reconn;
     reconn.max_retry_cnt = 0; // disable reconnect
     ws.connect_timeout = rnnoise_globals.ws_conn_timeout_ms;

     steady_clock::time_point start = steady_clock::now();
     steady_clock::time_point end;
     ws.setReconnect(&reconn);
     spdlogi("Try to connect funAsr server={}, conn_timeout={},  uuid={}", rnnoise_globals.asr_server_url, ws.connect_timeout, pvt->uuid);
     ws.open(rnnoise_globals.asr_server_url);
    
     // wait for connection ready
     {
         std::unique_lock<std::mutex> lock(mtx);
         cv.wait_for(lock, std::chrono::milliseconds(ws.connect_timeout), [&connected] { return connected; });
     } 

     if (pvt->asr_connected) {
         end = steady_clock::now();
         auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() * 0.001f;
         if (duration > rnnoise_globals.ws_conn_timeout_warn_ms) {
             spdloge("FunAsr connected successfully, BUT SPENT TOO LONG TIME,  time spent={} ms, uuid={}", duration, pvt->uuid);
         }
         else {
             spdlogi("FunAsr connected successfully, time spent ={} ms, uuid={}", duration, pvt->uuid);
         }

         switch_audio_resampler_t* resampler;
         if (rnnoise_globals.sample_rate == 16000) {
             if (switch_resample_create(&resampler, 8000, 16000, (uint32_t)640, SWITCH_RESAMPLE_QUALITY, 1) != SWITCH_STATUS_SUCCESS) {
                 spdlogf("Unable to create resampler!");
                 destroy(&pvt);
                 return NULL;
             }
         }

         int16_t data[PCM_8k_FRAME_SIZE] = {0};
         int16_t data_resample[PCM_16k_FRAME_SIZE] = {0};
         DenoiseState* denoise = NULL;
         if (rnnoise_globals.rnnoise_enable) {
             denoise = rnnoise_create();
             if (denoise) {
                 spdlogi("successfully create rnnoise instance,uuid={}  ", uuid);
             }
             else {
                spdlogf("FAILED to create rnnoise instance,uuid={}  ", uuid);
             }
         }
         float ok_data[PCM_16k_FRAME_SIZE];
         float* input = NULL;
         FILE* pcm_file = NULL;
         if (rnnoise_globals.write_pcm_enable) {
             char pcm_path[100] = {0};
             sprintf(pcm_path, "/tmp/16k_pcm_denoised_%s", uuid);
             pcm_file = fopen(pcm_path, "wb");
             if (!pcm_file) {
                 spdloge("FAILED to open file={}, uuid={} ", pcm_path, uuid);
             }
         }

         while (!pvt->ws_client_closed && !closed) {

             switch_size_t len = PCM_8k_FRAME_SIZE * 2;
             audio_queue_read(pvt->audio_queue, data, &len, 1);

             if (len == PCM_8k_FRAME_SIZE * 2) {
                 /* one frame read */

                 if (rnnoise_globals.sample_rate == 16000) {

                     // convert 8K to 16K
                     switch_resample_process(resampler, data, PCM_8k_FRAME_SIZE);
                     memcpy(data_resample, resampler->to, resampler->to_len * 2);

                     if (denoise) {
                         for (int i = 0; i < PCM_16k_FRAME_SIZE; i++) {
                             ok_data[i] = (float)data_resample[i];
                         }

                         input = ok_data;
                         for (int i = 0; i < 2; i++) {
                             rnnoise_process_frame(denoise, input, input);
                             input += PCM_8k_FRAME_SIZE;
                         }

                         for (int i = 0; i < PCM_16k_FRAME_SIZE; i++) {
                             data_resample[i] = (int16_t)ok_data[i]; // 转换为int16_t类型
                         }
                     }

                     ws.send((char*)data_resample, PCM_16k_FRAME_SIZE * 2, WS_OPCODE_BINARY);
                     if (pcm_file) {
                         fwrite(data_resample, sizeof(int16_t), PCM_16k_FRAME_SIZE, pcm_file);
                     }
                 }

                 if (rnnoise_globals.sample_rate == 8000) {
                     ws.send((char*)data, len, WS_OPCODE_BINARY);
                 }
             }
         }
         if (resampler) {
             switch_resample_destroy(&resampler);
         }
         if (denoise) {
             rnnoise_destroy(denoise);
         }
         if (pcm_file) {
             fclose(pcm_file);
         }
     }
     else {
         pvt->asr_closed = true;
         spdloge("WEBSOCKET CONNECT-FAILED, uuid={}", pvt->uuid);
     }

     if (!pvt->ws_client_closed) {
         // notify not to recv audio data
         pvt->asr_closed = true;
     }
     if (!closed) {
         ws.send("{\"is_speaking\":false}");
         // close is async
         ws.close();
         // wait for connection to be closed.
         {
             std::unique_lock<std::mutex> lock(mtx_for_close);
             cv_for_close.wait(lock, [&closed] { return closed; });
         }
     }
    
     if (closed) {
         spdlogi("Successfully close websocket ws object {}", uuid);
     }
     destroy(&pvt);
     spdlogi("Destroy funasr object for session {}", uuid);
     spdlogflush();
     pthread_exit(0);
     return NULL; 
 }

void random_crash_function() {
     // 设置随机数种子
     srand(time(NULL));

     // 生成一个 0 到 99 之间的随机数
     int random_number = rand() % 100;

     // 判断随机数是否触发崩溃
     if (random_number < 50) { // 假设触发崩溃的概率为 50%
         //hloge("Triggering random crash...");
         // 触发崩溃，退出程序
         int* null_ptr = NULL;
         *null_ptr = 10; 
     }
     else {
         spdlogi("No crash occurred.");
     }
}
 

void start_log_server() {
    HttpService http;    
    WebSocketService ws;
    ws.setPingInterval(10000);
    ws.onopen = [](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
        auto ctx = channel->newContextPtr<MyContext>();
        ctx->ipport = req->client_addr.ipport();
        spdlogi("recv request from client:{}", ctx->ipport);
    };
    ws.onmessage = [](const WebSocketChannelPtr& channel, const std::string& msg) {
        auto ctx = channel->getContextPtr<MyContext>();
        spdlogi("recv websocket msg from client:{}", ctx->ipport);
        spdlogi("{} log: {}", ctx->ipport, msg.data());
    };
    ws.onclose = [](const WebSocketChannelPtr& channel) {
        auto ctx = channel->getContextPtr<MyContext>();
        spdlogi("spdlog client closed, client address={}", ctx->ipport);       
        channel->deleteContextPtr();
    };

    WebSocketServer log_server;
    log_server.port = rnnoise_globals.port + 1;
    log_server.registerHttpService(&http);
    log_server.registerWebSocketService(&ws); 
    log_server.worker_threads = 5;
    log_server.start();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s  config_file_full_path \n ", argv[0]);
        return -10;
    }
    
    start_log_server();
    conf_ctx_init(&rnnoise_globals);
    const char* confile = argv[1];
    if (confile) {
        strncpy(rnnoise_globals.conf_file, confile, sizeof(rnnoise_globals.conf_file));
    }  
    parse_confile(rnnoise_globals.conf_file);

    init_sink();
    spdlogi("try to start rnnoise server ...!");    
    
    HttpService http;
    http.GET("/ping", [](const HttpContextPtr& ctx) {
        return ctx->send("pong");
    });
    http.GET("/about", [](const HttpContextPtr& ctx) { 
       std::string msg = R"(For the known memory issue in rnnoise lib, we develop a proxy middle-ware  minimize the impact of process crashes.
        The  websocket server works on multiple worker_processes model. 
        It resample and  denoise audio data before send to asr server. 
        That's the reason why this program comes.)";
        return ctx->send(msg.c_str()); 
    
    });
     
      
    WebSocketService ws;
    ws.setPingInterval(10000);
    ws.onopen = [](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {        
        auto ctx = channel->newContextPtr<MyContext>(); 
        
        rnnoise_data_t* pvt;
        switch_memory_pool_t* pool;
        switch_core_new_memory_pool(&pool);
        pvt = (rnnoise_data_t*)switch_core_alloc(pool, sizeof(rnnoise_data_t));
        pvt->pool = pool;
        
        char* uuid = switch_core_strdup(pool, req->Path().c_str());
        switch_replace_char(uuid, '/', '-', SWITCH_FALSE);
        switch_mutex_init(&pvt->write_mutex, SWITCH_MUTEX_UNNESTED, pool);          
        audio_queue_create(&pvt->audio_queue, uuid, uuid, pvt->pool);
        pvt->uuid = uuid;
        pvt->ws_client_closed = false;
        pvt->asr_connected = false;
        pvt->asr_closed = false;       
        pvt->ws_channel = channel;
        ctx->pvt = pvt;

        switch_threadattr_t* thd_attr = NULL;
        switch_threadattr_create(&thd_attr, pvt->pool);
        switch_threadattr_detach_set(thd_attr, 1);
        switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
        spdlogi("Try to create FunAsr-connect-Thread uuid={}", pvt->uuid);

        if (switch_thread_create(&pvt->asr_thread, thd_attr, send_audio_to_asr_server, (void*)pvt, pvt->pool) != SWITCH_STATUS_SUCCESS) {
            spdlogf("failed to create asrThread thread for session {}.", pvt->uuid);
            destroy(&pvt);
            return;
        }
       
        // random_crash_function();
    };
    ws.onmessage = [](const WebSocketChannelPtr& channel, const std::string& msg) {
        auto ctx = channel->getContextPtr<MyContext>();
        if (channel->opcode == WS_OPCODE_TEXT) {
            ctx->handleMessage(msg, channel->opcode);
        }

        if (!ctx->pvt || ctx->pvt->ws_client_closed || !ctx->pvt->asr_connected || ctx->pvt->asr_closed) {
            return;
        }
        if (channel->opcode == WS_OPCODE_BINARY && msg.length() == 320) {
            switch_mutex_lock(ctx->pvt->write_mutex);

            switch_size_t frame_len = 320;
            // send audio data to switch_buffer
            if (frame_len > 0) {
                audio_queue_write(ctx->pvt->audio_queue, (void*)msg.data(), &frame_len);
                audio_queue_signal(ctx->pvt->audio_queue);
            }

            switch_mutex_unlock(ctx->pvt->write_mutex);
        }
    };
    ws.onclose = [](const WebSocketChannelPtr& channel) {
        auto ctx = channel->getContextPtr<MyContext>();
        spdlogi("Freeswitch client closed, uuid={}", (ctx->pvt) ? ctx->pvt->uuid : "");        
        if (ctx->timerID != INVALID_TIMER_ID) {
            killTimer(ctx->timerID);
            ctx->timerID = INVALID_TIMER_ID;
        }
        if (ctx->pvt) {
            ctx->pvt->ws_client_closed = true;
        }
       channel->deleteContextPtr();
    };

    WebSocketServer server;
    server.port = rnnoise_globals.port;
    server.registerHttpService(&http);
    server.registerWebSocketService(&ws);
    if (!DEBUG_MODEL) {
        server.worker_processes = rnnoise_globals.worker_processes;
    }
    server.worker_threads = rnnoise_globals.worker_threads;
    server.start();
     
    spdlogi("rnnoise server STARTED ....");    
    spdlog::info("rnnoise server started, See log details: tail -f {} \n",  rnnoise_globals.logfile);
    spdlogflush();

    while (1) {
        sleep(10);
    }

    return 0;
}
