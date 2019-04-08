/*!
 *  Copyright (c) 2019 by Contributors
 * \file video_reader.cc
 * \brief FFmpeg video reader Impl
 */

#include "video_reader.h"

#include <decord/runtime/ndarray.h>

namespace decord {
namespace ffmpeg {

using NDArray = runtime::NDArray;

NDArray CopyToNDArray(AVFrame *p) {
    CHECK(p) << "Error: converting empty AVFrame to DLTensor";
    // int channel = p->linesize[0] / p->width;
    CHECK_EQ(AVPixelFormat(p->format), AV_PIX_FMT_RGB24) 
        << "Only support RGB24 image to NDArray conversion, given: " 
        << AVPixelFormat(p->format);
    
    DLContext ctx;
    if (p->hw_frames_ctx) {
        ctx = DLContext({kDLGPU, 0});
    } else {
        ctx = kCPU;
    }
    // LOG(INFO) << p->height << " x";
    DLTensor dlt;
    std::vector<int64_t> shape = {p->height, p->width, p->linesize[0] / p->width};
    dlt.data = p->data[0];
    dlt.ctx = ctx;
    dlt.ndim = 3;
    dlt.dtype = kUInt8;
    dlt.shape = dmlc::BeginPtr(shape);
    dlt.strides = NULL;
    dlt.byte_offset = 0;
    NDArray arr = NDArray::Empty({p->height, p->width, p->linesize[0] / p->width}, kUInt8, ctx);
    arr.CopyFrom(&dlt);
    return arr;
}

FFMPEGVideoReader::FFMPEGVideoReader(std::string fn, int width, int height)
     : codecs_(), actv_stm_idx_(-1), decoder_(), curr_frame_(0), width_(width), height_(height), eof_(false) {
    // allocate format context
    fmt_ctx_.reset(avformat_alloc_context());
    if (!fmt_ctx_) {
        LOG(FATAL) << "ERROR allocating memory for Format Context";
    }
    // LOG(INFO) << "opened fmt ctx";
    // open file
    auto fmt_ctx = fmt_ctx_.get();
    if(avformat_open_input(&fmt_ctx, fn.c_str(), NULL, NULL) != 0 ) {
        LOG(FATAL) << "ERROR opening file: " << fn;
    }

    LOG(INFO) << "opened input";

    // find stream info
    if (avformat_find_stream_info(fmt_ctx,  NULL) < 0) {
        LOG(FATAL) << "ERROR getting stream info of file" << fn;
    }

    LOG(INFO) << "find stream info";

    // initialize all video streams and store codecs info
    for (uint32_t i = 0; i < fmt_ctx_->nb_streams; ++i) {
        AVStream *st = fmt_ctx_->streams[i];
        AVCodec *local_codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            // store video stream codecs only
            codecs_.emplace_back(local_codec);
        } else {
            // audio, subtitle... skip
            AVCodec *tmp = NULL;
            codecs_.emplace_back(tmp);
        }
    }
    LOG(INFO) << "initialized all streams";
    // find best video stream (-1 means auto, relay on FFMPEG)
    SetVideoStream(-1);
    LOG(INFO) << "Set video stream";
    decoder_->Start();

    // // allocate AVFrame buffer
    // frame_ = av_frame_alloc();
    // CHECK(frame_) << "ERROR failed to allocated memory for AVFrame";

