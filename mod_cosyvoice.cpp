
#include <switch.h>

#define ASIO_STANDALONE 1

#include <websocketpp/client.hpp>
#include <websocketpp/common/thread.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <thread>

#include "nlohmann/json.hpp"

typedef struct {
    bool _debug;
    switch_atomic_t cosyvoice_concurrent_cnt;
} cosyvoice_global_t;

cosyvoice_global_t *cosyvoice_globals;

typedef bool (*vfs_exist_func_t) (const char *path);

typedef void *(*vfs_open_func_t) (const char *path);
typedef void (*vfs_close_func_t) (void *user_data);

typedef size_t (*vfs_get_filelen_func_t) (void *user_data);
typedef size_t (*vfs_seek_func_t) (size_t offset, int whence, void *user_data);
typedef size_t (*vfs_read_func_t) (void *ptr, size_t count, void *user_data);
typedef size_t (*vfs_write_func_t) (const void *ptr, size_t count, void *user_data);
typedef size_t (*vfs_tell_func_t) (void *user_data);

typedef struct {
    vfs_exist_func_t vfs_exist_func;
    vfs_open_func_t vfs_open_func;
    vfs_close_func_t vfs_close_func;
    vfs_get_filelen_func_t vfs_get_filelen_func;
    vfs_seek_func_t vfs_seek_func;
    vfs_read_func_t vfs_read_func;
    vfs_write_func_t vfs_write_func;
    vfs_tell_func_t vfs_tell_func;
} vfs_func_t;

template<typename T>
class WebsocketClient;

typedef WebsocketClient<websocketpp::config::asio_tls_client> cosyvoice_client;
//typedef WebsocketClient<websocketpp::config::asio_client> cosyvoice_client;

void gen_uuidstr_without_dash(std::string &str_uuid) {
    switch_uuid_t uuid;
    switch_uuid_get(&uuid);

    char buf[37]; // 32 bytes for UUID + 5 bytes for '-\0'
    switch_uuid_format(buf, &uuid);

    char str[33];

    // 手动拼接字符串，去掉破折号
    sprintf(str, "%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c",
           buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
           buf[9], buf[10], buf[11], buf[12], buf[14], buf[15], buf[16], buf[17],
           buf[19], buf[20], buf[21], buf[22], buf[24], buf[25], buf[26], buf[27],
           buf[28], buf[29], buf[30], buf[31], buf[32], buf[33], buf[34], buf[35]);

    str_uuid = str;
}

/**
 * Define a semi-cross platform helper method that waits/sleeps for a bit.
 */
void WaitABit(long milliseconds) {
#ifdef WIN32
    Sleep(1000);
#else
    usleep(1000 * milliseconds);
#endif
}

typedef websocketpp::config::asio_client::message_type::ptr message_ptr;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> context_ptr;

using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

context_ptr OnTlsInit(const websocketpp::connection_hdl &) {
    context_ptr ctx = websocketpp::lib::make_shared<asio::ssl::context>(asio::ssl::context::sslv23);

    try {
        ctx->set_options(
                asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                asio::ssl::context::no_sslv3 | asio::ssl::context::single_dh_use);

    } catch (std::exception &e) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "OnTlsInit asio::ssl::context::set_options exception: %s\n", e.what());
    }
    return ctx;
}

// template for tls or not config
template<typename T>
class WebsocketClient {
public:
    // typedef websocketpp::client<T> client;
    // typedef websocketpp::client<websocketpp::config::asio_tls_client>
    // wss_client;
    typedef websocketpp::lib::lock_guard<websocketpp::lib::mutex> scoped_lock;

