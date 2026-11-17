/*
 * Канал передачи звука между двумя плагинами Web Remote.
 *
 * Эффект-плагин (webremote-stream.so) стоит в цепочке обработки звука и
 * отдаёт каждый проходящий блок через hook libaudcore; основной плагин
 * (webremote.so) подписывается на этот hook и кодирует звук в MP3.
 * Плагины — отдельные .so и напрямую друг на друга не ссылаются.
 */

#ifndef WEBREMOTE_PCM_H
#define WEBREMOTE_PCM_H

#define WEBREMOTE_PCM_HOOK "web remote pcm"
/* плеер сбросил буфер (перемотка, смена трека); данных нет */
#define WEBREMOTE_FLUSH_HOOK "web remote flush"
#define WEBREMOTE_STREAM_BASENAME "webremote-stream"

struct WebRemotePcm
{
    const float * data; /* чередующиеся каналы, диапазон -1..1 */
    int samples;        /* всего отсчётов (кадры * каналы) */
    int channels;
    int rate;
};

#endif
