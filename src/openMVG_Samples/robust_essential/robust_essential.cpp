
// Copyright (c) 2012, 2013 Pierre MOULON.

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.


#include "openMVG/cameras/PinholeCamera.hpp"
#include "openMVG/image/image.hpp"
#include "openMVG/features/features.hpp"
#include "openMVG/matching/matcher_brute_force.hpp"
#include "openMVG/matching/indMatchDecoratorXY.hpp"
#include "openMVG/multiview/projection.hpp"
#include "openMVG/multiview/triangulation.hpp"
#include "openMVG_Samples/robust_essential/essential_estimation.hpp"

#include "nonFree/sift/SIFT_describer.hpp"
#include "openMVG_Samples/siftPutativeMatches/two_view_matches.hpp"

#include "third_party/stlplus3/filesystemSimplified/file_system.hpp"
#include "third_party/vectorGraphics/svgDrawer.hpp"

#include <string>
#include <iostream>

using namespace openMVG;
using namespace openMVG::matching;
using namespace svg;
using namespace std;

/// Read intrinsic K matrix from a file (ASCII)
/// F 0 ppx
/// 0 F ppy
/// 0 0 1
bool readIntrinsic(const std::string & fileName, Mat3 & K);

/// Export 3D point vector and camera position to PLY format
bool exportToPly(const std::vector<Vec3> & vec_points,
  const std::vector<Vec3> & vec_camPos,
  const std::string & sFileName);

/// Triangulate and export valid point as PLY (point in front of the cameras)
void triangulateAndSaveResult(
  const PinholeCamera & camL,
  const PinholeCamera & camR,
  const std::vector<size_t> & vec_inliers,
  const Mat & xL,
  const Mat & xR,
  std::vector<Vec3> & vec_3DPoints);

