#pragma once
#include "container_decoder.h"
#include <unordered_map>
class PCM_source;
class ReaProject;
namespace frzz {
PCM_source* create_container_source(const char* path);
PCM_source* create_container_type(const char* type,ThreadId main_thread);
bool container_track_options(PCM_source* source,ContainerAudio& audio,int& current,bool audio_import_only=false);
enum class VideoTrackOptions { not_video, single, unavailable, selectable };
using VideoTrackProbes=std::unordered_map<std::string,std::optional<ContainerAudio>>;
VideoTrackOptions video_track_options(PCM_source* source,ContainerAudio& audio,int& current,
                                      VideoTrackProbes& probes,bool switching_only);
const char* container_track_file(PCM_source* source);
PCM_source* create_selected_container_source(PCM_source* previous,ContainerAudio audio,int stream);
void queue_automatic_item_source_peaks(PCM_source* source,bool project_load=false);
bool native_peak_request_active();
void set_native_peak_dispatch_command(int command);
void dispatch_native_peak_requests();
bool has_video_container_audio(PCM_source* source);
PCM_source* container_peak_source_override(PCM_source* source,bool& physical_key);
void run_container_native_peak_command(int command,int flag,ReaProject* project,
                                       const std::vector<PCM_source*>& sources);
void queue_item_source_peaks(PCM_source* source,bool rebuild);
void poll_container_peaks();
void cancel_container_peaks();
void register_container_track_menu(int (*register_api)(const char*,void*));
void unregister_container_track_menu();
void poll_container_video();
}