    WebsocketClient(const std::string &token, const std::string &appkey, const std::string &saveto, vfs_func_t *vfs)
    : m_open(false), m_done(false), m_appkey(appkey), m_token(token), m_saveto(saveto), m_vfs(vfs) {
        gen_uuidstr_without_dash(m_task_id);

        // set up access channels to only log interesting things
        m_client.clear_access_channels(websocketpp::log::alevel::all);
        m_client.set_access_channels(websocketpp::log::alevel::connect);
        m_client.set_access_channels(websocketpp::log::alevel::disconnect);
        m_client.set_access_channels(websocketpp::log::alevel::app);

        // Initialize the Asio transport policy
        m_client.init_asio();
        m_client.start_perpetual();

        // Bind the handlers we are using
        using websocketpp::lib::bind;
        using websocketpp::lib::placeholders::_1;
        m_client.set_open_handler(bind(&WebsocketClient::on_open, this, _1));
        m_client.set_close_handler(bind(&WebsocketClient::on_close, this, _1));

        m_client.set_message_handler(
                [this](websocketpp::connection_hdl hdl, message_ptr msg) {
                    on_message(hdl, msg);
                });

        m_client.set_fail_handler(bind(&WebsocketClient::on_fail, this, _1));
        m_client.clear_access_channels(websocketpp::log::alevel::all);
    }

    std::string getThreadIdOfString(const std::thread::id &id) {
        std::stringstream sin;
        sin << id;
        return sin.str();
    }

