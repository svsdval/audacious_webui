/*
 * Web Remote — веб-интерфейс для управления Audacious.
 *
 * General-плагин со встроенным HTTP-сервером. Сетевые потоки только читают
 * и пишут сокеты; все обращения к API Audacious выполняются в главном потоке
 * (aud_drct_* не потокобезопасны), куда запросы передаются через QueuedFunc.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <libaudcore/audstrings.h>
#include <libaudcore/drct.h>
#include <libaudcore/hook.h>
#include <libaudcore/mainloop.h>
#include <libaudcore/playlist.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>
#include <libaudcore/probe.h>
#include <libaudcore/runtime.h>
#include <libaudcore/tuple.h>

#define CFG "web_remote"
#define EXPORT __attribute__((visibility("default")))

static const char index_html[] =
#include "index_html.inc"
    ;

/* ------------------------------------------------------------------------ */
/* Плагин                                                                    */
/* ------------------------------------------------------------------------ */

static void config_changed();

class WebRemote : public GeneralPlugin
{
public:
    static const char about[];
    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {"Web Remote", "audacious-web-remote",
                                        about, &prefs};

    constexpr WebRemote() : GeneralPlugin(info, false) {}

    bool init();
    void cleanup();
};

EXPORT WebRemote aud_plugin_instance;

const char WebRemote::about[] =
    "Web Remote\n\n"
    "Управление Audacious из браузера: плейлисты, текущий трек,\n"
    "удаление треков, переключение между плейлистами.\n\n"
    "По умолчанию интерфейс доступен по адресу http://localhost:8421/";

const char * const WebRemote::defaults[] = {"port",   "8421", "remote",
                                            "FALSE",  "password", "",
                                            nullptr};

const PreferencesWidget WebRemote::widgets[] = {
    WidgetSpin("Порт:", WidgetInt(CFG, "port", config_changed),
               {1024, 65535, 1}),
    WidgetCheck("Разрешить доступ с других устройств в сети",
                WidgetBool(CFG, "remote", config_changed)),
    WidgetEntry("Пароль (пусто — без пароля):",
                WidgetString(CFG, "password", config_changed), {true}),
    WidgetLabel("Логин при входе может быть любым. Без пароля управлять\n"
                "плеером сможет любой, кому доступен этот порт.")};

const PluginPreferences WebRemote::prefs = {{widgets}};

/* ------------------------------------------------------------------------ */
/* HTTP: типы и утилиты                                                      */
/* ------------------------------------------------------------------------ */

typedef std::map<std::string, std::string> Params;

struct Request
{
    std::string method, path;
    Params params;  /* query string + тело формы */
    Params headers; /* имена в нижнем регистре */
};

struct Response
{
    int code = 200;
    std::string type = "application/json; charset=utf-8";
    std::string body;
    bool cacheable = false;
};

static Response error_response(int code, const char * message)
{
    Response r;
    r.code = code;
    r.body = std::string("{\"error\":\"") + message + "\"}";
    return r;
}

static void json_str(std::string & out, const char * s)
{
    out += '"';
    for (; s && *s; s++)
    {
        unsigned char c = *s;
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += (char)c;
        }
        else if (c < 0x20)
        {
            char buf[8];
            snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        }
        else
            out += (char)c;
    }
    out += '"';
}

static void json_key(std::string & out, const char * key)
{
    out += '"';
    out += key;
    out += "\":";
}

static void json_field(std::string & out, const char * key, const char * val)
{
    json_key(out, key);
    json_str(out, val);
    out += ',';
}

static void json_field(std::string & out, const char * key, long long val)
{
    json_key(out, key);
    out += std::to_string(val);
    out += ',';
}

static void json_field(std::string & out, const char * key, int val)
{
    json_field(out, key, (long long)val);
}

static void json_field(std::string & out, const char * key, bool val)
{
    json_key(out, key);
    out += val ? "true," : "false,";
}

/* заменяет завершающую запятую закрывающей скобкой */
static void json_close(std::string & out, char bracket)
{
    if (!out.empty() && out.back() == ',')
        out.back() = bracket;
    else
        out += bracket;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static std::string url_decode(const std::string & s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '+')
            out += ' ';
        else if (s[i] == '%' && i + 2 < s.size() &&
                 hex_val(s[i + 1]) >= 0 && hex_val(s[i + 2]) >= 0)
        {
            out += (char)(hex_val(s[i + 1]) * 16 + hex_val(s[i + 2]));
            i += 2;
        }
        else
            out += s[i];
    }
    return out;
}

