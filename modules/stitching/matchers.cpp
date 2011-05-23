/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                          License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2000-2008, Intel Corporation, all rights reserved.
// Copyright (C) 2009, Willow Garage Inc., all rights reserved.
// Third party copyrights are property of their respective owners.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of the copyright holders may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/
#include <algorithm>
#include <functional>
#include "matchers.hpp"
#include "util.hpp"

using namespace std;
using namespace cv;
using namespace cv::gpu;


//////////////////////////////////////////////////////////////////////////////

void FeaturesFinder::operator ()(const Mat &image, ImageFeatures &features) 
{ 
    features.img_size = image.size();

    // Calculate histogram
    Mat hsv;
    cvtColor(image, hsv, CV_BGR2HSV);
    int hbins = 30, sbins = 32, vbins = 30;
    int hist_size[] = { hbins, sbins, vbins };
    float hranges[] = { 0, 180 };
    float sranges[] = { 0, 256 };
    float vranges[] = { 0, 256 };
    const float* ranges[] = { hranges, sranges, vranges };
    int channels[] = { 0, 1, 2 };
    calcHist(&hsv, 1, channels, Mat(), features.hist, 3, hist_size, ranges);

    find(image, features);
}

//////////////////////////////////////////////////////////////////////////////

namespace
{
    class CpuSurfFeaturesFinder : public FeaturesFinder
    {
    public:
        inline CpuSurfFeaturesFinder(double hess_thresh, int num_octaves, int num_layers, 
                                     int num_octaves_descr, int num_layers_descr) 
        {
            detector_ = new SurfFeatureDetector(hess_thresh, num_octaves, num_layers);
            extractor_ = new SurfDescriptorExtractor(num_octaves_descr, num_layers_descr);
        }

    protected:
        void find(const Mat &image, ImageFeatures &features);

    private:
        Ptr<FeatureDetector> detector_;
        Ptr<DescriptorExtractor> extractor_;
    };

    void CpuSurfFeaturesFinder::find(const Mat &image, ImageFeatures &features)
    {
        Mat gray_image;
        CV_Assert(image.depth() == CV_8U);
        cvtColor(image, gray_image, CV_BGR2GRAY);
        detector_->detect(gray_image, features.keypoints);
        extractor_->compute(gray_image, features.keypoints, features.descriptors);
    }
    
    class GpuSurfFeaturesFinder : public FeaturesFinder
    {
    public:
        inline GpuSurfFeaturesFinder(double hess_thresh, int num_octaves, int num_layers, 
                                     int num_octaves_descr, int num_layers_descr) 
        {
            surf_.keypointsRatio = 0.1f;
            surf_.hessianThreshold = hess_thresh;
            surf_.extended = false;
            num_octaves_ = num_octaves;
            num_layers_ = num_layers;
            num_octaves_descr_ = num_octaves_descr;
            num_layers_descr_ = num_layers_descr;
        }

    protected:
        void find(const Mat &image, ImageFeatures &features);

    private:
        SURF_GPU surf_;
        int num_octaves_, num_layers_;
        int num_octaves_descr_, num_layers_descr_;
    };

    void GpuSurfFeaturesFinder::find(const Mat &image, ImageFeatures &features)
    {
        GpuMat gray_image;
        CV_Assert(image.depth() == CV_8U);
        cvtColor(GpuMat(image), gray_image, CV_BGR2GRAY);

        GpuMat d_keypoints;
        GpuMat d_descriptors;
        surf_.nOctaves = num_octaves_;
        surf_.nOctaveLayers = num_layers_;
        surf_(gray_image, GpuMat(), d_keypoints);

        surf_.nOctaves = num_octaves_descr_;
        surf_.nOctaveLayers = num_layers_descr_;
        surf_(gray_image, GpuMat(), d_keypoints, d_descriptors, true);
        surf_.downloadKeypoints(d_keypoints, features.keypoints);

        d_descriptors.download(features.descriptors);
    }
}

