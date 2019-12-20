// This file is part of OpenMVG, an Open Multiple View Geometry C++ library.

// Copyright (c) 2012, 2013 Pierre MOULON.

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "openMVG/cameras/Camera_Pinhole.hpp"
#include "openMVG/cameras/Camera_Pinhole_Radial.hpp"
#include "openMVG/features/feature.hpp"
#include "openMVG/features/sift/SIFT_Anatomy_Image_Describer.hpp"
#include "openMVG/features/svg_features.hpp"
#include "openMVG/geometry/pose3.hpp"
#include "openMVG/image/image_io.hpp"
#include "openMVG/image/image_concat.hpp"
#include "openMVG/matching/indMatchDecoratorXY.hpp"
#include "openMVG/matching/regions_matcher.hpp"
#include "openMVG/matching/svg_matches.hpp"
#include "openMVG/multiview/triangulation.hpp"
#include "openMVG/numeric/eigen_alias_definition.hpp"
#include "openMVG/sfm/pipelines/sfm_robust_model_estimation.hpp"
#include "openMVG/sfm/sfm_data.hpp"
#include "openMVG/sfm/sfm_data_BA.hpp"
#include "openMVG/sfm/sfm_data_BA_ceres.hpp"
#include "openMVG/sfm/sfm_data_io.hpp"

#include "third_party/cmdLine/cmdLine.h"
#include "third_party/stlplus3/filesystemSimplified/file_system.hpp"

#include <iostream>
#include <string>
#include <utility>

using namespace openMVG;
using namespace openMVG::image;
using namespace openMVG::matching;
using namespace openMVG::cameras;
using namespace openMVG::geometry;
using namespace openMVG::sfm;
using namespace std;

/// Read intrinsic K matrix from a file (ASCII)
/// F 0 ppx
/// 0 F ppy
/// 0 0 1
bool readIntrinsic(const std::string & fileName, Mat3 & K);

