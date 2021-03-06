/******************************************************************************
 * Copyright (c) 2015-2017, Bradley J Chambers (brad.chambers@gmail.com)
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following
 * conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of Hobu, Inc. or Flaxen Geo Consulting nor the
 *       names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior
 *       written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 ****************************************************************************/

// PDAL implementation of K. Zhang, S.-C. Chen, D. Whitman, M.-L. Shyu, J. Yan,
// and C. Zhang, “A progressive morphological filter for removing nonground
// measurements from airborne LIDAR data,” Geosci. Remote Sensing, IEEE Trans.,
// vol. 41, no. 4, pp. 872–882, 2003.

#include "PMFFilter.hpp"

#include <pdal/EigenUtils.hpp>
#include <pdal/KDIndex.hpp>
#include <pdal/util/ProgramArgs.hpp>

#include "private/Segmentation.hpp"
#include "private/DimRange.hpp"

namespace pdal
{

static StaticPluginInfo const s_info
{
    "filters.pmf",
    "Progressive morphological filter",
    "http://pdal.io/stages/filters.pmf.html"
};

struct PMFArgs
{
    double m_cellSize;
    bool m_exponential;
    std::vector<DimRange> m_ignored;
    double m_initialDistance;
    StringList m_returns;
    double m_maxDistance;
    double m_maxWindowSize;
    double m_slope;
};

CREATE_STATIC_STAGE(PMFFilter, s_info)

PMFFilter::PMFFilter() : m_args(new PMFArgs)
{}

PMFFilter::~PMFFilter()
{}

std::string PMFFilter::getName() const
{
    return s_info.name;
}

void PMFFilter::addArgs(ProgramArgs& args)
{
    args.add("cell_size", "Cell size", m_args->m_cellSize, 1.0);
    args.add("exponential", "Exponential growth of window size?",
             m_args->m_exponential, true);
    args.add("ignore", "Ignore values", m_args->m_ignored);
    args.add("initial_distance", "Initial distance", m_args->m_initialDistance,
             0.15);
    args.add("returns", "Include only returns?", m_args->m_returns,
             {"last", "only"});
    args.add("max_distance", "Maximum distance", m_args->m_maxDistance, 2.5);
    args.add("max_window_size", "Maximum window size", m_args->m_maxWindowSize,
             33.0);
    args.add("slope", "Slope", m_args->m_slope, 1.0);
}

void PMFFilter::addDimensions(PointLayoutPtr layout)
{
    layout->registerDim(Dimension::Id::Classification);
}

void PMFFilter::prepared(PointTableRef table)
{
    const PointLayoutPtr layout(table.layout());

    for (auto& r : m_args->m_ignored)
    {
        r.m_id = layout->findDim(r.m_name);
        if (r.m_id == Dimension::Id::Unknown)
            throwError("Invalid dimension name in 'ignored' option: '" +
                r.m_name + "'.");
    }

    if (m_args->m_returns.size())
    {
        for (auto& r : m_args->m_returns)
        {
            Utils::trim(r);
            if ((r != "first") && (r != "intermediate") && (r != "last") &&
                (r != "only"))
            {
                throwError("Unrecognized 'returns' value: '" + r + "'.");
            }
        }

        if (!layout->hasDim(Dimension::Id::ReturnNumber) ||
            !layout->hasDim(Dimension::Id::NumberOfReturns))
        {
            log()->get(LogLevel::Warning) << "Could not find ReturnNumber and "
                                             "NumberOfReturns. Skipping "
                                             "segmentation of last returns and "
                                             "proceeding with all returns.\n";
            m_args->m_returns = {""};
        }
    }
}

PointViewSet PMFFilter::run(PointViewPtr input)
{
    PointViewSet viewSet;
    if (!input->size())
        return viewSet;

    // Segment input view into ignored/kept views.
    PointViewPtr ignoredView = input->makeNew();
    PointViewPtr keptView = input->makeNew();

    if (m_args->m_ignored.empty())
        keptView->append(*input);
    else
        Segmentation::ignoreDimRanges(m_args->m_ignored,
            input, keptView, ignoredView);

    // Classify remaining points with value of 1. processGround will mark ground
    // returns as 2.
    for (PointId i = 0; i < keptView->size(); ++i)
        keptView->setField(Dimension::Id::Classification, i, 1);

    // Segment kept view into two views
    PointViewPtr firstView = keptView->makeNew();
    PointViewPtr secondView = keptView->makeNew();
    if (m_args->m_returns.size())
    {
        Segmentation::segmentReturns(keptView, firstView, secondView,
                                     m_args->m_returns);
    }
    else
    {
        for (PointId i = 0; i < keptView->size(); ++i)
            firstView->appendPoint(*keptView.get(), i);
    }

    if (!firstView->size())
    {
        throwError("No returns to process.");
    }

    // Run the actual PMF algorithm.
    processGround(firstView);

    // Prepare the output PointView.
    PointViewPtr outView = input->makeNew();
    outView->append(*ignoredView);
    outView->append(*secondView);
    outView->append(*firstView);
    viewSet.insert(outView);

    return viewSet;
}

void PMFFilter::processGround(PointViewPtr view)
{
    // initialize bounds, rows, columns, and surface
    BOX2D bounds;
    view->calculateBounds(bounds);
    size_t cols = ((bounds.maxx - bounds.minx) / m_args->m_cellSize) + 1;
    size_t rows = ((bounds.maxy - bounds.miny) / m_args->m_cellSize) + 1;

    // initialize surface to NaN
    std::vector<double> ZImin(rows * cols,
                              std::numeric_limits<double>::quiet_NaN());

    // loop through all points, identifying minimum Z value for each populated
    // cell
    for (PointId i = 0; i < view->size(); ++i)
    {
        double x = view->getFieldAs<double>(Dimension::Id::X, i);
        double y = view->getFieldAs<double>(Dimension::Id::Y, i);
        double z = view->getFieldAs<double>(Dimension::Id::Z, i);
        int c = static_cast<int>(floor(x - bounds.minx) / m_args->m_cellSize);
        int r = static_cast<int>(floor(y - bounds.miny) / m_args->m_cellSize);
        size_t idx = c * rows + r;
        if (z < ZImin[idx] || std::isnan(ZImin[idx]))
            ZImin[idx] = z;
    }

    // convert vector to PointView for indexing
    PointViewPtr temp = view->makeNew();
    PointId i(0);
    for (size_t c = 0; c < cols; ++c)
    {
        for (size_t r = 0; r < rows; ++r)
        {
            size_t idx = c * rows + r;
            if (std::isnan(ZImin[idx]))
                continue;
            double x = bounds.minx + (c + 0.5) * m_args->m_cellSize;
            double y = bounds.miny + (r + 0.5) * m_args->m_cellSize;
            temp->setField(Dimension::Id::X, i, x);
            temp->setField(Dimension::Id::Y, i, y);
            temp->setField(Dimension::Id::Z, i, ZImin[idx]);
            i++;
        }
    }

    // build the 2D KD-tree
    KD2Index& kdi = temp->build2dIndex();

    // loop through all cells, and for each NaN, replace with elevation of
    // nearest neighbor
    std::vector<double> out = ZImin;
    for (size_t c = 0; c < cols; ++c)
    {
        for (size_t r = 0; r < rows; ++r)
        {
            size_t idx = c * rows + r;
            if (!std::isnan(out[idx]))
                continue;
            double x = bounds.minx + (c + 0.5) * m_args->m_cellSize;
            double y = bounds.miny + (r + 0.5) * m_args->m_cellSize;
            int k = 1;
            std::vector<PointId> neighbors(k);
            std::vector<double> sqr_dists(k);
            kdi.knnSearch(x, y, k, &neighbors, &sqr_dists);
            out[idx] = temp->getFieldAs<double>(Dimension::Id::Z, neighbors[0]);
        }
    }

    ZImin.swap(out);

    // initialize ground indices
    std::vector<PointId> groundIdx;
    for (PointId i = 0; i < view->size(); ++i)
        groundIdx.push_back(i);

    // Compute the series of window sizes and height thresholds
    std::vector<float> htvec;
    std::vector<float> wsvec;
    int iter = 0;
    float ws = 0.0f;
    float ht = 0.0f;

    // pre-compute window sizes and height thresholds
    while (ws < m_args->m_maxWindowSize)
    {
        // Determine the initial window size.
        if (m_args->m_exponential)
            ws = m_args->m_cellSize * (2.0f * std::pow(2, iter) + 1.0f);
        else
            ws = m_args->m_cellSize * (2.0f * (iter + 1) * 2 + 1.0f);

        // Calculate the height threshold to be used in the next iteration.
        if (iter == 0)
            ht = m_args->m_initialDistance;
        else
            ht = m_args->m_slope * (ws - wsvec[iter - 1]) * m_args->m_cellSize +
                 m_args->m_initialDistance;

        // Enforce max distance on height threshold
        if (ht > m_args->m_maxDistance)
            ht = m_args->m_maxDistance;

        wsvec.push_back(ws);
        htvec.push_back(ht);

        iter++;
    }

    // Progressively filter ground returns using morphological open
    for (size_t j = 0; j < wsvec.size(); ++j)
    {
        log()->get(LogLevel::Debug)
            << "Iteration " << j << " (height threshold = " << htvec[j]
            << ", window size = " << wsvec[j] << ")...\n";

        int iters = 0.5 * (wsvec[j] - 1);
        using namespace eigen;
        std::vector<double> me = erodeDiamond(ZImin, rows, cols, iters);
        std::vector<double> mo = dilateDiamond(me, rows, cols, iters);

        std::vector<PointId> groundNewIdx;
        for (auto p_idx : groundIdx)
        {
            double x = view->getFieldAs<double>(Dimension::Id::X, p_idx);
            double y = view->getFieldAs<double>(Dimension::Id::Y, p_idx);
            double z = view->getFieldAs<double>(Dimension::Id::Z, p_idx);

            int c =
                static_cast<int>(floor((x - bounds.minx) / m_args->m_cellSize));
            int r =
                static_cast<int>(floor((y - bounds.miny) / m_args->m_cellSize));

            if ((z - mo[c * rows + r]) < htvec[j])
                groundNewIdx.push_back(p_idx);
        }

        ZImin.swap(mo);
        groundIdx.swap(groundNewIdx);

        log()->get(LogLevel::Debug)
            << "Ground now has " << groundIdx.size() << " points.\n";
    }

    log()->get(LogLevel::Debug2)
        << "Labeled " << groundIdx.size() << " ground returns!\n";

    // set the classification label of ground returns as 2
    // (corresponding to ASPRS LAS specification)
    for (const auto& i : groundIdx)
        view->setField(Dimension::Id::Classification, i, 2);
}

} // namespace pdal
