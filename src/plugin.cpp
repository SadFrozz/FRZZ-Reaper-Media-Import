#define REAPERAPI_IMPLEMENT
#include "support.h"
#include "subtitle_parser.h"
#include "media_metadata.h"
#include "thd_ac3.h"
#include "lineparse.h"
#include "wdl_base64.h"
#include <string_view>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <algorithm>
#include <cmath>
namespace frzz {
static std::mutex log_mutex;
static std::ofstream log_file;
static ThreadId main_thread;
void log(const char* fmt, ...) noexcept {
#ifdef FRZZ_ENABLE_LOGGING
  try {
    char buf[8192]; va_list args; va_start(args,fmt);
    vsnprintf(buf,sizeof(buf),fmt,args); va_end(args);
    std::lock_guard<std::mutex> lock(log_mutex);
    if(log_file) {log_file << monotonic_ms() << " tid=" << numeric_thread_id() << " " << buf << '\n'; log_file.flush();}
  } catch (...) {}
#else
  (void)fmt;
#endif
}
static std::atomic<unsigned long long> next_id{1};
struct Lifetime { PCM_source* source{}; unsigned long long id{}; bool armed=true; bool queued=false; };
struct Pending { std::weak_ptr<Lifetime> life; MediaItem* item{}; MediaItem_Take* take{}; std::string item_guid; };
static std::mutex queue_mutex;
static std::vector<Pending> pending;
// The native undo point can contain the placeholder rather than the final items.
static constexpr char subtitle_undo_chunk[]="<FRZZ_SUBTITLE_UNDO_CUES";
static constexpr int max_subtitle_undo_bytes=32*1024*1024;

template<class Value>
static void append_undo_value(std::string& bytes,const Value& value) {
  bytes.append(reinterpret_cast<const char*>(&value),sizeof(value));
}

template<class Value>
static bool read_undo_value(std::string_view& bytes,Value& value) {
  if(bytes.size()<sizeof(value))return false;
  std::memcpy(&value,bytes.data(),sizeof(value));
  bytes.remove_prefix(sizeof(value));
  return true;
}

static void save_undo_cues(ProjectStateContext* ctx,const std::vector<Cue>& cues) {
  std::string bytes;
  for(const auto& cue:cues) {
    constexpr auto header_bytes=2*sizeof(double)+sizeof(std::uint32_t);
    if(bytes.size()+header_bytes+cue.text.size()>max_subtitle_undo_bytes)
      throw std::runtime_error("Subtitle undo snapshot exceeds size limit");
    append_undo_value(bytes,cue.start);
    append_undo_value(bytes,cue.end);
    append_undo_value(bytes,static_cast<std::uint32_t>(cue.text.size()));
    bytes+=cue.text;
  }
  ctx->AddLine("%s",subtitle_undo_chunk);
  cfg_encode_binary(ctx,bytes.data(),static_cast<int>(bytes.size()));
  ctx->AddLine(">");
}

static std::vector<Cue> load_undo_cues(ProjectStateContext* ctx) {
  std::string decoded;
  char line[4096];
  bool complete=false;
  while(ctx->GetLine(line,sizeof(line))>=0) {
    const char* start=line;
    while(*start==' ' || *start=='\t')++start;
    if(!std::strcmp(start,">")){complete=true;break;}
    unsigned char block[3072];
    const int size=wdl_base64decode(start,block,sizeof(block));
    if(size<=0 || decoded.size()+static_cast<size_t>(size)>max_subtitle_undo_bytes)
      throw std::runtime_error("Invalid subtitle undo snapshot");
    decoded.append(reinterpret_cast<const char*>(block),size);
  }
  if(!complete || decoded.empty())throw std::runtime_error("Incomplete subtitle undo snapshot");

  std::string_view bytes(decoded);
  std::vector<Cue> restored;
  while(!bytes.empty()) {
    Cue cue;
    std::uint32_t length;
    if(!read_undo_value(bytes,cue.start) || !read_undo_value(bytes,cue.end) ||
       !read_undo_value(bytes,length) || length>bytes.size() ||
       !std::isfinite(cue.start) || !std::isfinite(cue.end) || cue.start<0 || cue.end<cue.start)
      throw std::runtime_error("Invalid subtitle undo cue");
    cue.text.assign(bytes.substr(0,length));
    if(cue.text.find('\0')!=std::string::npos)throw std::runtime_error("Invalid subtitle undo text");
    bytes.remove_prefix(length);
    restored.push_back(std::move(cue));
  }
  return restored;
}

class SubtitleSource final : public PCM_source {
public:
  const unsigned long long id=next_id++;
  std::string file;
  std::vector<Cue> cues;
  bool available=false;
  std::shared_ptr<Lifetime> life=std::make_shared<Lifetime>();
  std::set<int> logged_calls;
  std::mutex calls_mutex;
  SubtitleSource() {life->source=this; life->id=id; log("subtitle created id=%llu source=%p",id,this);}
  ~SubtitleSource() override {log("subtitle destroyed id=%llu source=%p",id,this);}
  PCM_source* Duplicate() override {
    try { auto s=std::make_unique<SubtitleSource>(); s->file=file; s->cues=cues; s->available=available; s->life->armed=life->armed; log("subtitle Duplicate old=%llu new=%llu",id,s->id); return s.release(); } catch (...) {return nullptr;}
  }
  bool IsAvailable() override {return available;}
  void SetAvailable(bool v) override { available=v && !cues.empty(); }
  const char* GetType() override {return "FRZZ_SUBTITLE";}
  const char* GetFileName() override {return file.c_str();}
  bool SetFileName(const char* name) override {
    try { file=name?name:""; cues=parse_subtitles(read_utf8(file.c_str()),identify_format(file)); available=true; log("subtitle parsed id=%llu cues=%zu",id,cues.size()); return true; }
    catch(const std::exception& e) {available=false; log("subtitle error id=%llu %s",id,e.what()); return false;}
    catch(...) {available=false; return false;}
  }
  int GetNumChannels() override {return 1;}
  double GetSampleRate() override {return 0;}
  double GetLength() override {double end=0; for(auto& c:cues) end=std::max(end,c.end); return std::max(0.001,end);}
  int PropertiesWindow(HWND) override {return 0;}
  void GetSamples(PCM_source_transfer_t* block) override {block->samples_out=0;}
  void GetPeakInfo(PCM_source_peaktransfer_t* block) override {block->peaks_out=0;}
  void Peaks_Clear(bool) override {}
  int PeaksBuild_Begin() override {return 0;}
  int PeaksBuild_Run() override {return 0;}
  void PeaksBuild_Finish() override {}
  void SaveState(ProjectStateContext* ctx) override {
    try {
      State state;
      state.AddLine("FILE %p~",file.c_str());
      ctx->AddLine("%s",state.lines.front().c_str());
      if((ctx->GetTempFlag()&0xffff)==PROJSTATECTX_UNDO_REDO &&
         current_thread_id()==main_thread && life->armed && life->queued)
        save_undo_cues(ctx,cues);
      log("subtitle SaveState id=%llu flag=%d",id,ctx->GetTempFlag());
    } catch(const std::exception& e) {log("subtitle SaveState error id=%llu %s",id,e.what());}
      catch(...) {log("subtitle SaveState error id=%llu",id);}
  }
  int LoadState(const char*,ProjectStateContext* ctx) override {
    life->armed=false;
    life->queued=false;
    available=false;
    cues.clear();
    try {
      const bool is_undo=(ctx->GetTempFlag()&0xffff)==PROJSTATECTX_UNDO_REDO;
      std::vector<Cue> restored;
      std::string saved_file;
      char line[32768];
      while(ctx->GetLine(line,sizeof(line))>=0) {
        LineParser lp(false); if(lp.parse(line) || !lp.getnumtokens())continue;
        const char* token=lp.gettoken_str(0);
        if(!strcmp(token,">")) {
          if(is_undo && !restored.empty()) {
            file=std::move(saved_file);
            cues=std::move(restored);
            available=life->armed=true;
          } else if(!saved_file.empty())SetFileName(saved_file.c_str());
          return 0;
        }
        if(!strcmp(token,"FILE"))saved_file=lp.gettoken_str(1);
        if(!strcmp(token,subtitle_undo_chunk)) {
          auto snapshot=load_undo_cues(ctx);
          if(is_undo)restored=std::move(snapshot);
        }
      }
    } catch(const std::exception& e) {log("subtitle LoadState error id=%llu %s",id,e.what());}
      catch(...) {log("subtitle LoadState error id=%llu",id);}
    return -1;
  }
  int Extended(int call,void* p1,void* p2,void* p3) override {
    try {
      {std::lock_guard<std::mutex> lock(calls_mutex); if(logged_calls.insert(call).second) log("subtitle Extended id=%llu call=0x%x p1=%p p2=%p p3=%p",id,call,p1,p2,p3);}
      if(call==PCM_SOURCE_EXT_SETITEMCONTEXT) {
        log("SETITEMCONTEXT id=%llu item=%p take=%p main_thread=%d armed=%d",id,p1,p2,current_thread_id()==main_thread,life->armed);
        if(p1 && p2 && life->armed && current_thread_id()==main_thread) {
          char guid[128]{};
          if(!GetSetMediaItemInfo_String(static_cast<MediaItem*>(p1),"GUID",guid,false) || !guid[0]) {log("context lacks item GUID id=%llu",id);return 1;}
          auto item=static_cast<MediaItem*>(p1);
          auto take=static_cast<MediaItem_Take*>(p2);
          std::lock_guard<std::mutex> lock(queue_mutex);
          const auto duplicate=std::find_if(pending.begin(),pending.end(),[&](const Pending& p){
            return p.item==item && p.take==take && p.life.lock()==life;
          });
          if(duplicate!=pending.end())return 1;
          pending.push_back({life,item,take,guid});
          life->queued=true;
          log("conversion queued id=%llu guid=%s",id,guid);
        }
        return 1;
      }
    } catch (...) {}
    return 0;
  }
};
static ReaProject* owning_project(MediaItem* item,MediaItem_Take* take,PCM_source* src) {
  for(int i=0;;++i) {
    auto proj=EnumProjects(i,nullptr,0); if(!proj) return nullptr;
    if(ValidatePtr2(proj,item,"MediaItem*") && ValidatePtr2(proj,take,"MediaItem_Take*") && GetMediaItemTake_Item(take)==item && GetMediaItemTake_Source(take)==src) return proj;
  }
}
struct RefreshLock {
  RefreshLock(){PreventUIRefresh(1);}
  ~RefreshLock(){PreventUIRefresh(-1);}
};
static std::string with_notes(MediaItem* item,const std::string& text) {
  std::vector<char> buf(4096);
  while(!GetItemStateChunk(item,buf.data(),static_cast<int>(buf.size()),false)) {
    if(buf.size()>=1024*1024) throw std::runtime_error("Cannot retrieve new item state");
    buf.resize(buf.size()*2);
  }
  std::string chunk=buf.data();
  const auto end=chunk.rfind('>'); if(end==std::string::npos) throw std::runtime_error("Invalid new item state");
  std::string notes="<NOTES\n|";
  for(char c:text) {notes+=c; if(c=='\n') notes+='|';}
  notes+="\n>\nIMGRESOURCEFLAGS 8\n";
  chunk.insert(end,notes); return chunk;
}
static void process_pending() {
  std::vector<Pending> jobs;
  {std::lock_guard<std::mutex> lock(queue_mutex); jobs.swap(pending);}
  for(auto& p:jobs) {
    auto l=p.life.lock(); if(!l || !l->armed) continue;
    auto proj=owning_project(p.item,p.take,l->source);
    if(!proj) {log("conversion cancelled id=%llu invalid context",l->id);continue;}
    char guid[128]{};
    if(!GetSetMediaItemInfo_String(p.item,"GUID",guid,false) || p.item_guid!=guid) {
      log("conversion cancelled id=%llu changed item identity",l->id);
      continue;
    }
    // Only a standalone, untrimmed placeholder is eligible for deletion.
    constexpr double length_tolerance=1e-7;
    const auto item_length=GetMediaItemInfo_Value(p.item,"D_LENGTH");
    if(CountTakes(p.item)!=1 || GetMediaItemTakeInfo_Value(p.take,"D_STARTOFFS")!=0 ||
       GetMediaItemTakeInfo_Value(p.take,"D_PLAYRATE")!=1 || item_length+length_tolerance<l->source->GetLength()) {
      l->armed=false;
      log("conversion refused id=%llu multi-take/section/rate/trim",l->id);
      continue;
    }
    auto track=GetMediaItemTrack(p.item);
    const double origin=GetMediaItemInfo_Value(p.item,"D_POSITION");
    const auto cues=static_cast<SubtitleSource*>(l->source)->cues;
    l->armed=false;
    std::vector<MediaItem*> created;
    created.reserve(cues.size());
    {
      RefreshLock refresh;
      try {
        for(auto& cue:cues) {
          auto item=AddMediaItemToTrack(track); if(!item) throw std::runtime_error("Cannot create text item");
          created.push_back(item);
          if(!SetMediaItemInfo_Value(item,"D_POSITION",origin+cue.start) || !SetMediaItemInfo_Value(item,"D_LENGTH",cue.end-cue.start)) throw std::runtime_error("Cannot set text item timing");
          const auto chunk=with_notes(item,cue.text);
          if(!SetItemStateChunk(item,chunk.c_str(),false)) throw std::runtime_error("Cannot write NOTES chunk");
        }
        if(!DeleteTrackMediaItem(track,p.item)) throw std::runtime_error("Cannot delete placeholder");
        log("placeholder removed id=%llu",l->id);
        double start=0,end=0; GetSet_LoopTimeRange2(proj,true,false,&start,&end,false);
        SelectAllMediaItems(proj,false);
        log("conversion complete id=%llu created=%zu origin=%.9f track=%p",l->id,created.size(),origin,track);
      } catch(const std::exception& e) {
        for(auto item:created) DeleteTrackMediaItem(track,item);
        log("conversion rollback id=%llu error=%s",l->id,e.what());
      } catch(...) {
        for(auto item:created) DeleteTrackMediaItem(track,item);
        log("conversion rollback id=%llu unknown error",l->id);
      }
    }
    UpdateArrange();
  }
}
static PCM_source* create_video(const char* path) {
  std::unique_ptr<PCM_source> src(PCM_Source_CreateFromType("VIDEO"));
  if(!src) {log("VIDEO constructor failed path=%s",path); return nullptr;}
  State state;
  state.lines.emplace_back("VIDEO_DISABLED");
  state.AddLine("FILE %p~",path);
  state.lines.emplace_back(">");
  int result=src->LoadState("<SOURCE VIDEO",&state);
  log("VIDEO LoadState result=%d available=%d type=%s length=%.9f channels=%d samplerate=%.0f file=%s",result,src->IsAvailable(),src->GetType(),src->GetLength(),src->GetNumChannels(),src->GetSampleRate(),path);
  char info[4096]{};
  if(src->Extended(PCM_SOURCE_EXT_GETINFOSTRING,info,reinterpret_cast<void*>(static_cast<INT_PTR>(sizeof(info))),nullptr)) log("VIDEO backend info=%s",info);
  if(result<0 || !src->IsAvailable() || strcmp(src->GetType(),"VIDEO")) return nullptr;
  const double length=src->GetLength(),rate=src->GetSampleRate();
  const int channels=src->GetNumChannels();
  if(!std::isfinite(length) || length<=0 || !std::isfinite(rate) || rate<=0 || channels<=0) {
    log("VIDEO rejected invalid audio metadata length=%.9f rate=%.0f channels=%d file=%s",length,rate,channels,path);
    return nullptr;
  }
  return src.release();
}
static constexpr char raw_audio_type[] = "FRZZ_AUDIO";
class RawAudioDecoder final : public ISimpleMediaDecoder {
  std::string file;
  std::unique_ptr<PCM_source> native;
  std::unique_ptr<ThdAc3Decoder> thd_ac3;
  MediaMetadata metadata;
  INT64 position=0;
public:
  ISimpleMediaDecoder* Duplicate() override {
    try {auto copy=new RawAudioDecoder;copy->file=file;return copy;}catch(...){return nullptr;}
  }
  void Open(const char* filename,int,int,int) override {
    native.reset();thd_ac3.reset();position=0;metadata={};
    try {
      if(filename)file=filename;
      std::string extension=std::filesystem::u8path(file).extension().u8string();
      for(auto& c:extension)if(c>='A' && c<='Z')c+=('a'-'A');
      if(extension==".thd+ac3") {
        // Creating VIDEO once asks REAPER to load its configured FFmpeg build.
        native.reset(create_video(file.c_str()));
        std::string error;thd_ac3=create_thd_ac3_decoder(file.c_str(),error);native.reset();
        if(!thd_ac3){metadata.error=error;log("THD+AC3 handler rejected error=%s file=%s",error.c_str(),file.c_str());return;}
        metadata.codec="truehd";metadata.expected_codec="truehd";metadata.rate=thd_ac3->rate();metadata.channels=thd_ac3->channels();metadata.frames=thd_ac3->frames();metadata.duration=static_cast<double>(metadata.frames)/metadata.rate;metadata.format_major=thd_ac3->abi();
        log("THD+AC3 handler opened duration=%.9f frames=%lld rate=%d channels=%d ABI=%d file=%s",metadata.duration,metadata.frames,metadata.rate,metadata.channels,metadata.format_major,file.c_str());
        return;
      }
      bool scanned=scan_media(file.c_str(),metadata);
      if(!scanned && metadata.error==no_loaded_ffmpeg) {
        // Bootstrap REAPER's own library loader; do not search/load arbitrary DLL paths.
        native.reset(create_video(file.c_str()));
        scanned=native && scan_media(file.c_str(),metadata);
      }
      if(scanned && !native)native.reset(create_video(file.c_str()));
      if(!scanned || !native || native->GetNumChannels()!=metadata.channels || native->GetSampleRate()!=metadata.rate) {
        log("adapter rejected metadata error=%s file=%s",metadata.error.c_str(),file.c_str());native.reset();return;
      }
      log("adapter opened native_type=%s native_duration=%.9f corrected_duration=%.9f file=%s",native->GetType(),native->GetLength(),metadata.duration,file.c_str());
    }catch(const std::exception& e){log("adapter open error=%s",e.what());native.reset();}catch(...){native.reset();}
  }
  void Close(bool) override {native.reset();thd_ac3.reset();position=0;}
  const char* GetFileName() override {return file.c_str();}
  const char* GetType() override {return raw_audio_type;}
  void GetInfoString(char* buf,int size,char* title,int title_size) override {
    if(title && title_size>0)snprintf(title,title_size,"FRZZ Dolby/DTS audio");
    if(buf && size>0)snprintf(buf,size,"%s\r\nDuration: %.9f s\r\nSample rate: %d Hz\r\nChannels: %d\r\nFFmpeg ABI: %d\r\n%s",thd_ac3?"Streaming THD+AC3 container handler":"Raw Dolby/DTS/MLP audio",metadata.duration,metadata.rate,metadata.channels,metadata.format_major,IsOpen()?"":"Media offline");
  }
  bool IsOpen() override {return thd_ac3 || (native && native->IsAvailable());}
  int GetNumChannels() override {return metadata.channels;}
  int GetBitsPerSample() override {return thd_ac3?32:native?native->GetBitsPerSample():0;}
  double GetSampleRate() override {return metadata.rate;}
  INT64 GetLength() override {return metadata.frames;}
  INT64 GetPosition() override {return position;}
  void SetPosition(INT64 value) override {position=std::max<INT64>(0,value);}
  int ReadSamples(ReaSample* buffer,int length) override {
    if((!native && !thd_ac3) || !buffer || length<=0 || position>=metadata.frames)return 0;
    const int count=static_cast<int>(std::min<INT64>(length,metadata.frames-position));
    if(thd_ac3){const int read=thd_ac3->read(position,buffer,count);position+=read;return read;}
    PCM_source_transfer_t block{};block.time_s=static_cast<double>(position)/metadata.rate;block.samplerate=metadata.rate;block.nch=metadata.channels;block.length=count;block.samples=buffer;
    native->GetSamples(&block);
    const int read=std::clamp(block.samples_out,0,count);position+=read;return read;
  }
};
static PCM_source* create_raw_audio(const char* path) {
  // REAPER owns the generated PCM_source, buffering, peak-building and state handling.
  auto src=std::unique_ptr<PCM_source>(PCM_Source_CreateFromSimple(new RawAudioDecoder,path));
  if(!src || !src->IsAvailable())return nullptr;
  return src.release();
}
static PCM_source* create_type(const char* type,int priority) {
  if(priority!=0) return nullptr;
  // Accept the original project source identifier while reporting the audio type.
  if(!strcmp(type,raw_audio_type) || !strcmp(type,"FRZZ_VIDEO")) {
    try{return PCM_Source_CreateFromSimple(new RawAudioDecoder,nullptr);}catch(...){return nullptr;}
  }
  if(strcmp(type,"FRZZ_SUBTITLE")) return nullptr;
  try{return new SubtitleSource;}catch(...){return nullptr;}
}
static PCM_source* create_file(const char* path,int priority) {
  const auto format=identify_format(path);
  if(format==Format::unsupported) return nullptr;
  log("CreateFromFile priority=%d path=%s",priority,path);
  if(priority!=0) return nullptr;
  try {
    if(format==Format::raw_audio) return create_raw_audio(path);
    auto src=std::make_unique<SubtitleSource>();
    if(!src->SetFileName(path)) return nullptr;
    return src.release();
  } catch(const std::exception& e) {log("CreateFromFile error=%s",e.what());} catch(...) {log("CreateFromFile unknown error");}
  return nullptr;
}
static const char* extensions(int i,const char** desc) {
  if(desc) *desc=nullptr;
  if(i<0 || static_cast<size_t>(i)>=extensions_list.size())return nullptr;
  const auto& e=extensions_list[static_cast<size_t>(i)];
  if(desc)*desc=e.format==Format::raw_audio?"FRZZ Dolby/DTS audio":"FRZZ subtitles (ASS/SSA/SRT)";
  return e.name;
}
static pcmsrc_register_t registry{create_type,create_file,extensions};
static int (*reg)(const char*,void*);
static void timer() {
  static bool running=false;
  if(running || current_thread_id()!=main_thread) return;
  struct Running {bool& v; Running(bool& flag):v(flag){v=true;} ~Running(){v=false;}} guard(running);
  try {
    process_pending();
  } catch(const std::exception& e) {log("timer error=%s",e.what());} catch(...) {log("timer unknown error");}
}
}
extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE,reaper_plugin_info_t* rec) {
  using namespace frzz;
  if(!rec) {
    if(reg) {reg("-timer",reinterpret_cast<void*>(timer)); reg("-pcmsrc",&registry);}
    {std::lock_guard<std::mutex> lock(queue_mutex);pending.clear();}
    log("plugin unloaded"); return 0;
  }
  if(rec->caller_version!=REAPER_PLUGIN_VERSION || !rec->GetFunc || !rec->Register || REAPERAPI_LoadAPI(rec->GetFunc)) return 0;
  try {
    main_thread=current_thread_id();
#ifdef FRZZ_ENABLE_LOGGING
    log_file.open(std::filesystem::u8path(GetResourcePath())/"frzz_media_import.log",std::ios::app);
#endif
    log("plugin loaded host=%s plugin=1.1.1 extensions=25 target=REAPER7.66+FFmpeg5+",GetAppVersion());
    reg=rec->Register;
    if(!reg("pcmsrc",&registry)) return 0;
    if(!reg("timer",reinterpret_cast<void*>(timer))) {reg("-pcmsrc",&registry);return 0;}
    reg("ext_name",const_cast<char*>("FRZZ native media import"));
    return 1;
  } catch(...) {return 0;}
}
