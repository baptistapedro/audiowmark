/*
 * Copyright (C) 2018-2020 Stefan Westerfeld
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "wmspeed.hh"
#include "wmcommon.hh"
#include "syncfinder.hh"
#include "threadpool.hh"
#include "fft.hh"
#include "resample.hh"

#include <algorithm>

using std::vector;
using std::sort;
using std::max;

static WavData
truncate (const WavData& in_data, double seconds)
{
  WavData out_data = in_data;
  vector<float> short_samples = in_data.samples();

  const size_t want_n_samples = lrint (in_data.sample_rate() * seconds) * in_data.n_channels();
  if (short_samples.size() > want_n_samples)
    {
      short_samples.resize (want_n_samples);
      out_data.set_samples (short_samples);
    }
  return out_data;
}

static WavData
get_speed_clip (double location, const WavData& in_data, double clip_seconds)
{
  double end_sec = double (in_data.n_frames()) / in_data.sample_rate();
  double start_sec = location * (end_sec - clip_seconds);
  if (start_sec < 0)
    start_sec = 0;

  size_t start_point = start_sec * in_data.sample_rate();
  size_t end_point = std::min<size_t> (start_point + clip_seconds * in_data.sample_rate(), in_data.n_frames());
#if 0
  printf ("[%f %f] l%f\n", double (start_point) / in_data.sample_rate(), double (end_point) / in_data.sample_rate(),
                           double (end_point - start_point) / in_data.sample_rate());
#endif
  vector<float> out_signal (in_data.samples().begin() + start_point * in_data.n_channels(),
                            in_data.samples().begin() + end_point * in_data.n_channels());
  WavData clip_data (out_signal, in_data.n_channels(), in_data.sample_rate(), in_data.bit_depth());
  return clip_data;
}

struct SpeedScanParams
{
  double seconds        = 0;
  double step           = 0;
  int    n_steps        = 0;
  int    n_center_steps = 0;
};

class SpeedSync
{
public:
  struct Score
  {
    double speed = 0;
    double quality = 0;
  };
private:
  struct Mags
  {
    float umag = 0;
    float dmag = 0;
  };
  vector<vector<SyncFinder::FrameBit>> sync_bits;
  vector<vector<Mags>> fft_sync_bits;

  void prepare_mags();
  void compare (double relative_speed);

  std::mutex mutex;
  vector<Score> result_scores;
  const WavData& in_data;
  const SpeedScanParams scan_params;
  const double center;
  const int    frames_per_block;
public:
  SpeedSync (const WavData& in_data, const SpeedScanParams& scan_params, double center) :
    in_data (in_data),
    scan_params (scan_params),
    center (center),
    frames_per_block (mark_sync_frame_count() + mark_data_frame_count())
  {
    // constructor is run in the main thread; everything that is not thread-safe must happen here
    SyncFinder sync_finder;

    sync_bits = sync_finder.get_sync_bits (in_data, SyncFinder::Mode::BLOCK);
  }
  void
  start_prepare_job (ThreadPool& thread_pool)
  {
    thread_pool.add_job ([this]() { prepare_mags(); });
  }

  void
  start_search_jobs (ThreadPool& thread_pool)
  {
    for (int p = -scan_params.n_steps; p <= scan_params.n_steps; p++)
      {
        const double relative_speed = pow (scan_params.step, p);

        thread_pool.add_job ([relative_speed, this]() { compare (relative_speed); });
      }
  }

  vector<Score>
  get_scores()
  {
    return result_scores;
  }
};

void
SpeedSync::prepare_mags()
{
  WavData in_data_trc (truncate (in_data, scan_params.seconds / center));

  // we downsample the audio by factor 2 to improve performance
  WavData in_data_sub (resample_ratio (in_data_trc, center / 2, Params::mark_sample_rate / 2));

  const int sub_frame_size = Params::frame_size / 2;
  const int sub_sync_search_step = Params::sync_search_step / 2;

  double window_weight = 0;
  float window[sub_frame_size];
  for (size_t i = 0; i < sub_frame_size; i++)
    {
      const double fsize_2 = sub_frame_size / 2.0;
      // const double win =  window_cos ((i - fsize_2) / fsize_2);
      /* FIXME: is this the best choice */
      const double win = window_hamming ((i - fsize_2) / fsize_2);
      //const double win = 1;
      window[i] = win;
      window_weight += win;
    }

  /* normalize window using window weight */
  for (size_t i = 0; i < sub_frame_size; i++)
    {
      window[i] *= 2.0 / window_weight;
    }

  FFTProcessor fft_processor (sub_frame_size);

  float *in = fft_processor.in();
  float *out = fft_processor.out();

  fft_sync_bits.clear();
  size_t pos = 0;
  while (pos + sub_frame_size < in_data_sub.n_frames())
    {
      const std::vector<float>& samples = in_data_sub.samples();
      vector<float> fft_out_db;

      for (int ch = 0; ch < in_data_sub.n_channels(); ch++)
        {
          for (int i = 0; i < sub_frame_size; i++)
            {
              in[i] = samples[ch + (pos + i) * in_data_sub.n_channels()] * window[i];
            }
          fft_processor.fft();

          for (int i = Params::min_band; i <= Params::max_band; i++)
            {
              const float min_db = -96;
              float re = out[i * 2];
              float im = out[i * 2 + 1];
              float abs = sqrt (re * re + im * im); // FIXME: could avoid sqrt here
              fft_out_db.push_back (db_from_factor (abs, min_db));
            }
        }
      vector<Mags> mags;
      for (size_t bit = 0; bit < sync_bits.size(); bit++)
        {
          const vector<SyncFinder::FrameBit>& frame_bits = sync_bits[bit];
          for (const auto& frame_bit : frame_bits)
            {
              float umag = 0, dmag = 0;

              for (size_t i = 0; i < frame_bit.up.size(); i++)
                {
                  umag += fft_out_db[frame_bit.up[i]];
                  dmag += fft_out_db[frame_bit.down[i]];
                }
              mags.push_back (Mags {umag, dmag});
            }
        }
      fft_sync_bits.push_back (mags);
      pos += sub_sync_search_step;
    }
}

