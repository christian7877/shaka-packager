// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/mp4/mp4_media_parser.h"

#include "base/callback.h"
#include "base/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/time/time.h"
#include "media/base/audio_stream_info.h"
#include "media/base/buffers.h"
#include "media/base/media_sample.h"
#include "media/base/video_stream_info.h"
#include "media/mp4/box_definitions.h"
#include "media/mp4/box_reader.h"
#include "media/mp4/es_descriptor.h"
#include "media/mp4/rcheck.h"
#include "media/mp4/track_run_iterator.h"

namespace {

base::TimeDelta TimeDeltaFromRational(int64 numer, int64 denom) {
  DCHECK_LT((numer > 0 ? numer : -numer),
            kint64max / base::Time::kMicrosecondsPerSecond);
  return base::TimeDelta::FromMicroseconds(base::Time::kMicrosecondsPerSecond *
                                           numer / denom);
}

}  // namespace

namespace media {
namespace mp4 {

MP4MediaParser::MP4MediaParser()
    : state_(kWaitingForInit),
      moof_head_(0),
      mdat_tail_(0),
      has_audio_(false),
      has_video_(false),
      audio_track_id_(0),
      video_track_id_(0),
      // TODO(kqyang): do we need to care about it??
      has_sbr_(false),
      is_audio_track_encrypted_(false),
      is_video_track_encrypted_(false) {
}

MP4MediaParser::~MP4MediaParser() {}

void MP4MediaParser::Init(const InitCB& init_cb,
                          const NewSampleCB& new_sample_cb,
                          const NeedKeyCB& need_key_cb) {
  DCHECK_EQ(state_, kWaitingForInit);
  DCHECK(init_cb_.is_null());
  DCHECK(!init_cb.is_null());
  DCHECK(!new_sample_cb.is_null());
  DCHECK(!need_key_cb.is_null());

  ChangeState(kParsingBoxes);
  init_cb_ = init_cb;
  new_sample_cb_ = new_sample_cb;
  need_key_cb_ = need_key_cb;
}

void MP4MediaParser::Reset() {
  queue_.Reset();
  runs_.reset();
  moof_head_ = 0;
  mdat_tail_ = 0;
}

bool MP4MediaParser::Parse(const uint8* buf, int size) {
  DCHECK_NE(state_, kWaitingForInit);

  if (state_ == kError)
    return false;

  queue_.Push(buf, size);

  bool result, err = false;

  do {
    if (state_ == kParsingBoxes) {
      result = ParseBox(&err);
    } else {
      DCHECK_EQ(kEmittingSamples, state_);
      result = EnqueueSample(&err);
      if (result) {
        int64 max_clear = runs_->GetMaxClearOffset() + moof_head_;
        err = !ReadAndDiscardMDATsUntil(max_clear);
      }
    }
  } while (result && !err);

  if (err) {
    DLOG(ERROR) << "Error while parsing MP4";
    moov_.reset();
    Reset();
    ChangeState(kError);
    return false;
  }

  return true;
}

bool MP4MediaParser::ParseBox(bool* err) {
  const uint8* buf;
  int size;
  queue_.Peek(&buf, &size);
  if (!size) return false;

  scoped_ptr<BoxReader> reader(BoxReader::ReadTopLevelBox(buf, size, err));
  if (reader.get() == NULL) return false;

  if (reader->type() == FOURCC_MOOV) {
    *err = !ParseMoov(reader.get());
  } else if (reader->type() == FOURCC_MOOF) {
    moof_head_ = queue_.head();
    *err = !ParseMoof(reader.get());

    // Set up first mdat offset for ReadMDATsUntil().
    mdat_tail_ = queue_.head() + reader->size();

    // Return early to avoid evicting 'moof' data from queue. Auxiliary info may
    // be located anywhere in the file, including inside the 'moof' itself.
    // (Since 'default-base-is-moof' is mandated, no data references can come
    // before the head of the 'moof', so keeping this box around is sufficient.)
    return !(*err);
  } else {
    LOG(WARNING) << "Skipping unrecognized top-level box: "
                 << FourCCToString(reader->type());
  }

  queue_.Pop(reader->size());
  return !(*err);
}


bool MP4MediaParser::ParseMoov(BoxReader* reader) {
  moov_.reset(new Movie);
  RCHECK(moov_->Parse(reader));
  runs_.reset();

  has_audio_ = false;
  has_video_ = false;

  std::vector<scoped_refptr<StreamInfo> > streams;

  for (std::vector<Track>::const_iterator track = moov_->tracks.begin();
       track != moov_->tracks.end(); ++track) {
    // TODO(strobe): Only the first audio and video track present in a file are
    // used. (Track selection is better accomplished via Source IDs, though, so
    // adding support for track selection within a stream is low-priority.)
    const SampleDescription& samp_descr =
        track->media.information.sample_table.description;

    // TODO(strobe): When codec reconfigurations are supported, detect and send
    // a codec reconfiguration for fragments using a sample description index
    // different from the previous one
    size_t desc_idx = 0;
    for (size_t t = 0; t < moov_->extends.tracks.size(); t++) {
      const TrackExtends& trex = moov_->extends.tracks[t];
      if (trex.track_id == track->header.track_id) {
        desc_idx = trex.default_sample_description_index;
        break;
      }
    }
    RCHECK(desc_idx > 0);
    desc_idx -= 1;  // BMFF descriptor index is one-based

    if (track->media.handler.type == kAudio) {
      // TODO(kqyang): do we need to support multiple audio or video streams in
      // a single file?
      RCHECK(!has_audio_);

      RCHECK(!samp_descr.audio_entries.empty());

      // It is not uncommon to find otherwise-valid files with incorrect sample
      // description indices, so we fail gracefully in that case.
      if (desc_idx >= samp_descr.audio_entries.size())
        desc_idx = 0;
      const AudioSampleEntry& entry = samp_descr.audio_entries[desc_idx];
      const AAC& aac = entry.esds.aac;

      if (!(entry.format == FOURCC_MP4A || entry.format == FOURCC_EAC3 ||
            (entry.format == FOURCC_ENCA &&
             entry.sinf.format.format == FOURCC_MP4A))) {
        LOG(ERROR) << "Unsupported audio format 0x"
                   << std::hex << entry.format << " in stsd box.";
        return false;
      }

      ObjectType audio_type = static_cast<ObjectType>(entry.esds.object_type);
      DVLOG(1) << "audio_type " << std::hex << audio_type;
      if (audio_type == kForbidden && entry.format == FOURCC_EAC3) {
        audio_type = kEAC3;
      }

      AudioCodec codec = kUnknownAudioCodec;
      int num_channels = 0;
      int sample_per_second = 0;
      std::vector<uint8> extra_data;
      // Check if it is MPEG4 AAC defined in ISO 14496 Part 3 or
      // supported MPEG2 AAC varients.
      if (ESDescriptor::IsAAC(audio_type)) {
        codec = kCodecAAC;
        num_channels = aac.GetNumChannels(has_sbr_);
        sample_per_second = aac.GetOutputSamplesPerSecond(has_sbr_);
#if defined(OS_ANDROID)
        extra_data = aac.codec_specific_data();
#endif
      } else if (audio_type == kEAC3) {
        codec = kCodecEAC3;
        num_channels = entry.channelcount;
        sample_per_second = entry.samplerate;
      } else {
        LOG(ERROR) << "Unsupported audio object type 0x"
                   << std::hex << audio_type << " in esds.";
        return false;
      }

      is_audio_track_encrypted_ = entry.sinf.info.track_encryption.is_encrypted;
      DVLOG(1) << "is_audio_track_encrypted_: " << is_audio_track_encrypted_;
      streams.push_back(
          new AudioStreamInfo(track->header.track_id,
                              track->media.header.timescale,
                              codec,
                              entry.samplesize / 8,
                              num_channels,
                              sample_per_second,
                              extra_data.size() ? &extra_data[0] : NULL,
                              extra_data.size(),
                              is_audio_track_encrypted_));
      has_audio_ = true;
      audio_track_id_ = track->header.track_id;
    }

    if (track->media.handler.type == kVideo) {
      // TODO(kqyang): do we need to support multiple audio or video streams in
      // a single file?
      RCHECK(!has_video_);

      RCHECK(!samp_descr.video_entries.empty());
      if (desc_idx >= samp_descr.video_entries.size())
        desc_idx = 0;
      const VideoSampleEntry& entry = samp_descr.video_entries[desc_idx];

      if (!(entry.format == FOURCC_AVC1 ||
            (entry.format == FOURCC_ENCV &&
             entry.sinf.format.format == FOURCC_AVC1))) {
        LOG(ERROR) << "Unsupported video format 0x"
                   << std::hex << entry.format << " in stsd box.";
        return false;
      }

      is_video_track_encrypted_ = entry.sinf.info.track_encryption.is_encrypted;
      DVLOG(1) << "is_video_track_encrypted_: " << is_video_track_encrypted_;
      streams.push_back(
          new VideoStreamInfo(track->header.track_id,
                              track->media.header.timescale,
                              kCodecH264,
                              entry.width,
                              entry.height,
                              // No decoder-specific buffer needed for AVC.
                              NULL, 0,
                              is_video_track_encrypted_));
      has_video_ = true;
      video_track_id_ = track->header.track_id;
    }
  }

  // TODO(kqyang): figure out how to get duration for every tracks/streams.
  base::TimeDelta duration;
  if (moov_->extends.header.fragment_duration > 0) {
    duration = TimeDeltaFromRational(moov_->extends.header.fragment_duration,
                                     moov_->header.timescale);
  } else if (moov_->header.duration > 0 &&
             moov_->header.duration != kuint64max) {
    duration = TimeDeltaFromRational(moov_->header.duration,
                                     moov_->header.timescale);
  } else {
    duration = kInfiniteDuration();
  }

  init_cb_.Run(true, streams);

  EmitNeedKeyIfNecessary(moov_->pssh);
  return true;
}

bool MP4MediaParser::ParseMoof(BoxReader* reader) {
  RCHECK(moov_.get());  // Must already have initialization segment
  MovieFragment moof;
  RCHECK(moof.Parse(reader));
  if (!runs_)
    runs_.reset(new TrackRunIterator(moov_.get()));
  RCHECK(runs_->Init(moof));
  EmitNeedKeyIfNecessary(moof.pssh);
  ChangeState(kEmittingSamples);
  return true;
}

void MP4MediaParser::EmitNeedKeyIfNecessary(
    const std::vector<ProtectionSystemSpecificHeader>& headers) {
  // TODO(strobe): ensure that the value of init_data (all PSSH headers
  // concatenated in arbitrary order) matches the EME spec.
  // See https://www.w3.org/Bugs/Public/show_bug.cgi?id=17673.
  if (headers.empty())
    return;

  size_t total_size = 0;
  for (size_t i = 0; i < headers.size(); i++)
    total_size += headers[i].raw_box.size();

  scoped_ptr<uint8[]> init_data(new uint8[total_size]);
  size_t pos = 0;
  for (size_t i = 0; i < headers.size(); i++) {
    memcpy(&init_data.get()[pos], &headers[i].raw_box[0],
           headers[i].raw_box.size());
    pos += headers[i].raw_box.size();
  }
  need_key_cb_.Run(CONTAINER_MOV, init_data.Pass(), total_size);
}

bool MP4MediaParser::EnqueueSample(bool* err) {
  if (!runs_->IsRunValid()) {
    // Remain in kEnqueueingSamples state, discarding data, until the end of
    // the current 'mdat' box has been appended to the queue.
    if (!queue_.Trim(mdat_tail_))
      return false;

    ChangeState(kParsingBoxes);
    return true;
  }

  if (!runs_->IsSampleValid()) {
    runs_->AdvanceRun();
    return true;
  }

  DCHECK(!(*err));

  const uint8* buf;
  int buf_size;
  queue_.Peek(&buf, &buf_size);
  if (!buf_size) return false;

  bool audio = has_audio_ && audio_track_id_ == runs_->track_id();
  bool video = has_video_ && video_track_id_ == runs_->track_id();

  // Skip this entire track if it's not one we're interested in
  if (!audio && !video)
    runs_->AdvanceRun();

  // Attempt to cache the auxiliary information first. Aux info is usually
  // placed in a contiguous block before the sample data, rather than being
  // interleaved. If we didn't cache it, this would require that we retain the
  // start of the segment buffer while reading samples. Aux info is typically
  // quite small compared to sample data, so this pattern is useful on
  // memory-constrained devices where the source buffer consumes a substantial
  // portion of the total system memory.
  if (runs_->AuxInfoNeedsToBeCached()) {
    queue_.PeekAt(runs_->aux_info_offset() + moof_head_, &buf, &buf_size);
    if (buf_size < runs_->aux_info_size()) return false;
    *err = !runs_->CacheAuxInfo(buf, buf_size);
    return !*err;
  }

  queue_.PeekAt(runs_->sample_offset() + moof_head_, &buf, &buf_size);
  if (buf_size < runs_->sample_size()) return false;

  scoped_ptr<DecryptConfig> decrypt_config;
  std::vector<SubsampleEntry> subsamples;
  if (runs_->is_encrypted()) {
    decrypt_config = runs_->GetDecryptConfig();
    if (!decrypt_config) {
      *err = true;
      return false;
    }
    subsamples = decrypt_config->subsamples();
  }

  if (decrypt_config) {
    if (!subsamples.empty()) {
    // Create a new config with the updated subsamples.
    decrypt_config.reset(new DecryptConfig(
        decrypt_config->key_id(),
        decrypt_config->iv(),
        decrypt_config->data_offset(),
        subsamples));
    }
    // else, use the existing config.
  }

  std::vector<uint8> frame_buf(buf, buf + runs_->sample_size());
  scoped_refptr<MediaSample> stream_sample = MediaSample::CopyFrom(
      &frame_buf[0], frame_buf.size(), runs_->is_keyframe());

  if (decrypt_config)
    stream_sample->set_decrypt_config(decrypt_config.Pass());

  stream_sample->set_dts(runs_->dts());
  stream_sample->set_pts(runs_->cts());
  stream_sample->set_duration(runs_->duration());

  DVLOG(3) << "Pushing frame: aud=" << audio
           << ", key=" << runs_->is_keyframe()
           << ", dur=" << runs_->duration()
           << ", dts=" << runs_->dts()
           << ", cts=" << runs_->cts()
           << ", size=" << runs_->sample_size();

  new_sample_cb_.Run(runs_->track_id(), stream_sample);

  runs_->AdvanceSample();
  return true;
}

bool MP4MediaParser::ReadAndDiscardMDATsUntil(const int64 offset) {
  bool err = false;
  while (mdat_tail_ < offset) {
    const uint8* buf;
    int size;
    queue_.PeekAt(mdat_tail_, &buf, &size);

    FourCC type;
    int box_sz;
    if (!BoxReader::StartTopLevelBox(buf, size, &type, &box_sz, &err))
      break;

    if (type != FOURCC_MDAT) {
      LOG(ERROR) << "Unexpected box type while parsing MDATs: "
                 << FourCCToString(type);
    }
    mdat_tail_ += box_sz;
  }
  queue_.Trim(std::min(mdat_tail_, offset));
  return !err;
}

void MP4MediaParser::ChangeState(State new_state) {
  DVLOG(2) << "Changing state: " << new_state;
  state_ = new_state;
}

}  // namespace mp4
}  // namespace media