static void parse_form(const std::string & s, Params & params)
{
    size_t pos = 0;
    while (pos < s.size())
    {
        size_t end = s.find('&', pos);
        if (end == std::string::npos)
            end = s.size();
        size_t eq = s.find('=', pos);
        if (eq != std::string::npos && eq < end)
            params[url_decode(s.substr(pos, eq - pos))] =
                url_decode(s.substr(eq + 1, end - eq - 1));
        else if (end > pos)
            params[url_decode(s.substr(pos, end - pos))] = "";
        pos = end + 1;
    }
}

static std::string base64_decode(const std::string & in)
{
    std::string out;
    unsigned acc = 0;
    int bits = 0;
    for (char c : in)
    {
        int v;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            v = c - '0' + 52;
        else if (c == '+')
            v = 62;
        else if (c == '/')
            v = 63;
        else
            break;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out += (char)((acc >> bits) & 0xff);
        }
    }
    return out;
}

static bool const_time_equal(const std::string & a, const std::string & b)
{
    unsigned char diff = a.size() != b.size();
    for (size_t i = 0; i < a.size() && i < b.size(); i++)
        diff |= a[i] ^ b[i];
    return !diff;
}

static const char * status_text(int code)
{
    switch (code)
    {
    case 200:
        return "OK";
    case 202:
        return "Accepted";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    default:
        return "Service Unavailable";
    }
}

/* ------------------------------------------------------------------------ */
/* Выполнение в главном потоке                                               */
/* ------------------------------------------------------------------------ */

struct Job
{
    std::function<Response()> func;
    Response result;
    bool done = false;
};

static std::mutex job_mutex;
static std::condition_variable job_cond;
static std::deque<std::shared_ptr<Job>> job_queue;
static bool jobs_stopped = true;
static QueuedFunc job_runner;

static void run_jobs()
{
    for (;;)
    {
        std::shared_ptr<Job> job;
        {
            std::lock_guard<std::mutex> lock(job_mutex);
            if (job_queue.empty())
                return;
            job = job_queue.front();
            job_queue.pop_front();
        }

        Response result = job->func();

        {
            std::lock_guard<std::mutex> lock(job_mutex);
            job->result = std::move(result);
            job->done = true;
        }
        job_cond.notify_all();
    }
}

/* вызывается из сетевых потоков; блокируется до выполнения в главном */
static Response run_in_main(std::function<Response()> func)
{
    auto job = std::make_shared<Job>();
    job->func = std::move(func);

    std::unique_lock<std::mutex> lock(job_mutex);
    if (jobs_stopped)
        return error_response(503, "shutting down");

    job_queue.push_back(job);
    job_runner.queue(run_jobs);

    job_cond.wait(lock, [&] { return job->done || jobs_stopped; });
    if (!job->done)
        return error_response(503, "shutting down");

    return std::move(job->result);
}

/* ------------------------------------------------------------------------ */
/* API (главный поток)                                                       */
/* ------------------------------------------------------------------------ */

/* serial растёт при любом изменении содержимого плейлистов (в т.ч. при
 * дочитывании тегов), structure_serial — только при изменении состава.
 * Изменения очереди счётчики не затрагивают (они приходят вместе с
 * изменениями выделения), поэтому длина очереди отдаётся отдельно. */
static std::atomic<int> serial{1};
static std::atomic<int> structure_serial{1};

static void playlist_update_cb(void * data, void *)
{
    auto level = aud::from_ptr<Playlist::UpdateLevel>(data);
    if (level >= Playlist::Metadata)
        serial++;
    if (level >= Playlist::Structure)
        structure_serial++;
}

static int int_param(const Request & req, const char * name, int fallback)
{
    auto it = req.params.find(name);
    if (it == req.params.end() || it->second.empty())
        return fallback;
    char * end;
    long val = strtol(it->second.c_str(), &end, 10);
    return *end ? fallback : (int)val;
}

static const char * str_param(const Request & req, const char * name)
{
    auto it = req.params.find(name);
    return it == req.params.end() ? "" : it->second.c_str();
}

static bool has_param(const Request & req, const char * name)
{
    return req.params.find(name) != req.params.end();
}

