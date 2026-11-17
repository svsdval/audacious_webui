/*
 * Web Remote: отвод звука для трансляции в браузер.
 *
 * Эффект, который ничего не меняет в звуке, а только передаёт копию
 * основному плагину Web Remote (см. webremote-pcm.h). Включается и
 * выключается галочкой «Трансляция» в настройках Web Remote.
 */

#include <libaudcore/hook.h>
#include <libaudcore/plugin.h>

#include "webremote-pcm.h"

#define EXPORT __attribute__((visibility("default")))

class WebRemoteTap : public EffectPlugin
{
public:
    static constexpr PluginInfo info = {
        "Web Remote: трансляция звука", "audacious-web-remote",
        "Передаёт воспроизводимый звук плагину Web Remote для трансляции\n"
        "в браузер. Управляется из настроек Web Remote."};

    /* порядок 10 — после остальных эффектов, чтобы слушатель в браузере
     * слышал то же, что и колонки */
    constexpr WebRemoteTap() : EffectPlugin(info, 10, true) {}

    void start(int & channels, int & rate)
    {
        m_channels = channels;
        m_rate = rate;
    }

    Index<float> & process(Index<float> & data)
    {
        if (data.len() && m_channels > 0)
        {
            WebRemotePcm pcm = {data.begin(), data.len(), m_channels, m_rate};
            hook_call(WEBREMOTE_PCM_HOOK, &pcm);
        }
        return data;
    }

    bool flush(bool force)
    {
        hook_call(WEBREMOTE_FLUSH_HOOK, nullptr);
        return true;
    }

private:
    int m_channels = 0, m_rate = 0;
};

EXPORT WebRemoteTap aud_plugin_instance;
