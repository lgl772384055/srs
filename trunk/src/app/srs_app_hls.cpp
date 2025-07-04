//
// Copyright (c) 2013-2024 The SRS Authors
//
// SPDX-License-Identifier: MIT or MulanPSL-2.0
//

#include <srs_app_hls.hpp>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <algorithm>
#include <sstream>
using namespace std;

#include <srs_kernel_error.hpp>
#include <srs_kernel_codec.hpp>
#include <srs_protocol_amf0.hpp>
#include <srs_protocol_rtmp_stack.hpp>
#include <srs_app_config.hpp>
#include <srs_app_source.hpp>
#include <srs_core_autofree.hpp>
#include <srs_protocol_rtmp_stack.hpp>
#include <srs_app_pithy_print.hpp>
#include <srs_kernel_utility.hpp>
#include <srs_kernel_codec.hpp>
#include <srs_kernel_file.hpp>
#include <srs_protocol_stream.hpp>
#include <srs_kernel_ts.hpp>
#include <srs_app_utility.hpp>
#include <srs_app_http_hooks.hpp>
#include <srs_protocol_format.hpp>
#include <openssl/rand.h>

// drop the segment when duration of ts too small.
// TODO: FIXME: Refine to time unit.
#define SRS_HLS_SEGMENT_MIN_DURATION (100 * SRS_UTIME_MILLISECONDS)

// fragment plus the deviation percent.
#define SRS_HLS_FLOOR_REAP_PERCENT 0.3
// reset the piece id when deviation overflow this.
#define SRS_JUMP_WHEN_PIECE_DEVIATION 20

SrsHlsSegment::SrsHlsSegment(SrsTsContext* c, SrsAudioCodecId ac, SrsVideoCodecId vc, SrsFileWriter* w)
{
    sequence_no = 0;
    writer = w;
    tscw = new SrsTsContextWriter(writer, c, ac, vc);
}

SrsHlsSegment::~SrsHlsSegment()
{
    srs_freep(tscw);
}

void SrsHlsSegment::config_cipher(unsigned char* key,unsigned char* iv)
{
    memcpy(this->iv, iv,16);
    
    SrsEncFileWriter* fw = (SrsEncFileWriter*)writer;
    fw->config_cipher(key, iv);
}

srs_error_t SrsHlsSegment::rename()
{
    if (true) {
        std::stringstream ss;
        ss << srsu2msi(duration());
        uri = srs_string_replace(uri, "[duration]", ss.str());
    }

    return SrsFragment::rename();
}

SrsDvrAsyncCallOnHls::SrsDvrAsyncCallOnHls(SrsContextId c, SrsRequest* r, string p, string t, string m, string mu, int s, srs_utime_t d)
{
    req = r->copy();
    cid = c;
    path = p;
    ts_url = t;
    m3u8 = m;
    m3u8_url = mu;
    seq_no = s;
    duration = d;
}

SrsDvrAsyncCallOnHls::~SrsDvrAsyncCallOnHls()
{
    srs_freep(req);
}

srs_error_t SrsDvrAsyncCallOnHls::call()
{
    srs_error_t err = srs_success;
    
    if (!_srs_config->get_vhost_http_hooks_enabled(req->vhost)) {
        return err;
    }
    
    // the http hooks will cause context switch,
    // so we must copy all hooks for the on_connect may freed.
    // @see https://github.com/ossrs/srs/issues/475
    vector<string> hooks;
    
    if (true) {
        SrsConfDirective* conf = _srs_config->get_vhost_on_hls(req->vhost);
        
        if (!conf) {
            return err;
        }
        
        hooks = conf->args;
    }
    
    for (int i = 0; i < (int)hooks.size(); i++) {
        std::string url = hooks.at(i);
        if ((err = SrsHttpHooks::on_hls(cid, url, req, path, ts_url, m3u8, m3u8_url, seq_no, duration)) != srs_success) {
            return srs_error_wrap(err, "callback on_hls %s", url.c_str());
        }
    }
    
    return err;
}

string SrsDvrAsyncCallOnHls::to_string()
{
    return "on_hls: " + path;
}

SrsDvrAsyncCallOnHlsNotify::SrsDvrAsyncCallOnHlsNotify(SrsContextId c, SrsRequest* r, string u)
{
    cid = c;
    req = r->copy();
    ts_url = u;
}

SrsDvrAsyncCallOnHlsNotify::~SrsDvrAsyncCallOnHlsNotify()
{
    srs_freep(req);
}

srs_error_t SrsDvrAsyncCallOnHlsNotify::call()
{
    srs_error_t err = srs_success;
    
    if (!_srs_config->get_vhost_http_hooks_enabled(req->vhost)) {
        return err;
    }
    
    // the http hooks will cause context switch,
    // so we must copy all hooks for the on_connect may freed.
    // @see https://github.com/ossrs/srs/issues/475
    vector<string> hooks;
    
    if (true) {
        SrsConfDirective* conf = _srs_config->get_vhost_on_hls_notify(req->vhost);
        
        if (!conf) {
            return err;
        }
        
        hooks = conf->args;
    }
    
    int nb_notify = _srs_config->get_vhost_hls_nb_notify(req->vhost);
    for (int i = 0; i < (int)hooks.size(); i++) {
        std::string url = hooks.at(i);
        if ((err = SrsHttpHooks::on_hls_notify(cid, url, req, ts_url, nb_notify)) != srs_success) {
            return srs_error_wrap(err, "callback on_hls_notify %s", url.c_str());
        }
    }
    
    return err;
}

string SrsDvrAsyncCallOnHlsNotify::to_string()
{
    return "on_hls_notify: " + ts_url;
}

SrsHlsMuxer::SrsHlsMuxer()
{
    req = NULL;
    hls_fragment = hls_window = 0;
    hls_aof_ratio = 1.0;
    deviation_ts = 0;
    hls_cleanup = true;
    hls_wait_keyframe = true;
    previous_floor_ts = 0;
    accept_floor_ts = 0;
    hls_ts_floor = false;
    max_td = 0;
    writer = NULL;
    _sequence_no = 0;
    current = NULL;
    hls_keys = false;
    hls_fragments_per_key = 0;
    async = new SrsAsyncCallWorker();
    context = new SrsTsContext();
    segments = new SrsFragmentWindow();
    latest_acodec_ = SrsAudioCodecIdForbidden;
    
    memset(key, 0, 16);
    memset(iv, 0, 16);
}