    void on_message(websocketpp::connection_hdl hdl, message_ptr msg) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "on_message: opcode = %d\n", msg->get_opcode());

        const std::string &payload = msg->get_payload();
        switch (msg->get_opcode()) {
            case websocketpp::frame::opcode::text: {
                nlohmann::json synthesis_event = nlohmann::json::parse(payload);
                std::string id_str = getThreadIdOfString(std::this_thread::get_id());
                if (cosyvoice_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "thread: %s, on_message = %s\n",
                                      id_str.c_str(),
                                      payload.c_str());
                }

                if (synthesis_event["header"]["name"] == "SynthesisStarted") {
                    /* SynthesisStarted 事件
                    {
                        "header": {
                            "message_id": "05450bf69c53413f8d88aed1ee60****",
                                    "task_id": "640bc797bb684bd6960185651307****",
                                    "namespace": "FlowingSpeechSynthesizer",
                                    "name": "SynthesisStarted",
                                    "status": 20000000,
                                    "status_message": "GATEWAY|SUCCESS|Success."
                        },
                        "payload": {
                            "session_id": "1231231dfdf****"
                        }
                    } */
                    // onSynthesisStarted(m_asr_ctx, asr_result["text"]);
                    {
                        scoped_lock guard(m_lock);
                        m_synthesisReady = true;
                    }
                    if (cosyvoice_globals->_debug) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "on SynthesisStarted event\n");
                    }
                } else if (synthesis_event["header"]["name"] == "SentenceBegin") {
                    /* SentenceBegin 事件
                    {
                        "header": {
                            "message_id": "05450bf69c53413f8d88aed1ee60****",
                            "task_id": "640bc797bb684bd6960185651307****",
                            "namespace": "FlowingSpeechSynthesizer",
                            "name": "SentenceBegin",
                            "status": 20000000,
                            "status_message": "GATEWAY|SUCCESS|Success."
                        },
                        "payload": {
                            "index": 1
                        }
                    }
                     */
                    // onSentenceEnd(m_asr_ctx, asr_result["text"]);
                    if (cosyvoice_globals->_debug) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "on SentenceBegin event\n");
                    }
                } else if (synthesis_event["header"]["name"] == "SentenceSynthesis") {
                    /* SentenceSynthesis 事件
                    {
                        "header": {
                            "message_id": "05450bf69c53413f8d88aed1ee60****",
                            "task_id": "640bc797bb684bd6960185651307****",
                            "namespace": "FlowingSpeechSynthesizer",
                            "name": "SentenceSynthesis",
                            "status": 20000000,
                            "status_message": "GATEWAY|SUCCESS|Success."
                        },
                        "payload": {
                            "subtitles": [
                                {
                                    "text": "",
                                    "begin_time": 0,
                                    "end_time": 0,
                                    "begin_index": 0,
                                    "end_index": 1,
                                    "sentence": true,
                                    "phoneme_list": []
                                },
                                {
                                    "text": "今",
                                    "begin_time": 0,
                                    "end_time": 175,
                                    "begin_index": 0,
                                    "end_index": 1,
                                    "sentence": false,
                                    "phoneme_list": [
                                        {
                                            "begin_time": 0,
                                            "end_time": 120,
                                            "text": "j_c",
                                            "tone": "1"
                                        },
                                        {
                                            "begin_time": 120,
                                            "end_time": 170,
                                            "text": "in_c",
                                            "tone": "1"
                                        }
                                    ]
                                }
                            ]
                        }
                    }
                     */
                    // onSentenceEnd(m_asr_ctx, asr_result["text"]);
                    if (cosyvoice_globals->_debug) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "on SentenSentenceSynthesis event\n");
                    }
                } else if (synthesis_event["header"]["name"] == "SentenceEnd") {
                    /* SentenceEnd 事件
                    {
                        "header": {
                            "message_id": "05450bf69c53413f8d88aed1ee60****",
                            "task_id": "640bc797bb684bd6960185651307****",
                            "namespace": "FlowingSpeechSynthesizer",
                            "name": "SentenceEnd",
                            "status": 20000000,
                            "status_message": "GATEWAY|SUCCESS|Success."
                        },
                        "payload": {
                            "subtitles": [
                                {
                                    "text": "",
                                    "begin_time": 0,
                                    "end_time": 0,
                                    "begin_index": 0,
                                    "end_index": 1,
                                    "sentence": true,
                                    "phoneme_list": []
                                },
                                {
                                    "text": "今",
                                    "begin_time": 0,
                                    "end_time": 175,
                                    "begin_index": 0,
                                    "end_index": 1,
                                    "sentence": false,
                                    "phoneme_list": [
                                        {
                                            "begin_time": 0,
                                            "end_time": 120,
                                            "text": "j_c",
                                            "tone": "1"
                                        },
                                        {
                                            "begin_time": 120,
                                            "end_time": 170,
                                            "text": "in_c",
                                            "tone": "1"
                                        }
                                    ]
                                },
                                {
                                    "text": "天",
                                    "begin_time": 175,
                                    "end_time": 320,
                                    "begin_index": 1,
                                    "end_index": 2,
                                    "sentence": false,
                                    "phoneme_list": [
                                        {
                                            "begin_time": 0,
                                            "end_time": 120,
                                            "text": "t_c",
                                            "tone": "1"
                                        },
                                        {
                                            "begin_time": 120,
                                            "end_time": 170,
                                            "text": "ian_c",
                                            "tone": "1"
                                        }
                                    ]
                                }
                            ]
                        }
                    }
                     */
                    // onSentenceEnd(m_asr_ctx, asr_result["text"]);
                    if (cosyvoice_globals->_debug) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "on SentenceEnd event\n");
                    }
                } else if (synthesis_event["header"]["name"] == "SynthesisCompleted") {
                    /* SynthesisCompleted 事件
                    {
                        "header": {
                            "message_id": "05450bf69c53413f8d88aed1ee60****",
                                    "task_id": "640bc797bb684bd6960185651307****",
                                    "namespace": "FlowingSpeechSynthesizer",
                                    "name": "SynthesisCompleted",
                                    "status": 20000000,
                                    "status_message": "GATEWAY|SUCCESS|Success."
                        }
                    }
                     */
                    // onSentenceEnd(m_asr_ctx, asr_result["text"]);
                    if (cosyvoice_globals->_debug) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "on SynthesisCompleted event\n");
                    }
                    websocketpp::lib::error_code ec;

                    m_client.close(hdl, websocketpp::close::status::going_away, "", ec);

                    if (ec) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Error closing connection: %s\n",
                                          ec.message().c_str());
                    }
                }
                break;
            }
            case websocketpp::frame::opcode::binary: {
                // recived binary data
                const auto* wav_data = static_cast<const char*>(payload.data());
                int32_t num_samples = payload.size();

                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "binary: file_name: %s, data.size() %d, vfs: %p\n",
                                  m_saveto.c_str(), num_samples, m_vfs);

                /*
                void *tts_file = m_vfs->vfs_open_func(m_saveto.c_str());
                if (tts_file) {
                    m_vfs->vfs_seek_func(0, SEEK_END, tts_file);
                    m_vfs->vfs_write_func(wav_data, num_samples, tts_file);
                    m_vfs->vfs_close_func(tts_file);
                } else {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "binary: can't open file_name: %s for append\n",
                                      m_saveto.c_str());
                }
                 */

                if (cosyvoice_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "on binary audio data received\n");
                }
                break;
            }
            default:
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "un-handle opcode: %d\n", msg->get_opcode());
                break;
        }
    }

    // This method will block until the connection is complete
    int startSynthesis(const std::string &uri, const std::string &voice) {
        if (cosyvoice_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "StartSynthesis: %s voice: %s\n", uri.c_str(),
                              voice.c_str());
        }

        {
            // Create a new connection to the given URI
            websocketpp::lib::error_code ec;
            typename websocketpp::client<T>::connection_ptr con = m_client.get_connection(uri, ec);
            if (ec) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Get Connection Error: %s\n",
                                  ec.message().c_str());
                return -1;
            }
            // Grab a handle for this connection so we can talk to it in a thread
            // safe manor after the event loop starts.
            m_hdl = con->get_handle();
            con->append_header("X-NLS-Token", m_token);

            // Queue the connection. No DNS queries or network connections will be
            // made until the io_service event loop is run.
            m_client.connect(con);
        }

        // Create a thread to run the ASIO io_service event loop
        m_thread.reset(new websocketpp::lib::thread(&websocketpp::client<T>::run, &m_client));

        if (cosyvoice_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "start send cosyvoice first msg\n");
        }

        // first message
        bool wait = false;
        while (true) {
            {
                scoped_lock guard(m_lock);
                // If the connection has been closed, stop generating data
                if (m_done) {
                    return -1;
                }
                // If the connection hasn't been opened yet wait a bit and retry
                if (!m_open) {
                    wait = true;
                } else {
                    break;
                }
            }

            if (wait) {
                // LOG(INFO) << "wait.." << m_open;
                WaitABit(1000L);
                continue;
            }
        }

        {
            std::string message_id;
            gen_uuidstr_without_dash(message_id);
            // https://help.aliyun.com/zh/isi/developer-reference/websocket-protocol-description
            nlohmann::json json_startSynthesis = {
                    {"header", {
                            // 当次消息请求ID，随机生成32位唯一ID。
                            {"message_id", message_id},
                            // 整个实时语音合成的会话ID，整个请求中需要保持一致，32位唯一ID。
                            {"task_id", m_task_id},
                            {"namespace", "FlowingSpeechSynthesizer"},
                            {"name", "StartSynthesis"},
                            {"appkey", m_appkey}
                    }},
                    {"payload", {
                           {"voice", voice},
                           {"format", "wav"},
                           {"sample_rate", 16000},
                           {"volume", 100},
                           {"speech_rate", 60},
                           {"pitch_rate", 0},
                           {"enable_subtitle", true}
                    }}
            };

            std::string str_startSynthesis = json_startSynthesis.dump();
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "startSynthesis: send startSynthesis msg, detail: %s\n",
                              str_startSynthesis.c_str());

            websocketpp::lib::error_code ec;
            m_client.send(m_hdl, str_startSynthesis, websocketpp::frame::opcode::text, ec);
            if (ec) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "startSynthesis: send startSynthesis msg failed: %s\n",
                                  ec.message().c_str());
            } else {
                if (cosyvoice_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "startSynthesis: send startSynthesis msg success\n");
                }
            }
        }

        {
            bool wait = false;
            while (true) {
                {
                    scoped_lock guard(m_lock);
                    // If the connection has been closed, stop generating data
                    if (m_done) {
                        return -1;
                    }
                    // If the connection hasn't receive synthesisReady event
                    if (!m_synthesisReady) {
                        wait = true;
                    } else {
                        break;
                    }
                }

                if (wait) {
                    WaitABit(1000L);
                    if (cosyvoice_globals->_debug) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "startSynthesis: wait for SynthesisStarted event\n");
                    }
                    continue;
                }
            }
        }

        return 0;
    }

    int runSynthesis(const std::string &text) {
        std::string message_id;
        gen_uuidstr_without_dash(message_id);

        nlohmann::json json_runSynthesis = {
                {"header", {
                                   // 当次消息请求ID，随机生成32位唯一ID。
                                   {"message_id", message_id},
                                   // 整个实时语音合成的会话ID，整个请求中需要保持一致，32位唯一ID。
                                   {"task_id", m_task_id},
                                   {"namespace", "FlowingSpeechSynthesizer"},
                                   {"name", "RunSynthesis"},
                                   {"appkey", m_appkey}
                           }},
                {"payload", {
                                   {"text", text}
                           }}
        };

        std::string str_runSynthesis = json_runSynthesis.dump();
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "runSynthesis: send runSynthesis msg, detail: %s\n",
                          str_runSynthesis.c_str());

        websocketpp::lib::error_code ec;
        m_client.send(m_hdl, str_runSynthesis, websocketpp::frame::opcode::text, ec);
        if (ec) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "runSynthesis: send runSynthesis msg failed: %s\n",
                              ec.message().c_str());
        } else {
            if (cosyvoice_globals->_debug) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "runSynthesis: send runSynthesis msg success\n");
            }
        }
    }

    int stopSynthesis() {
        {
            std::string message_id;
            gen_uuidstr_without_dash(message_id);

            nlohmann::json json_stopSynthesis = {
                    {"header", {
                            // 当次消息请求ID，随机生成32位唯一ID。
                            {"message_id", message_id},
                            // 整个实时语音合成的会话ID，整个请求中需要保持一致，32位唯一ID。
                            {"task_id", m_task_id},
                            {"namespace", "FlowingSpeechSynthesizer"},
                            {"name", "StopSynthesis"},
                            {"appkey", m_appkey}
                    }}
            };

            std::string str_stopSynthesis = json_stopSynthesis.dump();
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "stopSynthesis: send stopSynthesis msg, detail: %s\n",
                              str_stopSynthesis.c_str());

            websocketpp::lib::error_code ec;
            m_client.send(m_hdl, str_stopSynthesis, websocketpp::frame::opcode::text, ec);
            if (ec) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "stopSynthesis: send stopSynthesis msg failed: %s\n",
                                  ec.message().c_str());
            } else {
                if (cosyvoice_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "stopSynthesis: send stopSynthesis msg success\n");
                }
            }
        }

        m_client.stop_perpetual();
        m_thread->join();
        // onChannelClosed(m_asr_ctx);
    }

    // The open handler will signal that we are ready to start sending data
    void on_open(const websocketpp::connection_hdl &) {
        if (cosyvoice_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Connection opened, starting data!\n");
        }

        {
            scoped_lock guard(m_lock);
            m_open = true;
        }
        // onTranscriptionStarted(m_asr_ctx);
    }

    // The close handler will signal that we should stop sending data
    void on_close(const websocketpp::connection_hdl &) {
        if (cosyvoice_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Connection closed, stopping data!\n");
        }

        {
            scoped_lock guard(m_lock);
            m_done = true;
        }
        // onTranscriptionCompleted(m_asr_ctx);
    }

    // The fail handler will signal that we should stop sending data
    void on_fail(const websocketpp::connection_hdl &) {
        if (cosyvoice_globals->_debug) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Connection failed, stopping data!\n");
        }

        {
            scoped_lock guard(m_lock);
            m_done = true;
        }
        // onTaskFailed(m_asr_ctx);
    }

    websocketpp::client<T> m_client;
    websocketpp::lib::shared_ptr<websocketpp::lib::thread> m_thread;

