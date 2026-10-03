//
// Retinal encoder handover (D-028): see handover_encoder.hpp.
//

#ifdef IS_ANDROID
#include <wrtc/video_factory/hardware/android/handover_encoder.hpp>

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <api/video/i420_buffer.h>
#include <api/video/video_frame.h>
#include <api/video_codecs/video_codec.h>
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <rtc_base/logging.h>
#include <rtc_base/time_utils.h>

namespace android {
    namespace {
        constexpr auto kSwitchWait = std::chrono::milliseconds(80);
        // Old-encoder frames held back while the switch key frame is on its way
        // (about 33 ms each); beyond this the oldest is sent anyway.
        constexpr size_t kMaxHeld = 6;

        // a is newer than b (RTP timestamp wrap-around).
        bool newer(const uint32_t a, const uint32_t b) {
            return a != b && static_cast<uint32_t>(a - b) < 0x80000000u;
        }

        // One thread per inner encoder: WebRTC's Java encoders are bound to the
        // thread that first uses them.
        class Worker {
        public:
            explicit Worker(std::string name) : thread([this, name = std::move(name)] {
                pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
                run();
            }) {}

            ~Worker() {
                {
                    std::lock_guard l(m);
                    stop = true;
                }
                cv.notify_all();
                thread.join();
            }

            void post(std::function<void()> f) {
                {
                    std::lock_guard l(m);
                    q.push_back(std::move(f));
                }
                cv.notify_one();
            }

            template <class F>
            auto invoke(F f) -> decltype(f()) {
                std::packaged_task<decltype(f())()> task(std::move(f));
                auto result = task.get_future();
                post([&task] { task(); });
                return result.get();
            }

        private:
            std::mutex m;
            std::condition_variable cv;
            std::deque<std::function<void()>> q;
            bool stop = false;
            std::thread thread;

            void run() {
                for (;;) {
                    std::function<void()> f;
                    {
                        std::unique_lock l(m);
                        cv.wait(l, [this] { return stop || !q.empty(); });
                        if (q.empty()) return;
                        f = std::move(q.front());
                        q.pop_front();
                    }
                    f();
                }
            }
        };

        class HandoverEncoder;

        struct Slot {
            int id = 0;
            int width = 0;
            int height = 0;
            std::unique_ptr<Worker> worker;
            std::unique_ptr<webrtc::VideoEncoder> encoder;
            std::unique_ptr<webrtc::EncodedImageCallback> callback;
            std::atomic<bool> ready{false};
            std::atomic<bool> failed{false};
            bool warm = false;       // its first (cold-start) key frame came out; discarded
            bool requestKey = false; // ask for the switch key frame on the next frame
            bool keySeen = false;    // the switch key frame came out
            std::optional<uint32_t> keyRequestTs; // frame the switch key frame was requested on
            int framesFed = 0;
            int64_t startedUs = 0;
            std::deque<uint32_t> inFlight; // RTP timestamps submitted, guarded by HandoverEncoder::m

            ~Slot() {
                if (encoder && worker) {
                    worker->invoke([this] {
                        encoder->Release();
                        encoder.reset();
                    });
                }
                worker.reset();
            }
        };

        class SlotCallback final : public webrtc::EncodedImageCallback {
        public:
            SlotCallback(HandoverEncoder* owner, Slot* slot) : owner(owner), slot(slot) {}
            Result OnEncodedImage(const webrtc::EncodedImage& image, const webrtc::CodecSpecificInfo* info) override;
            void OnFrameDropped(uint32_t rtpTimestamp, int spatialId, bool endOfTemporalUnit) override;

        private:
            HandoverEncoder* owner;
            Slot* slot;
        };

        // Slots are destroyed off the encoder and output threads.
        void retire(std::shared_ptr<Slot> slot) {
            if (!slot) return;
            std::thread([s = std::move(slot)]() mutable { s.reset(); }).detach();
        }