SurfFeaturesFinder::SurfFeaturesFinder(bool try_use_gpu, double hess_thresh, int num_octaves, int num_layers, 
                                       int num_octaves_descr, int num_layers_descr)
{
    if (try_use_gpu && getCudaEnabledDeviceCount() > 0)
        impl_ = new GpuSurfFeaturesFinder(hess_thresh, num_octaves, num_layers, num_octaves_descr, num_layers_descr);
    else
        impl_ = new CpuSurfFeaturesFinder(hess_thresh, num_octaves, num_layers, num_octaves_descr, num_layers_descr);
}


void SurfFeaturesFinder::find(const Mat &image, ImageFeatures &features)
{
    (*impl_)(image, features);
}


//////////////////////////////////////////////////////////////////////////////

MatchesInfo::MatchesInfo() : src_img_idx(-1), dst_img_idx(-1), num_inliers(0), confidence(0) {}

MatchesInfo::MatchesInfo(const MatchesInfo &other) { *this = other; }

const MatchesInfo& MatchesInfo::operator =(const MatchesInfo &other)
{
    src_img_idx = other.src_img_idx;
    dst_img_idx = other.dst_img_idx;
    matches = other.matches;
    inliers_mask = other.inliers_mask;
    num_inliers = other.num_inliers;
    H = other.H.clone();
    confidence = other.confidence;
    return *this;
}


//////////////////////////////////////////////////////////////////////////////

struct DistIdxPair
{
    bool operator<(const DistIdxPair &other) const { return dist < other.dist; }
    double dist;
    int idx;
};


struct MatchPairsBody
{
    MatchPairsBody(const MatchPairsBody& other)
            : matcher(other.matcher), features(other.features), 
              pairwise_matches(other.pairwise_matches), near_pairs(other.near_pairs) {}

    MatchPairsBody(FeaturesMatcher &matcher, const vector<ImageFeatures> &features, 
                   vector<MatchesInfo> &pairwise_matches, vector<pair<int,int> > &near_pairs)
            : matcher(matcher), features(features), 
              pairwise_matches(pairwise_matches), near_pairs(near_pairs) {}

    void operator ()(const BlockedRange &r) const 
    {
        const int num_images = static_cast<int>(features.size());
        for (int i = r.begin(); i < r.end(); ++i)
        {
            int from = near_pairs[i].first;
            int to = near_pairs[i].second;
            int pair_idx = from*num_images + to;

            matcher(features[from], features[to], pairwise_matches[pair_idx]);
            pairwise_matches[pair_idx].src_img_idx = from;
            pairwise_matches[pair_idx].dst_img_idx = to;

            size_t dual_pair_idx = to*num_images + from;

            pairwise_matches[dual_pair_idx] = pairwise_matches[pair_idx];
            pairwise_matches[dual_pair_idx].src_img_idx = to;
            pairwise_matches[dual_pair_idx].dst_img_idx = from;

            if (!pairwise_matches[pair_idx].H.empty())
                pairwise_matches[dual_pair_idx].H = pairwise_matches[pair_idx].H.inv();

            for (size_t j = 0; j < pairwise_matches[dual_pair_idx].matches.size(); ++j)
                swap(pairwise_matches[dual_pair_idx].matches[j].queryIdx,
                     pairwise_matches[dual_pair_idx].matches[j].trainIdx);
        }
    }

    FeaturesMatcher &matcher;
    const vector<ImageFeatures> &features;
    vector<MatchesInfo> &pairwise_matches;
    vector<pair<int,int> > &near_pairs;

private:
    void operator =(const MatchPairsBody&);
};


void FeaturesMatcher::operator ()(const vector<ImageFeatures> &features, vector<MatchesInfo> &pairwise_matches)
{
    const int num_images = static_cast<int>(features.size());

    Mat_<uchar> is_near(num_images, num_images);
    is_near.setTo(0);

    // Find good image pairs
    for (int i = 0; i < num_images; ++i)
    {
        vector<DistIdxPair> dists(num_images);
        for (int j = 0; j < num_images; ++j)
        {
            dists[j].dist = 1 - compareHist(features[i].hist, features[j].hist, CV_COMP_INTERSECT) 
                                / min(features[i].img_size.area(), features[j].img_size.area());
            dists[j].idx = j;
        }

        // Leave near images
        for (int j = 0; j < num_images; ++j)
            if (dists[j].dist < 0.6)
                is_near(i, dists[j].idx) = 1;

        // Leave k-nearest images
        int k = min(4, num_images);
        nth_element(dists.begin(), dists.end(), dists.begin() + k);
        for (int j = 0; j < k; ++j)
            is_near(i, dists[j].idx) = 1;
    }

    vector<pair<int,int> > near_pairs;
    for (int i = 0; i < num_images - 1; ++i)
        for (int j = i + 1; j < num_images; ++j)
            if (is_near(i, j))
                near_pairs.push_back(make_pair(i, j));

    pairwise_matches.resize(num_images * num_images);
    MatchPairsBody body(*this, features, pairwise_matches, near_pairs);

    if (is_thread_safe_)
        parallel_for(BlockedRange(0, static_cast<int>(near_pairs.size())), body);
    else
        body(BlockedRange(0, static_cast<int>(near_pairs.size())));
}