private:

    websocketpp::connection_hdl m_hdl;
    websocketpp::lib::mutex m_lock;
    std::string m_task_id;
    bool m_open;
    bool m_done;
    bool m_synthesisReady = false;
    std::string m_appkey;
    std::string m_token;
    std::string m_saveto;
    vfs_func_t *m_vfs;
};

cosyvoice_client *generateSynthesizer(const char *token, const char *appkey, const char *saveto, vfs_func_t *vfs) {
    auto *fac = new cosyvoice_client(std::string(token), std::string(appkey), std::string(saveto), vfs);
    if (!fac) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "generateClient failed.\n");
        return nullptr;
    }

    fac->m_client.set_tls_init_handler(bind(&OnTlsInit, ::_1));
    return fac;
}

//======================================== fun asr end ===============

//======================================== freeswitch module start ===============
SWITCH_MODULE_LOAD_FUNCTION(mod_cosyvoice_load);

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_cosyvoice_shutdown);

extern "C"
{
SWITCH_MODULE_DEFINITION(mod_cosyvoice, mod_cosyvoice_load, mod_cosyvoice_shutdown, nullptr);
};

/**
 * 根据AccessKey ID和AccessKey Secret重新生成一个token，
 * 并获取其有效期时间戳
 */
 /*
int generateToken(const char *akId, const char *akSecret, char **token, long *expireTime) {
    NlsToken nlsTokenRequest;
    nlsTokenRequest.setAccessKeyId(akId);
    nlsTokenRequest.setKeySecret(akSecret);
    //打印请求token的参数
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "begin send generate token rquest: akId=%s, akSecret=%s\n",
                      akId, akSecret);
    int ret = nlsTokenRequest.applyNlsToken();
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "request success, status code=%d, token=%s, expireTime=%d, message=%s\n", ret,
                      nlsTokenRequest.getToken(), nlsTokenRequest.getExpireTime(), nlsTokenRequest.getErrorMsg());
    if (ret < 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "generateToken Failed: %s\n",
                          nlsTokenRequest.getErrorMsg());
        return -1;
    }
    if (*token != nullptr) {
        free(*token);
    }
    *token = strdup(nlsTokenRequest.getToken());
    if (strcmp(*token, "") == 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "generateToken Failed: token is '' \n");
        return -1;
    }
    *expireTime = nlsTokenRequest.getExpireTime();
    return 0;
}
  */

