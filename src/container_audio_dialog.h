#pragma once
#include <string>
#include <vector>
namespace frzz {
struct ContainerStream;
enum class TrackDialogMode { import_audio, import_video, change };
int choose_container_stream(const std::vector<ContainerStream>& streams,TrackDialogMode mode);
int choose_video_item(const std::vector<std::string>& items);
#ifdef __APPLE__
void* audio_track_mouse_window();
#endif
}
