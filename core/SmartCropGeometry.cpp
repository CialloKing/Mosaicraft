#include "SmartCrop.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <opencv2/imgproc.hpp>
#include <tuple>

namespace mosaicraft
{
namespace
{
cv::Rect pixelFace(const cv::Rect2d &face, cv::Size covered, double margin)
{
    const int l = static_cast<int>(std::floor((face.x - face.width * margin) * covered.width + 1e-8));
    const int t = static_cast<int>(std::floor((face.y - face.height * margin) * covered.height + 1e-8));
    const int r = static_cast<int>(std::ceil((face.br().x + face.width * margin) * covered.width - 1e-8));
    const int b = static_cast<int>(std::ceil((face.br().y + face.height * margin) * covered.height - 1e-8));
    return cv::Rect(l, t, r - l, b - t) & cv::Rect(cv::Point(), covered);
}

std::vector<int> positions(int extent, int span, int bins, const std::vector<cv::Rect> &faces,
                           const std::vector<cv::Rect> &padded, bool horizontal)
{
    const int limit = extent - span;
    if (limit == 0)
    {
        return {0};
    }
    std::vector<int> result{0, limit, limit / 2};
    const auto add = [&](int offset)
    {
        result.push_back(std::clamp(offset, 0, limit));
    };
    for (int i = 1; i < bins; ++i)
    {
        // A foreground cell changes slope only when the crop enters or leaves it.
        const int edge = static_cast<int>(static_cast<int64_t>(i) * extent / bins);
        add(edge);
        add(edge + 1);
        add(edge - span);
        add(edge + 1 - span);
    }
    for (const auto *group : {&faces, &padded})
    {
        for (const auto &face : *group)
        {
            add(horizontal ? face.x : face.y);
            add((horizontal ? face.br().x : face.br().y) - span);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

// Interpolate the integral at fractional cell boundaries, not the crop itself.
double integralAt(const cv::Mat &integral, double x, double y)
{
    x = std::clamp(x, 0.0, static_cast<double>(integral.cols - 1));
    y = std::clamp(y, 0.0, static_cast<double>(integral.rows - 1));
    const int l = static_cast<int>(x), t = static_cast<int>(y);
    const int r = std::min(l + 1, integral.cols - 1), b = std::min(t + 1, integral.rows - 1);
    const double dx = x - l, dy = y - t;
    const auto *top = integral.ptr<double>(t), *bottom = integral.ptr<double>(b);
    return (1 - dy) * ((1 - dx) * top[l] + dx * top[r]) + dy * ((1 - dx) * bottom[l] + dx * bottom[r]);
}
}

SmartCropDecision chooseSmartCrop(cv::Size covered, cv::Size target, const SmartCropAnalysis &analysis)
{
    const cv::Rect center((covered.width - target.width) / 2, (covered.height - target.height) / 2,
                          target.width, target.height);
    std::vector<cv::Rect> faces, padded;
    int mainFace = -1;
    for (const auto &box : analysis.faces)
    {
        const auto face = pixelFace(box, covered, 0);
        if (face.empty())
        {
            continue;
        }
        if (mainFace < 0 || face.area() > faces[mainFace].area() ||
            (face.area() == faces[mainFace].area() && std::tie(face.x, face.y) <
                                                        std::tie(faces[mainFace].x, faces[mainFace].y)))
        {
            mainFace = static_cast<int>(faces.size());
        }
        faces.push_back(face);
        padded.push_back(pixelFace(box, covered, 0.10));
    }

    cv::Mat integral;
    double total = 0;
    if (!analysis.foreground.empty())
    {
        double low = 0, high = 0;
        cv::minMaxLoc(analysis.foreground, &low, &high);
        if (high - low >= 0.05)
        {
            cv::integral(analysis.foreground, integral, CV_64F);
            total = integral.at<double>(integral.rows - 1, integral.cols - 1);
        }
    }
    if (faces.empty() && total <= 0)
    {
        return SmartCropDecision{center, false, false};
    }

    const double sx = static_cast<double>(analysis.foreground.cols) / covered.width;
    const double sy = static_cast<double>(analysis.foreground.rows) / covered.height;
    const auto mass = [&](const cv::Rect &roi)
    {
        if (total <= 0)
        {
            return 0.0;
        }
        const double l = roi.x * sx, r = roi.br().x * sx, t = roi.y * sy, b = roi.br().y * sy;
        return integralAt(integral, r, b) - integralAt(integral, l, b) -
               integralAt(integral, r, t) + integralAt(integral, l, t);
    };
    const auto priority = [&](const cv::Rect &roi)
    {
        int other = 0, margins = 0;
        for (int i = 0; i < static_cast<int>(faces.size()); ++i)
        {
            other += i != mainFace && (faces[i] & roi) == faces[i];
            margins += (padded[i] & roi) == padded[i];
        }
        // No amount of heatmap mass or margin is allowed to cut a preservable face.
        return std::make_tuple(mainFace >= 0 ? (faces[mainFace] & roi).area() : 0, other, margins);
    };
    const auto distance = [&](const cv::Rect &roi)
    {
        return std::make_tuple(std::abs(roi.x - center.x) + std::abs(roi.y - center.y), roi.x, roi.y);
    };
    const auto xs = positions(covered.width, target.width, total > 0 ? analysis.foreground.cols : 0, faces, padded, true);
    const auto ys = positions(covered.height, target.height, total > 0 ? analysis.foreground.rows : 0, faces, padded, false);
    auto baseline = center, best = center;
    auto rank = priority(center);
    double bestMass = mass(center);
    for (const int x : xs)
    {
        for (const int y : ys)
        {
            const cv::Rect roi(x, y, target.width, target.height);
            const auto candidate = priority(roi);
            if (candidate < rank)
            {
                continue;
            }
            const double score = mass(roi);
            if (candidate > rank)
            {
                rank = candidate;
                baseline = best = roi;
                bestMass = score;
                continue;
            }
            if (distance(roi) < distance(baseline))
            {
                baseline = roi;
            }
            if (score > bestMass + 1e-8 || (std::abs(score - bestMass) <= 1e-8 && distance(roi) < distance(best)))
            {
                best = roi;
                bestMass = score;
            }
        }
    }
    const auto roi = total > 0 && bestMass - mass(baseline) >= 0.05 * total ? best : baseline;
    return SmartCropDecision{roi, mainFace >= 0, mainFace >= 0 && (faces[mainFace] & roi) != faces[mainFace]};
}
}