static switch_status_t gen_cosyvoice_audio(const char *_token,
                                           const char *_appkey,
                                           const char *_url,
                                           const char *_voice,
                                           const char *_text,
                                           const char *_saveto,
                                           vfs_func_t *vfs_funcs) {
    /*
     * Gen Token REF: https://help.aliyun.com/zh/isi/getting-started/use-http-or-https-to-obtain-an-access-token
    switch_mutex_lock(g_tts_lock);
    {
        time_t now;
        time(&now);
        if (g_tts_expireTime - now < 10) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                              "uuid_alitts: the TTS token will be expired, please generate new token by AccessKey-ID and AccessKey-Secret\n");
            if (-1 == generateToken(g_tts_ak_id, g_tts_ak_secret, &g_tts_token, &g_tts_expireTime)) {
                switch_mutex_unlock(g_tts_lock);
                return SWITCH_STATUS_FALSE;
            }
        }
    }
    switch_mutex_unlock(g_tts_lock);
    */

    auto *synthesizer = generateSynthesizer(_token, _appkey, _saveto, vfs_funcs);

    if (!synthesizer) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "generateSynthesizer failed.\n");
        return SWITCH_STATUS_FALSE;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "generateSynthesizer ok\n");

    synthesizer->startSynthesis(std::string(_url),  std::string(_voice));

    // increment aliasr concurrent count
    switch_atomic_inc(&cosyvoice_globals->cosyvoice_concurrent_cnt);

    synthesizer->runSynthesis(std::string(_text));
    synthesizer->stopSynthesis();

     // decrement aliasr concurrent count
     switch_atomic_dec(&cosyvoice_globals->cosyvoice_concurrent_cnt);

    delete synthesizer;

     switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "delete synthesizer ok\n");

    return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(cosyvoice_concurrent_cnt_function) {
    const uint32_t concurrent_cnt = switch_atomic_read (&cosyvoice_globals->cosyvoice_concurrent_cnt);
    stream->write_function(stream, "%d\n", concurrent_cnt);
    switch_event_t *event = nullptr;
    if (switch_event_create(&event, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS) {
        event->subclass_name = strdup("cosyvoice_concurrent_cnt");
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Event-Subclass", event->subclass_name);
        switch_event_add_header(event, SWITCH_STACK_BOTTOM, "CosyVoice-Concurrent-Cnt", "%d", concurrent_cnt);
        switch_event_fire(&event);
    }

    return SWITCH_STATUS_SUCCESS;
}

static void play_cosyvoice_audio(const char *_saveto, const char *_playback_id, const char *cmd, switch_memory_pool_t *pool) {
    char *args = switch_core_sprintf(pool, "%s file={vars_playback_id=%s}%s", cmd, _playback_id, _saveto);

    if (cosyvoice_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "play_cosyvoice_audio: call znc_uuid_play with [%s].\n", args);
    }

    switch_stream_handle_t stream = { nullptr };
    SWITCH_STANDARD_STREAM(stream);

    switch_api_execute("znc_uuid_play", args, nullptr, &stream);
    switch_safe_free(stream.data);
}

