// The panel of the Windows and Linux editors (DesktopEditor.hpp): written
// against Win32 + GDI+; on Linux, LinuxPlatform.hpp / LinuxGdiplus.hpp
// provide the names it uses. Each platform's window: WindowsEditor.cpp /
// LinuxEditor.cpp.
#include "DesktopEditor/DesktopEditor.hpp"
#include "OxoSynth2VST3/source/OxoSynth2Controller.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/base/keycodes.h"
#include "OxoSynth2Identity.h"
#include "EditorCore/Storage.hpp"
#include "EditorCore/Trash.hpp"
#include "EditorCore/UpdateCheck.hpp"
#include "Windows_Build/Source/WindowsZoomSettings.hpp"
#if defined(_WIN32)
#include <shellapi.h>
#endif
#include <cstring>

#include <algorithm>
#include <string>
#include <numeric>
#include <unordered_map>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <sstream>
#include <string_view>

using namespace Steinberg;
using namespace Gdiplus;

namespace oxo {
namespace {
constexpr uint64_t kWheelRunMs = 300; // wheel events closer than this = one run (wheel())
constexpr float kPi = 3.14159265358979323846f;
constexpr int kPanel=themes::kPanel, kHeader=themes::kHeader, kHeaderText=themes::kHeaderText,
              kLabel=themes::kLabel, kKnob=themes::kKnobBody, kKnobHighlight=themes::kKnobGradient,
              kDial=themes::kKnobDial, kButton=themes::kButton, kLed=themes::kLed, kRadio=themes::kRadio,
              kDisplayText=themes::kDisplayText, kDisplayTop=themes::kDisplayTop,
              kDisplayBottom=themes::kDisplayBottom;
constexpr int kKnobBody=themes::kKnobBody, kKnobGradient=themes::kKnobGradient, kKnobDial=themes::kKnobDial;
constexpr int kThemeCommand=1000; // theme menu items: kThemeCommand + index into the picks
constexpr const wchar_t* kProjectURL=L"https://github.com/chab/oxo_synth2"; // SynthPanelView.projectURL
// What the desktop calls these.
#if defined(_WIN32)
constexpr const wchar_t* kShowInFiles=L"Show in Explorer";
constexpr const wchar_t* kTrash=L"Recycle Bin";
#else
constexpr const wchar_t* kShowInFiles=L"Show in file manager";
constexpr const wchar_t* kTrash=L"Trash";
#endif
// Theme.lcdText (Shared/SynthControls.swift): fixed browser / LCD text.
constexpr int kLcdText=0xE0FFFF;
// The panel's own buttons (hoveredChrome_ / chromeRect): the tab row, the
// header, the browser's bottom bar.
enum Chrome {cNone=0,cTabEdit,cTabPlay,cTabBrowser,cPrev,cNext,cSearch,cWrite,cMenu,cLearnExit,
             cBrWrite,cBrNewBank,cBrFolder,cBrRefresh,cBrBankPrev,cBrBankNext,cBrImport,cBrExport};
// Regions redrawn on their own (design coordinates).
const RectF kLcdRect(214,4,302,21), kBankLineRect(214,30,332,21), kMeterRect(784,4,86,19),
            kBrowserRect(0,53,877,396), kGraphRect(481,79,189,79),
            kBankPanel(6,60,180,344), kPresetPanel(195,60,385,344), kPreviewPanel(588,60,283,344),
            kBottomBar(6,411,865,32), kSearchBox(202,84,372,21);
constexpr float kBankTop=92.f, kBankRowH=17.f, kPresetTop=128.f, kPresetRowH=15.5f;
constexpr int kBankRows=18, kPresetRows=17;

Color rgb(int rgbValue) { return Color(255, (rgbValue>>16)&255, (rgbValue>>8)&255, rgbValue&255); }
Color muted(const Color& c, float factor) {
    return Color(255, BYTE(c.GetR()*factor), BYTE(c.GetG()*factor), BYTE(c.GetB()*factor));
}
void fill(Graphics& g, const Color& c, float x,float y,float w,float h) {
    SolidBrush b(c); g.FillRectangle(&b, x,y,w,h);
}
void stroke(Graphics& g, const Color& c, float x,float y,float w,float h,float width=1) {
    Pen p(c,width); g.DrawRectangle(&p,x,y,w,h);
}
// Bank list order (UI only: hook indices, slot numbers and host state are
// untouched): digits compare as numbers ("Bank 2" < "Bank 10"), letters
// case-insensitively.
bool NaturalLess(const std::wstring& a, const std::wstring& b) {
    size_t i=0,j=0;
    while(i<a.size() && j<b.size()) {
        if(a[i]>=L'0' && a[i]<=L'9' && b[j]>=L'0' && b[j]<=L'9') {
            const size_t ai=i,bj=j;
            while(i<a.size() && a[i]>=L'0' && a[i]<=L'9') ++i;
            while(j<b.size() && b[j]>=L'0' && b[j]<=L'9') ++j;
            size_t nzA=ai,nzB=bj;
            while(nzA<i && a[nzA]==L'0') ++nzA;
            while(nzB<j && b[nzB]==L'0') ++nzB;
            const size_t la=i-nzA,lb=j-nzB;
            if(la!=lb) return la<lb;
            for(size_t k=0;k<la;k++) if(a[nzA+k]!=b[nzB+k]) return a[nzA+k]<b[nzB+k];
            // Stable equal-value tie break: fewer leading zeroes first.
            if(i-ai!=j-bj) return i-ai<j-bj;
        } else {
            const wchar_t ac=wchar_t(std::towlower(a[i])),bc=wchar_t(std::towlower(b[j]));
            if(ac!=bc) return ac<bc;
            ++i;++j;
        }
    }
    return i==a.size() && j!=b.size();
}
} // namespace

// Text is drawn as antialiased outlines in design-space coordinates: unlike
// grid-fitted glyph bitmaps, the letter positions do not snap to different
// pixels while the VST3 view is resized, and the ink bounds centre short LCD
// values. Fixed-size, natural-proportion text: long labels are ellipsized
// instead of shrinking the font; a shared ascender / descender reference
// keeps every baseline stable. Building an outline (AddString) is the
// expensive part, so each one is built ONCE per (text, size, box width,
// font) and then only filled.
class TextCache {
public:
    struct Shape {
        std::unique_ptr<GraphicsPath> path; // at the origin; null = nothing fits
        RectF ink;                          // its ink bounds
        float refY = 0, refH = 0;           // the line's reference ink ("Agjpqy")
        bool cut = false;                   // ellipsized to fit
    };
    TextCache() : segoe_(L"Segoe UI"), consolas_(L"Consolas") {
        format_.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
    }
    const Shape& get(const std::wstring& s, float fontSize, float maxInkWidth, bool mono) {
        std::wstring key=s;
        key+=L'\x1f';key+=std::to_wstring(std::lround(fontSize*100));
        key+=L'\x1f';key+=std::to_wstring(std::lround(maxInkWidth*100));
        key+=mono?L'm':L's';
        if(const auto it=shapes_.find(key); it!=shapes_.end()) return it->second;
        if(shapes_.size()>=kMaxShapes) shapes_.clear(); // LCD readouts / names come and go
        return shapes_.emplace(std::move(key),build(s,fontSize,maxInkWidth,mono)).first->second;
    }

private:
    static constexpr size_t kMaxShapes=4096;
    bool make(const std::wstring& s, float size, bool mono, GraphicsPath& path, RectF& bounds) const {
        return path.AddString(s.c_str(),-1,mono?&consolas_:&segoe_,FontStyleBold,size,PointF(0.f,0.f),&format_)==Ok &&
               path.GetBounds(&bounds)==Ok;
    }
    Shape build(const std::wstring& s, float size, float maxInkWidth, bool mono) const {
        Shape shape;
        auto path=std::make_unique<GraphicsPath>();
        if(!make(s,size,mono,*path,shape.ink)) return shape;
        if(shape.ink.Width>maxInkWidth) {
            // Reserve the full-size ellipsis and find the longest fitting prefix.
            const std::wstring ellipsis(1,wchar_t(0x2026));
            GraphicsPath dots;
            RectF dotBounds{};
            if(!make(ellipsis,size,mono,dots,dotBounds) || dotBounds.Width>maxInkWidth) return shape;
            size_t low=0,high=s.size();
            while(low<high) {
                const size_t mid=low+(high-low+1)/2;
                GraphicsPath probe;
                RectF probeBounds{};
                if(!make(s.substr(0,mid)+ellipsis,size,mono,probe,probeBounds)) return shape;
                if(probeBounds.Width<=maxInkWidth) low=mid; else high=mid-1;
            }
            std::wstring shortened=s.substr(0,low);
            // Windows wchar_t is UTF-16; don't cut a surrogate pair in half.
            if(!shortened.empty() && shortened.back()>=wchar_t(0xD800) && shortened.back()<=wchar_t(0xDBFF))
                shortened.pop_back();
            path->Reset();
            if(!make(shortened+ellipsis,size,mono,*path,shape.ink)) return shape;
            shape.cut=true;
        }
        GraphicsPath reference;
        RectF metrics{};
        if(!make(L"Agjpqy",size,mono,reference,metrics)) return shape;
        shape.refY=metrics.Y;
        shape.refH=metrics.Height;
        shape.path=std::move(path);
        return shape;
    }
    FontFamily segoe_, consolas_;
    StringFormat format_;
    std::unordered_map<std::wstring, Shape> shapes_;
};

namespace {
// Font size is chosen by the control, NEVER reduced to fit the string.
float textFontSize(float size) { return size; }
// The browser's bank names (drawBrowser, the tooltip's cut test).
constexpr float kBankNameW=116.f, kBankNameSize=10.5f;
constexpr float kTipTextSize=10.f, kTipPad=6.f, kTipH=17.f, kTipMargin=4.f;
Color blend(const Color& background, const Color& foreground, float amount) {
    amount=std::clamp(amount,0.f,1.f);
    auto part=[&](BYTE back, BYTE front) {
        return BYTE(std::lround(back*(1.f-amount)+front*amount));
    };
    return Color(255,part(background.GetR(),foreground.GetR()),
                 part(background.GetG(),foreground.GetG()),
                 part(background.GetB(),foreground.GetB()));
}
void roundedPath(GraphicsPath& path,float x,float y,float w,float h,float radius) {
    const float d=std::min(2.f*radius,std::min(w,h));
    path.AddArc(x,y,d,d,180,90);
    path.AddArc(x+w-d,y,d,d,270,90);
    path.AddArc(x+w-d,y+h-d,d,d,0,90);
    path.AddArc(x,y+h-d,d,d,90,90);
    path.CloseFigure();
}
void roundedFill(Graphics& g, const Color& color,float x,float y,float w,float h,float radius) {
    GraphicsPath path;roundedPath(path,x,y,w,h,radius);
    SolidBrush brush(color);g.FillPath(&brush,&path);
}
void roundedOutline(Graphics& g, const Color& color,float x,float y,float w,float h,float radius,float width=1.f) {
    GraphicsPath path;roundedPath(path,x,y,w,h,radius);
    Pen pen(color,width);g.DrawPath(&pen,&path);
}
Color alpha(const Color& c, int a) { return Color(BYTE(std::clamp(a,0,255)),c.GetR(),c.GetG(),c.GetB()); }
// The selector / switch LED: a lit one glows, an unlit one is a dull bead.
void led(Graphics& g, const Color& c, float x,float y,float radius=3, bool glowing=false) {
    if(glowing) {
        // Translucent SolidBrush halos (no PathGradientBrush: suspected of
        // hanging some VST3 host paints).
        for(int i=3;i>=1;--i) {
            const float halo=radius+1.5f*float(i);
            SolidBrush b(Color(BYTE(16+10*(4-i)),c.GetR(),c.GetG(),c.GetB()));
            g.FillEllipse(&b,x-halo,y-halo,2.f*halo,2.f*halo);
        }
    }
    SolidBrush b(c); g.FillEllipse(&b,x-radius,y-radius,2*radius,2*radius);
    const float spot=radius*.55f;
    SolidBrush shine(Color(BYTE(glowing?150:55),255,255,255));
    g.FillEllipse(&shine,x-radius*.55f,y-radius*.7f,spot,spot);
    Pen edge(Color(150,0,0,0),.6f);
    g.DrawEllipse(&edge,x-radius,y-radius,2*radius,2*radius);
}
// Set at the start of every draw: the light-strip ("Modern") themes also get flat
// pill buttons and a value arc round each knob.
bool gFlatStyle=false;
void darkButton(Graphics& g, Color face, float x,float y,float w,float h, bool hovered=false) {
    // Hover is visual only; the size and text layout never change.
    if (hovered) face=blend(face,rgb(0xFFFFFF),.09f);
    if (gFlatStyle) {
        GraphicsPath pill;roundedPath(pill,x,y,w,h,std::min(h*.5f,9.f));
        SolidBrush flat(face);
        g.FillPath(&flat,&pill);
        Pen rim(Color(150,255,255,255),.8f);
        g.DrawPath(&rim,&pill);
        return;
    }
    GraphicsPath path;roundedPath(path,x,y,w,h,3.f);
    LinearGradientBrush grad(PointF(x,y),PointF(x,y+h),blend(face,rgb(0xFFFFFF),.13f),blend(face,rgb(0x000000),.18f));
    g.FillPath(&grad,&path);
    Pen edge(Color(220,3,4,5),1.f);
    g.DrawPath(&edge,&path);
    Pen light(Color(38,255,255,255),.8f); // the top edge catches the light
    g.DrawLine(&light,x+2.5f,y+1.f,x+w-2.5f,y+1.f);
}
using Palette13=std::array<Color,themes::kSlotCount>;
void displayTile(Graphics& g,const Palette13& p,float x,float y,float w,float h,bool lit=false) {
    GraphicsPath path;roundedPath(path,x,y,w,h,2.5f);
    LinearGradientBrush face(PointF(x,y),PointF(x,y+h),p[kDisplayTop],p[kDisplayBottom]);
    g.FillPath(&face,&path);
    Pen outer(Color(235,4,6,7),1.2f);
    g.DrawPath(&outer,&path);
    GraphicsPath inner;roundedPath(inner,x+1.f,y+1.f,w-2.f,h-2.f,2.f);
    Pen rim(alpha(p[kDisplayText],lit?150:46),.7f);
    g.DrawPath(&rim,&inner);
}
// Relative brightness 0..1.
float luma(const Color& c) { return (.2126f*c.GetR()+.7152f*c.GetG()+.0722f*c.GetB())/255.f; }
// A theme whose header colour is very light gets light header strips with dark titles
// over deeper, recessed section wells (the "Modern" look); every other theme keeps
// the dark card with an accent title.
// Themes with a light knob colour get silver caps on a dark base ring.
bool silverKnobs(const Palette13& p) { return luma(p[kKnobGradient])>.65f; }
// The lower row of a light-strip theme is brushed silver: dark ink on light panels.
Palette13 lowerPalette(const Palette13& p) {
    Palette13 q=p;
    q[kLabel]=rgb(0x1C1C1E);
    q[kPanel]=blend(p[kHeader],rgb(0x000000),.10f);
    return q;
}
constexpr float kLowerRowTop=282.f;
bool lightStrips(const Palette13& p) { return luma(p[kHeader])>.75f && luma(p[kHeader])>luma(p[kPanel])+.35f; }
// A section / panel plate: a dark card with a header band and an accent title.
void sectionPlate(Graphics& g,const Palette13& p,float x,float y,float w,float h,float band=19.f,bool lower=false) {
    GraphicsPath path;roundedPath(path,x,y,w,h,3.5f);
    const bool strip=lightStrips(p);
    if(strip && lower) { // brushed silver card
        LinearGradientBrush silver(PointF(x,y),PointF(x,y+h),blend(p[kHeader],rgb(0xFFFFFF),.25f),blend(p[kHeader],rgb(0x000000),.14f));
        g.FillPath(&silver,&path);
        const GraphicsState keep=g.Save();
        g.SetClip(RectF(x+1.f,y+1.f,w-2.f,h-2.f),CombineModeReplace);
        Pen grain(Color(14,0,0,0),.6f);
        for(float gy=y+band+1.f;gy<y+h;gy+=2.f) g.DrawLine(&grain,x,gy,x+w,gy);
        g.Restore(keep);
        LinearGradientBrush bar(PointF(x,y),PointF(x,y+band),blend(p[kHeader],rgb(0xFFFFFF),.45f),blend(p[kHeader],rgb(0x000000),.06f));
        const GraphicsState keep2=g.Save();
        g.SetClip(RectF(x,y,w,band),CombineModeReplace);
        g.FillPath(&bar,&path);
        g.Restore(keep2);
        Pen edge(Color(150,70,72,76),1.f);
        g.DrawPath(&edge,&path);
        Pen under(Color(120,60,62,66),1.f);
        g.DrawLine(&under,x+1.f,y+band,x+w-1.f,y+band);
        return;
    }
    LinearGradientBrush body(PointF(x,y),PointF(x,y+h),
        strip ? blend(p[kPanel],rgb(0x000000),.30f) : blend(p[kPanel],rgb(0xFFFFFF),.035f),
        strip ? blend(p[kPanel],rgb(0x000000),.46f) : blend(p[kPanel],rgb(0x000000),.10f));
    g.FillPath(&body,&path);
    const GraphicsState saved=g.Save();
    g.SetClip(RectF(x,y,w,band),CombineModeReplace);
    LinearGradientBrush head(PointF(x,y),PointF(x,y+band),
        strip ? blend(p[kHeader],rgb(0xFFFFFF),.35f) : blend(p[kPanel],rgb(0x000000),.18f),
        strip ? blend(p[kHeader],rgb(0x000000),.12f) : blend(p[kPanel],rgb(0x000000),.34f));
    g.FillPath(&head,&path);
    g.Restore(saved);
    Pen line(Color(190,6,8,9),1.f);
    g.DrawLine(&line,x+1.f,y+band,x+w-1.f,y+band);
    Pen edge(blend(p[kPanel],p[kLabel],.17f),1.f);
    g.DrawPath(&edge,&path);
    Pen outer(Color(200,5,6,7),1.f);
    GraphicsPath out;roundedPath(out,x-.6f,y-.6f,w+1.2f,h+1.2f,4.f);
    g.DrawPath(&outer,&out);
}
void rule(Graphics& g,const Palette13& p,float x0,float y0,float x1,float y1) {
    Pen dark(Color(200,4,5,6),1.f), light(alpha(p[kLabel],34),1.f);
    g.DrawLine(&dark,x0,y0,x1,y1);
    if(y0==y1) g.DrawLine(&light,x0,y0+1.f,x1,y1+1.f); else g.DrawLine(&light,x0+1.f,y0,x1+1.f,y1);
}
// The knob: scale ticks, a brushed metal body; the pointer is drawn apart
// (it changes with the value, the body does not).
void paintKnobBody(Graphics& g,const Palette13& p,float x,float y,float d) {
    const float r=d*.5f;
    Pen ticks(alpha(p[kLabel],170),std::max(.8f,d*.045f));
    for(int i=0;i<11;i++) {
        const float a=(-140+i*28-90)*kPi/180, c=std::cos(a), s=std::sin(a);
        const float r0=r+d*.10f+.8f, r1=r0+std::max(2.2f,d*.17f)*(i%5==0 ? 1.15f : .85f);
        g.DrawLine(&ticks,x+c*r0,y+s*r0,x+c*r1,y+s*r1);
    }
    if(silverKnobs(p)) { // dark base ring, brushed-metal cap
        SolidBrush ring(blend(p[kKnobBody],rgb(0x000000),.35f));
        g.FillEllipse(&ring,x-r,y-r,d,d);
        const float cr=r*.80f;
        GraphicsPath cap;cap.AddEllipse(x-cr,y-cr,2*cr,2*cr);
        LinearGradientBrush metal(PointF(x,y-cr),PointF(x,y+cr),blend(p[kKnobGradient],rgb(0xFFFFFF),.40f),blend(p[kKnobGradient],rgb(0x000000),.38f));
        g.FillPath(&metal,&cap);
        for(const float k:{.34f,.54f,.74f}) {
            Pen brushed(Color(k<.5f?46:34,k<.5f?0:255,k<.5f?0:255,k<.5f?0:255),.6f);
            g.DrawEllipse(&brushed,x-cr*k-.0f,y-cr*k,2*cr*k,2*cr*k);
        }
        Pen capRim(Color(200,4,4,5),std::max(.8f,d*.04f));
        g.DrawEllipse(&capRim,x-cr,y-cr,2*cr,2*cr);
        Pen outer(Color(235,3,3,4),std::max(1.f,d*.06f));
        g.DrawEllipse(&outer,x-r,y-r,d,d);
        return;
    }
    GraphicsPath circle;
    circle.AddEllipse(x-r,y-r,d,d);
    PathGradientBrush body(&circle);
    body.SetCenterPoint(PointF(x-r*.30f,y-r*.38f));
    body.SetCenterColor(p[kKnobGradient]);
    INT count=1;
    Color surround[1]={p[kKnobBody]};
    body.SetSurroundColors(surround,&count);
    g.FillPath(&body,&circle);
    // the cap: a lighter disc fading towards the lower edge
    const float cr=r*.76f;
    GraphicsPath cap;cap.AddEllipse(x-cr,y-cr,2*cr,2*cr);
    LinearGradientBrush sheen(PointF(x,y-cr),PointF(x,y+cr),Color(120,255,255,255),Color(0,255,255,255));
    g.FillPath(&sheen,&cap);
    Pen rim(Color(235,3,3,4),std::max(1.f,d*.06f));
    g.DrawEllipse(&rim,x-r,y-r,d,d);
    Pen bevel(Color(60,255,255,255),.7f);
    g.DrawEllipse(&bevel,x-r+1.1f,y-r+1.1f,d-2.2f,d-2.2f);
}
void paintKnobPointer(Graphics& g,const Palette13& p,float x,float y,float d,float frac) {
    const float r=d*.5f;
    const float angle=(-140+280*frac-90)*kPi/180;
    const float dx=std::cos(angle), dy=std::sin(angle);
    if(gFlatStyle) { // a value arc just outside the knob body
        const float ar=r+d*.05f;
        Pen track(Color(90,255,255,255),std::max(1.2f,d*.07f)), value(p[kHeaderText],std::max(1.6f,d*.10f));
        track.SetStartCap(LineCapRound);track.SetEndCap(LineCapRound);value.SetStartCap(LineCapRound);value.SetEndCap(LineCapRound);
        const int steps=36;
        for(int pass=0;pass<2;pass++) {
            const float span=pass==0 ? 1.f : frac;
            const int n=std::max(1,int(std::ceil(steps*span)));
            for(int i=0;i<n;i++) {
                const float a0=(-230+280*span*float(i)/float(n))*kPi/180, a1=(-230+280*span*float(i+1)/float(n))*kPi/180;
                g.DrawLine(pass==0 ? &track : &value,PointF(x+std::cos(a0)*ar,y+std::sin(a0)*ar),PointF(x+std::cos(a1)*ar,y+std::sin(a1)*ar));
            }
        }
    }
    Pen hand(p[kKnobDial],std::max(1.5f,d*.085f));
    hand.SetStartCap(LineCapRound);hand.SetEndCap(LineCapRound);
    const float reach=silverKnobs(p) ? .70f : .86f;
    g.DrawLine(&hand,PointF(x+dx*r*.22f,y+dy*r*.22f),PointF(x+dx*r*reach,y+dy*r*reach));
}
// A vertical fader: the slot and its scale ticks (static)...
void paintFaderTrack(Graphics& g,const Palette13& p,float cx,float top,float length) {
    Pen ticks(alpha(p[kLabel],90),.8f);
    for(int i=0;i<8;i++) {
        const float y=top+length*float(i)/7.f;
        g.DrawLine(&ticks,cx-8.f,y,cx+8.f,y);
    }
    roundedFill(g,rgb(0x040506),cx-1.8f,top-1.f,3.6f,length+2.f,1.8f);
    roundedOutline(g,Color(110,255,255,255),cx-1.8f,top-1.f,3.6f,length+2.f,1.8f,.6f);
}
// ...and its cap (dynamic).
void paintFaderCap(Graphics& g,float cx,float top,float length,float frac) {
    const float cy=top+length*(1.f-std::clamp(frac,0.f,1.f));
    const float w=14.f,h=7.f;
    GraphicsPath cap;roundedPath(cap,cx-w/2,cy-h/2,w,h,1.8f);
    LinearGradientBrush face(PointF(cx,cy-h/2),PointF(cx,cy+h/2),rgb(0xE3E4E6),rgb(0x6E6F72));
    g.FillPath(&face,&cap);
    Pen edge(Color(230,4,4,5),.9f);
    g.DrawPath(&edge,&cap);
    Pen groove(Color(120,20,20,22),.8f);
    g.DrawLine(&groove,cx-w/2+1.f,cy,cx+w/2-1.f,cy);
}
// The magnifier of the search button.
void magnifier(Graphics& g,const Color& c,float cx,float cy,float size) {
    Pen pen(c,1.5f);
    pen.SetStartCap(LineCapRound);pen.SetEndCap(LineCapRound);
    const float r=size*.30f;
    g.DrawEllipse(&pen,cx-size*.12f-r,cy-size*.12f-r,2*r,2*r);
    g.DrawLine(&pen,cx+size*.10f,cy+size*.10f,cx+size*.34f,cy+size*.34f);
}
// ThemeStore.shared: one themes.json per process, shared by every editor.
// remember() writes once the edit settles (the theme editor streams edits).
struct SharedThemes {
    themes::Store store{storage::Root()/"themes.json"};
    ULONGLONG writeDue=0;
    void remember(const themes::Palette& p) { if(store.Remember(p)) writeDue=GetTickCount64()+400; }
    void flush(bool now) {
        if(writeDue && (now || GetTickCount64()>=writeDue)) {writeDue=0;store.Write();}
    }
    // The Windows port's theme FILES: <data folder>\Themes\*.json (portable:
    // Synth2_Data\Themes), listed after the saved themes; read each time a
    // list is shown, so files dropped in Explorer appear at once.
    std::vector<themes::Named> fileThemes() const {
        return themes::WithoutFactoryCopies(themes::LoadFolder(storage::Root()/"Themes"));
    }
    // The palette of an instance without a session theme. Re-reads the
    // file: the last pick may come from another process.
    themes::Palette startPalette() {
        if(writeDue) flush(true); else store.Load();
        return store.Current();
    }
};
SharedThemes& sharedThemes() {
    static SharedThemes shared;
    return shared;
}

} // namespace
themes::Store& DesktopEditor::themeStore() { return sharedThemes().store; }
std::vector<themes::Named> DesktopEditor::themeFiles() { return sharedThemes().fileThemes(); }

DesktopEditor::DesktopEditor(OxoSynth2Controller* controller)
    : CPluginView(nullptr), controller_(controller) {
    // Prefer last accepted user zoom in the active standard/portable data folder.
    // The first-run default is 1000 px wide (114%).
    const int savedWidth=winzoom::LoadWidth(storage::Root(),1000,kMinWidth,kMaxWidth);
    rect=ViewRect(0,0,savedWidth,int(std::lround(savedWidth*double(kDesignHeight)/double(kDesignWidth))));
    if(controller_) hooks_=controller_->MakeHooks();
    if(startGraphics()) textCache_=std::make_unique<TextCache>();
    constructLayout();
    buildLearnRects();
    // SynthModel.init: the session's palette, else the last one picked;
    // either way pinned to the session.
    const auto restored=hooks_.themeText ? themes::FromStateText(hooks_.themeText(hooks_.ctx)) : std::nullopt;
    theme_=restored ? *restored : sharedThemes().startPalette();
    for(size_t i=0;i<colors_.size();++i) colors_[i]=rgb(int(theme_[i]));
    if(hooks_.setThemeText) hooks_.setThemeText(hooks_.ctx,themes::StateText(theme_).c_str());
}
DesktopEditor::~DesktopEditor() {
    sharedThemes().flush(true);
    closeThemeEditor();
    removedFromParent();
    textCache_.reset(); // before GDI+ goes
    endGraphics();
}
tresult PLUGIN_API DesktopEditor::checkSizeConstraint(ViewRect* r) {
    if(!r) return kInvalidArgument;
    int width=std::clamp(int(r->getWidth()),kMinWidth,kMaxWidth);
    r->right=r->left+width;
    r->bottom=r->top+int(std::lround(width*double(kDesignHeight)/double(kDesignWidth)));
    return kResultTrue;
}
tresult PLUGIN_API DesktopEditor::onSize(ViewRect* r) {
    if(!r) return kInvalidArgument;
    rect=*r;
    layoutChild();
    // A zoom requested in this editor is stored at once. Some hosts send an
    // initial onSize at their own default: host resizes count only once the
    // editor has settled (the user dragging the host's frame), saved when
    // they stop.
    if(!resizeDragging_ && requestedZoomWidth_==rect.getWidth()) {
        requestedZoomWidth_=-1;
        pendingSizeSave_=-1;
        winzoom::SaveWidth(storage::Root(),rect.getWidth(),kMinWidth,kMaxWidth);
    } else if(!resizeDragging_ && window_ && GetTickCount64()>=sizeSettledAt_) {
        // the host's frame resized us: remember it once the drag stops
        pendingSizeSave_=rect.getWidth();
        pendingSizeSaveAt_=GetTickCount64()+kSizeSaveDelayMs;
    }
    return kResultTrue;
}
void DesktopEditor::savePendingSize(bool now) {
    if(pendingSizeSave_<0 || (!now && GetTickCount64()<pendingSizeSaveAt_)) return;
    winzoom::SaveWidth(storage::Root(),pendingSizeSave_,kMinWidth,kMaxWidth);
    pendingSizeSave_=-1;
}
void DesktopEditor::attachedToParent() {
    if(window_ || !systemWindow || !openWindow()) return; // the window + the 50 ms timer (tick)
    for(size_t id=0;id<knownValues_.size();++id) knownValues_[id]=value(int(id));
    sizeSettledAt_=GetTickCount64()+kSizeSettleMs;
    pendingSizeSave_=-1;
    // Critical: Json() and loadedProgram() call EnsureScanned(), which
    // scans all banks synchronously. Never call them before a background
    // scan has produced a usable revision (large libraries took ~1 min).
    const int revision=hooks_.libraryRevision ? hooks_.libraryRevision(hooks_.ctx) : 0;
    if(revision>0) { knownRevision_=revision; refreshBanks(); }
    // As the Mac editor: rescan in the background on every open, so banks
    // changed in Explorer (or zips dropped in) show up by themselves; the
    // saved index keeps the list usable meanwhile, the poll swaps it in.
    if(hooks_.startLibraryScan) hooks_.startLibraryScan(hooks_.ctx);
    repaint();
}
void DesktopEditor::removedFromParent() {
    updateCheck_.Cancel(); // no answer for a closed editor, no thread left behind
    typeMenu_=typeMenuHover_=-1;
    savePendingSize(true); // closed right after a host resize
    flash({});
    closeWindow(); // the editor object may outlive its window: no bitmaps while closed
}
int DesktopEditor::value(int id) const {
    return hooks_.getValue ? int(hooks_.getValue(hooks_.ctx,id)) : 0;
}
// SynthPanelView.ctrlSrcLabel: a MIDI ctrl source word as the mac shows it.
static std::wstring ctrlSrcLabel(int v) {
    switch(v>>8) {
    case 0xB0: {
        const int cc=v&0xFF;
        const wchar_t* name=L"cc";
        switch(cc) {
        case 1:name=L"mod wheel";break; case 2:name=L"breath";break; case 4:name=L"foot";break;
        case 5:name=L"porta time";break; case 7:name=L"volume";break; case 8:name=L"balance";break;
        case 10:name=L"pan";break; case 11:name=L"expression";break; case 64:name=L"sustain";break;
        case 65:name=L"portamento";break; case 66:name=L"sostenuto";break; case 67:name=L"soft";break;
        case 71:name=L"resonance";break; case 72:name=L"release";break; case 73:name=L"attack";break;
        case 74:name=L"cutoff";break; case 91:name=L"reverb";break; case 93:name=L"chorus";break;
        default:break;
        }
        return L"#"+std::to_wstring(cc)+L":"+name;
    }
    case 0xE0: return L"pitch bend";
    case 0xD0: return L"ch pressure";
    default: {
        if(v<=0) return L"none";
        wchar_t hex[16];
        swprintf(hex,16,L"0x%04X",unsigned(v));
        return hex;
    }
    }
}
std::wstring DesktopEditor::formatted(int id) const {
    if(id==86 || id==88) return ctrlSrcLabel(value(id));
    if(id==87 || id==89) {
        const int v=value(id);
        if(v<0) return L"none";
        if(v>=s2::kParamCount) return std::to_wstring(v);
        return toWide(hooks_.paramName ? hooks_.paramName(hooks_.ctx,v) : "");
    }
    if(!hooks_.formatValue) return std::to_wstring(value(id));
    // formatValue uses a shared scratch buffer; consume immediately.
    return toWide(hooks_.formatValue(hooks_.ctx,id,value(id)));
}
std::wstring DesktopEditor::patchName() const { return toWide(hooks_.patchName ? hooks_.patchName(hooks_.ctx) : "Init"); }
void DesktopEditor::flash(const std::wstring& text,unsigned milliseconds) {
    if(text.empty() && flashText_.empty()) return;
    flashText_=text;
    flashUntil_=text.empty() ? 0 : GetTickCount64()+milliseconds;
    invalidate(kLcdRect);
}
// SynthModel.show: an edit from the panel names the param and its value
// for 1.5 s (host automation shows nothing).
void DesktopEditor::show(int id) {
    flash(toWide(hooks_.displayName ? hooks_.displayName(hooks_.ctx,id) : "")+L":  "+formatted(id));
}

// ---- layout ---------------------------------------------------------------
// The dark Synth2 layout on an 877x449 canvas. Every Synth1 parameter keeps
// its original ID and is driven by OxoSynth2Hooks; coordinates are design
// pixels (the reference mock-up at half size).
void DesktopEditor::knob(int id,const wchar_t* name,float cx,float cy,float d,float below) {
    if(d<=22.f) d*=.87f; // the mock-up's small knobs
    if(below<=0.f) below=d*.5f+8.5f;
    controls_.push_back({Kind::Knob,id,0,cx-d*.65f,cy-d*.65f,d*1.3f,d*.65f+below+6.f,name,0,d,below});
}
void DesktopEditor::button(int id,const wchar_t* name,Kind kind,float x,float y,float w,float h) {
    controls_.push_back({kind,id,0,x,y,w,h,name});
}
void DesktopEditor::display(int id,float x,float y,float w,float h) {
    controls_.push_back({Kind::Display,id,0,x,y,w,h,L""});
}
// A selector LED at (ledX, cy) with its caption starting at labelX.
void DesktopEditor::radio(int id,int value,const wchar_t* name,float ledX,float cy,float labelX,float labelW) {
    const bool justLed=(id==41 || id==46); // the LFO destination matrix: names are separate labels
    if(justLed) controls_.push_back({Kind::Radio,id,value,ledX-8.f,cy-8.f,16.f,16.f,L"",0,0,0});
    else controls_.push_back({Kind::Radio,id,value,ledX-7.f,cy-7.f,labelX+labelW-ledX+7.f,14.f,name,0,labelX-(ledX-7.f),0});
}
void DesktopEditor::waves(int id,float cx0,float cx1,float cy0,float cy1,float w,float h,
                          const std::vector<int>& values,const std::vector<int>& shapes) {
    for(size_t i=0;i<values.size();i++) {
        const float cx=i%2==0 ? cx0 : cx1, cy=i/2==0 ? cy0 : cy1;
        controls_.push_back({Kind::Wave,id,values[i],cx-w/2,cy-h/2,w,h,L"",shapes[i]});
    }
}
void DesktopEditor::fader(int id,float cx,float top,float length) {
    controls_.push_back({Kind::Fader,id,0,cx-9.f,top-5.f,18.f,length+10.f,L""});
}
void DesktopEditor::plate(const wchar_t* name,float x,float y,float w,float h,bool highlighted) {
    controls_.push_back({Kind::Plate,-1,0,x,y,w,h,name,highlighted?1:0});
}
void DesktopEditor::label(float cx,float cy,const wchar_t* name,bool left) {
    if(left) controls_.push_back({Kind::Label,-1,0,cx,cy-8.f,90.f,16.f,name,0,1.f,0});
    else controls_.push_back({Kind::Label,-1,0,cx-45.f,cy-8.f,90.f,16.f,name,0,0,0});
}
void DesktopEditor::badge(float x,float y,const wchar_t* name,int id) {
    controls_.push_back({Kind::Badge,id,0,x,y,14.f,14.f,name});
}

void DesktopEditor::constructLayout() {
    controls_.clear();
    // ---- LFO
    badge(11,92,L"1",57);
    controls_.push_back({Kind::Wave,42,0,32,88,52,25,L"",0});
    button(41,L"dst",Kind::Cycle,89,92,30,16);
    knob(43,L"spd",25,136.5f,20,19);knob(44,L"amt",65,136.5f,20,19);
    label(106.5f,123.5f,L"-sync-");
    button(67,L"tempo",Kind::Check,84,131,44,13);
    button(68,L"key",Kind::Check,84,145,44,13);
    badge(11,184,L"2",58);
    controls_.push_back({Kind::Wave,47,0,32,181,52,25,L"",0});
    button(46,L"dst",Kind::Cycle,89,186,30,16);
    knob(48,L"spd",26,230,20,19);knob(49,L"amt",65,230,20,19);
    label(106.5f,217,L"-sync-");
    button(69,L"tempo",Kind::Check,84,224,44,13);
    button(70,L"key",Kind::Check,84,238,44,13);
    label(134,89,L"LFO1");controls_.back().b=7.6f;controls_.back().symbol=1;
    label(171,89,L"DESTINATION");controls_.back().b=7.6f;
    label(206,89,L"LFO2");controls_.back().b=7.6f;controls_.back().symbol=1;
    static const wchar_t* const dests[]={L"OSC2",L"OSC1,2",L"FILTER",L"AMP",L"P/W",L"FM",L"PAN"};
    for(int i=0;i<7;i++) {
        const float y=111.f+19.5f*float(i);
        radio(41,i+1,L"",137.5f,y,0);radio(46,i+1,L"",204.f,y,0);
        label(171,y,dests[i]);
    }
    // ---- oscillators
    plate(L"OSC 1",232,89,36,15);
    waves(0,290,316,88,103.5f,22,14,{0,1,2,3},{0,1,2,3});
    knob(76,L"det",364,93,19,15.5f);knob(45,L"FM",397.5f,93,19,15.5f);
    plate(L"SUB",232,124,36,15);
    waves(96,290,316,129,145,22,14,{0,1,2,3},{0,3,1,5});
    knob(95,L"level",364,132.5f,19,15.5f);
    radio(97,0,L"0oct",398,129,407,50);radio(97,1,L"-1oct",398,143,407,50);
    plate(L"OSC 2",232,168,36,15);
    waves(1,285.5f,314.5f,167.5f,187,26,13,{1,2,3,4},{1,2,3,4});
    button(7,L"ring",Kind::Toggle,327,170,27,13);
    button(6,L"sync",Kind::Toggle,356,170,29,13);
    button(4,L"track",Kind::Toggle,387,170,32,13);
    knob(2,L"pitch",433.5f,174,19,15);knob(3,L"fine",460,174,19,15);
    knob(5,L"mix",300,213,19,15.5f);knob(8,L"p/w",331,213,19,15.5f);
    knob(91,L"phase",368,213,19,15.5f);knob(72,L"tune",407,213,19,15.5f);
    display(9,434,204,29,19);label(448,228,L"key shift");
    label(232,246,L"MOD ENV",true);
    button(10,L"ON",Kind::Toggle,232,254,36,14);
    button(71,L"dest.",Kind::Cycle,275,254,35,14);
    knob(12,L"A",328,251.5f,18,14.5f);knob(13,L"D",364.5f,251.5f,18,14.5f);knob(11,L"amt",399,251.5f,18,14.5f);
    radio(71,0,L"OSC2",433,243.5f,441,30);radio(71,1,L"FM",433,254.5f,441,30);radio(71,2,L"P/W",433,265,441,30);
    // ---- filter (the response graph is drawn by drawFilterGraph)
    knob(19,L"cutoff",516,186,34,25);knob(20,L"resonance",581,186,34,25);
    button(14,L"type",Kind::Cycle,621,160,38,15);
    static const wchar_t* const types[]={L"LP12",L"LP24",L"HP12",L"BP12",L"LPDL"};
    for(int i=0;i<5;i++) radio(14,i,types[i],629,186.f+11.f*float(i),638,32);
    knob(23,L"sat",509,244,16,16);knob(22,L"trk",543,244,16,16);
    button(24,L"vel",Kind::Toggle,579,238,34,14);
    // ---- envelopes
    plate(L"FILTER",678.5f,109.5f,46,17);plate(L"AMP",678.5f,192.5f,46,17);
    static const wchar_t* const adsr[]={L"A",L"D",L"S",L"R"};
    for(int i=0;i<4;i++) {
        const float x=733.5f+26.f*float(i);
        label(x,88,adsr[i]);controls_.back().b=10.f;fader(15+i,x,95,58);
        label(x,173,adsr[i]);controls_.back().b=10.f;fader(25+i,x,181,58);
    }
    knob(21,L"amt",848.5f,111,20,20);
    knob(29,L"gain",848.5f,194,19,19);knob(30,L"vel",848.5f,233.5f,16,15.5f);
    // ---- voice
    radio(38,0,L"poly",19,309.5f,27);radio(38,1,L"mono",19,322,27);radio(38,2,L"legato",19,334,27);
    display(94,63,308,29,15);label(79,330,L"poly");
    button(73,L"unison",Kind::Toggle,110,308,48,15);
    display(93,126,330,29,15);label(140.5f,350.5f,L"num");
    label(35,355,L"portamento");knob(39,L"",34,371,18,1);
    button(74,L"auto",Kind::Toggle,62,356,39,14);
    knob(75,L"det",33,399,18,15);knob(92,L"phase",71,399,18,15);
    knob(84,L"sprd",106.5f,399,18,15);knob(85,L"pitch",142,399,18,15);
    // ---- arpeggiator
    button(59,L"ON",Kind::Toggle,180,308,35,15);
    button(31,L"type",Kind::Cycle,222,308,33,15);
    button(32,L"range",Kind::Cycle,262,308,33,15);
    static const wchar_t* const arp[]={L"updown",L"up",L"down",L"random"};
    static const wchar_t* const oct[]={L"1oct",L"2oct",L"3oct",L"4oct"};
    static const float arpY[]={343,357,371,385};
    for(int i=0;i<4;i++) {radio(31,i+1,arp[i],183,arpY[i],192,40);radio(32,i,oct[i],243,arpY[i],251,32);}
    knob(33,L"beat",295,342.5f,18,15);knob(34,L"gate",295,381.5f,18,15);
    // ---- wheel / midi
    label(346,312,L"p.b range");display(40,372,306,28,14);
    label(332,336.5f,L"src1");display(86,344,329,58,15);label(408,336.5f,L"->");display(87,414,329,60,15);
    label(332,358.5f,L"src2");display(88,344,351.5f,58,15);label(408,358.5f,L"->");display(89,414,351.5f,60,15);
    knob(50,L"sens1",367,393,19,18);knob(51,L"sens2",431,393,19,18);
    // ---- effect, equalizer / pan, delay, chorus
    button(77,L"ON",Kind::Toggle,495,306,36,15);display(78,494,327.5f,39,13.5f);
    knob(79,L"ctl1",498,369,18,17);knob(80,L"ctl2",528,369,18,17);knob(81,L"level",554,369,18,17);
    knob(61,L"freq",589,327,17,15);knob(62,L"level",615,327,17,15);knob(63,L"Q",640,327,17,15);
    knob(60,L"tone",599,374.5f,17,18);knob(90,L"L-R",632,374.5f,17,18);
    button(65,L"ON",Kind::Toggle,670,305,36,15);display(82,718.5f,305,37,15);
    knob(35,L"time",679.5f,341.5f,18,16);knob(83,L"sprd",708,341.5f,18,16);knob(36,L"fdbk",743,341.5f,18,16);
    knob(98,L"tone",695,381.5f,18,17);knob(37,L"d/w",730.5f,381.5f,18,17);
    button(66,L"ON",Kind::Toggle,779,305,36,15);display(64,828,305,37,15);
    knob(52,L"time",786.5f,341.5f,18,16);knob(53,L"depth",820,341.5f,18,16);knob(54,L"rate",852.5f,341.5f,18,16);
    knob(55,L"fdbk",800,381.5f,18,17);knob(56,L"level",836,381.5f,18,17);
    // ---- master volume (header)
    label(697,14,L"vol");
    knob(99,L"",723,14,16,1);
}

// The PLAY view (not in the mock-up): the performance controls at large size.
void DesktopEditor::constructPlayLayout() {
    controls_.clear();
    // tone
    knob(19,L"cutoff",62,125,46,34);knob(20,L"resonance",160,125,46,34);
    knob(21,L"env amt",258,125,46,34);knob(23,L"sat",356,125,46,34);
    static const wchar_t* const types[]={L"LP12",L"LP24",L"HP12",L"BP12",L"LPDL"};
    label(24,214,L"FILTER TYPE",true);
    for(int i=0;i<5;i++) radio(14,i,types[i],33.f+84.f*float(i),240,42.f+84.f*float(i),36);
    // envelopes
    plate(L"AMP",470,84,70,16);plate(L"FILTER",655,84,70,16);
    static const wchar_t* const adsr[]={L"A",L"D",L"S",L"R"};
    for(int i=0;i<4;i++) {
        const float xa=478.f+40.f*float(i), xf=663.f+40.f*float(i);
        fader(25+i,xa,112,130);label(xa,252,adsr[i]);
        fader(15+i,xf,112,130);label(xf,252,adsr[i]);
    }
    knob(29,L"gain",830,125,34,26);knob(30,L"vel",830,205,34,26);
    // lfo
    knob(43,L"spd 1",45,334,30,26);knob(44,L"amt 1",115,334,30,26);
    knob(48,L"spd 2",185,334,30,26);knob(49,L"amt 2",255,334,30,26);
    // effects
    knob(81,L"effect",338,334,30,26);knob(62,L"eq",408,334,30,26);
    knob(37,L"delay",478,334,30,26);knob(56,L"chorus",548,334,30,26);
    // arp + master
    button(59,L"ON",Kind::Toggle,606,302,44,16);
    knob(33,L"beat",645,358,28,23);knob(34,L"gate",705,358,28,23);
    button(73,L"unison",Kind::Toggle,606,325,64,16);
    label(697,14,L"vol");
    knob(99,L"",723,14,16,1);
}

void DesktopEditor::setView(View view) {
    if(view_==view && !controls_.empty()) return;
    view_=view;
    closeTypeMenu();
    if(view==View::Play) constructPlayLayout(); else constructLayout();
    buildLearnRects();
    hoveredControl_=-1;
    staticDirty_=true;
    repaint();
}
// SynthModel.applyTheme / setThemeColor: this instance wears it (session),
// and it becomes the palette the next new instance starts with.
void DesktopEditor::setTheme(const themes::Palette& palette,bool fromThemeEditor) {
    theme_=palette;
    for(size_t i=0;i<colors_.size();++i) colors_[i]=rgb(int(theme_[i]));
    if(hooks_.setThemeText) hooks_.setThemeText(hooks_.ctx,themes::StateText(theme_).c_str());
    sharedThemes().remember(theme_);
    if(!fromThemeEditor) refreshThemeEditor();
    staticDirty_=true;
    repaint();
}
// SynthModel.syncTheme: a session restore may bring another palette; one
// without (older session) keeps the current one, now pinned to the session.
void DesktopEditor::syncTheme() {
    const auto restored=hooks_.themeText ? themes::FromStateText(hooks_.themeText(hooks_.ctx)) : std::nullopt;
    if(!restored) {
        if(hooks_.setThemeText) hooks_.setThemeText(hooks_.ctx,themes::StateText(theme_).c_str());
        return;
    }
    if(*restored==theme_) return;
    theme_=*restored;
    for(size_t i=0;i<colors_.size();++i) colors_[i]=rgb(int(theme_[i]));
    refreshThemeEditor();
    staticDirty_=true;
    repaint();
}
// LogoMenu's theme column: factory themes, then the user's (saved, then
// theme files); the active one checked.
void DesktopEditor::appendThemes(HMENU menu,std::vector<themes::Palette>& picks) const {
    std::vector<themes::Named> list=themes::Factory();
    const auto user=sharedThemes().store.UserThemes();
    const auto files=sharedThemes().fileThemes();
    const size_t factoryCount=list.size(), savedEnd=factoryCount+user.size();
    list.insert(list.end(),user.begin(),user.end());
    list.insert(list.end(),files.begin(),files.end());
    bool checked=false;
    for(size_t i=0;i<list.size();++i) {
        if(i==factoryCount || (i==savedEnd && !user.empty())) AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        const bool active=!checked && list[i].palette==theme_;
        checked=checked || active;
        const std::wstring name=toWide(list[i].name.c_str())+(i==0?L" (default)":L"");
        AppendMenuW(menu,MF_STRING|(active?MF_CHECKED:0),UINT_PTR(kThemeCommand+int(picks.size())),name.c_str());
        picks.push_back(list[i].palette);
    }
}
bool DesktopEditor::pickTheme(int command,const std::vector<themes::Palette>& picks) {
    const int index=command-kThemeCommand;
    if(index<0 || index>=int(picks.size())) return false;
    setTheme(picks[size_t(index)]);
    return true;
}

// Drawing has two layers. The STATIC one (panel, sections, labels, knob
// bodies, button faces, LCD backgrounds) only changes with the theme or the
// size: it is rendered once into a bitmap. Every paint blits it, then draws
// the DYNAMIC parts (knob pointers, LEDs, values, waves, badges, LCD texts,
// voice grid, hover, browser), only where the window is invalid.
void DesktopEditor::text(Graphics& g,const std::wstring& s,float x,float y,float w,float h,float size,
                         Color c,bool,bool centred,bool mono) {
    if(s.empty() || w<=2.f || h<=1.f || !textCache_ || !visible(RectF(x,y,w,h))) return;
    const auto& shape=textCache_->get(s,textFontSize(size),w-2.f,mono);
    if(!shape.path) return;
    const float left=centred ? x+(w-shape.ink.Width)*.5f : x+1.f;
    const float top=y+(h-shape.refH)*.5f;
    const GraphicsState saved=g.Save();
    g.SetClip(RectF(x,y,w,h),CombineModeIntersect);
    g.TranslateTransform(left-shape.ink.X,top-shape.refY);
    SolidBrush brush(c);
    g.FillPath(&brush,shape.path.get());
    g.Restore(saved);
}

bool DesktopEditor::visible(const RectF& r) const {
    return r.X<paintClip_.X+paintClip_.Width && paintClip_.X<r.X+r.Width &&
           r.Y<paintClip_.Y+paintClip_.Height && paintClip_.Y<r.Y+r.Height;
}
RectF DesktopEditor::bounds(const Control& c) {
    // Room for the LED / wave glow and the learn frame around a knob.
    return RectF(c.x-5.f,c.y-5.f,c.w+10.f,c.h+10.f);
}
void DesktopEditor::invalidate(const RectF& r) const {
    if(!window_) return;
    const float sx=float(std::max(1,rect.getWidth()))/kDesignWidth;
    const float sy=float(std::max(1,rect.getHeight()))/kDesignHeight;
    invalidateDevice(int(std::floor(r.X*sx))-2,int(std::floor(r.Y*sy))-2,
                     int(std::ceil((r.X+r.Width)*sx))+2,int(std::ceil((r.Y+r.Height)*sy))+2);
}
// Everything showing parameter `id`: its controls, the LCD readout when it
// is the selected one, the voice grid for voice mode / polyphony.
void DesktopEditor::invalidateParam(int id) const {
    for(const auto& c:controls_) if(c.id==id) invalidate(bounds(c));
    if(id==38 || id==94) invalidate(kMeterRect);
    if(id==14 || id==19 || id==20) invalidate(kGraphRect); // the filter response curve
}
void DesktopEditor::valueChanged(int id) {
    if(id>=0 && id<int(knownValues_.size())) knownValues_[size_t(id)]=value(id);
    invalidateParam(id);
    if(id==typeMenu_) invalidateTypeMenu();
}
// Hover is visual only: redraw the old and the new target, nothing else.
void DesktopEditor::hoverChanged(int control,int chrome) {
    if(control==hoveredControl_ && chrome==hoveredChrome_) return;
    for(const int c:{hoveredControl_,control})
        if(c>=0 && c<int(controls_.size())) invalidate(bounds(controls_[size_t(c)]));
    for(const int c:{hoveredChrome_,chrome}) if(c) invalidate(chromeRect(c));
    hoveredControl_=control;
    hoveredChrome_=chrome;
}

// MIDI learn (SynthControls' Learnable, SynthModel's learn): in learn mode
// every patch control but the ctrl src / assign menus wears a hotspot
// around all its parts (a radio's LEDs, a wave grid): dim border = not
// learned, bright 2 px border = armed, learn-colour fill + CC badge =
// learned. Click arms, double click unlearns; an armed control stays armed
// (the next CC replaces).
void DesktopEditor::buildLearnRects() {
    learnRects_.fill(RectF(0,0,0,0));
    learnOrder_.clear();
    for(const auto& c:controls_) {
        if(c.id<0 || c.id>=kLearnCount || (c.id>=86 && c.id<=89) || c.kind==Kind::Label) continue;
        RectF& r=learnRects_[size_t(c.id)];
        const RectF own(c.x,c.y,c.w,c.h);
        if(r.Width<=0) {r=own;continue;}
        const RectF was=r;
        RectF::Union(r,was,own);
    }
    for(int id=0;id<kLearnCount;id++) {
        RectF& r=learnRects_[size_t(id)];
        if(r.Width<=0) continue;
        r.Inflate(3.f,3.f); // .padding(-3)
        learnOrder_.push_back(id);
    }
    // Drawn larger first, hit smaller first: a control inside a group's
    // box (e.g. beside a radio column) keeps its own hotspot.
    std::stable_sort(learnOrder_.begin(),learnOrder_.end(),[this](int a,int b) {
        const RectF &ra=learnRects_[size_t(a)], &rb=learnRects_[size_t(b)];
        return ra.Width*ra.Height>rb.Width*rb.Height;
    });
}
void DesktopEditor::setLearn(bool on) {
    if(on && browser_) closeBrowser(); // SynthModel.toggleLearn
    midiLearn_=on;
    learnTarget_=learnPrevTarget_=-1;
    if(on) {
        if(hooks_.lastCC) lastCcToken_=hooks_.lastCC(hooks_.ctx);
        for(int id=0;id<kLearnCount;id++) learnCC_[size_t(id)]=hooks_.midiCC ? hooks_.midiCC(hooks_.ctx,id) : 0;
    }
    repaint();
}
void DesktopEditor::learnSelect(int id) {
    for(const int r:{learnTarget_,id}) if(r>=0 && r<kLearnCount) invalidate(learnRects_[size_t(r)]);
    learnTarget_=id;
}
int DesktopEditor::learnHit(float x,float y) const {
    for(auto it=learnOrder_.rbegin();it!=learnOrder_.rend();++it)
        if(learnRects_[size_t(*it)].Contains(x,y)) return *it;
    return -1;
}
void DesktopEditor::drawLearn(Graphics& g) {
    static const Color learn=rgb(0x59A6E8); // Theme.learn
    for(const int id:learnOrder_) {
        const RectF& r=learnRects_[size_t(id)];
        if(!visible(r)) continue;
        const int cc=learnCC_[size_t(id)];
        const bool selected=learnTarget_==id;
        if(cc) {
            GraphicsPath box;roundedPath(box,r.X,r.Y,r.Width,r.Height,4.f);
            SolidBrush fillBrush(Color(102,learn.GetR(),learn.GetG(),learn.GetB()));
            g.FillPath(&fillBrush,&box);
        }
        const float width=selected ? 2.f : 1.f; // strokeBorder: inside the box
        roundedOutline(g,Color(selected ? 255 : 153,learn.GetR(),learn.GetG(),learn.GetB()),
                       r.X+width/2,r.Y+width/2,r.Width-width,r.Height-width,4.f-width/2,width);
        if(cc) {
            const std::wstring label=std::to_wstring(cc);
            const float w=6.f+6.2f*float(label.size()), h=13.f;
            const float bx=r.X+(r.Width-w)/2, by=r.Y+(r.Height-h)/2;
            GraphicsPath badge;roundedPath(badge,bx,by,w,h,2.4f);
            SolidBrush badgeBrush(learn);
            g.FillPath(&badgeBrush,&badge);
            roundedOutline(g,Color(191,255,255,255),bx+.5f,by+.5f,w-1.f,h-1.f,2.4f);
            text(g,label,bx,by,w,h,9,rgb(0xFFFFFF),true,true);
        }
    }
}
// The panel's own chrome buttons: the tabs, < >, search, write, menu, the
// browser's bottom bar and Exit MIDI Learn.
RectF DesktopEditor::chromeRect(int id) {
    switch(id) {
    case cTabEdit: return {17,31,57,19};   case cTabPlay: return {80,31,53,19};
    case cTabBrowser: return {139,31,62,19};
    case cPrev: return {215,5,22,19};      case cNext: return {493,5,22,19};
    case cSearch: return {529,5,24,19};    case cWrite: return {570,5,40,19};
    case cMenu: return {620,5,40,19};      case cLearnExit: return {692,31,178,19};
    case cBrBankPrev: return {160.5f,418,21,17}; case cBrBankNext: return {189,418,22,17};
    case cBrFolder: return {248,418,96,17};      case cBrRefresh: return {350,418,56,17};
    case cBrImport: case cBrWrite: return {661,418,96,17};
    case cBrExport: case cBrNewBank: return {767.5f,418,97,17};
    default: return {0,0,0,0};
    }
}
bool DesktopEditor::tabSelected(int id) const {
    if(id==cTabBrowser) return browser_;
    return !browser_ && ((id==cTabEdit && view_==View::Edit) || (id==cTabPlay && view_==View::Play));
}
void DesktopEditor::drawChrome(Graphics& g,int id,bool hovered) {
    const auto& p=colors_;
    const RectF r=chromeRect(id);
    if(!visible(RectF(r.X-2,r.Y-2,r.Width+4,r.Height+4))) return;
    const Color accent=p[kHeaderText];
    if(id==cTabEdit || id==cTabPlay || id==cTabBrowser) {
        const bool on=tabSelected(id);
        const wchar_t* name=id==cTabEdit ? L"EDIT" : id==cTabPlay ? L"PLAY" : L"BROWSER";
        if(on) {
            GraphicsPath path;roundedPath(path,r.X,r.Y,r.Width,r.Height,3.5f);
            LinearGradientBrush face(PointF(r.X,r.Y),PointF(r.X,r.Y+r.Height),blend(p[kPanel],accent,.52f),blend(p[kPanel],accent,.22f));
            g.FillPath(&face,&path);
            Pen edge(alpha(accent,235),1.2f);g.DrawPath(&edge,&path);
            Pen glow(alpha(accent,60),2.6f);g.DrawPath(&glow,&path);
        } else darkButton(g,p[kButton],r.X,r.Y,r.Width,r.Height,hovered);
        text(g,name,r.X,r.Y,r.Width,r.Height,10,on ? accent : rgb(0xEEEEEE),true,true);
        return;
    }
    if(id==cSearch) {
        darkButton(g,p[kButton],r.X,r.Y,r.Width,r.Height,hovered);
        magnifier(g,rgb(0xEEEEEE),r.X+r.Width/2,r.Y+r.Height/2,13.f);
        return;
    }
    const bool red=id==cWrite || id==cLearnExit;
    darkButton(g,red ? rgb(id==cWrite ? 0xB81715 : 0xA30D0D) : p[kButton],r.X,r.Y,r.Width,r.Height,hovered);
    const wchar_t* name=L"";
    float size=9.5f;
    switch(id) {
    case cPrev: name=L"<";size=12;break;      case cNext: name=L">";size=12;break;
    case cWrite: name=L"write";size=10;break; case cMenu: name=L"menu";size=10;break;
    case cLearnExit: name=L"Exit MIDI Learn";size=10;break;
    case cBrBankPrev: name=L"<";size=11;break; case cBrBankNext: name=L">";size=11;break;
    case cBrFolder: name=L"LIBRARY FOLDER";break; case cBrRefresh: name=L"REFRESH";break;
    case cBrImport: name=L"IMPORT BANK...";break; case cBrExport: name=L"EXPORT BANK...";break;
    case cBrWrite: name=L"WRITE PATCH";break; case cBrNewBank: name=L"NEW BANK...";break;
    default: break;
    }
    text(g,name,r.X,r.Y,r.Width,r.Height,size,rgb(0xFFFFFF),true,true);
}

void DesktopEditor::drawControlStatic(Graphics& g,const Control& c) {
    const bool lowerRow=gFlatStyle && c.y>=kLowerRowTop;
    const Palette13 lowerPal=lowerRow ? lowerPalette(colors_) : colors_;
    const auto& p=lowerRow ? lowerPal : colors_;
    switch(c.kind) {
    case Kind::Label: {
        const bool accentInk=c.symbol==1;
        const Color ink=accentInk ? p[kHeaderText] : p[kLabel];
        const float size=c.b>0.f && c.a!=1.f ? c.b : c.b>1.f ? c.b : 9.0f;
        text(g,c.label,c.x,c.y,c.w,c.h,size,ink,false,c.a!=1.f);
        return;
    }
    case Kind::Knob: {
        const float d=c.a;
        const float x=c.x+c.w/2, y=c.y+d*.65f;
        paintKnobBody(g,p,x,y,d);
        text(g,c.label,x-34,y+c.b-6.f,68,12,9.0f,p[kLabel],false,true);
        return;
    }
    case Kind::Toggle:
    case Kind::Cycle: {
        darkButton(g,p[kButton],c.x,c.y,c.w,c.h);
        const bool toggle=c.kind==Kind::Toggle;
        const float size=c.w<33.f ? 7.0f : 9.0f;
        const float lead=toggle ? (c.w<33.f ? 6.8f : 12.f) : 0.f;
        text(g,c.label,c.x+lead,c.y,c.w-lead-1.f,c.h,size,rgb(0xEDEDED),true,true);
        return;
    }
    case Kind::Check:
        text(g,c.label,c.x+14.f,c.y,c.w-14.f,c.h,9.0f,p[kLabel],false,false);
        return;
    case Kind::Display:
        displayTile(g,p,c.x,c.y,c.w,c.h);
        return;
    case Kind::Radio:
        if(c.id!=41 && c.id!=46) text(g,c.label,c.x+c.a,c.y,c.w-c.a,c.h,9.0f,p[kLabel],false,false);
        return;
    case Kind::Plate: {
        const bool hi=c.symbol==1;
        darkButton(g,hi ? blend(p[kButton],p[kHeaderText],.10f) : p[kButton],c.x,c.y,c.w,c.h);
        if(hi) roundedOutline(g,alpha(p[kHeaderText],230),c.x+.5f,c.y+.5f,c.w-1.f,c.h-1.f,3.f,1.1f);
        float size=7.8f; // shrinks until the name fits the plate (Windows' font is wider)
        while(size>5.6f && textCache_ && textCache_->get(c.label,textFontSize(size),c.w-4.f,false).cut) size-=.3f;
        text(g,c.label,c.x,c.y,c.w,c.h,size,hi ? p[kHeaderText] : rgb(0xEDEDED),true,true);
        return;
    }
    case Kind::Fader:
        paintFaderTrack(g,p,c.x+c.w/2,c.y+5.f,c.h-10.f);
        return;
    case Kind::Badge:
    case Kind::Wave:
        return; // their face depends on the value: dynamic
    }
}

void DesktopEditor::drawControlDynamic(Graphics& g,const Control& c,bool hovered) {
    const bool lowerRow=gFlatStyle && c.y>=kLowerRowTop;
    const Palette13 lowerPal=lowerRow ? lowerPalette(colors_) : colors_;
    const auto& p=lowerRow ? lowerPal : colors_;
    const Color ledOff=blend(p[kPanel],p[kLabel],.36f);
    switch(c.kind) {
    case Kind::Label:
    case Kind::Plate:
        return;
    case Kind::Badge: {
        const bool on=(c.id<0 || value(c.id)!=0);
        const Color accent=p[kHeaderText];
        GraphicsPath path;roundedPath(path,c.x,c.y,c.w,c.h,2.5f);
        SolidBrush fillBrush(on ? blend(p[kPanel],accent,.16f) : blend(p[kPanel],rgb(0),.25f));
        g.FillPath(&fillBrush,&path);
        Pen edge(on ? alpha(accent,hovered?255:220) : blend(p[kPanel],accent,hovered?.5f:.32f),1.1f);
        g.DrawPath(&edge,&path);
        text(g,c.label,c.x,c.y+.5f,c.w,c.h,10.5f,on ? accent : blend(p[kPanel],accent,.5f),true,true);
        return;
    }
    case Kind::Knob: {
        const float d=c.a;
        int low=0,high=127,def=64;
        if(hooks_.paramInfo) hooks_.paramInfo(hooks_.ctx,c.id,&low,&high,&def);
        const float frac=std::clamp((float(value(c.id))-float(low))/float(std::max(1,high-low)),0.f,1.f);
        paintKnobPointer(g,p,c.x+c.w/2,c.y+d*.65f,d,frac);
        return;
    }
    case Kind::Toggle:
    case Kind::Cycle:
        if(hovered) {
            darkButton(g,p[kButton],c.x,c.y,c.w,c.h,true);
            const bool toggle=c.kind==Kind::Toggle;
            const float lead=toggle ? (c.w<33.f ? 6.8f : 12.f) : 0.f;
            text(g,c.label,c.x+lead,c.y,c.w-lead-1.f,c.h,c.w<33.f ? 7.0f : 9.0f,rgb(0xEDEDED),true,true);
        }
        if(c.kind==Kind::Toggle) {
            const bool on=value(c.id)!=0;
            led(g,on?p[kLed]:ledOff,c.x+(c.w<33.f?6.5f:8.f),c.y+c.h/2,c.w<33.f?2.2f:2.9f,on);
        }
        return;
    case Kind::Check: {
        const bool on=value(c.id)!=0;
        led(g,on?p[kLed]:ledOff,c.x+6.5f,c.y+c.h/2,3.f,on);
        return;
    }
    case Kind::Display: {
        std::wstring v=formatted(c.id);
        float size=10.5f;
        if(c.id==9 || c.id==40) { // 2-digit displays
            wchar_t out[32];std::swprintf(out,32,L"%02d",value(c.id));v=out;
            size=12.5f;
        } else if(c.id>=86 && c.id<=89) size=8.2f;
        else if(c.id==94 || c.id==93) size=12.f;
        text(g,v,c.x+1,c.y+.5f,c.w-2,c.h-1,size,p[kDisplayText],true,true,true);
        return;
    }
    case Kind::Radio: {
        const bool on=value(c.id)==c.value;
        const bool justLed=(c.id==41 || c.id==46);
        led(g,on?p[kRadio]:ledOff,justLed ? c.x+c.w*.5f : c.x+7.f,c.y+c.h/2,3.3f,on);
        return;
    }
    case Kind::Wave:
        drawWave(g,c);
        return;
    case Kind::Fader: {
        int low=0,high=127,def=64;
        if(hooks_.paramInfo) hooks_.paramInfo(hooks_.ctx,c.id,&low,&high,&def);
        const float frac=std::clamp((float(value(c.id))-float(low))/float(std::max(1,high-low)),0.f,1.f);
        paintFaderCap(g,c.x+c.w/2,c.y+5.f,c.h-10.f,frac);
        return;
    }
    }
}

// WaveIcon.path: one shape stroked into (x, y, w, h). Shapes: 0 sine, 1 / 6
// (falling) saw, 2 pulse, 5 square, 3 triangle, 4 noise, 7 S&H, 8 smooth.
static void drawWaveShape(Graphics& g,Pen& pen,int shape,float x,float y,float w,float h) {
    switch(shape) {
        case 0: { // sine
            PointF pts[25];
            for(int i=0;i<25;i++) {
                const float t=i/24.f;
                pts[i]=PointF(x+w*t,y+h*(.5f-.5f*std::sin(2*kPi*t)));
            }
            g.DrawLines(&pen,pts,25);break;
        }
        case 1:case 6: { // saw / falling saw, original two-stroke shape
            PointF pts[]={{x,y+h},{x,y},{x+w,y+h}};
            g.DrawLines(&pen,pts,3);break;
        }
        case 2:case 5: { // pulse (30%) / square (50%)
            const float duty=(shape==2 ? .30f : .50f);
            PointF pts[]={{x,y+h},{x,y},{x+w*duty,y},
                          {x+w*duty,y+h},{x+w,y+h},{x+w,y}};
            g.DrawLines(&pen,pts,6);break;
        }
        case 3: { // triangle
            PointF pts[]={{x,y+h},{x+w*.5f,y},{x+w,y+h}};
            g.DrawLines(&pen,pts,3);break;
        }
        case 4: { // fixed sample of noise; same samples as WaveIcon.noise
            const float ys[]={.5f,.1f,.8f,.3f,.95f,.2f,.6f,0.f,.7f,.4f,.9f,.25f,.55f};
            PointF pts[13];
            for(int i=0;i<13;i++)pts[i]=PointF(x+w*i/12.f,y+h*ys[i]);
            g.DrawLines(&pen,pts,13);break;
        }
        case 7:case 8: { // stepped or smoothly interpolated random LFO
            const float ys[]={.6f,.1f,.8f,.35f,.95f};
            const float step=w/5.f, curve=step*.35f;
            GraphicsPath path;
            path.StartFigure();
            path.AddLine(x,y+h*ys[0],x+step-(shape==8?curve:0),y+h*ys[0]);
            for(int i=1;i<5;i++) {
                const float edge=x+step*i;
                const float prev=y+h*ys[i-1], level=y+h*ys[i];
                if(shape==8) {
                    path.AddBezier(edge-curve,prev,edge,prev,edge,level,edge+curve,level);
                } else path.AddLine(edge,prev,edge,level);
                path.AddLine(edge+(shape==8?curve:0),level,
                             x+step*(i+1)-(shape==8 && i<4?curve:0),level);
            }
            g.DrawPath(&pen,&path);break;
        }
        default:break;
    }
}
void DesktopEditor::drawWave(Graphics& g,const Control& c) {
    const auto& p=colors_;
    const bool lfo=(c.id==42||c.id==47);
    const bool on=(value(c.id)==c.value) || lfo;
    const Color ink=p[kDisplayText];
    if(lfo) displayTile(g,p,c.x,c.y,c.w,c.h,true);
    else {
        // a lit tile glows; the others stay in the display's dark.
        if(on) {
            roundedOutline(g,alpha(ink,28),c.x-2.f,c.y-2.f,c.w+4.f,c.h+4.f,4.5f,1.8f);
            roundedOutline(g,alpha(ink,55),c.x-1.f,c.y-1.f,c.w+2.f,c.h+2.f,3.5f,1.2f);
        }
        displayTile(g,p,c.x,c.y,c.w,c.h,on);
    }
    Pen pen(on ? ink : alpha(ink,125),lfo ? 1.9f : 1.5f);
    pen.SetStartCap(LineCapRound);pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    const float padX=lfo ? 8.f : 4.5f, padY=lfo ? 5.f : 3.f;
    const float x=c.x+padX,y=c.y+padY,w=c.w-2*padX,h=c.h-2*padY;
    int shape=c.symbol;
    if(lfo) {
        constexpr int cycleShapes[]={6,3,5,7,8,0};
        shape=cycleShapes[std::clamp(value(c.id),0,5)];
    }
    drawWaveShape(g,pen,shape,x,y,w,h);
}

// Type pick lists (SynthPanelView's TypeMenu): under the display / wave
// button, cells of its width x 17, display colours, Synth1's grids
// (effect type 2 x 5, LFO shapes 2 x 3; delay / chorus type one column),
// offered values only (chorus type 1 / 2 / 4).
static constexpr int kLfoShapeOrder[]={0,1,5,2,3,4}; // SynthPanelView.lfoShapeOrder
static constexpr int kLfoShapeIcon[]={6,3,5,7,8,0};  // value -> drawWaveShape's shape
DesktopEditor::TypeMenu DesktopEditor::typeMenu() const {
    TypeMenu m;
    if(typeMenu_<0) return m;
    const auto c=std::find_if(controls_.begin(),controls_.end(),[&](const Control& k) {
        return k.id==typeMenu_ && (k.kind==Kind::Display || k.kind==Kind::Wave);
    });
    if(c==controls_.end()) return m;
    const bool lfo=typeMenu_==42 || typeMenu_==47;
    const auto& info=s2::Info(typeMenu_);
    for(int k=0;k<=info.max-info.min;k++) {
        const int v=lfo ? kLfoShapeOrder[k] : info.min+k;
        if(s2::Offered(typeMenu_,v)) m.values.push_back(v);
    }
    m.rows=typeMenu_==78 ? 5 : 3;
    m.columns=(int(m.values.size())+m.rows-1)/m.rows;
    m.cellW=c->w;
    m.x=c->x;m.y=c->y+c->h+.5f;
    return m;
}
int DesktopEditor::typeMenuHit(float x,float y) const {
    const TypeMenu m=typeMenu();
    if(m.values.empty() || x<m.x || y<m.y) return -1;
    const int column=int((x-m.x)/m.cellW), row=int((y-m.y)/m.cellH);
    const size_t k=size_t(column*m.rows+row);
    return column<m.columns && row<m.rows && k<m.values.size() ? m.values[k] : -1;
}
void DesktopEditor::invalidateTypeMenu() const {
    const TypeMenu m=typeMenu();
    if(m.values.empty()) return;
    const RectF r=m.rect();
    invalidate(RectF(r.X-8.f,r.Y-6.f,r.Width+16.f,r.Height+16.f)); // + the shadow
}
void DesktopEditor::openTypeMenu(int id) {
    closeTypeMenu();
    typeMenu_=id;
    typeMenuHover_=-1;
    hoverChanged(-1,0);
    invalidateTypeMenu();
}
void DesktopEditor::closeTypeMenu() {
    if(typeMenu_<0) return;
    invalidateTypeMenu();
    typeMenu_=typeMenuHover_=-1;
}
void DesktopEditor::drawTypeMenu(Graphics& g) {
    const TypeMenu m=typeMenu();
    if(m.values.empty()) return;
    const RectF r=m.rect();
    if(!visible(RectF(r.X-8.f,r.Y-6.f,r.Width+16.f,r.Height+16.f))) return;
    const auto& p=colors_;
    // the mac's shadow (black 50 %, radius 6, 3 down): stacked soft rings
    for(int i=6;i>=1;--i) {
        SolidBrush shade(Color(BYTE(10),0,0,0));
        g.FillRectangle(&shade,r.X-float(i),r.Y+3.f-float(i),r.Width+2.f*float(i),r.Height+2.f*float(i));
    }
    LinearGradientBrush face(PointF(r.X,r.Y),PointF(r.X,r.Y+r.Height),p[kDisplayTop],p[kDisplayBottom]);
    g.FillRectangle(&face,r);
    const Color ink=p[kDisplayText];
    const int shown=s2::OfferedStep(typeMenu_,value(typeMenu_),0); // a loaded chorus 3 marks x2
    const bool lfo=typeMenu_==42 || typeMenu_==47;
    Pen cell(Color(89,0,0,0),.5f);
    for(size_t k=0;k<m.values.size();++k) {
        const int v=m.values[k];
        const float x=m.x+float(int(k)/m.rows)*m.cellW, y=m.y+float(int(k)%m.rows)*m.cellH;
        const bool on=v==shown;
        if(on) fill(g,ink,x,y,m.cellW,m.cellH);
        else if(v==typeMenuHover_) fill(g,Color(51,ink.GetR(),ink.GetG(),ink.GetB()),x,y,m.cellW,m.cellH);
        const Color itemInk=on ? p[kDisplayBottom] : ink;
        if(lfo) {
            Pen pen(itemInk,1.5f);
            pen.SetStartCap(LineCapRound);pen.SetEndCap(LineCapRound);pen.SetLineJoin(LineJoinRound);
            drawWaveShape(g,pen,kLfoShapeIcon[std::clamp(v,0,5)],x+5.f,y+4.f,m.cellW-10.f,m.cellH-8.f);
        } else {
            text(g,toWide(hooks_.formatValue ? hooks_.formatValue(hooks_.ctx,typeMenu_,v) : ""),
                 x,y,m.cellW,m.cellH,11,itemInk,true,true,true);
        }
        g.DrawRectangle(&cell,x+.25f,y+.25f,m.cellW-.5f,m.cellH-.5f);
    }
    Pen border(Color(179,0,0,0),1.f);
    g.DrawRectangle(&border,r.X+.5f,r.Y+.5f,r.Width-1.f,r.Height-1.f);
}

// The filter's magnitude response in dB at `hz`, from the engine's own
// laws (EngineCore/Filter.hpp): knob -> cutoff Hz, the SVF's damping for
// LP12 / HP12 / BP12, the Moog-style feedback for LP24, a softer feedback
// for the diode ladder. Display only; it never touches the sound.
static float filterResponseDb(int type,int cutoffKnob,int resKnob,float hz) {
    const float fc=16.7f*std::exp2(std::min(float(cutoffKnob),125.f)/12.624f);
    const float x=hz/fc, r=std::clamp(float(resKnob)/127.f,0.f,1.f);
    float mag=1.f;
    if(type==1 || type==4) { // LP24 / LPDL: 4 one-pole stages with global feedback
        const float k=std::max(0.f,(0.0297f*float(resKnob)-0.076f))*(type==1 ? 1.f : .72f);
        // (1 + jx)^4 + k
        const float re1=1.f, im1=x;
        float re=re1, im=im1;
        for(int i=0;i<3;i++) {const float nr=re*re1-im*im1, ni=re*im1+im*re1;re=nr;im=ni;}
        re+=k;
        mag=1.f/std::max(1e-4f,std::sqrt(re*re+im*im));
    } else { // 2-pole state variable
        const float d=std::max(.025f,1.f-r), gain=1.f-.7f*r;
        const float re=1.f-x*x, im=d*x;
        const float den=std::max(1e-4f,std::sqrt(re*re+im*im));
        mag=(type==2 ? x*x : type==3 ? x : 1.f)/den*gain;
    }
    return 20.f*std::log10(std::max(mag,1e-5f));
}
void DesktopEditor::drawFilterGraph(Graphics& g,bool frameOnly) {
    const auto& p=colors_;
    const RectF plot(503,84,163,57);
    const Color ink=p[kDisplayText];
    if(frameOnly) {
        displayTile(g,p,483,81,185,75);
        // dB lines + the frequency grid
        Pen grid(alpha(ink,36),.7f);
        static const wchar_t* const db[]={L"+12",L"0",L"-12",L"-24",L"-36"};
        for(int i=0;i<5;i++) {
            const float y=plot.Y+plot.Height*float(i)/4.f;
            g.DrawLine(&grid,plot.X,y,plot.X+plot.Width,y);
            text(g,db[i],484,y-6.f,19,12,7.8f,alpha(p[kLabel],215),false,false);
        }
        static const struct {float hz;const wchar_t* name;} marks[]={{20,L"20"},{50,L"50"},{100,L"100"},{200,L"200"},
            {500,L"500"},{1000,L"1k"},{2000,L"2k"},{5000,L"5k"},{10000,L"10k"}};
        for(const auto& m:marks) {
            const float x=plot.X+plot.Width*std::log10(m.hz/20.f)/3.f;
            g.DrawLine(&grid,x,plot.Y,x,plot.Y+plot.Height);
            text(g,m.name,std::clamp(x,plot.X+7.f,plot.X+plot.Width-8.f)-12.f,plot.Y+plot.Height+1.5f,24,10,7.6f,alpha(p[kLabel],215),false,true);
        }
        return;
    }
    const int type=value(14), cutoff=value(19), res=value(20);
    constexpr int n=96;
    PointF pts[n+3];
    auto yOf=[&](float dB) {return plot.Y+plot.Height*(12.f-std::clamp(dB,-36.f,12.f))/48.f;};
    for(int i=0;i<n;i++) {
        const float t=float(i)/float(n-1);
        const float hz=20.f*std::pow(1000.f,t);
        pts[i]=PointF(plot.X+plot.Width*t,yOf(filterResponseDb(type,cutoff,res,hz)));
    }
    pts[n]=PointF(plot.X+plot.Width,plot.Y+plot.Height);
    pts[n+1]=PointF(plot.X,plot.Y+plot.Height);
    const GraphicsState saved=g.Save();
    g.SetClip(plot,CombineModeReplace);
    LinearGradientBrush area(PointF(0,plot.Y),PointF(0,plot.Y+plot.Height),alpha(ink,100),alpha(ink,18));
    g.FillPolygon(&area,pts,n+2);
    Pen curve(ink,1.6f);
    curve.SetLineJoin(LineJoinRound);
    g.DrawLines(&curve,pts,n);
    g.Restore(saved);
}

void DesktopEditor::drawStatic(Graphics& g) {
    const auto& p=colors_;
    gFlatStyle=lightStrips(p);
    // background: the panel colour with a soft vignette
    {
        LinearGradientBrush bg(PointF(0,0),PointF(0,kDesignHeight),blend(p[kPanel],rgb(0xFFFFFF),.03f),blend(p[kPanel],rgb(0x000000),.22f));
        g.FillRectangle(&bg,0.f,0.f,kDesignWidth,kDesignHeight);
    }
    // header bar and tab row
    {
        LinearGradientBrush bar(PointF(0,0),PointF(0,27),blend(p[kPanel],rgb(0x000000),.22f),blend(p[kPanel],rgb(0x000000),.46f));
        g.FillRectangle(&bar,0.f,0.f,kDesignWidth,27.f);
        fill(g,blend(p[kPanel],rgb(0x000000),.58f),0,27.5f,kDesignWidth,25.5f);
        Pen line(blend(p[kPanel],p[kLabel],.2f),1.f);
        g.DrawLine(&line,0.f,27.5f,kDesignWidth,27.5f);
        Pen line2(Color(220,3,4,4),1.f);
        g.DrawLine(&line2,0.f,52.5f,kDesignWidth,52.5f);
        text(g,L"Synth2",15,1,86,25,20,blend(p[kLabel],rgb(0xE8D5B0),.35f),true);
        Pen sep(alpha(p[kLabel],70),1.f);
        g.DrawLine(&sep,101.f,6.f,101.f,21.f);
        text(g,L"Virtual Analog",110,5,100,17,10.8f,muted(p[kLabel],.82f));
        displayTile(g,p,kLcdRect.X,kLcdRect.Y,kLcdRect.Width,kLcdRect.Height);
        g.DrawLine(&sep,675.f,6.f,675.f,23.f);
        text(g,L"VOICES",740,6,44,17,7.6f,muted(p[kLabel],.9f),false,true);
        g.DrawLine(&sep,210.f,33.f,210.f,48.f);
    }
    struct Section {float x,y,w,h;const wchar_t* name;};
    static constexpr Section editSections[]={
        {5,60,214,213,L"LFO"},{224,60,250,213,L"OSCILLATORS"},{479,60,192,213,L"FILTER"},{676,60,197,213,L"ENVELOPES"},
        {5,284,163,139,L"VOICE"},{173,284,140,139,L"ARPEGGIATOR"},{318,284,162,139,L"WHEEL / MIDI"},
        {485,284,84,139,L"EFFECT"},{574,284,81,139,L"EQ / PAN"},{660,284,102,139,L"TEMPO DELAY"},
        {767,284,106,139,L"CHORUS / FLANGER"},
    };
    static constexpr Section playSections[]={
        {5,60,436,213,L"TONE"},{446,60,427,213,L"ENVELOPES"},{5,284,290,139,L"LFO"},
        {300,284,290,139,L"EFFECTS"},{595,284,278,139,L"ARPEGGIATOR"},
    };
    const bool edit=view_==View::Edit;
    const Section* sections=edit ? editSections : playSections;
    const size_t count=edit ? std::size(editSections) : std::size(playSections);
    for(size_t i=0;i<count;i++) {
        const auto& sec=sections[i];
        sectionPlate(g,p,sec.x,sec.y,sec.w,sec.h,19.f,gFlatStyle&&sec.y>=kLowerRowTop);
        if(lightStrips(p)) text(g,sec.name,sec.x,sec.y,sec.w,19,9.2f,rgb(0x1C1C1E),true,true); // centred, dark
        else text(g,sec.name,sec.x+9,sec.y,sec.w-12,19,9.6f,p[kHeaderText],true);
    }
    const Palette13 lowerPal=gFlatStyle ? lowerPalette(p) : p;
    if(edit) {
        // dividers
        rule(g,p,9,169.5f,122,169.5f);
        for(const float y:{116.f,157.f,197.5f,236.f}) rule(g,p,230,y,468,y);
        rule(g,p,314,238,314,272);
        rule(g,p,484,227,565,227);rule(g,p,569,234,569,261);
        rule(g,p,679,162.5f,870,162.5f);
        rule(g,lowerPal,106.5f,306,106.5f,382);rule(g,lowerPal,9,346,100,346);rule(g,lowerPal,9,383,162,383);
        rule(g,lowerPal,230,335,230,395);
        rule(g,lowerPal,333,375,475,375);
        // the LFO destination matrix
        roundedFill(g,rgb(0x08090B),126.5f,99,89,140,4.f);
        roundedOutline(g,blend(p[kPanel],p[kLabel],.2f),126.5f,99,89,140,4.f);
        Pen row(alpha(p[kLabel],36),.8f);
        for(int i=0;i<6;i++) {
            const float y=111.f+19.5f*float(i)+9.75f;
            g.DrawLine(&row,131.f,y,211.f,y);
        }
        drawFilterGraph(g,true);
    } else {
        rule(g,p,15,205,430,205);
    }
    for(const auto& c:controls_) drawControlStatic(g,c);
    // version / project link
    text(g,L"Synth2 V"+toWide(OXO_VERSION)+L"  -  a Synth1 recreation",8,426,240,16,8.6f,alpha(p[kLabel],95),false,false);
    // Small bottom-right host-resize grip. All drawing scales as one canvas.
    Pen grip(blend(p[kPanel],p[kLabel],.5f),1.f);
    for(int i=0;i<3;i++) {
        const float a=float(i*4);
        g.DrawLine(&grip,kDesignWidth-17.f+a,kDesignHeight-3.f,kDesignWidth-3.f,kDesignHeight-17.f+a);
    }
}

void DesktopEditor::drawDynamic(Graphics& g) {
    gFlatStyle=lightStrips(colors_);
    const auto& p=colors_;
    // The browser covers the panel area below the tabs: only the header's
    // controls (volume) show through.
    for(size_t i=0;i<controls_.size();++i) {
        const auto& c=controls_[i];
        if((browser_ && c.y+c.h>kBrowserRect.Y+4.f) || !visible(bounds(c))) continue;
        drawControlDynamic(g,c,hoveredControl_==int(i));
    }
    if(!browser_ && view_==View::Edit && visible(kGraphRect)) drawFilterGraph(g,false);
    if(midiLearn_ && !browser_) drawLearn(g);
    for(const int id:{cTabEdit,cTabPlay,cTabBrowser,cPrev,cNext,cSearch,cWrite,cMenu})
        drawChrome(g,id,hoveredChrome_==id);
    int loadedBank=-1,loadedSlot=-1;
    if(visible(kLcdRect) || visible(kBankLineRect)) {
        if(knownRevision_>0 && hooks_.loadedProgram)
            hooks_.loadedProgram(hooks_.ctx,&loadedBank,&loadedSlot);
        loadedBankForPaint_=loadedBank;
        loadedSlotForPaint_=loadedSlot;
    }
    if(visible(kLcdRect)) {
        const int prefix=(loadedSlot>=0 ? loadedSlot+1 : 0);
        wchar_t formattedNumber[16];
        if(prefix) std::swprintf(formattedNumber,16,L"%03d: ",prefix);
        else std::wcscpy(formattedNumber,L"---: ");
        text(g,std::wstring(formattedNumber)+patchName(),240,4.5f,250,21,14.5f,p[kDisplayText],true,true,true);
    }
    if(!midiLearn_) {
        if(visible(kBankLineRect)) {
            std::wstring second=flashText_;
            Color ink=p[kDisplayText];
            if(second.empty()) {
                ink=p[kLabel];
                if(loadedBank>=0 && loadedBank<int(banks_.size())) {
                    wchar_t num[16];std::swprintf(num,16,L"%02d: ",displayBankNumber(loadedBank));
                    second=std::wstring(num)+banks_[size_t(loadedBank)].leaf;
                } else second=knownRevision_>0 ? L"Click the preset name to browse banks" : L"Scanning preset banks...";
            }
            text(g,second,218,31,kBankLineRect.Width-8,19,10.5f,ink,false,false,true);
        }
        if(visible(kMeterRect)) {
            const uint64_t states=hooks_.voiceStates ? hooks_.voiceStates(hooks_.ctx) : 0;
            // Poly shows the allowed voice count; Mono and Legato show one
            // allocated cell. Playing / released voices show in every mode.
            const int allocated=value(38)==0 ? std::clamp(value(94),1,32) : 1;
            const float cw=kMeterRect.Width/16.f, ch=kMeterRect.Height/2.f;
            for(int i=0;i<32;i++) {
                const int state=int((states>>(i*2))&3);
                const float x=kMeterRect.X+float(i%16)*cw, y=kMeterRect.Y+float(i/16)*ch;
                Color color=blend(p[kPanel],rgb(0),.55f);
                if(state==2) color=p[kLed];
                else if(state==1) color=blend(p[kPanel],p[kLed],.45f);
                else if(i<allocated) color=blend(p[kPanel],p[kLabel],.16f);
                roundedFill(g,color,x+.7f,y+.8f,cw-1.4f,ch-1.6f,.8f);
            }
        }
    } else {
        drawChrome(g,cLearnExit,hoveredChrome_==cLearnExit);
        text(g,L"Select a control, then move a MIDI CC",218,31,470,19,9.5f,p[kLabel],true,false);
    }
    if(browser_ && visible(kBrowserRect)) drawBrowser(g);
    drawTypeMenu(g);
    if(press_.active && press_.dragging && visible(dragLabelRect())) {
        // slotDraggable's preview: the label on the display colour
        const RectF r=dragLabelRect();
        GraphicsPath box;roundedPath(box,r.X,r.Y,r.Width,r.Height,2.f);
        SolidBrush face(Color(230,p[themes::kDisplayTop].GetR(),p[themes::kDisplayTop].GetG(),p[themes::kDisplayTop].GetB()));
        g.FillPath(&face,&box);
        text(g,press_.label,r.X+3,r.Y,r.Width-6,r.Height,10.2f,rgb(kLcdText),false,false,true);
    }
    drawBankTip(g);
}

void DesktopEditor::refreshBanks() {
    // Even a request to obtain Json() is a blocking scan before revision 1.
    if(knownRevision_<=0 || !hooks_.libraryJSON) return;
    const char* data=hooks_.libraryJSON(hooks_.ctx);
    if(!data) return;
    std::string json=data; // the hooks share one scratch buffer
    // SynthModel.loadLibrary: a zip found in the library was imported
    // ("X.zip imported and moved to the Recycle Bin"), shown 10 s.
    if(const char* notice=hooks_.libraryNotice ? hooks_.libraryNotice(hooks_.ctx) : nullptr; notice && *notice)
        flash(toWide(notice),10000);
    data=json.c_str();
    json::Value root;
    if(!json::Parse(data,root) || !root.IsObject()) return;
    std::vector<Bank> scanned;
    std::string libraryRoot;
    if(const auto* v=root.Find("root"); v && v->IsString()) libraryRoot=v->string;
    if(const auto* list=root.Find("banks"); list && list->type==json::Value::Type::Array) {
        for(const auto& item:list->array) {
            Bank b;
            if(const auto* v=item.Find("name"); v && v->IsString()) b.name=v->string;
            if(const auto* v=item.Find("source"); v && v->IsString()) b.source=v->string;
            if(const auto* v=item.Find("readOnly")) b.readOnly=v->type==json::Value::Type::Bool && v->boolean;
            if(const auto* v=item.Find("slots"); v && v->type==json::Value::Type::Array)
                for(const auto& slot:v->array) b.slots.push_back(slot.IsString() ? slot.string : std::string());
            scanned.push_back(std::move(b));
        }
    }
    // Backend order/indices remain unchanged. Natural sort is UI-only, so
    // the .sy1 slot numbers, host state and bank hook indices are untouched.
    // The browser stays on the bank it shows (SynthModel.loadLibrary keeps
    // currentBank by its id = source).
    const std::string previousSelection = selectedBank_>=0 && selectedBank_<int(banks_.size()) ?
        banks_[size_t(selectedBank_)].source : std::string();
    libraryRoot_=std::move(libraryRoot);
    banks_=std::move(scanned);
    for(auto& b:banks_) { // once per refresh, not per painted row
        b.wideName=toWide(b.name.c_str());
        b.leaf=toWide(s2::BankLeafName(b.name).c_str());
    }
    resetBankTip(); // its bank index may be another bank now
    if(!previousSelection.empty()) {
        selectedBank_=bankBySource(previousSelection);
        if(selectedBank_<0) selectedSlot_=-1;
    }
    bankOrder_.resize(banks_.size());
    std::iota(bankOrder_.begin(),bankOrder_.end(),0);
    std::stable_sort(bankOrder_.begin(),bankOrder_.end(),[this](int a,int b) {
        // The read-only factory bank stays first.
        if(a==0 && banks_[size_t(a)].readOnly) return true;
        if(b==0 && banks_[size_t(b)].readOnly) return false;
        return NaturalLess(banks_[size_t(a)].wideName,banks_[size_t(b)].wideName);
    });
    int loadedBank=-1,loadedSlot=-1;
    if(hooks_.loadedProgram) hooks_.loadedProgram(hooks_.ctx,&loadedBank,&loadedSlot);
    if(selectedBank_==-1 && loadedBank>=0) selectedBank_=loadedBank;
    if(selectedBank_==-1 && !banks_.empty()) selectedBank_=bankOrder_.front();
    if(selectedBank_>=int(banks_.size())) selectedBank_=banks_.empty()?-1:bankOrder_.front();
    if(selectedSlot_<0 && loadedSlot>=0 && selectedBank_==loadedBank) selectedSlot_=loadedSlot;
    // The write dialog never targets the read-only factory bank: out of it
    // (or out of no bank) it opens on the "User" bank, proposed even when
    // the library has none (the write makes it). A writable bank in view
    // stays (the loaded one: written back). As PatchBrowserView.leaveReadOnlyBank.
    if(writing_ && !banks_.empty() && (selectedBank_<0 || banks_[size_t(selectedBank_)].readOnly)) {
        const int user=userBank();
        const int pick=user>=0 ? user : kNewUserBank;
        if(pick!=selectedBank_) {
            selectedBank_=pick;selectedSlot_=-1;
            scrollBankIntoView(pick);
        }
    }
    if(writing_ && selectedSlot_<0 && selectedBank_==kNewUserBank) selectedSlot_=0;
    if(writing_ && selectedSlot_<0 && selectedBank_>=0) {
        const auto& slots=banks_[size_t(selectedBank_)].slots;
        auto it=std::find(slots.begin(),slots.end(),"");
        selectedSlot_=it==slots.end()?0:int(it-slots.begin());
    }
    bankScroll_=std::clamp(bankScroll_,0,std::max(0,bankRowCount()-kBankRows));
    repaint();
}
int DesktopEditor::userBank() const {
    for(size_t b=0;b<banks_.size();b++)
        if(!banks_[b].readOnly && banks_[b].name==s2::kUserBankName) return int(b);
    return -1;
}
bool DesktopEditor::offersNewUserBank() const {
    return writing_ && !banks_.empty() && userBank()<0;
}
int DesktopEditor::bankRowCount() const {
    return int(bankOrder_.size())+(offersNewUserBank()?1:0);
}
int DesktopEditor::bankRowOf(int bank) const {
    if(bank==kNewUserBank) return offersNewUserBank() ? int(bankOrder_.size()) : -1;
    const auto it=std::find(bankOrder_.begin(),bankOrder_.end(),bank);
    return it==bankOrder_.end() ? -1 : int(it-bankOrder_.begin());
}
void DesktopEditor::scrollBankIntoView(int bank) {
    const int row=bankRowOf(bank);
    if(row>=0 && (row<bankScroll_ || row>=bankScroll_+kBankRows)) bankScroll_=std::max(0,row-kBankRows+1);
}
int DesktopEditor::bankBySource(const std::string& source) const {
    for(size_t i=0;i<banks_.size();i++) if(banks_[i].source==source) return int(i);
    return -1;
}
// The library changed under us (a hook rescanned): take the new list.
void DesktopEditor::adoptLibrary() {
    if(hooks_.libraryRevision) knownRevision_=hooks_.libraryRevision(hooks_.ctx);
    refreshBanks();
}
// A bank's index in the hooks' list as of NOW, -1 = gone: menus, message
// boxes and drags are modal loops in which the timer runs, and a scan
// landing meanwhile renumbers the banks.
int DesktopEditor::bankNow(const std::string& source) {
    if(hooks_.libraryRevision && hooks_.libraryRevision(hooks_.ctx)!=knownRevision_) adoptLibrary();
    return bankBySource(source);
}
// "NNN:name" of a slot (LCD messages, drag label).
std::wstring DesktopEditor::slotText(int bank,int slot) const {
    wchar_t number[8];std::swprintf(number,8,L"%03d:",slot+1);
    return number+toWide(banks_[size_t(bank)].slots[size_t(slot)].c_str());
}
void DesktopEditor::openBrowser(bool writing) {
    browser_=true;writing_=writing;
    bankScroll_=slotScroll_=0;selectedSlot_=-1;
    // SynthModel.openBrowser: opening selects the loaded program's bank (the
    // write dialog: refreshBanks then leaves a read-only one for User).
    int loadedBank=-1,loadedSlot=-1;
    if(knownRevision_>0 && hooks_.loadedProgram) hooks_.loadedProgram(hooks_.ctx,&loadedBank,&loadedSlot);
    if(loadedBank>=0 && loadedBank<int(banks_.size())) selectedBank_=loadedBank;
    else if(selectedBank_==kNewUserBank) selectedBank_=-1;
    // SynthModel.openBrowser: a background rescan on every open (the list
    // in place stays usable; a scan already running is not restarted).
    if(hooks_.startLibraryScan) hooks_.startLibraryScan(hooks_.ctx);
    refreshBanks();
    scrollBankIntoView(selectedBank_); // as the list's scrollTo(currentBank)
    updateEditControl();
    takeKeyFocus();
    repaint();
}
void DesktopEditor::closeBrowser() {
    browser_=writing_=false;
    resetBankTip();
    if(selectedBank_==kNewUserBank) selectedBank_=-1; // the next refresh selects the loaded bank
    releaseKeyFocus(); // before the name field goes
    updateEditControl();
    repaint();
}
// The patch's category column. The library has no category data, so it is
// read from the name (keywords), falling back to "Synth".
std::wstring DesktopEditor::categoryOf(const std::string& name) {
    std::wstring w=toWide(name.c_str());
    for(auto& ch:w) ch=wchar_t(std::towlower(ch));
    struct Rule {const wchar_t* key;const wchar_t* category;};
    static constexpr Rule rules[]={
        {L"string",L"Strings"},{L"orchestr",L"Strings"},{L"violin",L"Strings"},{L"cello",L"Strings"},
        {L"pad",L"Pads"},{L"atmos",L"Pads"},{L"ambient",L"Pads"},{L"choir",L"Pads"},{L"warm",L"Pads"},
        {L"bass",L"Bass"},{L"sub ",L"Bass"},{L"lead",L"Leads"},{L"solo",L"Leads"},{L"sync",L"Leads"},
        {L"brass",L"Brass"},{L"horn",L"Brass"},{L"trumpet",L"Brass"},{L"organ",L"Keys"},
        {L"piano",L"Keys"},{L"keys",L"Keys"},{L"clav",L"Keys"},{L"rhodes",L"Keys"},{L"bell",L"Keys"},
        {L"pluck",L"Plucks"},{L"harp",L"Plucks"},{L"guitar",L"Plucks"},{L"arp",L"Arp"},{L"seq",L"Arp"},
        {L"sweep",L"Motion"},{L"motion",L"Motion"},{L"wobble",L"Motion"},{L"evolv",L"Motion"},
        {L"fx",L"FX"},{L"noise",L"FX"},{L"effect",L"FX"},{L"riser",L"FX"},{L"hit",L"FX"},
        {L"drum",L"Drums"},{L"kick",L"Drums"},{L"snare",L"Drums"},{L"hat",L"Drums"},{L"perc",L"Drums"},
    };
    for(const auto& r:rules) if(w.find(r.key)!=std::wstring::npos) return r.category;
    return L"Synth";
}
// The patches the preset list shows: the shown bank's slots, or, while
// searching, only the named patches whose name or category match.
std::vector<int> DesktopEditor::shownSlots() const {
    std::vector<int> out;
    const bool fresh=writing_ && selectedBank_==kNewUserBank;
    int count=-1;
    if(fresh) count=s2::kBankSlots;
    else if(selectedBank_>=0 && selectedBank_<int(banks_.size())) count=int(banks_[size_t(selectedBank_)].slots.size());
    if(count<=0) return out;
    std::wstring query=searchText_;
    for(auto& ch:query) ch=wchar_t(std::towlower(ch));
    for(int i=0;i<count;i++) {
        if(fresh || query.empty()) {out.push_back(i);continue;}
        const std::string& name=banks_[size_t(selectedBank_)].slots[size_t(i)];
        if(name.empty()) continue;
        std::wstring hay=toWide(name.c_str())+L" "+categoryOf(name);
        for(auto& ch:hay) ch=wchar_t(std::towlower(ch));
        if(hay.find(query)!=std::wstring::npos) out.push_back(i);
    }
    return out;
}
// Typing in the browser fills the search box (the write dialog's name
// field has the keyboard instead).
bool DesktopEditor::searchKey(wchar_t ch) {
    if(!browser_ || writing_) return false;
    if(ch==8) {
        if(searchText_.empty()) return true;
        searchText_.pop_back();
    } else if(ch>=32 && ch!=127) {
        if(searchText_.size()>=40) return true;
        searchText_.push_back(ch);
    } else return false;
    slotScroll_=0;
    invalidate(kBrowserRect);
    return true;
}
void DesktopEditor::stepBank(int direction) {
    const int n=int(bankOrder_.size());
    if(n==0) return;
    int row=bankRowOf(selectedBank_);
    if(row<0) row=direction>0 ? -1 : n;
    for(int guard=0;guard<n;guard++) {
        row+=direction;
        if(row<0 || row>=n) return;
        const int bank=bankOrder_[size_t(row)];
        if(writing_ && banks_[size_t(bank)].readOnly) continue;
        selectedBank_=bank;selectedSlot_=-1;slotScroll_=0;
        if(writing_) {
            const auto& slots=banks_[size_t(bank)].slots;
            const auto it=std::find(slots.begin(),slots.end(),"");
            selectedSlot_=it==slots.end() ? 0 : int(it-slots.begin());
        }
        scrollBankIntoView(bank);
        repaint();
        return;
    }
}
int DesktopEditor::bankRowAt(float x,float y) const {
    if(x<kBankPanel.X+2 || x>=kBankPanel.X+kBankPanel.Width-2 || y<kBankTop || y>=kBankTop+kBankRowH*float(kBankRows)) return -1;
    const int visual=bankScroll_+int((y-kBankTop)/kBankRowH);
    if(visual>=0 && visual<int(bankOrder_.size())) return bankOrder_[size_t(visual)];
    return visual==int(bankOrder_.size()) && offersNewUserBank() ? kNewUserBank : -1;
}
int DesktopEditor::slotAt(float x,float y) const {
    if(x<kPresetPanel.X+6 || x>=kPresetPanel.X+kPresetPanel.Width-14 ||
       y<kPresetTop || y>=kPresetTop+kPresetRowH*float(kPresetRows)) return -1;
    const auto shown=shownSlots();
    const int at=slotScroll_+int((y-kPresetTop)/kPresetRowH);
    return at>=0 && at<int(shown.size()) ? shown[size_t(at)] : -1;
}

// Right click in the browser = PatchBrowserView's libraryMenu: a bank =
// Export bank... / Show in Explorer [/ Delete bank...], a patch = Show in
// Explorer [/ Delete]; nothing on read-only banks' deletes, nothing on
// banks the write dialog cannot pick. Elsewhere in the browser: no menu.
bool DesktopEditor::showLibraryMenu(float x,float y,int screenX,int screenY) {
    dismissBankTip();
    if(!browser_ || !kBrowserRect.Contains(x,y)) return false;
    enum {kExport=1,kReveal,kDeleteBank,kDelete};
    // What the menu acts on, taken NOW: the menu and the message boxes
    // answer later (Windows: from modal loops) while the timer still runs,
    // and a background scan landing meanwhile renumbers the banks.
    std::string source,slotFile;
    std::wstring slotLabel; // "NNN:name", for the LCD
    HMENU menu=CreatePopupMenu();
    if(!menu) return true;
    if(const int row=bankRowAt(x,y); row>=0 && row<int(banks_.size())) {
        const Bank& b=banks_[size_t(row)];
        if(!(writing_ && b.readOnly)) {
            source=b.source;
            AppendMenuW(menu,MF_STRING,kExport,L"Export bank...");
            AppendMenuW(menu,MF_STRING,kReveal,kShowInFiles);
            if(!b.readOnly) {
                AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
                AppendMenuW(menu,MF_STRING,kDeleteBank,L"Delete bank...");
            }
        }
    } else if(const int index=slotAt(x,y); index>=0 && selectedBank_!=kNewUserBank) {
        const int b=selectedBank_,s=index;
        if(b>=0 && b<int(banks_.size()) && !banks_[size_t(b)].slots[size_t(s)].empty() && hooks_.slotPath) {
            slotFile=hooks_.slotPath(hooks_.ctx,b,s);
            slotLabel=slotText(b,s);
            AppendMenuW(menu,MF_STRING,kReveal,kShowInFiles);
            if(!banks_[size_t(b)].readOnly) {
                AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
                AppendMenuW(menu,MF_STRING,kDelete,L"Delete");
            }
        }
    }
    popupMenu(menu,screenX,screenY,[this,source,slotFile,slotLabel](int choice) {
        auto bankNow=[this,source] {return this->bankNow(source);};
        switch(choice) {
        case kReveal:
            revealInExplorer(slotFile.empty() ? source : slotFile);
            break;
        case kExport:
            pickFolder(L"Export bank: a folder holding its patches is created in the folder you choose",
                       [this,bankNow](const std::wstring& dir) {
                const int bank=bankNow();
                const std::string made=bank>=0 && hooks_.exportBank ? hooks_.exportBank(hooks_.ctx,bank,toUtf8(dir).c_str()) : "";
                if(made.empty()) messageBox(L"Unable to export this bank.",L"Synth2",MB_ICONWARNING);
                else {
                    flash(L"bank exported as "+std::filesystem::path(toWide(made.c_str())).filename().wstring(),4000);
                    revealInExplorer(made);
                }
            });
            break;
        case kDeleteBank: {
            // DeleteBankDialog
            const int bank=bankNow();
            if(bank<0) break;
            const Bank& b=banks_[size_t(bank)];
            const auto count=std::count_if(b.slots.begin(),b.slots.end(),[](const std::string& s){return !s.empty();});
            const std::wstring name=toWide(b.name.c_str()), leaf=b.leaf; // b may not survive until the answer
            const std::wstring detail=L"\u201C"+name+L"\u201D ("+std::to_wstring(count)+
                (count==1?L" patch":L" patches")+(CanRecycle(std::filesystem::path(toWide(b.source.c_str())))
                    ? L") will be moved to the "+std::wstring(kTrash)+L"."
                    : L") will be deleted: this drive has no "+std::wstring(kTrash)+L". This can't be undone.");
            messageBox(detail,L"Delete bank?",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2,[this,bankNow,leaf](int answer) {
                if(answer!=IDYES) return;
                const int bank=bankNow();
                const int result=bank>=0 && hooks_.deleteBank ? hooks_.deleteBank(hooks_.ctx,bank) : 0;
                if(result==0) messageBox(L"Unable to delete this bank.",L"Synth2",MB_ICONWARNING);
                else flash(L"bank "+leaf+(result==1 ? L" moved to the "+std::wstring(kTrash) : std::wstring(L" deleted")));
                adoptLibrary();
                repaint();
            });
            break;
        }
        case kDelete: {
            // SynthModel.deleteSlot: the .sy1 to the Recycle Bin (PatchManager's
            // rule: deleted where there is none), then a rescan.
            if(slotFile.empty()) break;
            const std::filesystem::path file(toWide(slotFile.c_str()));
            std::error_code ec;
            if(MoveToTrash(file)) flash(slotLabel+L" moved to the "+kTrash);
            else flash(std::filesystem::remove(file,ec) ? slotLabel+L" deleted" : std::wstring(L"delete failed"));
            if(hooks_.rescanLibrary) hooks_.rescanLibrary(hooks_.ctx);
            adoptLibrary();
            break;
        }
        default: break;
        }
        repaint();
    });
    return true;
}
// The bank's number in the list ("NN:"), as the browser shows it.
int DesktopEditor::displayBankNumber(int bank) const {
    const int row=bankRowOf(bank);
    return row>=0 ? row : bank;
}
bool DesktopEditor::isVersionBox(float x,float y) const {
    return x>=6.f && x<250.f && y>=424.f && y<444.f;
}

void DesktopEditor::drawBrowser(Graphics& g) {
    const auto& p=colors_;
    const Color accent=p[kHeaderText];
    const Color ink=p[kLabel];
    const Color numberInk=blend(p[kPanel],ink,.62f);
    const Color emptyInk=blend(p[kPanel],ink,.34f);
    // The browser covers everything under the tab row.
    {
        LinearGradientBrush bg(PointF(0,53),PointF(0,kDesignHeight),blend(p[kPanel],rgb(0xFFFFFF),.03f),blend(p[kPanel],rgb(0x000000),.22f));
        g.FillRectangle(&bg,0.f,53.f,kDesignWidth,kDesignHeight-53.f);
    }
    sectionPlate(g,p,kBankPanel.X,kBankPanel.Y,kBankPanel.Width,kBankPanel.Height,24.f);
    sectionPlate(g,p,kPresetPanel.X,kPresetPanel.Y,kPresetPanel.Width,kPresetPanel.Height,24.f);
    sectionPlate(g,p,kPreviewPanel.X,kPreviewPanel.Y,kPreviewPanel.Width,kPreviewPanel.Height,24.f);
    text(g,writing_?L"BANKS (choose where to write)":L"BANKS",kBankPanel.X+10,kBankPanel.Y,kBankPanel.Width-12,24,10.4f,accent,true);
    text(g,writing_?L"WRITE PATCH":L"PRESETS",kPresetPanel.X+10,kPresetPanel.Y,95,24,10.4f,accent,true);
    text(g,L"PATCH PREVIEW",kPreviewPanel.X+10,kPreviewPanel.Y,kPreviewPanel.Width-12,24,10.4f,accent,true);

    // --- banks
    const int visualCount=bankRowCount();
    for(int i=0;i<kBankRows;i++) {
        const int visual=bankScroll_+i;
        if(visual<0 || visual>=visualCount) break;
        const int bi=visual<int(bankOrder_.size()) ? bankOrder_[size_t(visual)] : kNewUserBank;
        const bool isNew=bi==kNewUserBank;
        const bool selected=bi==selectedBank_;
        const bool locked=!isNew && banks_[size_t(bi)].readOnly;
        const float ry=kBankTop+kBankRowH*float(i), rx=kBankPanel.X+6.f, rw=kBankPanel.Width-12.f;
        if(selected) {
            roundedFill(g,alpha(accent,52),rx,ry,rw,kBankRowH-1.f,3.f);
            roundedOutline(g,alpha(accent,230),rx+.5f,ry+.5f,rw-1.f,kBankRowH-2.f,3.f,1.1f);
        }
        // the folder
        const Color folder=selected||!locked ? accent : blend(p[kPanel],ink,.55f);
        Pen fp(folder,1.05f);
        g.DrawRectangle(&fp,rx+7.f,ry+5.f,12.f,8.f);
        g.DrawLine(&fp,rx+7.f,ry+5.f,rx+9.5f,ry+3.2f);g.DrawLine(&fp,rx+9.5f,ry+3.2f,rx+12.f,ry+3.2f);g.DrawLine(&fp,rx+12.f,ry+3.2f,rx+13.5f,ry+5.f);
        wchar_t prefix[16];
        if(isNew) std::swprintf(prefix,16,L"--: ");
        else std::swprintf(prefix,16,L"%02d: ",visual);
        const std::wstring name=isNew ? toWide(s2::kUserBankName)+L" (new)" : banks_[size_t(bi)].leaf;
        text(g,std::wstring(prefix)+name,rx+24.f,ry,rw-26.f,kBankRowH,10.f,selected ? accent : p[kLabel],false,false);
        if(bi>=0 && bi==dropBank_) roundedOutline(g,p[kDisplayText],rx+.5f,ry+.5f,rw-1.f,kBankRowH-2.f,3.f,1.6f);
    }
    // a bank list too long for its panel: a slim scroll bar
    if(visualCount>kBankRows) {
        const float trackY=kBankTop,trackH=kBankRowH*float(kBankRows);
        const float thumbH=std::max(18.f,trackH*float(kBankRows)/float(visualCount));
        const float thumbY=trackY+(trackH-thumbH)*float(bankScroll_)/float(std::max(1,visualCount-kBankRows));
        roundedFill(g,rgb(0x08090B),kBankPanel.X+kBankPanel.Width-6.f,trackY,3.f,trackH,1.5f);
        roundedFill(g,blend(p[kPanel],ink,.45f),kBankPanel.X+kBankPanel.Width-6.f,thumbY,3.f,thumbH,1.5f);
    }

    // --- presets: the search box, the table header, the rows
    roundedFill(g,rgb(0x0B0C0E),kSearchBox.X,kSearchBox.Y,kSearchBox.Width,kSearchBox.Height,4.f);
    roundedOutline(g,blend(p[kPanel],ink,.22f),kSearchBox.X+.5f,kSearchBox.Y+.5f,kSearchBox.Width-1.f,kSearchBox.Height-1.f,4.f);
    magnifier(g,ink,kSearchBox.X+13.f,kSearchBox.Y+kSearchBox.Height/2,12.f);
    if(searchText_.empty() || writing_)
        text(g,L"SEARCH PRESETS... (type)",kSearchBox.X+27.f,kSearchBox.Y,kSearchBox.Width-34.f,kSearchBox.Height,9.4f,emptyInk,false,false);
    else {
        text(g,searchText_,kSearchBox.X+27.f,kSearchBox.Y,kSearchBox.Width-34.f,kSearchBox.Height,10.f,rgb(0xFFFFFF),false,false,true);
        const auto& shape=textCache_->get(searchText_,textFontSize(10.f),kSearchBox.Width-36.f,true);
        const float cx=std::min(kSearchBox.X+29.f+shape.ink.Width+1.f,kSearchBox.X+kSearchBox.Width-8.f);
        fill(g,accent,cx,kSearchBox.Y+4.f,1.f,kSearchBox.Height-8.f);
    }
    const float tableX=kPresetPanel.X+6.f, tableW=kPresetPanel.Width-12.f;
    fill(g,rgb(0x0E0F11),tableX,kSearchBox.Y+kSearchBox.Height+5.f,tableW,16.f);
    text(g,L"NO.",tableX+8.f,111,30,16,9.2f,ink,true);
    text(g,L"PRESET",tableX+55.f,111,100,16,9.2f,ink,true);
    text(g,L"CATEGORY",tableX+245.f,111,80,16,9.2f,ink,true);
    fill(g,rgb(0x08090B),tableX,kPresetTop,tableW,kPresetRowH*float(kPresetRows));
    if(knownRevision_<=0) {
        text(g,L"Scanning preset banks in the background...",tableX+12,kPresetTop+10,300,18,10.5f,rgb(0xC1DCE0));
    } else if(selectedBank_==-1) {
        text(g,L"Select a bank.",tableX+12,kPresetTop+10,300,18,10.5f,rgb(0xC1DCE0));
    } else if(selectedBank_==kNewUserBank || (selectedBank_>=0 && selectedBank_<int(banks_.size()))) {
        const auto shown=shownSlots();
        const bool fresh=selectedBank_==kNewUserBank;
        if(shown.empty() && !searchText_.empty())
            text(g,L"No presets match.",tableX+12,kPresetTop+10,300,18,10.5f,rgb(0xC1DCE0));
        int loadedBank=loadedBankForPaint_, loadedSlot=loadedSlotForPaint_;
        for(int r=0;r<kPresetRows;r++) {
            const int at=slotScroll_+r;
            if(at>=int(shown.size())) break;
            const int slot=shown[size_t(at)];
            const float ry=kPresetTop+kPresetRowH*float(r);
            const std::string label=fresh ? std::string() : banks_[size_t(selectedBank_)].slots[size_t(slot)];
            const bool valid=!label.empty();
            const bool selected=writing_ ? slot==selectedSlot_ : (valid && selectedBank_==loadedBank && slot==loadedSlot);
            if(r%2==1) fill(g,rgb(0x0C0D0F),tableX,ry,tableW-9.f,kPresetRowH);
            if(selected) {
                roundedFill(g,alpha(accent,60),tableX+1.f,ry,tableW-11.f,kPresetRowH,2.f);
                roundedOutline(g,alpha(accent,235),tableX+1.5f,ry+.5f,tableW-12.f,kPresetRowH-1.f,2.f,1.1f);
            }
            wchar_t number[8];std::swprintf(number,8,L"%03d",slot+1);
            const Color rowInk=selected ? rgb(0xFFFFFF) : valid ? p[kLabel] : emptyInk;
            text(g,number,tableX+8.f,ry,36,kPresetRowH,9.6f,selected ? accent : numberInk,false,false,true);
            text(g,toWide(label.c_str()),tableX+55.f,ry,185,kPresetRowH,9.8f,rowInk,false,false,true);
            if(valid) text(g,categoryOf(label),tableX+245.f,ry,110,kPresetRowH,9.4f,selected ? rgb(0xFFFFFF) : blend(p[kPanel],ink,.8f),false,false);
            if(slot==dropSlot_) roundedOutline(g,p[kDisplayText],tableX+1.5f,ry+.5f,tableW-12.f,kPresetRowH-1.f,2.f,1.6f);
        }
        const int total=int(shown.size());
        if(total>kPresetRows) {
            const float trackH=kPresetRowH*float(kPresetRows);
            const float thumbH=std::max(20.f,trackH*float(kPresetRows)/float(total));
            const float thumbY=kPresetTop+(trackH-thumbH)*float(slotScroll_)/float(std::max(1,total-kPresetRows));
            roundedFill(g,rgb(0x15171A),tableX+tableW-8.f,kPresetTop,5.f,trackH,2.5f);
            roundedFill(g,blend(p[kPanel],ink,.5f),tableX+tableW-8.f,thumbY,5.f,thumbH,2.5f);
        }
    }
    if(writing_ && selectedBank_>=0 && selectedBank_<int(banks_.size()) && banks_[size_t(selectedBank_)].readOnly)
        text(g,L"Factory bank is read-only. Choose a user bank.",kPresetPanel.X+10,kPresetTop+kPresetRowH*float(kPresetRows)+2,330,14,9.2f,rgb(0xE2A3A3));

    // --- patch preview: the LOADED patch (clicking a preset loads it)
    {
        const float px=kPreviewPanel.X, pw=kPreviewPanel.Width;
        int lb=-1,ls=-1;
        if(knownRevision_>0 && hooks_.loadedProgram) hooks_.loadedProgram(hooks_.ctx,&lb,&ls);
        const bool known=lb>=0 && lb<int(banks_.size()) && ls>=0 && ls<int(banks_[size_t(lb)].slots.size());
        std::wstring bankName=known ? banks_[size_t(lb)].leaf : L"-";
        std::wstring patchName_=known ? slotText(lb,ls) : this->patchName();
        if(known) {wchar_t n[8];std::swprintf(n,8,L"%02d: ",displayBankNumber(lb));bankName=n+bankName;}
        text(g,L"BANK",px+10,86,50,16,9.4f,ink,false,false);
        text(g,L":",px+58,86,8,16,9.4f,ink,false,false);
        text(g,bankName,px+68,86,pw-76,16,10.4f,p[kDisplayText],false,false,true);
        text(g,L"PATCH",px+10,103,50,16,9.4f,ink,false,false);
        text(g,L":",px+58,103,8,16,9.4f,ink,false,false);
        text(g,patchName_,px+68,103,pw-76,16,10.4f,p[kDisplayText],false,false,true);
        rule(g,p,px+8,122,px+pw-8,122);
        auto plate=[&](const wchar_t* label,float x,float y,float w,float h,bool hi) {
            darkButton(g,hi ? blend(p[kButton],accent,.10f) : p[kButton],x,y,w,h);
            if(hi) roundedOutline(g,alpha(accent,230),x+.5f,y+.5f,w-1.f,h-1.f,3.f,1.1f);
            text(g,label,x,y,w,h,8.4f,hi ? accent : rgb(0xEDEDED),true,true);
        };
        auto oscTile=[&](int id,float y,const wchar_t* label) {
            plate(label,px+14,y+8,46,17,false);
            const float tx=px+72,tw=pw-82;
            displayTile(g,p,tx,y,tw,34.f,true);
            Pen pen(p[kDisplayText],1.8f);
            pen.SetStartCap(LineCapRound);pen.SetEndCap(LineCapRound);pen.SetLineJoin(LineJoinRound);
            const int v=std::clamp(value(id),0,4);
            static constexpr int shapes[]={0,1,2,3,4};
            drawWaveShape(g,pen,shapes[v],tx+12,y+6,tw-24,22);
        };
        oscTile(0,130.f,L"OSC 1");
        oscTile(1,172.f,L"OSC 2");
        rule(g,p,px+8,216,px+pw-8,216);
        plate(L"FILTER",px+14,239,48,18,false);
        for(const int id:{19,20}) {
            const float cx=id==19 ? px+124.f : px+207.f, cy=246.f, d=30.f;
            paintKnobBody(g,p,cx,cy,d);
            int low=0,high=127,def=64;
            if(hooks_.paramInfo) hooks_.paramInfo(hooks_.ctx,id,&low,&high,&def);
            paintKnobPointer(g,p,cx,cy,d,std::clamp(float(value(id)-low)/float(std::max(1,high-low)),0.f,1.f));
            text(g,id==19 ? L"cutoff" : L"resonance",cx-40,cy+22,80,13,9.0f,ink,false,true);
        }
        rule(g,p,px+8,288,px+pw-8,288);
        plate(L"AMP",px+14,323,48,18,false);
        static constexpr const wchar_t* adsr[]={L"A",L"D",L"S",L"R"};
        for(int i=0;i<4;i++) {
            const float cx=px+109.f+40.5f*float(i);
            text(g,adsr[i],cx-10,294,20,13,9.4f,ink,false,true);
            paintFaderTrack(g,p,cx,312.f,62.f);
            int low=0,high=127,def=64;
            if(hooks_.paramInfo) hooks_.paramInfo(hooks_.ctx,25+i,&low,&high,&def);
            paintFaderCap(g,cx,312.f,62.f,std::clamp(float(value(25+i)-low)/float(std::max(1,high-low)),0.f,1.f));
        }
    }

    // --- bottom bar
    roundedFill(g,blend(p[kPanel],rgb(0x000000),.38f),kBottomBar.X,kBottomBar.Y,kBottomBar.Width,kBottomBar.Height,4.f);
    roundedOutline(g,blend(p[kPanel],ink,.2f),kBottomBar.X+.5f,kBottomBar.Y+.5f,kBottomBar.Width-1.f,kBottomBar.Height-1.f,4.f);
    text(g,L"BANK POSITION",kBottomBar.X+14,kBottomBar.Y,112,kBottomBar.Height,10.4f,accent,true);
    {
        const int row=bankRowOf(selectedBank_), last=std::max(0,int(bankOrder_.size())-1);
        wchar_t pos[32];
        if(row>=0 && row<int(bankOrder_.size())) std::swprintf(pos,32,L"%02d / %02d",row,last);
        else std::swprintf(pos,32,L"-- / %02d",last);
        text(g,pos,kBottomBar.X+104,kBottomBar.Y,60,kBottomBar.Height,10.2f,rgb(0xFFFFFF),false,false,true);
        Pen sep(alpha(ink,70),1.f);
        g.DrawLine(&sep,215.f,418.f,215.f,436.f);
    }
    for(const int id:{cBrBankPrev,cBrBankNext,cBrFolder,cBrRefresh}) drawChrome(g,id,hoveredChrome_==id);
    if(writing_) for(const int id:{cBrWrite,cBrNewBank}) drawChrome(g,id,hoveredChrome_==id);
    else for(const int id:{cBrImport,cBrExport}) drawChrome(g,id,hoveredChrome_==id);
}

void DesktopEditor::setValueOnce(int id,int v) {
    if(!hooks_.paramInfo || !hooks_.beginEdit || !hooks_.setValue || !hooks_.endEdit) return;
    int low=0,high=127,def=0;
    hooks_.paramInfo(hooks_.ctx,id,&low,&high,&def);
    v=std::clamp(v,low,high);
    const bool changed=v!=value(id); // SynthModel.set: no change, no readout
    hooks_.beginEdit(hooks_.ctx,id);
    hooks_.setValue(hooks_.ctx,id,v);
    hooks_.endEdit(hooks_.ctx,id);
    valueChanged(id);
    if(changed) show(id);
}
void DesktopEditor::cycle(int id,int step) {
    int low=0,high=127,def=0;
    if(hooks_.paramInfo) hooks_.paramInfo(hooks_.ctx,id,&low,&high,&def);
    const int v=value(id)+step;
    setValueOnce(id,v>high?low:v<low?high:v);
}
// < > (SynthModel.step): the next / previous non-empty slot, rolling into
// the neighbouring banks (list order) at the ends.
void DesktopEditor::stepPatch(int direction) {
    if(banks_.empty()) refreshBanks();
    const int n=int(bankOrder_.size());
    if(n==0) return;
    int bank=-1,slot=-1;
    if(knownRevision_>0 && hooks_.loadedProgram) hooks_.loadedProgram(hooks_.ctx,&bank,&slot);
    int row=bankRowOf(bank);
    if(row<0 || row>=n) {
        row=std::clamp(bankRowOf(selectedBank_),0,n-1);
        slot=-1;
    }
    int p=slot>=0 ? slot : direction>0 ? -1 : int(banks_[size_t(bankOrder_[size_t(row)])].slots.size());
    for(int guard=0;guard<n*s2::kBankSlots+1;guard++) {
        const auto& slots=banks_[size_t(bankOrder_[size_t(row)])].slots;
        p+=direction>0?1:-1;
        if(p>=int(slots.size())) {row=(row+1)%n;p=-1;continue;}
        if(p<0) {row=(row-1+n)%n;p=int(banks_[size_t(bankOrder_[size_t(row)])].slots.size());continue;}
        if(!slots[size_t(p)].empty()) {loadSlot(bankOrder_[size_t(row)],p);return;}
    }
}

// The write dialog's Write (its button, Return / Enter, or the bottom bar's
// write clicked again: PatchBrowserView.commitWrite).
void DesktopEditor::commitWrite() {
    if(!browser_ || !writing_) return;
    const bool newUser=selectedBank_==kNewUserBank;
    if(selectedSlot_<0)return;
    if(!newUser && (selectedBank_<0 || selectedBank_>=int(banks_.size()) ||
                    banks_[size_t(selectedBank_)].readOnly))return;
    std::wstring name=patchName();
    if(const auto typed=nameFieldText()) name=*typed;
    // The proposed User bank is made now, by the write (newBank
    // rescans: its index is valid for the write right away).
    int bank=selectedBank_;
    if(newUser && (!hooks_.newBank || (bank=hooks_.newBank(hooks_.ctx,s2::kUserBankName))<0)) {
        messageBox(L"Cannot create the User bank. Check the library folder is writable.",L"Synth2",MB_ICONWARNING);
        return;
    }
    if(hooks_.writeLibraryPatch && hooks_.writeLibraryPatch(hooks_.ctx,bank,selectedSlot_,toUtf8(name).c_str())) {
        const int slot=selectedSlot_;
        closeBrowser();refreshBanks();
        if(bank<int(banks_.size())) { // SynthModel.write
            wchar_t where[24];
            std::swprintf(where,24,L"written to %02d:",displayBankNumber(bank));
            wchar_t number[8];std::swprintf(number,8,L" %03d",slot+1);
            flash(where+banks_[size_t(bank)].leaf+number);
        }
    } else messageBox(L"Cannot write that preset. Check the library folder is writable.",L"Synth2",MB_ICONWARNING);
}

// Keys (SynthModel.browserKey, fed by WM_KEYDOWN and IPlugView::onKeyDown):
// Esc cancels a drag / clears the search / closes the browser / leaves MIDI
// learn; with the browser open, up / down = previous / next patch (rolling
// into the neighbouring banks, as < >), left / right = the previous / next
// bank, Backspace edits the search, Return / Enter close; in the write
// dialog Return / Enter write. With Ctrl / Alt / Win held, and the arrows
// with the browser closed: the host's. False = not ours.
bool DesktopEditor::keyDown(UINT key) {
    dismissBankTip();
    if((GetKeyState(VK_CONTROL)|GetKeyState(VK_MENU)|GetKeyState(VK_LWIN)|GetKeyState(VK_RWIN))&0x8000) return false;
    if(key==VK_ESCAPE) {
        if(typeMenu_>=0) closeTypeMenu();
        else if(press_.active) dragEnd(false);
        else if(browser_ && !searchText_.empty() && !writing_) {searchText_.clear();slotScroll_=0;invalidate(kBrowserRect);}
        else if(browser_) closeBrowser();
        else if(midiLearn_) setLearn(false);
        else return false;
        return true;
    }
    if(!browser_) return false;
    if(key==VK_BACK) return searchKey(8);
    // Typed text goes to the search box (the characters arrive as WM_CHAR /
    // onKeyDown's char): the key itself stays with us, not the host's.
    if(!writing_ && ((key>=0x30 && key<=0x5A) || key==0x20 || (key>=0xBA && key<=0xDF))) return true;
    if(key==VK_RETURN) {
        if(writing_) commitWrite(); else closeBrowser();
        return true;
    }
    if(writing_ || (key!=VK_UP && key!=VK_DOWN && key!=VK_LEFT && key!=VK_RIGHT)) return false;
    if(key==VK_LEFT || key==VK_RIGHT) {stepBank(key==VK_RIGHT?1:-1);return true;}
    if(selectedBank_<0 || selectedBank_>=int(banks_.size())) return true;
    const auto& slots=banks_[size_t(selectedBank_)].slots;
    int loadedBank=-1,loadedSlot=-1;
    if(knownRevision_>0 && hooks_.loadedProgram) hooks_.loadedProgram(hooks_.ctx,&loadedBank,&loadedSlot);
    if(loadedBank!=selectedBank_ || loadedSlot<0) { // another bank shown: its first patch
        const auto first=std::find_if(slots.begin(),slots.end(),[](const std::string& s){return !s.empty();});
        if(first!=slots.end()) loadSlot(selectedBank_,int(first-slots.begin()));
        return true;
    }
    stepPatch(key==VK_DOWN?1:-1);
    return true;
}
tresult PLUGIN_API DesktopEditor::onKeyDown(char16 key,int16 keyCode,int16 modifiers) {
#if !defined(_WIN32)
    if(modal()) return kResultFalse; // a menu / dialog is open (the host runs on): the keys wait with the panel
#endif
    if(nameFieldKey(key,keyCode,modifiers)) return kResultTrue; // typing in the write dialog's name
    if(modifiers&(kAlternateKey|kCommandKey|kControlKey)) return kResultFalse;
    if(browser_ && !writing_ && key>=32 && key!=127) return searchKey(wchar_t(key)) ? kResultTrue : kResultFalse;
    UINT vk=0;
    switch(keyCode) {
    case KEY_UP: vk=VK_UP;break;       case KEY_DOWN: vk=VK_DOWN;break;
    case KEY_LEFT: vk=VK_LEFT;break;   case KEY_RIGHT: vk=VK_RIGHT;break;
    case KEY_RETURN: case KEY_ENTER: vk=VK_RETURN;break;
    case KEY_ESCAPE: vk=VK_ESCAPE;break;
    default: return kResultFalse;
    }
    return keyDown(vk) ? kResultTrue : kResultFalse;
}
// SynthModel.load: the browser follows the loaded patch.
void DesktopEditor::loadSlot(int bank,int slot) {
    if(!hooks_.loadLibraryPatch || !hooks_.loadLibraryPatch(hooks_.ctx,bank,slot)) return;
    if(bank!=selectedBank_) slotScroll_=0;
    selectedBank_=bank;selectedSlot_=slot;
    scrollBankIntoView(bank);
    const auto shown=shownSlots();
    const auto it=std::find(shown.begin(),shown.end(),slot);
    if(it!=shown.end()) {
        const int row=int(it-shown.begin());
        if(row<slotScroll_) slotScroll_=row; else if(row>=slotScroll_+kPresetRows) slotScroll_=row-kPresetRows+1;
    }
    repaint();
}

// The panel's own buttons under a point (design coordinates), 0 = none. The
// tab row and the header always; the bottom bar only with the browser open.
int DesktopEditor::chromeAt(float x,float y) const {
    std::vector<int> ids={cTabEdit,cTabPlay,cTabBrowser,cPrev,cNext,cSearch,cWrite,cMenu};
    if(midiLearn_ && !browser_) ids.push_back(cLearnExit);
    if(browser_) {
        for(const int id:{cBrBankPrev,cBrBankNext,cBrFolder,cBrRefresh}) ids.push_back(id);
        if(writing_) {ids.push_back(cBrWrite);ids.push_back(cBrNewBank);}
        else {ids.push_back(cBrImport);ids.push_back(cBrExport);}
    }
    for(const int id:ids) if(chromeRect(id).Contains(x,y)) return id;
    return 0;
}
void DesktopEditor::chromeClick(int id) {
    switch(id) {
    case cTabEdit: case cTabPlay:
        if(browser_) closeBrowser();
        setView(id==cTabEdit ? View::Edit : View::Play);
        return;
    case cTabBrowser: if(browser_) closeBrowser(); else openBrowser(false);return;
    case cPrev: stepPatch(-1);return;
    case cNext: stepPatch(1);return;
    case cSearch: if(!browser_) openBrowser(false);return;
    case cWrite: if(browser_ && writing_) commitWrite(); else openBrowser(true);return;
    case cMenu: {POINT p;GetCursorPos(&p);showMainMenu(p.x,p.y);return;}
    case cLearnExit: setLearn(false);return;
    case cBrBankPrev: stepBank(-1);return;
    case cBrBankNext: stepBank(1);return;
    case cBrWrite: commitWrite();return;
    case cBrNewBank: {
        // SynthModel.newBank: a uniquely named folder, then write into it.
        if(!hooks_.newBank) return;
        askText(L"New bank",L"Bank name:",L"New Bank",[this](const std::wstring& name) {
            const int bank=hooks_.newBank(hooks_.ctx,toUtf8(name).c_str());
            if(bank<0) {
                messageBox(L"Cannot create that bank. Check the library folder is writable.",L"Synth2",MB_ICONWARNING);
                return;
            }
            if(hooks_.libraryRevision) knownRevision_=hooks_.libraryRevision(hooks_.ctx);
            refreshBanks();
            if(bank<int(banks_.size())) {selectedBank_=bank;selectedSlot_=0;slotScroll_=0;}
            repaint();
        });
        return;
    }
    case cBrImport: {POINT p;GetCursorPos(&p);showImportDialog(p.x,p.y);return;}
    case cBrFolder:
        if(hooks_.libraryRoot) {
            auto path=toWide(hooks_.libraryRoot(hooks_.ctx));
            ShellExecuteW(window_,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        }
        return;
    case cBrRefresh:
        if(hooks_.startLibraryScan) hooks_.startLibraryScan(hooks_.ctx);
        else if(hooks_.rescanLibrary) hooks_.rescanLibrary(hooks_.ctx);
        refreshBanks();return;
    case cBrExport: {
        // The shown bank, as its context menu's Export bank...
        if(selectedBank_<0 || selectedBank_>=int(banks_.size())) {flash(L"select a bank to export",2500);return;}
        const std::string source=banks_[size_t(selectedBank_)].source;
        pickFolder(L"Export bank: a folder holding its patches is created in the folder you choose",
                   [this,source](const std::wstring& dir) {
            const int bank=bankNow(source);
            const std::string made=bank>=0 && hooks_.exportBank ? hooks_.exportBank(hooks_.ctx,bank,toUtf8(dir).c_str()) : "";
            if(made.empty()) messageBox(L"Unable to export this bank.",L"Synth2",MB_ICONWARNING);
            else {
                flash(L"bank exported as "+std::filesystem::path(toWide(made.c_str())).filename().wstring(),4000);
                revealInExplorer(made);
            }
        });
        return;
    }
    default: return;
    }
}

void DesktopEditor::browserClick(float x,float y,bool doubleClick) {
    if(x>=kBankPanel.X && x<kBankPanel.X+kBankPanel.Width && y>=kBankTop-2) {
        const int bank=bankRowAt(x,y);
        if(bank==kNewUserBank ||
           (bank>=0 && bank<int(banks_.size()) && !(writing_ && banks_[size_t(bank)].readOnly))) {
            selectedBank_=bank;selectedSlot_=-1;slotScroll_=0;
            if(bank==kNewUserBank) selectedSlot_=0;
            if(writing_ && bank>=0) {
                const auto& slots=banks_[size_t(bank)].slots;
                auto it=std::find(slots.begin(),slots.end(),"");
                selectedSlot_=it==slots.end()?0:int(it-slots.begin());
            }
        }
        repaint();return;
    }
    if(!writing_ && !searchText_.empty() && kSearchBox.Contains(x,y) && x>=kSearchBox.X+kSearchBox.Width-24.f) {
        searchText_.clear();slotScroll_=0;repaint();return; // the box's right end clears it
    }
    if(const int index=slotAt(x,y); index>=0) {
        // A patch may be dragged: its click acts on release (as a tap),
        // unless it turned into a drag. Empty slots act at once.
        const bool patch=selectedBank_>=0 && !banks_[size_t(selectedBank_)].slots[size_t(index)].empty();
        if(doubleClick || !patch) {slotClick(index,doubleClick);return;}
        press_={true,false,selectedBank_,index,banks_[size_t(selectedBank_)].source,{},x,y,
                slotText(selectedBank_,index)};
        GetCursorPos(&press_.start);
        SetCapture(window_);
    }
}
// A click on a slot of the shown bank: the write dialog picks it, the
// browser loads it (a double click also closes).
void DesktopEditor::slotClick(int index,bool doubleClick) {
    selectedSlot_=index;
    const bool valid=selectedBank_>=0 && selectedBank_<int(banks_.size()) &&
        index<int(banks_[size_t(selectedBank_)].slots.size()) && !banks_[size_t(selectedBank_)].slots[size_t(index)].empty();
    if(!writing_ && valid && hooks_.loadLibraryPatch) {
        hooks_.loadLibraryPatch(hooks_.ctx,selectedBank_,index);
        if(doubleClick)closeBrowser();
    }
    repaint();
}

// Browser drag and drop (PatchBrowserView's SlotDrop / BankDrop), inside
// our own window: a patch dragged onto another slot of its bank MOVES there
// (an occupied one swaps; the files are renamed), onto a bank row COPIES
// into that bank's first free slot ("+" cursor; the browser stays on the
// bank shown). Never into the read-only factory bank (its patches can be
// copied out); no bank drop in the write dialog.
void DesktopEditor::dragMove(float x,float y) {
    if(!press_.active) return;
    if(!press_.dragging) {
        POINT now{};GetCursorPos(&now);
        if(std::abs(now.x-press_.start.x)<GetSystemMetrics(SM_CXDRAG) &&
           std::abs(now.y-press_.start.y)<GetSystemMetrics(SM_CYDRAG)) return;
        press_.dragging=true;
    }
    invalidate(dragLabelRect());
    press_.x=x;press_.y=y;
    int slot=-1,bank=-1,full=-1;
    const bool sameBank=press_.bank==selectedBank_ && press_.bank>=0 && press_.bank<int(banks_.size());
    if(const int s=slotAt(x,y); s>=0 && sameBank && !banks_[size_t(press_.bank)].readOnly) slot=s;
    else if(const int b=bankRowAt(x,y); b>=0 && !writing_ && !banks_[size_t(b)].readOnly) {
        // a full bank refuses the copy: forbidden cursor, said once on entering
        const auto& slots=banks_[size_t(b)].slots;
        if(std::any_of(slots.begin(),slots.end(),[](const std::string& s){return s.empty();})) bank=b;
        else full=b;
    }
    if(full>=0 && full!=dragFullBank_) flash(L"Bank is full (128 patches): cannot add a patch",3000);
    dragFullBank_=full;
    if(slot!=dropSlot_ || bank!=dropBank_) {
        dropSlot_=slot;dropBank_=bank;
        invalidate(kBrowserRect);
    }
    invalidate(dragLabelRect());
    SetCursor(dragCursor());
}
void DesktopEditor::dragEnd(bool drop) {
    if(!press_.active) return;
    const SlotPress press=press_;
    const int toSlot=dropSlot_, toBank=dropBank_;
    press_.active=false;dropSlot_=dropBank_=dragFullBank_=-1;
    if(GetCapture()==window_) ReleaseCapture();
    invalidate(kBrowserRect);
    if(!press.dragging) { // a plain click
        if(drop && press.bank==selectedBank_) slotClick(press.slot,false);
        return;
    }
    if(!drop) return;
    // the indices as of now (a scan may have landed during the drag)
    const int from=bankNow(press.source);
    if(from<0) return;
    if(toSlot>=0 && toSlot!=press.slot) { // SynthModel.movePatch
        if(!hooks_.movePatch || !hooks_.movePatch(hooks_.ctx,from,press.slot,toSlot)) return;
        adoptLibrary();
        wchar_t moved[32];std::swprintf(moved,32,L"moved %03d -> %03d",press.slot+1,toSlot+1);
        flash(moved);
    } else if(toBank>=0 && toBank<int(banks_.size()) && hooks_.copyPatch) { // SynthModel.copyPatch
        const std::string target=banks_[size_t(toBank)].source;
        const int to=bankBySource(target);
        if(to<0) return;
        const std::wstring name=banks_[size_t(to)].leaf;
        const int number=displayBankNumber(to);
        const int s=hooks_.copyPatch(hooks_.ctx,from,press.slot,to);
        adoptLibrary();
        if(s>=0) {
            wchar_t where[48];std::swprintf(where,48,L"copied to %02d:",number);
            wchar_t at[8];std::swprintf(at,8,L" %03d",s+1);
            flash(where+name+at,3000);
        } else flash(s==-1 ? L"Bank is full (128 patches): cannot add a patch" : L"copy failed",s==-1?5000:1500);
    }
}
// The dragged patch's label follows the pointer (the mac's drag preview).
RectF DesktopEditor::dragLabelRect() const {
    return RectF(press_.x+10.f,press_.y+6.f,140.f,18.f);
}
HCURSOR DesktopEditor::dragCursor() {
    if(dropBank_>=0) return copyCursor();
    return LoadCursorW(nullptr,dropSlot_>=0 || dropBank_>=0 ? IDC_ARROW : IDC_NO);
}

void DesktopEditor::bankTipHover(float x,float y) {
    const int bank=browser_ && !press_.active && typeMenu_<0 ? bankRowAt(x,y) : -1;
    if(bank==bankTip_.bank) return; // the same row: the tip stays where it opened
    hideBankTip();
    bankTip_=BankTip{};
    bankTip_.bank=bank;
    if(bank<0 || bank>=int(banks_.size()) || !textCache_) return;
    const Bank& b=banks_[size_t(bank)];
    const bool nested=b.leaf!=b.wideName;
    if(!nested && !textCache_->get(b.leaf,textFontSize(kBankNameSize),kBankNameW-2.f,true).cut) return;
    wchar_t prefix[12];std::swprintf(prefix,12,L"%02d:",displayBankNumber(bank));
    bankTip_.text=prefix+b.wideName;
    bankTip_.x=x;bankTip_.y=y;
    const uint64_t now=GetTickCount64();
    if(now<bankTipWarmUntil_) {bankTip_.shown=true;invalidate(bankTipRect());}
    else bankTip_.dueAt=now+500;
}
void DesktopEditor::hideBankTip() {
    bankTip_.dueAt=0;
    if(!bankTip_.shown) return;
    invalidate(bankTipRect());
    bankTip_.shown=false;
    bankTipWarmUntil_=GetTickCount64()+5000;
}
void DesktopEditor::dismissBankTip() {
    hideBankTip();
    bankTip_.text.clear(); // bank kept: quiet until the pointer leaves the row
}
void DesktopEditor::resetBankTip() {
    hideBankTip();
    bankTip_=BankTip{};
}
// Below-right of the pointer, kept inside the panel (above the pointer
// when there is no room below); the shadow's margin included.
RectF DesktopEditor::bankTipRect() const {
    if(bankTip_.text.empty() || !textCache_) return RectF();
    const float w=textCache_->get(bankTip_.text,textFontSize(kTipTextSize),kDesignWidth,false).ink.Width+2*kTipPad+2;
    const float x=std::max(kTipMargin,std::min(bankTip_.x+10.f,kDesignWidth-w-kTipMargin));
    float y=bankTip_.y+18.f;
    if(y+kTipH>kDesignHeight-kTipMargin) y=bankTip_.y-kTipH-6.f;
    return RectF(x-3.f,std::max(kTipMargin,y)-3.f,w+6.f,kTipH+8.f);
}
// SynthTooltip's bubble: the LCD's colours, a black edge, a soft shadow.
void DesktopEditor::drawBankTip(Graphics& g) {
    if(!bankTip_.shown) return;
    RectF r=bankTipRect();
    if(r.Width<=0 || !visible(r)) return;
    r=RectF(r.X+3.f,r.Y+3.f,r.Width-6.f,kTipH);
    for(int i=3;i>=1;--i) {
        GraphicsPath shade;roundedPath(shade,r.X-float(i)+1.f,r.Y+1.f,r.Width+2.f*float(i)-2.f,r.Height+float(i)+1.f,4.f+float(i));
        SolidBrush brush(Color(BYTE(18*(4-i)),0,0,0));
        g.FillPath(&brush,&shade);
    }
    GraphicsPath box;roundedPath(box,r.X,r.Y,r.Width,r.Height,4.f);
    SolidBrush face(colors_[kDisplayBottom]);
    g.FillPath(&face,&box);
    roundedOutline(g,Color(204,0,0,0),r.X+.5f,r.Y+.5f,r.Width-1.f,r.Height-1.f,4.f);
    text(g,bankTip_.text,r.X+kTipPad-1.f,r.Y,r.Width-2*kTipPad+2.f,r.Height,kTipTextSize,rgb(kLcdText));
}
void DesktopEditor::mouseLeft() {
    hoverChanged(-1,0);
    if(typeMenuHover_>=0) {typeMenuHover_=-1;invalidateTypeMenu();}
    bankTipHover(-1.f,-1.f);
}

// Hover uses the same design-space hit rectangles as clicks. It only
// invalidates the window when the hovered target changes; idle redraws remain off.
void DesktopEditor::updateHover(float x,float y) {
    bankTipHover(x,y);
    if(typeMenu_>=0) { // the open pick list covers the panel (the mac's overlay)
        const int v=typeMenuHit(x,y);
        if(v!=typeMenuHover_) {typeMenuHover_=v;invalidateTypeMenu();}
        hoverChanged(-1,0);
        return;
    }
    int nextControl=-1;
    int nextChrome=chromeAt(x,y);
    const bool overBrowser=browser_ && kBrowserRect.Contains(x,y);
    if(nextChrome==0 && !overBrowser && !(midiLearn_ && false)) {
        if(!midiLearn_) { // learn mode: the hotspots take the clicks
            // Match mouseDown's reverse draw order so overlapping hit areas
            // always highlight the control that will receive the click.
            for(size_t i=controls_.size();i>0;--i) {
                const auto& c=controls_[i-1];
                // Highlight clickable buttons only, not waves, LEDs or LCD displays.
                if(c.id<0 || (c.kind!=Kind::Toggle && c.kind!=Kind::Cycle && c.kind!=Kind::Badge)) continue;
                if(x>=c.x && x<c.x+c.w && y>=c.y && y<c.y+c.h) {nextControl=int(i-1);break;}
            }
        }
    }
    hoverChanged(nextControl,nextChrome);
}

void DesktopEditor::mouseDown(float x,float y,bool doubleClick) {
    dismissBankTip();
    if(typeMenu_>=0) { // a pick, or a click anywhere else closes it
        const int id=typeMenu_, v=typeMenuHit(x,y);
        closeTypeMenu();
        if(v>=0) {setValueOnce(id,v);show(id);}
        return;
    }
    if(x>=kDesignWidth-22.f && y>=kDesignHeight-22.f && !doubleClick) {
        if(!plugFrame)return;
        GetCursorPos(&resizeStartScreen_);
        resizeStartWidth_=rect.getWidth();
        resizeDragging_=true;
        SetCapture(window_);
        return;
    }
    if(const int chrome=chromeAt(x,y)) {chromeClick(chrome);return;}
    // The LCD opens / closes the browser; the logo opens the theme menu.
    if(kLcdRect.Contains(x,y)) {if(browser_) closeBrowser(); else openBrowser(false);return;}
    if(y<29.f && x<105.f) {
        POINT p;GetCursorPos(&p);showThemeMenu(p.x,p.y);return;
    }
    // Inside the browser's panel only the browser takes the mouse.
    if(browser_ && kBrowserRect.Contains(x,y)) {browserClick(x,y,doubleClick);return;}
    // Learn mode: a hotspot takes the click (the controls under it stop
    // taking input); the others (volume, ctrl src / assign menus) still work.
    if(const int id=midiLearn_ ? learnHit(x,y) : -1; id>=0) {
        if(doubleClick) {
            // The first click of the pair armed it: the mac's double tap
            // leaves the selection as it was.
            if(learnCC_[size_t(id)] && hooks_.setMidiCC) {
                hooks_.setMidiCC(hooks_.ctx,id,0);
                flash(L"MIDI LEARN: "+toWide(hooks_.paramName ? hooks_.paramName(hooks_.ctx,id) : "")+L" cleared");
            }
            learnCC_[size_t(id)]=0;
            learnSelect(learnPrevTarget_);
            invalidate(learnRects_[size_t(id)]);
        } else {
            learnPrevTarget_=learnTarget_;
            learnSelect(id);
        }
        return;
    }
    // The filter graph: drag the curve (across = cutoff, up = resonance).
    if(!midiLearn_ && view_==View::Edit && !browser_ && kGraphRect.Contains(x,y)) {
        if(doubleClick) {
            for(const int id:{19,20}) {
                int a=0,b=127,d=0;
                if(hooks_.paramInfo) hooks_.paramInfo(hooks_.ctx,id,&a,&b,&d);
                setValueOnce(id,d);
            }
            return;
        }
        dragGraph_=true;
        if(hooks_.beginEdit) {hooks_.beginEdit(hooks_.ctx,19);hooks_.beginEdit(hooks_.ctx,20);}
        SetCapture(window_);
        graphTo(x,y);
        return;
    }
    for(auto it=controls_.rbegin();it!=controls_.rend();++it) {
        const auto& c=*it;
        if(x<c.x || x>=c.x+c.w || y<c.y || y>=c.y+c.h)continue;
        if(c.id<0)continue;
        // WM_LBUTTONDBLCLK is delivered for the SECOND rapid click.
        // Only knobs and non-cycling value displays reset on a double click,
        // exactly as in SwiftUI. Cycle buttons, LEDs, waveform selectors and
        // toggles must treat every fast click as another normal selection.
        const bool doubleTapResets = c.kind==Kind::Knob || c.kind==Kind::Fader ||
            (c.kind==Kind::Display && c.id!=64 && c.id!=78 && c.id!=82);
        if(doubleClick && doubleTapResets) {
            int a=0,b=127,d=0;
            if(hooks_.paramInfo) hooks_.paramInfo(hooks_.ctx,c.id,&a,&b,&d);
            setValueOnce(c.id,d);return;
        }
        switch(c.kind) {
        case Kind::Knob:
        case Kind::Display:
            if(c.kind==Kind::Display && (c.id==64 || c.id==78 || c.id==82)) {openTypeMenu(c.id);return;}
            if(c.kind==Kind::Display && c.id>=86 && c.id<=89) {
                POINT screen;GetCursorPos(&screen);
                showParameterMenu(c.id,screen.x,screen.y);return;
            }
            dragId_=c.id;dragStartY_=y;dragStartValue_=value(c.id);
            if(hooks_.beginEdit)hooks_.beginEdit(hooks_.ctx,c.id);
            SetCapture(window_);return;
        case Kind::Fader:
            // The cap goes to the pointer, then follows it (vertical, whole track).
            dragId_=c.id;dragFader_=true;dragFaderTop_=c.y+5.f;dragFaderLen_=c.h-10.f;
            if(hooks_.beginEdit)hooks_.beginEdit(hooks_.ctx,c.id);
            SetCapture(window_);
            dragTo(y);
            return;
        case Kind::Toggle:
        case Kind::Badge: setValueOnce(c.id,value(c.id)?0:1);return;
        case Kind::Cycle: cycle(c.id,1);return;
        case Kind::Wave:
            if(c.id==42 || c.id==47) openTypeMenu(c.id);
            else setValueOnce(c.id,c.value);
            return;
        case Kind::Radio: setValueOnce(c.id,c.value);return;
        default:return;
        }
    }
}
// A fader's value for a pointer height (its whole track).
void DesktopEditor::dragTo(float y) {
    int low=0,high=127,def=0;
    if(hooks_.paramInfo) hooks_.paramInfo(hooks_.ctx,dragId_,&low,&high,&def);
    const float frac=std::clamp(1.f-(y-dragFaderTop_)/std::max(1.f,dragFaderLen_),0.f,1.f);
    const int v=low+int(std::lround(frac*float(high-low)));
    if(v==value(dragId_)) return;
    if(hooks_.setValue) hooks_.setValue(hooks_.ctx,dragId_,v);
    valueChanged(dragId_);
    show(dragId_);
}
// The filter graph's plot: across = cutoff (its axis is the knob's own
// exponential scale), up = resonance.
void DesktopEditor::graphTo(float x,float y) {
    const float px=503.f,py=84.f,pw=163.f,ph=57.f;
    const int cutoff=std::clamp(int(std::lround((x-px)/pw*127.f)),0,127);
    const int res=std::clamp(int(std::lround((1.f-(y-py)/ph)*127.f)),0,127);
    for(const auto& [id,v]:{std::pair<int,int>{19,cutoff},{20,res}}) {
        if(v==value(id)) continue;
        if(hooks_.setValue) hooks_.setValue(hooks_.ctx,id,v);
        valueChanged(id);
    }
    show(19);
}
void DesktopEditor::mouseMove(float x, float y) {
    updateHover(x,y);
    if(press_.active) {dragMove(x,y);return;}
    if(resizeDragging_) {
        POINT current{};GetCursorPos(&current);
        requestWidth(resizeStartWidth_+(current.x-resizeStartScreen_.x));
        return;
    }
    if(dragGraph_) {graphTo(x,y);return;}
    if(dragFader_) {dragTo(y);return;}
    if(dragId_<0)return;
    int low=0,high=127,def=0;
    if(hooks_.paramInfo) hooks_.paramInfo(hooks_.ctx,dragId_,&low,&high,&def);
    const int span=std::max(1,high-low);
    const bool fine=(GetKeyState(VK_SHIFT)&0x8000)!=0;
    const float pixels=(fine?900.f:180.f);
    int newValue=std::clamp(dragStartValue_+int(std::lround((dragStartY_-y)*span/pixels)),low,high);
    if(newValue==value(dragId_)) return;
    if(hooks_.setValue)hooks_.setValue(hooks_.ctx,dragId_,newValue);
    valueChanged(dragId_);
    show(dragId_);
}
void DesktopEditor::mouseUp() {
    if(press_.active) {dragEnd(true);return;}
    if(resizeDragging_) {
        resizeDragging_=false;
        requestedZoomWidth_=-1;
        pendingSizeSave_=-1;
        // Save just once on mouse-up rather than writing on every drag frame.
        winzoom::SaveWidth(storage::Root(),rect.getWidth(),kMinWidth,kMaxWidth);
        if(GetCapture()==window_)ReleaseCapture();
        return;
    }
    if(dragGraph_) {
        dragGraph_=false;
        if(hooks_.endEdit) {hooks_.endEdit(hooks_.ctx,19);hooks_.endEdit(hooks_.ctx,20);}
        if(GetCapture()==window_)ReleaseCapture();
        return;
    }
    if(dragId_<0)return;
    if(hooks_.endEdit)hooks_.endEdit(hooks_.ctx,dragId_);
    dragId_=-1;dragFader_=false;
    if(GetCapture()==window_)ReleaseCapture();
}
// What a wheel event at (x, y) moves: a parameter id, kWheelBanks /
// kWheelSlots (the browser's lists), or -1 (nothing: buttons, the ctrl src
// / assign menus, the vertical LED lists, an open pick list, learn mode).
constexpr int kWheelBanks=-2, kWheelSlots=-3;
int DesktopEditor::wheelTarget(float x,float y) const {
    // Scrolling over the master volume must change volume, not the browser.
    if(browser_ && kBrowserRect.Contains(x,y)) {
        if(kBankPanel.Contains(x,y)) return kWheelBanks;
        return kPresetPanel.Contains(x,y) ? kWheelSlots : -1;
    }
    if(midiLearn_) return -1;
    if(typeMenu_>=0 && typeMenu().rect().Contains(x,y)) return -1; // the mac's WheelShield
    for(auto it=controls_.rbegin();it!=controls_.rend();++it) {
        const auto& c=*it;
        if(c.id<0 || x<c.x || x>c.x+c.w || y<c.y || y>c.y+c.h) continue;
        // Buttons are click-only. Do not change toggles, numbered buttons or
        // cycling buttons when the user scrolls over them.
        if(c.kind==Kind::Toggle || c.kind==Kind::Cycle ||
           c.kind==Kind::Badge || c.kind==Kind::Label) return -1;
        // The MIDI source / assign menus and the vertical LED lists ignore
        // the wheel; everything else steps by one, up = value up, clamped.
        if(c.kind==Kind::Display && c.id>=86 && c.id<=89) return -1;
        if(c.kind==Kind::Radio && c.id!=71 && c.id!=97) return -1;
        return c.id;
    }
    return -1;
}
void DesktopEditor::wheel(int direction,float x,float y) {
    if(direction==0) return;
    dismissBankTip(); // the list moves under it
    // A RUN of wheel events (< kWheelRunMs apart) belongs to what was under
    // the cursor at its first one (the mac's WheelMonitor momentum owner):
    // touchpad / free-spin inertia carries no momentum flag on Windows, so
    // without this its tail moved the next knob under a drifting cursor.
    // Events over anything else are dropped until the run ends.
    const int target=wheelTarget(x,y);
    const uint64_t now=GetTickCount64();
    if(now-wheelLastAt_>=kWheelRunMs) wheelOwner_=target;
    wheelLastAt_=now;
    if(target!=wheelOwner_ || target==-1) return;
    if(target==kWheelBanks) {
        // Wheel input is expressed as rows, not merely the sign of one
        // notch. A standard wheel step now moves eight bank entries.
        const int next=std::clamp(bankScroll_-direction,0,std::max(0,bankRowCount()-kBankRows));
        if(next!=bankScroll_) {bankScroll_=next;invalidate(kBrowserRect);}
    } else if(target==kWheelSlots) {
        const int total=int(shownSlots().size());
        const int amount=(direction>0?1:-1)*std::max(1,(std::abs(direction)+1)/2)*3;
        const int next=std::clamp(slotScroll_-amount,0,std::max(0,total-kPresetRows));
        if(next!=slotScroll_) {slotScroll_=next;invalidate(kBrowserRect);}
    } else {
        // Menu > Invert mouse wheel: value controls only (the lists above
        // scroll like any page of the desktop).
        const bool invert=hooks_.invertWheel && hooks_.invertWheel(hooks_.ctx)!=0;
        const int current=value(target);
        const int next=s2::OfferedStep(target,current,(direction>0)!=invert?1:-1); // skips chorus type 3
        if(next!=current) setValueOnce(target,next);
    }
}

// EngineCore/PatchLibrary.hpp `ImportPayload`: "patches=N", then
// "+<count> <folder>" per bank created, then "!<path>: <reason>" per
// refusal. The old shape was a bare folder list, so a successful import of
// loose .sy1 files, a full Imported bank and an unsupported pick all looked
// like "nothing happened".
struct ImportPayload {
    int patches = 0;
    int banks = 0;
    std::vector<std::string> refused;

    bool Empty() const { return patches == 0 && banks == 0; }
};

inline int LeadingNumber(const std::string& s, size_t from) {
    int n = 0;
    for(size_t i = from; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) n = n * 10 + (s[i] - '0');
    return n;
}

inline ImportPayload ParseImportPayload(const std::string& payload) {
    ImportPayload out;
    size_t at = 0;
    while(at < payload.size()) {
        size_t end = payload.find('\n', at);
        if(end == std::string::npos) end = payload.size();
        const std::string line = payload.substr(at, end - at);
        at = end + 1;
        if(line.rfind("patches=", 0) == 0) out.patches = LeadingNumber(line, 8);
        else if(!line.empty() && line[0] == '+') ++out.banks;
        else if(!line.empty() && line[0] == '!') out.refused.push_back(line.substr(1));
        else if(!line.empty()) ++out.banks; // an older host: a bare folder list
    }
    return out;
}

void DesktopEditor::showImportDialog(int screenX,int screenY) {
    HMENU menu=CreatePopupMenu();
    AppendMenuW(menu,MF_STRING,1,L"Import .sy1 or ZIP bank...");
    AppendMenuW(menu,MF_STRING,2,L"Import folder...");
    popupMenu(menu,screenX,screenY,[this](int selected) {
        if(!selected || !hooks_.importItems)return;
        auto import=[this](const std::wstring& path) {
            const std::string bytes=toUtf8(path);
            const char* imported=hooks_.importItems(hooks_.ctx,bytes.c_str());
            const ImportPayload out=ParseImportPayload(imported ? imported : "");
            if(out.Empty()) {
                // The reasons matter here: "already in the library" and "no
                // .sy1 inside" need different replies from the user, and the
                // old build said only "No new preset bank was imported".
                std::wstring why=L"Already in the library, not a bank folder / zip / .sy1, or no .sy1 presets inside.";
                if(!out.refused.empty()) why=toWide(out.refused.front().c_str());
                messageBox(L"Nothing was imported.\n\n"+why,L"Synth2",MB_ICONINFORMATION);
            } else {
                std::wstring text=L"imported "+std::to_wstring(out.patches)+
                                  (out.patches==1?L" patch":L" patches")+L" into "+
                                  std::to_wstring(out.banks)+(out.banks==1?L" bank":L" banks");
                if(!out.refused.empty()) text+=L" ("+std::to_wstring(out.refused.size())+L" skipped)";
                flash(text);
            }
            refreshBanks();
            repaint();
        };
        if(selected==1) chooseFile(FileDialog::ImportFile,import);
        else pickFolder(L"Choose a folder containing Synth1 .sy1 presets",import);
    });
}

void DesktopEditor::showFileDialog(bool save) {
    chooseFile(save?FileDialog::SavePatch:FileDialog::OpenPatch,[this,save](const std::wstring& filename) {
        const std::string path=toUtf8(filename);
        if(save) {
            if(hooks_.savePatchFile && !hooks_.savePatchFile(hooks_.ctx,path.c_str()))
                messageBox(L"Unable to save this patch.",L"Synth2",MB_ICONWARNING);
        } else {
            if(hooks_.loadPatchFile && hooks_.loadPatchFile(hooks_.ctx,path.c_str())) {
                knownSerial_=-1;
                refreshBanks();
            } else messageBox(L"Unable to load this .sy1 patch.",L"Synth2",MB_ICONWARNING);
        }
    });
}

// UI sizes: commands 120.. (the logo menu, menu > Select size).
static constexpr int kSizeLevels[]={75,100,125,150,200,250,300};
constexpr int kSizeCommand=120,kSizeCount=int(sizeof(kSizeLevels)/sizeof(kSizeLevels[0]));
void DesktopEditor::appendSizes(HMENU menu,const wchar_t* suffix) const {
    const int current=int(std::lround(100.f*float(rect.getWidth())/kDesignWidth));
    for(int i=0;i<kSizeCount;i++)
        AppendMenuW(menu,MF_STRING|(current==kSizeLevels[i] ? MF_CHECKED : 0),UINT_PTR(kSizeCommand+i),
                    (std::to_wstring(kSizeLevels[i])+L"%"+suffix).c_str());
}
bool DesktopEditor::pickSize(int command) {
    if(command<kSizeCommand || command>=kSizeCommand+kSizeCount) return false;
    requestWidth(int(std::lround(kDesignWidth*kSizeLevels[command-kSizeCommand]/100.f)));
    return true;
}
void DesktopEditor::showThemeMenu(int x,int y) {
    HMENU m=CreatePopupMenu();
    appendSizes(m,L" size");
    AppendMenuW(m,MF_SEPARATOR,0,nullptr);
    std::vector<themes::Palette> picks;
    appendThemes(m,picks);
    popupMenu(m,x,y,[this,picks](int choice) {if(!pickTheme(choice,picks)) pickSize(choice);});
}
void DesktopEditor::requestWidth(int width) {
    if(!plugFrame)return;
    width=std::clamp(width,kMinWidth,kMaxWidth);
    ViewRect r(0,0,width,0);
    checkSizeConstraint(&r);
    requestedZoomWidth_=r.getWidth();
    if(plugFrame->resizeView(this,&r)!=kResultOk)
        requestedZoomWidth_=-1;
}
void DesktopEditor::showMainMenu(int x,int y) {
    HMENU menu=CreatePopupMenu();
    AppendMenuW(menu,MF_STRING,10,L"Initialize patch");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(menu,MF_STRING,11,L"Load patch file...");
    AppendMenuW(menu,MF_STRING,12,L"Save patch file...");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(menu,MF_STRING,13,midiLearn_?L"Exit MIDI Learn":L"MIDI Learn");
    const bool programChange=hooks_.programChange && hooks_.programChange(hooks_.ctx)!=0;
    AppendMenuW(menu,MF_STRING|(programChange?MF_CHECKED:0),17,L"Program change");
    AppendMenuW(menu,MF_STRING,14,L"All notes off");
    HMENU sizeMenu=CreatePopupMenu(),themeMenu=CreatePopupMenu();
    appendSizes(sizeMenu,L"");
    std::vector<themes::Palette> picks;
    appendThemes(themeMenu,picks);
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(menu,MF_POPUP,UINT_PTR(sizeMenu),L"Select size");
    AppendMenuW(menu,MF_POPUP,UINT_PTR(themeMenu),L"Select theme");
    AppendMenuW(menu,MF_STRING,15,L"Themes...");
    const bool invertWheel=hooks_.invertWheel && hooks_.invertWheel(hooks_.ctx)!=0;
    AppendMenuW(menu,MF_STRING|(invertWheel?MF_CHECKED:0),18,L"Invert mouse wheel");
    // UpdateCheck.enabled: no product slug (forks) = no entry
    if(*OXO_UPDATE_PRODUCT) {
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING|(updateCheck_.Running()?MF_GRAYED:0),16,L"Check for updates...");
    }
    popupMenu(menu,x,y,[this,programChange,invertWheel,picks](int selected) {
        switch(selected) {
            case 10:
                if(hooks_.initializePatch)hooks_.initializePatch(hooks_.ctx);
                break;
            case 11: showFileDialog(false);break;
            case 12: showFileDialog(true);break;
            case 13: setLearn(!midiLearn_);break;
            case 14: if(hooks_.allNotesOff) {hooks_.allNotesOff(hooks_.ctx);flash(L"all notes off");} break;
            case 17: // SynthModel.programChange
                if(hooks_.setProgramChange) {
                    hooks_.setProgramChange(hooks_.ctx,programChange?0:1);
                    flash(programChange?L"program change: off":L"program change: on");
                }
                break;
            case 15: showThemeEditor();break;
            case 18: // SynthModel.invertWheel
                if(hooks_.setInvertWheel) {
                    hooks_.setInvertWheel(hooks_.ctx,invertWheel?0:1);
                    flash(invertWheel?L"invert mouse wheel: off":L"invert mouse wheel: on");
                }
                break;
            case 16: checkForUpdates();break;
            default: if(!pickTheme(selected,picks)) pickSize(selected);break;
        }
        repaint();
    });
}

// SynthModel.checkForUpdates on Windows: the LCD says "Checking…" while
// releases.json comes in the background, then a message box answers (the
// mac's UpdateDialog texts, EditorCore/UpdateCheck.hpp). Closing the
// editor cancels it.
void DesktopEditor::checkForUpdates() {
    const std::string site=OXO_VENDOR_URL;
    if(!window_ || !startUpdateCheck(site+"/releases.json")) return;
    flash(toWide(update::Message({update::Kind::Checking,{},{},{}},update::SiteName(site),"").c_str()),30000);
}
void DesktopEditor::updateArrived() {
    const std::string site=OXO_VENDOR_URL;
    const std::optional<std::string> body=updateCheck_.Take();
    const update::State state=body ? update::Evaluate(*body,OXO_UPDATE_PRODUCT,kUpdatePlatform,OXO_VERSION,
                                                       std::atoi(OXO_BUILD_NUMBER),site)
                                   : update::State{};
    flash({});
    std::wstring text=toWide(update::Message(state,update::SiteName(site),update::Email(OXO_VENDOR_EMAIL)).c_str());
    if(state.kind==update::Kind::Available) {
        text+=L"\n\nOpen the download page?";
        messageBox(text,L"Check for updates",MB_YESNO|MB_ICONINFORMATION,[this,page=toWide(state.page.c_str())](int answer) {
            if(answer==IDYES) ShellExecuteW(window_,L"open",page.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        });
    } else {
        messageBox(text,L"Check for updates",MB_OK|(state.kind==update::Kind::Latest?MB_ICONINFORMATION:MB_ICONWARNING));
    }
}
void DesktopEditor::showParameterMenu(int id,int x,int y) {
    HMENU menu=CreatePopupMenu();
    std::vector<int> values;
    const int current=value(id);
    // MIDI ctrl source / assign: native popup menus (reliable keyboard and
    // mouse operation inside VST3 hosts) with the mac's items
    // (SynthPanelView.ctrlSrc / ctrlAssign). A check marks the current value
    // (and its CC sub-menu).
    auto add=[&](HMENU target,const std::wstring& name,int v) {
        const UINT flags=MF_STRING|(current==v ? MF_CHECKED : 0);
        AppendMenuW(target,flags,UINT_PTR(2000+values.size()),name.c_str());
        values.push_back(v);
    };
    if(id==86 || id==88) {
        add(menu,L"pitch bend",0xE000);add(menu,L"ch pressure",0xD000);
        for(int base=0;base<128;base+=32) {
            HMENU sub=CreatePopupMenu();
            for(int cc=base;cc<base+32;cc++) add(sub,ctrlSrcLabel(0xB000|cc),0xB000|cc);
            const bool inside=(current>>8)==0xB0 && (current&0xFF)>=base && (current&0xFF)<base+32;
            const std::wstring title=L"CC "+std::to_wstring(base)+L"\u2013"+std::to_wstring(base+31);
            AppendMenuW(menu,MF_POPUP|(inside ? MF_CHECKED : 0),UINT_PTR(sub),title.c_str());
        }
    } else if(id==87 || id==89) {
        add(menu,L"none",-1);
        static const int assigns[]={76,45,95,2,12,13,11,5,8,91,15,16,17,18,19,20,21,23,22,25,26,27,28,29,
            79,80,81,61,62,63,60,90,35,36,37,83,98,52,53,54,55,56,39,75,92,84,85,34,43,44,48,49};
        for(int item:assigns) add(menu,toWide(hooks_.paramName?hooks_.paramName(hooks_.ctx,item):""),item);
    }
    popupMenu(menu,x,y,[this,id,values](int selected) { // the menu is destroyed with its sub-menus
        if(selected>=2000&&selected<2000+int(values.size()))setValueOnce(id,values[size_t(selected-2000)]);
    });
}
#if defined(OXO_SNAPSHOT)
void DesktopEditor::snapshotMode(const std::string& mode) {
    if(mode=="play") {setView(View::Play);return;}
    if(mode.rfind("search:",0)==0) searchText_=toWide(mode.substr(7).c_str());
    if(mode=="browser" || mode=="write" || mode.rfind("search:",0)==0) {
        browser_=true;writing_=mode=="write";
        int b=-1,sl=-1;
        hooks_.loadedProgram(hooks_.ctx,&b,&sl);
        selectedBank_=b;selectedSlot_=sl;
        refreshBanks();
    }
}
#endif
void DesktopEditor::tick() {
    sharedThemes().flush(false);
    savePendingSize(false);
    if(flashUntil_ && GetTickCount64()>=flashUntil_) flash({});
    if(bankTip_.dueAt && GetTickCount64()>=bankTip_.dueAt) {
        bankTip_.dueAt=0;bankTip_.shown=true;
        invalidate(bankTipRect());
    }
    bool dirty=false;
    if(hooks_.patchSerial) {
        const int serial=hooks_.patchSerial(hooks_.ctx);
        if(serial!=knownSerial_) {
            knownSerial_=serial;syncTheme();dirty=true;
        }
    }
    // Poll even with the browser closed: the first full scan must finish
    // asynchronously, without any synchronous EnsureScanned() on startup.
    if(hooks_.libraryRevision) {
        const int revision=hooks_.libraryRevision(hooks_.ctx);
        if(revision!=knownRevision_) {
            knownRevision_=revision;refreshBanks();dirty=true;
        }
    }
    // SynthModel.incomingCC: every new CC is consumed (none learned while
    // nothing is armed), CC 1..127 only; the control stays armed.
    if(midiLearn_ && hooks_.lastCC) {
        const int token=hooks_.lastCC(hooks_.ctx);
        if(token!=lastCcToken_ && token>0) {
            lastCcToken_=token;
            const int cc=token&127;
            if(learnTarget_>=0 && learnTarget_<kLearnCount && cc>=1 && learnCC_[size_t(learnTarget_)]!=cc && hooks_.setMidiCC) {
                hooks_.setMidiCC(hooks_.ctx,learnTarget_,cc);
                learnCC_[size_t(learnTarget_)]=cc;
                invalidate(learnRects_[size_t(learnTarget_)]);
                flash(L"MIDI LEARN: "+toWide(hooks_.paramName ? hooks_.paramName(hooks_.ctx,learnTarget_) : "")+
                      L" = CC "+std::to_wstring(cc));
            }
        }
    }
    if(dirty) repaint();
    // Host automation / MIDI / patch edits: redraw what shows a changed
    // value (SynthModel polls the same way).
    for(size_t id=0;id<knownValues_.size();++id)
        if(value(int(id))!=knownValues_[id]) valueChanged(int(id));
    if(!midiLearn_ && hooks_.voiceStates) {
        const uint64_t states=hooks_.voiceStates(hooks_.ctx);
        if(states!=knownVoiceStates_) {knownVoiceStates_=states;invalidate(kMeterRect);}
    }
}

} // namespace oxo
