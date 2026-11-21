#include "container_decoder.h"
#include "mov_track_metadata.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
std::string be(unsigned long long value,int bytes=4) {
  std::string out(static_cast<size_t>(bytes),'\0');
  while(bytes>0) {
    out[static_cast<size_t>(--bytes)]=static_cast<char>(value&0xff);
    value>>=8;
  }
  return out;
}

std::string atom(std::string type,const std::string& payload) {
  return be(payload.size()+8)+type+payload;
}

std::string header(unsigned id,bool version_one=false) {
  if(version_one)return atom("tkhd",be(0x01000000)+be(0,8)+be(0,8)+be(id));
  return atom("tkhd",be(0)+be(0)+be(0)+be(id));
}

std::string locale_name(unsigned language,const std::string& value) {
  return atom("tnam",be(0)+be(language,2)+value);
}

void require(bool condition,const char* message) {
  if(!condition)throw std::runtime_error(message);
}
}

int main() {
  try {
    // Genre and movie-level title atoms must not be used as names for any track.
    const auto legacy=atom("trak",header(44)+atom("udta",atom("name","DUB | Custom")));
    const auto russian=static_cast<unsigned>((18<<10)|(21<<5)|19);
    const auto english=static_cast<unsigned>((5<<10)|(14<<5)|7);
    const auto utf16=std::string("\xfe\xff\x04\x14\x04\x43\x04\x31\x04\x3b\x04\x4f\x04\x36\0\0",16);
    const auto localized=atom("trak",header(109,true)+atom("udta",locale_name(english,"English")+locale_name(russian,utf16)));
    const std::string key="com.apple.quicktime.title";
    const auto keys=atom("keys",be(0)+be(1)+be(key.size()+8)+"mdta"+key);
    const auto title=atom(be(1),atom("data",be(1)+be(0)+"Director commentary"));
    const auto media=atom("mdia",atom("elng",be(0)+std::string("en-GB\0",6)));
    const auto tagged=atom("trak",header(7)+media+atom("meta",be(0)+keys+atom("ilst",title))+
      atom("udta",atom("name","Lower priority")));
    const auto broken=atom("trak",header(9)+atom("udta",be(1000)+"name"+"truncated"));
    const auto global=atom("udta",atom("name","Movie title"));
    const auto media_file=atom("mdat","junk")+atom("moov",global+legacy+localized+tagged+broken);

    const auto path=std::filesystem::temp_directory_path()/
      ("frzz-mov-track-test-"+std::to_string(frzz::monotonic_ms())+"-"+std::to_string(frzz::numeric_thread_id())+".mp4");
    struct RemoveFile {
      std::filesystem::path filename;
      ~RemoveFile(){std::error_code ignored;std::filesystem::remove(filename,ignored);}
    } remove{path};
    {
      std::ofstream file(path,std::ios::binary);
      require(static_cast<bool>(file.write(media_file.data(),static_cast<std::streamsize>(media_file.size()))),"write MOV fixture");
    }

    frzz::ContainerAudio info;
    info.streams.resize(5);
    const unsigned ids[]{7,9,109,99,44};
    for(size_t i=0;i<info.streams.size();++i) {
      info.streams[i].track_id=ids[i];
      info.streams[i].title="Fallback";
    }
    info.streams[2].language="rus";
    frzz::read_mov_track_metadata(path.u8string().c_str(),info);
    require(info.streams[0].title=="Director commentary" && info.streams[0].language=="en-GB","Apple track title and elng");
    require(info.streams[1].title=="Fallback","ignore truncated box");
    require(info.streams[2].title=="Дубляж","localized UTF-16 tnam and version 1 track ID");
    require(info.streams[3].title=="Fallback","ignore movie-level title");
    require(info.streams[4].title=="DUB | Custom","bare track udta name");
  } catch(const std::exception& e) {
    std::cerr<<e.what()<<'\n';
    return 1;
  }
}
