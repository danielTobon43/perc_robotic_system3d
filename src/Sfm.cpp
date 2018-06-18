﻿//***********************************************
//HEADERS
//***********************************************
#include "include/Sfm.h"

/********************************************
                  PIPELINE
********************************************/
bool StructFromMotion::run_SFM(){

  std::cout << "************************************************" << std::endl;
  std::cout << "              3D MAPPING                        " << std::endl;
  std::cout << "************************************************" << std::endl;

  if(nImages.size() <= 0) {
      std::cerr << "No images to work on." << std::endl;
      return false;
  }

  nCameraPoses.resize(nImages.size()); //Define a fixed size for vector(cv::Matx34f) camera poses

  // **(1) FEATURE DETECTION AND EXTRACTION - ALL IMAGES
  bool success = extractFeature();
  if(not success){
      std::cerr << "No could find features. corrupt images" << std::endl;
      return false;
  }

  // **(2) PRINT INPUT IMAGES
  for(unsigned int i=0;i<nImages.size();i++){
      cv::namedWindow("Input images",cv::WINDOW_NORMAL);
      cv::resizeWindow("Input images",nImages[i].cols,nImages[i].rows);
      cv::moveWindow("Input images",0,0);
      cv::imshow("Input images",nImages[i]);
      cv::waitKey(100);
  }
  cv::destroyWindow("Input images");

  // **(3) BASE RECONSTRUCTION
  success= baseTriangulation();
  if(not success){
      std::cerr << "No could find a good pair for initial reconstruction" << std::endl;
      return false;
  }
  /*
  // **(6) VISUALIZER 3D MAPPING
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud;
  fromPoint3DToPCLCloud(nReconstructionCloud,cloud);
  pcl::visualization::CloudViewer viewer("MAP3D");
  viewer.showCloud(cloud,"cloudSFM");

  while(!viewer.wasStopped ()) { // Display the visualiser until 'q' key is pressed
  }
  */

  // **(4) ADD MORE VIEWS
  success = addMoreViews();
  if(not success){
      std::cerr << "Could not add more views" << std::endl;
      return false;
  }

  // **(5) PMVS2
  PMVS2();

  // **(6) VISUALIZER 3D MAPPING
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud;
  fromPoint3DToPCLCloud(nReconstructionCloud,cloud);
  pcl::visualization::CloudViewer viewer("MAP3D");
  viewer.showCloud(cloud,"cloudSFM");

  std::cout << "Press q to continue --> [DENSE PROCESS]..." << std::endl;
  while(!viewer.wasStopped ()) { // Display the visualiser until 'q' key is pressed
  }

  // **(7) DENSIFIYING POINTCLOUD
  std::system("./pmvs2 denseCloud/ options.txt");

  // **(8) CONVERT PLY TO PCD
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloudPLY (new pcl::PointCloud<pcl::PointXYZRGB>);
  pcl::PLYReader ply;
  ply.read("denseCloud/models/options.txt.ply",*cloudPLY);
  pcl::io::savePCDFile("MAP3D.pcd",*cloudPLY);
  cloudPCL = cloudPLY;

  // **(9) VISUALIZER DENSE 3D MAPPING
  pcl::visualization::CloudViewer viewer2("DENSE MAP3D");
  viewer2.showCloud(cloudPLY,"denseCloud");

  while(!viewer2.wasStopped ()) { // Display the visualiser until 'q' key is pressed
  }

  std::cout << "Press q to continue --> [MESH CLOUD]..." << std::endl;
  // **(10) MESHING POINTCLOUD
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloudXYZ(new pcl::PointCloud<pcl::PointXYZ>);
  pcl::io::loadPCDFile("MAP3D.pcd",*cloudXYZ);

  pcl::PointCloud<pcl::PointXYZ>::Ptr filterCloud(new pcl::PointCloud<pcl::PointXYZ>);
  pcl::PolygonMesh mesh;

  cloudPointFilter(cloudXYZ,filterCloud);
  removePoints(cloudXYZ,filterCloud);
  create_mesh(filterCloud,mesh);
  vizualizeMesh(mesh);

  std::cout << "************************************************" << std::endl;
  std::cout << "************************************************" << std::endl;

  return true;
}

/********************************************
 FUNCTIONS
********************************************/

//===============================================
//IMAGES LOAD
//===============================================
bool StructFromMotion::imagesLOAD(const std::string&  directoryPath){

  std::cout << "Getting images..." << std::flush;
  pathImages = directoryPath;
  boost::filesystem::path dirPath(directoryPath);

  if(not boost::filesystem::exists(dirPath) or not boost::filesystem::is_directory(dirPath)){
      std::cerr << "Cannot open directory: " << directoryPath << std::endl;
      return false;
  }

  for(boost::filesystem::directory_entry& x : boost::filesystem::directory_iterator(dirPath)){
      std::string extension = x.path().extension().string();
      boost::algorithm::to_lower(extension);
      if(extension == ".jpg" or extension == ".png"){
          nImagesPath.push_back(x.path().string());
      }
  }

  /*sort data set of images in vector*/
  std::sort(nImagesPath.begin(), nImagesPath.end());

  if(nImagesPath.size() <= 0){
       std::cerr << "Unable to find valid files in images directory (\"" << directoryPath << "\")."
                 << std::endl;
       return false;
  }

  std::cout << "Found " << nImagesPath.size() << " image files in directory." << std::endl;
  /*Read input images and save them in a images vector*/
  for(const std::string& imageFilename : nImagesPath){
      //cv::COLOR_BGR2GRAY
      cv::Mat img   = cv::imread(imageFilename,0); //Read input image
      cv::Mat temp = img.clone(); //Copy image to temp variable

      cv::Mat resize1,resize2,GaussianBlur;
      cv::resize(temp,resize1,cv::Size(640,480),0.0,0.0); //Define a size of 640x480

     // cv::resize(resize1,resize2,cv::Size(),0.75,0.75); //Define a size of 640(75%)x480(75%)
     // cv::GaussianBlur(resize1,GaussianBlur, cv::Size(3,3),0,0); //Apply a filter gaussian for noise

      nImages.push_back(resize1); //Save input image in nImages vector

      if(nImages.back().empty()) {
          std::cerr << "[x]"<<"\n" <<"Unable to read image from file: " << imageFilename << std::endl;
          return false;
      }
  }

  if(nImages.size()<2){
      std::cerr << "Sorry. is not enough images, 6 minimum" << std::endl;
      return false;
      }

  return true;
}

