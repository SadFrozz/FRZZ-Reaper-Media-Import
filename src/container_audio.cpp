#include "api.h"
#include "video_settings.h"
#include "container_audio.h"
#include "container_decoder.h"
#include "container_audio_dialog.h"
#include "container_native_video.h"
#include "container_peaks.h"
#include "lineparse.h"
#include "support.h"
#include <cstdio>
#include <stdexcept>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

namespace frzz {
namespace fs=std::filesystem;
namespace {
constexpr char source_type[]="FRZZ_CONTAINER_AUDIO";
thread_local bool opening_video=false;
std::atomic<bool> choosing_track{};
static bool native_multistream_enabled(std::string_view extension) {
  char preference[4096]{};
  if(!get_config_var_string("video_decprio",preference,sizeof(preference)))return false;
  std::string_view list(preference);
  constexpr std::string_view separators=" ;\t\r\n";
  while(!list.empty()) {
    const auto end=list.find_first_of(separators);
    const auto entry=list.substr(0,end);
    const auto colon=entry.find(':');
    const auto decoder=entry.substr(0,colon);
    if(decoder=="ffmpeg_mta" || decoder=="ffmpeg_hw_mta" || decoder=="ffmpeg_mta_hw") {
      if(colon==entry.npos)return true;
      auto extensions=entry.substr(colon+1);
      const bool exclude=!extensions.empty() && extensions.front()=='!';
      if(exclude)extensions.remove_prefix(1);
      bool listed=false;
      while(!extensions.empty()) {
        const auto comma=extensions.find(',');
        if(extensions.substr(0,comma)==extension){listed=true;break;}
        if(comma==extensions.npos)break;
        extensions.remove_prefix(comma+1);
      }
      if(listed!=exclude)return true;
    }
    if(end==list.npos)break;
    list.remove_prefix(end+1);
  }
  return false;
}


static std::string extension(const char* path) {
  auto ext=fs::u8path(path).extension().u8string();
  for(auto& ch:ext)if(ch>='A' && ch<='Z')ch+=('a'-'A');
  return ext;
}

class DirectDecoder final : public ISimpleMediaDecoder {
  const fs::path resource;
  const ContainerStream stream;
  const std::string file,peak_identity;
  std::unique_ptr<ContainerDecoder> decoder;
  INT64 position{};
public:
  DirectDecoder(fs::path root,ContainerStream track,std::string source,std::string peak_key)
    :resource(std::move(root)),stream(std::move(track)),file(std::move(source)),peak_identity(std::move(peak_key)){}
  ISimpleMediaDecoder* Duplicate() override {
    try{return new DirectDecoder(resource,stream,file,peak_identity);}
    catch(...){return nullptr;}
  }
  void Open(const char*,int,int,int) override {
    Close(true);
    try{decoder=open_container(file.c_str(),resource,stream);}catch(...){}
  }
  void Close(bool) override {decoder.reset();position=0;}
  const char* GetFileName() override {return peak_identity.c_str();}
  const char* GetType() override {return source_type;}
  void GetInfoString(char* buf,int size,char* title,int title_size) override {
    if(title && title_size>0)std::snprintf(title,title_size,"FRZZ container audio");
    if(buf && size>0)std::snprintf(buf,size,"Audio stream %d (%s), %d channels, %d Hz",stream.index,stream.codec.c_str(),stream.channels,stream.rate);
  }
  bool IsOpen() override {return decoder!=nullptr;}
  int GetNumChannels() override {return stream.channels;}
  int GetBitsPerSample() override {return 0;}
  double GetSampleRate() override {return stream.rate;}
  INT64 GetLength() override {return stream.frames;}
  INT64 GetPosition() override {return position;}
  void SetPosition(INT64 frame) override {position=std::clamp<INT64>(frame,0,stream.frames);}
  int ReadSamples(ReaSample* buffer,int len) override {
    if(!decoder || !buffer || len<=0 || position>=stream.frames)return 0;
    const auto count=static_cast<int>(std::min<INT64>(len,stream.frames-position));
    const auto read=decoder->read(position,buffer,count);
    position+=read;
    return read;
  }
};

static PCM_source* open_video(const char* path) {
  if(opening_video)return nullptr;
  struct Open {Open(){opening_video=true;}~Open(){opening_video=false;}} open;
  return PCM_Source_CreateFromFile(path);
}

static std::unique_ptr<PCM_source> load_video(const char* first,State& state,bool ignore_audio,
                                              bool restoring_on_main_thread=false) {
  // Native VIDEO stores its "Ignore audio" setting as AUDIO 0.
  int depth=0;
  state.lines.erase(std::remove_if(state.lines.begin(),state.lines.end(),[&depth](const auto& raw){
    auto line=std::string_view(raw);
    const auto first=line.find_first_not_of(" \t");
    if(first==line.npos)return false;
    line.remove_prefix(first);
    if(line.front()=='<')++depth;
    else if(line.front()=='>')--depth;
    return depth==0 && (line.substr(0,6)=="AUDIO " || line.substr(0,6)=="AUDIO\t");
  }),state.lines.end());
  if(ignore_audio)state.lines.insert(state.lines.begin(),"AUDIO 0");
  std::unique_ptr<PCM_source> source(PCM_Source_CreateFromType("VIDEO"));
  if(!source)return {};
  // A restored project can display its first frame before the extension timer.
  // Try once more after LoadState in case opening the source bound FFmpeg.
  if(restoring_on_main_thread)poll_native_hardware_video();
  if(source->LoadState(first,&state)<0)return {};
  if(restoring_on_main_thread)poll_native_hardware_video();
  return source;
}

static std::unique_ptr<PCM_source> open_video_without_audio(const char* path) {
  State state;
  state.AddLine("FILE %p~",path);
  state.lines.emplace_back(">");
  return load_video("<SOURCE VIDEO",state,true);
}

static std::unique_ptr<PCM_source> copy_video(PCM_source& original,const char* filename=nullptr,bool ignore_audio=true) {
  State state;
  state.SetTempFlag(PROJSTATECTX_UNDO_REDO);
  original.SaveState(&state);
  if(ignore_audio && !filename && std::find(state.lines.begin(),state.lines.end(),"AUDIO 0")!=state.lines.end())
    return std::unique_ptr<PCM_source>(original.Duplicate());
  if(filename) {
    state.lines.erase(std::remove_if(state.lines.begin(),state.lines.end(),[](const auto& line){
      const auto first=line.find_first_not_of(" \t");
      return first!=std::string::npos && line.compare(first,5,"FILE ")==0;
    }),state.lines.end());
    state.AddLine("FILE %p~",static_cast<const void*>(filename));
  }
  state.lines.emplace_back(">");
  return load_video("<SOURCE VIDEO",state,ignore_audio);
}

static std::unique_ptr<PCM_source> open_native_video_track(
    PCM_source& original,const std::string& file,const ContainerAudio& audio,int index) {
  const auto track=std::find_if(audio.streams.begin(),audio.streams.end(),[index](const auto& stream){
    return stream.index==index;
  });
  if(track==audio.streams.end() || track->frames<=0)return {};
  const bool delayed=track->start_offset_frames>0;
  if(track->start_offset_frames<0)return {};
  bool project_channels=track->channels>8 || (!track->channel_map.empty() && !track->native_planar_bed);
  for(size_t i=0;i<track->channel_map.size();++i)
    project_channels|=track->channel_map[i]!=static_cast<int>(i);
  const auto ext=extension(file.c_str());
  if(ext.size()<2 || native_multistream_enabled(ext.substr(1)))return {};
  const auto alias=native_video_track_path(file,index,static_cast<int>(track-audio.streams.begin()),
                                          track->source_channels,track->channels,NativeVideoTrackUse::decoder,
                                          delayed || project_channels?track->rate:0,track->start_offset_frames,
                                          project_channels?&track->bed_speakers:nullptr);
  if(alias.empty())return {};
  char decoder_settings[4096]{};
  get_config_var_string("video_decprio",decoder_settings,sizeof(decoder_settings));
  State state;
  state.SetTempFlag(PROJSTATECTX_UNDO_REDO);
  original.SaveState(&state);
  state.lines.erase(std::remove_if(state.lines.begin(),state.lines.end(),[](const auto& line){
    const auto first=line.find_first_not_of(" \t");
    return first!=std::string::npos &&
      (line.compare(first,5,"FILE ")==0 || line.compare(first,6,"AUDIO ")==0);
  }),state.lines.end());
  state.AddLine("FILE %p~",static_cast<const void*>(alias.c_str()));
  state.lines.emplace_back(">");
  NativeVideoTrackCheck check(alias);
  std::unique_ptr<PCM_source> source(PCM_Source_CreateFromType("VIDEO"));
  if(!source || source->LoadState("<SOURCE VIDEO",&state)<0 || !source->IsAvailable() ||
     source->GetNumChannels()!=track->channels || source->GetSampleRate()!=track->rate)return {};
  if(!native_video_track_verified(alias,decoder_settings)) {
    if(!check.used())return {};
    if(delayed || project_channels) {
      // Verify the actual first decode before letting this source enter the
      // project. Some streams advertise a start time that their packets omit.
      std::vector<ReaSample> samples(static_cast<std::size_t>(track->channels));
      PCM_source_transfer_t first{};
      first.samples=samples.data();
      first.length=1;
      first.nch=track->channels;
      first.samplerate=track->rate;
      source->GetSamples(&first);
      if(first.samples_out!=1 || (project_channels && !check.projection_used()))return {};
      if(delayed && (!check.timing_used() ||
         std::any_of(samples.begin(),samples.end(),[](ReaSample sample){return sample!=0;})))return {};
    }
    verify_native_video_track(alias,decoder_settings);
  }
  return source;
}

class ContainerSource;
struct VideoFallback {
  std::weak_ptr<int> life;
  ContainerSource* source;
  MediaItem* item;
  MediaItem_Take* take;
};
std::deque<VideoFallback> video_fallbacks;

struct NativePeakRequest {
  VideoFallback target;
  std::uint64_t media_generation;
};
std::vector<NativePeakRequest> native_peak_requests;
int native_peak_dispatch_command{};
bool native_peak_dispatch_posted{};
thread_local const std::vector<PCM_source*>* native_peak_sources{};


bool auto_peaks_enabled(bool project_load) {
  constexpr int generate_peaks_on_import=1;
  constexpr int generate_peaks_on_project_load=2;
  int size{};
  const auto* mode=static_cast<const int*>(get_config_var("peakcachegenmode",&size));
  const int flag=project_load?generate_peaks_on_project_load:generate_peaks_on_import;
  return mode && size==sizeof(*mode) && (*mode&flag);
}

struct PeakJob {
  std::unique_ptr<PCM_source> source;
  std::string key;
  bool started=false;
  bool rebuild=false;
};
std::deque<PeakJob> peak_jobs;
std::mutex peak_jobs_mutex;

void queue_peaks(PCM_source* source,bool rebuild) {
  const auto* filename=source?source->GetFileName():nullptr;
  if(!filename || !*filename || !source->IsAvailable())return;
  const std::string_view key(filename);
  std::lock_guard<std::mutex> lock(peak_jobs_mutex);
  for(auto& job:peak_jobs) {
    if(key!=job.key)continue;
    if(!job.started)job.rebuild|=rebuild;
    return;
  }
  if(auto copy=std::unique_ptr<PCM_source>(source->Duplicate()))
    peak_jobs.push_back({std::move(copy),std::string(key),false,rebuild});
}

void read_building_peaks(const char* key,PCM_source_peaktransfer_t& block) {
  std::unique_lock<std::mutex> lock(peak_jobs_mutex,std::try_to_lock);
  if(!lock || peak_jobs.empty() || !key)return;
  const auto& job=peak_jobs.front();
  // REAPER's builder exposes its in-progress peaks through the source instance it is building.
  if(job.started && job.key==key)job.source->GetPeakInfo(&block);
}

class ContainerSource final:public PCM_source {
  const fs::path resource;
  const ThreadId main_thread;
  std::string file;
  ContainerAudio info;
  std::unique_ptr<PCM_source> backend;
  std::unique_ptr<PCM_source> preview_video;
  std::unique_ptr<PCM_source> video;
  std::unique_ptr<PCM_source> default_audio;
  std::atomic<PCM_source*> ready{};
  std::atomic<PCM_source*> default_ready{};
  std::mutex peak_mutex;
  PCM_source* peak_builder{};
  std::shared_ptr<int> life=std::make_shared<int>(0);
  std::uint64_t media_generation=0;
  int stream_index=-1;
  std::atomic<bool> armed{};
  bool available=true,video_queued=false;
  bool project_load=false,peaks_checked=false;
  bool check_peaks_after_track_change=false;