int main(int argc, char **argv)
{
  CmdLine cmd;

  std::string sImgFilename1, sImgFilename2;
  std::string sIntrinsicFilename;
  std::string sOutPrefix;
  std::string sOutDir;

  cmd.add(make_option('i', sImgFilename1, "image file 1"));
  cmd.add(make_option('j', sImgFilename2, "image file 2"));
  cmd.add(make_option('k', sIntrinsicFilename, "intrinsic file"));
  cmd.add(make_option('p', sOutPrefix, "output prefix"));
  cmd.add(make_option('o', sOutDir, "output directory"));
  cmd.process(argc, argv);

  const string jpg_filenameL = sImgFilename1;
  const string jpg_filenameR = sImgFilename2;

  Image<unsigned char> imageL, imageR;
  ReadImage(jpg_filenameL.c_str(), &imageL);
  ReadImage(jpg_filenameR.c_str(), &imageR);

  //--
  // Detect regions thanks to an image_describer
  //--
  using namespace openMVG::features;
  std::unique_ptr<Image_describer> image_describer(new SIFT_Anatomy_Image_describer);
  std::map<IndexT, std::unique_ptr<features::Regions>> regions_perImage;
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
    string out_filename = sOutDir + "\\01_concat_" + sOutPrefix + ".jpg";
    WriteImage(out_filename.c_str(), concat);
  }

  //- Draw features on the two image (side by side)
  {
    Features2SVG
    (
      jpg_filenameL,
      { imageL.Width(), imageL.Height() },
      regionsL->Features(),
      jpg_filenameR,
      { imageR.Width(), imageR.Height() },
      regionsR->Features(),
      sOutDir + "\\02_features_" + sOutPrefix + ".svg"
    );
  }

  std::vector<IndMatch> vec_PutativeMatches;
  //-- Perform matching -> find Nearest neighbor, filtered with Distance ratio
  {
    // Find corresponding points
    matching::DistanceRatioMatch(
      0.8, matching::BRUTE_FORCE_L2,
      *regions_perImage.at(0).get(),
      *regions_perImage.at(1).get(),
      vec_PutativeMatches);

    IndMatchDecorator<float> matchDeduplicator(
      vec_PutativeMatches, featsL, featsR);
    matchDeduplicator.getDeduplicated(vec_PutativeMatches);

    std::cout
      << regions_perImage.at(0)->RegionCount() << " #Features on image A" << std::endl
      << regions_perImage.at(1)->RegionCount() << " #Features on image B" << std::endl
      << vec_PutativeMatches.size() << " #matches with Distance Ratio filter" << std::endl;

    // Draw correspondences after Nearest Neighbor ratio filter
    const bool bVertical = true;
    Matches2SVG
    (
      jpg_filenameL,
      { imageL.Width(), imageL.Height() },
      regionsL->GetRegionsPositions(),
      jpg_filenameR,
      { imageR.Width(), imageR.Height() },
      regionsR->GetRegionsPositions(),
      vec_PutativeMatches,
      sOutDir + "\\03_Matches_" + sOutPrefix + ".svg",
      bVertical
    );
  }

  // Essential geometry filtering of putative matches
  {
    Mat3 K;
    //read K from file
    if (!readIntrinsic(sIntrinsicFilename, K))
    {
      std::cerr << "Cannot read intrinsic parameters." << std::endl;
      return EXIT_FAILURE;
    }

    const Pinhole_Intrinsic
      camL(imageL.Width(), imageL.Height(), K(0, 0), K(0, 2), K(1, 2)),
      camR(imageR.Width(), imageR.Height(), K(0, 0), K(0, 2), K(1, 2));

    //A. prepare the corresponding putatives points
    Mat xL(2, vec_PutativeMatches.size());
    Mat xR(2, vec_PutativeMatches.size());
    for (size_t k = 0; k < vec_PutativeMatches.size(); ++k) {
      const PointFeature & imaL = featsL[vec_PutativeMatches[k].i_];
      const PointFeature & imaR = featsR[vec_PutativeMatches[k].j_];
      xL.col(k) = imaL.coords().cast<double>();
      xR.col(k) = imaR.coords().cast<double>();
    }

    //B. Compute the relative pose thanks to a essential matrix estimation
    const std::pair<size_t, size_t> size_imaL(imageL.Width(), imageL.Height());
    const std::pair<size_t, size_t> size_imaR(imageR.Width(), imageR.Height());
    RelativePose_Info relativePose_info;
    if (!robustRelativePose(&camL, &camR, xL, xR, relativePose_info, size_imaL, size_imaR, 256))
    {
      std::cerr << " /!\\ Robust relative pose estimation failure."
        << std::endl;
      return EXIT_FAILURE;
    }

    std::cout << "\nFound an Essential matrix:\n"
      << "\tprecision: " << relativePose_info.found_residual_precision << " pixels\n"
      << "\t#inliers: " << relativePose_info.vec_inliers.size() << "\n"
      << "\t#matches: " << vec_PutativeMatches.size()
      << std::endl;

    // Show Essential validated point
    const bool bVertical = true;
    InlierMatches2SVG
    (
      jpg_filenameL,
      { imageL.Width(), imageL.Height() },
      regionsL->GetRegionsPositions(),
      jpg_filenameR,
      { imageR.Width(), imageR.Height() },
      regionsR->GetRegionsPositions(),
      vec_PutativeMatches,
      relativePose_info.vec_inliers,
      sOutDir + "\\04_ACRansacEssential_" + sOutPrefix + ".svg",
      bVertical
    );

    std::cout << std::endl
      << "-- Rotation|Translation matrices: --" << "\n"
      << relativePose_info.relativePose.rotation() << "\n\n"
      << relativePose_info.relativePose.translation() << "\n" << std::endl;

    //C. Triangulate and check valid points
    // invalid points that do not respect cheirality are discarded (removed
    //  from the list of inliers).

    int iBAType = 2;
    const bool bSharedIntrinsic = (iBAType == 2 || iBAType == 3) ? true : false;

    // Setup a SfM scene with two view corresponding the pictures
    SfM_Data tiny_scene;
    tiny_scene.views[0].reset(new View("", 0, bSharedIntrinsic ? 0 : 1, 0, imageL.Width(), imageL.Height()));
    tiny_scene.views[1].reset(new View("", 1, bSharedIntrinsic ? 0 : 1, 1, imageR.Width(), imageR.Height()));
    // Setup intrinsics camera data
    switch (iBAType)
    {
    case 1: // Each view use it's own pinhole camera intrinsic
      tiny_scene.intrinsics[0].reset(new Pinhole_Intrinsic(imageL.Width(), imageL.Height(), K(0, 0), K(0, 2), K(1, 2)));
      tiny_scene.intrinsics[1].reset(new Pinhole_Intrinsic(imageR.Width(), imageR.Height(), K(0, 0), K(0, 2), K(1, 2)));
      break;
    case 2: // Shared pinhole camera intrinsic
      tiny_scene.intrinsics[0].reset(new Pinhole_Intrinsic(imageL.Width(), imageL.Height(), K(0, 0), K(0, 2), K(1, 2)));
      break;
    case 3: // Shared pinhole camera intrinsic with radial K3 distortion
      tiny_scene.intrinsics[0].reset(new Pinhole_Intrinsic_Radial_K3(imageL.Width(), imageL.Height(), K(0, 0), K(0, 2), K(1, 2)));
      break;
    default:
      std::cerr << "Invalid input number" << std::endl;
      return EXIT_FAILURE;
    }

    // Setup poses camera data
    const Pose3 pose0 = tiny_scene.poses[tiny_scene.views[0]->id_pose] = Pose3(Mat3::Identity(), Vec3::Zero());
    const Pose3 pose1 = tiny_scene.poses[tiny_scene.views[1]->id_pose] = relativePose_info.relativePose;

    // Init structure by inlier triangulation
    const Mat34 P1 = tiny_scene.intrinsics[tiny_scene.views[0]->id_intrinsic]->get_projective_equivalent(pose0);
    const Mat34 P2 = tiny_scene.intrinsics[tiny_scene.views[1]->id_intrinsic]->get_projective_equivalent(pose1);
    Landmarks & landmarks = tiny_scene.structure;
    for (size_t i = 0; i < relativePose_info.vec_inliers.size(); ++i) {
      const SIOPointFeature & LL = regionsL->Features()[vec_PutativeMatches[relativePose_info.vec_inliers[i]].i_];
      const SIOPointFeature & RR = regionsR->Features()[vec_PutativeMatches[relativePose_info.vec_inliers[i]].j_];
      // Point triangulation
      Vec3 X;
      TriangulateDLT(
        P1, LL.coords().cast<double>().homogeneous(),
        P2, RR.coords().cast<double>().homogeneous(), &X);
      // Reject point that is behind the camera
      if (Depth(pose0.rotation(), pose0.translation(), X) < 0 &&
        Depth(pose1.rotation(), pose1.translation(), X) < 0)
        continue;
      // Add a new landmark (3D point with it's 2d observations)
      landmarks[i].obs[tiny_scene.views[0]->id_view] = Observation(LL.coords().cast<double>(), vec_PutativeMatches[relativePose_info.vec_inliers[i]].i_);
      landmarks[i].obs[tiny_scene.views[1]->id_view] = Observation(RR.coords().cast<double>(), vec_PutativeMatches[relativePose_info.vec_inliers[i]].j_);
      landmarks[i].X = X;
    }
    Save(tiny_scene, sOutDir + "\\EssentialGeometry_start_" + sOutPrefix + ".ply", ESfM_Data(ALL));

    //D. Perform Bundle Adjustment of the scene

    Bundle_Adjustment_Ceres bundle_adjustment_obj;
    bundle_adjustment_obj.Adjust(tiny_scene,
      Optimize_Options(
        Intrinsic_Parameter_Type::NONE,
        Extrinsic_Parameter_Type::ADJUST_ALL,
        Structure_Parameter_Type::ADJUST_ALL));

    Mat3 R1 = tiny_scene.poses[0].rotation();
    Vec3 t1 = tiny_scene.poses[0].translation();
    Mat3 R2 = tiny_scene.poses[1].rotation();
    Vec3 t2 = tiny_scene.poses[1].translation();
    Mat3 R = R1 * R2.inverse();  /* posture of cam2 based on cam1 */
    Vec3 t = -R1 * R2.inverse() * t2 + t1;  /* center cam2 based on cam1 */

    { /* output R */
      std::string filename = sOutDir + "\\posture_" + sOutPrefix + ".txt";
      std::ofstream of(filename);
      of << R(0, 0) << " " << R(0, 1) << " " << R(0, 2) << std::endl;
      of << R(1, 0) << " " << R(1, 1) << " " << R(1, 2) << std::endl;
      of << R(2, 0) << " " << R(2, 1) << " " << R(2, 2) << std::endl;
      of.close();
    }
    { /* output t */
      std::string filename = sOutDir + "\\center_" + sOutPrefix + ".txt";
      std::ofstream of(filename);
      of << t(0) << std::endl << t(1) << std::endl << t(2) << std::endl;
      of.close();
    }

    std::ofstream of_X(sOutDir + "\\X_" + sOutPrefix + ".txt");
    std::ofstream of_x1(sOutDir + "\\x1_" + sOutPrefix + ".txt");
    std::ofstream of_x2(sOutDir + "\\x2_" + sOutPrefix + ".txt");
    std::ofstream of_desc1(sOutDir + "\\desc1_" + sOutPrefix + ".txt");
    std::ofstream of_desc2(sOutDir + "\\desc2_" + sOutPrefix + ".txt");
    for (Landmarks::const_iterator itr = tiny_scene.structure.cbegin();
      itr != tiny_scene.structure.cend(); ++itr)
    {
      Vec3 X = itr->second.X;
      Vec2 x1 = itr->second.obs.at(0).x;
      Vec2 x2 = itr->second.obs.at(1).x;
      const SIFT_Regions::DescriptorT &desc1
        = regionsL->Descriptors()[vec_PutativeMatches[relativePose_info.vec_inliers[itr->first]].i_];
      const SIFT_Regions::DescriptorT &desc2
        = regionsR->Descriptors()[vec_PutativeMatches[relativePose_info.vec_inliers[itr->first]].j_];

      X = R1 * X + t1;
      of_X << X(0) << " " << X(1) << " " << X(2) << std::endl;
      of_x1 << x1(0) << " " << x1(1) << std::endl;
      of_x2 << x2(0) << " " << x2(1) << std::endl;
      for (int i = 0; i < desc1.size(); ++i) {
        of_desc1 << int(desc1(i)) << " ";
      }
      of_desc1 << std::endl;
      for (int i = 0; i < desc2.size(); ++i) {
        of_desc2 << int(desc2(i)) << " ";
      }
      of_desc2 << std::endl;
    }
    of_X.close();
    of_x1.close();
    of_x2.close();
    of_desc1.close();
    of_desc2.close();
    
    Save(tiny_scene, sOutDir + "\\EssentialGeometry_refined_" + sOutPrefix + "_.ply", ESfM_Data(ALL));
  }
  return EXIT_SUCCESS;
}

bool readIntrinsic(const std::string & fileName, Mat3 & K)
{
  // Load the K matrix
  ifstream in;
  in.open( fileName.c_str(), ifstream::in);
  if (in.is_open())  {
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