//////////////////////////////////////////////////////////////////////////////

namespace
{
    class CpuMatcher : public FeaturesMatcher
    {
    public:
        CpuMatcher(float match_conf) : FeaturesMatcher(true), match_conf_(match_conf) {}
        void match(const ImageFeatures &features1, const ImageFeatures &features2, MatchesInfo& matches_info);

    private:
        float match_conf_;
    };

    void CpuMatcher::match(const ImageFeatures &features1, const ImageFeatures &features2, MatchesInfo& matches_info)
    {
        matches_info.matches.clear();

        BruteForceMatcher< L2<float> > matcher;
        vector< vector<DMatch> > pair_matches;

        // Find 1->2 matches
        matcher.knnMatch(features1.descriptors, features2.descriptors, pair_matches, 2);
        for (size_t i = 0; i < pair_matches.size(); ++i)
        {
            if (pair_matches[i].size() < 2)
                continue;
            const DMatch& m0 = pair_matches[i][0];
            const DMatch& m1 = pair_matches[i][1];
            if (m0.distance < (1.f - match_conf_) * m1.distance)
                matches_info.matches.push_back(m0);
        }

        // Find 2->1 matches
        pair_matches.clear();
        matcher.knnMatch(features2.descriptors, features1.descriptors, pair_matches, 2);
        for (size_t i = 0; i < pair_matches.size(); ++i)
        {
            if (pair_matches[i].size() < 2)
                continue;
            const DMatch& m0 = pair_matches[i][0];
            const DMatch& m1 = pair_matches[i][1];
            if (m0.distance < (1.f - match_conf_) * m1.distance)
                matches_info.matches.push_back(DMatch(m0.trainIdx, m0.queryIdx, m0.distance));
        }
    }
        
    class GpuMatcher : public FeaturesMatcher
    {
    public:
        GpuMatcher(float match_conf) : match_conf_(match_conf) {}
        void match(const ImageFeatures &features1, const ImageFeatures &features2, MatchesInfo& matches_info);

    private:
        float match_conf_;
        GpuMat descriptors1_;
        GpuMat descriptors2_;
        GpuMat trainIdx_, distance_, allDist_;
    };

    void GpuMatcher::match(const ImageFeatures &features1, const ImageFeatures &features2, MatchesInfo& matches_info)
    {
        matches_info.matches.clear();

        BruteForceMatcher_GPU< L2<float> > matcher;
        
        descriptors1_.upload(features1.descriptors);
        descriptors2_.upload(features2.descriptors);

        vector< vector<DMatch> > pair_matches;

        // Find 1->2 matches
        matcher.knnMatch(descriptors1_, descriptors2_, trainIdx_, distance_, allDist_, 2);
        matcher.knnMatchDownload(trainIdx_, distance_, pair_matches);
        for (size_t i = 0; i < pair_matches.size(); ++i)
        {
            if (pair_matches[i].size() < 2)
                continue;
            const DMatch& m0 = pair_matches[i][0];
            const DMatch& m1 = pair_matches[i][1];

            CV_Assert(m0.queryIdx < static_cast<int>(features1.keypoints.size()));
            CV_Assert(m0.trainIdx < static_cast<int>(features2.keypoints.size()));

            if (m0.distance < (1.f - match_conf_) * m1.distance)
                matches_info.matches.push_back(m0);
        }

        // Find 2->1 matches
        pair_matches.clear();
        matcher.knnMatch(descriptors2_, descriptors1_, trainIdx_, distance_, allDist_, 2);
        matcher.knnMatchDownload(trainIdx_, distance_, pair_matches);
        for (size_t i = 0; i < pair_matches.size(); ++i)
        {
            if (pair_matches[i].size() < 2)
                continue;
            const DMatch& m0 = pair_matches[i][0];
            const DMatch& m1 = pair_matches[i][1];

            CV_Assert(m0.trainIdx < static_cast<int>(features1.keypoints.size()));
            CV_Assert(m0.queryIdx < static_cast<int>(features2.keypoints.size()));

            if (m0.distance < (1.f - match_conf_) * m1.distance)
                matches_info.matches.push_back(DMatch(m0.trainIdx, m0.queryIdx, m0.distance));
        }
    }
}