    // // allocate AVPacket buffer
    // pkt_ = av_packet_alloc();
    // CHECK(pkt_) << "ERROR failed to allocated memory for AVPacket";
}

FFMPEGVideoReader::~FFMPEGVideoReader(){
    // avformat_free_context(fmt_ctx_);
    // avformat_close_input(&fmt_ctx_);
    LOG(INFO) << "Destruct Video REader";
}

void FFMPEGVideoReader::SetVideoStream(int stream_nb) {
    CHECK(fmt_ctx_ != NULL);
    AVCodec *dec;
    int st_nb = av_find_best_stream(fmt_ctx_.get(), AVMEDIA_TYPE_VIDEO, stream_nb, -1, &dec, 0);
    LOG(INFO) << "find best stream: " << st_nb;
    CHECK_GE(st_nb, 0) << "ERROR cannot find video stream with wanted index: " << stream_nb;
    // initialize the mem for codec context
    CHECK(codecs_[st_nb] == dec) << "Codecs of " << st_nb << " is NULL";
    LOG(INFO) << "codecs of stream: " << codecs_[st_nb] << " name: " <<  codecs_[st_nb]->name;
    decoder_ = std::unique_ptr<FFMPEGThreadedDecoder>(new FFMPEGThreadedDecoder());
    auto dec_ctx = avcodec_alloc_context3(dec);
    // CHECK_GE(avcodec_copy_context(dec_ctx, fmt_ctx_->streams[stream_nb]->codec), 0) << "Error: copy context";
    // CHECK_GE(avcodec_parameters_to_context(dec_ctx, fmt_ctx_->streams[st_nb]->codecpar), 0) << "Error: copy parameters to codec context.";
    // copy codec parameters to context
    CHECK_GE(avcodec_parameters_to_context(dec_ctx, fmt_ctx_->streams[st_nb]->codecpar), 0)
        << "ERROR copying codec parameters to context";
    // initialize AVCodecContext to use given AVCodec
    CHECK_GE(avcodec_open2(dec_ctx, codecs_[st_nb], NULL), 0)
        << "ERROR open codec through avcodec_open2";
    LOG(INFO) << "codecs opened.";
    actv_stm_idx_ = st_nb;
    LOG(INFO) << "time base: " << fmt_ctx_->streams[st_nb]->time_base.num << " / " << fmt_ctx_->streams[st_nb]->time_base.den;
    dec_ctx->time_base = fmt_ctx_->streams[st_nb]->time_base;
    char descr[128];
    std::snprintf(descr, sizeof(descr),
            "scale=%d:%d", width_, height_);
    decoder_->SetCodecContext(dec_ctx, std::string(descr));
    IndexKeyframes();
    LOG(INFO) << "Printing key frames...";
    for (auto i : key_indices_) {
        LOG(INFO) << i;
    }
}

unsigned int FFMPEGVideoReader::QueryStreams() const {
    CHECK(fmt_ctx_ != NULL);
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
        // iterate and print stream info
        // feel free to add more if needed
        AVStream *st = fmt_ctx_->streams[i];
        AVCodec *codec = codecs_[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            LOG(INFO) << "video stream [" << i << "]:"
                << " Average FPS: " 
                << static_cast<float>(st->avg_frame_rate.num) / st->avg_frame_rate.den
                << " Start time: "
                << st->start_time
                << " Duration: "
                << st->duration
                << " Codec Type: "
                << codec->name
                << " ID: "
                << codec->id
                << " bit_rate: "
                << st->codecpar->bit_rate
                << " Resolution: "
                << st->codecpar->width << "x" << st->codecpar->height
                << " Frame count: "
                << st->nb_frames;
        } else {
            const char *codec_type = av_get_media_type_string(st->codecpar->codec_type);
            codec_type = codec_type? codec_type : "unknown type";
            LOG(INFO) << codec_type << " stream [" << i << "].";
        }
        
    }
    return fmt_ctx_->nb_streams;
}

int64_t FFMPEGVideoReader::GetFrameCount() const {
   CHECK(fmt_ctx_ != NULL);
   CHECK(actv_stm_idx_ >= 0 && actv_stm_idx_ < fmt_ctx_->nb_streams);
   int64_t cnt = fmt_ctx_->streams[actv_stm_idx_]->nb_frames;
   if (cnt < 1) {
       AVStream *stm = fmt_ctx_->streams[actv_stm_idx_];
       // many formats do not provide accurate frame count, use duration and FPS to approximate
       cnt = static_cast<double>(stm->avg_frame_rate.num) / stm->avg_frame_rate.den * fmt_ctx_->duration / AV_TIME_BASE;
   }
   return cnt;
}

bool FFMPEGVideoReader::Seek(int64_t pos) {
    decoder_->Clear();
    eof_ = false;
    int64_t ts = pos * fmt_ctx_->streams[actv_stm_idx_]->duration / GetFrameCount();
    int ret = avformat_seek_file(fmt_ctx_.get(), actv_stm_idx_, 
                                ts-1, ts, ts+1, 
                                AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME);
    // int tm = av_rescale(pos, fmt_ctx_->streams[actv_stm_idx_]->time_base.den, fmt_ctx_->streams[actv_stm_idx_]->time_base.num) / 1000;
    // LOG(INFO) << "TM: " << tm;
    // int ret = avformat_seek_file(fmt_ctx_.get(), actv_stm_idx_, 
    //                             tm, tm, tm, 
    //                             0);
    if (ret < 0) LOG(WARNING) << "Failed to seek file to position: " << pos;
    LOG(INFO) << "seek return: " << ret;
    decoder_->Start();
    if (ret >= 0) {
        curr_frame_ = pos;
    }
    return ret >= 0;
}

int64_t FFMPEGVideoReader::LocateKeyframe(int64_t pos) {
    if (key_indices_.size() < 1) return 0;
    if (pos <= key_indices_[0]) return 0;
    if (pos >= GetFrameCount()) return key_indices_.back();
    auto it = std::upper_bound(key_indices_.begin(), key_indices_.end(), pos) - 1;
    return *it;
}

bool FFMPEGVideoReader::SeekAccurate(int64_t pos) {
    int64_t key_pos = LocateKeyframe(pos);
    LOG(INFO) << "Accurate seek to " << pos << " with key pos: " << key_pos;
    bool ret = Seek(key_pos);
    if (!ret) return false;
    SkipFrames(pos - key_pos);
    return true;
}