#define COSYVOICE_DEBUG_SYNTAX "<on|off>"
SWITCH_STANDARD_API(mod_cosyvoice_debug)
{
    if (zstr(cmd)) {
        stream->write_function(stream, "-USAGE: %s\n", COSYVOICE_DEBUG_SYNTAX);
    } else {
        if (!strcasecmp(cmd, "on")) {
            cosyvoice_globals->_debug = true;
            stream->write_function(stream, "cosyvoice Debug: on\n");
        } else if (!strcasecmp(cmd, "off")) {
            cosyvoice_globals->_debug = false;
            stream->write_function(stream, "cosyvoice Debug: off\n");
        } else {
            stream->write_function(stream, "-USAGE: %s\n", COSYVOICE_DEBUG_SYNTAX);
        }
    }
    return SWITCH_STATUS_SUCCESS;
}

static std::string to_utf8(uint32_t cp) {
    /*
    if using C++11 or later, you can do this:

    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> conv;
    return conv.to_bytes( (char32_t)cp );

    Otherwise...
    */

    std::string result;

    int count;
    if (cp <= 0x007F) {
        count = 1;
    }
    else if (cp <= 0x07FF) {
        count = 2;
    }
    else if (cp <= 0xFFFF) {
        count = 3;
    }
    else if (cp <= 0x10FFFF) {
        count = 4;
    }
    else {
        return result; // or throw an exception
    }

    result.resize(count);

    if (count > 1)
    {
        for (int i = count-1; i > 0; --i)
        {
            result[i] = (char) (0x80 | (cp & 0x3F));
            cp >>= 6;
        }

        for (int i = 0; i < count; ++i)
            cp |= (1 << (7-i));
    }

    result[0] = (char) cp;

    return result;
}