static std::string display_name(const char * uri)
{
    if (!uri)
        return std::string();
    StringBuf buf = uri_to_display(uri);
    const char * name = buf;
    const char * slash = strrchr(name, '/');
    return std::string(slash && slash[1] ? slash + 1 : name);
}

static std::string tuple_title(const Tuple & tuple, const char * uri)
{
    String title = tuple.get_str(Tuple::Title);
    if (title && title[0])
        return std::string(title);
    return display_name(uri);
}

static const char * safe(const String & s) { return s ? (const char *)s : ""; }

static Response api_status()
{
    std::string out = "{";

    bool playing = aud_drct_get_playing();
    bool ready = playing && aud_drct_get_ready();
    bool paused = playing && aud_drct_get_paused();

    json_field(out, "state", !playing ? "stopped" : paused ? "paused" : "playing");
    json_field(out, "volume", aud_drct_get_volume_main());
    json_field(out, "shuffle", aud_get_bool(nullptr, "shuffle"));
    json_field(out, "repeat", aud_get_bool(nullptr, "repeat"));
    json_field(out, "serial", serial.load());
    json_field(out, "sserial", structure_serial.load());
    json_field(out, "active", Playlist::active_playlist().index());
    json_field(out, "playing", Playlist::playing_playlist().index());

    if (playing)
    {
        Playlist list = Playlist::playing_playlist();
        int pos = list.get_position();
        String file = aud_drct_get_filename();
        Tuple tuple = ready ? aud_drct_get_tuple()
                            : list.entry_tuple(pos, Playlist::NoWait);

        json_key(out, "track");
        out += '{';
        json_field(out, "title", tuple_title(tuple, file).c_str());
        json_field(out, "artist", safe(tuple.get_str(Tuple::Artist)));
        json_field(out, "album", safe(tuple.get_str(Tuple::Album)));
        json_field(out, "codec", safe(tuple.get_str(Tuple::Codec)));
        json_field(out, "file", file ? (const char *)uri_to_display(file) : "");
        json_field(out, "entry", pos);
        json_field(out, "time", ready ? aud_drct_get_time() : 0);
        json_field(out, "length", ready ? aud_drct_get_length() : 0);
        if (ready)
        {
            int bitrate = 0, samplerate = 0, channels = 0;
            aud_drct_get_info(bitrate, samplerate, channels);
            json_field(out, "bitrate", bitrate / 1000);
            json_field(out, "samplerate", samplerate);
        }
        json_close(out, '}');
        out += ',';
    }

    json_key(out, "playlists");
    out += '[';
    int n = Playlist::n_playlists();
    for (int i = 0; i < n; i++)
    {
        Playlist list = Playlist::by_index(i);
        out += '{';
        json_field(out, "title", safe(list.get_title()));
        json_field(out, "entries", list.n_entries());
        json_field(out, "position", list.get_position());
        json_field(out, "queued", list.n_queued());
        json_close(out, '}');
        out += ',';
    }
    json_close(out, ']');
    out += '}';

    Response r;
    r.body = std::move(out);
    return r;
}

static bool get_playlist(const Request & req, Playlist & list)
{
    int index = int_param(req, "pl", -1);
    if (index < 0 || index >= Playlist::n_playlists())
        return false;
    list = Playlist::by_index(index);
    return true;
}

static Response api_playlist(const Request & req)
{
    Playlist list;
    if (!get_playlist(req, list))
        return error_response(404, "no such playlist");

    /* счётчики читаются до содержимого: если плейлист изменится позже,
     * клиент увидит новый serial и перечитает список */
    int cur_serial = serial.load();
    int cur_sserial = structure_serial.load();

    std::string out = "{";
    json_field(out, "index", list.index());
    json_field(out, "title", safe(list.get_title()));
    json_field(out, "position", list.get_position());
    json_field(out, "serial", cur_serial);
    json_field(out, "sserial", cur_sserial);
    json_field(out, "total_ms", (long long)list.total_length_ms());
    json_field(out, "queued", list.n_queued());

    /* запись: [название, исполнитель, альбом, длина в мс, место в очереди] */
    json_key(out, "entries");
    out += '[';
    int n = list.n_entries();
    bool have_queue = list.n_queued() > 0;
    for (int i = 0; i < n; i++)
    {
        Tuple tuple = list.entry_tuple(i, Playlist::NoWait);
        String title = tuple.get_str(Tuple::Title);

        out += '[';
        if (title && title[0])
            json_str(out, title);
        else
            json_str(out, display_name(list.entry_filename(i)).c_str());
        out += ',';
        json_str(out, safe(tuple.get_str(Tuple::Artist)));
        out += ',';
        json_str(out, safe(tuple.get_str(Tuple::Album)));
        out += ',';
        int length = tuple.get_int(Tuple::Length);
        out += std::to_string(length > 0 ? length : 0);
        out += ',';
        out += std::to_string(have_queue ? list.queue_find_entry(i) + 1 : 0);
        out += "],";
    }
    json_close(out, ']');
    out += '}';

    Response r;
    r.body = std::move(out);
    return r;
}

