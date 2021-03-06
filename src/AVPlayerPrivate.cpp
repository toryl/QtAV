/******************************************************************************
    QtAV:  Media play library based on Qt and FFmpeg
    Copyright (C) 2014-2015 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
******************************************************************************/

#include "AVPlayerPrivate.h"
#include "filter/FilterManager.h"
#include "output/OutputSet.h"
#include "QtAV/AudioDecoder.h"
#include "QtAV/AudioFormat.h"
#include "QtAV/AudioResampler.h"
#include "QtAV/VideoCapture.h"
#include "QtAV/private/AVCompat.h"
#include "utils/Logger.h"

namespace QtAV {

namespace Internal {

int computeNotifyPrecision(qint64 duration, qreal fps)
{
    if (duration <= 0 || duration > 60*1000) // no duration or 10min
        return 500;
    if (duration > 20*1000)
        return 250;
    int dt = 500;
    if (fps > 1) {
        dt = qMin(250, int(qreal(dt*2)/fps));
    } else {
        dt = duration / 80; //<= 250
    }
    return qMax(20, dt);
}
} // namespace Internal

static bool correct_audio_channels(AVCodecContext *ctx) {
    if (ctx->channels <= 0) {
        if (ctx->channel_layout) {
            ctx->channels = av_get_channel_layout_nb_channels(ctx->channel_layout);
        }
    } else {
        if (!ctx->channel_layout) {
            ctx->channel_layout = av_get_default_channel_layout(ctx->channels);
        }
    }
    return ctx->channel_layout > 0 && ctx->channels > 0;
}

AVPlayer::Private::Private()
    : auto_load(false)
    , async_load(true)
    , loaded(false)
    , relative_time_mode(true)
    , fmt_ctx(0)
    , media_start_pts(0)
    , media_end(kInvalidPosition)
    , last_position(0)
    , reset_state(true)
    , start_position(0)
    , stop_position(kInvalidPosition)
    , repeat_max(0)
    , repeat_current(0)
    , timer_id(-1)
    , audio_track(0)
    , video_track(0)
    , subtitle_track(0)
    , read_thread(0)
    , clock(new AVClock(AVClock::AudioClock))
    , vo(0)
    , ao(0)
    , adec(0)
    , vdec(0)
    , athread(0)
    , vthread(0)
    , vcapture(0)
    , speed(1.0)
    , ao_enabled(true)
    , vos(0)
    , aos(0)
    , brightness(0)
    , contrast(0)
    , saturation(0)
    , seeking(false)
    , seek_type(AccurateSeek)
    , seek_target(0)
    , interrupt_timeout(30000)
    , mute(false)
    , notify_interval(500)
{
    demuxer.setInterruptTimeout(interrupt_timeout);
    /*
     * reset_state = true;
     * must be the same value at the end of stop(), and must be different from value in
     * stopFromDemuxerThread()(which is false), so the initial value must be true
     */

    vc_ids
#if QTAV_HAVE(DXVA)
            //<< VideoDecoderId_DXVA
#endif //QTAV_HAVE(DXVA)
#if QTAV_HAVE(VAAPI)
            //<< VideoDecoderId_VAAPI
#endif //QTAV_HAVE(VAAPI)
#if QTAV_HAVE(CEDARV)
            << VideoDecoderId_Cedarv
#endif //QTAV_HAVE(CEDARV)
            << VideoDecoderId_FFmpeg;
    ao_ids
#if QTAV_HAVE(OPENAL)
            << AudioOutputId_OpenAL
#endif
#if QTAV_HAVE(PORTAUDIO)
            << AudioOutputId_PortAudio
#endif
#if QTAV_HAVE(OPENSL)
            << AudioOutputId_OpenSL
#endif
#if QTAV_HAVE(DSOUND)
            << AudioOutputId_DSound
#endif
              ;
}
AVPlayer::Private::~Private() {
    // TODO: scoped ptr
    if (ao) {
        delete ao;
        ao = 0;
    }
    if (adec) {
        delete adec;
        adec = 0;
    }
    if (vdec) {
        delete vdec;
        vdec = 0;
    }
    if (vos) {
        vos->clearOutputs();
        delete vos;
        vos = 0;
    }
    if (aos) {
        aos->clearOutputs();
        delete aos;
        aos = 0;
    }
    if (vcapture) {
        delete vcapture;
        vcapture = 0;
    }
    if (clock) {
        delete clock;
        clock = 0;
    }
    if (read_thread) {
        delete read_thread;
        read_thread = 0;
    }
}

void AVPlayer::Private::initStatistics()
{
    initBaseStatistics();
    initAudioStatistics(demuxer.audioStream());
    initVideoStatistics(demuxer.videoStream());
    //initSubtitleStatistics(demuxer.subtitleStream());
}

//TODO: av_guess_frame_rate in latest ffmpeg
void AVPlayer::Private::initBaseStatistics()
{
    statistics.reset();
    statistics.url = current_source.type() == QVariant::String ? current_source.toString() : QString();
    statistics.bit_rate = fmt_ctx->bit_rate;
    statistics.format = QString().sprintf("%s - %s", fmt_ctx->iformat->name, fmt_ctx->iformat->long_name);
    //AV_TIME_BASE_Q: msvc error C2143
    //fmt_ctx->duration may be AV_NOPTS_VALUE. AVDemuxer.duration deals with this case
    statistics.start_time = QTime(0, 0, 0).addMSecs(int(demuxer.startTime()));
    statistics.duration = QTime(0, 0, 0).addMSecs((int)demuxer.duration());
    if (vdec)
        statistics.video.decoder = VideoDecoderFactory::name(vdec->id()).c_str();
    statistics.metadata.clear();
    AVDictionaryEntry *tag = NULL;
    while ((tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        statistics.metadata.insert(tag->key, tag->value);
    }
    notify_interval = Internal::computeNotifyPrecision(demuxer.duration(), demuxer.frameRate());
    qDebug("notify_interval: %d", notify_interval);
}

void AVPlayer::Private::initCommonStatistics(int s, Statistics::Common *st, AVCodecContext *avctx)
{
    AVStream *stream = fmt_ctx->streams[s];
    qDebug("stream: %d, duration=%lld (%lld ms), time_base=%f", s, stream->duration, qint64(qreal(stream->duration)*av_q2d(stream->time_base)*1000.0), av_q2d(stream->time_base));
    // AVCodecContext.codec_name is deprecated. use avcodec_get_name. check null avctx->codec?
    st->codec = avcodec_get_name(avctx->codec_id);
    st->codec_long = get_codec_long_name(avctx->codec_id);
    st->total_time = QTime(0, 0, 0).addMSecs(stream->duration == AV_NOPTS_VALUE ? 0 : int(qreal(stream->duration)*av_q2d(stream->time_base)*1000.0));
    st->start_time = QTime(0, 0, 0).addMSecs(stream->start_time == AV_NOPTS_VALUE ? 0 : int(qreal(stream->start_time)*av_q2d(stream->time_base)*1000.0));
    qDebug("codec: %s(%s)", qPrintable(st->codec), qPrintable(st->codec_long));
    st->bit_rate = avctx->bit_rate; //fmt_ctx
    st->frames = stream->nb_frames;
    //qDebug("time: %f~%f, nb_frames=%lld", st->start_time, st->total_time, stream->nb_frames); //why crash on mac? av_q2d({0,0})?
    AVDictionaryEntry *tag = NULL;
    while ((tag = av_dict_get(stream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        st->metadata.insert(tag->key, tag->value);
    }
}

void AVPlayer::Private::initAudioStatistics(int s)
{
    AVCodecContext *avctx = demuxer.audioCodecContext();
    if (!avctx) {
        statistics.audio = Statistics::Common();
        statistics.audio_only = Statistics::AudioOnly();
        return;
    }
    statistics.audio.available = s == demuxer.audioStream();
    initCommonStatistics(s, &statistics.audio, avctx);
    correct_audio_channels(avctx);
    statistics.audio_only.block_align = avctx->block_align;
    statistics.audio_only.channels = avctx->channels;
    char cl[128]; //
    // nb_channels -1: will use av_get_channel_layout_nb_channels
    av_get_channel_layout_string(cl, sizeof(cl), avctx->channels, avctx->channel_layout); //TODO: ff version
    statistics.audio_only.channel_layout = cl;
    statistics.audio_only.sample_fmt = av_get_sample_fmt_name(avctx->sample_fmt);
    statistics.audio_only.frame_size = avctx->frame_size;
    statistics.audio_only.sample_rate = avctx->sample_rate;
}

void AVPlayer::Private::initVideoStatistics(int s)
{
    AVCodecContext *avctx = demuxer.videoCodecContext();
    if (!avctx) {
        statistics.video = Statistics::Common();
        statistics.video_only = Statistics::VideoOnly();
        return;
    }
    statistics.video.available = s == demuxer.videoStream();
    initCommonStatistics(s, &statistics.video, avctx);
    AVStream *stream = fmt_ctx->streams[s];
    statistics.video.frames = stream->nb_frames;
    //http://ffmpeg.org/faq.html#AVStream_002er_005fframe_005frate-is-wrong_002c-it-is-much-larger-than-the-frame-rate_002e
    //http://libav-users.943685.n4.nabble.com/Libav-user-Reading-correct-frame-rate-fps-of-input-video-td4657666.html
    //FIXME: which 1 should we choose? avg_frame_rate may be nan or 0, then use AVStream.r_frame_rate, r_frame_rate may be wrong(guessed value)
    // TODO: seems that r_frame_rate will be removed libav > 9.10. Use macro to check version?
    //if (stream->avg_frame_rate.num) //avg_frame_rate.num,den may be 0
        statistics.video_only.frame_rate = av_q2d(stream->avg_frame_rate);
    //else
    //    statistics.video_only.frame_rate = av_q2d(stream->r_frame_rate);
    statistics.video_only.coded_height = avctx->coded_height;
    statistics.video_only.coded_width = avctx->coded_width;
    statistics.video_only.gop_size = avctx->gop_size;
    statistics.video_only.pix_fmt = av_get_pix_fmt_name(avctx->pix_fmt);
    statistics.video_only.height = avctx->height;
    statistics.video_only.width = avctx->width;
}
// notify statistics change after audio/video thread is set
bool AVPlayer::Private::setupAudioThread(AVPlayer *player)
{
    demuxer.setStreamIndex(AVDemuxer::AudioStream, audio_track);
    // pause demuxer, clear queues, set demuxer stream, set decoder, set ao, resume
    // clear packets before stream changed
    if (athread) {
        athread->packetQueue()->clear();
        athread->setDecoder(0);
        athread->setOutput(0);
        initAudioStatistics(demuxer.audioStream());
    }
    AVCodecContext *avctx = demuxer.audioCodecContext();
    if (!avctx) {
        // TODO: close ao?
        return false;
    }
    qDebug("has audio");
    // TODO: no delete, just reset avctx and reopen
    if (adec) {
        adec->disconnect();
        delete adec;
        adec = 0;
    }
    adec = new AudioDecoder();
    connect(adec, SIGNAL(error(QtAV::AVError)), player, SIGNAL(error(QtAV::AVError)));
    adec->setCodecContext(avctx);
    adec->setOptions(ac_opt);
    if (!adec->open()) {
        AVError e(AVError::AudioCodecNotFound);
        qWarning() << e.string();
        emit player->error(e);
        return false;
    }
    statistics.audio.decoder = adec->name();
    //TODO: setAudioOutput() like vo
    if (!ao && ao_enabled) {
        foreach (AudioOutputId aoid, ao_ids) {
            qDebug("trying audio output '%s'", AudioOutputFactory::name(aoid).c_str());
            ao = AudioOutputFactory::create(aoid);
            if (ao) {
                qDebug("audio output found.");
                break;
            }
        }
    }
    if (!ao) {
        // TODO: only when no audio stream or user disable audio stream. running an audio thread without sound is waste resource?
        //masterClock()->setClockType(AVClock::ExternalClock);
        //return;
    } else {
        correct_audio_channels(avctx);
        AudioFormat af;
        af.setSampleRate(avctx->sample_rate);
        af.setSampleFormatFFmpeg(avctx->sample_fmt);
        // 5, 6, 7 channels may not play
        if (avctx->channels > 2)
            af.setChannelLayout(ao->preferredChannelLayout());
        else
            af.setChannelLayoutFFmpeg(avctx->channel_layout);
        //af.setChannels(avctx->channels);
        // FIXME: workaround. planar convertion crash now!
        if (af.isPlanar()) {
            af.setSampleFormat(ao->preferredSampleFormat());
        }
        if (!ao->isSupported(af)) {
            if (!ao->isSupported(af.sampleFormat())) {
                af.setSampleFormat(ao->preferredSampleFormat());
            }
            if (!ao->isSupported(af.channelLayout())) {
                af.setChannelLayout(ao->preferredChannelLayout());
            }
        }
        if (ao->audioFormat() != af) {
            qDebug("ao audio format is changed. reopen ao");
            ao->close();
            ao->setAudioFormat(af);
            if (!ao->open()) {
                //could not open audio device. use extrenal clock
                delete ao;
                ao = 0;
                return false;
            }
        }
    }
    if (ao)
        adec->resampler()->setOutAudioFormat(ao->audioFormat());
    adec->resampler()->inAudioFormat().setSampleFormatFFmpeg(avctx->sample_fmt);
    adec->resampler()->inAudioFormat().setSampleRate(avctx->sample_rate);
    adec->resampler()->inAudioFormat().setChannels(avctx->channels);
    adec->resampler()->inAudioFormat().setChannelLayoutFFmpeg(avctx->channel_layout);
    adec->prepare();
    if (!athread) {
        qDebug("new audio thread");
        athread = new AudioThread(player);
        athread->setClock(clock);
        athread->setStatistics(&statistics);
        athread->setOutputSet(aos);
        qDebug("demux thread setAudioThread");
        read_thread->setAudioThread(athread);
        //reconnect if disconnected
        QList<Filter*> filters = FilterManager::instance().audioFilters(player);
        //TODO: isEmpty()==false but size() == 0 in debug mode, it's a Qt bug? we can not just foreach without check empty in debug mode
        if (filters.size() > 0) {
            foreach (Filter *filter, filters) {
                athread->installFilter(filter);
            }
        }
    }
    athread->setDecoder(adec);
    player->setAudioOutput(ao);
    int queue_min = 0.61803*qMax<qreal>(24.0, statistics.video_only.frame_rate);
    int queue_max = int(1.61803*(qreal)queue_min); //about 1 second
    athread->packetQueue()->setThreshold(queue_min);
    athread->packetQueue()->setCapacity(queue_max);
    return true;
}

bool AVPlayer::Private::setupVideoThread(AVPlayer *player)
{
    demuxer.setStreamIndex(AVDemuxer::VideoStream, video_track);
    // pause demuxer, clear queues, set demuxer stream, set decoder, set ao, resume
    // clear packets before stream changed
    if (vthread) {
        vthread->packetQueue()->clear();
        // TODO: wait for next keyframe
        vthread->setDecoder(0); // TODO: not work now. must dynamic check decoder in every loop in VideoThread.run()
        initVideoStatistics(demuxer.videoStream());
    }
    AVCodecContext *avctx = demuxer.videoCodecContext();
    if (!avctx) {
        return false;
    }
    if (vdec) {
        vdec->disconnect();
        delete vdec;
        vdec = 0;
    }
    foreach(VideoDecoderId vid, vc_ids) {
        qDebug("**********trying video decoder: %s...", VideoDecoderFactory::name(vid).c_str());
        VideoDecoder *vd = VideoDecoderFactory::create(vid);
        if (!vd) {
            continue;
        }
        //vd->isAvailable() //TODO: the value is wrong now
        vd->setCodecContext(avctx);
        vd->setOptions(vc_opt);
        if (vd->prepare() && vd->open()) {
            vdec = vd;
            qDebug("**************Video decoder found");
            break;
        }
        delete vd;
    }
    if (!vdec) {
        // DO NOT emit error signals in VideoDecoder::open(). 1 signal is enough
        AVError e(AVError::VideoCodecNotFound);
        qWarning() << e.string();
        emit player->error(e);
        return false;
    }
    connect(vdec, SIGNAL(error(QtAV::AVError)), player, SIGNAL(error(QtAV::AVError)));
    statistics.video.decoder = vdec->name();
    if (!vthread) {
        vthread = new VideoThread(player);
        vthread->setClock(clock);
        vthread->setStatistics(&statistics);
        vthread->setVideoCapture(vcapture);
        vthread->setOutputSet(vos);
        read_thread->setVideoThread(vthread);

        QList<Filter*> filters = FilterManager::instance().videoFilters(player);
        if (filters.size() > 0) {
            foreach (Filter *filter, filters) {
                vthread->installFilter(filter);
            }
        }
    }
    vthread->setDecoder(vdec);
    vthread->setBrightness(brightness);
    vthread->setContrast(contrast);
    vthread->setSaturation(saturation);
    int queue_min = 0.61803*qMax<qreal>(24.0, statistics.video_only.frame_rate);
    int queue_max = int(1.61803*(qreal)queue_min); //about 1 second
    vthread->packetQueue()->setThreshold(queue_min);
    vthread->packetQueue()->setCapacity(queue_max);
    return true;
}

} //namespace QtAV