void
SpeedSync::compare (double relative_speed)
{
  const int steps_per_frame = Params::frame_size / Params::sync_search_step;
  const int pad_start = frames_per_block * steps_per_frame;
  const double relative_speed_inv = 1 / relative_speed;
  Score best_score;

  assert (steps_per_frame * Params::sync_search_step == Params::frame_size);
  // FIXME: pad_start must be scaled with speed
  for (int offset = -pad_start; offset < 0; offset++)
    {
      double sync_quality = 0;
      int mi = 0;
      int frame_bit_count = 0;
      int bit_count = 0;
      for (size_t sync_bit = 0; sync_bit < Params::sync_bits; sync_bit++)
        {
          const vector<SyncFinder::FrameBit>& frame_bits = sync_bits[sync_bit];

          float umag = 0;
          float dmag = 0;
          for (size_t f = 0; f < Params::sync_frames_per_bit; f++)
            {
              int index = offset + frame_bits[f].frame * steps_per_frame;

              const int index1 = index * relative_speed_inv;
              if (index1 >= 0 && index1 < (int) fft_sync_bits.size())
                {
                  umag += fft_sync_bits[index1][mi].umag;
                  dmag += fft_sync_bits[index1][mi].dmag;
                  frame_bit_count++;
                }
              index += frames_per_block * steps_per_frame;

              const int index2 = index * relative_speed_inv;
              if (index2 >= 0 && index2 < (int) fft_sync_bits.size())
                {
                  umag += fft_sync_bits[index2][mi].dmag;
                  dmag += fft_sync_bits[index2][mi].umag;
                  frame_bit_count++;
                }
              mi++;
            }
          /* convert avoiding bias, raw_bit < 0 => 0 bit received; raw_bit > 0 => 1 bit received */
          double raw_bit;
          if (umag == 0 || dmag == 0)
            {
              raw_bit = 0;
            }
          else if (umag < dmag)
            {
              raw_bit = 1 - umag / dmag;
            }
          else
            {
              raw_bit = dmag / umag - 1;
            }
          const int expect_data_bit = sync_bit & 1; /* expect 010101 */
          const double q = expect_data_bit ? raw_bit : -raw_bit;
          sync_quality += q * frame_bit_count;
          bit_count += frame_bit_count;
        }
      if (bit_count)
        {
          sync_quality /= bit_count;
          sync_quality = fabs (sync_quality);
          //sync_quality = fabs (sync_finder.normalize_sync_quality (sync_quality));
          //printf ("%d %f\n", offset, fabs (sync_quality));
          if (sync_quality > best_score.quality)
            {
              best_score.quality = sync_quality;
              best_score.speed = relative_speed * center;
            }
        }
    }
  //printf ("%f %f\n", best_score.speed, best_score.quality);
  std::lock_guard<std::mutex> lg (mutex);
  result_scores.push_back (best_score);
}

