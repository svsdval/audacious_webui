/*
 * Web Remote — веб-интерфейс для управления Audacious.
 *
 * General-плагин со встроенным HTTP-сервером. Сетевые потоки только читают
 * и пишут сокеты; все обращения к API Audacious выполняются в главном потоке
 * (aud_drct_* не потокобезопасны), куда запросы передаются через QueuedFunc.
 */

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <libaudcore/audstrings.h>
#include <libaudcore/drct.h>
#include <libaudcore/equalizer.h>
#include <libaudcore/hook.h>
#include <libaudcore/mainloop.h>
#include <libaudcore/playlist.h>
#include <libaudcore/plugin.h>
#include <libaudcore/plugins.h>
#include <libaudcore/preferences.h>
#include <libaudcore/probe.h>
#include <libaudcore/runtime.h>
#include <libaudcore/tuple.h>

#include "webremote-pcm.h"

#define CFG "web_remote"
#define EXPORT __attribute__((visibility("default")))

static const char index_html[] =
#include "index_html.inc"
    ;

/* ------------------------------------------------------------------------ */
/* Плагин                                                                    */
/* ------------------------------------------------------------------------ */

static void config_changed();
static void stream_config_changed();

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
                                            "stream", "FALSE", "stream_bitrate",
                                            "192",    "upload", "TRUE",
                                            "download", "TRUE",
                                            "upload_dir", "", "upload_exts", "",
                                            nullptr};

const PreferencesWidget WebRemote::widgets[] = {
    WidgetSpin("Порт:", WidgetInt(CFG, "port", config_changed),
               {1024, 65535, 1}),
    WidgetCheck("Разрешить доступ с других устройств в сети",
                WidgetBool(CFG, "remote", config_changed)),
    WidgetEntry("Пароль (пусто — без пароля):",
                WidgetString(CFG, "password", config_changed), {true}),
    WidgetLabel("Логин при входе может быть любым. Без пароля управлять\n"
                "плеером сможет любой, кому доступен этот порт."),
    WidgetCheck("Трансляция звука в браузер (MP3)",
                WidgetBool(CFG, "stream", stream_config_changed)),
    WidgetSpin("Битрейт:", WidgetInt(CFG, "stream_bitrate"),
               {64, 320, 32, "кбит/с"}, WIDGET_CHILD),
    WidgetLabel("На странице появится кнопка «Слушать здесь». Поток также\n"
                "доступен плеерам по адресу /stream.mp3. Новый битрейт\n"
                "применяется со следующего трека."),
    WidgetCheck("Разрешить скачивание треков со страницы",
                WidgetBool(CFG, "download")),
    WidgetCheck("Разрешить загрузку файлов со страницы",
                WidgetBool(CFG, "upload")),
    WidgetFileEntry("Папка для загруженных файлов:",
                    WidgetString(CFG, "upload_dir"), {FileSelectMode::Folder},
                    WIDGET_CHILD),
    WidgetEntry("Разрешённые расширения:", WidgetString(CFG, "upload_exts"),
                {false}, WIDGET_CHILD),
    WidgetLabel("Если папка не выбрана, файлы сохраняются в\n"
                "~/Music/Audacious Web. Расширения перечисляются через\n"
                "пробел или запятую (mp3, flac, xm); пустая строка — все\n"
                "форматы, которые умеют включённые модули ввода Audacious.")};

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
    size_t body_len = 0; /* только для загрузки файла: тело ещё не прочитано */
    bool http10 = false;
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
    case 415:
        return "Unsupported Media Type";
    case 500:
        return "Internal Server Error";
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
static std::atomic<bool> stream_running{false};
static bool stream_available();
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

/* --- отмена удаления --- */

/* Удалённые записи запоминаются вместе с тегами, чтобы их можно было
 * вернуть на прежнее место. Используется только в главном потоке. */
struct UndoItem
{
    Playlist list;
    int at;
    Index<PlaylistAddItem> items;
    std::string title; /* что показать на кнопке «Вернуть» */
};

static const size_t UNDO_MAX = 30;
static std::deque<UndoItem> undo_stack;

/* вызывается перед удалением <count> записей, начиная с <at> */
static void undo_remember(Playlist list, int at, int count)
{
    if (count <= 0)
        return;

    UndoItem item;
    item.list = list;
    item.at = at;
    for (int i = at; i < at + count; i++)
        item.items.append(list.entry_filename(i),
                          list.entry_tuple(i, Playlist::NoWait), nullptr);

    if (count == 1)
        item.title = tuple_title(item.items[0].tuple, item.items[0].filename);
    else
        item.title = std::string("весь плейлист «") + safe(list.get_title()) +
                     "» (" + std::to_string(count) + ")";

    undo_stack.push_back(std::move(item));
    if (undo_stack.size() > UNDO_MAX)
        undo_stack.pop_front();
}

static Response api_undo()
{
    /* плейлист могли удалить — возвращать уже некуда */
    while (!undo_stack.empty() && !undo_stack.back().list.exists())
        undo_stack.pop_back();
    if (undo_stack.empty())
        return error_response(409, "nothing to undo");

    UndoItem item = std::move(undo_stack.back());
    undo_stack.pop_back();

    int count = item.items.len();
    int at = aud::min(item.at, item.list.n_entries());
    item.list.insert_items(at, std::move(item.items), false);

    Response r;
    r.body = "{\"ok\":true,\"pl\":" + std::to_string(item.list.index()) +
             ",\"entry\":" + std::to_string(at) +
             ",\"count\":" + std::to_string(count) + "}";
    return r;
}

static void eq_json(std::string & out, double preamp, const float * bands);

