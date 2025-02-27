/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2015-2019 The qTox Project Contributors
 * Copyright © 2024-2025 The TokTok team.
 */

extern "C"
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#pragma GCC diagnostic pop
}

#include "corevideosource.h"
#include "videoframe.h"

/**
 * @class CoreVideoSource
 * @brief A VideoSource that emits frames received by Core.
 */

/**
 * @var std::atomic_int subscribers
 * @brief Number of subscribers
 *
 * @var std::atomic_bool deleteOnClose
 * @brief If true, self-delete after the last subscriber is gone
 */

/**
 * @brief CoreVideoSource constructor.
 * @note Only CoreAV should create a CoreVideoSource since
 * only CoreAV can push images to it.
 */
CoreVideoSource::CoreVideoSource()
    : subscribers{0}
    , deleteOnClose{false}
    , stopped{false}
{
}

/**
 * @brief Makes a copy of the vpx_image_t and emits it as a new VideoFrame.
 * @param vpxFrame Frame to copy.
 */
void CoreVideoSource::pushFrame(const vpx_image_t* vpxFrame)
{
    if (stopped)
        return;

    QMutexLocker<QMutex> locker(&biglock);

    std::shared_ptr<VideoFrame> vframe;
    int width = vpxFrame->d_w;
    int height = vpxFrame->d_h;

    if (subscribers <= 0)
        return;

    AVFrame* avFrame = av_frame_alloc();
    if (!avFrame)
        return;

    avFrame->width = width;
    avFrame->height = height;
    avFrame->format = AV_PIX_FMT_YUV420P;

    int bufSize =
        av_image_alloc(avFrame->data, avFrame->linesize, width, height,
                       static_cast<AVPixelFormat>(AV_PIX_FMT_YUV420P), VideoFrame::dataAlignment);

    if (bufSize < 0) {
        av_frame_free(&avFrame);
        return;
    }

    for (int i = 0; i < 3; ++i) {
        int dstStride = avFrame->linesize[i];
        int srcStride = vpxFrame->stride[i];
        int minStride = std::min(dstStride, srcStride);
        int size = (i == 0) ? height : height / 2;

        for (int j = 0; j < size; ++j) {
            uint8_t* dst = avFrame->data[i] + dstStride * j;
            uint8_t* src = vpxFrame->planes[i] + srcStride * j;
            memcpy(dst, src, minStride);
        }
    }

    emit frameAvailable(VideoFrame::fromAVFrameUntracked(id, avFrame, true));
}

void CoreVideoSource::subscribe()
{
    QMutexLocker<QMutex> locker(&biglock);
    ++subscribers;
}

void CoreVideoSource::unsubscribe()
{
    biglock.lock();
    if (--subscribers == 0) {
        if (deleteOnClose) {
            biglock.unlock();
            // DANGEROUS: No member access after this point, that's why we manually unlock
            delete this;
            return;
        }
    }
    biglock.unlock();
}

/**
 * @brief Setup delete on close
 * @param If true, self-delete after the last subscriber is gone
 */
void CoreVideoSource::setDeleteOnClose(bool newstate)
{
    QMutexLocker<QMutex> locker(&biglock);
    deleteOnClose = newstate;
}

/**
 * @brief Stopping the source.
 * @see The callers in CoreAV for the rationale
 *
 * Stopping the source will block any pushFrame calls from doing anything
 */
void CoreVideoSource::stopSource()
{
    QMutexLocker<QMutex> locker(&biglock);
    stopped = true;
    emit sourceStopped();
}

void CoreVideoSource::restartSource()
{
    QMutexLocker<QMutex> locker(&biglock);
    stopped = false;
}