//===============================================
//GET CAMERA MATRIX
//===============================================
bool StructFromMotion::getCameraMatrix(const std::string str){

    std::cout << "Getting camera matrix..." << std::endl;
    cv::Mat intrinsics;
    cv::Mat cameraDistCoeffs;

    /*Read camera calobration file*/
    cv::FileStorage fs(str, cv::FileStorage::READ);

    /*Get data from tags: Camera_Matrix and Distortion_Coefficients*/
    fs["Camera_Matrix"] >> intrinsics;
    fs["Distortion_Coefficients"] >> cameraDistCoeffs;

    if(intrinsics.empty() or intrinsics.at<float>(2,0) !=0){
        std::cerr << "Error: no found or invalid camera calibration file.xml" << std::endl;
        return false;
    }

    /*Fill local variables with input data*/
    cameraMatrix.K = intrinsics;                  //Matrix K (3x3)
    cameraMatrix.distCoef = cameraDistCoeffs;     //Distortion coefficients (1x5)
    cameraMatrix.invK = inverse(intrinsics);      //Inverse matrix K
    cameraMatrix.fx = intrinsics.at<float>(0,0);  //Focal length in x
    cameraMatrix.fy = intrinsics.at<float>(1,1);  //Focal length in y
    cameraMatrix.cx = intrinsics.at<float>(0,2);  //Center image in x
    cameraMatrix.cy = intrinsics.at<float>(1,2);  //Center image in y

    std::cout << "Camera matrix:" << "\n" << intrinsics << std::endl;
    std::cout <<"Distortion coefficients: "<< std::endl;
    std::cout << cameraDistCoeffs << std::endl;

    if(cameraMatrix.K.empty()){
        std::cerr << "Could not load local variables with camera calibration file data" << std::endl;
        return false;
    }else{
        return true;
    }
}

//===============================================
// Extract feature
//===============================================
bool StructFromMotion::extractFeature(){

  std::cout << "Getting features from all images..." << std::endl;
  nFeatureImages.resize(nImages.size());

  for(size_t n=0;n<nImages.size();n++){

      nFeatureImages[n] = getFeature(nImages[n]);
      Feature ft = nFeatureImages[n];
      std::cout << "Image:" << n << " --> " << ft.pt2D.size() << " keypoints" << std::endl;
   }

  if(nFeatureImages.empty()){return false;}
  return true;
}

//===============================================
//Get Feature
//===============================================
Feature StructFromMotion::getFeature(const cv::Mat& image){

    Feature feature;
    detector->detect(image,feature.kps);
    detector->compute(image,feature.kps,feature.descriptors);
    keypoints2F(feature.kps,feature.pt2D);
    return feature;
}

//===============================================
//FUNCTION: KEYPOINTS TO POINTS2D
//===============================================
void StructFromMotion::keypoints2F(Keypoints& keypoints, Points2f& points2D){

  points2D.clear();
  for(const auto& kps: keypoints){
         points2D.push_back(kps.pt);
   }
}

//===============================================
//FUNCTION: BASE RECONSTRUCTION
//===============================================
bool StructFromMotion::baseTriangulation(){

  std::map<float,std::pair<int,int>> bestViews = findBestPair();

  for(std::pair<const float,std::pair<int,int>>& bestpair : bestViews){

      int queryImage = bestpair.second.first;
      int trainImage = bestpair.second.second;

      //Matching bestMatch = getMatching(nFeatureImages[queryImage],nFeatureImages[trainImage]);

      Matching bestMatch;
      MatchFeatures(queryImage,trainImage,&bestMatch);

      std::cout << "Best pair:" << "["<< queryImage << "," << trainImage<<"]" << " has:"
                << bestMatch.size() << " matches" << " and " << bestpair.first << " inliers." << std::endl;

      cv::Matx34f Pleft  = cv::Matx34f::eye();
      cv::Matx34f Pright = cv::Matx34f::eye();

      std::cout << "Estimating camera pose with Essential Matrix..." << std::flush;
      bool success = getCameraPose(cameraMatrix,bestMatch,nFeatureImages[queryImage],
                                   nFeatureImages[trainImage],Pleft,Pright);

      if(not success){
         std::cout << "[X]" << std::endl;
         std::cerr << "Failed. stereo view could not be obtained " << queryImage << "," << trainImage
                   << ", something wrong." << std::endl;
         continue;
      }

      std::cout << "Camera:" << queryImage << "\n" << Pleft << std::endl;
      std::cout << "Camera:" << trainImage <<"\n" << Pright << std::endl;
      std::cout << "Showing matches between "<< "image:" << queryImage << " and image:"
                << trainImage << std::endl;

      cv::Mat matchImage;
      cv::destroyWindow("Matching pairs");
      cv::drawMatches(nImages[queryImage],nFeatureImages[queryImage].kps,nImages[trainImage],
                      nFeatureImages[trainImage].kps,bestMatch,matchImage,
                      cv::Scalar::all(-1),cv::Scalar::all(-1),std::vector<char>(),2);
      cv::namedWindow("Best pair matching",cv::WINDOW_NORMAL);
      cv::resizeWindow("Best pair matching",matchImage.cols,matchImage.rows);
      cv::putText(matchImage, "Image " + std::to_string(queryImage) + "                        "+
                  "                     "+  "                     " + + "          " +
                  "Image" + std::to_string(trainImage),
                  cv::Point(10,matchImage.rows-10),cv::FONT_ITALIC,0.5,cv::Scalar(0,255,0),1);
      cv::moveWindow("Best pair matching",700,0);
      cv::imshow("Best pair matching", matchImage);
      cv::waitKey(0);
      cv::destroyWindow("Best pair matching");

      std::vector<Point3D> pointcloud;

      success = triangulateViews(nFeatureImages[queryImage],nFeatureImages[trainImage],
                                 Pleft,Pright,bestMatch,cameraMatrix,
                                 std::make_pair(queryImage,trainImage),pointcloud);

      if(not success){
          std::cerr << "Could not triangulate image:" << queryImage << " and image:"<< trainImage
                    << std::endl;
          continue;
      }

      nReconstructionCloud = pointcloud;

      nCameraPoses[queryImage] = Pleft;
      nCameraPoses[trainImage] = Pright;

      nDoneViews.insert(queryImage);
      nDoneViews.insert(trainImage);

      nGoodViews.insert(queryImage);
      nGoodViews.insert(trainImage);

      break;
  }  
  adjustCurrentBundle();
  return true;
}