SrsHlsMuxer::~SrsHlsMuxer()
{
    srs_freep(segments);
    srs_freep(current);
    srs_freep(req);
    srs_freep(async);
    srs_freep(context);
    srs_freep(writer);
}

void SrsHlsMuxer::dispose()
{
    srs_error_t err = srs_success;
    
    segments->dispose();
    
    if (current) {
        if ((err = current->unlink_tmpfile()) != srs_success) {
            srs_warn("Unlink tmp ts failed %s", srs_error_desc(err).c_str());
            srs_freep(err);
        }
        srs_freep(current);
    }
    
    if (unlink(m3u8.c_str()) < 0) {
        srs_warn("dispose unlink path failed. file=%s", m3u8.c_str());
    }
    
    srs_trace("gracefully dispose hls %s", req? req->get_stream_url().c_str() : "");
}

int SrsHlsMuxer::sequence_no()
{
    return _sequence_no;
}

string SrsHlsMuxer::ts_url()
{
    return current? current->uri:"";
}

srs_utime_t SrsHlsMuxer::duration()
{
    return current? current->duration():0;
}

int SrsHlsMuxer::deviation()
{
    // no floor, no deviation.
    if (!hls_ts_floor) {
        return 0;
    }
    
    return deviation_ts;
}

SrsAudioCodecId SrsHlsMuxer::latest_acodec()
{
    // If current context writer exists, we query from it.
    if (current && current->tscw) return current->tscw->acodec();

    // Get the configured or updated config.
    return latest_acodec_;
}

void SrsHlsMuxer::set_latest_acodec(SrsAudioCodecId v)
{
    // Refresh the codec in context writer for current segment.
    if (current && current->tscw) current->tscw->set_acodec(v);

    // Refresh the codec for future segments.
    latest_acodec_ = v;
}

srs_error_t SrsHlsMuxer::initialize()
{
    return srs_success;
}

srs_error_t SrsHlsMuxer::on_publish(SrsRequest* req)
{
    srs_error_t err = srs_success;

    if ((err = async->start()) != srs_success) {
        return srs_error_wrap(err, "async start");
    }

    return err;
}

srs_error_t SrsHlsMuxer::on_unpublish()
{
    async->stop();
    return srs_success;
}

srs_error_t SrsHlsMuxer::update_config(SrsRequest* r, string entry_prefix,
    string path, string m3u8_file, string ts_file, srs_utime_t fragment, srs_utime_t window,
    bool ts_floor, double aof_ratio, bool cleanup, bool wait_keyframe, bool keys,
    int fragments_per_key, string key_file ,string key_file_path, string key_url)
{
    srs_error_t err = srs_success;
    
    srs_freep(req);
    req = r->copy();
    
    hls_entry_prefix = entry_prefix;
    hls_path = path;
    hls_ts_file = ts_file;
    hls_fragment = fragment;
    hls_aof_ratio = aof_ratio;
    hls_ts_floor = ts_floor;
    hls_cleanup = cleanup;
    hls_wait_keyframe = wait_keyframe;
    previous_floor_ts = 0;
    accept_floor_ts = 0;
    hls_window = window;
    deviation_ts = 0;

    hls_keys = keys;
    hls_fragments_per_key = fragments_per_key;
    hls_key_file = key_file;
    hls_key_file_path = key_file_path;
    hls_key_url = key_url;
   
    // generate the m3u8 dir and path.
    m3u8_url = srs_path_build_stream(m3u8_file, req->vhost, req->app, req->stream);
    m3u8 = path + "/" + m3u8_url;
    
    // when update config, reset the history target duration.
    max_td = fragment * _srs_config->get_hls_td_ratio(r->vhost);
    
    // create m3u8 dir once.
    m3u8_dir = srs_path_dirname(m3u8);
    if ((err = srs_create_dir_recursively(m3u8_dir)) != srs_success) {
        return srs_error_wrap(err, "create dir");
    }

    if (hls_keys && (hls_path != hls_key_file_path)) {
        string key_file = srs_path_build_stream(hls_key_file, req->vhost, req->app, req->stream);
        string key_url = hls_key_file_path + "/" + key_file;
        string key_dir = srs_path_dirname(key_url);
        if ((err = srs_create_dir_recursively(key_dir)) != srs_success) {
            return srs_error_wrap(err, "create dir");
        }
    }

    if(hls_keys) {
        writer = new SrsEncFileWriter();
    } else {
        writer = new SrsFileWriter();
    }

    return err;
}