static Response api_art()
{
    Response r;
    String file = aud_drct_get_playing() ? aud_drct_get_filename() : String();
    if (!file)
        return error_response(404, "nothing playing");

    bool queued = false;
    AudArtPtr art = aud_art_request(file, AUD_ART_DATA, &queued);
    const Index<char> * data = art.data();
    if (!data || !data->len())
        return error_response(queued ? 202 : 404, queued ? "loading" : "no art");

    r.body.assign(data->begin(), data->len());

    const unsigned char * b = (const unsigned char *)r.body.data();
    size_t len = r.body.size();
    if (len > 3 && b[0] == 0xff && b[1] == 0xd8)
        r.type = "image/jpeg";
    else if (len > 8 && !memcmp(b, "\x89PNG", 4))
        r.type = "image/png";
    else if (len > 6 && !memcmp(b, "GIF8", 4))
        r.type = "image/gif";
    else if (len > 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4))
        r.type = "image/webp";
    else if (len > 2 && b[0] == 'B' && b[1] == 'M')
        r.type = "image/bmp";
    else
        r.type = "application/octet-stream";

    r.cacheable = true; /* адрес картинки содержит имя файла трека */
    return r;
}

/* Удаляет запись. Если удалён играющий трек, а Audacious из-за этого
 * остановил воспроизведение (настройка «переходить к следующему при
 * удалении» выключена), продолжает со следующего трека. */
static void remove_entry(Playlist list, int entry)
{
    bool was_current = aud_drct_get_playing() &&
                       list == Playlist::playing_playlist() &&
                       list.get_position() == entry;
    bool was_paused = was_current && aud_drct_get_paused();

    list.remove_entry(entry);

    if (!was_current || list.get_position() >= 0)
        return;

    int n = list.n_entries();
    if (!n || (entry >= n && !aud_get_bool(nullptr, "repeat")))
        return;

    list.set_position(entry < n ? entry : 0);
    list.start_playback(was_paused);
}

static Response ok_response()
{
    Response r;
    r.body = "{\"ok\":true}";
    return r;
}