static Response api_status()
{
    std::string out = "{";

    bool playing = aud_drct_get_playing();
    bool ready = playing && aud_drct_get_ready();
    bool paused = playing && aud_drct_get_paused();

    json_field(out, "state", !playing ? "stopped" : paused ? "paused" : "playing");
    json_field(out, "volume", aud_drct_get_volume_main());
    json_field(out, "shuffle", aud_get_bool(nullptr, "shuffle"));
    /* «Повтор трека» в Audacious — это сочетание двух настроек: «повтор» и
     * «не переходить к следующему». Странице отдаются два раздельных режима. */
    bool repeat = aud_get_bool(nullptr, "repeat");
    bool no_advance = aud_get_bool(nullptr, "no_playlist_advance");
    json_field(out, "repeat", repeat && !no_advance);
    json_field(out, "repeat_one", repeat && no_advance);
    json_field(out, "serial", serial.load());
    json_field(out, "sserial", structure_serial.load());
    json_field(out, "active", Playlist::active_playlist().index());
    json_field(out, "playing", Playlist::playing_playlist().index());
    json_field(out, "stream", stream_available());
    json_field(out, "upload", aud_get_bool(CFG, "upload"));
    json_field(out, "download", aud_get_bool(CFG, "download"));
    json_key(out, "eq");
    out += '{';
    json_field(out, "active", aud_get_bool(nullptr, "equalizer_active"));
    eq_json(out, aud_get_double(nullptr, "equalizer_preamp"), nullptr);
    json_close(out, '}');
    out += ',';
    json_field(out, "undo", (int)undo_stack.size());
    if (!undo_stack.empty())
        json_field(out, "undo_title", undo_stack.back().title.c_str());

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

/* --- эквалайзер --- */

/* Усиление передаётся целым числом десятых долей дБ (-35 = -3,5 дБ):
 * печать и разбор дробных чисел зависят от локали (в русской — запятая),
 * а JSON и адрес запроса требуют точку. */
static int eq_tenths(double db) { return (int)lround(db * 10); }

static double eq_db(int tenths)
{
    return aud::clamp(tenths, -AUD_EQ_MAX_GAIN * 10, AUD_EQ_MAX_GAIN * 10) / 10.0;
}

/* дописывает "preamp":…,"bands":[…]; bands == nullptr — текущие полосы */
static void eq_json(std::string & out, double preamp, const float * bands)
{
    double current[AUD_EQ_NBANDS];
    if (!bands)
        aud_eq_get_bands(current);

    json_field(out, "preamp", eq_tenths(preamp));
    json_key(out, "bands");
    out += '[';
    for (int i = 0; i < AUD_EQ_NBANDS; i++)
    {
        out += std::to_string(eq_tenths(bands ? bands[i] : current[i]));
        out += ',';
    }
    json_close(out, ']');
    out += ',';
}

/* предустановки, сохранённые пользователем в самом Audacious */
static Response api_eq_presets()
{
    std::string out = "{\"presets\":[";
    for (const EqualizerPreset & preset : aud_eq_read_presets("eq.preset"))
    {
        out += '{';
        json_field(out, "name", safe(preset.name));
        eq_json(out, preset.preamp, preset.bands);
        json_close(out, '}');
        out += ',';
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

    undo_remember(list, entry, 1);
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
    else if (action == "shuffle")
    {
        if (has_param(req, "value"))
            aud_set_bool(nullptr, "shuffle", int_param(req, "value", 0));
        else
            aud_toggle_bool(nullptr, "shuffle");
    }
    else if (action == "repeat" || action == "repeat_one")
    {
        /* два режима повтора взаимно исключают друг друга */
        bool one = action == "repeat_one";
        bool repeat = aud_get_bool(nullptr, "repeat");
        bool no_advance = aud_get_bool(nullptr, "no_playlist_advance");
        bool current = repeat && (one ? no_advance : !no_advance);
        bool enable = has_param(req, "value") ? int_param(req, "value", 0) != 0
                                              : !current;

        if (one && enable && !current)
            /* запоминаем повтор плейлиста, чтобы вернуть его при выключении */
            aud_set_bool(CFG, "repeat_before_one", repeat && !no_advance);

        if (enable)
        {
            aud_set_bool(nullptr, "repeat", true);
            aud_set_bool(nullptr, "no_playlist_advance", one);
        }
        else if (current)
        {
            aud_set_bool(nullptr, "no_playlist_advance", false);
            aud_set_bool(nullptr, "repeat",
                         one && aud_get_bool(CFG, "repeat_before_one"));
        }
    }
    else if (action == "eq")
    {
        /* можно передать любое сочетание: active, preamp, bands (все десять
         * через запятую) или band + value (одна полоса) */
        if (has_param(req, "active"))
            aud_set_bool(nullptr, "equalizer_active", int_param(req, "active", 0));
        if (has_param(req, "preamp"))
            aud_set_double(nullptr, "equalizer_preamp",
                           eq_db(int_param(req, "preamp", 0)));
        if (has_param(req, "bands"))
        {
            double bands[AUD_EQ_NBANDS];
            const char * p = str_param(req, "bands");
            for (int i = 0; i < AUD_EQ_NBANDS; i++)
            {
                char * end;
                long value = strtol(p, &end, 10);
                if (end == p || (*end != ',' && *end) ||
                    (!*end && i < AUD_EQ_NBANDS - 1))
                    return error_response(400, "bad bands");
                bands[i] = eq_db(value);
                p = *end ? end + 1 : end;
            }
            aud_eq_set_bands(bands);
        }
        if (has_param(req, "band"))
        {
            int band = int_param(req, "band", -1);
            if (band < 0 || band >= AUD_EQ_NBANDS || !has_param(req, "value"))
                return error_response(400, "bad band");
            aud_eq_set_band(band, eq_db(int_param(req, "value", 0)));
        }
    }
    else if (action == "undo")
        return api_undo();
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
        {
            undo_remember(list, 0, list.n_entries());
            list.remove_all_entries();
        }
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

/* --- форматы, разрешённые для загрузки --- */

static std::mutex ext_mutex;
static std::vector<std::string> ext_cache; /* в нижнем регистре, по алфавиту */

/* Обновляет список расширений: либо заданный в настройках, либо всё, что
 * зарегистрировали включённые модули ввода Audacious. Главный поток. */
static void refresh_exts()
{
    std::vector<std::string> exts;
    auto add = [&exts](std::string ext) {
        for (char & c : ext)
            if (c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
        if (!ext.empty() && ext.find('/') == std::string::npos)
            exts.push_back(ext);
    };

    String custom = aud_get_str(CFG, "upload_exts");
    std::string list = custom ? (const char *)custom : "";

    if (list.find_first_not_of(" ,;.") != std::string::npos)
    {
        size_t pos = 0;
        while ((pos = list.find_first_not_of(" ,;.", pos)) != std::string::npos)
        {
            size_t end = list.find_first_of(" ,;", pos);
            if (end == std::string::npos)
                end = list.size();
            add(list.substr(pos, end - pos));
            pos = end;
        }
    }
    else
    {
        for (PluginHandle * handle : aud_plugin_list(PluginType::Input))
        {
            if (!aud_plugin_get_enabled(handle))
                continue;
            auto plugin = (const InputPlugin *)aud_plugin_get_header(handle);
            if (!plugin)
                continue;
            auto ext = plugin->input_info.keys[InputKey::Ext];
            for (; ext && *ext; ext++)
                add(*ext);
        }
    }

    std::sort(exts.begin(), exts.end());
    exts.erase(std::unique(exts.begin(), exts.end()), exts.end());

    std::lock_guard<std::mutex> lock(ext_mutex);
    ext_cache.swap(exts);
}

static Response api_formats()
{
    refresh_exts();

    std::string out = "{\"exts\":[";
    {
        std::lock_guard<std::mutex> lock(ext_mutex);
        for (const std::string & ext : ext_cache)
        {
            json_str(out, ext.c_str());
            out += ',';
        }
    }
    json_close(out, ']');
    out += '}';

    Response r;
    r.body = std::move(out);
    return r;
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
        if (action == "eq_presets")
            return api_eq_presets();
        if (action == "formats")
            return api_formats();
        return error_response(404, "not found");
    }

    return api_command(action, req);
}

/* ------------------------------------------------------------------------ */
/* HTTP-сервер (сетевые потоки)                                              */
/* ------------------------------------------------------------------------ */

static const size_t MAX_HEADER = 16 * 1024;
static const size_t MAX_BODY = 64 * 1024;
static const char UPLOAD_PATH[] = "/api/upload";
static const unsigned long long UPLOAD_MAX = 2ULL * 1024 * 1024 * 1024;
static const size_t MAX_CONNECTIONS = 200; /* браузер держит до 6 на вкладку */

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

/* ------------------------------------------------------------------------ */
/* Трансляция звука в MP3                                                    */
/* ------------------------------------------------------------------------ */

/* Звук приходит из потока воспроизведения (hook от эффект-плагина), в
 * отдельном потоке кодируется LAME и раздаётся всем подключённым слушателям.
 * Пока слушателей нет, ничего не копируется и не кодируется. Когда плеер
 * стоит или на паузе, в поток идёт тишина, чтобы браузер не разрывал
 * соединение.
 *
 * Формат потока постоянный (44,1 кГц, стерео, один битрейт), иначе браузеры
 * спотыкаются на границе треков с разной частотой; пересчёт частоты
 * выполняет сам LAME. */

/* libmp3lame подгружается во время работы: заголовки для сборки не нужны,
 * а без библиотеки плагин работает, просто без трансляции. */
struct lame_global_flags;

static struct
{
    void * lib;
    lame_global_flags * (*init)();
    int (*set_in_samplerate)(lame_global_flags *, int);
    int (*set_out_samplerate)(lame_global_flags *, int);
    int (*set_num_channels)(lame_global_flags *, int);
    int (*set_brate)(lame_global_flags *, int);
    int (*set_quality)(lame_global_flags *, int);
    int (*set_bWriteVbrTag)(lame_global_flags *, int);
    int (*set_disable_reservoir)(lame_global_flags *, int);
    int (*init_params)(lame_global_flags *);
    int (*encode)(lame_global_flags *, const float *, int, unsigned char *, int);
    int (*flush)(lame_global_flags *, unsigned char *, int);
    int (*close)(lame_global_flags *);
} lame;

static bool lame_load()
{
    if (lame.lib)
        return true;

    void * lib = dlopen("libmp3lame.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
    {
        AUDERR("Трансляция недоступна: %s\n", dlerror());
        return false;
    }

    bool ok = true;
    auto sym = [&](const char * name) {
        void * p = dlsym(lib, name);
        if (!p)
        {
            AUDERR("В libmp3lame нет функции %s\n", name);
            ok = false;
        }
        return p;
    };

#define LAME_SYM(field, name) lame.field = (decltype(lame.field))sym(name)
    LAME_SYM(init, "lame_init");
    LAME_SYM(set_in_samplerate, "lame_set_in_samplerate");
    LAME_SYM(set_out_samplerate, "lame_set_out_samplerate");
    LAME_SYM(set_num_channels, "lame_set_num_channels");
    LAME_SYM(set_brate, "lame_set_brate");
    LAME_SYM(set_quality, "lame_set_quality");
    LAME_SYM(set_bWriteVbrTag, "lame_set_bWriteVbrTag");
    LAME_SYM(set_disable_reservoir, "lame_set_disable_reservoir");
    LAME_SYM(init_params, "lame_init_params");
    LAME_SYM(encode, "lame_encode_buffer_interleaved_ieee_float");
    LAME_SYM(flush, "lame_encode_flush");
    LAME_SYM(close, "lame_close");
#undef LAME_SYM

    if (!ok)
    {
        dlclose(lib);
        return false;
    }

    lame.lib = lib;
    return true;
}

static const int STREAM_RATE = 44100;
static const size_t STREAM_CLIENT_MAX = 1024 * 1024; /* отставший слушатель */
static const int STREAM_TICK_MS = 20;
static const long long STREAM_CUSHION_US = 100000;    /* запас от заиканий */
static const long long STREAM_QUEUE_MAX_US = 15000000;

struct PcmSegment
{
    int rate;
    std::vector<float> data; /* стерео */
};

struct StreamClient
{
    std::string buf;
    bool dropped = false;
};

static std::mutex st_mutex;
static std::condition_variable st_enc_cond;    /* будит кодировщик */
static std::condition_variable st_client_cond; /* будит слушателей */
static std::deque<PcmSegment> st_segments;
static size_t st_front_pos = 0;   /* сколько кадров первого сегмента выдано */
static long long st_queued_us = 0; /* длительность звука в очереди */
static std::list<StreamClient *> st_clients;
static std::atomic<int> st_n_clients{0};
static bool st_quit = false;
static std::thread st_thread;
static std::atomic<bool> server_closing{false};

/* вызывается из потока воспроизведения */
static void stream_pcm_cb(void * data, void *)
{
    if (!st_n_clients.load(std::memory_order_relaxed))
        return;

    auto pcm = (const WebRemotePcm *)data;
    int ch = pcm->channels;
    int frames = pcm->samples / ch;
    if (frames <= 0 || pcm->rate <= 0)
        return;

    std::lock_guard<std::mutex> lock(st_mutex);

    /* очередь не должна расти без конца — лучше пропуск, чем рост памяти */
    if (st_queued_us > STREAM_QUEUE_MAX_US)
        return;

    if (st_segments.empty() || st_segments.back().rate != pcm->rate)
        st_segments.push_back({pcm->rate, {}});

    std::vector<float> & out = st_segments.back().data;
    size_t pos = out.size();
    out.resize(pos + (size_t)frames * 2);

    const float * in = pcm->data;
    for (int i = 0; i < frames; i++, in += ch)
    {
        /* моно дублируется, из многоканального берутся левый и правый */
        out[pos++] = in[0];
        out[pos++] = ch > 1 ? in[1] : in[0];
    }

    st_queued_us += (long long)frames * 1000000 / pcm->rate;
}

static void stream_clear_queue()
{
    st_segments.clear();
    st_front_pos = 0;
    st_queued_us = 0;
}

/* Плеер сбросил свой буфер (перемотка, переход к другому треку): звук,
 * который мы держали в очереди, уже не прозвучит — выбрасываем и его. */
static void stream_flush_cb(void *, void *)
{
    std::lock_guard<std::mutex> lock(st_mutex);
    stream_clear_queue();
}

static lame_global_flags * encoder_open(int rate, int bitrate)
{
    lame_global_flags * enc = lame.init();
    if (!enc)
        return nullptr;

    lame.set_in_samplerate(enc, rate);
    lame.set_out_samplerate(enc, STREAM_RATE);
    lame.set_num_channels(enc, 2);
    lame.set_brate(enc, bitrate);
    lame.set_quality(enc, 5);
    lame.set_bWriteVbrTag(enc, 0);
    /* без резервуара каждый кадр самодостаточен: слушатель может
     * подключиться в любой момент, а кодировщик — пересоздаваться */
    lame.set_disable_reservoir(enc, 1);

    if (lame.init_params(enc) < 0)
    {
        lame.close(enc);
        return nullptr;
    }
    return enc;
}

/* Поток кодировщика.
 *
 * Звук приходит от плеера с опережением: Audacious заполняет свой выходной
 * буфер заранее, а при перемотке и смене трека выбрасывает его. Поэтому
 * кодировщик не отправляет звук по мере поступления (иначе каждое
 * переключение добавляло бы слушателю отставание на размер буфера), а
 * держит его в очереди и выдаёт строго со скоростью реального времени —
 * так же, как звуковая карта. Очередь повторяет выходной буфер плеера и
 * сбрасывается вместе с ним, так что поток идёт вровень с колонками. Когда
 * звука нет (стоп, пауза, очередь опустела), в поток идёт тишина. */
static void encoder_loop()
{
    typedef std::chrono::steady_clock Clock;
    using std::chrono::microseconds;

    lame_global_flags * enc = nullptr;
    int enc_rate = 0;
    std::vector<unsigned char> mp3;
    std::vector<float> pcm;
    std::string encoded;

    Clock::time_point last = Clock::now();
    long long owed_us = 0;   /* сколько звука ещё нужно выдать */
    bool rebuffering = true; /* после опустошения очереди копим запас */

    std::unique_lock<std::mutex> lock(st_mutex);

    while (!st_quit)
    {
        if (st_clients.empty())
        {
            stream_clear_queue();
            if (enc)
            {
                lame.close(enc);
                enc = nullptr;
            }
            st_enc_cond.wait(lock,
                             [] { return st_quit || !st_clients.empty(); });
            last = Clock::now();
            owed_us = 0;
            rebuffering = true;
            continue;
        }

        st_enc_cond.wait_for(lock, std::chrono::milliseconds(STREAM_TICK_MS),
                             [] { return st_quit; });
        if (st_quit)
            break;

        /* состояние плеера спрашиваем без своей блокировки */
        lock.unlock();
        bool playing = aud_drct_get_playing();
        bool paused = playing && aud_drct_get_paused();
        lock.lock();

        Clock::time_point now = Clock::now();
        owed_us += std::chrono::duration_cast<microseconds>(now - last).count();
        last = now;
        if (owed_us > 500000) /* поток надолго заснул — не навёрстываем */
            owed_us = 500000;

        if (!playing)
            stream_clear_queue();
        if (rebuffering && st_queued_us >= STREAM_CUSHION_US)
            rebuffering = false;

        int rate;
        pcm.clear();

        if (!paused && !rebuffering && !st_segments.empty())
        {
            PcmSegment & seg = st_segments.front();
            rate = seg.rate;

            size_t want = (size_t)(owed_us * rate / 1000000);
            size_t have = seg.data.size() / 2 - st_front_pos;
            size_t take = std::min(want, have);

            pcm.assign(seg.data.begin() + st_front_pos * 2,
                       seg.data.begin() + (st_front_pos + take) * 2);

            long long taken_us = (long long)take * 1000000 / rate;
            owed_us -= taken_us;
            st_queued_us = std::max(0LL, st_queued_us - taken_us);
            st_front_pos += take;

            if (take == have)
            {
                st_segments.pop_front();
                st_front_pos = 0;
                if (st_segments.empty())
                {
                    st_queued_us = 0;
                    rebuffering = true;
                }
            }
        }
        else
        {
            rate = enc_rate ? enc_rate : STREAM_RATE;
            size_t frames = (size_t)(owed_us * rate / 1000000);
            pcm.assign(frames * 2, 0.0f);
            owed_us -= (long long)frames * 1000000 / rate;
        }

        if (pcm.empty())
            continue;

        lock.unlock();

        encoded.clear();
        int frames = pcm.size() / 2;

        if (!enc || rate != enc_rate)
        {
            mp3.resize(7200);
            if (enc)
            {
                int n = lame.flush(enc, mp3.data(), mp3.size());
                if (n > 0)
                    encoded.append((const char *)mp3.data(), n);
                lame.close(enc);
            }
            enc = encoder_open(rate, aud_get_int(CFG, "stream_bitrate"));
            enc_rate = rate;
        }

        if (enc)
        {
            mp3.resize((size_t)frames * 5 / 4 + 7200);
            int n = lame.encode(enc, pcm.data(), frames, mp3.data(), mp3.size());
            if (n > 0)
                encoded.append((const char *)mp3.data(), n);
        }

        lock.lock();

        if (!encoded.empty())
        {
            for (StreamClient * client : st_clients)
            {
                if (client->buf.size() + encoded.size() > STREAM_CLIENT_MAX)
                    client->dropped = true;
                else
                    client->buf += encoded;
            }
            st_client_cond.notify_all();
        }
    }

    if (enc)
        lame.close(enc);
}

/* обслуживает одного слушателя; выполняется в потоке его соединения */
static void stream_serve(int fd, bool head_only)
{
    static const char head[] = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: audio/mpeg\r\n"
                               "Cache-Control: no-store\r\n"
                               "X-Content-Type-Options: nosniff\r\n"
                               "Connection: close\r\n\r\n";

    if (!send_all(fd, head, sizeof head - 1) || head_only)
        return;

    StreamClient client;
    std::unique_lock<std::mutex> lock(st_mutex);
    st_clients.push_back(&client);
    st_n_clients++;
    st_enc_cond.notify_all();

    std::string chunk;
    for (;;)
    {
        st_client_cond.wait_for(lock, std::chrono::milliseconds(500), [&] {
            return !client.buf.empty() || client.dropped || st_quit;
        });
        if (client.dropped || st_quit || server_closing)
            break;
        if (client.buf.empty())
            continue;

        chunk.clear();
        chunk.swap(client.buf);

        lock.unlock();
        bool sent = send_all(fd, chunk.data(), chunk.size());
        lock.lock();

        if (!sent)
            break;
    }

    st_clients.remove(&client);
    st_n_clients--;
}

static void stream_stop()
{
    if (!stream_running)
        return;

    stream_running = false;
    hook_dissociate(WEBREMOTE_PCM_HOOK, stream_pcm_cb);
    hook_dissociate(WEBREMOTE_FLUSH_HOOK, stream_flush_cb);

    {
        std::lock_guard<std::mutex> lock(st_mutex);
        st_quit = true;
    }
    st_enc_cond.notify_all();
    st_client_cond.notify_all();
    st_thread.join();
}

static void stream_start()
{
    if (stream_running || !lame_load())
        return;

    {
        std::lock_guard<std::mutex> lock(st_mutex);
        st_quit = false;
    }
    st_thread = std::thread(encoder_loop);
    hook_associate(WEBREMOTE_PCM_HOOK, stream_pcm_cb, nullptr);
    hook_associate(WEBREMOTE_FLUSH_HOOK, stream_flush_cb, nullptr);
    stream_running = true;
}

/* включает или выключает эффект-плагин, отводящий звук */
static void stream_enable_tap(bool enable)
{
    PluginHandle * tap = aud_plugin_lookup_basename(WEBREMOTE_STREAM_BASENAME);
    if (!tap)
    {
        if (enable)
            AUDERR("Не найден плагин " WEBREMOTE_STREAM_BASENAME
                   ".so — трансляция будет молчать\n");
        return;
    }
    if (aud_plugin_get_enabled(tap) != enable)
        aud_plugin_enable(tap, enable);
}

/* Трансляция доступна, когда работает кодировщик и включён эффект,
 * отводящий звук, — неважно, галочкой в настройках Web Remote или прямо
 * в списке эффектов Audacious. */
static bool stream_available()
{
    if (!stream_running)
        return false;
    PluginHandle * tap = aud_plugin_lookup_basename(WEBREMOTE_STREAM_BASENAME);
    return tap && aud_plugin_get_enabled(tap);
}

static void stream_config_changed()
{
    stream_enable_tap(aud_get_bool(CFG, "stream"));
}

/* при запуске приводит галочку и эффект к одному состоянию */
static void stream_sync_tap()
{
    if (aud_get_bool(CFG, "stream"))
        stream_enable_tap(true);
    else if (stream_available())
        aud_set_bool(CFG, "stream", true);
}

static bool send_response(int fd, const Response & r, bool head_only,
                          bool keep_alive = false)
{
    std::string head = "HTTP/1.1 " + std::to_string(r.code) + " " +
                       status_text(r.code) + "\r\n";
    head += "Content-Type: " + r.type + "\r\n";
    head += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    head += r.cacheable ? "Cache-Control: private, max-age=3600\r\n"
                        : "Cache-Control: no-store\r\n";
    if (r.code == 401)
        head += "WWW-Authenticate: Basic realm=\"Audacious\", charset=\"UTF-8\"\r\n";
    head += "X-Content-Type-Options: nosniff\r\n";
    head += keep_alive ? "Connection: keep-alive\r\n\r\n"
                       : "Connection: close\r\n\r\n";

    if (!send_all(fd, head.data(), head.size()))
        return false;
    return head_only || send_all(fd, r.body.data(), r.body.size());
}

/* Читает и разбирает запрос; при ошибке возвращает код HTTP, иначе 0.
 * <buf> хранит прочитанное между запросами одного соединения. */
static int read_request(int fd, Request & req, std::string & buf)
{
    size_t header_end;
    char chunk[4096];

    while ((header_end = buf.find("\r\n\r\n")) == std::string::npos)
    {
        if (buf.size() > MAX_HEADER)
            return 413;
        ssize_t n = recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0)
            return -1; /* клиент молчит или ушёл — закрываем без ответа */
        buf.append(chunk, n);
    }

    size_t line_end = buf.find("\r\n");
    std::string line = buf.substr(0, line_end);
    size_t sp1 = line.find(' ');
    size_t sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp2 <= sp1)
        return 400;

    req.method = line.substr(0, sp1);
    req.http10 = line.compare(sp2 + 1, std::string::npos, "HTTP/1.0") == 0;
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
        if (*end || len > (req.path == UPLOAD_PATH ? UPLOAD_MAX : MAX_BODY))
            return 413;
        body_len = len;
    }

    /* файл не читается в память: его тело обработчик загрузки пишет на
     * диск по мере поступления */
    if (req.path == UPLOAD_PATH)
    {
        req.body_len = body_len;
        buf.erase(0, header_end + 4);
        return 0;
    }

    size_t total = header_end + 4 + body_len;
    while (buf.size() < total)
    {
        ssize_t n = recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0)
            return -1;
        buf.append(chunk, n);
    }

    if (body_len)
        parse_form(buf.substr(header_end + 4, body_len), req.params);

    buf.erase(0, total);
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

/* ------------------------------------------------------------------------ */
/* Загрузка файлов                                                           */
/* ------------------------------------------------------------------------ */

/* Страница присылает файл телом POST-запроса /api/upload?name=…&pl=N.
 * Файл сохраняется в папку загрузок и добавляется в плейлист. Имя файла
 * приходит от клиента, поэтому из него берётся только последняя часть пути,
 * а расширение должно быть из списка разрешённых (см. refresh_exts):
 * записать что-то за пределы папки или положить туда произвольный файл
 * нельзя. */

static std::string upload_dir()
{
    String dir = aud_get_str(CFG, "upload_dir");
    if (dir && dir[0])
    {
        /* виджет выбора папки сохраняет адрес file:// */
        if (!strncmp(dir, "file://", 7))
        {
            StringBuf path = uri_to_filename(dir);
            if (path)
                return std::string(path);
        }
        else
            return std::string(dir);
    }

    const char * home = getenv("HOME");
    return std::string(home ? home : "/tmp") + "/Music/Audacious Web";
}

static bool make_dirs(const std::string & dir)
{
    for (size_t pos = 1; pos <= dir.size(); pos++)
    {
        if (pos < dir.size() && dir[pos] != '/')
            continue;
        if (mkdir(dir.substr(0, pos).c_str(), 0755) < 0 && errno != EEXIST)
            return false;
    }
    return true;
}

/* возвращает безопасное имя файла или пустую строку */
static std::string upload_name(const std::string & raw)
{
    size_t slash = raw.find_last_of("/\\");
    std::string name = slash == std::string::npos ? raw : raw.substr(slash + 1);

    for (char & c : name)
        if ((unsigned char)c < 0x20 || c == 0x7f)
            c = '_';

    if (name.empty() || name[0] == '.' || name.size() > 200)
        return std::string();

    size_t dot = name.rfind('.');
    if (dot == std::string::npos)
        return std::string();

    std::string ext = name.substr(dot + 1);
    for (char & c : ext)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';

    std::lock_guard<std::mutex> lock(ext_mutex);
    if (std::binary_search(ext_cache.begin(), ext_cache.end(), ext))
        return name;
    return std::string();
}

/* читает и отбрасывает тело запроса, чтобы клиент дождался ответа с ошибкой */
static bool discard_body(int fd, std::string & pending, size_t len)
{
    if (len > 256 * 1024 * 1024)
        return false; /* проще разорвать соединение */

    size_t have = std::min(pending.size(), len);
    pending.erase(0, have);
    len -= have;

    char chunk[65536];
    while (len)
    {
        ssize_t n = recv(fd, chunk, std::min(len, sizeof chunk), 0);
        if (n <= 0)
            return false;
        len -= n;
    }
    return true;
}

/* <consumed> — прочитано ли тело целиком (иначе соединение надо закрыть) */
static Response handle_upload(int fd, const Request & req, std::string & pending,
                              bool & consumed)
{
    static std::atomic<int> counter{0};

    Response error;
    std::string name;

    if (!check_auth(req))
        error = error_response(401, "unauthorized");
    else if (req.method != "POST")
        error = error_response(405, "method not allowed");
    else if (!check_origin(req))
        error = error_response(403, "cross-origin request rejected");
    else if (!aud_get_bool(CFG, "upload"))
        error = error_response(403, "uploads are disabled");
    else if (!req.body_len)
        error = error_response(400, "empty file");
    else
    {
        /* список форматов мог измениться: включили модуль, поменяли настройку */
        run_in_main([] {
            refresh_exts();
            return Response();
        });
        if ((name = upload_name(str_param(req, "name"))).empty())
            error = error_response(415, "unsupported file type");
    }

    if (error.code != 200)
    {
        consumed = discard_body(fd, pending, req.body_len);
        return error;
    }

    consumed = false;

    std::string dir = upload_dir();
    if (!make_dirs(dir))
        return error_response(500, "cannot create upload folder");

    /* сначала во временный файл: недокачанное не должно выглядеть как трек */
    std::string tmp = dir + "/.upload-" + std::to_string(getpid()) + "-" +
                      std::to_string(counter++) + ".part";
    int out = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (out < 0)
        return error_response(500, "cannot write to upload folder");

    auto write_all = [out](const char * data, size_t len) {
        while (len)
        {
            ssize_t n = write(out, data, len);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return false;
            data += n;
            len -= n;
        }
        return true;
    };

    size_t left = req.body_len;
    size_t have = std::min(pending.size(), left);
    bool ok = write_all(pending.data(), have);
    pending.erase(0, have);
    left -= have;

    bool disk_error = !ok;
    char chunk[65536];
    while (ok && left)
    {
        ssize_t n = recv(fd, chunk, std::min(left, sizeof chunk), 0);
        if (n <= 0)
            ok = false; /* клиент пропал или прервал загрузку */
        else if (!write_all(chunk, n))
            ok = false, disk_error = true;
        else
            left -= n;
    }

    if (close(out) < 0)
        ok = false, disk_error = true;

    if (!ok)
    {
        unlink(tmp.c_str());
        return error_response(disk_error ? 500 : 400,
                              disk_error ? "write failed" : "upload interrupted");
    }

    consumed = true;

    /* имя не должно затирать существующий файл: «песня (2).mp3» */
    size_t dot = name.rfind('.');
    std::string path = dir + "/" + name;
    for (int i = 2; access(path.c_str(), F_OK) == 0 && i < 1000; i++)
        path = dir + "/" + name.substr(0, dot) + " (" + std::to_string(i) + ")" +
               name.substr(dot);

    if (rename(tmp.c_str(), path.c_str()) < 0)
    {
        unlink(tmp.c_str());
        return error_response(500, "cannot save file");
    }

    return run_in_main([req, path] {
        Playlist list;
        if (!get_playlist(req, list))
            list = Playlist::active_playlist();
        list.insert_entry(-1, filename_to_uri(path.c_str()), Tuple(), false);

        std::string out = "{\"ok\":true,";
        json_field(out, "file", path.c_str());
        json_field(out, "pl", list.index());
        json_close(out, '}');

        Response r;
        r.body = std::move(out);
        return r;
    });
}

/* ------------------------------------------------------------------------ */
/* Скачивание трека                                                          */
/* ------------------------------------------------------------------------ */

/* GET /api/download?pl=N&entry=M отдаёт файл записи плейлиста. Отдать можно
 * только то, что стоит в плейлисте, — произвольный путь указать нельзя.
 * С параметром check=1 файл не передаётся: страница сначала убеждается,
 * что скачивание возможно, и только потом запускает его в браузере. */

/* главный поток: путь к файлу записи в теле ответа либо ошибка */
static Response download_resolve(const Request & req)
{
    if (has_param(req, "sserial") &&
        int_param(req, "sserial", 0) != structure_serial.load())
        return error_response(409, "playlist changed");

    Playlist list;
    if (!get_playlist(req, list))
        return error_response(404, "no such playlist");

    int entry = int_param(req, "entry", -1);
    if (entry < 0 || entry >= list.n_entries())
        return error_response(404, "no such entry");

    String uri_str = list.entry_filename(entry);
    std::string uri = uri_str ? (const char *)uri_str : "";
    if (uri.compare(0, 7, "file://"))
        return error_response(415, "not a local file");

    /* у дорожек внутри одного файла (cue, модули) адрес кончается на ?N */
    size_t q = uri.rfind('?');
    if (q != std::string::npos &&
        uri.find_first_not_of("0123456789", q + 1) == std::string::npos)
        uri.resize(q);

    StringBuf path = uri_to_filename(uri.c_str());
    if (!path)
        return error_response(415, "not a local file");

    Response r;
    r.body = (const char *)path;
    return r;
}

/* возвращает false, если соединение нужно закрыть */
static bool serve_download(int fd, const Request & req, bool keep)
{
    Response r;
    if (!check_auth(req))
        r = error_response(401, "unauthorized");
    else if (req.method != "GET")
        r = error_response(405, "method not allowed");
    else if (!aud_get_bool(CFG, "download"))
        r = error_response(403, "downloads are disabled");
    else
        r = run_in_main([req] { return download_resolve(req); });

    if (r.code != 200)
        return send_response(fd, r, false, keep) && keep;

    std::string path = r.body;
    int in = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (in < 0 || fstat(in, &st) < 0 || !S_ISREG(st.st_mode))
    {
        if (in >= 0)
            close(in);
        return send_response(fd, error_response(404, "file is not available"),
                             false, keep) && keep;
    }

    size_t slash = path.rfind('/');
    std::string name = path.substr(slash + 1);

    if (has_param(req, "check"))
    {
        close(in);
        std::string out = "{\"ok\":true,";
        json_field(out, "name", name.c_str());
        json_field(out, "size", (long long)st.st_size);
        json_close(out, '}');
        Response info;
        info.body = std::move(out);
        return send_response(fd, info, false, keep) && keep;
    }

    /* имя файла в заголовке — в кодировке RFC 5987, иначе кириллица ломается */
    std::string encoded;
    for (unsigned char c : name)
    {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_')
            encoded += (char)c;
        else
        {
            char buf[4];
            snprintf(buf, sizeof buf, "%%%02X", c);
            encoded += buf;
        }
    }

    std::string head = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/octet-stream\r\n"
                       "Content-Length: " + std::to_string((long long)st.st_size) + "\r\n"
                       "Content-Disposition: attachment; filename*=UTF-8''" + encoded + "\r\n"
                       "Cache-Control: no-store\r\n"
                       "X-Content-Type-Options: nosniff\r\n";
    head += keep ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";

    bool ok = send_all(fd, head.data(), head.size());
    long long left = st.st_size;
    char chunk[65536];
    while (ok && left > 0)
    {
        ssize_t n = read(in, chunk, std::min<long long>(left, sizeof chunk));
        if (n <= 0)
            ok = false; /* файл укоротили — обещанную длину уже не отдать */
        else
        {
            ok = send_all(fd, chunk, n);
            left -= n;
        }
    }

    close(in);
    return ok && keep;
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

/* клиент просит закрыть соединение после ответа */
static bool wants_close(const Request & req)
{
    auto it = req.headers.find("connection");
    if (it == req.headers.end())
        return req.http10;

    std::string value = it->second;
    for (char & c : value)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return value.find("close") != std::string::npos ||
           (req.http10 && value.find("keep-alive") == std::string::npos);
}

static void serve_connection(Connection * conn)
{
    int fd = conn->fd;

    /* столько соединение ждёт следующего запроса и столько же — записи */
    struct timeval tv = {20, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    /* Клиент может пропасть, не закрыв соединение (телефон вышел из зоны
     * Wi-Fi). Без этих настроек ядро ещё минутами пытается доставить ему
     * данные, а трансляция продолжает их копить. */
    int idle = 10, interval = 5, probes = 3, unacked_ms = 20000;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof probes);
    setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &unacked_ms, sizeof unacked_ms);

    /* Соединение обслуживает запросы один за другим (keep-alive): страница
     * опрашивает состояние каждую секунду, и открывать для этого каждый раз
     * новое соединение по Wi-Fi медленно и ненадёжно. */
    std::string pending;
    for (;;)
    {
        Request req;
        int err = read_request(fd, req, pending);
        if (err > 0)
            send_response(fd, error_response(err, "bad request"), false);
        if (err)
            break;

        bool head_only = req.method == "HEAD";
        if (req.path == "/stream.mp3" && (req.method == "GET" || head_only) &&
            check_auth(req) && stream_running)
        {
            stream_serve(fd, head_only); /* до отключения слушателя */
            break;
        }

        bool keep = !wants_close(req) && !server_closing;
        if (req.path == "/api/download" && !head_only)
        {
            if (!serve_download(fd, req, keep))
                break;
            continue;
        }

        Response response;
        if (req.path == UPLOAD_PATH)
        {
            bool consumed = false;
            response = handle_upload(fd, req, pending, consumed);
            keep = keep && consumed;
        }
        else
            response = handle_request(req);

        if (!send_response(fd, response, head_only, keep) || !keep)
            break;
    }

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
        if (conns.size() >= MAX_CONNECTIONS)
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
    server_closing = false;
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

    server_closing = true; /* отпускает слушателей трансляции */
    st_client_cond.notify_all();

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

static QueuedFunc stream_init_timer;

bool WebRemote::init()
{
    aud_config_set_defaults(CFG, defaults);
    hook_associate("playlist update", playlist_update_cb, nullptr);

    /* Если порт занят, плагин всё равно включается, чтобы порт можно было
     * сменить в настройках. */
    server_start();

    /* Эффект включается не сразу, а когда заработает главный цикл: во время
     * init() Audacious ещё может загружать плагины. */
    stream_start(); /* без слушателей кодировщик простаивает */
    stream_init_timer.queue(stream_sync_tap);
    return true;
}

void WebRemote::cleanup()
{
    stream_init_timer.stop();
    server_stop();
    undo_stack.clear();
    stream_stop();
    hook_dissociate("playlist update", playlist_update_cb);
}