void FFMPEGVideoReader::PushNext() {
    // AVPacket *packet = av_packet_alloc();
    // AVPacketPtr packet = AVPacketPool::Get()->Acquire();
    AVPacketPtr packet = AVPacketPtr(av_packet_alloc());
    int ret = -1;
    while (!eof_) {
        ret = av_read_frame(fmt_ctx_.get(), packet.get());
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                eof_ = true;
                return;
            } else {
                LOG(FATAL) << "Error: av_read_frame failed with " << AVERROR(ret);
            }
            return;
        }
        if (packet->stream_index == actv_stm_idx_) {
            // LOG(INFO) << "Packet index: " << packet->stream_index << " vs. " << actv_stm_idx_;
            // av_packet_unref(packet);
            // LOG(INFO) << "Successfully load packet";
            decoder_->Push(packet);
            // LOG(INFO) << "Pushed packet to decoder.";
            break;
        }
    }
}

NDArray FFMPEGVideoReader::NextFrame() {
    AVFramePtr frame;
    decoder_->Start();
    bool ret = false;
    while (!ret) {
        PushNext();
        ret = decoder_->Pop(&frame);
        if (!ret && eof_) {
            return NDArray::Empty({}, kUInt8, kCPU);
        }
    }
    
    // int ret = decoder_->Pop(&frame);
    // if (!ret) {
    //     return NDArray::Empty({}, kUInt8, kCPU);
    // }
    NDArray arr = CopyToNDArray(frame.get());
    ++curr_frame_;
    // av_frame_free(&frame);
    return arr;
}

void FFMPEGVideoReader::IndexKeyframes() {
    Seek(0);
    key_indices_.clear();
    // AVPacketPtr packet = AVPacketPool::Get()->Acquire();
    AVPacketPtr packet = AVPacketPtr(av_packet_alloc());
    int ret = -1;
    bool eof = false;
    int64_t cnt = 0;
    while (!eof) {
        ret = av_read_frame(fmt_ctx_.get(), packet.get());
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                eof = true;
                break;
            } else {
                LOG(FATAL) << "Error: av_read_frame failed with " << AVERROR(ret);
            }
            break;
        }
        if (packet->stream_index == actv_stm_idx_) {
            if (packet->flags & AV_PKT_FLAG_KEY) {
                key_indices_.emplace_back(cnt);
            }
            ++cnt;
        }
    }
    Seek(0);
}

runtime::NDArray FFMPEGVideoReader::GetKeyIndices() {
    DLManagedTensor dlt;
    dlt.dl_tensor.data = dmlc::BeginPtr(key_indices_);
    dlt.dl_tensor.dtype = kInt64;
    dlt.dl_tensor.ctx = kCPU;
    std::vector<int64_t> shape = {static_cast<int64_t>(key_indices_.size())};
    dlt.dl_tensor.shape = dmlc::BeginPtr(shape);
    dlt.dl_tensor.ndim = 1;
    dlt.dl_tensor.byte_offset = 0;
    dlt.deleter = nullptr;
    dlt.manager_ctx = nullptr;
    runtime::NDArray orig = runtime::NDArray::FromDLPack(&dlt);
    runtime::NDArray ret = runtime::NDArray::Empty(shape, kInt64, kCPU);
    LOG(INFO) << "begin copy!";
    ret.CopyFrom(orig);
    LOG(INFO) << "copied!";
    return ret;
}

std::vector<int64_t> FFMPEGVideoReader::GetKeyIndicesVector() const {
    return key_indices_;
}

void FFMPEGVideoReader::SkipFrames(int64_t num) {
    // check if skip pass keyframes, if so, we can seek to latest keyframe first
    LOG(INFO) << " Skip Frame start: " << num << " current frame: " << curr_frame_;
    if (num < 1) return;
    num = std::min(GetFrameCount() - curr_frame_, num);
    auto it1 = std::upper_bound(key_indices_.begin(), key_indices_.end(), curr_frame_) - 1;
    CHECK_GE(it1 - key_indices_.begin(), 0);
    auto it2 = std::upper_bound(key_indices_.begin(), key_indices_.end(), curr_frame_ + num) - 1;
    CHECK_GE(it2 - key_indices_.begin(), 0);
    LOG(INFO) << "first: " << it1 - key_indices_.begin() << " second: " << it2 - key_indices_.begin() << ", " << *it1 << ", " << *it2;
    if (it2 > it1) {
        LOG(INFO) << "Seek to frame: " << *it2;
        Seek(*it2);
        num += curr_frame_ - *it2;
    }

    LOG(INFO) << "started skipping with: " << num;
    AVFramePtr frame;
    decoder_->Start();
    bool ret = false;
    while ((!eof_) && num > 0) {
        PushNext();
        ret = decoder_->Pop(&frame);
        if (!ret) continue;
        ++curr_frame_;
        // LOG(INFO) << "skip: " << num;
        --num;
    }
    LOG(INFO) << " stopped skipframes";
}

}  // namespace ffmpeg
}  // namespace decord