static Response api_command(const std::string & action, const Request & req)
{
    /* --- управление воспроизведением --- */

    if (action == "play")
        aud_drct_play();
    else if (action == "pause")
        aud_drct_pause();
    else if (action == "playpause")
        aud_drct_play_pause();
    else if (action == "stop")
        aud_drct_stop();
    else if (action == "next")
        aud_drct_pl_next();
    else if (action == "prev")
        aud_drct_pl_prev();
    else if (action == "seek")
    {
        int time = int_param(req, "time", -1);
        if (time < 0)
            return error_response(400, "bad time");
        if (aud_drct_get_playing() && aud_drct_get_ready())
            aud_drct_seek(time);
    }
    else if (action == "volume")
    {
        int volume = int_param(req, "value", -1);
        if (volume < 0 || volume > 100)
            return error_response(400, "bad volume");
        aud_drct_set_volume_main(volume);
    }
    else if (action == "shuffle" || action == "repeat")
    {
        if (has_param(req, "value"))
            aud_set_bool(nullptr, action.c_str(), int_param(req, "value", 0));
        else
            aud_toggle_bool(nullptr, action.c_str());
    }
    else if (action == "remove_current")
    {
        Playlist list = Playlist::playing_playlist();
        if (!list.exists())
            return error_response(409, "nothing playing");
        int pos = list.get_position();
        /* защита от повторного нажатия: клиент сообщает, какой трек
         * он считал текущим */
        if (has_param(req, "entry") && int_param(req, "entry", -1) != pos)
            return error_response(409, "track changed");
        if (pos < 0)
            return error_response(409, "nothing playing");
        remove_entry(list, pos);
    }

    /* --- плейлисты --- */

    else if (action == "playlist_new")
    {
        Playlist list = Playlist::insert_playlist(-1);
        const char * title = str_param(req, "title");
        if (title[0])
            list.set_title(title);
        list.activate();

        Response r;
        r.body = "{\"ok\":true,\"index\":" + std::to_string(list.index()) + "}";
        return r;
    }
    else
    {
        /* остальные команды адресуют плейлист (и запись) по номеру, поэтому
         * отклоняются, если состав плейлистов изменился после того, как
         * клиент получил список */
        if (has_param(req, "sserial") &&
            int_param(req, "sserial", 0) != structure_serial.load())
            return error_response(409, "playlist changed");

        Playlist list;
        if (!get_playlist(req, list))
            return error_response(404, "no such playlist");

        int entry = int_param(req, "entry", -1);
        bool entry_ok = entry >= 0 && entry < list.n_entries();

        if (action == "activate")
            list.activate();
        else if (action == "playlist_play")
        {
            list.activate();
            list.start_playback();
        }
        else if (action == "playlist_rename")
        {
            const char * title = str_param(req, "title");
            if (!title[0])
                return error_response(400, "empty title");
            list.set_title(title);
        }
        else if (action == "playlist_delete")
            list.remove_playlist();
        else if (action == "playlist_clear")
            list.remove_all_entries();
        else if (action == "add")
        {
            const char * uri = str_param(req, "uri");
            if (!uri[0])
                return error_response(400, "empty uri");
            if (uri[0] == '/')
                list.insert_entry(-1, filename_to_uri(uri), Tuple(), false);
            else if (strstr(uri, "://"))
                list.insert_entry(-1, uri, Tuple(), false);
            else
                return error_response(400, "bad uri");
        }
        else if (action == "jump")
        {
            if (!entry_ok)
                return error_response(404, "no such entry");
            list.set_position(entry);
            list.start_playback(); /* заодно снимает паузу */
        }
        else if (action == "remove")
        {
            if (!entry_ok)
                return error_response(404, "no such entry");
            remove_entry(list, entry);
        }
        else if (action == "queue")
        {
            if (!entry_ok)
                return error_response(404, "no such entry");
            int qpos = list.queue_find_entry(entry);
            if (qpos >= 0)
                list.queue_remove(qpos);
            else
                list.queue_insert(-1, entry);
        }
        else
            return error_response(404, "unknown command");
    }

    return ok_response();
}

static Response handle_api(const Request & req)
{
    std::string action = req.path.substr(5); /* после "/api/" */

    if (req.method == "GET")
    {
        if (action == "status")
            return api_status();
        if (action == "playlist")
            return api_playlist(req);
        if (action == "art")
            return api_art();
        return error_response(404, "not found");
    }

    return api_command(action, req);
}

/* ------------------------------------------------------------------------ */
/* HTTP-сервер (сетевые потоки)                                              */
/* ------------------------------------------------------------------------ */

static const size_t MAX_HEADER = 16 * 1024;
static const size_t MAX_BODY = 64 * 1024;

struct Connection
{
    std::thread thread;
    int fd = -1;
    std::atomic<bool> finished{false};
};

static std::thread accept_thread;
static int listen_fd = -1;
static int wake_pipe[2] = {-1, -1};

static std::mutex conn_mutex; /* защищает conns и fd внутри них */
static std::list<std::unique_ptr<Connection>> conns;

static std::mutex cfg_mutex;
static std::string cfg_password;
static int running_port = 0;
static bool running_remote = false;

static bool send_all(int fd, const char * data, size_t len)
{
    while (len)
    {
        ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            return false;
        }
        data += n;
        len -= n;
    }
    return true;
}

static void send_response(int fd, const Response & r, bool head_only)
{
    std::string head = "HTTP/1.1 " + std::to_string(r.code) + " " +
                       status_text(r.code) + "\r\n";
    head += "Content-Type: " + r.type + "\r\n";
    head += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    head += r.cacheable ? "Cache-Control: private, max-age=3600\r\n"
                        : "Cache-Control: no-store\r\n";
    if (r.code == 401)
        head += "WWW-Authenticate: Basic realm=\"Audacious\", charset=\"UTF-8\"\r\n";
    head += "X-Content-Type-Options: nosniff\r\n"
            "Connection: close\r\n\r\n";

    if (send_all(fd, head.data(), head.size()) && !head_only)
        send_all(fd, r.body.data(), r.body.size());
}

