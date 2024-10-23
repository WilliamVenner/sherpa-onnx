// sherpa-onnx/csrc/speaker-embedding-extractor-nemo-impl.h
//
// Copyright (c)  2024  Xiaomi Corporation

#ifndef SHERPA_ONNX_CSRC_SPEAKER_EMBEDDING_EXTRACTOR_NEMO_IMPL_H_
#define SHERPA_ONNX_CSRC_SPEAKER_EMBEDDING_EXTRACTOR_NEMO_IMPL_H_
#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "Eigen/Dense"
#include "sherpa-onnx/csrc/speaker-embedding-extractor-impl.h"
#include "sherpa-onnx/csrc/speaker-embedding-extractor-nemo-model.h"
#include "sherpa-onnx/csrc/transpose.h"

namespace sherpa_onnx {

class SpeakerEmbeddingExtractorNeMoImpl : public SpeakerEmbeddingExtractorImpl {
 public:
  explicit SpeakerEmbeddingExtractorNeMoImpl(
      const SpeakerEmbeddingExtractorConfig &config)
      : model_(config) {}

#if __ANDROID_API__ >= 9
  SpeakerEmbeddingExtractorNeMoImpl(
      AAssetManager *mgr, const SpeakerEmbeddingExtractorConfig &config)
      : model_(mgr, config) {}
#endif

  int32_t Dim() const override { return model_.GetMetaData().output_dim; }

  std::unique_ptr<OnlineStream> CreateStream() const override {
    FeatureExtractorConfig feat_config;
    const auto &meta_data = model_.GetMetaData();
    feat_config.sampling_rate = meta_data.sample_rate;
    feat_config.feature_dim = meta_data.feat_dim;
    feat_config.normalize_samples = true;
    feat_config.snip_edges = true;
    feat_config.frame_shift_ms = meta_data.window_stride_ms;
    feat_config.frame_length_ms = meta_data.window_size_ms;
    feat_config.low_freq = 0;
    feat_config.is_librosa = true;
    feat_config.remove_dc_offset = false;
    feat_config.window_type = meta_data.window_type;

    return std::make_unique<OnlineStream>(feat_config);
  }

  bool IsReady(OnlineStream *s) const override {
    return s->GetNumProcessedFrames() < s->NumFramesReady();
  }

  std::vector<float> Compute(OnlineStream *s) const override {
    int32_t num_frames = s->NumFramesReady() - s->GetNumProcessedFrames();
    if (num_frames <= 0) {
      SHERPA_ONNX_LOGE(
          "Please make sure IsReady(s) returns true. num_frames: %d",
          num_frames);
      return {};
    }

    std::vector<float> features =
        s->GetFrames(s->GetNumProcessedFrames(), num_frames);

    s->GetNumProcessedFrames() += num_frames;

    int32_t feat_dim = features.size() / num_frames;

    const auto &meta_data = model_.GetMetaData();
    if (!meta_data.feature_normalize_type.empty()) {
      if (meta_data.feature_normalize_type == "per_feature") {
        NormalizePerFeature(features.data(), num_frames, feat_dim);
      } else {
        SHERPA_ONNX_LOGE("Unsupported feature_normalize_type: %s",
                         meta_data.feature_normalize_type.c_str());
        exit(-1);
      }
    }

    if (num_frames % 16 != 0) {
      int32_t pad = 16 - num_frames % 16;
      features.resize((num_frames + pad) * feat_dim);
    }

    auto memory_info =
        Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);

    std::array<int64_t, 3> x_shape{1, num_frames, feat_dim};
    Ort::Value x =
        Ort::Value::CreateTensor(memory_info, features.data(), features.size(),
                                 x_shape.data(), x_shape.size());

    x = Transpose12(model_.Allocator(), &x);

    int64_t x_lens = num_frames;
    std::array<int64_t, 1> x_lens_shape{1};
    Ort::Value x_lens_tensor = Ort::Value::CreateTensor(
        memory_info, &x_lens, 1, x_lens_shape.data(), x_lens_shape.size());

    Ort::Value embedding =
        model_.Compute(std::move(x), std::move(x_lens_tensor));
    std::vector<int64_t> embedding_shape =
        embedding.GetTensorTypeAndShapeInfo().GetShape();

    std::vector<float> ans(embedding_shape[1]);
    std::copy(embedding.GetTensorData<float>(),
              embedding.GetTensorData<float>() + ans.size(), ans.begin());

    return ans;
  }

 private:
  void NormalizePerFeature(float *p, int32_t num_frames,
                           int32_t feat_dim) const {
    // Vectors to store sums for each feature
    std::vector<float> mean(feat_dim, 0.0f);

    // First pass: Compute the mean for each feature
    for (int32_t i = 0; i < num_frames; ++i) {
      for (int32_t j = 0; j < feat_dim; ++j) {
        mean[j] += p[i * feat_dim + j];
      }
    }
    for (int32_t j = 0; j < feat_dim; ++j) {
      mean[j] /= num_frames;
    }

    // Second pass: Compute the variance for each feature
    std::vector<float> variance(feat_dim, 0.0f);
    for (int32_t i = 0; i < num_frames; ++i) {
      for (int32_t j = 0; j < feat_dim; ++j) {
        float diff = p[i * feat_dim + j] - mean[j];
        variance[j] += diff * diff;
      }
    }
    for (int32_t j = 0; j < feat_dim; ++j) {
      variance[j] /= num_frames;
    }

    // Compute standard deviation, ensuring it's not zero
    std::vector<float> stddev(feat_dim);
    for (int32_t j = 0; j < feat_dim; ++j) {
      // Add a small epsilon to variance to avoid division by zero
      stddev[j] = std::sqrt(std::max(variance[j], 0.0f) + 1e-8f);
    }

    // Normalize the data
    for (int32_t i = 0; i < num_frames; ++i) {
      for (int32_t j = 0; j < feat_dim; ++j) {
        p[i * feat_dim + j] = (p[i * feat_dim + j] - mean[j]) / stddev[j];
      }
    }
  }

 private:
  SpeakerEmbeddingExtractorNeMoModel model_;
};

}  // namespace sherpa_onnx

#endif  // SHERPA_ONNX_CSRC_SPEAKER_EMBEDDING_EXTRACTOR_NEMO_IMPL_H_
