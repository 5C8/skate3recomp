// Custom songs (SkateToolkit > Music, game patch target).
//
// WHY. Skate 3's iPod song list is compiled into the executable: MusicSystem::Initialize (retail
// sub_82499938, sk83_na_m rex_Sk8_Audio_MusicSystem_Initialize 0x825E5A08) registers two
// MusicCore::SongList entries in the MusicCore it is given (r3): "IPOD" (47 = "IPOD_INVALID" + the 46
// licensed songs) and "DJINTERACTIVE" (26). Entries live at core + 32 + 28 * i (char name[16], u16
// count at +16, u32 ids at +20, const char** strs at +24); their number at core + 480.
//   * MusicCore::FindSongIndex (0x82D14B00) looks an IPodSongInfo hash up in ids[] - a hash not there
//     plays nothing; the index found is passed as the MPF named value "ipod" and event 0 branches to
//     the index-th section-1 start node of ipod.mpf (sample 0 nodes: song i at its i-th start node).
//   * the music player lists ids[1..count-1] (GetNumMusicPlayerSongs / GetMusicPlayerSongIDFromIndex);
//     MusicCore::GetSongInfo builds "IPOD_" + strs[i] + "_ARTIST" / "_TITLE" / "_ALBUM" labels.
//   * id = StringHash(upper("IPOD_" + str)): h = 0xFFFFFFFF, h = h * 33 + c.
//
// WHAT THIS DOES. After the original runs, the "IPOD" list is pointed at new guest arrays built from
// <game>/data/audio/music/custom_songs.txt (song strings as the toolkit wrote them: "Artist_Title"):
//   * a plain file: the game's own entries + one per line (appended);
//   * a file whose first line is "!list": the whole list, in order, after IPOD_INVALID - retail songs the
//     toolkit removed are left out (their ipod.mpf start nodes moved out of the counted sections, so
//     the i-th entry still plays the i-th start node), retail songs kept reuse the game's own entries
//     (listed as "=XXXXXXXX", their id: one retail song has no strings to name it by). The toolkit writes the matching ipod.mpf / Ipod_Stream.mus (a 32-node
// block per song in section 1, a replay start in section 2), the strings and the playlist rows.
// No file = nothing changes. Limits: the profile's enabled-song list holds 100 (profile + 248, count
// at + 648: filled from this list on a profile reset) - the list is capped at 99 songs.

#include "generated/skate3_init.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace skate3 {
const std::filesystem::path& GameDataRoot();   // skate3_app_common.cpp
}

namespace {

constexpr uint32_t kListsAt = 32, kListSize = 28, kListCount = 480, kMaxLists = 16;
constexpr uint32_t kMaxSongs = 100;              // the profile's enabled list (incl. IPOD_INVALID)

uint32_t SongHash(const std::string& s) {
  uint32_t h = 0xFFFFFFFFu;
  for (unsigned char c : s) h = h * 33u + uint32_t(std::toupper(c));
  return h;
}

std::vector<std::string> ReadManifest(bool* full_list) {
  std::vector<std::string> out;
  *full_list = false;
  const auto path = skate3::GameDataRoot() / "data" / "audio" / "music" / "custom_songs.txt";
  std::ifstream f(path);
  if (!f) return out;
  std::string line;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
    size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos || line[b] == '#') continue;
    line = line.substr(b);
    if (out.empty() && !*full_list && line == "!list") {
      *full_list = true;
      continue;
    }
    if (line.size() > 120) continue;
    out.push_back(line);
  }
  return out;
}