srs_error_t SrsHlsMuxer::segment_open()
{
    srs_error_t err = srs_success;
    
    if (current) {
        srs_warn("ignore the segment open, for segment is already open.");
        return err;
    }
    
    // when segment open, the current segment must be NULL.
    srs_assert(!current);
    
    // load the default acodec from config.
    SrsAudioCodecId default_acodec = SrsAudioCodecIdAAC;
    if (true) {
        std::string default_acodec_str = _srs_config->get_hls_acodec(req->vhost);
        if (default_acodec_str == "mp3") {
            default_acodec = SrsAudioCodecIdMP3;
        } else if (default_acodec_str == "aac") {
            default_acodec = SrsAudioCodecIdAAC;
        } else if (default_acodec_str == "an") {
            default_acodec = SrsAudioCodecIdDisabled;
        } else {
            srs_warn("hls: use aac for other codec=%s", default_acodec_str.c_str());
        }
    }
    // Now that we know the latest audio codec in stream, use it.
    if (latest_acodec_ != SrsAudioCodecIdForbidden) default_acodec = latest_acodec_;
    
    // load the default vcodec from config.
    SrsVideoCodecId default_vcodec = SrsVideoCodecIdAVC;
    if (true) {
        std::string default_vcodec_str = _srs_config->get_hls_vcodec(req->vhost);
        if (default_vcodec_str == "h264") {
            default_vcodec = SrsVideoCodecIdAVC;
        } else if (default_vcodec_str == "vn") {
            default_vcodec = SrsVideoCodecIdDisabled;
        } else {
            srs_warn("hls: use h264 for other codec=%s", default_vcodec_str.c_str());
        }
    }
    
    // new segment.
    current = new SrsHlsSegment(context, default_acodec, default_vcodec, writer);
    current->sequence_no = _sequence_no++;

    if ((err = write_hls_key()) != srs_success) {
        return srs_error_wrap(err, "write hls key");
    }
    
    // generate filename.
    std::string ts_file = hls_ts_file;
    ts_file = srs_path_build_stream(ts_file, req->vhost, req->app, req->stream);
    if (hls_ts_floor) {
        // accept the floor ts for the first piece.
        int64_t current_floor_ts = srs_update_system_time() / hls_fragment;
        if (!accept_floor_ts) {
            accept_floor_ts = current_floor_ts - 1;
        } else {
            accept_floor_ts++;
        }
        
        // jump when deviation more than 10p
        if (accept_floor_ts - current_floor_ts > SRS_JUMP_WHEN_PIECE_DEVIATION) {
            srs_warn("hls: jmp for ts deviation, current=%" PRId64 ", accept=%" PRId64, current_floor_ts, accept_floor_ts);
            accept_floor_ts = current_floor_ts - 1;
        }
        
        // when reap ts, adjust the deviation.
        deviation_ts = (int)(accept_floor_ts - current_floor_ts);
        
        // dup/jmp detect for ts in floor mode.
        if (previous_floor_ts && previous_floor_ts != current_floor_ts - 1) {
            srs_warn("hls: dup/jmp ts, previous=%" PRId64 ", current=%" PRId64 ", accept=%" PRId64 ", deviation=%d",
                     previous_floor_ts, current_floor_ts, accept_floor_ts, deviation_ts);
        }
        previous_floor_ts = current_floor_ts;
        
        // we always ensure the piece is increase one by one.
        std::stringstream ts_floor;
        ts_floor << accept_floor_ts;
        ts_file = srs_string_replace(ts_file, "[timestamp]", ts_floor.str());
        
        // TODO: FIMXE: we must use the accept ts floor time to generate the hour variable.
        ts_file = srs_path_build_timestamp(ts_file);
    } else {
        ts_file = srs_path_build_timestamp(ts_file);
    }
    if (true) {
        std::stringstream ss;
        ss << current->sequence_no;
        ts_file = srs_string_replace(ts_file, "[seq]", ss.str());
    }
    current->set_path(hls_path + "/" + ts_file);
    
    // the ts url, relative or absolute url.
    // TODO: FIXME: Use url and path manager.
    std::string ts_url = current->fullpath();
    if (srs_string_starts_with(ts_url, m3u8_dir)) {
        ts_url = ts_url.substr(m3u8_dir.length());
    }
    while (srs_string_starts_with(ts_url, "/")) {
        ts_url = ts_url.substr(1);
    }
    current->uri += hls_entry_prefix;
    if (!hls_entry_prefix.empty() && !srs_string_ends_with(hls_entry_prefix, "/")) {
        current->uri += "/";
        
        // add the http dir to uri.
        string http_dir = srs_path_dirname(m3u8_url);
        if (!http_dir.empty()) {
            current->uri += http_dir + "/";
        }
    }
    current->uri += ts_url;
    
    // create dir recursively for hls.
    if ((err = current->create_dir()) != srs_success) {
        return srs_error_wrap(err, "create dir");
    }
    
    // open temp ts file.
    std::string tmp_file = current->tmppath();
    if ((err = current->writer->open(tmp_file)) != srs_success) {
        return srs_error_wrap(err, "open hls muxer");
    }

    // reset the context for a new ts start.
    context->reset();
    
    return err;
}

srs_error_t SrsHlsMuxer::on_sequence_header()
{
    srs_error_t err = srs_success;
    
    srs_assert(current);
    
    // set the current segment to sequence header,
    // when close the segement, it will write a discontinuity to m3u8 file.
    current->set_sequence_header(true);
    
    return err;
}

bool SrsHlsMuxer::is_segment_overflow()
{
    srs_assert(current);
    
    // to prevent very small segment.
    if (current->duration() < 2 * SRS_HLS_SEGMENT_MIN_DURATION) {
        return false;
    }
    
    // Use N% deviation, to smoother.
    srs_utime_t deviation = hls_ts_floor? SRS_HLS_FLOOR_REAP_PERCENT * deviation_ts * hls_fragment : 0;

    // Keep in mind that we use max_td for the base duration, not the hls_fragment. To calculate
    // max_td, multiply hls_fragment by hls_td_ratio.
    return current->duration() >= max_td + deviation;
}

bool SrsHlsMuxer::wait_keyframe()
{
    return hls_wait_keyframe;
}

bool SrsHlsMuxer::is_segment_absolutely_overflow()
{
    // @see https://github.com/ossrs/srs/issues/151#issuecomment-83553950
    srs_assert(current);
    
    // to prevent very small segment.
    if (current->duration() < 2 * SRS_HLS_SEGMENT_MIN_DURATION) {
        return false;
    }
    
    // use N% deviation, to smoother.
    srs_utime_t deviation = hls_ts_floor? SRS_HLS_FLOOR_REAP_PERCENT * deviation_ts * hls_fragment : 0;
    return current->duration() >= hls_aof_ratio * hls_fragment + deviation;
}

bool SrsHlsMuxer::pure_audio()
{
    return current && current->tscw && current->tscw->video_codec() == SrsVideoCodecIdDisabled;
}

srs_error_t SrsHlsMuxer::flush_audio(SrsTsMessageCache* cache)
{
    srs_error_t err = srs_success;
    
    // if current is NULL, segment is not open, ignore the flush event.
    if (!current) {
        srs_warn("flush audio ignored, for segment is not open.");
        return err;
    }
    
    if (!cache->audio || cache->audio->payload->length() <= 0) {
        return err;
    }
    
    // update the duration of segment.
    update_duration(cache->audio->dts);

    if ((err = current->tscw->write_audio(cache->audio)) != srs_success) {
        return srs_error_wrap(err, "hls: write audio");
    }
    
    // write success, clear and free the msg
    srs_freep(cache->audio);
    
    return err;
}