        // The frame fitted to w x h, keeping its aspect: a wider frame is
        // cropped at the centre, a narrower one gets black side bars (as the
        // viewer will show the narrower picture anyway).
        webrtc::VideoFrame fitFrame(const webrtc::VideoFrame& frame, const int w, const int h) {
            const auto src = frame.video_frame_buffer()->ToI420();
            const int sw = src->width(), sh = src->height();
            auto dst = webrtc::I420Buffer::Create(w, h);
            if (static_cast<int64_t>(sw) * h >= static_cast<int64_t>(w) * sh) {
                const int cropW = std::min(sw, static_cast<int>(static_cast<int64_t>(sh) * w / h) & ~1);
                dst->CropAndScaleFrom(*src, ((sw - cropW) / 2) & ~1, 0, cropW, sh);
            } else {
                const int fitW = std::max(2, static_cast<int>(static_cast<int64_t>(sw) * h / sh) & ~1);
                const auto scaled = webrtc::I420Buffer::Create(fitW, h);
                scaled->ScaleFrom(*src);
                webrtc::I420Buffer::SetBlack(dst.get());
                const int x = ((w - fitW) / 2) & ~1;
                for (int row = 0; row < h; row++) {
                    std::memcpy(dst->MutableDataY() + row * dst->StrideY() + x, scaled->DataY() + row * scaled->StrideY(), fitW);
                }
                for (int row = 0; row < (h + 1) / 2; row++) {
                    std::memcpy(dst->MutableDataU() + row * dst->StrideU() + x / 2, scaled->DataU() + row * scaled->StrideU(), (fitW + 1) / 2);
                    std::memcpy(dst->MutableDataV() + row * dst->StrideV() + x / 2, scaled->DataV() + row * scaled->StrideV(), (fitW + 1) / 2);
                }
            }
            webrtc::VideoFrame out = frame;
            out.set_video_frame_buffer(dst);
            return out;
        }

        class HandoverEncoder final : public webrtc::VideoEncoder {
        public:
            HandoverEncoder(const webrtc::Environment& env, webrtc::SdpVideoFormat format, webrtc::VideoEncoderFactory* factory,
                            std::unique_ptr<webrtc::VideoEncoder> first)
                : env(env), format(std::move(format)), factory(factory), first(std::move(first)) {}

            ~HandoverEncoder() override {
                std::shared_ptr<Slot> a, p;
                {
                    std::lock_guard l(m);
                    a = std::move(active);
                    p = std::move(pending);
                }
                p.reset();
                a.reset();
            }