//===============================================
//BEST PAIR FOR BASELINE
//===============================================
std::map<float,std::pair<int,int>>  StructFromMotion::findBestPair(){

  std::cout << "Getting best two views for baseline..." << std::flush;
  std::map<float,std::pair<int,int>> numInliers;
  const size_t numImg = nImages.size();

  for(int queryImage=0;queryImage<numImg-1;queryImage++) {
     for(int trainImage=queryImage+1;trainImage<numImg;trainImage++){

        //Matching correspondences = getMatching(nFeatureImages[queryImage],nFeatureImages[trainImage]);

        std::vector<cv::DMatch> correspondences;
        MatchFeatures(queryImage,trainImage,&correspondences);

        Feature alignedQ,alignedT;
        AlignedPointsFromMatch(nFeatureImages[queryImage],nFeatureImages[trainImage],correspondences,
                               alignedQ,alignedT);
        //0.004 * maxVal

        cv::Mat matchImage;
        cv::drawMatches(nImages[queryImage],nFeatureImages[queryImage].kps,nImages[trainImage],
                        nFeatureImages[trainImage].kps,correspondences,matchImage,
                        cv::Scalar::all(-1),cv::Scalar::all(-1),std::vector<char>(),2);

        cv::namedWindow("Matching pairs",cv::WINDOW_NORMAL);
        cv::resizeWindow("Matching pairs",matchImage.cols,matchImage.rows);
        cv::moveWindow("Matching pairs",700,0);
        cv::putText(matchImage, "Image" + std::to_string(queryImage)+ "                        "+
                    "                     " +  "                     " + "     " +
                    +" Image" + std::to_string(trainImage),
                     cv::Point(10,matchImage.rows-10),cv::FONT_ITALIC,0.5,cv::Scalar(0,255,0),1);
        cv::imshow("Matching pairs", matchImage);
        cv::waitKey(1);

        if(correspondences.size()<30) continue;
        int N = findHomographyInliers(nFeatureImages[queryImage],nFeatureImages[trainImage],
                                                   correspondences);

        if(N < 60) continue;

        float poseInliersRatio = (float)N/(float)correspondences.size();
        std::cout << "pair:" << "[" << queryImage << "," << trainImage << "]" << " has:" << poseInliersRatio
                  << " pose inliers ratio."<< std::endl;

        numInliers[poseInliersRatio]=std::make_pair(queryImage,trainImage);
     }
  }
  return numInliers;
}

//===============================================
//FUNCTION: FEATURE MATCHING
//===============================================
Matching StructFromMotion::getMatching(const Feature& queryImage,const Feature& trainImage){

  /*Knn matching*/
  Matching goodMatches;
  std::vector<Matching> initialMatching;
  matcher->knnMatch(queryImage.descriptors,trainImage.descriptors,initialMatching,2);

  /*RATIO-TEST FILTER*/
  for(unsigned i = 0; i < initialMatching.size(); i++) {
      if(initialMatching[i][0].distance <= NN_MATCH_RATIO * initialMatching[i][1].distance) {
          goodMatches.push_back(initialMatching[i][0]);
      }
  }

  return goodMatches;
}

//===============================================
//FUNCTION: FIND HOMOGRAPHY INLIERS
//===============================================
int StructFromMotion::findHomographyInliers(const Feature& queryFeature,const Feature& trainFeature,const Matching& matches){

  Feature alignedQuery,alignedTrain;
  AlignedPointsFromMatch(queryFeature,trainFeature,matches,alignedQuery,alignedTrain);

  double minVal,maxVal;
  cv::minMaxIdx(alignedQuery.pt2D,&minVal,&maxVal);

  cv::Mat matrixH(3,3,CV_32FC3);
  cv::Mat inliersMask;

  //0.004 * maxVal

  matrixH = cv::findHomography(alignedQuery.pt2D,alignedTrain.pt2D,CV_RANSAC,
                               10.0,inliersMask);

  int numInliers = cv::countNonZero(inliersMask);
  return numInliers;
}

//===============================================
//FUNCTION: ALIGNED POINTS
//===============================================
void StructFromMotion::AlignedPointsFromMatch(const Feature& queryImg,const Feature& trainImg,const Matching& matches,Feature& alignedL,Feature& alignedR){

   std::vector<int> leftId,rightId;
   AlignedPoints(queryImg,trainImg,matches,alignedL,alignedR,leftId,rightId);  
}

void StructFromMotion::AlignedPoints(const Feature& queryImg,const Feature& trainImg,const Matching& matches, Feature& alignedL, Feature& alignedR,std::vector<int>& idLeftOrigen,std::vector<int>& idRightOrigen){

      //align left and right point sets
      for(unsigned int i=0;i<matches.size();i++){

        alignedL.pt2D.push_back(queryImg.pt2D[matches[i].queryIdx]);
        alignedR.pt2D.push_back(trainImg.pt2D[matches[i].trainIdx]);

        idLeftOrigen.push_back(matches[i].queryIdx);
        idRightOrigen.push_back(matches[i].trainIdx);
      }
}