srs_error_t SrsHlsMuxer::flush_video(SrsTsMessageCache* cache)
{
    srs_error_t err = srs_success;
    
    // if current is NULL, segment is not open, ignore the flush event.
    if (!current) {
        srs_warn("flush video ignored, for segment is not open.");
        return err;
    }
    
    if (!cache->video || cache->video->payload->length() <= 0) {
        return err;
    }
    
    srs_assert(current);
    
    // update the duration of segment.
    update_duration(cache->video->dts);

    if ((err = current->tscw->write_video(cache->video)) != srs_success) {
        return srs_error_wrap(err, "hls: write video");
    }
    
    // write success, clear and free the msg
    srs_freep(cache->video);
    
    return err;
}

void SrsHlsMuxer::update_duration(uint64_t dts)
{
    current->append(dts / 90);
}

srs_error_t SrsHlsMuxer::segment_close()
{
    srs_error_t err = do_segment_close();

    // We always cleanup current segment.
    srs_freep(current);

    return err;
}

srs_error_t SrsHlsMuxer::do_segment_close()
{
    srs_error_t err = srs_success;
    
    if (!current) {
        srs_warn("ignore the segment close, for segment is not open.");
        return err;
    }
    
    // when close current segment, the current segment must not be NULL.
    srs_assert(current);

    // We should always close the underlayer writer.
    if (current && current->writer) {
        current->writer->close();
    }
    
    // valid, add to segments if segment duration is ok
    // when too small, it maybe not enough data to play.
    // when too large, it maybe timestamp corrupt.
    // make the segment more acceptable, when in [min, max_td * 3], it's ok.
    bool matchMinDuration = current->duration() >= SRS_HLS_SEGMENT_MIN_DURATION;
    bool matchMaxDuration = current->duration() <= max_td * 3 * 1000;
    if (matchMinDuration && matchMaxDuration) {
        // rename from tmp to real path
        if ((err = current->rename()) != srs_success) {
            return srs_error_wrap(err, "rename");
        }
        
        // use async to call the http hooks, for it will cause thread switch.
        if ((err = async->execute(new SrsDvrAsyncCallOnHls(_srs_context->get_id(), req, current->fullpath(),
            current->uri, m3u8, m3u8_url, current->sequence_no, current->duration()))) != srs_success) {
            return srs_error_wrap(err, "segment close");
        }
        
        // use async to call the http hooks, for it will cause thread switch.
        if ((err = async->execute(new SrsDvrAsyncCallOnHlsNotify(_srs_context->get_id(), req, current->uri))) != srs_success) {
            return srs_error_wrap(err, "segment close");
        }
        
        // close the muxer of finished segment.
        srs_freep(current->tscw);

        segments->append(current);
        current = NULL;
    } else {
        // reuse current segment index.
        _sequence_no--;
        
        srs_trace("Drop ts segment, sequence_no=%d, uri=%s, duration=%dms",
            current->sequence_no, current->uri.c_str(), srsu2msi(current->duration()));
        
        // rename from tmp to real path
        if ((err = current->unlink_tmpfile()) != srs_success) {
            return srs_error_wrap(err, "rename");
        }
    }
    
    // shrink the segments.
    segments->shrink(hls_window);
    
    // refresh the m3u8, donot contains the removed ts
    err = refresh_m3u8();
    
    // remove the ts file.
    segments->clear_expired(hls_cleanup);
    
    // check ret of refresh m3u8
    if (err != srs_success) {
        return srs_error_wrap(err, "hls: refresh m3u8");
    }
    
    return err;
}

srs_error_t SrsHlsMuxer::write_hls_key()
{
    srs_error_t err = srs_success;
    
    if (hls_keys && current->sequence_no % hls_fragments_per_key == 0) {
        if (RAND_bytes(key, 16) < 0) {
            return srs_error_wrap(err, "rand key failed.");
        }
        if (RAND_bytes(iv, 16) < 0) {
            return srs_error_wrap(err, "rand iv failed.");
        }
        
        string key_file = srs_path_build_stream(hls_key_file, req->vhost, req->app, req->stream);
        key_file = srs_string_replace(key_file, "[seq]", srs_int2str(current->sequence_no));
        string key_url = hls_key_file_path + "/" + key_file;
        
        SrsFileWriter fw;
        if ((err = fw.open(key_url)) != srs_success) {
            return srs_error_wrap(err, "open file %s", key_url.c_str());
        }
        
        err = fw.write(key, 16, NULL);
        fw.close();
        
        if (err != srs_success) {
            return srs_error_wrap(err, "write key");
        }
    }
    
    if (hls_keys) {
        current->config_cipher(key, iv);
    }
    
    return err;
}

srs_error_t SrsHlsMuxer::refresh_m3u8()
{
    srs_error_t err = srs_success;
    
    // no segments, also no m3u8, return.
    if (segments->empty()) {
        return err;
    }
    
    std::string temp_m3u8 = m3u8 + ".temp";
    if ((err = _refresh_m3u8(temp_m3u8)) == srs_success) {
        if (rename(temp_m3u8.c_str(), m3u8.c_str()) < 0) {
            err = srs_error_new(ERROR_HLS_WRITE_FAILED, "hls: rename m3u8 file failed. %s => %s", temp_m3u8.c_str(), m3u8.c_str());
        }
    }
    
    // remove the temp file.
    if (srs_path_exists(temp_m3u8)) {
        if (unlink(temp_m3u8.c_str()) < 0) {
            srs_warn("ignore remove m3u8 failed, %s", temp_m3u8.c_str());
        }
    }
    
    return err;
}