  std::unique_ptr<PCM_source> make_backend(const std::string& path,const ContainerAudio& metadata,int index) const {
    if(index==-2)return std::unique_ptr<PCM_source>(open_video(path.c_str()));
    const auto track=std::find_if(metadata.streams.begin(),metadata.streams.end(),[&](const auto& s){return s.index==index;});
    if(track==metadata.streams.end() || track->frames<=0)return {};
    // Give the native peak writer the actual media metadata even when the
    // selected track needs the direct decoder for timing or channel layout.
    if(metadata.has_video) {
      const auto ext=extension(path.c_str());
      if(ext.size()>1 && !native_multistream_enabled(ext.substr(1))) {
        const auto alias=native_video_track_path(path,index,static_cast<int>(track-metadata.streams.begin()),
                                                 track->source_channels,track->channels,NativeVideoTrackUse::peaks);
        if(!alias.empty())
          return std::unique_ptr<PCM_source>(PCM_Source_CreateFromSimple(new DirectDecoder(resource,*track,path,alias),alias.c_str()));
      }
    }
    // This virtual filename is only a native peak-cache key. The decoder opens path.
    std::error_code error;
    const auto media=fs::u8path(path);
    const auto modified=fs::last_write_time(media,error);
    if(error)return {};
    const auto size=fs::file_size(media,error);
    if(error)return {};
    const auto stamp=static_cast<long long>(modified.time_since_epoch().count());
    const auto signature=std::to_string(stamp)+"-"+std::to_string(size);
    const auto peak_key=path+"."+signature+".sync3"+(track->channel_map.empty()?"":".bed")+".track"+std::to_string(index);
    return std::unique_ptr<PCM_source>(PCM_Source_CreateFromSimple(new DirectDecoder(resource,*track,path,peak_key),peak_key.c_str()));
  }