int main() {

  std::string sInputDir = stlplus::folder_up(string(THIS_SOURCE_DIR))
    + "/imageData/SceauxCastle/";
  const string jpg_filenameL = sInputDir + "100_7101.jpg";
  const string jpg_filenameR = sInputDir + "100_7102.jpg";

  Image<unsigned char> imageL, imageR;
  ReadImage(jpg_filenameL.c_str(), &imageL);
  ReadImage(jpg_filenameR.c_str(), &imageR);

  //--
  // Detect regions thanks to an image_describer
  //--
  using namespace openMVG::features;
  std::unique_ptr<Image_describer> image_describer(new SIFT_Image_describer);
  std::map<IndexT, std::unique_ptr<features::Regions> > regions_perImage;
  image_describer->Describe(imageL, regions_perImage[0]);
  image_describer->Describe(imageR, regions_perImage[1]);

  const SIFT_Regions* regionsL = dynamic_cast<SIFT_Regions*>(regions_perImage.at(0).get());
  const SIFT_Regions* regionsR = dynamic_cast<SIFT_Regions*>(regions_perImage.at(1).get());

  const PointFeatures
    featsL = regions_perImage.at(0)->GetRegionsPositions(),
    featsR = regions_perImage.at(1)->GetRegionsPositions();

  // Show both images side by side
  {
    Image<unsigned char> concat;
    ConcatH(imageL, imageR, concat);
    string out_filename = "01_concat.jpg";
    WriteImage(out_filename.c_str(), concat);
  }

  //- Draw features on the two image (side by side)
  {
    Image<unsigned char> concat;
    ConcatH(imageL, imageR, concat);

    //-- Draw features :
    for (size_t i=0; i < featsL.size(); ++i )  {
      const SIOPointFeature point = regionsL->Features()[i];
      DrawCircle(point.x(), point.y(), point.scale(), 255, &concat);
    }
    for (size_t i=0; i < featsR.size(); ++i )  {
      const SIOPointFeature point = regionsR->Features()[i];
      DrawCircle(point.x()+imageL.Width(), point.y(), point.scale(), 255, &concat);
    }
    string out_filename = "02_features.jpg";
    WriteImage(out_filename.c_str(), concat);
  }

  std::vector<IndMatch> vec_PutativeMatches;
  //-- Perform matching -> find Nearest neighbor, filtered with Distance ratio
  {
    // Define a matcher and a metric to find corresponding points
    typedef SIFT_Regions::DescriptorT DescriptorT;
    typedef L2_Vectorized<DescriptorT::bin_type> Metric;
    typedef ArrayMatcherBruteForce<DescriptorT::bin_type, Metric> MatcherT;
    // Distance ratio squared due to squared metric
    getPutativesMatches<DescriptorT, MatcherT>(
      ((SIFT_Regions*)regions_perImage.at(0).get())->Descriptors(),
      ((SIFT_Regions*)regions_perImage.at(1).get())->Descriptors(),
      Square(0.8), vec_PutativeMatches);

    IndMatchDecorator<float> matchDeduplicator(
            vec_PutativeMatches, featsL, featsR);
    matchDeduplicator.getDeduplicated(vec_PutativeMatches);

    // Draw correspondences after Nearest Neighbor ratio filter
    svgDrawer svgStream( imageL.Width() + imageR.Width(), max(imageL.Height(), imageR.Height()));
    svgStream.drawImage(jpg_filenameL, imageL.Width(), imageL.Height());
    svgStream.drawImage(jpg_filenameR, imageR.Width(), imageR.Height(), imageL.Width());
    for (size_t i = 0; i < vec_PutativeMatches.size(); ++i) {
      //Get back linked feature, draw a circle and link them by a line
      const SIOPointFeature L = regionsL->Features()[vec_PutativeMatches[i]._i];
      const SIOPointFeature R = regionsR->Features()[vec_PutativeMatches[i]._j];
      svgStream.drawLine(L.x(), L.y(), R.x()+imageL.Width(), R.y(), svgStyle().stroke("green", 2.0));
      svgStream.drawCircle(L.x(), L.y(), L.scale(), svgStyle().stroke("yellow", 2.0));
      svgStream.drawCircle(R.x()+imageL.Width(), R.y(), R.scale(),svgStyle().stroke("yellow", 2.0));
    }
    string out_filename = "03_siftMatches.svg";
    ofstream svgFile( out_filename.c_str() );
    svgFile << svgStream.closeSvgFile().str();
    svgFile.close();
  }

  // Essential geometry filtering of putative matches
  {
    Mat3 K;
    //read K from file
    if (!readIntrinsic(stlplus::create_filespec(sInputDir,"K","txt"), K))
    {
      std::cerr << "Cannot read intrinsic parameters." << std::endl;
      return EXIT_FAILURE;
    }

    //A. prepare the corresponding putatives points
    Mat xL(2, vec_PutativeMatches.size());
    Mat xR(2, vec_PutativeMatches.size());
    for (size_t k = 0; k < vec_PutativeMatches.size(); ++k)  {
      const PointFeature & imaL = featsL[vec_PutativeMatches[k]._i];
      const PointFeature & imaR = featsR[vec_PutativeMatches[k]._j];
      xL.col(k) = imaL.coords().cast<double>();
      xR.col(k) = imaR.coords().cast<double>();
    }

    //B. robust estimation of the essential matrix
    std::vector<size_t> vec_inliers;
    Mat3 E;
    std::pair<size_t, size_t> size_imaL(imageL.Width(), imageL.Height());
    std::pair<size_t, size_t> size_imaR(imageR.Width(), imageR.Height());
    double thresholdE = 0.0, NFA = 0.0;
    if (robustEssential(
      K, K,         // intrinsics
      xL, xR,       // corresponding points
      &E,           // essential matrix
      &vec_inliers, // inliers
      size_imaL,    // Left image size
      size_imaR,    // Right image size
      &thresholdE,  // Found AContrario Theshold
      &NFA,         // Found AContrario NFA
      std::numeric_limits<double>::infinity()))
    {
      std::cout << "\nFound an Essential matrix under the confidence threshold of: "
        << thresholdE << " pixels\n\twith: " << vec_inliers.size() << " inliers"
        << " from: " << vec_PutativeMatches.size()
        << " putatives correspondences"
        << std::endl;

      // Show Essential validated point
      svgDrawer svgStream( imageL.Width() + imageR.Width(), max(imageL.Height(), imageR.Height()));
      svgStream.drawImage(jpg_filenameL, imageL.Width(), imageL.Height());
      svgStream.drawImage(jpg_filenameR, imageR.Width(), imageR.Height(), imageL.Width());
      for ( size_t i = 0; i < vec_inliers.size(); ++i)  {
        const SIOPointFeature & LL = regionsL->Features()[vec_PutativeMatches[vec_inliers[i]]._i];
        const SIOPointFeature & RR = regionsR->Features()[vec_PutativeMatches[vec_inliers[i]]._j];
        const Vec2f L = LL.coords();
        const Vec2f R = RR.coords();
        svgStream.drawLine(L.x(), L.y(), R.x()+imageL.Width(), R.y(), svgStyle().stroke("green", 2.0));
        svgStream.drawCircle(L.x(), L.y(), LL.scale(), svgStyle().stroke("yellow", 2.0));
        svgStream.drawCircle(R.x()+imageL.Width(), R.y(), RR.scale(),svgStyle().stroke("yellow", 2.0));
      }
      string out_filename = "04_ACRansacEssential.svg";
      ofstream svgFile( out_filename.c_str() );
      svgFile << svgStream.closeSvgFile().str();
      svgFile.close();

      //C. Extract the rotation and translation of the camera from the essential matrix
      Mat3 R;
      Vec3 t;
      if (!estimate_Rt_fromE(K, K, xL, xR, E, vec_inliers,
        &R, &t))
      {
        std::cerr << " /!\\ Failed to compute initial R|t for the initial pair" << std::endl;
        return false;
      }
      std::cout << std::endl
        << "-- Rotation|Translation matrices: --" << std::endl
        << R << std::endl << std::endl << t << std::endl;

      // Build Left and Right Camera
      PinholeCamera camL(K, Mat3::Identity(), Vec3::Zero());
      PinholeCamera camR(K, R, t);

      //C. Triangulate and export inliers as a PLY scene
      std::vector<Vec3> vec_3DPoints;
      triangulateAndSaveResult(
        camL, camR,
        vec_inliers,
        xL, xR, vec_3DPoints);

      // Export as PLY (camera pos + 3Dpoints)
      std::vector<Vec3> vec_camPos;
      vec_camPos.push_back( camL._C );
      vec_camPos.push_back( camR._C );
      exportToPly(vec_3DPoints, vec_camPos, "EssentialGeometry.ply");

    }
    else  {
      std::cout << "ACRANSAC was unable to estimate a rigid essential matrix"
        << std::endl;
    }
  }
  return EXIT_SUCCESS;
}