srs_error_t SrsHlsMuxer::_refresh_m3u8(string m3u8_file)
{
    srs_error_t err = srs_success;
    
    // no segments, return.
    if (segments->empty()) {
        return err;
    }
    
    SrsFileWriter writer;
    if ((err = writer.open(m3u8_file)) != srs_success) {
        return srs_error_wrap(err, "hls: open m3u8 file %s", m3u8_file.c_str());
    }
    
    // #EXTM3U\n
    // #EXT-X-VERSION:3\n
    std::stringstream ss;
    ss << "#EXTM3U" << SRS_CONSTS_LF;
    ss << "#EXT-X-VERSION:3" << SRS_CONSTS_LF;
    
    // #EXT-X-MEDIA-SEQUENCE:4294967295\n
    SrsHlsSegment* first = dynamic_cast<SrsHlsSegment*>(segments->first());
    if (first == NULL) {
        return srs_error_new(ERROR_HLS_WRITE_FAILED, "segments cast");
    }

    ss << "#EXT-X-MEDIA-SEQUENCE:" << first->sequence_no << SRS_CONSTS_LF;
    
    // #EXT-X-TARGETDURATION:4294967295\n
    /**
     * @see hls-m3u8-draft-pantos-http-live-streaming-12.pdf, page 25
     * The Media Playlist file MUST contain an EXT-X-TARGETDURATION tag.
     * Its value MUST be equal to or greater than the EXTINF duration of any
     * media segment that appears or will appear in the Playlist file,
     * rounded to the nearest integer. Its value MUST NOT change. A
     * typical target duration is 10 seconds.
     */
    // @see https://github.com/ossrs/srs/issues/304#issuecomment-74000081
    srs_utime_t max_duration = segments->max_duration();
    int target_duration = (int)ceil(srsu2msi(srs_max(max_duration, max_td)) / 1000.0);
    
    ss << "#EXT-X-TARGETDURATION:" << target_duration << SRS_CONSTS_LF;
    
    // write all segments
    for (int i = 0; i < segments->size(); i++) {
        SrsHlsSegment* segment = dynamic_cast<SrsHlsSegment*>(segments->at(i));
        
        if (segment->is_sequence_header()) {
            // #EXT-X-DISCONTINUITY\n
            ss << "#EXT-X-DISCONTINUITY" << SRS_CONSTS_LF;
        }
        
        if(hls_keys && ((segment->sequence_no % hls_fragments_per_key) == 0)) {
            char hexiv[33];
            srs_data_to_hex(hexiv, segment->iv, 16);
            hexiv[32] = '\0';
            
            string key_file = srs_path_build_stream(hls_key_file, req->vhost, req->app, req->stream);
            key_file = srs_string_replace(key_file, "[seq]", srs_int2str(segment->sequence_no));
            
            string key_path = key_file;
            //if key_url is not set,only use the file name
            if (!hls_key_url.empty()) {
                key_path = hls_key_url + key_file;
            }
            
            ss << "#EXT-X-KEY:METHOD=AES-128,URI=" << "\"" << key_path << "\",IV=0x" << hexiv << SRS_CONSTS_LF;
        }
        
        // "#EXTINF:4294967295.208,\n"
        ss.precision(3);
        ss.setf(std::ios::fixed, std::ios::floatfield);
        ss << "#EXTINF:" << srsu2msi(segment->duration()) / 1000.0 << ", no desc" << SRS_CONSTS_LF;
        
        // {file name}\n
        std::string seg_uri = segment->uri;
        if (true) {
	        std::stringstream stemp;
	        stemp << srsu2msi(segment->duration());
	        seg_uri = srs_string_replace(seg_uri, "[duration]", stemp.str());
        }
        //ss << segment->uri << SRS_CONSTS_LF;
        ss << seg_uri << SRS_CONSTS_LF;
    }
    
    // write m3u8 to writer.
    std::string m3u8 = ss.str();
    if ((err = writer.write((char*)m3u8.c_str(), (int)m3u8.length(), NULL)) != srs_success) {
        return srs_error_wrap(err, "hls: write m3u8");
    }
    
    return err;
}

SrsHlsController::SrsHlsController()
{
    tsmc = new SrsTsMessageCache();
    muxer = new SrsHlsMuxer();
}

SrsHlsController::~SrsHlsController()
{
    srs_freep(muxer);
    srs_freep(tsmc);
}

srs_error_t SrsHlsController::initialize()
{
    srs_error_t err = muxer->initialize();
    if (err != srs_success) {
        return srs_error_wrap(err, "hls muxer initialize");
    }
    return srs_success;
}

void SrsHlsController::dispose()
{
    muxer->dispose();
}

int SrsHlsController::sequence_no()
{
    return muxer->sequence_no();
}

string SrsHlsController::ts_url()
{
    return muxer->ts_url();
}

srs_utime_t SrsHlsController::duration()
{
    return muxer->duration();
}

int SrsHlsController::deviation()
{
    return muxer->deviation();
}