bool StructFromMotion::getCameraPose(const CameraData& intrinsics,const Matching & matches,
                                     const Feature& left, const Feature& right,
                                     cv::Matx34f& Pleft, cv::Matx34f& Pright){

  if (intrinsics.K.empty()) {

      std::cerr << "Intrinsics matrix (K) must be initialized." << std::endl;
      return false;
  }

  Feature alignedLeft,alignedRight;
  AlignedPointsFromMatch(left,right,matches,alignedLeft,alignedRight);

  // ESSENTIAL MATRIX
  cv::Mat mask;
  cv::Mat E = cv::findEssentialMat(alignedLeft.pt2D, alignedRight.pt2D,
                                   intrinsics.K,CV_RANSAC,0.999, 1.0,mask);

  // CAMERA POSE -> Rotation and Traslation (MOTION ESTIMATION)
  cv::Mat R,T;
  cv::Point2d pp = cv::Point2d(intrinsics.cx,intrinsics.cy);
  cv::recoverPose(E,alignedLeft.pt2D, alignedRight.pt2D,R,T,intrinsics.fx,pp,mask);

  bool success = CheckCoherentRotation(R);

  if(not success){

      std::cerr << "Bad rotation." << std::endl;
      return false;
  }

  //Rotational element in a 3x4 matrix
  const cv::Rect ROT(0, 0, 3, 3);
  //Translational element in a 3x4 matrix
  const cv::Rect TRA(3, 0, 1, 3);

  Pleft  = cv::Matx34f::eye();
  R.copyTo(cv::Mat(3, 4, CV_32FC1, Pright.val)(ROT));
  T.copyTo(cv::Mat(3, 4, CV_32FC1, Pright.val)(TRA));

  return success;
}

//===============================================
//FUNCTION: CHECK ROTATION MATRIX (Must be det=1)
//===============================================
bool StructFromMotion::CheckCoherentRotation(cv::Mat& R){

   if(fabsf(determinante(R))-1.0 > 1e-07) {

      std::cout << "det(R) != +-1.0, this is not a rotation matrix" << std::endl;
      return false;
    }
    return true;
}

//===============================================
//FUNCTION: TRIANGULATE VIEWS
//===============================================
bool StructFromMotion::triangulateViews(const Feature& query,const Feature& train,const cv::Matx34f& P1,const cv::Matx34f& P2,const Matching& matches,const CameraData& matrixK,const std::pair<int,int>& pair,std::vector<Point3D>& pointcloud){

  std::cout << "** IMAGE COORDINATE - CAMERA COORDINATE CONVERTION **" << std::endl;

  pointcloud.clear();

  Feature alignedQuery,alignedTrain;
  std::vector<int> leftBackReference,rightBackReference;
  AlignedPoints(query,train,matches,alignedQuery,alignedTrain,
                                  leftBackReference,rightBackReference);

  // NORMALIZE IMAGE COORDINATE TO CAMERA COORDINATE (pixels --> metric)
  std::cout << "Normalizing points..." << std::flush;
  cv::Mat normalizedLeftPts,normalizedRightPts;
  cv::undistortPoints(alignedQuery.pt2D, normalizedLeftPts, matrixK.K,matrixK.distCoef);
  cv::undistortPoints(alignedTrain.pt2D, normalizedRightPts, matrixK.K, matrixK.distCoef);
  std::cout << "[DONE]" << std::endl;

  // TRIANGULATE POINTS
  std::cout << "Triangulating points..." << std::flush;
  cv::Mat pts3dHomogeneous;
  cv::triangulatePoints(P1,P2,normalizedLeftPts,normalizedRightPts,pts3dHomogeneous);
  std::cout << "[DONE]" << std::endl;

  std::cout << "** CAMERA COORDINATE - WORLD COORDINATE CONVERTION **" << std::endl;

  // CONVERTION CAMERA COORDINATE - WORLD COORDINATE
  std::cout << "Converting points to world coordinate..." << std::flush;
  cv::Mat pts3d;
  cv::convertPointsFromHomogeneous(pts3dHomogeneous.t(),pts3d);
  std::cout << "[DONE]" << std::endl;

  cv::Mat rvecLeft;
  cv::Rodrigues(P1.get_minor<3,3>(0,0),rvecLeft);
  cv::Mat tvecLeft(P1.get_minor<3,1>(0,3));

  Points2f projectedLeft(alignedQuery.pt2D.size());
  cv::projectPoints(pts3d,rvecLeft,tvecLeft,matrixK.K,matrixK.distCoef,projectedLeft);

  cv::Mat rvecRight;
  cv::Rodrigues(P2.get_minor<3,3>(0,0),rvecRight);
  cv::Mat tvecRight(P2.get_minor<3,1>(0,3));

  Points2f projectedRight(alignedTrain.pt2D.size());
  cv::projectPoints(pts3d,rvecRight,tvecRight,matrixK.K,matrixK.distCoef,projectedRight);

  std::cout << "Creating a pointcloud vector..." << std::flush;
  const float MIN_REPROJECTION_ERROR = 8.0f; //Maximum 10-pixel allowed re-projection error

  for(int i = 0; i < pts3d.rows; i++){

      //check if point reprojection error is small enough

      const float queryError = cv::norm(projectedLeft[i]  - alignedQuery.pt2D[i]);
      const float trainError = cv::norm(projectedRight[i] - alignedTrain.pt2D[i]);

      if(MIN_REPROJECTION_ERROR < queryError or
         MIN_REPROJECTION_ERROR < trainError) continue;

          Point3D p;
          p.pt = cv::Point3f(pts3d.at<float>(i, 0),
                             pts3d.at<float>(i, 1),
                             pts3d.at<float>(i, 2));

          //use back reference to point to original Feature in images
          p.idxImage[pair.first]  = leftBackReference[i];
          p.idxImage[pair.second] = rightBackReference[i];
          p.pt2D[pair.first]=nFeatureImages[pair.first].pt2D[leftBackReference[i]];
          p.pt2D[pair.second]=nFeatureImages[pair.second].pt2D[rightBackReference[i]];

          pointcloud.push_back(p);
  }

  std::cout << "[DONE]" << std::endl;
  std::cout << "New triangulated points: " << pointcloud.size() << " 3d pts" << std::endl;
  return true;
}

//===============================================
//BUNDLE ADJUSTMENT
//===============================================
void StructFromMotion::adjustCurrentBundle() {

  std::cout << "Bundle adjuster..." << std::endl;
  BundleAdjustment::adjustBundle(nReconstructionCloud,nCameraPoses,cameraMatrix,nFeatureImages);

}