/* читает и разбирает запрос; при ошибке возвращает код HTTP, иначе 0 */
static int read_request(int fd, Request & req)
{
    std::string buf;
    size_t header_end;
    char chunk[4096];

    while ((header_end = buf.find("\r\n\r\n")) == std::string::npos)
    {
        if (buf.size() > MAX_HEADER)
            return 413;
        ssize_t n = recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0)
            return -1; /* клиент ничего не прислал — молча закрываем */
        buf.append(chunk, n);
    }

    size_t line_end = buf.find("\r\n");
    std::string line = buf.substr(0, line_end);
    size_t sp1 = line.find(' ');
    size_t sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp2 <= sp1)
        return 400;

    req.method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t q = target.find('?');
    req.path = target.substr(0, q);
    if (q != std::string::npos)
        parse_form(target.substr(q + 1), req.params);

    size_t pos = line_end + 2;
    while (pos < header_end)
    {
        size_t end = buf.find("\r\n", pos);
        size_t colon = buf.find(':', pos);
        if (colon != std::string::npos && colon < end)
        {
            std::string name = buf.substr(pos, colon - pos);
            for (char & c : name)
                if (c >= 'A' && c <= 'Z')
                    c += 'a' - 'A';
            size_t vstart = buf.find_first_not_of(" \t", colon + 1);
            req.headers[name] =
                vstart < end ? buf.substr(vstart, end - vstart) : "";
        }
        pos = end + 2;
    }

    size_t body_len = 0;
    auto it = req.headers.find("content-length");
    if (it != req.headers.end())
    {
        char * end;
        unsigned long long len = strtoull(it->second.c_str(), &end, 10);
        if (*end || len > MAX_BODY)
            return 413;
        body_len = len;
    }

    std::string body = buf.substr(header_end + 4);
    while (body.size() < body_len)
    {
        ssize_t n = recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0)
            return -1;
        body.append(chunk, n);
    }
    body.resize(body_len);

    if (body_len)
        parse_form(body, req.params);

    return 0;
}

static bool check_auth(const Request & req)
{
    std::string password;
    {
        std::lock_guard<std::mutex> lock(cfg_mutex);
        password = cfg_password;
    }
    if (password.empty())
        return true;

    auto it = req.headers.find("authorization");
    if (it == req.headers.end() || it->second.compare(0, 6, "Basic "))
        return false;

    std::string cred = base64_decode(it->second.substr(6));
    size_t colon = cred.find(':');
    if (colon == std::string::npos)
        return false;
    return const_time_equal(cred.substr(colon + 1), password);
}

/* Запросы, меняющие состояние, принимаются только со страницы самого
 * плагина: чужой сайт, открытый в том же браузере, не должен управлять
 * плеером (CSRF). */
static bool check_origin(const Request & req)
{
    auto origin = req.headers.find("origin");
    if (origin == req.headers.end())
        return true; /* не браузер (curl и т.п.) */

    auto host = req.headers.find("host");
    if (host == req.headers.end())
        return false;

    const std::string & o = origin->second;
    size_t scheme = o.find("://");
    return scheme != std::string::npos && o.substr(scheme + 3) == host->second;
}

static Response handle_request(const Request & req)
{
    if (!check_auth(req))
        return error_response(401, "unauthorized");

    bool is_get = req.method == "GET" || req.method == "HEAD";
    if (!is_get && req.method != "POST")
        return error_response(405, "method not allowed");

    if (req.path == "/" || req.path == "/index.html")
    {
        if (!is_get)
            return error_response(405, "method not allowed");
        Response r;
        r.type = "text/html; charset=utf-8";
        r.body.assign(index_html, sizeof index_html - 1);
        return r;
    }

    if (req.path.compare(0, 5, "/api/"))
        return error_response(404, "not found");

    if (!is_get && !check_origin(req))
        return error_response(403, "cross-origin request rejected");

    return run_in_main([req] { return handle_api(req); });
}