            int InitEncode(const webrtc::VideoCodec* c, const Settings& s) override {
                std::unique_lock l(m);
                const bool releasing = releaseRequested;
                releaseRequested = false;
                const bool sameType = active && c->codecType == codec.codecType;
                codec = *c;
                settings.emplace(s);
                if (!active) {
                    auto slot = makeSlot(c->width, c->height, std::move(first));
                    const int r = slot->worker->invoke([&] {
                        const int rc = slot->encoder->InitEncode(c, s);
                        if (rc == WEBRTC_VIDEO_CODEC_OK) slot->encoder->RegisterEncodeCompleteCallback(slot->callback.get());
                        return rc;
                    });
                    if (r != WEBRTC_VIDEO_CODEC_OK) return r;
                    slot->ready = true;
                    active = std::move(slot);
                    RTC_LOG(LS_INFO) << "[Retinal handover] encoder " << active->id << " started at " << c->width << "x" << c->height;
                    return r;
                }
                std::shared_ptr<Slot> cancelled;
                if (!held.empty()) {
                    // A cancelled handover's held frames still go out, in order.
                    std::vector<Held> flush(std::make_move_iterator(held.begin()), std::make_move_iterator(held.end()));
                    held.clear();
                    forward(l, flush);
                    l.lock();
                }
                if (releasing && sameType && (c->width != active->width || c->height != active->height)) {
                    cancelled = std::move(pending);
                    startPending(*c, s);
                    l.unlock();
                    retire(std::move(cancelled));
                    return WEBRTC_VIDEO_CODEC_OK;
                }
                if (releasing && sameType && pending) {
                    // Back to the active encoder's size before the handover finished.
                    cancelled = std::move(pending);
                    RTC_LOG(LS_INFO) << "[Retinal handover] back to " << c->width << "x" << c->height << " before the switch: keeping encoder " << active->id;
                    l.unlock();
                    retire(std::move(cancelled));
                    return WEBRTC_VIDEO_CODEC_OK;
                }
                // Anything else: as before (release and initialise the active encoder).
                // Never with m held: releasing joins the encoder's output thread,
                // which may be waiting for m to deliver a frame.
                cancelled = std::move(pending);
                auto a = active;
                const webrtc::VideoCodec cc = *c;
                l.unlock();
                retire(std::move(cancelled));
                int released = WEBRTC_VIDEO_CODEC_OK;
                int r = a->worker->invoke([&] {
                    if (releasing) released = a->encoder->Release();
                    if (released != WEBRTC_VIDEO_CODEC_OK) return released;
                    const int rc = a->encoder->InitEncode(&cc, s);
                    if (rc == WEBRTC_VIDEO_CODEC_OK) a->encoder->RegisterEncodeCompleteCallback(a->callback.get());
                    return rc;
                });
                if (released != WEBRTC_VIDEO_CODEC_OK) {
                    // The old encoder did not release: never initialise it again; use a fresh one.
                    RTC_LOG(LS_WARNING) << "[Retinal handover] encoder " << a->id << " failed to release (" << released << "): replacing it";
                    std::shared_ptr<Slot> fresh;
                    {
                        std::lock_guard g(m);
                        fresh = makeSlot(cc.width, cc.height, nullptr);
                    }
                    r = fresh->encoder ? fresh->worker->invoke([&] {
                        const int rc = fresh->encoder->InitEncode(&cc, s);
                        if (rc == WEBRTC_VIDEO_CODEC_OK) fresh->encoder->RegisterEncodeCompleteCallback(fresh->callback.get());
                        return rc;
                    }) : WEBRTC_VIDEO_CODEC_ERROR;
                    std::shared_ptr<Slot> broken;
                    {
                        std::lock_guard g(m);
                        broken = std::move(active);
                        fresh->ready = true;
                        active = std::move(fresh);
                    }
                    retire(std::move(broken));
                    return r;
                }
                std::lock_guard g(m);
                a->width = cc.width;
                a->height = cc.height;
                a->inFlight.clear();
                return r;
            }

            int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override {
                std::lock_guard l(m);
                sink = callback;
                return WEBRTC_VIDEO_CODEC_OK;
            }

            int32_t Release() override {
                std::lock_guard l(m);
                // Kept running: a size change follows with InitEncode. Destruction releases everything.
                if (active) releaseRequested = true;
                return WEBRTC_VIDEO_CODEC_OK;
            }

            int32_t Encode(const webrtc::VideoFrame& frame, const std::vector<webrtc::VideoFrameType>* types) override {
                std::unique_lock l(m);
                if (!active) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
                if (pending && pending->failed) {
                    fallBackToRestart(l);
                    l.lock();
                }
                auto a = active;
                if (!pending) {
                    a->inFlight.push_back(frame.rtp_timestamp());
                    l.unlock();
                    const int r = a->worker->invoke([&] { return a->encoder->Encode(frame, types); });
                    if (r != WEBRTC_VIDEO_CODEC_OK) dropInFlight(a.get(), frame.rtp_timestamp());
                    return r;
                }
                // Handover in progress: the old encoder keeps sending (fitted
                // frames, no key frame); the new one gets the frame once ready.
                auto p = pending;
                const bool feedPending = p->ready;
                bool key = false;
                if (feedPending) {
                    if (p->requestKey && !p->keyRequestTs) p->keyRequestTs = frame.rtp_timestamp();
                    key = p->framesFed++ == 0 || p->requestKey;
                    p->requestKey = false;
                }
                std::vector<webrtc::VideoFrameType> pendingTypes{key ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta};
                a->inFlight.push_back(frame.rtp_timestamp());
                bridged++;
                l.unlock();
                const auto fitted = fitFrame(frame, a->width, a->height);
                const std::vector delta{webrtc::VideoFrameType::kVideoFrameDelta};
                const int r = a->worker->invoke([&] { return a->encoder->Encode(fitted, &delta); });
                if (r != WEBRTC_VIDEO_CODEC_OK) dropInFlight(a.get(), frame.rtp_timestamp());
                if (feedPending) p->worker->invoke([&] { return p->encoder->Encode(frame, &pendingTypes); });
                return WEBRTC_VIDEO_CODEC_OK;
            }