//===============================================
//FUNCTION: ADD MORE VIEWS
//===============================================
bool StructFromMotion::addMoreViews(){

      std::vector<cv::Point3f> points3D;
      std::vector<cv::Point2f> points2D;

      for(int NEW_VIEW = 0;NEW_VIEW<nImages.size();NEW_VIEW++){

          //if(nDoneViews.count(NEW_VIEW)==1) continue; //Skip done views
          if(nDoneViews.find(NEW_VIEW)!=nDoneViews.end())continue;;

          std::cout <<"\n"<< "===================================="<< std::endl;
          std::cout << "ESTIMATING MORE CAMERAS PROJECTION..." << std::endl;
          std::cout << "Extracting 2d3d correspondences..." << std::endl;

          Matching bestMatches;
          int DONE_VIEW;
          find2D3DMatches(NEW_VIEW,points3D,points2D,bestMatches,DONE_VIEW);
          std::cout << "Adding " << NEW_VIEW << " to existing "
                    << cv::Mat(std::vector<int>(nDoneViews.begin(), nDoneViews.end())).t() << std::endl;
          nDoneViews.insert(NEW_VIEW);

          std::cout << "Estimating camera pose..." << std::endl;
          cv::Matx34f newCameraPose = cv::Matx34f::eye();         
          bool success = findCameraPosePNP(cameraMatrix,points3D,points2D,newCameraPose);

          if(not success){
             continue;
          }                   

          nCameraPoses[NEW_VIEW] = newCameraPose;
          std::vector<Point3D> new_triangulated;

          int queryImage,trainImage;

          if(NEW_VIEW < DONE_VIEW){
              queryImage = NEW_VIEW;
              trainImage = DONE_VIEW;
          }else{
              queryImage = DONE_VIEW;
              trainImage = NEW_VIEW;
          }

          bool good_triangulation = triangulateViews(nFeatureImages[queryImage],nFeatureImages[trainImage],
                                                     nCameraPoses[queryImage],nCameraPoses[trainImage],
                                                     bestMatches,cameraMatrix,
                                                     std::make_pair(queryImage,trainImage),new_triangulated);

          if(not good_triangulation){
            continue;
          }

          std::cout << "before triangulation: " << nReconstructionCloud.size() << std::endl;;
          mergeNewPoints(new_triangulated);         
          std::cout << "after " << nReconstructionCloud.size() << std::endl;
          adjustCurrentBundle() ;   
      }

 std::cout << "\n"<< "=============================== " << std::endl;
 std::cout << "Images processed = " << nDoneViews.size() << " of " << nImages.size() << std::endl;
 std::cout << "PointCloud size = " << nReconstructionCloud.size() << " pts3D" << std::endl;

 return true;
}

//===============================================
//FUNCTION: FIND CORRESPONDENCES 2D-3D
//===============================================
void StructFromMotion::find2D3DMatches(const int& NEW_VIEW,
                                       std::vector<cv::Point3f>& points3D,
                                       std::vector<cv::Point2f>& points2D,Matching& bestMatches,int& DONEVIEW){

     points3D.clear(); points2D.clear();
     int queryImage,trainImage;
     int bestNumMatches = 0;
     Matching bestMatch;

     for(int doneView : nDoneViews){

         if(NEW_VIEW < doneView){
             queryImage = NEW_VIEW;
             trainImage = doneView;
         }else{
             queryImage = doneView;
             trainImage = NEW_VIEW;
         }

         //const Matching match = getMatching(nFeatureImages[queryImage],nFeatureImages[trainImage]);
         Matching match;
         MatchFeatures(queryImage,trainImage,&match);

         int numMatches = match.size();
         if(numMatches > bestNumMatches) {
            bestMatch       = match;
            bestNumMatches = numMatches;
            DONEVIEW = doneView;
         }
     }

     bestMatches = bestMatch;


     //scan all cloud 3D points
     for(const Point3D& cloudPoint : nReconstructionCloud){

         bool found2DPoint = false;
         //scan all originating views for that 3D point
         for(const std::pair<const int,int>& origViewAndPoint : cloudPoint.idxImage){

             //check for 2D-2D matching
             const int originatingViewIndex      = origViewAndPoint.first;
             const int originatingViewFeatureIndex = origViewAndPoint.second;

             if(originatingViewIndex != DONEVIEW)continue;

             //scan all 2D-2D matches between originating view and new view
             for(const cv::DMatch& m : bestMatch){

                 int matched2DPointInNewView = -1;
                 if(originatingViewIndex < NEW_VIEW) { //originating view is 'left'

                     if(m.queryIdx == originatingViewFeatureIndex){
                         matched2DPointInNewView = m.trainIdx;
                     }
                 }else{ //originating view is 'right'

                     if(m.trainIdx == originatingViewFeatureIndex){
                         matched2DPointInNewView = m.queryIdx;
                     }
                 }
                 if(matched2DPointInNewView >= 0){

                     //This point is matched in the new view
                     const Feature& newViewFeatures = nFeatureImages[NEW_VIEW];
                     points2D.push_back(newViewFeatures.pt2D[matched2DPointInNewView]);
                     points3D.push_back(cloudPoint.pt);
                     found2DPoint = true;
                     break;
                  }
              }

              if(found2DPoint){

                  break;
              }
         }
    }

     std::cout << "Found: " << points3D.size() << " Pt3D and "
                  << points2D.size() << " Pt2D" << std::endl;

}


//==============================================
//INVERSE MATRIX-DETERMINANT FUNCTION EIGEN
//==============================================

cv::Mat StructFromMotion::inverse(cv::Mat& matrix){

    Eigen::MatrixXd invMatrix,invMatrixTranspose;
    Eigen::Map<Eigen::Matrix<double, Eigen::Dynamic,
                                    Eigen::Dynamic,
                                    Eigen::RowMajor> > eigenMatrix((double *)matrix.data,3,3);

    invMatrix = eigenMatrix.inverse();
    invMatrixTranspose = invMatrix.transpose();
    // create an OpenCV Mat header for the Eigen data:
    cv::Mat inv(invMatrixTranspose.rows(),
                                         invMatrixTranspose.cols(),
                                         CV_64FC1,invMatrixTranspose.data());

    return inv;
  }

double StructFromMotion::determinante(cv::Mat& relativeRotationCam){

  Eigen::Map<Eigen::Matrix<double,Eigen::Dynamic,
                                  Eigen::Dynamic,
                                  Eigen::RowMajor> > eigenMatrix((double *)relativeRotationCam.data,3,3);

  Eigen::FullPivLU<Eigen::Matrix<double, Eigen::Dynamic,
                                         Eigen::Dynamic,
                                         Eigen::RowMajor>> eigenMatrixV2(eigenMatrix);

  double det = eigenMatrixV2.determinant();
  return det;
}