BestOf2NearestMatcher::BestOf2NearestMatcher(bool try_use_gpu, float match_conf, int num_matches_thresh1, int num_matches_thresh2)
{
    if (try_use_gpu && getCudaEnabledDeviceCount() > 0)
        impl_ = new GpuMatcher(match_conf);
    else
        impl_ = new CpuMatcher(match_conf);

    is_thread_safe_ = impl_->isThreadSafe();
    num_matches_thresh1_ = num_matches_thresh1;
    num_matches_thresh2_ = num_matches_thresh2;
}


void BestOf2NearestMatcher::match(const ImageFeatures &features1, const ImageFeatures &features2,
                                  MatchesInfo &matches_info)
{
    (*impl_)(features1, features2, matches_info);

    // Check if it makes sense to find homography
    if (matches_info.matches.size() < static_cast<size_t>(num_matches_thresh1_))
        return;
    // Construct point-point correspondences for homography estimation
    Mat src_points(1, matches_info.matches.size(), CV_32FC2);
    Mat dst_points(1, matches_info.matches.size(), CV_32FC2);
    for (size_t i = 0; i < matches_info.matches.size(); ++i)
    {
        const DMatch& m = matches_info.matches[i];

        Point2f p = features1.keypoints[m.queryIdx].pt;
        p.x -= features1.img_size.width * 0.5f;
        p.y -= features1.img_size.height * 0.5f;
        src_points.at<Point2f>(0, i) = p;

        p = features2.keypoints[m.trainIdx].pt;
        p.x -= features2.img_size.width * 0.5f;
        p.y -= features2.img_size.height * 0.5f;
        dst_points.at<Point2f>(0, i) = p;
    }

    // Find pair-wise motion
    matches_info.H = findHomography(src_points, dst_points, matches_info.inliers_mask, CV_RANSAC);

    // Find number of inliers
    matches_info.num_inliers = 0;
    for (size_t i = 0; i < matches_info.inliers_mask.size(); ++i)
        if (matches_info.inliers_mask[i])
            matches_info.num_inliers++;

    matches_info.confidence = matches_info.num_inliers / (8 + 0.3*matches_info.matches.size());

    // Check if we should try to refine motion
    if (matches_info.num_inliers < num_matches_thresh2_)
        return;

    // Construct point-point correspondences for inliers only
    src_points.create(1, matches_info.num_inliers, CV_32FC2);
    dst_points.create(1, matches_info.num_inliers, CV_32FC2);
    int inlier_idx = 0;
    for (size_t i = 0; i < matches_info.matches.size(); ++i)
    {
        if (!matches_info.inliers_mask[i])
            continue;

        const DMatch& m = matches_info.matches[i];

        Point2f p = features1.keypoints[m.queryIdx].pt;
        p.x -= features1.img_size.width * 0.5f;
        p.y -= features1.img_size.height * 0.5f;
        src_points.at<Point2f>(0, inlier_idx) = p;

        p = features2.keypoints[m.trainIdx].pt;
        p.x -= features2.img_size.width * 0.5f;
        p.y -= features2.img_size.height * 0.5f;
        dst_points.at<Point2f>(0, inlier_idx) = p;

        inlier_idx++;
    }

    // Rerun motion estimation on inliers only
    matches_info.H = findHomography(src_points, dst_points, CV_RANSAC);
}
