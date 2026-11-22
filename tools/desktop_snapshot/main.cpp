// Offscreen snapshot of the Windows / Linux editor (DesktopEditor) with a
// mock host, to PNG: the C++ editors' twin of tools/ui_snapshot (SwiftUI).
// Linux + cairo only (the GDI+ shim); needs no X server. Build + run:
//   tools/desktop_snapshot/build.sh && \
//   tools/desktop_snapshot/run/desktop_snapshot <edit|play|browser|write> out.png [width] [theme]
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <cwchar>
#define private public
#define protected public
#include "DesktopEditor/DesktopEditor.hpp"
#undef private
#undef protected
#include "EngineCore/InitPatch.hpp"
#include "EngineCore/ParamFormat.hpp"
#include "EngineCore/Params.hpp"

using namespace oxo;
namespace {
std::array<int32_t, 100> gValues{};
std::string gScratch, gName = "The Ultimate Strings", gJson;
int gBank = 1, gSlot = 109;
uint64_t gVoices = 0;

const char* keep(std::string s) { gScratch = std::move(s); return gScratch.c_str(); }

// A neutral mock library: factory banks + user banks, string-ish names in the loaded one.
std::string makeLibrary() {
    static const char* factory[] = {"Core Analog", "Classic Leads", "Bold Basses", "Pads & Atmos", "Keys & Plucks", "FX & Motion"};
    static const char* user[] = {"Madame Eljay", "My Patches", "Live Set"};
    static const char* strings[] = {"Classic Strings", "Lush Chamber", "Analog Ensemble", "Silver Strings", "Warm Section",
        "Studio Strings", "The Ultimate Strings", "Long Arco", "Pizzicato Ensemble", "String Pad I", "String Pad II",
        "Orchestral Bed", "Movie Strings", "Hybrid Strings", "Slow String Sweep", "String Harmonics", "String Boutique"};
    auto bank = [&](const std::string& name, bool ro, int count, bool strs) {
        std::string j = "{\"name\":\"" + name + "\",\"source\":\"/lib/" + name + "\",\"readOnly\":" + (ro ? "true" : "false") + ",\"slots\":[";
        for (int i = 0; i < 128; i++) {
            std::string n;
            if (strs && i >= 103 && i < 103 + 17) n = strings[i - 103];
            else if (i < count) n = std::string(name.substr(0, 4)) + " Patch " + std::to_string(i + 1);
            j += (i ? "," : "") + std::string("\"") + n + "\"";
        }
        return j + "]}";
    };
    std::string j = "{\"root\":\"/lib\",\"banks\":[";
    j += bank("Synth2 Factory", true, 128, false);
    for (const char* f : factory) j += "," + bank(f, true, 60, false);
    j += "," + bank(user[0], false, 128, true);
    for (int i = 1; i < 3; i++) j += "," + bank(user[i], false, 30 + i, false);
    return j + "]}";
}
OxoSynth2Hooks mock() {
    OxoSynth2Hooks h{};
    h.ctx = nullptr;
    h.getValue = [](void*, int32_t id) { return id >= 0 && id < 100 ? gValues[size_t(id)] : 0; };
    h.beginEdit = [](void*, int32_t) {};
    h.endEdit = [](void*, int32_t) {};
    h.setValue = [](void*, int32_t id, int32_t v) { if (id >= 0 && id < 100) gValues[size_t(id)] = v; };
    h.paramInfo = [](void*, int32_t id, int32_t* mn, int32_t* mx, int32_t* df) {
        const auto& p = s2::Info(id);
        *mn = p.min; *mx = p.max; *df = p.def;
    };
    h.paramName = [](void*, int32_t id) { return s2::Info(id).name; };
    h.displayName = [](void*, int32_t id) { return s2::DisplayName(id, gValues[s2::kEffectType]); };
    h.formatValue = [](void*, int32_t id, int32_t v) {
        const int sync = s2::TempoSyncSwitch(id);
        return keep(s2::FormatParam(id, v, sync >= 0 && gValues[size_t(sync)], gValues[s2::kEffectType]));
    };
    h.patchName = [](void*) { return gName.c_str(); };
    h.setPatchName = [](void*, const char*) {};
    h.patchSerial = [](void*) { return int32_t(1); };
    h.libraryJSON = [](void*) { gJson = makeLibrary(); return gJson.c_str(); };
    h.libraryRevision = [](void*) { return int32_t(1); };
    h.libraryNotice = [](void*) { return ""; };
    h.libraryRoot = [](void*) { return "/lib"; };
    h.loadLibraryPatch = [](void*, int32_t b, int32_t s) { gBank = b; gSlot = s; return int32_t(1); };
    h.slotPath = [](void*, int32_t, int32_t) { return ""; };
    h.voiceStates = [](void*) { return gVoices; };
    h.loadedProgram = [](void*, int32_t* b, int32_t* s) { *b = gBank; *s = gSlot; };
    h.themeText = [](void*) { return ""; };
    h.setThemeText = [](void*, const char*) {};
    h.invertWheel = [](void*) { return int32_t(0); };
    return h;
}
// Values close to a pad / strings patch.
void pad() {
    for (int i = 0; i < s2::kParamCount; i++) gValues[size_t(i)] = s2::kInitPatchValues[size_t(i)];
    gValues[s2::kVolumeParamId] = s2::Info(s2::kVolumeParamId).def;
    const std::map<int, int> v = {{0, 3}, {1, 1}, {2, 64}, {3, 70}, {5, 64}, {8, 40}, {9, 0}, {76, 50}, {45, 30}, {95, 30}, {97, 1},
        {96, 0}, {71, 0}, {10, 1}, {11, 40}, {12, 30}, {13, 50}, {72, 64}, {91, 40}, {19, 82}, {20, 40}, {14, 1}, {15, 10}, {16, 70},
        {17, 100}, {18, 70}, {21, 80}, {22, 64}, {23, 30}, {24, 1}, {25, 20}, {26, 60}, {27, 90}, {28, 70}, {29, 100}, {30, 64},
        {41, 2}, {46, 3}, {42, 3}, {47, 3}, {43, 40}, {44, 40}, {48, 60}, {49, 30}, {67, 1}, {59, 1}, {31, 1}, {32, 0}, {33, 60}, {34, 90},
        {38, 0}, {94, 32}, {73, 0}, {74, 1}, {93, 2}, {77, 1}, {78, 1}, {65, 1}, {66, 1}, {82, 1}, {64, 1}, {40, 2}, {86, 0xB001}, {88, 0xB001},
        {87, 56}, {89, 48}, {6, 0}, {7, 0}, {4, 1}, {99, 100}};
    for (const auto& [k, x] : v) gValues[size_t(k)] = x;
    gVoices = 0b01'10'10'10'01ull << 6; // a few voices
}
} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "edit";
    const std::string out = argc > 2 ? argv[2] : "snapshot.png";
    const int width = argc > 3 ? std::atoi(argv[3]) : 1754;
    pad();
    DesktopEditor ed(nullptr);
    ed.hooks_ = mock();
    const int height = int(std::lround(width * DesktopEditor::kDesignHeight / DesktopEditor::kDesignWidth));
    ed.rect = Steinberg::ViewRect(0, 0, width, height);
    ed.knownRevision_ = 1;
    for (size_t i = 0; i < ed.knownValues_.size(); ++i) ed.knownValues_[i] = ed.value(int(i));
    ed.refreshBanks();
    ed.snapshotMode(mode);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    cairo_t* cr = cairo_create(surface);
    {
        Gdiplus::Graphics g(cr);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.ScaleTransform(float(width) / DesktopEditor::kDesignWidth, float(height) / DesktopEditor::kDesignHeight);
        ed.paintClip_ = Gdiplus::RectF(0, 0, DesktopEditor::kDesignWidth, DesktopEditor::kDesignHeight);
        ed.drawStatic(g);
        ed.drawDynamic(g);
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    const auto status = cairo_surface_write_to_png(surface, out.c_str());
    cairo_surface_destroy(surface);
    std::printf("%s %dx%d %s\n", out.c_str(), width, height, status == CAIRO_STATUS_SUCCESS ? "ok" : "FAILED");
    return status == CAIRO_STATUS_SUCCESS ? 0 : 1;
}