srs_error_t SrsHlsController::on_publish(SrsRequest* req)
{
    srs_error_t err = srs_success;
    
    std::string vhost = req->vhost;
    std::string stream = req->stream;
    std::string app = req->app;

    srs_utime_t hls_fragment = _srs_config->get_hls_fragment(vhost);
    double hls_td_ratio = _srs_config->get_hls_td_ratio(vhost);
    srs_utime_t hls_window = _srs_config->get_hls_window(vhost);
    
    // get the hls m3u8 ts list entry prefix config
    std::string entry_prefix = _srs_config->get_hls_entry_prefix(vhost);
    // get the hls path config
    std::string path = _srs_config->get_hls_path(vhost);
    std::string m3u8_file = _srs_config->get_hls_m3u8_file(vhost);
    std::string ts_file = _srs_config->get_hls_ts_file(vhost);
    bool cleanup = _srs_config->get_hls_cleanup(vhost);
    bool wait_keyframe = _srs_config->get_hls_wait_keyframe(vhost);
    // the audio overflow, for pure audio to reap segment.
    double hls_aof_ratio = _srs_config->get_hls_aof_ratio(vhost);
    // whether use floor(timestamp/hls_fragment) for variable timestamp
    bool ts_floor = _srs_config->get_hls_ts_floor(vhost);
    // the seconds to dispose the hls.
    srs_utime_t hls_dispose = _srs_config->get_hls_dispose(vhost);

    bool hls_keys = _srs_config->get_hls_keys(vhost);
    int hls_fragments_per_key = _srs_config->get_hls_fragments_per_key(vhost);
    string hls_key_file =  _srs_config->get_hls_key_file(vhost);
    string hls_key_file_path = _srs_config->get_hls_key_file_path(vhost);
    string hls_key_url = _srs_config->get_hls_key_url(vhost);
    
    // TODO: FIXME: support load exists m3u8, to continue publish stream.
    // for the HLS donot requires the EXT-X-MEDIA-SEQUENCE be monotonically increase.

    if ((err = muxer->on_publish(req)) != srs_success) {
        return srs_error_wrap(err, "muxer publish");
    }
    
    if ((err = muxer->update_config(req, entry_prefix, path, m3u8_file, ts_file, hls_fragment,
        hls_window, ts_floor, hls_aof_ratio, cleanup, wait_keyframe,hls_keys,hls_fragments_per_key,
        hls_key_file, hls_key_file_path, hls_key_url)) != srs_success ) {
        return srs_error_wrap(err, "hls: update config");
    }
    
    if ((err = muxer->segment_open()) != srs_success) {
        return srs_error_wrap(err, "hls: segment open");
    }

    // This config item is used in SrsHls, we just log its value here.
    bool hls_dts_directly = _srs_config->get_vhost_hls_dts_directly(req->vhost);

    srs_trace("hls: win=%dms, frag=%dms, prefix=%s, path=%s, m3u8=%s, ts=%s, tdr=%.2f, aof=%.2f, floor=%d, clean=%d, waitk=%d, dispose=%dms, dts_directly=%d",
        srsu2msi(hls_window), srsu2msi(hls_fragment), entry_prefix.c_str(), path.c_str(), m3u8_file.c_str(), ts_file.c_str(),
        hls_td_ratio, hls_aof_ratio, ts_floor, cleanup, wait_keyframe, srsu2msi(hls_dispose), hls_dts_directly);
    
    return err;
}

srs_error_t SrsHlsController::on_unpublish()
{
    srs_error_t err = srs_success;

    if ((err = muxer->flush_audio(tsmc)) != srs_success) {
        return srs_error_wrap(err, "hls: flush audio");
    }
    
    if ((err = muxer->segment_close()) != srs_success) {
        return srs_error_wrap(err, "hls: segment close");
    }

    if ((err = muxer->on_unpublish()) != srs_success) {
        return srs_error_wrap(err, "muxer unpublish");
    }
    
    return err;
}

srs_error_t SrsHlsController::on_sequence_header()
{
    // TODO: support discontinuity for the same stream
    // currently we reap and insert discontinity when encoder republish,
    // but actually, event when stream is not republish, the
    // sequence header may change, for example,
    // ffmpeg ingest a external rtmp stream and push to srs,
    // when the sequence header changed, the stream is not republish.
    return muxer->on_sequence_header();
}

srs_error_t SrsHlsController::write_audio(SrsAudioFrame* frame, int64_t pts)
{
    srs_error_t err = srs_success;

    // Refresh the codec ASAP.
    if (muxer->latest_acodec() != frame->acodec()->id) {
        srs_trace("HLS: Switch audio codec %d(%s) to %d(%s)", muxer->latest_acodec(), srs_audio_codec_id2str(muxer->latest_acodec()).c_str(),
            frame->acodec()->id, srs_audio_codec_id2str(frame->acodec()->id).c_str());
        muxer->set_latest_acodec(frame->acodec()->id);
    }
    
    // write audio to cache.
    if ((err = tsmc->cache_audio(frame, pts)) != srs_success) {
        return srs_error_wrap(err, "hls: cache audio");
    }

    // First, update the duration of the segment, as we might reap the segment. The duration should
    // cover from the first frame to the last frame.
    muxer->update_duration(tsmc->audio->dts);
    
    // reap when current source is pure audio.
    // it maybe changed when stream info changed,
    // for example, pure audio when start, audio/video when publishing,
    // pure audio again for audio disabled.
    // so we reap event when the audio incoming when segment overflow.
    // @see https://github.com/ossrs/srs/issues/151
    // we use absolutely overflow of segment to make jwplayer/ffplay happy
    // @see https://github.com/ossrs/srs/issues/151#issuecomment-71155184
    if (tsmc->audio && muxer->is_segment_absolutely_overflow()) {
        if ((err = reap_segment()) != srs_success) {
            return srs_error_wrap(err, "hls: reap segment");
        }
    }
    
    // for pure audio, aggregate some frame to one.
    // TODO: FIXME: Check whether it's necessary.
    if (muxer->pure_audio() && tsmc->audio) {
        if (pts - tsmc->audio->start_pts < SRS_CONSTS_HLS_PURE_AUDIO_AGGREGATE) {
            return err;
        }
    }
    
    // directly write the audio frame by frame to ts,
    // it's ok for the hls overload, or maybe cause the audio corrupt,
    // which introduced by aggregate the audios to a big one.
    // @see https://github.com/ossrs/srs/issues/512
    if ((err = muxer->flush_audio(tsmc)) != srs_success) {
        return srs_error_wrap(err, "hls: flush audio");
    }
    
    return err;
}

srs_error_t SrsHlsController::write_video(SrsVideoFrame* frame, int64_t dts)
{
    srs_error_t err = srs_success;
    
    // write video to cache.
    if ((err = tsmc->cache_video(frame, dts)) != srs_success) {
        return srs_error_wrap(err, "hls: cache video");
    }

    // First, update the duration of the segment, as we might reap the segment. The duration should
    // cover from the first frame to the last frame.
    muxer->update_duration(tsmc->video->dts);
    
    // when segment overflow, reap if possible.
    if (muxer->is_segment_overflow()) {
        // do reap ts if any of:
        //      a. wait keyframe and got keyframe.
        //      b. always reap when not wait keyframe.
        if (!muxer->wait_keyframe() || frame->frame_type == SrsVideoAvcFrameTypeKeyFrame) {
            // reap the segment, which will also flush the video.
            if ((err = reap_segment()) != srs_success) {
                return srs_error_wrap(err, "hls: reap segment");
            }
        }
    }
    
    // flush video when got one
    if ((err = muxer->flush_video(tsmc)) != srs_success) {
        return srs_error_wrap(err, "hls: flush video");
    }
    
    return err;
}