            void SetRates(const RateControlParameters& parameters) override {
                std::shared_ptr<Slot> a, p;
                {
                    std::lock_guard l(m);
                    rates = parameters;
                    a = active;
                    if (pending && pending->ready) p = pending;
                }
                if (a) a->worker->invoke([&] { a->encoder->SetRates(parameters); });
                if (p) p->worker->invoke([&] { p->encoder->SetRates(parameters); });
            }

            void OnPacketLossRateUpdate(const float rate) override {
                if (const auto a = current()) a->worker->post([a, rate] { a->encoder->OnPacketLossRateUpdate(rate); });
            }

            void OnRttUpdate(const int64_t rtt) override {
                if (const auto a = current()) a->worker->post([a, rtt] { a->encoder->OnRttUpdate(rtt); });
            }

            void OnLossNotification(const LossNotification& n) override {
                if (const auto a = current()) a->worker->post([a, n] { a->encoder->OnLossNotification(n); });
            }

            EncoderInfo GetEncoderInfo() const override {
                std::shared_ptr<Slot> a;
                {
                    std::lock_guard l(m);
                    a = active;
                }
                if (a && a->encoder) return a->encoder->GetEncoderInfo();
                return first ? first->GetEncoderInfo() : EncoderInfo();
            }

            webrtc::EncodedImageCallback::Result onImage(Slot* slot, const webrtc::EncodedImage& image, const webrtc::CodecSpecificInfo* info) {
                std::unique_lock l(m);
                const uint32_t ts = image.RtpTimestamp();
                if (active && slot == active.get()) {
                    popInFlight(slot, ts);
                    cv.notify_all();
                    if (cut && !newer(*cut, ts)) return webrtc::EncodedImageCallback::Result(webrtc::EncodedImageCallback::Result::OK);
                    std::vector<Held> send;
                    if (pending && pending->keyRequestTs && !newer(*pending->keyRequestTs, ts)) {
                        // The new encoder's switch key frame may be for this frame: hold it.
                        held.push_back(Held{image, info ? std::optional(*info) : std::nullopt});
                        if (held.size() <= kMaxHeld) return webrtc::EncodedImageCallback::Result(webrtc::EncodedImageCallback::Result::OK);
                        send.push_back(std::move(held.front()));
                        held.pop_front();
                    } else {
                        send.push_back(Held{image, info ? std::optional(*info) : std::nullopt});
                    }
                    return forward(l, send);
                }
                if (pending && slot == pending.get() && !slot->keySeen) {
                    // A cold encoder's first key frame comes late (S21: ~90 ms vs ~24 ms per
                    // frame once running), so it only warms the encoder up; the switch is at
                    // the next key frame, requested once it is warm.
                    if (!slot->warm) {
                        if (image.IsKey()) {
                            slot->warm = true;
                            slot->requestKey = true;
                            RTC_LOG(LS_INFO) << "[Retinal handover] encoder " << slot->id << " warm after "
                                             << (webrtc::TimeMicros() - slot->startedUs) / 1000 << " ms; requesting the switch key frame";
                        }
                        return webrtc::EncodedImageCallback::Result(webrtc::EncodedImageCallback::Result::OK);
                    }
                    if (!image.IsKey()) return webrtc::EncodedImageCallback::Result(webrtc::EncodedImageCallback::Result::OK);
                    if (lastSentTs && !newer(ts, *lastSentTs)) {
                        // Its frame already went out from the old encoder: ask again.
                        slot->requestKey = true;
                        slot->keyRequestTs.reset();
                        staleKeys++;
                        return webrtc::EncodedImageCallback::Result(webrtc::EncodedImageCallback::Result::OK);
                    }
                    slot->keySeen = true;
                    cut = ts;
                    const auto waitStart = std::chrono::steady_clock::now();
                    // Frames before the switch go first, in order.
                    cv.wait_for(l, kSwitchWait, [&] { return !active || active->inFlight.empty() || !newer(ts, active->inFlight.front()); });
                    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count();
                    std::vector<Held> send;
                    size_t dropped = 0;
                    for (auto& h : held) {
                        if (newer(ts, h.image.RtpTimestamp())) send.push_back(std::move(h));
                        else dropped++;
                    }
                    held.clear();
                    send.push_back(Held{image, info ? std::optional(*info) : std::nullopt});
                    auto old = std::move(active);
                    active = std::move(pending);
                    cut.reset();
                    RTC_LOG(LS_INFO) << "[Retinal handover] switched encoder " << old->id << " " << old->width << "x" << old->height
                                     << " -> " << active->id << " " << active->width << "x" << active->height << " after "
                                     << (webrtc::TimeMicros() - active->startedUs) / 1000 << " ms (" << bridged << " frames bridged, waited "
                                     << waited << " ms for the old encoder, key frame " << image.size() << " bytes, "
                                     << send.size() - 1 << " held frames sent, " << dropped << " dropped, " << staleKeys << " stale key frames)";
                    bridged = 0;
                    staleKeys = 0;
                    auto result = forward(l, send);
                    retire(std::move(old));
                    return result;
                }
                // A cancelled or retired encoder, or a pending one after its key frame was taken.
                return webrtc::EncodedImageCallback::Result(webrtc::EncodedImageCallback::Result::OK);
            }