void ExtendIpod(uint8_t* base, uint32_t core) {
  bool full_list = false;
  const auto songs = ReadManifest(&full_list);
  if (songs.empty() && !full_list) return;
  const uint32_t lists = REX_LOAD_U32(core + kListCount);
  uint32_t entry = 0;
  for (uint32_t i = 0; i < lists && i < kMaxLists; ++i) {
    const uint32_t e = core + kListsAt + kListSize * i;
    char name[17] = {};
    for (int c = 0; c < 16; ++c) name[c] = char(REX_LOAD_U8(e + c));
    if (std::strcmp(name, "IPOD") == 0) {
      entry = e;
      break;
    }
  }
  if (!entry) {
    REXLOG_WARN("custom-songs: no IPOD song list in MusicCore {:08X} - {} song(s) not added", core, songs.size());
    return;
  }
  const uint32_t count = REX_LOAD_U16(entry + 16);
  const uint32_t ids = REX_LOAD_U32(entry + 20), strs = REX_LOAD_U32(entry + 24);
  std::vector<uint32_t> old_ids(count), old_strs(count);
  for (uint32_t i = 0; i < count; ++i) {
    old_ids[i] = REX_LOAD_U32(ids + 4 * i);
    old_strs[i] = REX_LOAD_U32(strs + 4 * i);
  }
  // the list kept from the game's own (all of it when appending; IPOD_INVALID only with "!list")
  std::vector<std::pair<uint32_t, uint32_t>> keep;   // (id, guest string)
  for (uint32_t i = 0; i < (full_list ? std::min<uint32_t>(count, 1) : count); ++i) keep.emplace_back(old_ids[i], old_strs[i]);
  std::vector<std::pair<uint32_t, std::string>> add;
  for (const auto& s : songs) {
    // "=XXXXXXXX": a game song by its id (the toolkit lists kept retail songs this way)
    const bool by_id = s.size() == 9 && s[0] == '=';
    const uint32_t id = by_id ? uint32_t(std::strtoul(s.c_str() + 1, nullptr, 16)) : SongHash("IPOD_" + s);
    bool dup = false;
    for (const auto& k : keep) dup = dup || k.first == id;
    for (const auto& a : add) dup = dup || a.first == id;
    if (dup) continue;
    if (keep.size() + add.size() >= kMaxSongs) {
      REXLOG_WARN("custom-songs: {} - the game holds {} songs at most, the rest not added", s, kMaxSongs - 1);
      break;
    }
    bool retail = false;
    for (uint32_t i = 1; i < count && !retail; ++i)
      if (old_ids[i] == id) {
        keep.emplace_back(id, old_strs[i]);           // a game song, in the order the file lists it
        retail = true;
      }
    if (!retail && !by_id) add.emplace_back(id, s);
  }
  if (!full_list && add.empty()) return;
  const uint32_t kept = uint32_t(keep.size());
  const uint32_t n = kept + uint32_t(add.size());
  auto* mem = rex::system::kernel_memory();
  const uint32_t new_ids = mem->SystemHeapAlloc(4 * n, 16);
  const uint32_t new_strs = mem->SystemHeapAlloc(4 * n, 16);
  uint32_t blob_size = 0;
  for (const auto& a : add) blob_size += uint32_t(a.second.size()) + 1;
  const uint32_t blob = mem->SystemHeapAlloc((blob_size + 16) & ~15u, 16);
  if (!new_ids || !new_strs || !blob) {
    REXLOG_WARN("custom-songs: guest allocation failed - songs not added");
    return;
  }
  for (uint32_t i = 0; i < kept; ++i) {
    REX_STORE_U32(new_ids + 4 * i, keep[i].first);
    REX_STORE_U32(new_strs + 4 * i, keep[i].second);
  }
  uint32_t at = blob;
  for (size_t k = 0; k < add.size(); ++k) {
    const auto& [id, s] = add[k];
    for (size_t c = 0; c < s.size(); ++c) REX_STORE_U8(at + uint32_t(c), uint8_t(s[c]));
    REX_STORE_U8(at + uint32_t(s.size()), 0);
    REX_STORE_U32(new_ids + 4 * (kept + uint32_t(k)), id);
    REX_STORE_U32(new_strs + 4 * (kept + uint32_t(k)), at);
    at += uint32_t(s.size()) + 1;
  }
  REX_STORE_U32(entry + 20, new_ids);
  REX_STORE_U32(entry + 24, new_strs);
  REX_STORE_U16(entry + 16, uint16_t(n));
  REXLOG_INFO("custom-songs: IPOD list {} -> {} songs ({} of the game's kept, {} new from custom_songs.txt{})",
              count - 1, n - 1, kept - 1, add.size(), full_list ? ", whole list" : "");
}

}  // namespace

// MusicSystem::Initialize (r3 = the MusicCore): the song lists registered, then extended.
extern "C" REX_FUNC(sub_82499938) {
  const uint32_t core = ctx.r3.u32;
  __imp__sub_82499938(ctx, base);
  if ((ctx.r3.u32 & 0xFF) != 0) ExtendIpod(base, core);
}