  void publish(std::unique_ptr<PCM_source> source) {
    backend=std::move(source);
    ready.store(backend.get(),std::memory_order_release);
  }

  void publish_default(std::unique_ptr<PCM_source> source) {
    default_audio=std::move(source);
    default_ready.store(default_audio.get(),std::memory_order_release);
  }

  PCM_source* stock_audio() const {
    return video_settings().multiple_audio_tracks?nullptr:default_ready.load(std::memory_order_acquire);
  }

  PCM_source* playback_source() const {
    if(auto* source=stock_audio())return source;
    return ready.load(std::memory_order_acquire);
  }

  PCM_source* picture_source() const {
    return uses_native_video()?ready.load(std::memory_order_acquire):video.get();
  }

  void update_default_file(const std::string& path,PCM_source* picture) {
    if(default_audio) {
      // Relink the existing stock source in place. REAPER can still hold the
      // address returned to a playback worker or through GetSource().
      const auto* current_path=default_audio->GetFileName();
      if(!default_ready.load(std::memory_order_acquire) || !current_path || path!=current_path)
        default_ready.store(default_audio->SetFileName(path.c_str())?default_audio.get():nullptr,
                            std::memory_order_release);
      return;
    }
    auto source=picture?copy_video(*picture,path.c_str(),false):
      std::unique_ptr<PCM_source>(open_video(path.c_str()));
    if(!source && picture)source.reset(open_video(path.c_str()));
    publish_default(std::move(source));
  }

  bool select(int index,bool with_video=false) {
    std::lock_guard<std::mutex> lock(peak_mutex);
    if(with_video && (!preview_video || !preview_video->IsAvailable() || std::strcmp(preview_video->GetType(),"VIDEO")!=0))return false;
    if(with_video) {
      if(auto source=open_native_video_track(*preview_video,file,info,index)) {
        stream_index=index;
        publish(std::move(source));
        publish_default(std::move(preview_video));
        return true;
      }
    }
    auto source=index==-2 && preview_video && preview_video->IsAvailable()?
      std::move(preview_video):make_backend(file,info,index);
    if(!source || !source->IsAvailable())return false;
    if(with_video) {
      auto picture=copy_video(*preview_video);
      if(!picture || !picture->IsAvailable())return false;
      video=std::move(picture);
    }
    stream_index=index;
    publish(std::move(source));
    if(index>=0)publish_default(std::move(preview_video));
    log("container selected stream=%d rate=%.0f channels=%d",stream_index,GetSampleRate(),GetNumChannels());
    return true;
  }