static double
speed_scan (ThreadPool& thread_pool, double clip_location, const WavData& in_data, const SpeedScanParams& scan_params, double speed, bool print_results)
{
  /* speed is between 0.8 and 1.25, so we use a clip seconds factor of 1.3 to provide enough samples */
  WavData in_clip = get_speed_clip (clip_location, in_data, scan_params.seconds * 1.3);

  auto t0 = get_time();

  vector<std::unique_ptr<SpeedSync>> speed_sync;
  for (int c = -scan_params.n_center_steps; c <= scan_params.n_center_steps; c++)
    {
      double c_speed = speed * pow (scan_params.step, c * (scan_params.n_steps * 2 + 1));

      speed_sync.push_back (std::make_unique<SpeedSync> (in_clip, scan_params, c_speed));
    }

  for (auto& s : speed_sync)
    s->start_prepare_job (thread_pool);
  thread_pool.wait_all();

  auto t1 = get_time();

  for (auto& s : speed_sync)
    s->start_search_jobs (thread_pool);
  thread_pool.wait_all();

  auto t2 = get_time();

  vector<SpeedSync::Score> scores;
  for (auto& s : speed_sync)
    {
      vector<SpeedSync::Score> step_scores = s->get_scores();
      scores.insert (scores.end(), step_scores.begin(), step_scores.end());
    }

  /* output best result, or: if there is not a unique best result, average all best results */

  double best_quality = 0;
  for (auto score : scores)
    best_quality = max (best_quality, score.quality);

  double best_speed = 0;
  int speed_count = 0;
  for (auto score : scores)
    {
      const double factor = 0.99; /* all matches which are closer than this are considered relevant */

      if (score.quality >= best_quality * factor)
        {
          best_speed += score.speed;
          speed_count++;
        }
    }
  if (speed_count)
    best_speed /= speed_count;

  if (print_results)
    {
      printf ("detect_speed %.0f %f %f %f %.3f %.3f\n",
        scan_params.seconds,
        best_speed,
        best_quality,
        100 * fabs (best_speed - Params::test_speed) / Params::test_speed,
        t1 - t0, t2 - t1);
    }
  return best_speed;
}

static double
get_clip_location (const WavData& in_data)
{
  Random rng (0, Random::Stream::speed_clip);

  /* to improve performance, we don't hash all samples but just a few */
  const vector<float>& samples = in_data.samples();
  vector<float> xsamples;
  for (size_t p = 0; p < samples.size(); p += rng() % 1000)
    xsamples.push_back (samples[p]);

  rng.seed (Random::seed_from_hash (xsamples), Random::Stream::speed_clip);

  return rng.random_double();
}

double
detect_speed (const WavData& in_data, bool print_results)
{
  double clip_location = get_clip_location (in_data);

  ThreadPool thread_pool;

  /* first pass:  find approximation for speed */
  const SpeedScanParams scan1
    {
      .seconds        = 21,

      /* step / n_steps / n_center_steps settings: speed approximately 0.8..1.25 */
      .step           = 1.0007,
      .n_steps        = 5,
      .n_center_steps = 28
    };
  double speed = speed_scan (thread_pool, clip_location, in_data, scan1, /* start speed */ 1.0, print_results);

  /* second pass: fast refine (not always perfect) */
  const SpeedScanParams scan2
    {
      .seconds        = 50,
      .step           = 1.00005,
      .n_steps        = 20,
      .n_center_steps = 0
    };
  speed = speed_scan (thread_pool, clip_location, in_data, scan2, speed, print_results);
  return speed;
}