            void onDropped(Slot* slot, const uint32_t ts, const int spatialId, const bool end) {
                std::unique_lock l(m);
                if (!active || slot != active.get()) return;
                popInFlight(slot, ts);
                cv.notify_all();
                if (cut && !newer(*cut, ts)) return;
                auto* out = sink;
                l.unlock();
                if (out) out->OnFrameDropped(ts, spatialId, end);
            }

        private:
            const webrtc::Environment env;
            const webrtc::SdpVideoFormat format;
            webrtc::VideoEncoderFactory* const factory;
            std::unique_ptr<webrtc::VideoEncoder> first;

            mutable std::mutex m;
            std::condition_variable cv;
            std::shared_ptr<Slot> active, pending;
            webrtc::EncodedImageCallback* sink = nullptr;
            webrtc::VideoCodec codec;
            std::optional<Settings> settings;
            std::optional<RateControlParameters> rates;
            std::optional<uint32_t> cut;
            bool releaseRequested = false;
            int nextId = 1;
            int bridged = 0;
            int staleKeys = 0;
            struct Held {
                webrtc::EncodedImage image;
                std::optional<webrtc::CodecSpecificInfo> info;
            };
            std::deque<Held> held;
            std::optional<uint32_t> lastSentTs;
            std::mutex sendMutex; // keeps frames in decision order on their way to WebRTC

            // Called with m held; releases it. Sends in order, after any earlier decision.
            webrtc::EncodedImageCallback::Result forward(std::unique_lock<std::mutex>& l, std::vector<Held>& frames) {
                auto* out = sink;
                for (const auto& f : frames) lastSentTs = f.image.RtpTimestamp();
                std::lock_guard send(sendMutex);
                l.unlock();
                webrtc::EncodedImageCallback::Result result(webrtc::EncodedImageCallback::Result::OK);
                for (const auto& f : frames) {
                    result = out ? out->OnEncodedImage(f.image, f.info ? &*f.info : nullptr)
                                 : webrtc::EncodedImageCallback::Result(webrtc::EncodedImageCallback::Result::ERROR_SEND_FAILED);
                }
                return result;
            }

            std::shared_ptr<Slot> current() {
                std::lock_guard l(m);
                return active;
            }

            std::shared_ptr<Slot> makeSlot(const int w, const int h, std::unique_ptr<webrtc::VideoEncoder> encoder) {
                auto slot = std::make_shared<Slot>();
                slot->id = nextId++;
                slot->width = w;
                slot->height = h;
                slot->startedUs = webrtc::TimeMicros();
                slot->worker = std::make_unique<Worker>("RetinalEnc" + std::to_string(slot->id));
                slot->encoder = encoder ? std::move(encoder) : factory->Create(env, format);
                slot->callback = std::make_unique<SlotCallback>(this, slot.get());
                return slot;
            }