//===============================================
//FUNCTION: FIND CAMERA POSE PNP RANSAC
//===============================================

bool StructFromMotion::findCameraPosePNP(const CameraData& intrinsics,const std::vector<cv::Point3f>& pts3D,const std::vector<cv::Point2f>& pts2D,cv::Matx34f& P){

  if(pts3D.size() <= 7 || pts2D.size() <= 7 || pts3D.size() != pts2D.size()) {

      //something went wrong aligning 3D to 2D points..
      std::cerr << "couldn't find [enough] corresponding cloud points... (only " << pts3D.size() << ")"
                << std::endl;
      return false;
   }

  cv::Mat rvec, T;
  //cv::Mat inliers;
  std::vector<int> inliers;
  double minVal, maxVal;
  cv::minMaxIdx(pts2D, &minVal, &maxVal);
  //"solvePnPRansac"
  cv::solvePnPRansac(pts3D,pts2D,intrinsics.K,intrinsics.distCoef,rvec,T,true,1000,
                     0.006 * maxVal,0.99,inliers,CV_EPNP);
/*
  std::vector<cv::Point2f> projected3D;
  cv::projectPoints(pts3D, rvec, T, intrinsics.K,intrinsics.distCoef, projected3D);

  if(inliers.size() == 0) { //get inliers
     for(int i = 0; i < projected3D.size(); i++) {
         if(cv::norm(projected3D[i] - pts2D[i]) < 10.0){
             inliers.push_back(i);

          }
     }
  }

  if(inliers.size() < (double)(pts2D.size()) / 5.0) {

      std::cerr << "not enough inliers to consider a good pose (" << inliers.size() << "/"
                << pts2D.size() << ")" << std::endl;
      return false;
  }

  if (cv::norm(T) > 200.0) {

      // this is bad...
      std::cerr << "estimated camera movement is too big, skip this camera\r\n";
      return false;
  }
*/
  cv::Mat R;
  cv::Rodrigues(rvec, R);
  if(!CheckCoherentRotation(R)) {

     std::cerr << "rotation is incoherent. we should try a different base view..." << std::endl;
     return false;
  }

  std::cout << "found t = " << "\n"<< T << "\nR = \n" << R << std::endl;

  //Rotational element in a 3x4 matrix
  const cv::Rect ROT(0, 0, 3, 3);

  //Translational element in a 3x4 matrix
  const cv::Rect TRA(3, 0, 1, 3);

  R.copyTo(cv::Mat(3, 4, CV_32FC1, P.val)(ROT));
  T.copyTo(cv::Mat(3, 4, CV_32FC1, P.val)(TRA));

  return true;

}

void StructFromMotion::mergeNewPoints(const std::vector<Point3D>& newPointCloud) {

  std::cout << "Adding new points..." << std::flush;

  const float MERGE_CLOUD_POINT_MIN_MATCH_DISTANCE   = 0.01;
//  const float MERGE_CLOUD_FEATURE_MIN_MATCH_DISTANCE = 20.0;

      size_t newPoints = 0;
 //     size_t mergedPoints = 0;

      for(const Point3D& p : newPointCloud) {
          const cv::Point3f newPoint = p.pt; //new 3D point

          bool foundAnyMatchingExistingViews = false;
          bool foundMatching3DPoint = false;
          for(Point3D& existingPoint : nReconstructionCloud) {
              if(cv::norm(existingPoint.pt - newPoint) < MERGE_CLOUD_POINT_MIN_MATCH_DISTANCE){
                  //This point is very close to an existing 3D cloud point
                  foundMatching3DPoint = true;
                  break;

               }
          }

          if(not foundAnyMatchingExistingViews and not foundMatching3DPoint) {
              //This point did not match any existing cloud points - add it as new.
              nReconstructionCloud.push_back(p);
              newPoints++;
          }
      }

      std::cout << "[DONE]" << std::endl;
      std::cout << "New points:" << newPoints << std::endl;
}

Matching StructFromMotion::matchingFor2D3D(Feature& feature1,Feature& feature2){

  cv::Ptr<cv::DescriptorMatcher> matcherFlan = cv::DescriptorMatcher::create(2);
  std::vector<cv::DMatch> matches12,matches21,buenosMatches;
  matcherFlan ->match(feature1.descriptors,feature2.descriptors,matches12);
  matcherFlan ->match(feature2.descriptors,feature1.descriptors,matches21);

  for (size_t i=0; i < matches12.size(); i++){
      cv::DMatch forward = matches12[i];
      cv::DMatch backward = matches21[forward.trainIdx];
      if(backward.trainIdx==forward.queryIdx){
          buenosMatches.push_back(forward);
      }
  }
  return buenosMatches;
}

void StructFromMotion::createPointsTxt(){

  std::cout << "Saving points for bundle with prefix:" << "points.txt" << std::endl;
  std::ofstream ofs("points.txt");
  for(const Point3D& pt3d : nReconstructionCloud) {

      ofs << pt3d.pt.x << " " << pt3d.pt.y << " " <<  pt3d.pt.z << " ";
      int cont = 1;

      for(const std::pair<const int,int> originatingView : pt3d.idxImage){

          if(cont ==1){
              ofs << std::tuple_size<decltype(originatingView)>::value << " ";
          }

          const int viewIdx = originatingView.first;
          const cv::Point2f p2d = nFeatureImages[viewIdx].pt2D[originatingView.second];

          ofs << viewIdx << " " << p2d.x << " " << p2d.y << " ";
          cont +=1;
        }
      ofs << std::endl;
  }
  ofs.close();
}