  void finish_peaks_locked() {
    if(auto* source=std::exchange(peak_builder,nullptr))source->PeaksBuild_Finish();
  }

public:
  ContainerSource(fs::path root,ThreadId thread):resource(std::move(root)),main_thread(thread){}
  ~ContainerSource() override {PeaksBuild_Finish();}
  const ContainerAudio& track_options() const {return info;}
  int selected_track() const {return stream_index;}
  bool open_track(int index) {return select(index);}
  bool uses_native_video() const {
    auto* source=ready.load(std::memory_order_acquire);
    return stream_index>=0 && source && std::strcmp(source->GetType(),"VIDEO")==0;
  }
  bool uses_stock_audio() const {return stock_audio()!=nullptr;}
  bool open_track_from(PCM_source* previous,int index) {
    auto* original=std::strcmp(previous->GetType(),source_type)==0?previous->GetSource():previous;
    if(original && std::strcmp(original->GetType(),"VIDEO")==0) {
      if(auto source=open_native_video_track(*original,file,info,index)) {
        stream_index=index;
        publish(std::move(source));
        update_default_file(file,original);
        return true;
      }
    }
    if(!open_track(index) || !attach_video(previous))return false;
    update_default_file(file,video.get());
    return true;
  }
  bool attach_video(PCM_source* previous) {
    auto* original=std::strcmp(previous->GetType(),source_type)==0?previous->GetSource():previous;
    if(!original || std::strcmp(original->GetType(),"VIDEO")!=0)return true;
    video=copy_video(*original,file.c_str());
    return video && video->IsAvailable();
  }
  PeakFileStatus peak_status() const {
    auto* source=playback_source();
    if(!source)return PeakFileStatus::missing;
    const char* key=source->GetFileName();
    if(!key || !*key)return PeakFileStatus::missing;
    char path[32768]{};
    GetPeakFileNameEx(key,path,sizeof(path),false);
    const auto channels=source->GetNumChannels();
    const auto rate=static_cast<int>(source->GetSampleRate());
    const auto frames=static_cast<std::int64_t>(std::llround(source->GetLength()*rate));
    if(!*path || channels<=0 || rate<=0 || frames<=0)return PeakFileStatus::missing;
    // DirectDecoder reports its exact output length, including timeline padding.
    const bool exact_duration=std::strcmp(source->GetType(),source_type)==0;
    return inspect_peak_file(path,channels,rate,frames,is_native_video_track_path(key)?file.c_str():nullptr,exact_duration);
  }
  bool has_video_audio() const {return uses_native_video() || (video && backend);}
  bool has_physical_peak_key() const {
    auto* source=playback_source();
    return source && is_native_video_track_path(source->GetFileName());
  }
  bool automatic_peaks_enabled() const {return auto_peaks_enabled(project_load);}
  bool is_current_peak_request(std::uint64_t generation) const {return media_generation==generation;}
  void retry_peak_check_on_attach() {peaks_checked=false;}
  void request_track_change_peaks() {check_peaks_after_track_change=true;}
  void refresh_audio_route(MediaItem* item,MediaItem_Take* take) {
    if(stream_index<0 || !available)return;
    ++media_generation;
    peaks_checked=false;
    request_track_change_peaks();
    if(has_video_audio())Extended(PCM_SOURCE_EXT_SETITEMCONTEXT,item,take,nullptr);
    else if(automatic_peaks_enabled())queue_audio_peaks(false);
  }
  void queue_audio_peaks(bool rebuild) {
    auto* source=playback_source();
    if(!source)return;
    const auto status=rebuild?PeakFileStatus::incomplete:peak_status();
    if(status!=PeakFileStatus::complete)queue_peaks(source,status==PeakFileStatus::incomplete);
  }
  void set_info(const char* path,ContainerAudio data) {
    if(file!=path) {
      file=path;
      ++media_generation;
      check_peaks_after_track_change=false;
    }
    info=std::move(data);
  }
  bool arm(){
    preview_video.reset(open_video(file.c_str()));
    if(!preview_video || !preview_video->IsAvailable() ||
       (info.has_video && std::strcmp(preview_video->GetType(),"VIDEO")!=0))return false;
    ready.store(preview_video.get(),std::memory_order_release);
    armed=true;
    return true;
  }
  bool restore_video(MediaItem_Take* take) {
    if(stream_index!=-2 || !backend)return false;
    {
      std::lock_guard<std::mutex> lock(peak_mutex);
      finish_peaks_locked();
    }
    GetSetMediaItemTakeInfo(take,"P_SOURCE",backend.get());
    if(GetMediaItemTake_Source(take)!=backend.get())return false;
    backend.release();
    return true;
  }
  PCM_source* Duplicate() override {
    try {
      auto other=std::make_unique<ContainerSource>(resource,main_thread);
      other->file=file;
      other->info=info;
      other->stream_index=stream_index;
      other->available=available;
      if(video) {
        other->video.reset(video->Duplicate());
        if(!other->video)return nullptr;
      }
      if(default_audio) {
        std::unique_ptr<PCM_source> duplicate(default_audio->Duplicate());
        if(!duplicate)return nullptr;
        other->publish_default(std::move(duplicate));
        if(!default_ready.load(std::memory_order_acquire))other->default_ready.store(nullptr);
      }
      if(auto* src=ready.load(std::memory_order_acquire)) {
        std::unique_ptr<PCM_source> duplicate(src->Duplicate());
        if(!duplicate)return nullptr;
        auto* pointer=duplicate.get();
        if(src==preview_video.get())other->preview_video=std::move(duplicate);
        else other->backend=std::move(duplicate);
        other->ready.store(pointer,std::memory_order_release);
      }
      other->armed=armed.load();
      return other.release();
    }catch(...){return nullptr;}
  }
  bool IsAvailable() override {
    auto* src=playback_source();
    return available && (src?src->IsAvailable():!file.empty()) && (!video || video->IsAvailable());
  }
  void SetAvailable(bool value) override {
    std::lock_guard<std::mutex> lock(peak_mutex);
    if(!value)finish_peaks_locked();
    available=value;
    auto* src=ready.load();
    if(src)src->SetAvailable(value);
    if(default_audio)default_audio->SetAvailable(value);
    if(!value && preview_video && preview_video.get()!=src)preview_video->SetAvailable(false);
    if(video)video->SetAvailable(value);
  }
  const char* GetType() override {return source_type;}
  // REAPER uses the wrapped native source for video and this source for audio.
  PCM_source* GetSource() override {
    if(native_peak_sources && std::find(native_peak_sources->begin(),native_peak_sources->end(),this)!=native_peak_sources->end())
      return playback_source();
    return picture_source();
  }
  const char* GetFileName() override {return file.c_str();}
  bool SetFileName(const char* name) override {
    if(!name)return false;
    try {
      const std::string path=name;
      ContainerAudio next;
      if(stream_index==-1) {
        std::unique_ptr<PCM_source> replacement(open_video(path.c_str()));
        if(!replacement || !replacement->IsAvailable())return false;
        const bool choose=video_settings().multiple_audio_tracks &&
          probe_container(path.c_str(),resource,next) && next.streams.size()>1 &&
          (!next.has_video || std::strcmp(replacement->GetType(),"VIDEO")==0);
        std::lock_guard<std::mutex> lock(peak_mutex);
        finish_peaks_locked();
        set_info(path.c_str(),std::move(next));
        if(choose) {
          preview_video=std::move(replacement);
          ready.store(preview_video.get(),std::memory_order_release);
          armed=true;
        } else {
          armed=false;
          stream_index=-2;
          publish(std::move(replacement));
          preview_video.reset();
        }
        available=true;
        return true;
      }
      if(stream_index!=-2 && !probe_container(path.c_str(),resource,next))return false;
      auto* picture=uses_native_video()?backend.get():video.get();
      if(picture) {
        if(auto native=open_native_video_track(*picture,path,next,stream_index)) {
          std::lock_guard<std::mutex> lock(peak_mutex);
          finish_peaks_locked();
          if(file!=path) {
            project_load=false;
            peaks_checked=false;
          }
          update_default_file(path,native.get());
          video.reset();
          set_info(path.c_str(),std::move(next));
          publish(std::move(native));
          available=true;
          return true;
        }
      }
      auto source=make_backend(path,next,stream_index);
      if(!source || !source->IsAvailable())return false;
      std::unique_ptr<PCM_source> replacement;
      if(picture && (path!=file || !video)) {
        replacement=path!=file?open_video_without_audio(path.c_str()):copy_video(*picture,path.c_str());
        if(!replacement || !replacement->IsAvailable())return false;
      }
      std::lock_guard<std::mutex> lock(peak_mutex);
      finish_peaks_locked();
      const auto* previous_key=backend?backend->GetFileName():nullptr;
      const auto* next_key=source->GetFileName();
      if(file!=path || (previous_key && next_key && std::strcmp(previous_key,next_key)!=0)) {
        project_load=false;
        peaks_checked=false;
      }
      if(stream_index>=0)update_default_file(path,replacement?replacement.get():picture);
      set_info(path.c_str(),std::move(next));
      if(replacement)video=std::move(replacement);
      publish(std::move(source));
      available=true;
      return true;
    } catch(...){return false;}
  }
  int GetNumChannels() override {auto* src=playback_source();return src?src->GetNumChannels():1;}
  double GetSampleRate() override {auto* src=playback_source();return src?src->GetSampleRate():0;}
  double GetLength() override {
    auto* src=video?video.get():playback_source();
    return src?src->GetLength():std::max(0.001,info.duration);
  }
  int GetBitsPerSample() override {auto* src=playback_source();return src?src->GetBitsPerSample():0;}
  int PropertiesWindow(HWND parent) override {
    auto* src=ready.load(std::memory_order_acquire);
    return src?src->PropertiesWindow(parent):0;
  }
  void GetSamples(PCM_source_transfer_t* block) override {
    if(auto* src=playback_source())src->GetSamples(block);
    else block->samples_out=0;
  }
  void SaveState(ProjectStateContext* context) override {
    char relative[32768]{};
    if((context->GetTempFlag()&0xffff)!=PROJSTATECTX_UNDO_REDO)relative_fn(file.c_str(),relative,sizeof(relative));
    const auto* filename=relative[0]?relative:file.c_str();
    context->AddLine("FILE %p~",static_cast<const void*>(filename));
    if(stream_index!=-1)context->AddLine("STREAM %d",stream_index);
    if(auto* picture=picture_source()) {
      State state;
      state.SetTempFlag(context->GetTempFlag());
      picture->SaveState(&state);
      context->AddLine("<SOURCE VIDEO");
      for(const auto& line:state.lines) {
        const auto first=line.find_first_not_of(" \t");
        if(first!=std::string::npos && line.compare(first,5,"FILE ")==0)
          context->AddLine("FILE %p~",static_cast<const void*>(filename));
        else context->AddLine("%s",line.c_str());
      }
      context->AddLine(">");
    }
  }
  int LoadState(const char*,ProjectStateContext* context) override {
    try {
      std::string path;
      int index=-1;
      char line[32768];
      while(context->GetLine(line,sizeof(line))>=0) {
        LineParser parser;
        if(parser.parse(line) || !parser.getnumtokens())continue;
        const auto* key=parser.gettoken_str(0);
        if(!strcmp(key,">")) {
          armed=false;
          ++media_generation;
          peaks_checked=false;
          check_peaks_after_track_change=false;
          const int state_kind=context->GetTempFlag()&0xffff;
          project_load=state_kind==0 || state_kind==PROJSTATECTX_SAVE_LOAD;
          stream_index=index==-1?-2:index;
          char resolved[32768]{};
          if(!path.empty())resolve_fn(path.c_str(),resolved,sizeof(resolved));
          file=resolved[0]?resolved:std::move(path);
          available=!file.empty() && SetFileName(file.c_str());
          return 0;
        }
        if(!strcmp(key,"FILE"))path=parser.gettoken_str(1);
        else if(!strcmp(key,"STREAM"))index=parser.gettoken_int(1);
        else if(!strcmp(key,"<SOURCE") && !strcmp(parser.gettoken_str(1),"VIDEO")) {
          const std::string first=line;
          State state;
          state.SetTempFlag(context->GetTempFlag());
          int depth=1;
          while(depth && context->GetLine(line,sizeof(line))>=0) {
            state.lines.emplace_back(line);
            const auto* row=line;
            while(*row==' ' || *row=='\t')++row;
            if(*row=='<')++depth;
            else if(*row=='>')--depth;
          }
          if(depth)return -1;
          video=load_video(first.c_str(),state,true,current_thread_id()==main_thread);
          if(!video)return -1;
        }
      }
    }catch(...){}
    return -1;
  }
  int Extended(int call,void* p1,void* p2,void* p3) override {
    const bool attached=call==PCM_SOURCE_EXT_SETITEMCONTEXT && p1 && p2 && current_thread_id()==main_thread;
    if(attached && armed && !choosing_track) {
      choosing_track=true;
      try {
        log("container context source=%p item=%p take=%p",this,p1,p2);
        const auto ext=extension(file.c_str());
        const auto mode=info.has_video?TrackDialogMode::import_video:TrackDialogMode::import_audio;
        const int chosen=!video_settings().multiple_audio_tracks ||
          (ext.size()>1 && native_multistream_enabled(ext.substr(1)))?-2:choose_container_stream(info.streams,mode);
        if(!select(chosen,chosen>=0 && info.has_video)) {
          if(chosen>=0)ShowMessageBox("The selected stream could not be opened by FFmpeg.","FRZZ audio track import",0);
          if(!select(-2))available=false;
        }
      }catch(...){}
      armed=false;
      choosing_track=false;
    }
    // Native media import and project loading already enqueue VIDEO in REAPER.
    // A source replacement needs an explicit request if its cache is missing.
    if(attached && (!uses_native_video() || check_peaks_after_track_change) &&
       has_video_audio() && available && stream_index>=0 && !peaks_checked) {
      peaks_checked=true;
      try {
        if(automatic_peaks_enabled() && peak_status()!=PeakFileStatus::complete)
          native_peak_requests.push_back({
            {life,this,static_cast<MediaItem*>(p1),static_cast<MediaItem_Take*>(p2)},media_generation});
      }catch(...){}
    }
    if(attached && stream_index==-2 && !video_queued) {
      try {
        video_fallbacks.push_back({life,this,static_cast<MediaItem*>(p1),static_cast<MediaItem_Take*>(p2)});
        video_queued=true;
      }catch(...){}
    }
    if(call==PCM_SOURCE_EXT_SETITEMCONTEXT) {
      if(video)video->Extended(call,p1,p2,p3);
      if(auto* stock=default_ready.load(std::memory_order_acquire))stock->Extended(call,p1,p2,p3);
      if(auto* src=ready.load(std::memory_order_acquire))return src->Extended(call,p1,p2,p3);
    } else if(auto* src=playback_source())return src->Extended(call,p1,p2,p3);
    return 0;
  }
  void GetPeakInfo(PCM_source_peaktransfer_t* block) override {
    block->peaks_out=0;
    auto* source=playback_source();
    if(!source)return;
    source->GetPeakInfo(block);
    if(block->peaks_out || std::strcmp(source->GetType(),source_type)!=0)return;
    try {
      const auto* key=source->GetFileName();
      // Physical aliases use REAPER's reader even while their cache is building.
      if(is_native_video_track_path(key))return;
      // Older hosts use a virtual filename that the native reader cannot stat.
      read_building_peaks(key,*block);
      if(block->peaks_out)return;
      char path[32768]{};
      GetPeakFileNameEx(key,path,sizeof(path),false);
      const auto channels=source->GetNumChannels();
      const auto rate=static_cast<int>(source->GetSampleRate());
      const auto frames=static_cast<std::int64_t>(std::llround(source->GetLength()*rate));
      if(*path && channels>0 && rate>0 && frames>0)read_peak_file(path,channels,rate,frames,*block);
    } catch(...) {}
  }
  void Peaks_Clear(bool erase) override {
    std::lock_guard<std::mutex> lock(peak_mutex);
    finish_peaks_locked();
    if(auto* src=playback_source();src && !armed)src->Peaks_Clear(erase);
  }
  int PeaksBuild_Begin() override {
    std::lock_guard<std::mutex> lock(peak_mutex);
    if(peak_builder)return 0;
    auto* source=playback_source();
    if(!source || armed)return 0;
    const int started=source->PeaksBuild_Begin();
    if(!started)return 0;
    peak_builder=source;
    return started;
  }
  int PeaksBuild_Run() override {
    std::lock_guard<std::mutex> lock(peak_mutex);
    return peak_builder?peak_builder->PeaksBuild_Run():0;
  }
  void PeaksBuild_Finish() override {
    std::lock_guard<std::mutex> lock(peak_mutex);
    finish_peaks_locked();
  }
};
}

PCM_source* create_container_source(const char* path) {
  const auto ext=extension(path);
  if(opening_video || choosing_track || (ext!=".mkv" && ext!=".mp4"))return nullptr;
#ifdef _WIN32
  // REAPER can bind FFmpeg as part of import before the next timer callback.
  // Install the existing HDR callbacks before opening this media source.
  poll_native_hardware_video();
#endif
  if(!video_settings().multiple_audio_tracks || native_multistream_enabled(ext.substr(1)))return nullptr;
  const auto resource=fs::u8path(GetResourcePath());
  ContainerAudio probe;
  if(!probe_container(path,resource,probe) || probe.streams.size()<2)return nullptr;
  auto source=std::make_unique<ContainerSource>(resource,current_thread_id());
  source->set_info(path,std::move(probe));
  if(!source->arm())return nullptr;
  return source.release();
}

PCM_source* create_container_type(const char* type,ThreadId main_thread) {
  if(std::strcmp(type,source_type)!=0)return nullptr;
  return new ContainerSource(fs::u8path(GetResourcePath()),main_thread);
}

static PCM_source* section_media(PCM_source* source) {
  for(int depth=0;source && depth<32;++depth) {
    if(std::strcmp(source->GetType(),"SECTION")!=0)return source;
    source=source->GetSource();
  }
  return nullptr;
}

void refresh_container_audio_routes() {
  for(int p=0;auto* project=EnumProjects(p,nullptr,0);++p) {
    const auto items=CountMediaItems(project);
    for(int i=0;i<items;++i) {
      auto* item=GetMediaItem(project,i);
      for(int t=0;t<CountTakes(item);++t) {
        auto* take=GetTake(item,t);
        auto* source=section_media(GetMediaItemTake_Source(take));
        if(auto* container=dynamic_cast<ContainerSource*>(source))container->refresh_audio_route(item,take);
      }
    }
  }
  UpdateArrange();
}

const char* container_track_file(PCM_source* source) {
  source=section_media(source);
  return source?source->GetFileName():nullptr;
}

static bool switching_supported(std::string_view ext) {
  return video_settings().multiple_audio_tracks &&
    (ext==".mkv" || ext==".mp4") && !native_multistream_enabled(ext.substr(1));
}

static bool read_track_options(PCM_source* source,const char* path,ContainerAudio& audio,int& current) {
  const auto type=std::string_view(source->GetType());
  if(type==source_type) {
    const auto* container=dynamic_cast<ContainerSource*>(source);
    if(!container)return false;
    audio=container->track_options();
    current=container->selected_track();
  } else if(type=="VIDEO") {
    if(!probe_container(path,fs::u8path(GetResourcePath()),audio))return false;
    current=-1;
  } else return false;
  return true;
}

static bool has_alternate_track(const ContainerAudio& audio,int current) {
  return audio.streams.size()>1 && std::any_of(audio.streams.begin(),audio.streams.end(),[current](const auto& stream){
    return stream.frames>0 && stream.index!=current;
  });
}

bool container_track_options(PCM_source* source,ContainerAudio& audio,int& current,bool audio_import_only) {
  source=section_media(source);
  if(audio_import_only && (!source || std::strcmp(source->GetType(),source_type)!=0 || source->GetSource()))return false;
  const auto* path=source?source->GetFileName():nullptr;
  return path && *path && switching_supported(extension(path)) &&
    read_track_options(source,path,audio,current) && has_alternate_track(audio,current);
}

VideoTrackOptions video_track_options(PCM_source* source,ContainerAudio& audio,int& current,
                                      VideoTrackProbes& probes,bool switching_only) {
  source=section_media(source);
  if(!source)return VideoTrackOptions::not_video;
  const auto type=std::string_view(source->GetType());
  if(type!="VIDEO" && (type!=source_type || !source->GetSource()))return VideoTrackOptions::not_video;
  const auto* path=source->GetFileName();
  const bool has_path=path && *path;
  const bool supported=has_path && switching_supported(extension(path));
  if(switching_only && !supported)return VideoTrackOptions::unavailable;
  bool found=false;
  if(has_path && type=="VIDEO") {
    auto [entry,first]=probes.try_emplace(path);
    if(first && !read_track_options(source,path,entry->second.emplace(),current))entry->second.reset();
    if(entry->second) {
      audio=*entry->second;
      current=-1;
      found=true;
    }
  } else if(has_path) {
    found=read_track_options(source,path,audio,current);
  }
  if(!found) {
    int audio_only=0;
    source->Extended(PCM_SOURCE_EXT_NOAUDIO,&audio_only,nullptr,nullptr);
    return audio_only?VideoTrackOptions::not_video:VideoTrackOptions::unavailable;
  }
  if(!audio.has_video)return VideoTrackOptions::not_video;
  if(audio.streams.size()==1)return VideoTrackOptions::single;
  if(!supported || !has_alternate_track(audio,current))
    return VideoTrackOptions::unavailable;
  return VideoTrackOptions::selectable;
}

PCM_source* create_selected_container_source(PCM_source* previous,ContainerAudio audio,int stream) {
  auto* media=section_media(previous);
  if(!media)return nullptr;
  auto source=std::make_unique<ContainerSource>(fs::u8path(GetResourcePath()),current_thread_id());
  source->set_info(media->GetFileName(),std::move(audio));
  if(!source->open_track_from(media,stream))return nullptr;
  source->request_track_change_peaks();
  if(media==previous)return source.release();
  std::unique_ptr<PCM_source> copy(previous->Duplicate());
  if(!copy)return nullptr;
  auto* section=copy.get();
  for(int depth=0;section && depth<32;++depth) {
    auto* child=section->GetSource();
    if(!child)return nullptr;
    if(std::strcmp(child->GetType(),"SECTION")==0) {
      section=child;
      continue;
    }
    section->SetSource(source.get());
    if(section->GetSource()!=source.get())return nullptr;
    source.release();
    return copy.release();
  }
  return nullptr;
}

void queue_automatic_item_source_peaks(PCM_source* source,bool project_load) {
  if(auto_peaks_enabled(project_load))queue_item_source_peaks(source,false);
}

bool native_peak_request_active() {return native_peak_sources!=nullptr;}

bool has_video_container_audio(PCM_source* source) {
  auto* container=dynamic_cast<ContainerSource*>(section_media(source));
  return container && container->has_video_audio();
}

PCM_source* container_peak_source_override(PCM_source* source,bool& physical_key) {
  auto* container=dynamic_cast<ContainerSource*>(section_media(source));
  if(!container || !container->has_video_audio())return nullptr;
  const bool stock=container->uses_stock_audio();
  if(!stock && container->uses_native_video())return nullptr;
  physical_key=stock || container->has_physical_peak_key();
  return container;
}

void run_container_native_peak_command(int command,int flag,ReaProject* project,
                                       const std::vector<PCM_source*>& sources) {
  // Let the native action see the selected audio leaf while the take retains its video.
  struct SourceScope {
    const std::vector<PCM_source*>* previous=native_peak_sources;
    explicit SourceScope(const std::vector<PCM_source*>& sources){native_peak_sources=&sources;}
    ~SourceScope(){native_peak_sources=previous;}
  } scope(sources);
  Main_OnCommandEx(command,flag,project);
}

void queue_item_source_peaks(PCM_source* source,bool rebuild) {
  source=section_media(source);
  if(auto* container=dynamic_cast<ContainerSource*>(source))container->queue_audio_peaks(rebuild);
  else queue_peaks(source,rebuild);
}

void poll_container_peaks() {
  static std::uint64_t last_redraw=0;
  bool finished=false;
  {
    std::lock_guard<std::mutex> lock(peak_jobs_mutex);
    if(peak_jobs.empty())return;
    auto& job=peak_jobs.front();
    if(!job.started) {
      if(job.rebuild)job.source->Peaks_Clear(true);
      job.started=job.source->PeaksBuild_Begin()!=0;
      if(!job.started) {
        peak_jobs.pop_front();
        return;
      }
    }
    finished=!job.source->PeaksBuild_Run();
    if(finished) {
      job.source->PeaksBuild_Finish();
      peak_jobs.pop_front();
    }
  }
  const auto now=monotonic_ms();
  if(finished || now-last_redraw>=200) {
    last_redraw=now;
    UpdateArrange();
  }
}

void cancel_container_peaks() {
  native_peak_requests.clear();
  std::lock_guard<std::mutex> lock(peak_jobs_mutex);
  peak_jobs.clear();
}

static bool native_peak_requests_ready() {
  return !native_peak_requests.empty() && !choosing_track && !native_peak_request_active();
}

void set_native_peak_dispatch_command(int command) {
  native_peak_dispatch_command=command;
  native_peak_dispatch_posted=false;
}

void dispatch_native_peak_requests() {
  native_peak_dispatch_posted=false;
  if(!native_peak_requests_ready())return;
  auto requests=std::move(native_peak_requests);
  native_peak_requests.clear();
  for(int index=0;;++index) {
    auto* project=EnumProjects(index,nullptr,0);
    if(!project)break;
    std::vector<PCM_source*> sources;
    std::vector<MediaItem*> targets;
    for(const auto& [job,generation]:requests) {
      if(job.life.expired() || !ValidatePtr2(project,job.item,"MediaItem*") ||
         !ValidatePtr2(project,job.take,"MediaItem_Take*") || GetMediaItemTake_Item(job.take)!=job.item ||
         section_media(GetMediaItemTake_Source(job.take))!=job.source || !job.source->is_current_peak_request(generation) ||
         !job.source->automatic_peaks_enabled())continue;
      const auto status=job.source->peak_status();
      if(status==PeakFileStatus::complete)continue;
      if(status==PeakFileStatus::incomplete) {
        if(GetActiveTake(job.item)!=job.take) {
          job.source->retry_peak_check_on_attach();
          continue;
        }
        if(job.source->has_physical_peak_key()) {
          // REAPER can accept a cache that covers only part of the source.
          // Invalidate just this source before running the normal missing-peaks action.
          job.source->Peaks_Clear(true);
        } else if(!job.source->uses_native_video()) {
          job.source->queue_audio_peaks(true);
          continue;
        }
      }
      sources.push_back(job.source);
      targets.push_back(job.item);
    }
    if(targets.empty())continue;

    struct RestoreSelection {
      ReaProject* project;
      std::vector<std::pair<MediaItem*,bool>> changed;
      ~RestoreSelection() {
        for(const auto& [item,selected]:changed)
          if(ValidatePtr2(project,item,"MediaItem*"))SetMediaItemSelected(item,selected);
      }
      void set(MediaItem* item,bool selected) {
        if(IsMediaItemSelected(item)==selected)return;
        changed.emplace_back(item,!selected);
        SetMediaItemSelected(item,selected);
      }
    } selection{project,{}};
    for(int i=CountSelectedMediaItems(project)-1;i>=0;--i) {
      auto* item=GetSelectedMediaItem(project,i);
      if(std::find(targets.begin(),targets.end(),item)==targets.end())selection.set(item,false);
    }
    for(auto* item:targets)selection.set(item,true);

    constexpr int build_missing_peaks_for_selected_items=40245;
    run_container_native_peak_command(build_missing_peaks_for_selected_items,0,project,sources);
  }
}

void poll_container_video() {
  auto jobs=std::move(video_fallbacks);
  video_fallbacks.clear();
  for(const auto& job:jobs) {
    if(job.life.expired())continue;
    for(int index=0;;++index) {
      auto* proj=EnumProjects(index,nullptr,0);
      if(!proj)break;
      if(!ValidatePtr2(proj,job.item,"MediaItem*") || !ValidatePtr2(proj,job.take,"MediaItem_Take*") ||
         GetMediaItemTake_Item(job.take)!=job.item || GetMediaItemTake_Source(job.take)!=job.source)continue;
      if(job.source->restore_video(job.take)) {
        delete job.source;
        UpdateItemInProject(job.item);
      }
      break;
    }
  }
  if(native_peak_dispatch_command && !native_peak_dispatch_posted && native_peak_requests_ready())
    native_peak_dispatch_posted=PostMessage(GetMainHwnd(),WM_COMMAND,static_cast<WPARAM>(native_peak_dispatch_command),0)!=0;
}
}
