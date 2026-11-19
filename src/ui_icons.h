#pragma once

#include <d2d1.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace clipper::ui {

enum class IconId {
    BrandEmblem,
    FilmClips,
    MonitorCapture,
    WaveformAudio,
    KeyboardKeys,
    SparkSave,
    PlayTriangle,
    FolderOpen,
    TrashBin,
    RefreshArrows,
    SpeakerOn,
    MicOn,
    BellChime,
    CheckCircle,
    MinimizeTray,
    PowerQuit,
    SlidersTune,
    ExternalArrow
};

namespace detail {

struct IconXform {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float s = 1.0f;

    D2D1_POINT_2F Pt(float nx, float ny) const {
        return D2D1::Point2F(x0 + nx * s, y0 + ny * s);
    }

    D2D1_RECT_F Rect(float l, float t, float r, float b) const {
        return D2D1::RectF(x0 + l * s, y0 + t * s, x0 + r * s, y0 + b * s);
    }

    D2D1_ROUNDED_RECT RRect(float l, float t, float r, float b, float rad) const {
        return D2D1::RoundedRect(Rect(l, t, r, b), rad * s, rad * s);
    }
};

inline void DrawPoly(ID2D1RenderTarget* rt, ID2D1Factory* factory, ID2D1Brush* brush,
                     ID2D1StrokeStyle* strokeStyle, const IconXform& xf,
                     std::initializer_list<D2D1_POINT_2F> pts, bool closed, bool filled, float sw) {
    if (pts.size() < 2 || !factory || !rt || !brush) return;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(&path))) return;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) return;

    auto it = pts.begin();
    sink->BeginFigure(xf.Pt(it->x, it->y), filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    for (++it; it != pts.end(); ++it) {
        sink->AddLine(xf.Pt(it->x, it->y));
    }
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    if (SUCCEEDED(sink->Close())) {
        if (filled) {
            rt->FillGeometry(path.Get(), brush);
        } else {
            rt->DrawGeometry(path.Get(), brush, sw, strokeStyle);
        }
    }
}

inline void DrawArcDeg(ID2D1RenderTarget* rt, ID2D1Factory* factory, ID2D1Brush* brush,
                       ID2D1StrokeStyle* strokeStyle, const IconXform& xf,
                       float cx, float cy, float r, float startDeg, float endDeg, float sw) {
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    float a0 = startDeg * kDegToRad;
    float a1 = endDeg * kDegToRad;
    D2D1_POINT_2F p0 = xf.Pt(cx + std::cos(a0) * r, cy + std::sin(a0) * r);
    D2D1_POINT_2F p1 = xf.Pt(cx + std::cos(a1) * r, cy + std::sin(a1) * r);

    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(&path))) return;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) return;

    float sweepDiff = std::fabs(endDeg - startDeg);
    D2D1_ARC_SIZE arcSz = (sweepDiff > 180.0f) ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
    D2D1_SWEEP_DIRECTION dir = (endDeg >= startDeg)
        ? D2D1_SWEEP_DIRECTION_CLOCKWISE
        : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;

    sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(p1, D2D1::SizeF(r * xf.s, r * xf.s), 0.0f, dir, arcSz));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (SUCCEEDED(sink->Close())) {
        rt->DrawGeometry(path.Get(), brush, sw, strokeStyle);
    }
}

}