void StructFromMotion::PMVS2(){

  /*FOLDERS FOR PMVS2*/
  std::cout << "Creating folders for PMVS2..." << std::endl;
  std::system("mkdir -p denseCloud/visualize");
  std::system("mkdir -p denseCloud/txt");
  std::system("mkdir -p denseCloud/models");
  std::cout << "Created: \nfolder:visualize" << "\n" << "folder:txt" << "\n" << "folder:models" << std::endl;

  /*OPTIONS CONFIGURATION FILE FOR PMVS2*/
  std::cout << "Creating options file for PMVS2..." << std::endl;
  ofstream option("denseCloud/options.txt");
 // option << "minImageNum 5" << std::endl;
 // option << "CPU 4" << std::endl;
  option << "timages  -1 " << 0 << " " << (nImages.size()-1) << std::endl;
  option << "oimages 0" << std::endl;
  option << "level 1" << std::endl;
  option.close();
  std::cout << "Created: options.txt" << std::endl;

  /*CAMERA POSES AND IMAGES INPUT FOR PMVS2*/
  std::cout << "Saving camera poses for PMVS2..." << std::endl;
  std::cout << "Saving camera images for PMVS2..." << std::endl;
  for(int i=0; i<nCameraPoses.size(); i++) {

      /*
      cv::Matx33f R = pose.get_minor<3, 3>(0, 0);
      Eigen::Map<Eigen::Matrix3f> R_eigen(R.val);
      Eigen::Quaternionf q(R_eigen);
      */

      char str[256];
      boost::filesystem::directory_entry x(nImagesPath[i]);
      std::string extension = x.path().extension().string();
      boost::algorithm::to_lower(extension);

      std::sprintf(str, "cp -f %s denseCloud/visualize/%04d.jpg", nImagesPath[i].c_str(), (int)i);
      std::system(str);
      cv::imwrite(str, nImages[i]);

      std::sprintf(str, "denseCloud/txt/%04d.txt", (int)i);
      ofstream ofs(str);
      cv::Matx34f pose = nCameraPoses[i];

      //K*P
      pose = (cv::Matx33f)cameraMatrix.K*pose;

      ofs << "CONTOUR" << std::endl;
      ofs << pose(0,0) << " " << pose(0,1) << " " << pose(0,2) << " " << pose(0,3) << "\n"
          << pose(1,0) << " " << pose(1,1) << " " << pose(1,2) << " " << pose(1,3) << "\n"
          << pose(2,0) << " " << pose(2,1) << " " << pose(2,2) << " " << pose(2,3) << std::endl;

      ofs << std::endl;
      ofs.close();
  } 
  std::cout << "Camera poses saved." << "\n" << "Camera images saved." << std::endl; 
}

void StructFromMotion::fromPoint3DToPCLCloud(const std::vector<Point3D> &input_cloud,
                                             pcl::PointCloud<pcl::PointXYZ>::Ptr& cloudPCL){

  cloudPCL.reset(new pcl::PointCloud<pcl::PointXYZ> ());
  for(size_t i = 0; i < input_cloud.size(); ++i){
      Point3D pt3d = input_cloud[i];
      pcl::PointXYZ pclp;
      pclp.x  = pt3d.pt.x;
      pclp.y  = pt3d.pt.y;
      pclp.z  = pt3d.pt.z;
      cloudPCL->push_back(pclp);
   }
   cloudPCL->width = (uint32_t) cloudPCL->points.size(); // number of points
   cloudPCL->height = 1;	// a list, one row of data
   cloudPCL->header.frame_id ="map";
   cloudPCL->is_dense = false;
}

void StructFromMotion::cloudPointFilter(pcl::PointCloud<pcl::PointXYZ>::Ptr &cloud,
  pcl::PointCloud<pcl::PointXYZ>::Ptr &filterCloud){
  //Removes points where values of selected field are out of range.

  pcl::PassThrough<pcl::PointXYZ> pass_through;
  pass_through.setInputCloud (cloud);
  pass_through.setFilterFieldName ("x");
  pass_through.setFilterLimits (0.003, 0.83);
  pass_through.filter (*filterCloud);
}

void StructFromMotion::removePoints(pcl::PointCloud<pcl::PointXYZ>::Ptr &cloud,
  pcl::PointCloud<pcl::PointXYZ>::Ptr &filterCloud){
  // Removes all points with less than a given
  // number of neighbors within a radius

  pcl::RadiusOutlierRemoval<pcl::PointXYZ> radius_outlier_removal;
  radius_outlier_removal.setInputCloud(cloud);
  radius_outlier_removal.setRadiusSearch (0.07);
  radius_outlier_removal.setMinNeighborsInRadius (150);
  radius_outlier_removal.filter (*filterCloud);

}

void StructFromMotion::create_mesh(pcl::PointCloud<pcl::PointXYZ>::Ptr &cloud,pcl::PolygonMesh &mesh){

  pcl::NormalEstimationOMP<pcl::PointXYZ, pcl::Normal> ne;
  ne.setNumberOfThreads (8);
  // ne.setInputCloud (cloud_smoothed);
  ne.setInputCloud (cloud);
  pcl::search::KdTree<pcl::PointXYZ>::Ptr tree (new pcl::search::KdTree<pcl::PointXYZ> ());
  ne.setSearchMethod (tree);
  ne.setKSearch (10); //20
  //ne.setRadiusSearch (0.5); // no funciona
  pcl::PointCloud<pcl::Normal>::Ptr cloud_normals (new pcl::PointCloud<pcl::Normal>());
  ne.compute(*cloud_normals);

  for(std::size_t i = 0; i < cloud_normals->size (); ++i){
    cloud_normals->points[i].normal_x *= -1;
    cloud_normals->points[i].normal_y *= -1;
    cloud_normals->points[i].normal_z *= -1;
  }
  pcl::PointCloud<pcl::PointNormal>::Ptr cloud_smoothed_normals (new pcl::PointCloud<pcl::PointNormal> ());
  pcl::concatenateFields (*cloud, *cloud_normals, *cloud_smoothed_normals);//x

  pcl::Poisson<pcl::PointNormal> poisson;
  poisson.setDepth (9);//9
  poisson.setInputCloud (cloud_smoothed_normals);
  poisson.setPointWeight(4);//4
  //poisson.setDegree(5);
  poisson.setSamplesPerNode(1.5);//1.5
  poisson.setScale(1.1);//1.1
  poisson.setIsoDivide(8);//8
  poisson.setConfidence(1);
  poisson.setManifold(0);
  poisson.setOutputPolygons(0);
  poisson.setSolverDivide(8);//8
  poisson.reconstruct(mesh);

}

