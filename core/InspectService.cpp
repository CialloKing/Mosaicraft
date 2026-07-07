#include "InspectService.h"

#include "Database.h"
#include "FeatureUtils.h"
#include "MosaicService.h"
#include "UnicodeIO.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>

namespace mosaicraft
{

InspectResult InspectService::inspect(const InspectRequest& request) const
{
    InspectResult out;
    out.imagePath = request.imagePath;
    if (request.imagePath.empty())
    {
        out.status = ServiceResult::failure(1, "imagePath is required");
        return out;
    }

    cv::Mat img = imreadUnicode(request.imagePath, cv::IMREAD_COLOR);
    if (img.empty())
    {
        out.status = ServiceResult::failure(1, "cannot read image: " + request.imagePath);
        return out;
    }

    out.width = img.cols;
    out.height = img.rows;

    cv::Mat native;
    cv::resize(img, native, cv::Size(180, 320), 0, 0, cv::INTER_LINEAR);

    cv::Mat lab;
    cv::cvtColor(native, lab, cv::COLOR_BGR2Lab);
    cv::Scalar m = cv::mean(lab);
    out.avgL = m[0];
    out.avgA = m[1];
    out.avgB = m[2];
    out.edgeDensity = computeEdgeDensity(native);

    auto lbp = computeLBPHistogram(native);
    for (float v : lbp)
    {
        if (v > 0) out.lbpEntropy -= v * std::log2(v);
    }

    Database db(resolveDbPathForService(request.dbPath));
    if (db.isOpen())
    {
        out.databaseAvailable = true;
        out.databaseTotal = db.totalCount();
        out.candidateMinL = out.avgL - 20.0;
        out.candidateMaxL = out.avgL + 20.0;
        auto candidates = db.queryIdsByLRange(out.candidateMinL, out.candidateMaxL, 200, false);
        out.candidateCount = static_cast<int>(candidates.size());

        auto all = db.allRecords();
        if (!all.empty())
        {
            out.libraryMinL = 255.0;
            out.libraryMaxL = 0.0;
            double sumL = 0.0;
            for (const auto& r : all)
            {
                out.libraryMinL = std::min(out.libraryMinL, r.avgL);
                out.libraryMaxL = std::max(out.libraryMaxL, r.avgL);
                sumL += r.avgL;
                if (r.avgL < 30) out.libraryDark++;
                else if (r.avgL < 70) out.libraryMid++;
                else out.libraryBright++;
            }
            out.libraryAvgL = sumL / all.size();
        }
    }

    out.status = ServiceResult::success("inspect completed");
    return out;
}

} // namespace mosaicraft