bool readIntrinsic(const std::string & fileName, Mat3 & K)
{
  // Load the K matrix
  ifstream in;
  in.open( fileName.c_str(), ifstream::in);
  if(in.is_open())  {
    for (int j=0; j < 3; ++j)
      for (int i=0; i < 3; ++i)
        in >> K(j,i);
  }
  else  {
    std::cerr << std::endl
      << "Invalid input K.txt file" << std::endl;
    return false;
  }
  return true;
}

/// Export 3D point vector and camera position to PLY format
bool exportToPly(const std::vector<Vec3> & vec_points,
  const std::vector<Vec3> & vec_camPos,
  const std::string & sFileName)
{
  std::ofstream outfile;
  outfile.open(sFileName.c_str(), std::ios_base::out);

  outfile << "ply"
    << '\n' << "format ascii 1.0"
    << '\n' << "element vertex " << vec_points.size()+vec_camPos.size()
    << '\n' << "property float x"
    << '\n' << "property float y"
    << '\n' << "property float z"
    << '\n' << "property uchar red"
    << '\n' << "property uchar green"
    << '\n' << "property uchar blue"
    << '\n' << "end_header" << std::endl;

  for (size_t i=0; i < vec_points.size(); ++i)  {
      outfile << vec_points[i].transpose()
      << " 255 255 255" << "\n";
  }

  for (size_t i=0; i < vec_camPos.size(); ++i)  {
    outfile << vec_camPos[i].transpose()
      << " 0 255 0" << "\n";
  }
  outfile.flush();
  bool bOk = outfile.good();
  outfile.close();
  return bOk;
}

/// Triangulate and export valid point as PLY (point in front of the cameras)
void triangulateAndSaveResult(
  const PinholeCamera & camL,
  const PinholeCamera & camR,
  const std::vector<size_t> & vec_inliers,
  const Mat & xL,
  const Mat & xR,
  std::vector<Vec3> & vec_3DPoints)
{
  std::vector<double> vec_residuals;
  size_t nbPointWithNegativeDepth = 0;
  for (size_t k = 0; k < vec_inliers.size(); ++k) {
    const Vec2 & xL_ = xL.col(vec_inliers[k]);
    const Vec2 & xR_ = xR.col(vec_inliers[k]);

    Vec3 X = Vec3::Zero();
    TriangulateDLT(camL._P, xL_, camR._P, xR_, &X);

    // Compute residual:
    double dResidual = (camL.Residual(X, xL_) + camR.Residual(X, xR_))/2.0;
    vec_residuals.push_back(dResidual);
    if (camL.Depth(X) < 0 && camR.Depth(X) < 0) {
      ++nbPointWithNegativeDepth;
    }
    else  {
      vec_3DPoints.push_back(X);
    }
  }
  if (nbPointWithNegativeDepth > 0)
  {
    std::cout << nbPointWithNegativeDepth
      << " correspondence(s) with negative depth have been discarded."
      << std::endl;
  }
  // Display some statistics of reprojection errors
  float dMin, dMax, dMean, dMedian;
  minMaxMeanMedian<float>(vec_residuals.begin(), vec_residuals.end(),
                        dMin, dMax, dMean, dMedian);

  std::cout << std::endl
    << "Essential matrix estimation, residuals statistics:" << "\n"
    << "\t-- Residual min:\t" << dMin << std::endl
    << "\t-- Residual median:\t" << dMedian << std::endl
    << "\t-- Residual max:\t "  << dMax << std::endl
    << "\t-- Residual mean:\t " << dMean << std::endl;
}