static void serve_connection(Connection * conn)
{
    int fd = conn->fd;

    struct timeval tv = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    Request req;
    int err = read_request(fd, req);
    if (err > 0)
        send_response(fd, error_response(err, "bad request"), false);
    else if (!err)
        send_response(fd, handle_request(req), req.method == "HEAD");

    {
        std::lock_guard<std::mutex> lock(conn_mutex);
        shutdown(fd, SHUT_RDWR);
        close(fd);
        conn->fd = -1;
    }
    conn->finished = true;
}

static void reap_connections(bool all)
{
    std::list<std::unique_ptr<Connection>> done;
    {
        std::lock_guard<std::mutex> lock(conn_mutex);
        for (auto it = conns.begin(); it != conns.end();)
        {
            auto next = std::next(it);
            if (all || (*it)->finished)
                done.splice(done.end(), conns, it);
            it = next;
        }
    }
    for (auto & conn : done)
        conn->thread.join();
}

static void accept_loop()
{
    for (;;)
    {
        struct pollfd fds[2] = {{listen_fd, POLLIN, 0}, {wake_pipe[0], POLLIN, 0}};
        if (poll(fds, 2, 5000) < 0 && errno != EINTR)
            break;
        if (fds[1].revents)
            break;

        reap_connections(false);

        if (!(fds[0].revents & POLLIN))
            continue;

        int fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0)
            continue;

        std::lock_guard<std::mutex> lock(conn_mutex);
        if (conns.size() >= 64)
        {
            close(fd);
            continue;
        }
        conns.emplace_back(new Connection);
        Connection * conn = conns.back().get();
        conn->fd = fd;
        conn->thread = std::thread(serve_connection, conn);
    }
}

static bool server_start()
{
    int port = aud_get_int(CFG, "port");
    bool remote = aud_get_bool(CFG, "remote");
    {
        std::lock_guard<std::mutex> lock(cfg_mutex);
        cfg_password = (const char *)aud_get_str(CFG, "password");
    }

    listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0)
    {
        AUDERR("socket: %s\n", strerror(errno));
        return false;
    }

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(remote ? INADDR_ANY : INADDR_LOOPBACK);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        listen(listen_fd, 16) < 0 || pipe2(wake_pipe, O_CLOEXEC) < 0)
    {
        AUDERR("Не удалось открыть порт %d: %s\n", port, strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(job_mutex);
        jobs_stopped = false;
    }

    running_port = port;
    running_remote = remote;
    accept_thread = std::thread(accept_loop);

    AUDINFO("Web Remote: http://%s:%d/\n", remote ? "0.0.0.0" : "127.0.0.1",
            port);
    return true;
}

static void server_stop()
{
    if (listen_fd < 0)
        return;

    /* освобождаем потоки, ждущие главный поток, иначе join() ниже
     * зависнет: главный поток — это мы */
    {
        std::lock_guard<std::mutex> lock(job_mutex);
        jobs_stopped = true;
        job_queue.clear();
    }
    job_cond.notify_all();
    job_runner.stop();

    if (write(wake_pipe[1], "x", 1) < 0)
        AUDERR("write: %s\n", strerror(errno));
    accept_thread.join();

    {
        std::lock_guard<std::mutex> lock(conn_mutex);
        for (auto & conn : conns)
            if (conn->fd >= 0)
                shutdown(conn->fd, SHUT_RDWR);
    }
    reap_connections(true);

    close(listen_fd);
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    listen_fd = wake_pipe[0] = wake_pipe[1] = -1;
}

static void config_changed()
{
    {
        std::lock_guard<std::mutex> lock(cfg_mutex);
        cfg_password = (const char *)aud_get_str(CFG, "password");
    }

    if (listen_fd >= 0 && aud_get_int(CFG, "port") == running_port &&
        aud_get_bool(CFG, "remote") == running_remote)
        return;

    server_stop();
    server_start();
}

/* ------------------------------------------------------------------------ */

bool WebRemote::init()
{
    aud_config_set_defaults(CFG, defaults);
    hook_associate("playlist update", playlist_update_cb, nullptr);

    /* Если порт занят, плагин всё равно включается, чтобы порт можно было
     * сменить в настройках. */
    server_start();
    return true;
}

void WebRemote::cleanup()
{
    server_stop();
    hook_dissociate("playlist update", playlist_update_cb);
}