inline void DrawVectorIcon(ID2D1RenderTarget* rt, ID2D1Factory* factory, ID2D1Brush* brush,
                           ID2D1StrokeStyle* strokeStyle, IconId icon,
                           D2D1_RECT_F bounds, float strokeWidth = 1.6f) {
    if (!rt || !factory || !brush) return;
    float w = bounds.right - bounds.left;
    float h = bounds.bottom - bounds.top;
    float size = std::min(w, h);
    if (size <= 0.0f) return;

    detail::IconXform xf{};
    xf.x0 = (bounds.left + bounds.right - size) * 0.5f;
    xf.y0 = (bounds.top + bounds.bottom - size) * 0.5f;
    xf.s = size / 24.0f;

    float sw = strokeWidth;

    switch (icon) {
        case IconId::BrandEmblem:
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {4.0f, 8.5f}, {4.0f, 4.0f}, {8.5f, 4.0f} }, false, false, sw);
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {15.5f, 4.0f}, {20.0f, 4.0f}, {20.0f, 8.5f} }, false, false, sw);
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {20.0f, 15.5f}, {20.0f, 20.0f}, {15.5f, 20.0f} }, false, false, sw);
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {8.5f, 20.0f}, {4.0f, 20.0f}, {4.0f, 15.5f} }, false, false, sw);
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {9.8f, 8.0f}, {16.6f, 12.0f}, {9.8f, 16.0f} }, true, true, sw);
            break;

        case IconId::FilmClips:
            rt->DrawRoundedRectangle(xf.RRect(3.0f, 4.5f, 21.0f, 19.5f, 2.5f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(3.2f, 8.5f), xf.Pt(20.8f, 8.5f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(7.8f, 4.5f), xf.Pt(9.8f, 8.5f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(13.8f, 4.5f), xf.Pt(15.8f, 8.5f), brush, sw, strokeStyle);
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {10.2f, 11.1f}, {15.0f, 14.0f}, {10.2f, 16.9f} }, true, true, sw);
            break;

        case IconId::MonitorCapture:
            rt->DrawRoundedRectangle(xf.RRect(2.5f, 3.5f, 21.5f, 16.2f, 2.5f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(12.0f, 16.2f), xf.Pt(12.0f, 20.2f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(7.5f, 20.2f), xf.Pt(16.5f, 20.2f), brush, sw, strokeStyle);
            rt->FillEllipse(D2D1::Ellipse(xf.Pt(12.0f, 9.8f), 2.1f * xf.s, 2.1f * xf.s), brush);
            break;

        case IconId::WaveformAudio:
            rt->DrawLine(xf.Pt(4.0f, 10.0f), xf.Pt(4.0f, 14.0f), brush, sw * 1.15f, strokeStyle);
            rt->DrawLine(xf.Pt(8.0f, 6.0f), xf.Pt(8.0f, 18.0f), brush, sw * 1.15f, strokeStyle);
            rt->DrawLine(xf.Pt(12.0f, 3.5f), xf.Pt(12.0f, 20.5f), brush, sw * 1.15f, strokeStyle);
            rt->DrawLine(xf.Pt(16.0f, 7.0f), xf.Pt(16.0f, 17.0f), brush, sw * 1.15f, strokeStyle);
            rt->DrawLine(xf.Pt(20.0f, 9.5f), xf.Pt(20.0f, 14.5f), brush, sw * 1.15f, strokeStyle);
            break;

        case IconId::KeyboardKeys:
            rt->DrawRoundedRectangle(xf.RRect(2.5f, 5.5f, 21.5f, 18.5f, 2.5f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(6.5f, 9.5f), xf.Pt(7.5f, 9.5f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(10.5f, 9.5f), xf.Pt(11.5f, 9.5f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(14.5f, 9.5f), xf.Pt(15.5f, 9.5f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(6.5f, 12.6f), xf.Pt(7.5f, 12.6f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(10.5f, 12.6f), xf.Pt(11.5f, 12.6f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(14.5f, 12.6f), xf.Pt(17.5f, 12.6f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(7.5f, 15.6f), xf.Pt(16.5f, 15.6f), brush, sw, strokeStyle);
            break;

        case IconId::SparkSave:
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {13.4f, 2.5f}, {5.2f, 13.4f}, {11.5f, 13.4f},
                  {10.6f, 21.5f}, {18.8f, 10.6f}, {12.5f, 10.6f} },
                true, true, sw);
            break;

        case IconId::PlayTriangle:
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {8.2f, 5.4f}, {18.6f, 12.0f}, {8.2f, 18.6f} },
                true, true, sw);
            break;

        case IconId::FolderOpen:
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {3.0f, 6.0f}, {3.0f, 18.5f}, {21.0f, 18.5f},
                  {21.0f, 8.2f}, {12.0f, 8.2f}, {10.0f, 6.0f} },
                true, false, sw);
            break;

        case IconId::TrashBin:
            rt->DrawLine(xf.Pt(4.5f, 6.5f), xf.Pt(19.5f, 6.5f), brush, sw, strokeStyle);
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {9.0f, 6.5f}, {9.5f, 4.0f}, {14.5f, 4.0f}, {15.0f, 6.5f} }, false, false, sw);
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {6.2f, 6.5f}, {7.4f, 19.8f}, {16.6f, 19.8f}, {17.8f, 6.5f} }, false, false, sw);
            rt->DrawLine(xf.Pt(10.0f, 10.2f), xf.Pt(10.4f, 16.5f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(14.0f, 10.2f), xf.Pt(13.6f, 16.5f), brush, sw, strokeStyle);
            break;

        case IconId::RefreshArrows:
            detail::DrawArcDeg(rt, factory, brush, strokeStyle, xf, 12.0f, 12.0f, 7.2f, 45.0f, 334.0f, sw);
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {18.6f, 4.9f}, {18.6f, 8.9f}, {14.6f, 8.9f} }, false, false, sw);
            break;

        case IconId::SpeakerOn:
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {3.5f, 9.5f}, {7.5f, 9.5f}, {12.2f, 5.2f},
                  {12.2f, 18.8f}, {7.5f, 14.5f}, {3.5f, 14.5f} },
                true, false, sw);
            detail::DrawArcDeg(rt, factory, brush, strokeStyle, xf, 12.5f, 12.0f, 4.0f, -48.0f, 48.0f, sw);
            detail::DrawArcDeg(rt, factory, brush, strokeStyle, xf, 12.5f, 12.0f, 7.4f, -48.0f, 48.0f, sw);
            break;

        case IconId::MicOn:
            rt->DrawRoundedRectangle(xf.RRect(9.2f, 2.8f, 14.8f, 13.4f, 2.8f), brush, sw, strokeStyle);
            detail::DrawArcDeg(rt, factory, brush, strokeStyle, xf, 12.0f, 10.6f, 5.6f, 0.0f, 180.0f, sw);
            rt->DrawLine(xf.Pt(12.0f, 16.2f), xf.Pt(12.0f, 20.5f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(8.5f, 20.5f), xf.Pt(15.5f, 20.5f), brush, sw, strokeStyle);
            break;

        case IconId::BellChime:
            detail::DrawArcDeg(rt, factory, brush, strokeStyle, xf, 12.0f, 9.5f, 5.5f, -180.0f, 0.0f, sw);
            rt->DrawLine(xf.Pt(6.5f, 9.5f), xf.Pt(5.2f, 16.2f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(17.5f, 9.5f), xf.Pt(18.8f, 16.2f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(4.0f, 16.2f), xf.Pt(20.0f, 16.2f), brush, sw, strokeStyle);
            detail::DrawArcDeg(rt, factory, brush, strokeStyle, xf, 12.0f, 17.8f, 2.2f, 15.0f, 165.0f, sw);
            break;

        case IconId::CheckCircle:
            rt->DrawEllipse(D2D1::Ellipse(xf.Pt(12.0f, 12.0f), 9.0f * xf.s, 9.0f * xf.s), brush, sw);
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {8.2f, 12.2f}, {10.9f, 14.9f}, {16.0f, 9.3f} }, false, false, sw);
            break;

        case IconId::MinimizeTray:
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {4.5f, 15.0f}, {4.5f, 19.5f}, {19.5f, 19.5f}, {19.5f, 15.0f} }, false, false, sw);
            rt->DrawLine(xf.Pt(12.0f, 4.5f), xf.Pt(12.0f, 14.2f), brush, sw, strokeStyle);
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {8.2f, 10.6f}, {12.0f, 14.4f}, {15.8f, 10.6f} }, false, false, sw);
            break;

        case IconId::PowerQuit:
            rt->DrawLine(xf.Pt(12.0f, 3.6f), xf.Pt(12.0f, 11.4f), brush, sw, strokeStyle);
            detail::DrawArcDeg(rt, factory, brush, strokeStyle, xf, 12.0f, 12.6f, 7.4f, -46.0f, 226.0f, sw);
            break;

        case IconId::SlidersTune:
            rt->DrawLine(xf.Pt(3.5f, 6.5f), xf.Pt(20.5f, 6.5f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(3.5f, 12.0f), xf.Pt(20.5f, 12.0f), brush, sw, strokeStyle);
            rt->DrawLine(xf.Pt(3.5f, 17.5f), xf.Pt(20.5f, 17.5f), brush, sw, strokeStyle);
            rt->FillEllipse(D2D1::Ellipse(xf.Pt(8.5f, 6.5f), 2.1f * xf.s, 2.1f * xf.s), brush);
            rt->FillEllipse(D2D1::Ellipse(xf.Pt(15.5f, 12.0f), 2.1f * xf.s, 2.1f * xf.s), brush);
            rt->FillEllipse(D2D1::Ellipse(xf.Pt(10.0f, 17.5f), 2.1f * xf.s, 2.1f * xf.s), brush);
            break;

        case IconId::ExternalArrow:
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {13.5f, 5.0f}, {5.0f, 5.0f}, {5.0f, 19.0f}, {19.0f, 19.0f}, {19.0f, 10.5f} },
                false, false, sw);
            rt->DrawLine(xf.Pt(11.5f, 12.5f), xf.Pt(19.5f, 4.5f), brush, sw, strokeStyle);
            detail::DrawPoly(rt, factory, brush, strokeStyle, xf,
                { {14.5f, 4.5f}, {19.5f, 4.5f}, {19.5f, 9.5f} }, false, false, sw);
            break;
    }
}

}