void StructFromMotion::vizualizeMesh(pcl::PolygonMesh &mesh){

  boost::shared_ptr<pcl::visualization::PCLVisualizer> viewer (new pcl::visualization::PCLVisualizer ("MAP3D MESH"));
  viewer->setBackgroundColor (0, 0, 0);
  viewer->addPolygonMesh(mesh,"meshes",0);
  viewer->addCoordinateSystem (1.0);
  viewer->initCameraParameters ();

  std::cout << "Press q to finish 3D mapping and start segmentation process..." << std::endl;
  while (!viewer->wasStopped ()){
      viewer->spin();
  }
}

void StructFromMotion::optical_flow_feature_match(){
/*
  //detect keypoints for all images
  cv::FastFeatureDetector ffd;
  //DenseFeatureDetector ffd;
  std::vector<std::vector<cv::KeyPoint>>imgs_kps;
  for(size_t i=0;i<nFeatureImages.size();i++){
      imgs_kps.push_back(nFeatureImages[i].kps);
  }
  ffd.detect(nImages,imgs_kps);

  int loop1_top = nImages.size() - 1, loop2_top = nImages.size();
  int frame_num_i = 0;
  //pragma omp parallel for
  for(frame_num_i = 0; frame_num_i < loop1_top; frame_num_i++) {
      for(int frame_num_j = frame_num_i + 1; frame_num_j < loop2_top; frame_num_j++){

          std::cout << "------------ Match " << "image:" << frame_num_i << "," << "image:"
                    << frame_num_j<< " ------------\n";
          std::vector<cv::DMatch>& matches_tmp;
          MatchFeatures(frame_num_i, frame_num_j, matches_tmp);
          /*
          matches_matrix[std::make_pair(frame_num_i, frame_num_j)] = matches_tmp;
          std::vector<cv::DMatch> matches_tmp_flip = FlipMatches(matches_tmp);
          matches_matrix[std::make_pair(frame_num_j, frame_num_i)] = matches_tmp_flip;

      }
   }
   */
}

void StructFromMotion::MatchFeatures(int idx_i, int idx_j, std::vector<cv::DMatch>* matches) {
		std::vector<cv::Point2f> i_pts = nFeatureImages[idx_i].pt2D;
		//keypoints2F(nFeatureImages[idx_i].kps, nFeatureImages[i_pts].pt2D);

		std::vector<cv::Point2f> j_pts(i_pts.size());

		// making sure images are grayscale
		cv::Mat prevgray, gray;
		if(nImages[idx_i].channels() == 3) {
			cv::cvtColor(nImages[idx_i], prevgray, CV_RGB2GRAY);
			cv::cvtColor(nImages[idx_j], gray, CV_RGB2GRAY);
		}else {
			prevgray = nImages[idx_i];
			gray = nImages[idx_j];
		}

		std::vector<uchar> vstatus(i_pts.size());
		std::vector<float> verror(i_pts.size());
		cv::calcOpticalFlowPyrLK(prevgray, gray, i_pts, j_pts, vstatus, verror);

		double thresh = 1.0;
		std::vector<cv::Point2f> to_find;
		std::vector<int> to_find_back_idx;
		for(unsigned int i = 0; i < vstatus.size(); i++){
		    if(vstatus[i] && verror[i] < 12.0){
			to_find_back_idx.push_back(i);
			to_find.push_back(j_pts[i]);
		     }else{
			vstatus[i] = 0;
		     }
		}

		std::set<int> found_in_imgpts_j;
		cv::Mat to_find_flat = cv::Mat(to_find).reshape(1, to_find.size());

		std::vector<cv::Point2f> j_pts_to_find = nFeatureImages[idx_j].pt2D;
		//keypoints2F(imgpts[idx_j], j_pts_to_find);
		cv::Mat j_pts_flat = cv::Mat(j_pts_to_find).reshape(1, j_pts_to_find.size());

		std::vector<std::vector<cv::DMatch> > knn_matches;
		//FlannBasedMatcher matcher;
		cv::BFMatcher matcher(CV_L2);
		matcher.radiusMatch(to_find_flat, j_pts_flat, knn_matches, 2.0f);
		//Prune

		for(int i = 0; i < knn_matches.size(); i++){
		    cv::DMatch _m;

		    if(knn_matches[i].size() == 1){
			_m = knn_matches[i][0];
		     }else if(knn_matches[i].size()>1){
			if(knn_matches[i][0].distance / knn_matches[i][1].distance < 0.7){
			    _m = knn_matches[i][0];
			}else{
			    continue; // did not pass ratio test
			}
		     }else{
			continue; // no match
		     }
		     if(found_in_imgpts_j.find(_m.trainIdx) == found_in_imgpts_j.end()){ // prevent duplicates
			 _m.queryIdx = to_find_back_idx[_m.queryIdx]; //back to original indexing of points for <i_idx>
			 matches->push_back(_m);
			 found_in_imgpts_j.insert(_m.trainIdx);
			}

		}

		std::cout << "pruned " << matches->size() << " / " << knn_matches.size() << " matches" << std::endl;

		/*
#if 0
		{
			// draw flow field
			Mat img_matches; cvtColor(grey_imgs[idx_i], img_matches, CV_GRAY2BGR);
			i_pts.clear(); j_pts.clear();
			for (int i = 0; i < matches->size(); i++) {
				//if (i%2 != 0) {
				//				continue;
				//			}
				Point i_pt = imgpts[idx_i][(*matches)[i].queryIdx].pt;
				Point j_pt = imgpts[idx_j][(*matches)[i].trainIdx].pt;
				i_pts.push_back(i_pt);
				j_pts.push_back(j_pt);
				vstatus[i] = 1;
			}
			drawArrows(img_matches, i_pts, j_pts, vstatus, verror, Scalar(0, 255));
			stringstream ss;
			ss << matches->size() << " matches";
			ss.clear(); ss << "flow_field_"<<idx_i<<"and"<<idx_j << ".png";//<< omp_get_thread_num()
			imshow(ss.str(), img_matches);

			//direct wirte
			imwrite(ss.str(), img_matches);
			//int c = waitKey(0);
			//if (c == 's') {
			//	imwrite(ss.str(), img_matches);
			//}
			destroyWindow(ss.str());
		}
#endif
*/
}