static void ues_to_utf8(std::string &ues) {
    std::string::size_type startIdx = 0;
    do {
        startIdx = ues.find("\\u", startIdx);
        if (startIdx == std::string::npos) break;

        std::string::size_type endIdx = ues.find_first_not_of("0123456789abcdefABCDEF", startIdx+2);
        if (endIdx == std::string::npos) {
            endIdx = ues.length() + 1;
        }

        std::string tmpStr = ues.substr(startIdx+2, endIdx-(startIdx+2));
        std::istringstream iss(tmpStr);

        uint32_t cp;
        if (iss >> std::hex >> cp)
        {
            std::string utf8 = to_utf8(cp);
            ues.replace(startIdx, 2+tmpStr.length(), utf8);
            startIdx += utf8.length();
        }
        else {
            startIdx += 2;
        }
    }
    while (true);
}

#define MAX_API_ARGC 20

// uuid_cosyvoice <uuid> text=XXXXX saveto=<path> token=<token> appkey=<key> url=<url> playback_id=<id> voice=<voice>
SWITCH_STANDARD_API(uuid_cosyvoice_function) {
    if (zstr(cmd)) {
        stream->write_function(stream, "uuid_cosyvoice: parameter missing.\n");
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "uuid_cosyvoice: parameter missing.\n");
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t status = SWITCH_STATUS_SUCCESS;
    char *_token = nullptr;
    char *_text = nullptr;
    char *_saveto = nullptr;
    char *_appkey = nullptr;
    char *_url = nullptr;
    char *_voice = nullptr;
    char *_playback_id = nullptr;
    vfs_func_t *vfs_funcs;

    switch_memory_pool_t *pool;
    switch_core_new_memory_pool(&pool);
    char *my_cmd = switch_core_strdup(pool, cmd);

    char *argv[MAX_API_ARGC];
    memset(argv, 0, sizeof(char *) * MAX_API_ARGC);

    int argc = switch_split(my_cmd, ' ', argv);
    if (cosyvoice_globals->_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "cmd:%s, args count: %d\n", my_cmd, argc);
    }

    if (argc < 1) {
        stream->write_function(stream, "uuid is required.\n");
        switch_goto_status(SWITCH_STATUS_SUCCESS, end);
    }

    for (int idx = 1; idx < MAX_API_ARGC; idx++) {
        if (argv[idx]) {
            char *ss[2] = {nullptr, nullptr};
            int cnt = switch_split(argv[idx], '=', ss);
            if (cnt == 2) {
                char *var = ss[0];
                char *val = ss[1];
                if (cosyvoice_globals->_debug) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "process arg: %s = %s\n", var, val);
                }
                if (!strcasecmp(var, "token")) {
                    _token = switch_core_strdup(pool, val);
                    continue;
                }
                if (!strcasecmp(var, "text")) {
                    std::string ues(val);
                    ues_to_utf8(ues);
                    _text = switch_core_strdup(pool, ues.c_str());
                    continue;
                }
                if (!strcasecmp(var, "saveto")) {
                    _saveto = val;
                    continue;
                }
                if (!strcasecmp(var, "appkey")) {
                    _appkey = val;
                    continue;
                }
                if (!strcasecmp(var, "voice")) {
                    _voice = val;
                    continue;
                }
                if (!strcasecmp(var, "url")) {
                    _url = val;
                    continue;
                }
                if (!strcasecmp(var, "playback_id")) {
                    _playback_id = val;
                    continue;
                }
            }
        }
    }

    {
        switch_core_session_t *ses = switch_core_session_force_locate(argv[0]);
        if (!ses) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "cosyvoice failed, can't found session by %s\n", argv[0]);
            switch_goto_status(SWITCH_STATUS_SUCCESS, end);
        } else {
            switch_channel_t *channel = switch_core_session_get_channel(ses);
            vfs_funcs = (vfs_func_t*)switch_channel_get_private(channel, "vfs_mem");
            switch_core_session_rwunlock(ses);
            if (!vfs_funcs) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "cosyvoice failed, can't found vfs_mem %s\n", argv[0]);
                switch_goto_status(SWITCH_STATUS_SUCCESS, end);
            }
        }
    }

    if (!vfs_funcs->vfs_exist_func(_saveto)) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "cosyvoice_audio %s !NOT! exist, gen it\n", _saveto);

        if ( gen_cosyvoice_audio(_token, _appkey, _url, _voice, _text,  _saveto, vfs_funcs) != SWITCH_STATUS_SUCCESS ) {
            switch_goto_status(SWITCH_STATUS_SUCCESS, end);
        }
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "cosyvoice_audio %s exist, just play it\n", _saveto);
    }

    play_cosyvoice_audio(_saveto, _playback_id, cmd, pool);

    end:
    switch_core_destroy_memory_pool(&pool);
    return status;
}