srs_error_t SrsHlsController::reap_segment()
{
    srs_error_t err = srs_success;
    
    // TODO: flush audio before or after segment?
    // TODO: fresh segment begin with audio or video?
    
    // close current ts.
    if ((err = muxer->segment_close()) != srs_success) {
        // When close segment error, we must reopen it for next packet to write.
        srs_error_t r0 = muxer->segment_open();
        if (r0 != srs_success) {
            srs_warn("close segment err %s", srs_error_desc(r0).c_str());
            srs_freep(r0);
        }

        return srs_error_wrap(err, "hls: segment close");
    }
    
    // open new ts.
    if ((err = muxer->segment_open()) != srs_success) {
        return srs_error_wrap(err, "hls: segment open");
    }
    
    // segment open, flush video first.
    if ((err = muxer->flush_video(tsmc)) != srs_success) {
        return srs_error_wrap(err, "hls: flush video");
    }
    
    // segment open, flush the audio.
    // @see: ngx_rtmp_hls_open_fragment
    /* start fragment with audio to make iPhone happy */
    if ((err = muxer->flush_audio(tsmc)) != srs_success) {
        return srs_error_wrap(err, "hls: flush audio");
    }
    
    return err;
}

SrsHls::SrsHls()
{
    req = NULL;
    hub = NULL;
    
    enabled = false;
    disposable = false;
    unpublishing_ = false;
    async_reload_ = reloading_ = false;
    last_update_time = 0;
    hls_dts_directly = false;
    
    previous_audio_dts = 0;
    aac_samples = 0;
    
    jitter = new SrsRtmpJitter();
    controller = new SrsHlsController();
    
    pprint = SrsPithyPrint::create_hls();
}

SrsHls::~SrsHls()
{
    srs_freep(jitter);
    srs_freep(controller);
    srs_freep(pprint);
}

void SrsHls::async_reload()
{
    async_reload_ = true;
}

srs_error_t SrsHls::reload()
{
    srs_error_t err = srs_success;

    // Ignore if not active.
    if (!enabled) return err;

    int reloading = 0, reloaded = 0, refreshed = 0;
    err = do_reload(&reloading, &reloaded, &refreshed);
    srs_trace("async reload hls %s, reloading=%d, reloaded=%d, refreshed=%d",
        req->get_stream_url().c_str(), reloading, reloaded, refreshed);

    return err;
}

srs_error_t SrsHls::do_reload(int *reloading, int *reloaded, int *refreshed)
{
    srs_error_t err = srs_success;

    if (!async_reload_ || reloading_) return err;
    reloading_ = true;
    *reloading = 1;

    on_unpublish();
    if ((err = on_publish()) != srs_success) {
        return srs_error_wrap(err, "hls publish failed");
    }
    *reloaded = 1;

    // Before feed the sequence header, must reset the reloading.
    reloading_ = false;
    async_reload_ = false;

    // After reloading, we must request the sequence header again.
    if ((err = hub->on_hls_request_sh()) != srs_success) {
        return srs_error_wrap(err, "hls request sh");
    }
    *refreshed = 1;

    return err;
}

void SrsHls::dispose()
{
    if (enabled) {
        on_unpublish();
    }
    
    // Ignore when hls_dispose disabled.
    // @see https://github.com/ossrs/srs/issues/865
    srs_utime_t hls_dispose = _srs_config->get_hls_dispose(req->vhost);
    if (!hls_dispose) {
        return;
    }
    
    controller->dispose();
}

srs_error_t SrsHls::cycle()
{
    srs_error_t err = srs_success;

    if (last_update_time <= 0) {
        last_update_time = srs_get_system_time();
    }
    
    if (!req) {
        return err;
    }

    // When unpublishing, we must wait for it done.
    if (unpublishing_) return err;
    
    // When reloading, we must wait for it done.
    if (async_reload_) return err;

    // If not unpublishing and not reloading, try to dispose HLS stream.
    srs_utime_t hls_dispose = _srs_config->get_hls_dispose(req->vhost);
    if (hls_dispose <= 0) {
        return err;
    }
    if (srs_get_system_time() - last_update_time <= hls_dispose) {
        return err;
    }
    last_update_time = srs_get_system_time();

    if (!disposable) {
        return err;
    }
    disposable = false;

    srs_trace("hls cycle to dispose hls %s, timeout=%dms", req->get_stream_url().c_str(), hls_dispose);
    dispose();
    
    return err;
}

srs_error_t SrsHls::initialize(SrsOriginHub* h, SrsRequest* r)
{
    srs_error_t err = srs_success;
    
    hub = h;
    req = r;
    
    if ((err = controller->initialize()) != srs_success) {
        return srs_error_wrap(err, "controller initialize");
    }
    
    return err;
}

srs_error_t SrsHls::on_publish()
{
    srs_error_t err = srs_success;

    // update the hls time, for hls_dispose.
    last_update_time = srs_get_system_time();
    
    // support multiple publish.
    if (enabled) {
        return err;
    }
    
    if (!_srs_config->get_hls_enabled(req->vhost)) {
        return err;
    }
    
    if ((err = controller->on_publish(req)) != srs_success) {
        return srs_error_wrap(err, "hls: on publish");
    }

    // If enabled, directly turn FLV timestamp to TS DTS.
    // @remark It'll be reloaded automatically, because the origin hub will republish while reloading.
    hls_dts_directly = _srs_config->get_vhost_hls_dts_directly(req->vhost);
    
    // if enabled, open the muxer.
    enabled = true;
    // Reset the unpublishing state.
    unpublishing_ = false;
    
    // ok, the hls can be dispose, or need to be dispose.
    disposable = true;
    
    return err;
}