            // Called with m held.
            void startPending(const webrtc::VideoCodec& c, const Settings& s) {
                pending = makeSlot(c.width, c.height, nullptr);
                bridged = 0;
                Slot* p = pending.get();
                if (!p->encoder) {
                    p->failed = true;
                    return;
                }
                RTC_LOG(LS_INFO) << "[Retinal handover] " << active->width << "x" << active->height << " -> " << c.width << "x" << c.height
                                 << ": starting encoder " << p->id << " while encoder " << active->id << " keeps sending";
                p->worker->post([this, p, c, s] {
                    const int rc = p->encoder->InitEncode(&c, s);
                    if (rc != WEBRTC_VIDEO_CODEC_OK) {
                        RTC_LOG(LS_WARNING) << "[Retinal handover] encoder " << p->id << " failed to start (" << rc << ")";
                        p->failed = true;
                        return;
                    }
                    p->encoder->RegisterEncodeCompleteCallback(p->callback.get());
                    std::optional<RateControlParameters> r;
                    {
                        std::lock_guard l(m);
                        r = rates;
                    }
                    if (r) p->encoder->SetRates(*r);
                    p->ready = true;
                    RTC_LOG(LS_INFO) << "[Retinal handover] encoder " << p->id << " started in " << (webrtc::TimeMicros() - p->startedUs) / 1000 << " ms";
                });
            }

            // Handover failed: restart the active encoder at the new size (stock behaviour).
            void fallBackToRestart(std::unique_lock<std::mutex>& l) {
                auto failed = std::move(pending);
                auto a = active;
                const auto c = codec;
                const auto s = *settings;
                RTC_LOG(LS_WARNING) << "[Retinal handover] falling back to restarting encoder " << a->id;
                l.unlock();
                retire(std::move(failed));
                a->worker->invoke([&] {
                    a->encoder->Release();
                    if (a->encoder->InitEncode(&c, s) == WEBRTC_VIDEO_CODEC_OK) a->encoder->RegisterEncodeCompleteCallback(a->callback.get());
                });
                std::lock_guard g(m);
                a->width = c.width;
                a->height = c.height;
                a->inFlight.clear();
            }

            void popInFlight(Slot* slot, const uint32_t ts) {
                while (!slot->inFlight.empty() && !newer(slot->inFlight.front(), ts)) slot->inFlight.pop_front();
            }

            void dropInFlight(Slot* slot, const uint32_t ts) {
                std::lock_guard l(m);
                std::erase(slot->inFlight, ts);
                cv.notify_all();
            }
        };

        webrtc::EncodedImageCallback::Result SlotCallback::OnEncodedImage(const webrtc::EncodedImage& image, const webrtc::CodecSpecificInfo* info) {
            return owner->onImage(slot, image, info);
        }

        void SlotCallback::OnFrameDropped(const uint32_t rtpTimestamp, const int spatialId, const bool endOfTemporalUnit) {
            owner->onDropped(slot, rtpTimestamp, spatialId, endOfTemporalUnit);
        }
    } // namespace

    HandoverEncoderFactory::HandoverEncoderFactory(std::unique_ptr<webrtc::VideoEncoderFactory> inner) : inner(std::move(inner)) {}

    std::vector<webrtc::SdpVideoFormat> HandoverEncoderFactory::GetSupportedFormats() const {
        return inner->GetSupportedFormats();
    }

    std::vector<webrtc::SdpVideoFormat> HandoverEncoderFactory::GetImplementations() const {
        return inner->GetImplementations();
    }

    webrtc::VideoEncoderFactory::CodecSupport HandoverEncoderFactory::QueryCodecSupport(
        const webrtc::SdpVideoFormat& format, std::optional<std::string> scalabilityMode, std::optional<webrtc::Resolution> resolution) const {
        return inner->QueryCodecSupport(format, std::move(scalabilityMode), resolution);
    }

    std::unique_ptr<webrtc::VideoEncoder> HandoverEncoderFactory::Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) {
        auto encoder = inner->Create(env, format);
        if (!encoder) return encoder;
        RTC_LOG(LS_INFO) << "[Retinal handover] " << format.name << " encoder with handover on frame-size changes";
        return std::make_unique<HandoverEncoder>(env, format, inner.get(), std::move(encoder));
    }

    std::unique_ptr<webrtc::VideoEncoderFactory::EncoderSelectorInterface> HandoverEncoderFactory::GetEncoderSelector() const {
        return inner->GetEncoderSelector();
    }
} // android
#endif