/**
 *  定义load函数，加载时运行
 */
SWITCH_MODULE_LOAD_FUNCTION(mod_cosyvoice_load) {
    switch_api_interface_t *api_interface = nullptr;
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_cosyvoice load starting\n");

    cosyvoice_globals = (cosyvoice_global_t *)switch_core_alloc(pool, sizeof(cosyvoice_global_t));

    cosyvoice_globals->_debug = true;

    SWITCH_ADD_API(api_interface,
                   "cosyvoice_concurrent_cnt",
                   "cosyvoice_concurrent_cnt api",
                   cosyvoice_concurrent_cnt_function,
                   "<cmd><args>");

    SWITCH_ADD_API(api_interface, "cosyvoice_debug", "Set cosyvoice debug", mod_cosyvoice_debug, COSYVOICE_DEBUG_SYNTAX);

    SWITCH_ADD_API(api_interface, "uuid_cosyvoice", "Invoke CosyVoice", uuid_cosyvoice_function, "<uuid> text=XXXXX saveto=<path> token=<token> appkey=<key> url=<url> playback_id=<id> voice=<voice>");

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_cosyvoice loaded\n");

    return SWITCH_STATUS_SUCCESS;
}

/**
 *  定义shutdown函数，关闭时运行
 */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_cosyvoice_shutdown) {

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " mod_cosyvoice shutdown called\n");
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " mod_cosyvoice unload\n");
    return SWITCH_STATUS_SUCCESS;
}