void SrsHls::on_unpublish()
{
    srs_error_t err = srs_success;
    
    // support multiple unpublish.
    if (!enabled) {
        return;
    }

    // During unpublishing, there maybe callback that switch to other coroutines.
    if (unpublishing_) return;
    unpublishing_ = true;
    
    if ((err = controller->on_unpublish()) != srs_success) {
        srs_warn("hls: ignore unpublish failed %s", srs_error_desc(err).c_str());
        srs_freep(err);
    }
    
    enabled = false;
    unpublishing_ = false;
}

srs_error_t SrsHls::on_audio(SrsSharedPtrMessage* shared_audio, SrsFormat* format)
{
    srs_error_t err = srs_success;

    // If not able to transmux to HLS, ignore.
    if (!enabled || unpublishing_) return err;
    if (async_reload_) return reload();

    // Ignore if no format->acodec, it means the codec is not parsed, or unknown codec.
    // @issue https://github.com/ossrs/srs/issues/1506#issuecomment-562079474
    if (!format->acodec) {
        return err;
    }
    
    // update the hls time, for hls_dispose.
    last_update_time = srs_get_system_time();
    
    SrsSharedPtrMessage* audio = shared_audio->copy();
    SrsAutoFree(SrsSharedPtrMessage, audio);
    
    // ts support audio codec: aac/mp3
    SrsAudioCodecId acodec = format->acodec->id;
    if (acodec != SrsAudioCodecIdAAC && acodec != SrsAudioCodecIdMP3) {
        return err;
    }
    
    // ignore sequence header
    srs_assert(format->audio);
    if (acodec == SrsAudioCodecIdAAC && format->audio->aac_packet_type == SrsAudioAacFrameTraitSequenceHeader) {
        return controller->on_sequence_header();
    }
    
    // TODO: FIXME: config the jitter of HLS.
    if ((err = jitter->correct(audio, SrsRtmpJitterAlgorithmOFF)) != srs_success) {
        return srs_error_wrap(err, "hls: jitter");
    }
    
    // Reset the aac samples counter when DTS jitter.
    if (previous_audio_dts > audio->timestamp) {
        previous_audio_dts = audio->timestamp;
        aac_samples = 0;
    }

    // The diff duration in ms between two FLV audio packets.
    int diff = ::abs((int)(audio->timestamp - previous_audio_dts));
    previous_audio_dts = audio->timestamp;

    // Guess the number of samples for each AAC frame.
    // If samples is 1024, the sample-rate is 8000HZ, the diff should be 1024/8000s=128ms.
    // If samples is 1024, the sample-rate is 44100HZ, the diff should be 1024/44100s=23ms.
    // If samples is 2048, the sample-rate is 44100HZ, the diff should be 2048/44100s=46ms.
    int nb_samples_per_frame = 0;
    int guessNumberOfSamples = diff * srs_flv_srates[format->acodec->sound_rate] / 1000;
    if (guessNumberOfSamples > 0) {
        if (guessNumberOfSamples < 960) {
            nb_samples_per_frame = 960;
        } else if (guessNumberOfSamples < 1536) {
            nb_samples_per_frame = 1024;
        } else if (guessNumberOfSamples < 3072) {
            nb_samples_per_frame = 2048;
        } else {
            nb_samples_per_frame = 4096;
        }
    }
    
    // Recalc the DTS by the samples of AAC.
    aac_samples += nb_samples_per_frame;
    int64_t dts = 90000 * aac_samples / srs_flv_srates[format->acodec->sound_rate];

    // If directly turn FLV timestamp, overwrite the guessed DTS.
    // @doc https://github.com/ossrs/srs/issues/1506#issuecomment-562063095
    if (hls_dts_directly) {
        dts = audio->timestamp * 90;
    }
    
    if ((err = controller->write_audio(format->audio, dts)) != srs_success) {
        return srs_error_wrap(err, "hls: write audio");
    }
    
    return err;
}

srs_error_t SrsHls::on_video(SrsSharedPtrMessage* shared_video, SrsFormat* format)
{
    srs_error_t err = srs_success;

    // If not able to transmux to HLS, ignore.
    if (!enabled || unpublishing_) return err;
    if (async_reload_) return reload();

    // Ignore if no format->vcodec, it means the codec is not parsed, or unknown codec.
    // @issue https://github.com/ossrs/srs/issues/1506#issuecomment-562079474
    if (!format->vcodec) {
        return err;
    }

    // update the hls time, for hls_dispose.
    last_update_time = srs_get_system_time();
    
    SrsSharedPtrMessage* video = shared_video->copy();
    SrsAutoFree(SrsSharedPtrMessage, video);
    
    // ignore info frame,
    // @see https://github.com/ossrs/srs/issues/288#issuecomment-69863909
    srs_assert(format->video);
    if (format->video->frame_type == SrsVideoAvcFrameTypeVideoInfoFrame) {
        return err;
    }
    
    srs_assert(format->vcodec);
    if (format->vcodec->id != SrsVideoCodecIdAVC) {
        return err;
    }
    
    // ignore sequence header
    if (format->video->avc_packet_type == SrsVideoAvcFrameTraitSequenceHeader) {
        return controller->on_sequence_header();
    }
    
    // TODO: FIXME: config the jitter of HLS.
    if ((err = jitter->correct(video, SrsRtmpJitterAlgorithmOFF)) != srs_success) {
        return srs_error_wrap(err, "hls: jitter");
    }
    
    int64_t dts = video->timestamp * 90;
    if ((err = controller->write_video(format->video, dts)) != srs_success) {
        return srs_error_wrap(err, "hls: write video");
    }
    
    // pithy print message.
    hls_show_mux_log();
    
    return err;
}

void SrsHls::hls_show_mux_log()
{
    pprint->elapse();
    
    if (!pprint->can_print()) {
        return;
    }
    
    // the run time is not equals to stream time,
    // @see: https://github.com/ossrs/srs/issues/81#issuecomment-48100994
    // it's ok.
    srs_trace("-> " SRS_CONSTS_LOG_HLS " time=%" PRId64 "ms, sno=%d, ts=%s, dur=%dms, dva=%dp",
              pprint->age(), controller->sequence_no(), controller->ts_url().c_str(),
              srsu2msi(controller->duration()), controller->deviation());
}


